# Test-suite cleanup plan

This document tracks failing tests in `just test` as of branch
`claude/relaxed-dijkstra-EoKvS`, the quick fixes that were applied, the
real bugs / missing features that came out of those investigations, and
the tests that are now flagged as exercising genuinely missing behavior
(and therefore left failing intentionally).

## Snapshot of the run before this branch

* `tur_tests` (fixture suite): ~351 unique failures
* `tur_flags_tests`: 1 failure (`effect-export-syntax`)
* `turi_fixture_tests`: 2 failures (`result-basic`, `typed/tzipper-basic`)
* Other suites: pass

The bulk of the fixture failures fell into a small number of root causes
that had nothing to do with the actual fixture's intent.

## Snapshot after this branch

* `tur_tests`: 5 unique fixture failures remain (all documented below)
* All other suites: pass

`just test` now reports `956 passed, 5 failed` for the compiled-fixture
suite, with the remaining failures all classified below.

## What changed

### 1. Stdlib: fixed `-Wincompatible-pointer-types` warnings

A bunch of fixtures were tripping a `cc` warning that leaked into stderr
and caused indirect failures (notably `effect-export-syntax` in
`tur_flags_tests` checks combined stdout+stderr equal to `"hello"`).

The root cause: several stdlib functions cast through anonymous struct
types like:

```c
struct { bool is_ok; int64_t ok_val; int64_t err_val; } *a =
    (struct { bool is_ok; int64_t ok_val; int64_t err_val; } *)(intptr_t)r1;
```

In C99, each anonymous struct literal is a *distinct* type, so the
cast-to and the variable-of are incompatible pointer types. Fix: cast
through `void *`, which converts implicitly. Updated:

* `stdlib/result.tur` -- `result-eq?`, `result-collect`, `result-partition`
* `stdlib/tmutmap.tur` -- `tmutmap-new`

After this change, `effect-export-syntax` passes again and the warning
noise is gone from every fixture's stderr.

### 2. Regenerated stale codegen snapshots (mass quick fix)

Recent stdlib / runtime work (EX1/EXG1: constrained-existential types,
DS3: rc<Struct> cycle support) added new typedefs and helpers to the
shared C preamble emitted by `tur emit-c`. ~290 fixtures with an
`expected.c` snapshot fell out of sync purely because of this preamble
change -- the program output (`expected.stdout`) was still correct.

A script (kept inline at `tools/` would be overkill -- it was a
throwaway script) was used to copy `actual.c` -> `expected.c` for every
fixture whose `expected.stdout` matched `actual.stdout` and whose
generated C compiled cleanly. This is a quick fix; the snapshots are
again capturing the current runtime preamble.

Per the snapshot decision below, this was the last time the regeneration
sweep should be needed at scale -- once `expected.c` is dropped for
behavior fixtures, future preamble changes will only touch the small
remaining codegen suite.

**Note:** Snapshot regeneration is mechanical and will need to be redone
whenever the runtime preamble changes. Worth a thought to either:

* add a `tools/update-snapshots.py` helper, or
* drop expected.c snapshots entirely for fixtures whose value is the
  runtime behavior and not the literal C output. The runtime preamble
  is shared across all fixtures, so 290 copies of the same bytes do not
  buy us much.

**Decision:** Drop `expected.c` for behavior fixtures. Audit which
fixtures actually validate codegen (likely a small set under explicit
codegen directories) and keep `expected.c` only for those; the rest
verify `expected.stdout` only. This eliminates the recurring 290-file
churn outright. No `tools/update-snapshots.py` helper needed if the
remaining codegen surface is small enough to regenerate by hand;
revisit if it isn't.

### 3. Renamed user-defined symbols that now collide with auto-loaded stdlib

When `stdlib/result.tur`, `stdlib/option.tur`, `stdlib/tvec.tur`,
`stdlib/typeclass-eq.tur`, etc. were promoted to auto-loaded modules,
old fixtures that *manually* defined helpers like `ok`, `err`, `Vec`,
`Cons`, `Pair`, `Option`, `Eq`, `Show`, `result-eq?`, `ok-or`,
`result-free`, `result-map`, ... started failing. Two failure modes:

* `defstruct` collisions raise a clear Turmeric-level diagnostic
  ("defstruct: `Vec` is already defined").
