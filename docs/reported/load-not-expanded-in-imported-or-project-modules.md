---
title: Top-level (load "...") is not expanded inside imported / project-mode modules -- arrow.tur's >>> is unreachable from a defmodule that is imported or project-built
category: Reported
severity: high
status: LARGELY RESOLVED -- import path fixed; project-mode typeclass-ordering fixed; project-mode bare-defn `load` fixed; project-mode single-module `load` of a runtime-preamble-dependent file (poly-fn / higher-kinded dict / spliced ADT) now emits the missing per-module scaffolding (`tur_poly_fn_t`, base `tur_adt_*` typedef + ctors, fn-ptr typedefs) and compiles + runs. All regression-covered. Two narrower follow-ons remain: (a) stdlib files that use ambient types they do not themselves `(load ...)` (e.g. arrow.tur's `Tuple2`), and (b) a typeclass instance leaking into an *importer's* TU. See "Status".
description: The (load "path") preprocessor runs only on the entry compilation unit (elaborate_program). A module pulled in via `import` (elab_load_module) elaborates its forms directly and never runs the preprocessor, so a top-level `(load ...)` in that file hits elab_load and errors with "load is only valid at the top level". In `tur build .` project mode the load is reached but its transitive loads resolve against an incomplete typeclass environment ("typeclass 'Functor' is not defined" from either.tur). Net effect: a defmodule cannot `(load "stdlib/arrow.tur")` to reach `>>>` in any build mode that goes through the module system.
---

# `load` is a no-op-then-error inside imported / project-built modules

## Status (2026-06-06)

- **Import path (`tur run`/`tur check` of a consumer that `import`s the module):
  FIXED.** `elab_load_module` now runs the `(load ...)` preprocessor over the
  imported file's forms before elaboration (sharing a compilation-global
  visited set with the entry), and registers the spliced top-level definitions
  for emission. A `defmodule` that `(load "stdlib/arrow.tur")`s can now reach
  `>>>` through `import`. Regression fixture:
  `tests/fixtures/load-in-imported-module/`.
- **Project-mode typeclass ordering (`tur build .`): FIXED.** `stdlib/either.tur`
  now `(load "stdlib/typeclass-functor.tur")`s its own `Functor` dependency
  instead of relying on the full stdlib auto-load (which project mode does not
  run). The "typeclass 'Functor' is not defined" error is gone.
- **Project-mode bare-defn `load` (`tur build <dir>`): FIXED + regression-covered
  (2026-06-10).** A `defmodule` that top-level-`(load ...)`s a *bare-defn* stdlib
  file (e.g. `stdlib/math.tur`) now builds and links in separate-compilation
  project mode, and an exported defn that uses the spliced names (`sqrt`/`floor`)
  is callable cross-module. Locked in by the
  `build-project-load-bare-defn-module` scenario in
  `tests/run-build-project.sh` (builds a two-module project where `app/ops`
  loads `stdlib/math.tur` and `app/main` imports it; the binary returns
  `floor(sqrt(3*3+4*4)) = 5`).
- **Project-mode separate-compilation codegen for *runtime-preamble-dependent*
  loads: FIXED for the single-module case + regression-covered (2026-06-10).**
  The blocker was **not** "any typeclass in project mode" -- a *monomorphic*
  instance always compiled and ran under `tur build <dir>` (verified:
  `(defclass Eqx [a] (eqx? [x y] :bool))` + `(definstance Eqx [int] ...)`). What
  broke was loading content that needs runtime scaffolding the per-module `.c`
  never emitted: rank-2 dispatch typed `tur_poly_fn_t`, the base `tur_adt_*`
  typedef + its `ctor_*` allocators for a spliced ADT, and the on-demand
  `tur_fnptr_*` fn-ptr typedefs. A `tur build <dir>` of a single module that
  `(load "stdlib/either.tur")`s (pulling in the `Either` ADT + the higher-kinded
  `Functor` instance whose `fmap` dispatches through `tur_poly_fn_t`) now
  compiles, links, and runs: `fmap (*2)` over `(Right 21)` yields `42`. Locked in
  by `build-project-load-higher-kinded-module` in `tests/run-build-project.sh`.

  Fix (`src/compiler/emit_module.c`):
  1. The base ADT Pass-0 emission (`typedef struct tur_adt_<Name>` + one
     `ctor_<Ctor>` per constructor) was extracted into a shared
     `emit_adt_typedef_and_ctors` helper, now called by both `emit_program`
     (whole-program) and a new Pass 0 in `emit_implementation` (per-module
     `.c`). Base ADT typedefs/ctors live in the `.c`, not the header, so they
     are TU-local and never collide across modules. Struct typedefs are *not*
     re-emitted in the `.c` -- the header already emits them all.
  2. `emit_header` now declares `tur_poly_fn_t` (guarded by
     `#ifndef TUR_POLY_FN_T_DEFINED` so a module that includes several module
     headers does not redefine the anonymous struct), so both the header's
     exported signatures and the `.c`'s internal typeclass-method signatures
     resolve it.
  3. `emit_implementation` flushes the `tur_fnptr_*` fn-ptr typedefs registered
     while emitting bodies (identical fn-ptr typedef redefinition is well-formed,
     so this stays conflict-free). Monomorphized ADT-application structs are
     deliberately *not* re-flushed here -- the header already emits them for
     exported uses and an anonymous-struct redefinition would be ill-formed.

