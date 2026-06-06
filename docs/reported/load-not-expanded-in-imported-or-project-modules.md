---
title: Top-level (load "...") is not expanded inside imported / project-mode modules -- arrow.tur's >>> is unreachable from a defmodule that is imported or project-built
category: Reported
severity: high
description: The (load "path") preprocessor runs only on the entry compilation unit (elaborate_program). A module pulled in via `import` (elab_load_module) elaborates its forms directly and never runs the preprocessor, so a top-level `(load ...)` in that file hits elab_load and errors with "load is only valid at the top level". In `tur build .` project mode the load is reached but its transitive loads resolve against an incomplete typeclass environment ("typeclass 'Functor' is not defined" from either.tur). Net effect: a defmodule cannot `(load "stdlib/arrow.tur")` to reach `>>>` in any build mode that goes through the module system.
---

# `load` is a no-op-then-error inside imported / project-built modules

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

- The repro above compiles and runs under all three of: standalone
  `tur check`, `tur run` of an importing consumer, and `tur build .`.
- A loaded path is spliced **once** even when both the entry and an imported
  module load it (no duplicate-symbol C errors).
- `bash tests/run.sh` stays green; add a fixture with a defmodule that
  `(load ...)`s a bare-defn stdlib file and calls a loaded name.

## Impact

With both this and
[captureless-closure-not-boxed-at-fat-ptr-void-boundary.md](captureless-closure-not-boxed-at-fat-ptr-void-boundary.md)
open, `tur-signal` Phase 5 cannot replace its interim `__chain-loop` fold with
`>>>`. See `docs/upcoming/tur-signal-rebuild-plan.md` Phase 5.
