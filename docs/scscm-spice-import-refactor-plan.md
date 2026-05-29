# Plan: scscm Spice Import Refactor + `scscm-compile` Fixture Repair

> **Status:** Draft Plan
> **Last Updated:** 2026-05-29
> **Type:** Spice refactor + test-fixture repair
> **Related:**
> - `../turmeric-spices/spices/scscm/` (the spice being refactored)
> - `tests/fixtures/scscm-compile/` (the broken fixture this plan unblocks)
> - `docs/guides/developing-spices-guide.md` (per-file `tur check` rules)
> - `docs/local-spice-dev-workflow-plan.md` (`:path` resolution gaps)

---

## Overview

`tests/fixtures/scscm-compile` is the only `requires.spices` fixture in
the repo and has been failing on `main` since the scscm spice was
restructured around per-file "cross-file type stubs" (turmeric-spices
commit `7366106`). The fixture uses `(load "...")` to inline five spice
source files into a single translation unit; the spice's stub pattern
emits duplicate `static` C functions across those files and clang
rejects the merged TU.

The fixture failure surfaces a deeper issue: **the spice itself does
not currently build as a project either** -- `tur build
../turmeric-spices/spices/scscm` errors with `unbound symbol
'tur-scscm'` from its own `build.tur`, and `compile.tur` calls
`tokenize` / `parse` / `expand-all` / `generate-all` from local stub
bodies that just `return NULL`. So in any compilation model that links
multiple spice files together (whether via `load` or via project mode),
the spice is broken.

This plan repairs both: convert the spice to proper `defmodule` +
`import`, drop the stub pattern, and rewrite the fixture as a small
project that imports `scscm/compile`.

## Scope and non-scope

In scope:
- Wrapping each of `lexer.tur`, `parser.tur`, `codegen.tur`,
  `expander.tur`, `compile.tur` in `(defmodule scscm/<name> ...)` with
  explicit `(export ...)` lists.
- Replacing every "cross-file type stub" block with `(import scscm/<other>
  :refer [...])`.
- Verifying per-file `tur check` / `tur emit-c` still works inside the
  spice tree (via the auto-spice `build.tur` walk-up described in
  CLAUDE.md "Per-file Commands Inside a Spice").
- Fixing `build.tur` so `tur build .` on the spice succeeds end-to-end.
- Rewriting `tests/fixtures/scscm-compile/scscm-compile.tur` to use
  `(defmodule ...) (import scscm/compile :refer [...])` instead of
  `(load ...)`.
- Adding a fixture-local `build.tur` that declares the scscm spice as
  a local-path dependency, and marking the fixture
  `requires.dedicated-runner` (since `tur build <file>` does not
  auto-discover; project mode does).
- Adding a ctest target in `CMakeLists.txt` for the new dedicated
  runner.

Out of scope:
- Compiler changes (new `(declare ...)` form, `(when-undefined ...)`
  macro, or `(load ...)`-time dedup). Those would also fix the problem
  but are larger and not needed once the spice uses real imports.
- Other spices in `../turmeric-spices` that may share the stub pattern
  (`tidal`, possibly others). Inventory + cleanup of those should be a
  follow-up once the scscm migration validates the approach.
- Re-exporting low-level Result helpers (`lex-ok?`, `lex-err?`,
  `lex-ok-val`, etc.) into a shared `scscm/result` module. We can do
  that in a v2 if the duplication-via-import becomes painful; v1 just
  exports them from `scscm/lexer` where the real implementations live.
- Renaming `lex-*` back to plain `ok-*` / `err-*`. The `lex-` prefix
  was a deliberate naming choice in turmeric-spices `7357f3f`
  ("Phase 1 Category A").

## Current state

### What the spice looks like today