- **Remaining follow-ons (narrower, distinct from the load-codegen gap):**
  1. **Ambient-stdlib dependencies in a loaded file.** `tur build <dir>` of a
     module that `(load "stdlib/arrow.tur")`s still fails, but now on
     `unknown type name 'Tuple2'`: arrow.tur's inline-C uses `Tuple2` (a
     `stdlib/tuple.tur` defstruct) without `(load "stdlib/tuple.tur")`ing it,
     relying on the whole-program stdlib auto-load that project mode does not
     run. The fix is stdlib hygiene -- arrow.tur should load its own `Tuple2`
     dependency (as it already does for `Functor` via either.tur) -- not a
     codegen change.
  2. **Instance leaking into an importer's TU.** When module B `(import)`s
     module A, and A `(load ...)`s a typeclass instance, elaborating B
     re-runs A's load and emits A's instance method (with inline-C referencing
     `tur_adt_*`) into B's `.c`, where the base ADT typedef is absent
     (`unknown type name 'tur_adt_Either'` in B). B should call A's exported
     entry points and dispatch through A's dictionary rather than re-emitting
     the instance; this is a cross-module typeclass-instance ownership issue,
     separate from per-module load codegen.

## Summary

**Severity: High.** `(load "path")` is a top-level-only preprocessor directive.
It is expanded only for the **entry** compilation unit. A file pulled in through
the module system (`import`, or `tur build .`/`tur build <dir>` project descent)
does **not** get its top-level `(load ...)` forms expanded, so:

- via `import` (`tur run`/`tur check` of a consumer): the `load` reaches
  `elab_load` and errors `load is only valid at the top level; move it before
  the enclosing defmodule or defn` -- even though it already *is* before the
  `defmodule`, at column 1.
- via `tur build .` (project mode through `build.tur`): the load is processed,
  but its transitive `(load "stdlib/either.tur")` resolves `Functor` against an
  environment that has not yet loaded `stdlib/typeclass.tur`, so it errors
  `typeclass 'Functor' is not defined`.

Either way a `defmodule` that wants `>>>` from `stdlib/arrow.tur` cannot get it.

This is the structural follow-on to
[load-inside-defmodule-silently-loses-names.md](load-inside-defmodule-silently-loses-names.md):
PR #303 made an *in-body* load a loud error, but a *top-level* load in an
*imported* file is still silently skipped and then errors at the use site.

## Minimal repro

`spices/signal/src/signal/compose.tur` (a defmodule), top of file:

```turmeric
(load "stdlib/arrow.tur")          ;; column 1, before the defmodule

(defmodule signal/compose
  (export effects-chain)
(defn effects-chain
  [effects : int ^fat input : (fn [float] #{} float)] : ptr<void>
  (>>> input input))
) ;; end
```

- `tur check src/signal/compose.tur` (standalone entry) -> **exit 0**. The
  preprocessor runs; `>>>` resolves.
- `tur run tests/signal/<consumer>.tur` where the consumer does
  `(import signal/compose ...)` -> **error**:
  `compose.tur:1:1: error: load is only valid at the top level; move it before
  the enclosing defmodule or defn`.
