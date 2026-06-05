---
title: (load ...) is not idempotent -- double-loading a typeclass module miscompiles
category: Reported
description: Loading the same stdlib module twice (directly, or once directly and once transitively) re-emits its definstance method functions and dictionary structs, producing C "redefinition of __inst_*" / "redefinition of struct dict_*" errors. This makes it unsafe to add (load "stdlib/typeclass.tur") to a module that is itself loaded alongside another module that already pulls typeclass.tur in, which blocked adding Show/Ord instances for Bound/Range in the range GADT migration.
---

# `(load ...)` is not idempotent -- double-load miscompiles typeclass modules

> **RESOLVED (2026-06-05).** Fixed at the root: `definstance` is now idempotent.
> `elab_definstance` (`src/compiler/elab_typeclasses.c`) computes the instance's
> codegen type-arg suffix once up front (factored into the new
> `build_inst_type_suffix` helper, shared with the `__inst_*` method-name
> builder so the dedup key can never drift from the emitted names) and, before
> registering/elaborating, scans the instance env for an already-registered
> instance of the same `(typeclass, suffix)`. On a match the redundant
> definition is a silent no-op (first definition wins) -- no second dictionary
> struct/singleton or `__inst_*` function is emitted, so the ODR collision is
> gone. This fixes both the direct double-`load` and the
> partial-stub-plus-full-`typeclass.tur` case, since the duplicate is caught by
> the emitted symbol it would collide on rather than by file path. Regression
> fixture: `tests/fixtures/load-typeclass-idempotent`. Full suite green
> (1483 passed, 0 failed).
>
> Note: the *actual* collision in the minimal repro is even tighter than the
> original diagnosis -- a **single** `(load "stdlib/typeclass.tur")` already
> collides with the auto-loaded `typeclass-clone.tur` stub (both define
> `Clone [int]`), so a path-keyed load guard (proposed direction 1) alone would
> NOT have fixed it. The instance-level dedup (direction 2) is the correct root
> cause. The deferred `Show`/`Ord [Bound]` instances can now be added to
> `range.tur` to close range-gadt phase B2.

> **Severity:** hard compile error (cc fails) on a benign-looking double
> `load`; also an expressiveness/ergonomics blocker -- it prevents a module
> from declaring a `load` dependency on `typeclass.tur` if any co-loaded
> module already pulls it in transitively.
> **Found:** 2026-06-04, executing
> [range-gadt-typeclass-migration-plan](../archive/history/range-gadt-typeclass-migration-plan.md)
> phase B2 (Show/Ord instances for `Bound`/`Range`).
> **Status:** RESOLVED (2026-06-05).

## Summary

`load` does not de-duplicate by path. Loading a module that contains
`definstance` forms twice re-emits that instance's method function and
dictionary struct, so the generated C has two `static` definitions of the
same symbol and `cc` rejects it:

```
error: redefinition of '__inst_Clone_clone_int'
error: redefinition of 'struct dict_Clone_int'
```

## Minimal repro

```turmeric
(load "stdlib/typeclass.tur")
(load "stdlib/typeclass.tur")
(defn main [] : int (println (show 42)) 0)
```

```sh
tur run repro.tur
```

The second `load` re-emits every primitive `definstance` (Clone, Eq, Show,
Num, Ord, ...), each colliding with the first.

## Observed vs. expected

- **Observed:** `cc invocation failed` with `redefinition of '__inst_*'` /
  `redefinition of 'struct dict_*'`.
- **Expected:** a repeated `load` of an already-loaded path is a no-op (the
  usual include-guard / "loaded set" semantics), so double-loading is
  harmless.

## Why it matters beyond the obvious repro

The failure also fires when a module is loaded *once directly and once
transitively*. Concretely, during the range GADT migration:

- `range.tur` transitively pulls in the partial typeclass modules
  (`typeclass-eq.tur`, `typeclass-clone.tur`, ...) that are auto-loaded into
  every program, but **not** the full `stdlib/typeclass.tur` (so `show` / the
  `Show` and `Ord` *classes* are absent after `(load "stdlib/range.tur")`).
- To add `(definstance Show [Bound] ...)` / `(definstance Ord [Bound] ...)`
  one needs the `Show` / `Ord` classes, which only `typeclass.tur` defines.
- Adding `(load "stdlib/typeclass.tur")` to `range.tur` or `range-bound.tur`
  re-defines `Clone`/`Eq`/... already present via the partial modules ->
  redefinition error. So the instances cannot be placed where the orphan-rule
  (`TUR-E0013`) requires them (the module defining `Bound`).

This is the direct reason B2 shipped `Eq [Bound]` (Eq is auto-loaded, no
extra `load` needed) plus self-contained `range->str` / `bound->str`
formatters, but **not** `Show`/`Ord` typeclass instances.

## Root cause (direction)

`load` lowers to re-elaborating the target file's forms in the current
module. There is no "already loaded" set keyed by canonical path, so
`definstance` (and any other top-level form that emits a uniquely-named
`static` symbol) is emitted once per `load`. Typeclass modules are the most
visible victim because their `definstance` method/dictionary symbols are
globally unique by (class, type), so a second copy is a guaranteed ODR
collision rather than a silently-shadowing redefinition.

## Proposed fix directions

1. **Path-keyed load guard (preferred).** Track canonicalised loaded paths in
   the elaborator; a `load` of an already-loaded path becomes a no-op. This
   matches the mental model and fixes both the direct and transitive cases.
2. **De-duplicate emitted instance symbols.** Before codegen, drop duplicate
   `definstance` emissions keyed by (class, type-args). Narrower; only covers
   typeclasses, not other top-level `static` symbols.

(1) is the clean root-cause fix and would also let `typeclass.tur` be loaded
defensively from any module.

## Validation

After a fix, the minimal repro should run and print `42`, and a module that
both transitively pulls a partial typeclass module and explicitly
`(load "stdlib/typeclass.tur")` should compile. At that point the deferred
`Show`/`Ord [Bound]` instances (and a nominal-`Range` `Show`) can be added to
`range.tur` to fully close B2.
