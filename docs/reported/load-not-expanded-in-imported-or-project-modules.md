---
title: Top-level (load "...") is not expanded inside imported / project-mode modules -- arrow.tur's >>> is unreachable from a defmodule that is imported or project-built
category: Reported
severity: high
status: RESOLVED -- import path fixed; project-mode typeclass-ordering fixed; project-mode bare-defn `load` fixed; project-mode `load` of a runtime-preamble-dependent file (poly-fn / higher-kinded dict / spliced ADT / `^fat` combinators) now emits the missing per-module scaffolding (`tur_poly_fn_t`, base `tur_adt_*` typedef + ctors, fn-ptr typedefs, and the link-safe closure/fat fixed runtime: `__tur_fatshim*` / `__tur_poly_to_fat*` / `TUR_APPLY*`) and compiles + runs; arrow.tur is self-contained re: tuple.tur's `Tuple2`; and an imported module's typeclass instance no longer leaks into the importer's TU under separate compilation. All regression-covered (`tests/run-build-project.sh`). See "Status".
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

- **Follow-on 1 -- ambient-stdlib dependencies in a loaded file: FIXED
  (2026-06-10).** `tur build <dir>` of a module that `(load "stdlib/arrow.tur")`s
  previously failed on `unknown type name 'Tuple2'`: arrow.tur's `__arrow_pair_*`
  inline-C used `Tuple2` (a `stdlib/tuple.tur` defstruct) without loading it,
  relying on the whole-program auto-load that project mode does not run. Resolved
  as stdlib hygiene: the `__arrow_pair_*` helpers now use a local
  layout-compatible pair struct `P { int64_t e1; int64_t e2; }` instead of the
  external `Tuple2`, exactly as the sibling `__ac_pair_*` helpers in the Arrow
  instance already do, so arrow.tur is self-contained. (Loading tuple.tur itself
  was *not* the right fix: tuple.tur's generic `tuple2` exposes a separate,
  pre-existing parametric-struct-by-value ABI inconsistency that whole-program
  masks via specialization/pruning -- tracked separately, out of scope here.)
  Compiling arrow.tur in project mode additionally needed the `^fat` runtime
  (below), which is now emitted. Regression: `build-project-load-arrow-fatshim`.

- **`^fat` / closure fixed runtime under separate compilation: FIXED
  (2026-06-10).** A side discovery while finishing follow-on 1: *any*
  separate-compilation module using a `^fat` parameter failed with
  `'__tur_fatshim1' undeclared` (reproduced with zero `load`s), because the
  whole-program runtime preamble -- which defines the `^fat` auto-shims, the
  poly-to-fat thunks, and the `TUR_APPLY*` inline-C macros -- is not emitted in
  separate compilation. The *link-safe* subset of that preamble (everything
  `static` or a macro/typedef; no external-linkage symbols) was extracted into a
  shared `emit_closure_fat_runtime` helper, now emitted by both `emit_program`
  (whole-program) and per module `.c` in `emit_implementation`. The
  external-linkage runtime (STM, TVar, etc.) is deliberately *not* duplicated --
  that would collide at link time -- which is why a wholesale preamble copy was
  never viable.

- **Follow-on 2 -- typeclass instance leaking into an importer's TU: FIXED
  (2026-06-10).** When module B `(import)`s module A and A `(load ...)`s a
  typeclass instance, elaborating B re-ran A's load and self-registered A's
  instance/method/class defs into B's translation unit (B's `.c` carried A's
  `__inst_*` method body referencing `tur_adt_Either`, whose typedef is absent in
  B -> `unknown type name 'tur_adt_Either'`). Root cause: `elab_definstance`
  (and the class/method registration sites) call `elab_register_file_def`
  unconditionally, bypassing the `!separate_compilation` guard that
  `elab_load_module` applies to bare spliced defns. Fixed by tracking an
  `in_imported_module` flag (set across `elab_load_module`'s elaboration loop)
  and gating `elab_register_file_def`: under separate compilation, defs
  elaborated while loading an imported module are not registered for emission in
  the importer's TU -- they belong to the owner module's own TU, which the
  importer `#include`s the header of. Regression:
  `build-project-import-higher-kinded`.

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
  `build-project-load-higher-kinded-module`); `tur build <dir>` of a module that
  `load`s `stdlib/arrow.tur` and composes via the `^fat` bare `>>>` arrow
  (scenario `build-project-load-arrow-fatshim`); and `tur build <dir>` of a
  two-module project where B `import`s A and A `load`s a higher-kinded instance,
  verifying the instance does not leak into B's TU (scenario
  `build-project-import-higher-kinded`). All project-mode scenarios live in
  `tests/run-build-project.sh`; run with
  `ASAN_OPTIONS=detect_leaks=0 bash tests/run-build-project.sh`.
- A loaded path is spliced **once** even when both the entry and an imported
  module load it (no duplicate-symbol C errors).
- `bash tests/run.sh` stays green (arrow/category fixture snapshots regenerated
  for arrow.tur's local-pair-struct change).
- **Out of scope (distinct pre-existing issue, not a `load` follow-on):** the
  generic parametric-struct-by-value ABI inconsistency in `tuple.tur`'s `tuple2`
  (`make-struct` boxes a carrier while the generic `(.e1 t)` reads it by value),
  which whole-program masks via specialization/pruning and separate compilation
  would expose if tuple.tur were loaded wholesale.

## Impact

With both this and
[captureless-closure-not-boxed-at-fat-ptr-void-boundary.md](captureless-closure-not-boxed-at-fat-ptr-void-boundary.md)
open, `tur-signal` Phase 5 cannot replace its interim `__chain-loop` fold with
`>>>`. See `docs/upcoming/tur-signal-rebuild-plan.md` Phase 5.