```
turmeric-spices/spices/scscm/
  build.tur            -- (defpackage tur-scscm :exports {...})
  src/scscm/
    lexer.tur          -- flat defns (no defmodule); REAL impls of
                          tokenize, lex-ok, lex-err, scscm-cstr-as-int,
                          token-type, ...
    parser.tur         -- flat defns; REAL parse/ast-*; STUB copies
                          of lex-*, scscm-cstr-eq?, token-{type,value}
    codegen.tur        -- flat defns; REAL generate-all; STUB copies
                          of lex-*, ast-*, scscm-cstr-eq?, ...
    expander.tur       -- flat defns; REAL expand-all; STUB copies
                          of ast-*, scscm-cstr-eq?, ...
    compile.tur        -- flat defns; REAL compile-text/compile-file;
                          STUB copies of tokenize, parse, expand-all,
                          generate-all, ast-free-all, lex-*, ... that
                          all `return NULL` or pass through their input.
```

Each "STUB copy" is a full `(defn ... #{Unsafe} ... ```c ... ```)`
emitting a `static` C function. In `lexer.tur` / `parser.tur` /
`codegen.tur` / `expander.tur` the stub bodies are real implementations
of helpers borrowed from another file (so each file is self-sufficient
for per-file `tur emit-c`). In `compile.tur` the stub bodies are no-ops
(`return NULL`, etc.) -- which means **if you actually run `compile.tur`
in isolation the pipeline silently does nothing**.

### What breaks

| Surface                          | Result |
|----------------------------------|--------|
| `tur emit-c src/scscm/<file>.tur` | succeeds (stubs make each file self-contained) |
| `tur build .` on the spice       | fails: `unbound symbol 'tur-scscm'` from build.tur |
| `tur build` of any single file that `(load ...)`s multiple spice files | fails: duplicate static C functions |
| `tests/fixtures/scscm-compile`   | fails as above |

### Why the fixture's rename didn't fix it

Commit `047d7bdf` renamed `ok?`/`err?`/`ok-val` -> `lex-ok?`/`lex-err?`/`lex-ok-val`
in the fixture (the spice was renamed in turmeric-spices `7357f3f`).
That fix is correct on its own but is masked by the underlying
duplicate-symbol failure -- it just moves the failure from the Turmeric
type-checker to the C compiler.

## Plan

### Phase 1 -- Make the spice build as a project

