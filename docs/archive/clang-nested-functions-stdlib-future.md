# `tur build` fails under clang: GCC nested-function extension used in `stdlib/future.tur`

**Severity:** medium -- builds (and the gate suite) fail under any clang
toolchain for every fixture that touches `future-with-timeout`,
`future-timeout`, `future-capturing-closure`, `future-linear`,
`future-split-free`, and `promise-linear`. ~22 of the 28 residual gate
failures on macOS (after the clang-17 `-Wint-conversion` workaround in
[`clang17-wint-conversion-codegen.md`](clang17-wint-conversion-codegen.md))
trace here.

GCC supports nested function definitions; clang **explicitly does
not**. No compiler flag fixes this. The inline-C blocks in
`stdlib/future.tur` define helper functions inside another C function's
body, which the codegen passes through verbatim. Under gcc the
fixtures build and the tests pass; under clang the cc step fails with
"function definition is not allowed here" + "use of undeclared
identifier" for every callsite of the nested function.

## Repro

```sh
cc --version | head -1   # any clang
./build/tur build tests/fixtures/future-capturing-closure/input.tur
```

Observed:

```
.../future-capturing-closure_input_tur.c:6093:31: error: use of undeclared identifier 'thread_fn'
 6093 |   pthread_create(&tid, &attr, thread_fn, arg);
      |                               ^
.../future-capturing-closure_input_tur.c:6138:28: error: function definition is not allowed here
 6138 |       void *wfn(void *raw) {
      |                            ^
```

## Affected sites

Three inline-C blocks in `stdlib/future.tur`, each defining a small
`void *<name>(void *raw)` helper inside the body of a Turmeric `defn`:

| Line | Helper | Enclosing defn |
| --- | --- | --- |
| `stdlib/future.tur:924` | `thread_fn` | `future-timeout` |
| `stdlib/future.tur:967` | `tfn`       | `future-with-timeout` |
| `stdlib/future.tur:991` | `wfn`       | `future-with-timeout` (nested-inside-nested) |

Each helper has the same shape:

```c
void *name(void *raw) {
    SomeArgStruct *a = (SomeArgStruct *)raw;
    /* mutate state through a->... */
    free(a); return NULL;
}
pthread_create(&tid, &attr, name, arg);
```

The helpers do **not** capture any outer-scope variables -- everything
they need flows through `raw` (a heap-allocated arg struct). So lifting
them to file scope is mechanical; the GCC-nested-function syntax was
purely a stylistic choice, not a load-bearing capture.

## Proposed fixes (ranked)

### Option A: hoist each helper to a top-level Turmeric `defn` with an inline-C body

Each Turmeric `defn` already lowers to a file-scope `static` C
function (verified: `tur emit-c /tmp/spike-hello.tur` shows every
defn becomes `static <return> name(...) { ... }`). So we can replace
the nested `void *thread_fn(void *raw) { ... }` with a sibling
`(defn future-thread-fn-internal [raw :ptr] :ptr ...)` and reference
its address from the parent.

The friction:

- The current bodies use locally-defined `typedef struct { ... }
  TimeoutArg;` (etc.). Those typedefs would need to live at file
  scope too -- either inlined into the helper's own inline-C block as
  a `static` shim, or moved to a shared "futures preamble" defn that
  every future defn includes.
