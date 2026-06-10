---
title: Typeclasses have no associated type members
category: Expressiveness hole
severity: Forces a macro-driven parallel registry for any "class chooses an output type" pattern (ECS Component->Storage, Iterable->Iter, Collection->Elem). Workable, but adds per-plan boilerplate and loses elaborator-level coherence on the projection.
description: `definstance` supports value members but no associated *type* members. Classes that want to project from an instance type to another type (Haskell `type Storage c = ...`, Rust `type Item;` on `Iterator`) cannot be expressed; every such pattern has to be flattened into a second class or encoded by macro-generated wrappers. ECS v1 absorbs this with a `defcomponent`-emitted registry; the swap to a real associated type is a transparent surface-API improvement (E2d).
---

# Typeclass associated type members missing

## Summary

`docs/guides/typeclass-internals-guide.md` documents coherent,
module-scoped dispatch with value members. There is no surface syntax
for an **associated type member** -- the Haskell `type Storage c = ...`
or Rust `type Item;` pattern, where the class projects the instance
type to another type that the elaborator then uses in callers' types.

## Where this bites

- `docs/upcoming/v1/ecs-spice-plan.md` lines 64-69 define `Component`
  with an associated `Storage` so `defcomponent Pos :storage :dense`
  ties `Pos` to a `DenseStorage Pos` and `get-Pos`/`set-Pos`/`iter-Pos`
  type-check against that storage. Without associated types this has
  to be re-encoded as either:
  - A parallel registry resolved by macros (loses static checking on
    `Storage Pos` in user-written code), or
  - A second class `HasStorage T S` with `S` as a class-level type
    parameter and a functional dependency (we have neither functional
    deps nor multi-param classes documented as shipping today), or
  - Name-mangled wrappers (`Pos__storage`) referenced by string in
    macros.
- Any iterator / collection / monad-transformer plan that wants the
  return type of one method to depend on the instance type runs into
  the same wall.

## Observed vs. expected

**Observed.** `definstance Component Pos { ... }` declares value
members only. There is no `type Storage = DenseStorage Pos` line that
the elaborator picks up.

**Expected.**
```turmeric
defclass Component T
  type Storage : Type
  defn make-storage [] : Storage
  ...

definstance Component Pos
  type Storage = DenseStorage Pos
  defn make-storage [] (dense-storage-new)
```
...and elsewhere `(Storage Pos)` resolves at the type level to
`DenseStorage Pos`.

## Root cause

The elaborator's class representation stores method signatures, not
type-level projections. Adding associated types means:
- A type-level dispatch mechanism (`(Storage Pos)` is a type-level
  expression resolved by instance lookup),
- Coherence under reduction (two equivalent paths to `(Storage Pos)`
  must produce the same type),
- A surface syntax in `defclass` / `definstance`.

The HKT plan archive flagged "associated types / type families" as
post-HKT work; nobody has picked it up.

## Proposed directions

1. **Land associated types as a small, focused milestone.** The
   no-arithmetic, no-functional-dep, single-output-type version is a
   reasonable scope: one `type Foo = T` per class, looked up by the
   same dictionary the value members use.
2. **In the interim, document the macro-registry pattern as the
   official workaround** for the plans that want this (ECS in
   particular), so each plan does not invent its own.
3. **Decide whether multi-param classes / functional deps are coming.**
   Some of the use cases (Component -> Storage) admit a multi-param
   class with a fundep as an alternative encoding. Without either, the
   pattern has no clean Turmeric expression.

## Validation of a fix

- `defclass Container T { type Elem : Type; ... }` plus instances for
  `Vec Int` (Elem = Int) and `Vec Str` (Elem = Str) type-check, and
  `(Elem (Vec Int))` reduces to `Int` in a type annotation.
- An ECS fixture that calls `(iter-storage (storage-of Pos))` checks
  against `DenseStorage Pos` at compile time, with a wrong-storage
  mismatch reported on the `definstance`, not at the use site.

## Related

- `docs/guides/typeclass-internals-guide.md`
- `docs/guides/hkt-guide.md`
- `docs/upcoming/v1/ecs-spice-plan.md` lines 64-79, D2 (lines 337-355)
