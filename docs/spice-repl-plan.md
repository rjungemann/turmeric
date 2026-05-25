# Plan: Spice-aware REPL via AOT compile + dlopen

> **Status:** Draft Plan
> **Last Updated:** 2026-05-25
> **Type:** Compiler / CLI / Runtime

---

## Overview

`tur repl` today is the tree-walking interpreter with no project context.
You can type `(import frame/csv :refer [read-csv])` and it will fail to
resolve, because the REPL has none of the spice-aware path-discovery
machinery that per-file `tur check` / `tur emit-c` / `tur run` got in the
spice-aware-check pass. Even if resolution worked, most non-trivial spice
defns are inline-C bodies that the interpreter can't execute -- they only
make sense as compiled native code.

The plan is to make spices usable at the REPL by **AOT-compiling the
enclosing project's spice tree into a shared library once at REPL start,
`dlopen`-ing it, and binding spice imports to foreign symbols** discovered
via `dlsym`. Pure-Turmeric defns continue running in the interpreter;
inline-C defns dispatch through a small FFI shim. File mtimes on the spice
tree are checked at REPL start (and optionally on `(reload)`), triggering
a partial rebuild when stale.

This is the same architecture used by Racket's FFI, Common Lisp's
`cffi:load-foreign-library`, and Julia's `ccall`: separate the "interactive
shell" from the "compiled image," and bridge them with a typed foreign-call
boundary.