* `defn` collisions are **not** diagnosed at the Turmeric level (see
  Missing Feature 1 below); they sneak through and the generated C
  fails with `redefinition of 'ok'`, `conflicting types for 'err'`,
  etc.

I renamed the local definitions in the affected fixtures by prefixing
them with `u-` (for "user"), `My`, or `H` (for "heap") -- whichever
preserved readability:

| Fixture | Rename |
| --- | --- |
| `clone-vec` | `Vec` -> `HVec` |
| `show-list` | `Cons` -> `HCons`, `Show` -> `MyShow` |
| `show-option` | `Option` -> `MyOption`, `Show` -> `MyShow` |
| `show-pair` | `Pair` -> `MyPair`, `Show` -> `MyShow` |
| `gadt-refine-nat`, `gadt-stdlib-vec` | `Vec` -> `MyVec` |
| `typeclass-derived`, `typeclass-primitives` | `Eq`/`Ord`/`Show` -> `MyEqDerived`/`MyOrdPrim`/... |
| `must-expect`, `must-msg`, `must-result-err`, `must-result-ok` | `ok`, `err`, `some`, `result-must*`, `option-expect` -> `u-*` |
| `warn-suppress-ignore`, `warn-unused-result` | `ok` -> `u-ok` |
| `error-context` | `ok`, `err`, `ok?`, `err-context`, `err-val-cstr` -> `u-*` (kept `err` as a param name -- those don't collide because they only exist inside inline-C blocks) |
| `error-ok-or` | `ok`, `err`, `some`, `none`, `ok-or` -> `u-*` |
| `result-collect` | `ok`, `err`, `result-collect`, `result-partition*` -> `u-*` |
| `result-combinators` | `ok`, `err`, `result-map*`, `result-flat-map`, `result-or`, `result-or-else` -> `u-*` |
| `result-display` | `ok`, `err` -> `u-*` (also adjusted the cstr literals printed) |
| `result-question-op` | `ok`, `err`, `ok?`, `err?`, `ok-val`, `err-val` -> `u-*`, and wrapped the `?` call in `(unsafe ...)` because the stdlib lowering for `?` calls into the deprecated unsafe `err?`/`err-val`. |
| `stdlib-result-runtime` | `ok`, `err`, `result-free`, `result-unwrap*`, `result-expect` -> `u-*` |
| `structural-eq` | `ok`, `err`, `result-eq?`, `result-eq` -> `u-*` |

Each fixture also had its `expected.c` snapshot regenerated and its
`expected.stderr` updated when the panic message changed.

### 4. `tzipper-basic` and `typed/tzipper-basic` double-free

Both fixtures called `(free left)` and `(free right)` *after*
`(tzipper-free z)`. But `tzipper-free` itself frees the neighbour arrays
(see `stdlib/tzipper.tur:122-138`), so the explicit `free` was a
double-free that glibc happened to detect (`free(): double free detected
in tcache 2`).

Fix: removed the redundant `free` calls; added a comment in each
fixture pointing at the ownership contract. (Note: arguably the
ownership contract on `tzipper-new` -- "transfers ownership to the
zipper" -- is surprising; see Missing Feature 4.)

### 5. Updated `must-result-err` expected stderr

Once the local `err` was renamed `u-err`, the panic message in the
fixture itself changed from `result-must: called on err` to
`u-result-must: called on u-err`. Updated `expected.stderr` to match.

## Remaining failures, by category

### Genuinely missing features (left failing intentionally)

#### Missing Feature 1 -- `defclass` redefinition is not diagnosed [FIXED]

Tests: `errors/kinds-hkt-reserved`, `errors/typeclass-no-instance`
(both now passing).

These fixtures expect that redefining a typeclass produces a Turmeric
diagnostic ("typeclass `Functor` is already defined" / "typeclass `MyEq`
is already defined"). The elaborator previously silently accepted the
second `defclass` and the test exited 0.

`defstruct` already has this check (see `clone-vec` diagnostic); the
same check is now in place for `defclass`. Identical-signature
re-declarations (the stdlib pre-declare case for Eq, Functor, ...)
remain silent.

**Decision:** Allow idempotent redefinition only if the signatures
match; otherwise hard-error. Implemented via
`typeclass_signatures_match` in `src/compiler/elab_typeclasses.c`:
compares type-parameter count and kinds, method count, and per-method
name + parameter type-kinds + return type-kind. Identical = silent
skip (preserves the stdlib pre-declaration of `Eq`, `Functor`, ...);
different = `typeclass 'Foo' is already defined` diagnostic.

The defclass parsing was split into three passes (parse type params /
parse signatures / elaborate default bodies) so the redefinition
check happens after enough information exists to compare, but before
any orphan `__default_*` file-level FnDefs get registered for an
about-to-be-skipped re-declaration.

**Fixture follow-up [done]:** Both `errors/kinds-hkt-reserved`
(differs in `[^f]` vs `[a]` + `fmap` vs `map`) and
`errors/typeclass-no-instance` (differs in `eq?` vs `eq2?`) trip the
differing-signature branch. Both fixtures now pass.

#### Missing Feature 2 -- `?` operator diagnostic when result helpers are out of scope [FIXED]

Test: `errors/result-question-op` (now passing, re-purposed).

The fixture previously expected: "name resolution error -- `err?` not
in scope". The elaborator instead reported: "unsafe function `err?`
requires an enclosing (unsafe ...)". Both errors were *correct* in
their own context (stdlib's `err?` *is* unsafe), but the test
predated the deprecation/`#{Unsafe}` annotation.

**Decision (implemented):** Lower `?` through a pair of stdlib
helpers in `stdlib/result.tur` -- `__tur-q-is-err?` and
`__tur-q-ok-val` -- that touch the internal result representation
directly via inline-C. The `?` lowering in `src/compiler/elab_forms.c`
calls these helpers by name; user call sites of `?` no longer need
their own `(unsafe ...)` wrapper, and the deprecation warnings on
`err?` / `ok-val` no longer leak through every compilation. The
early-return branch was also simplified from `(return (err (err-val
__q)))` to `(return __q)` -- the original is already the err
Result.

Why this over a per-site auto-wrap:

* Audit surface collapses to one named function. A reviewer can
  inspect `__tur_result_question` and know the full unsafe story for
  `?` across the language.
* Deprecation of `err?` / `err-val` becomes "update one function"
  instead of "chase the lowering pass" -- the dependency is visible
  in the runtime preamble rather than scattered through
  compiler-synthesized code at every call site.
* The `(unsafe ...)` annotation stays meaningful in user code --
  it only appears when the user took responsibility, not as
  ceremony around every `?`.
* MF3 interaction is simpler: the helper resolves `err?` / `err-val`
  exactly once at its own definition site, against whatever scope
  applies there. No per-call-site shadow-aware resolution in the
  lowering.

Caveats to watch:

* The helper is a runtime ABI surface -- once published, changing
  its signature is a breaking change. Pin the signature early and
  document it.
* If user code shadows `err?` and expects `?` in their module to use
  the shadow, that no longer works (the helper is resolved against
  the stdlib scope). Document this as the intentional tradeoff; if
  per-module shadowing of `?` is needed later, revisit.
* Long-term, consider re-lowering `?` to a typeclass dispatch
  (Try / MonadFail) so the helper becomes one instance of a general
  mechanism rather than a hard-coded runtime function.

**Fixture follow-up [done]:** `errors/result-question-op` was
re-purposed to cover `?` applied to a non-Result value (here, an int
literal). The new lowering's helper call site catches it with
"function `__tur-q-is-err?` arg 1: expected ptr<void>, got int",
which is strictly more informative than the previous
"if condition must be bool" diagnostic.

#### Missing Feature 3 -- `defn` shadowing of stdlib symbols emits broken C instead of a diagnostic [FIXED]

Was the root cause of most of the rename churn in section 3 above. If
a user wrote `(defn ok ...)`, the auto-loaded stdlib `ok` and the
user `ok` both got emitted as static C functions named `ok` -- the C
compiler then complained.

**Decision (implemented):** Hard error by default. A user `defn`
colliding with an auto-loaded stdlib name produces:

    defn: 'ok' is already defined by an auto-loaded stdlib module;
    rename the local definition

Implementation:

* `Binding` gains an `is_from_stdlib` flag (`src/compiler/expr.h`).
* `Elab` gains an `in_stdlib_load` flag that's true while the
  auto-loaded stdlib prefix elaborates, false during user-form
  elaboration (`src/compiler/elab_internal.h`,
  `src/compiler/elab_toplevel.c`).
* `binding_new` sets `is_from_stdlib = is_global && e->in_stdlib_load`
  so any global binding created while elaborating the stdlib prefix
  is marked.
* `elab_toplevel.c`'s pass-1 forward-decl pre-pass now skips when the
  name already resolves in `e.global`, so user code does not pre-
  register a duplicate stub that would shadow the stdlib binding in
  pass 2's reverse-iterating `scope_lookup`.
* `elab_defn`'s redef check fires the diagnostic when
  `existing->is_from_stdlib && !e->in_stdlib_load`.

The plan originally also proposed an opt-in `#{Shadow}` attribute
that would mangle the user's C symbol to allow shadowing. Skipped
for now: the section-3 rename table already covers every collision
we know about, and the attribute can be added later if a real use
case emerges. Tracking that as deferred follow-up.

**Fixture follow-up:** the new check tripped on
`errors/gadt-refine-escape` (defined a user `unbox` that collided
with `stdlib/safe.tur`'s `unbox`). Renamed the user fn to `my-unbox`;
the fixture continues to exercise the skolem-escape diagnostic it
was meant to test. Section 3's existing rename table covered every
other case.

#### Missing Feature 4 -- stdlib internal conflict between `stdlib/gadt-vec.tur` and `stdlib/tvec.tur`

Test: `gadt-stdlib-vec-stdlib` (left failing).

`stdlib/tvec.tur` defines `(defstruct Vec [A] ...)` and is auto-loaded.
`stdlib/gadt-vec.tur` defines `(defgadt Vec [a] ...)` and is loaded
explicitly via `(load "stdlib/gadt-vec.tur")` by this fixture. When the
GADT is loaded after the struct is already in scope, the `:Vec`
parameter annotation inside `gadt-vec.tur` resolves to the *struct*
`Vec` (a `tyvar`), not the GADT, and the file fails to elaborate.

Either:

* rename the GADT in `stdlib/gadt-vec.tur` (e.g. `(defgadt GVec ...)`);
* or teach the elaborator to keep separate namespaces for structs vs
  GADTs;
* or remove `gadt-vec.tur` if it's superseded by `tvec.tur`.

This is squarely a stdlib bug. Test left failing as the forcing
function.

**Decision:** Separate struct / GADT namespaces in the elaborator.
The collision is a class of bug, not a one-off, so fix it at the
right layer rather than papering over with a rename. Sketch:
`src/compiler/elab_structs.c` registers GADT names in a distinct
lookup table; `:Type` annotations in parameter / return position
resolve preferring GADTs in annotation context. Write a short design
note in `docs/` before coding so the resolution rules (and any
ambiguity diagnostics) are settled up front.

#### Missing Feature 5 -- `tzipper-new` ownership contract [FIXED]

The previous API silently took ownership of the left/right arrays
passed to `tzipper-new`; `tzipper-free` later freed them. Surprising
for a container, and the implementation contract wasn't documented.

**Decision (implemented):** Switch to borrow + copy. `tzipper-new`
now mallocs its own left / right buffers and `memcpy`s the caller's
data in `stdlib/tzipper.tur`; the caller retains the originals and
must free them. `tzipper-free` continues to free the zipper-owned
copies. The docstring now spells out the ownership semantics with an
example.

**Fixture follow-up [done]:** `tests/fixtures/tzipper-basic/input.tur`
and `tests/fixtures/typed/tzipper-basic/input.tur` re-added explicit
`(free left)` / `(free right)` calls after each `tzipper-new`. Both
fixtures pass cleanly under ASAN. No other internal stdlib callers
of `tzipper-new` exist.

### External dependency missing (left failing intentionally)

#### `scscm-compile` [FIXED]

The fixture does `(load "../turmeric-spices/tur-scscm/src/scscm/...")`,
which requires the `turmeric-spices` repo to be checked out as a
sibling directory. In CI / fresh containers it isn't.

**Decision (implemented):** Added the `requires.spices` skip marker.
Mirrors the existing `requires.tsan` plumbing; `tests/run.sh` detects
the missing sibling `../turmeric-spices/` directory and skips.
Marker file at `tests/fixtures/scscm-compile/requires.spices`.
`CLAUDE.md` now lists all `requires.*` markers and has an "Optional
dependencies" section describing how to enable.

### Compiler memory leaks (uncovered while executing this plan)

Running `bash tests/run.sh` directly (rather than via `ctest`) surfaces
~148 additional "build failed" failures that `ctest` hides by setting
`ASAN_OPTIONS=detect_leaks=0`. Each is a LeakSanitizer report against
the `tur` binary itself -- a pre-existing leak in the compiler, not a
runtime program leak. Plug them so the two invocation paths agree and
so future leaks aren't masked.

Distinct leak sites observed across the ~148 fixtures:

| Site | File:line | Count | Status |
| --- | --- | --- | --- |
| `collect_free_vars` (closure captures) | `src/compiler/elab_core.c:446` | 114 | Fixed (arena-copy in `elab_fns.c:1568`) |
| `fresh_tmp` (codegen temp names) | various | 131 | Fixed (free at each leaking site) |
| `elab_defdynamic` | `src/compiler/elab_dynvars.c:141` | 10 | Fixed (`free(e.dynvar_entries)` in elab teardown) |
| `dynvar_push_active` | `src/compiler/elab_dynvars.c:168` | 9 | Fixed (`free(e.active_dynvar_bindings)` in elab teardown) |
| `scope_add` (try-catch / select / match) | `src/compiler/elab_concurrent.c` / `elab_structs.c` | 5 | Fixed (added `scope_free` at each site) |
| `read_curly_infix` | `src/compiler/reader.c:1367` | 1 | Fixed (free items[] on n==1 path) |

After all fixes, `bash tests/run.sh` reports **956 passed, 5 failed** -- the same as `just test` (ctest). The five remaining failures are the plan-known intentional failures (Missing Features 1, 2, 4 and `scscm-compile`).

#### Compiler-leak task 1 -- `fresh_tmp` family (largest)

`fresh_tmp`, `fresh_frame`, `fresh_defer_thunk`, `fresh_defer_env` each
malloc a 24-byte string and return it to the caller. No caller frees
the result -- the strings live for the rest of compilation. There are
~83 `fresh_tmp` call sites; chasing each is impractical.

**Decision:** add a string pool to `EmitCtx` (`char **fresh_strs` +
count/cap) that owns every allocation from the four `fresh_*` helpers,
and free the pool when `emit_program` / `emit_implementation` tear
down `EmitCtx`. Existing call sites are unchanged.

#### Compiler-leak task 2 -- `dynvar_push_active`

Inspect the malloc site, identify the matching destructor (or its
absence), and either free in the existing teardown path or arena-allocate.

#### Compiler-leak task 3 -- `elab_defdynamic`

As above.

#### Compiler-leak task 4 -- `scope_add`

`scope_free` exists (called in `elab_fns.c` after `collect_free_vars`).
Find the path where a scope is created without a matching `scope_free`.

#### Compiler-leak task 5 -- `read_curly_infix`

Single leak in the reader; likely a transient buffer not freed on the
success path. Small.

#### Compiler-leak task 6 -- alignment commit

Once all of the above are resolved and `bash tests/run.sh` matches
`ctest`'s pass/fail counts, no further change is needed. If any leak
proves intractable, leave a `# TODO(compiler-leak)` marker and add
`ASAN_OPTIONS=detect_leaks=0` to `tests/run.sh` so the two paths
agree.

## Files of note touched on this branch

* `stdlib/result.tur` -- pointer-cast warning fix in `result-eq?`,
  `result-collect`, `result-partition`.
* `stdlib/tmutmap.tur` -- pointer-cast warning fix in `tmutmap-new`.
* `tests/fixtures/**/expected.c` -- mass regeneration (~300 files);
  later dropped entirely for behavior fixtures (see section 1 of the
  Decisions block).
* Renamed fixtures in section 3.
* `tests/fixtures/tzipper-basic/input.tur`,
  `tests/fixtures/typed/tzipper-basic/input.tur` -- removed double-free.
* `src/compiler/elab_fns.c` -- closure captures now arena-copied so
  they share the closure's lifetime (plugs the `collect_free_vars` leak).