- The address of a Turmeric defn is `&<name>`-able from inline-C of
  another defn only if the helper has been emitted before the caller
  in the .c file. The compiler emits in source order, so as long as
  the helper sits above the caller in `stdlib/future.tur`, we are
  fine. (A forward-declaration trick also works: `static void *<name>(void *);`
  at the top of the caller's inline-C.)

Estimated effort: **2-3 hours**, all within `stdlib/future.tur`. No
language changes required.

### Option B: ship the helpers as a hand-written `.c` linked into the stdlib

Use the same path the reactor / async I/O code uses: declare the
helpers with `(extern-c <name> [...] : ...)` in `future.tur`, write
the implementations in `stdlib/future_threads.c`, and add the file to
whatever build glue compiles the stdlib `.c` siblings.

Cleaner separation of concerns, but introduces a new `.c` for three
~10-line functions and needs the stdlib build to learn about a new
source file. Heavier than option A; reserve it if option A hits a
forward-declaration wall the codegen does not accept.

Estimated effort: **3-4 hours** plus build-glue changes.

### Option C: add a `(c-preamble ...)` form to the language

Adds a way for a `.tur` file to inject a file-scope inline-C block at
the top of the emitted module. The most generally useful of the three
-- this same pattern recurs every time stdlib wants to thread-safely
host a callback whose signature is dictated by a C API.

Heavy: requires a new top-level form in the elaborator, codegen
plumbing, documentation, and a couple of fixtures. Estimated effort:
**1-2 days**. Worth doing eventually; **not** the right call for
unblocking the gate suite.

### Recommendation

Take **option A**. Smallest blast radius, no language change, no new
build steps. Promote to option C when a second / third call site
appears.

## Why ship the report before the fix

The compiler/codegen work above (option A) is straightforward but
non-trivial; the corresponding `stdlib/future.tur` patch needs to (i)
hoist the typedef structs, (ii) hoist the function bodies into new
defns, (iii) update the three callers to reference the hoisted names.
There is real risk of subtle semantic differences in pthread argument
ownership / cleanup paths that warrant careful review on its own
branch, not bundled into a "look into the gate failures" session.

Filing the report carries the diagnosis + recommended fix forward so
the work isn't lost while the desktop-editor track keeps moving.

## Suite delta if option A lands

The 28 residual gate failures decompose as:

- ~22 build failures from nested functions (this report).
- 4 `future-*` build failures that may be the same root cause once
  the nested-function fix lands and the AOT carrier surfaces a
  different latent issue. Worth retrying once option A is in.
- ~4-6 stdout-mismatch / image-hook failures that are unrelated to
  either codegen issue. Separate report.

Best case after option A: gate drops from 28 → ~6 failures, all
runtime-test drift in the image-hook fixtures.

---

## Resolution

Option A landed. Three sites in `stdlib/future.tur` plus one in
`stdlib/taskgroup.tur` (also a nested `void timeout_fn` discovered when
re-running the suite) were hoisted into top-level defns following the
`stdlib/threadpool.tur::tp-worker` pattern: each defn declares
`[raw : ptr<void>] : ptr<void>` with the inline-C body
self-contained (struct typedefs redeclared inline), and the parent's
`pthread_create(...)` call uses the cast
`(void *(*)(void *))<mangled_name>`.

Concrete sites:

- `stdlib/future.tur`: `future-timeout-thread`,
  `future-with-timeout-tfn`, `future-with-timeout-wfn`.
- `stdlib/taskgroup.tur`: `spawn-timeout-thread-fn`.

While running the suite, two adjacent clang-17 fallout issues
surfaced and were folded in:

- `stdlib/str.tur`: `str-from-cstr` and `str-free` were declared with
  no explicit return type (defaulting to `:void` codegen) but had
  inline-C bodies that `return`ed values. Clang 17 promoted
  `-Wreturn-mismatch` to a default error. Added explicit `: ptr<void>`
  and `: int` annotations to match the bodies. Affected fixtures:
  `re-union-patterns`, `reader-macros-rx-literal`, `sum-either-str-parse`.
- Apple clang 17's `-Wint-conversion` and `-Wreturn-mismatch`
  promotions are tracked separately in
  `docs/reported/clang17-wint-conversion-codegen.md`. The
  `-Wno-error=int-conversion` workaround landed alongside (commit
  347ae1b02).

**Gate delta:** 206 → 4. The residual 4 are all stdout mismatches in
`image-hooks-tracked`, `image-reload-hook`, `image-roundtrip`, and
`load-in-imported-module`. Verified pre-existing by stashing the
Option A changes and re-running -- they reproduce against HEAD.
Filed separately under `docs/reported/`.