A smaller alternative -- a `tur repl --load <lib.so>` flag that takes a
pre-built shared library by path -- is sketched in the
[Smaller variant](#smaller-variant-explicit-load-flag) section at the
bottom and lands first as a validation step.

---

## Motivation

What works today:

```sh
cd turmeric-spices/spices/frame
tur build src/frame/csv.tur          # compiles, links a binary
tur run   tests/frame/csv_test.tur   # auto-discovers spice src/; runs
tur check src/frame/csv.tur          # auto-discovers; reports clean
```

What doesn't:

```sh
cd turmeric-spices/spices/frame
tur repl
> (import frame/csv :refer [read-csv])
error: module 'frame/csv' not found
> (read-csv-string "x,y\n1,2\n" 0 0 1 0 "")
error: unbound symbol 'read-csv-string'
```

There's no clean way for a user to:
- Open a frame at the REPL to inspect it after a script
- Pair the REPL with `tur run` to iterate on a CSV pipeline
- Use `tur-frame` / `tur-sqlite` / `tur-plutovg` interactively at all

The current escape hatches (write a one-off `.tur` that imports + runs,
or `tur eval '(...)'` for single expressions) lose history, bindings, and
incremental edit -- the things a REPL exists to provide.

---

## Architecture

```
                              tur repl
                                 |
                                 v
                +----------------+----------------+
                | Auto-discovery (spice tree)     |
                | -- walk up to build.tur,        |
                | -- enumerate :spices deps,      |
                | -- collect src/ + tests/        |
                +----------------+----------------+
                                 |
                                 v
                +----------------+----------------+
                | Build pipeline                  |
                | -- tur build --shared <tree>    |
                | -- emit .tur-repl-cache/lib.so  |
                |    (skip if mtimes unchanged)   |
                +----------------+----------------+
                                 |
                                 v
                +----------------+----------------+
                | dlopen + symbol-table prime     |
                | -- read manifest of exports     |
                | -- {module/name -> ffi_thunk}   |
                +----------------+----------------+
                                 |
                                 v
                +----------------+----------------+
                | REPL eval loop                  |
                | -- import resolves through map  |
                | -- inline-C defns -> FFI call   |
                | -- pure-Turmeric defns -> AST   |
                +---------------------------------+
```

Three subsystems:

1. **Build to shared library** (compiler/CLI work).
2. **FFI dispatch** (runtime work; libffi or hand-rolled).
3. **REPL module resolution** (interpreter work).

---

## Subsystem 1: `tur build --shared`

Today `tur build <dir>` produces an executable by codegen + cc + ld. The
linker invocation needs a small split:

- `-fPIC` on every translation unit (already cheap to add).
- Final link with `-shared -o lib<name>.so` (or `.dylib`/`.dll`) instead
  of the executable line.
- No `main()` symbol expected -- if the project has one, exclude it from
  the shared build via a separate codegen flag.

Additional outputs alongside the library:

- **`exports.manifest`** -- text file mapping
  `<module>/<defn-name> -> <mangled-c-symbol> :: <type-signature>`. The
  type signature is the small DSL `(int int) -> int`, `(cstr) -> int`,
  etc. -- enough for the FFI shim to construct the right call.
- **`build.manifest`** -- list of `(source-path, mtime, sha256)` tuples
  used for the up-to-date check.

Invocations:

```sh
tur build --shared <dir>             # implicit -o based on <dir>/build.tur :name
tur build --shared <dir> -o lib.so
tur build --shared --manifest <path> # also emit exports.manifest at this path
```

Cache location: `.tur-repl-cache/` inside the project root (next to
`build.tur`). Add to `.gitignore` automatically when first created.

---

## Subsystem 2: FFI dispatch

Once `dlopen(handle)` returns, each spice export is callable via
`dlsym(handle, "frame__csv__read_csv")` -- but the interpreter doesn't
know the C signature, and you can't call a `void*` of unknown arity in
portable C.

Two options:

### Option A: libffi (recommended)

Vendor or system-link [libffi](https://sourceware.org/libffi/). For each
imported defn, `ffi_prep_cif` once with the type signature from
`exports.manifest`; on call, marshal the interpreter's tagged values
into the right registers via `ffi_call`.

Pros:
- Battle-tested, cross-platform, handles every Turmeric type combo.
- Available on every platform Turmeric targets (Linux, macOS, FreeBSD,
  WASM via Emscripten port, Windows via mingw).
- Adds ~30-50KB to the REPL binary.

Cons:
- New dependency. Vendoring is straightforward (libffi is small and
  ASIZE-friendly), but it does add a build step.

### Option B: Hand-rolled dispatch table

Turmeric's foreign type universe is small:
- `:int` -> `int64_t` (8 bytes, integer register)
- `:cstr` -> `const char*` (8 bytes, integer register on 64-bit)
- `:float` -> `double` (8 bytes, vector register)
- `:bool` -> `bool` (1 byte, integer register)
- `:void` -> return only

Signatures distill to (return-class, arg-classes) where each class is
INT (int/cstr/bool) or FLOAT. For arity 0..8 that's 2 * 2^8 + (arity-0
return-only) = ~512 dispatcher functions, but in practice ~30 covers
everything in the existing spice tree.

A small `gen-dispatchers.py` script could enumerate the common shapes
and emit C functions like:

```c
int64_t  call_i_i  (void *fn, int64_t a)                { return ((int64_t(*)(int64_t))fn)(a); }
int64_t  call_i_ii (void *fn, int64_t a, int64_t b)     { return ((int64_t(*)(int64_t,int64_t))fn)(a, b); }
double   call_f_i  (void *fn, int64_t a)                { return ((double(*)(int64_t))fn)(a); }
void     call_v_i  (void *fn, int64_t a)                { ((void(*)(int64_t))fn)(a); }
/* ... ~30 of these covering everything in turmeric-spices today */
```

Pros:
- No new dep. Stays in the existing build.
- Slightly faster than libffi (one indirect call vs the trampoline).

Cons:
- New signatures need a regeneration step (or runtime error: "no
  dispatcher for (cstr,int,float,float) -> int").
- More code to own.

**Recommendation: start with the hand-rolled table** (lower upfront work
and matches the project's "minimal C deps" stance), but design the
interface so libffi can drop in later if a signature explosion shows up.
The dispatcher table can live in `src/runtime/ffi_dispatch.c`.

---

## Subsystem 3: REPL module resolution

The REPL's reader / evaluator currently treats `(import ...)` as a parse
error (or unbound macro). Wiring it up:

1. **At REPL start** -- after AOT build + dlopen, walk every
   `exports.manifest` entry and populate an in-memory map:

   ```
   ("frame/csv", "read-csv")    -> (mangled "frame__csv__read_csv",
                                    sig "(cstr,int,int,int,int,cstr) -> int",
                                    handle <dlsym ptr>)
   ```

2. **When the user types `(import M :refer [a b c])`** -- look up each
   `(M, a)` / `(M, b)` / `(M, c)` in the map. For each:
   - Native (inline-C source): install a binding in the current REPL env
     whose value is an "FFI thunk" closure -- when called with N args,
     marshals them per the recorded signature and dispatches.
   - Pure-Turmeric: load `<spice-root>/src/<M>.tur` lazily, parse, and
     install the resulting AST nodes in the env (the existing
     interpreter behavior).

3. **Type-error surface** stays where it is today (the interpreter's
   arg-count and arg-type checks): the FFI thunk records the declared
   signature, so calling `(read-csv "x.csv" "abc" ...)` at the REPL
   produces the same `expected :int, got :cstr` error you'd get from
   `tur check`.

4. **For pure-Turmeric defns that **call** native ones**, the path is
   trivial: the interpreter steps through Turmeric AST until it hits a
   call to a name bound to an FFI thunk, marshals, dispatches.

5. **For native -> Turmeric callbacks (higher-order arg)** -- explicitly
   **not supported in v1**. Document as a future-work item; user-visible
   error is "this function takes a Turmeric callable; only native
   callables are bindable at the REPL today."

---

## Implementation phases

- [ ] **RP0** -- New CLI flag `tur build --shared <dir>`. Codegen tweak
  to skip `main`; linker line emits `-fPIC -shared`. Tested with a
  one-defn smoke project: build, then `dlopen` + `dlsym` from a hand-
  written C harness, confirm the symbol calls.

- [ ] **RP1** -- `exports.manifest` writer in the codegen path. Records
  `module/name -> mangled symbol :: signature` for every public defn.
  Includes per-arg type tags and the return type tag using the existing
  Turmeric type DSL.

- [ ] **RP2** -- `src/runtime/ffi_dispatch.c` with the hand-rolled
  dispatcher table. Generator script (`tools/gen_ffi_dispatch.py`)
  produces dispatchers for every (return-class, arg-classes) shape
  present in `turmeric-spices/spices/*/build.tur`. Unit tests:
  one trampoline per common shape.

- [ ] **RP3** -- REPL changes (in `src/turi/repl.c`):
  - Auto-discover `build.tur` walking up from cwd; skip if absent
    (then REPL behaves like today: pure-Turmeric, no spices).
  - Invoke `tur build --shared <tree>` as a subprocess (or in-process
    helper) on REPL start. Skip the rebuild when `build.manifest` says
    every source file is fresh.
  - `dlopen` the library, parse `exports.manifest`, populate the symbol
    map.

- [ ] **RP4** -- Interpreter binding integration. `(import M :refer [a])`
  consults the symbol map. Install FFI-thunk bindings for native exports;
  fall through to the existing AST-load path for pure-Turmeric ones.
  Both kinds coexist in the same env.

- [ ] **RP5** -- `(reload)` REPL form. Re-checks `build.manifest`,
  triggers a partial rebuild if anything changed, refreshes the symbol
  map for any bindings already imported into the current session. The
  REPL prompt prints which modules were rebuilt.

- [ ] **RP6** -- Watch mode (optional). `tur repl --watch` (or a session
  toggle) spawns an inotify / kqueue / FSEvents watcher on the spice
  src/. On change, queue a rebuild + reload; print a one-line summary
  before the next prompt.

- [ ] **RP7** -- Error surface polish:
  - "no dispatcher for signature X" -> hint pointing at
    `tools/gen_ffi_dispatch.py` (or libffi fallback if we adopt it).
  - "spice not built yet" -> auto-build prompt.
  - Symbol mismatch (manifest says fn exists, dlsym returns NULL)
    -> clear "stale manifest; run with --rebuild" message.

- [ ] **RP8** -- Documentation update:
  - `docs/repl.md` (new) explaining the workflow + cache layout.
  - CLAUDE.md note that the REPL now auto-discovers like other per-file
    commands.

---

## CLI changes

After RP3:

```sh
# Inside a project with build.tur:
tur repl
> (import frame/csv :refer [read-csv-string])
> (define f (read-csv-string "x,y\n1,2\n" 0 0 1 0 ""))
> (print-frame f)
| x | y |
| int64 | int64 |
|-------|-------|
| 1     | 2     |
> (reload)
# rebuilt frame/csv.tur (mtime changed); 1 binding refreshed
```

Outside a project, behaves exactly as today (pure-Turmeric REPL, no
spice imports available; clear message on attempted import).

Flags (all on `tur repl`):

| Flag | Effect |
|------|--------|
| `--no-auto-spice` | Skip auto-discovery; pure-Turmeric mode |
| `--rebuild` | Force a rebuild even if `build.manifest` is fresh |
| `--watch` | Watch spice src/, auto-rebuild on change |
| `--load <lib.so>` | Skip auto-discovery; load an explicit pre-built library (see [Smaller variant](#smaller-variant-explicit-load-flag)) |

---

## Cache layout

`.tur-repl-cache/` next to `build.tur`:

```
.tur-repl-cache/
  lib.so                  -- or libfoo.dylib / foo.dll
  exports.manifest        -- module/name -> symbol :: signature
  build.manifest          -- (path, mtime, sha256) per source file
  c-cache/                -- intermediate .c + .o files for incremental rebuild
```

Auto-added to `.gitignore` on first build. The whole directory is
reproducible from source, so deleting it just costs one rebuild.

---

## Tests

- `tests/repl/spice-import.sh`: integration test that runs `tur repl`
  in a fixture project, types an import + call, asserts the expected
  output via a transcript file (similar to existing
  `tests/fixtures/*` shape).

- `tests/repl/rebuild.sh`: build, touch a source file, send `(reload)`,
  assert the function returns the new behavior.

- `tests/repl/no-spice.sh`: confirm REPL still works outside a project
  (pure-Turmeric expressions evaluate; `(import frame/csv)` returns a
  clear "no project here" error).

- `tests/runtime/ffi-dispatch-unit.c`: each generated dispatcher
  function tested with a known callable. Catches ABI mistakes before
  they surface as silent value corruption.

- `tests/repl/error-surface.sh`: a few negative cases (missing symbol,
  stale manifest, dispatcher gap) and assert the error message
  surfaces the right next step.

---

## Risks and open questions

1. **Symbol name collisions across spices.** Two spices that both
   define `frame/foo` would mangle to the same C symbol. The compiler
   already enforces unique module names within a build, but the
   shared-lib path makes the failure more visible (linker error
   instead of build-time error). Document; add a friendlier diagnostic.

2. **Cross-spice transitive deps.** When project A depends on spice B
   which depends on spice C, the `--shared` build needs to include
   all three trees and arbitrate symbol visibility. The existing
   `build.tur :spices` map already records this; we just need to
   make sure the linker invocation pulls in everything.

3. **WASM builds.** WASM doesn't have `dlopen` in the conventional
   sense; Emscripten has its own dynamic linking story. v1 of this
   plan is native-only; document that the WASM REPL stays
   interpreter-only. (Not a regression -- it's interpreter-only today.)

4. **Inline-C globals.** A few spices stash file-scope static state
   in inline-C blocks (the FNV-1a sort statics in `frame/sort.tur`,
   the timestamp parser cache in `frame/csv.tur`). Those work today
   because each `tur build` is a fresh process. At the REPL the same
   library lives across many evaluations; static state persists.
   This is **probably desired** (caching) but needs an explicit
   audit. Document the new contract: "static C state in spice defns
   persists for the lifetime of the REPL session."

5. **Threading.** The interpreter is single-threaded; the loaded
   library may launch threads (rare in turmeric-spices today, but
   `tur-rtaudio` does). At REPL teardown we need to give those
   threads a chance to stop. v1: document. v2: explicit shutdown hook
   per spice.

6. **Reload safety for refcount-shared state.** `tur-frame` columns
   share refcounts via shared buffers. If a `(reload)` swaps the
   shared library mid-session, any column allocated by the old image
   becomes a dangling pointer. v1 mitigation: `(reload)` invalidates
   all live FFI-thunk bindings AND forces a fresh repl env (`(clear)`
   semantics). v2: per-image generation tagging.

7. **`tools/gen_ffi_dispatch.py` drift.** As spices grow, new
   signature shapes will appear. CI should run a check that every
   public defn's signature is covered by a generated dispatcher.
   Failure mode without the check: a runtime "no dispatcher" error
   the first time a user imports the function -- noisy but not
   silent corruption.

8. **Debuggability.** When an FFI call segfaults, the user sees a
   process crash with no Turmeric-level stack frame. v1: print the
   last imported call name + args on `SIGSEGV` via a handler. v2:
   integrate with `lldb` / `gdb` via the existing debug-info path.

---

## Smaller variant: explicit `--load` flag

If a week of work is too much to commit to up front, an 80% slice
takes about a day:

- Add `tur build --shared <dir>` (RP0) and `exports.manifest` (RP1).
- Skip auto-discovery + watch + (reload). Add a single
  `tur repl --load <lib.so> --manifest <path>` invocation.
- User runs `tur build --shared .` once, then
  `tur repl --load .tur-repl-cache/lib.so --manifest .tur-repl-cache/exports.manifest`.
- The REPL behaves as in the full plan -- imports resolve, FFI thunks
  dispatch -- but the user manually re-runs the build when they edit
  source files.

This is enough to:
- Validate the FFI dispatcher works end-to-end.
- Confirm the manifest schema covers the spice tree.
- Get interactive spice usage in front of users for feedback.

The full plan's auto-discovery + watch + (reload) can land in a follow-up
once the foundation is proven.

---

## Future work

- **libffi adoption** if the hand-rolled dispatcher table grows
  unwieldy or starts requiring shape-explosion entries.
- **Native -> Turmeric callbacks** so spices can take Turmeric closures
  as args (needed for, e.g., custom sort comparators or filter
  predicates).
- **Shared-library hot-swap on `(reload)`** without invalidating live
  bindings -- requires a generation marker on every column / frame /
  spice value and an indirection through a per-generation symbol map.
- **WASM REPL** via Emscripten's `MAIN_MODULE` + `SIDE_MODULE` dynamic
  linking. The REPL is then a webapp that loads spice modules at
  runtime; matches the web-REPL that already exists in `web/`.
- **Notebook integration.** A `tur-notebook` spice that wraps the REPL
  in a Jupyter-style cell interface, reusing the same FFI dispatcher
  for cell-by-cell execution.
- **Profile-guided AOT.** Once the REPL workflow is in real use,
  collect hot-path call counts and feed them back to the codegen for
  inlining decisions. Out of scope for v1 but a natural fit later.