- `tur build .` from the spice root -> **error**:
  `stdlib/either.tur:185:14: error: typeclass 'Functor' is not defined`
  (arrow.tur -> either.tur transitively, with no typeclass.tur in scope).

Workaround that *does* compile (but is not viable for a library): have the
**entry** file `(load "stdlib/arrow.tur")` at top level before importing the
module. Because `import` shares the entry's `Elab` global scope, the imported
module then sees `>>>`. This only helps examples/tests with a hand-written
entry; the library's own `tur build` has no such entry, so it cannot work this
way.

## Root cause

`src/compiler/elab_toplevel.c:838-849` (`elaborate_program`) runs
`load_expand_forms` over the entry forms before the two-pass elaboration -- this
is the *only* call site of the preprocessor.

`src/compiler/elab_module.c:242-291` (`elab_load_module`) reads the imported
file with `read_all_with_registry` and then iterates straight into
`elab_form(e, forms[i])` (line 273-291). It never calls `load_expand_forms`, so
a `(load ...)` form survives to elaboration and reaches `elab_load`
(`elab_module.c:38-41`), which is hard-wired to error.

The project-mode (`tur build .`) path *does* expand the load but threads the
sub-loaded files through an elaboration order that has not established the
typeclass environment arrow.tur -> either.tur depends on -- a transitive-load
ordering bug on top of the missing-expansion bug.

## Proposed fix directions

1. **Run `load_expand_forms` in `elab_load_module`** before the
   `elab_form` loop, sharing a *compilation-global* visited-path set so a path
   loaded by the entry (or by another imported module) is not spliced twice
   (today `LoadExpandCtx.loaded` is per-call). Spliced top-level defns from the
   loaded file must also be registered for emission (the loop currently only
   `elab_register_file_def`s `EX_DEFMODULE` at line 287-290; bare defns from
   `arrow.tur` would elaborate into scope but never reach codegen -> link
   errors). This is the principled fix and the larger one.
2. **Resolve the transitive typeclass ordering** so arrow.tur's
   `(load "stdlib/either.tur")` (which needs `Functor`) composes in project
   mode -- ensure typeclass.tur is in scope before either.tur, regardless of
   which file triggered the load.
3. **Sidestep for the spice**: do not use `load` from a library module at all;
   keep composition self-contained (the interim `__chain-loop`/`__apply-sf`
   fold in `compose.tur`). This is what tur-signal ships today; it does not need
   `>>>` and so does not need arrow.tur loaded. (Note: it shares the captureless
   blocker -- see
   [captureless-closure-not-boxed-at-fat-ptr-void-boundary.md](captureless-closure-not-boxed-at-fat-ptr-void-boundary.md).)

## How to validate

- **Done:** standalone `tur check` (preprocessor runs), `tur run`/`emit-c` of an
  importing consumer (fixture `tests/fixtures/load-in-imported-module/`),
  `tur build <dir>` of a project whose module top-level-`load`s a *bare-defn*
  stdlib file (scenario `build-project-load-bare-defn-module`), and
  `tur build <dir>` of a single module that `load`s a *runtime-preamble-dependent*
  stdlib file -- `stdlib/either.tur`, exercising the `Either` ADT + higher-kinded
  `Functor` instance + `tur_poly_fn_t` dispatch (scenario
  `build-project-load-higher-kinded-module`). Both project-mode scenarios live in
  `tests/run-build-project.sh`; run with
  `ASAN_OPTIONS=detect_leaks=0 bash tests/run-build-project.sh`.
- A loaded path is spliced **once** even when both the entry and an imported
  module load it (no duplicate-symbol C errors).
- `bash tests/run.sh` stays green.
- **Still open (narrower follow-ons, see Status):** (a) a loaded stdlib file
  that uses an ambient type it does not itself `(load ...)` (arrow.tur's
  `Tuple2`) -- an stdlib-hygiene fix, not codegen; and (b) a typeclass instance
  re-emitted into an *importer's* TU when the importer transitively re-runs the
  load -- a cross-module instance-ownership issue.

## Impact

With both this and
[captureless-closure-not-boxed-at-fat-ptr-void-boundary.md](captureless-closure-not-boxed-at-fat-ptr-void-boundary.md)
open, `tur-signal` Phase 5 cannot replace its interim `__chain-loop` fold with
`>>>`. See `docs/upcoming/tur-signal-rebuild-plan.md` Phase 5.