1. Fix `build.tur` so `tur build .` from `spices/scscm/` works. The
   current error (`unbound symbol 'tur-scscm'`) suggests `defpackage`
   is being elaborated as a normal call instead of a special form;
   reproduce, check whether the package name needs quoting or whether
   this is a regression in `tur` itself. (If it's a `tur` regression
   it's a hard blocker for this plan and gets split out.)
2. Convert each src file to `(defmodule scscm/<name> ...)` with
   explicit `(export <symbol-list>)`. The `:exports` map in
   `build.tur` is the authoritative list to mirror.
3. Without removing stubs yet, verify each module still emits
   correctly via per-file `tur emit-c`. This is the "stable baseline"
   commit.

### Phase 2 -- Replace stubs with imports

For each file in dependency order (`lexer` -> `parser` ->
`codegen`/`expander` -> `compile`):

1. Delete the `;; ---- cross-file type stubs ----` block.
2. Add `(import scscm/<other> :refer [<symbols>])` for every name the
   block was satisfying.
3. Run `tur check src/scscm/<file>.tur` and confirm the auto-spice
   `build.tur` walk-up resolves the import. (Per CLAUDE.md: "the
   spice's `src/` is added to the module-resolution search path".)
4. Run `tur build .` on the spice tree end-to-end.
5. (Optional) Build & run a tiny smoke program inside the spice's own
   `tests/` directory that calls `compile-text` on a fixed input and
   compares the output.

The dependency graph is acyclic:
- `lexer` has no intra-spice deps (lex/Result helpers live here).
- `parser` depends on `lexer` (Result helpers, token accessors).
- `codegen` depends on `lexer` (Result helpers) and `parser` (AST
  accessors).
- `expander` depends on `lexer` and `parser`.
- `compile` depends on all four.

So no `defmodule` cycles to worry about.

### Phase 3 -- Repair the fixture

1. Add `tests/fixtures/scscm-compile/build.tur`:
   ```turmeric
   (defpackage scscm-compile-test
     :modules [scscm-compile-test/main]
     :spices #{
       "tur-scscm" #{:path "../../../../turmeric-spices/spices/scscm"}
     })
   ```
2. Move the fixture body into `src/scscm-compile-test/main.tur`,
   wrapped in `(defmodule scscm-compile-test/main (import scscm/compile
   :refer [compile-text]) (import scscm/lexer :refer [tokenize lex-ok?
   lex-err? lex-ok-val scscm-int-as-cstr]) ...)`.
3. Delete `(load ...)` calls entirely.
4. Add `requires.dedicated-runner` to the fixture (so `tests/run.sh`
   skips it; project-mode fixtures already follow this pattern -- see
   `tests/fixtures/spice-resolver-deps/`).
5. Add a ctest target in `CMakeLists.txt` mirroring the existing
   `tur_eval_*` / spice-resolver dedicated runners. The target builds
   the fixture project (`tur build tests/fixtures/scscm-compile`) and
   diffs `actual.stdout` against `expected.stdout`.

### Phase 4 -- Verify and document

1. Confirm `bash tests/run.sh` passes with the new fixture
   skipped-as-PASS via `requires.dedicated-runner`.
2. Confirm `ctest -R scscm_compile` (or whatever target name is
   chosen) passes.
3. Confirm `requires.spices` still skips properly when
   `../turmeric-spices` is absent (add the same check to the ctest
   target -- skip-with-pass when the sibling repo isn't there).
4. Update `docs/guides/developing-spices-guide.md` to note that
   cross-file stubs are an anti-pattern and `(import ...)` is the
   blessed approach. Reference this plan.

## Risks

1. **The `defpackage` error in Phase 1 may not be a quick fix.** If it
   turns out to be a `tur` regression rather than a malformed
   `build.tur`, Phase 1 expands into a separate compiler fix and this
   whole plan stalls behind it.
2. **`requires.dedicated-runner` adds CI surface area.** The new ctest
   target needs to gracefully skip when the sibling repo is absent
   (current dedicated-runner fixtures don't all do this -- check the
   pattern in `CMakeLists.txt`).
3. **Other spices may share the stub pattern.** If `tidal/`, etc. also
   use cross-file stubs, they'll have the same fragility waiting to be
   discovered when something tries to `(load ...)` them. Inventory
   them as a follow-up but don't block this plan on cleaning them all
   up.
4. **`:path` spice deps may not actually resolve modules end-to-end.**
   `docs/local-spice-dev-workflow-plan.md` explicitly calls out that
   the `:path` form is "accepted by the manifest parser ... but does
   not actually resolve modules". If that's still true,
   Phase 3 step 1 won't work and the fixture needs a different
   wiring (URL-based dep, or the gap from that other plan needs to
   land first). **This is the biggest unknown -- validate before
   committing to Phase 3.**

## Validation checklist

Before declaring this plan done:

- [ ] `tur build ../turmeric-spices/spices/scscm` exits 0.
- [ ] `tur emit-c` on every individual file in the spice exits 0,
      with no duplicate-symbol or unbound-symbol diagnostics.
- [ ] A smoke program inside the spice that calls `compile-text "(+ 1 2)"`
      prints the expected sclang output.
- [ ] `bash tests/run.sh` passes with 0 failures.
- [ ] `ctest -R scscm` passes.
- [ ] `bash tests/run.sh` still reports scscm-compile as PASS (skipped)
      when `../turmeric-spices` is absent.

## Out of scope but worth flagging

- The `compile.tur` stubs that `return NULL` mean the spice's
  `compile-text` is silently a no-op today even outside the fixture.
  Anyone who has been "using" the spice via its build.tur project
  output has been getting empty results. Worth a quick note in the
  spice's commit message during Phase 2.
- `docs/local-spice-dev-workflow-plan.md` proposes the `:members`
  workspace mechanism that would make Phase 3 trivial -- if that
  plan lands first, the fixture's `build.tur` could declare the
  sibling spice with `:members` instead of `:path`.
