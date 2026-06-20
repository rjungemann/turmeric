---
title: ECS Refinement-Typed APIs Plan
category: Planning
description: The two refinement-type-gated ECS surfaces (strict-aliveness entities and `/has` world bounds) split out of the now-archived `ecs-spice-plan.md` so it can land standalone once refinement types ship.
---

# ECS Refinement-Typed APIs -- Plan

> **Status 2026-06-20.** Split out of the shipped-and-archived
> [`ecs-spice-plan`](../../archive/ecs-spice-plan.md) (originally tracked
> there as **E2b**). All ECS substrate work (E0-E4, E2c sized-rectangular
> iteration, E2d associated-type storage) shipped 2026-06-11 through
> 2026-06-17. This plan covers the **only** ECS surface that remained
> deferred: the two refinement-typed APIs that need the refinement-types
> elaborator to land first.
>
> Gating: [`refinement-types-plan.md`](refinement-types-plan.md). Until
> RT0-RT6 ship, this plan is dormant -- nothing here is unblocked by
> further spice-side work.

## Goal

Add two compile-time-checked surfaces to `tur-ecs` that the v1 spice
deliberately left as runtime-checked `option` returns, because the
type-system machinery to express them was not yet shipping:

1. **Refinement-typed entities** (`entity-alive!` strict-aliveness API)
   -- a handle whose type carries `/alive` so accessor calls do not need
   to return `option<T>`; use-after-despawn becomes a *type* error, not a
   runtime `(none)`.
2. **Refinement-typed world bounds** (`/has Pos /has Vel` predicates on
   the world type parameter) -- the third polymorphism encoding for
   systems, alongside (a) monomorphic-against-a-concrete-world and (b)
   typeclass-bounded (`(HasPos W) (HasVel W)`).

Both are **pure surface additions** over the substrate that already
shipped. Nothing in the current `ecs` or `ecs-raylib` spice has to
change to accommodate them when they do land; existing call sites stay
correct.

## Why this is split out

The parent ECS plan landed everything that did *not* depend on
refinement types. Carrying the two deferred items inline kept the parent
in the "in-flight" bucket indefinitely even though every shipping phase
was green. Splitting lets the parent move to the archive (it is done as
scoped) and gives the refinement-typed APIs a separate plan that can be
sequenced against [`refinement-types-plan.md`](refinement-types-plan.md)
directly.

## Prerequisites

- **Refinement types in the elaborator.** Specifically the subset called
  out in [`refinement-types-plan.md`](refinement-types-plan.md):
  - Quantifier-free linear-arithmetic predicates on integer
    parameters / locals (covers the generational `alive?` predicate when
    spelled as `gen == slot.gen`).
  - Predicate refinement on struct fields and opaque newtypes (so
    `(refine Entity (/alive w))` is a real type a function can return).
  - SMT discharge via libz3 with `-Xrefinements` opt-in. The ECS uses
    will fit comfortably inside the quantifier-free fragment.
- **Either** a `(refine T P)` type former usable in user code, **or**
  the equivalent `T | P` surface the refinement plan settles on. This
  plan is written assuming `(refine ...)`; trivial syntactic substitution
  if RT lands a different spelling.

No spice-side prerequisites remain. Substructural caps, associated-type
storage, sized worlds, and the variadic `defworld` / `for-each` /
`sized-defworld` machinery are all in. The refinement-typed surfaces
slot in next to them.

## Phasing

### RE0 -- Refinement-typed `Entity` (strict-aliveness API)

Add a `(refine Entity (/alive w))` type and a parallel accessor family:

- `entity-alive!` -- promotes `Entity` to `(refine Entity (/alive w))`
  inside a tested branch; outside the branch, the un-refined `Entity`
  is the only type available.
- `get-Pos-alive` / `set-Pos-alive!` / `has-Pos-alive?` -- take the
  refined entity, return `T` (not `option<T>`) for reads, no-`option`
  writes. The cap surface stays identical -- `WriteCap<Pos>` /
  `ReadCap<Pos>` are orthogonal to aliveness.
- Sized analogues: `sized-entity-alive!`,
  `sized-get-Pos-alive` / `sized-set-Pos-alive!`, etc.

Existing `get-Pos` / `set-Pos!` keep their `option`-returning shapes;
they remain the default surface. The `-alive` family is opt-in and
costs the user one `if-alive` test up front in exchange for losing the
`option` unwrap at every accessor call.

**Acceptance:** a fixture under
`../turmeric-spices/spices/ecs/tests/` that:
- Spawns N entities, despawns half, runs an `if-alive` test, and
  reads `Pos` non-`option`ally inside the alive branch.
- A *negative* fixture under `tests/errors/` that calls
  `get-Pos-alive` on an un-refined `Entity` and fails to elaborate.

### RE1 -- Refinement-typed world bounds (`/has`)

Add `(refine W (/has Pos))` (and the AND-composition, `/has Pos /has
Vel`) as a predicate on the world type parameter.

Surface:
```turmeric
defn integrate [W] [w : (refine W (/has Pos /has Vel)) dt : float] : void
  ...
```

The predicate elaborates to a *structural* field-presence check on
`W`'s component set -- no dictionary, no runtime indirection (this is
the win over the typeclass-bounded encoding (b)). `defworld`
auto-emits the corresponding refinement witnesses for every component
in its declared set; the elaborator solves `/has Pos` for a world
declared with `Pos` by direct field lookup.

The typeclass-bounded encoding `(HasPos W)` stays available; pick the
encoding by trade-off:
- `(refine W (/has Pos))` -- structural, zero-overhead, but requires
  `-Xrefinements`.
- `(HasPos W)` -- dictionary-dispatched, one indirection per call, no
  refinements needed. v1 default.

**Acceptance:**
- Fixture: a `defn` whose world parameter is
  `(refine W (/has Pos))` and that compiles cleanly when invoked on a
  world declared with `Pos`, *without* needing a `HasComponent`
  instance.
- Negative fixture under `tests/errors/`: same `defn` invoked on a
  world that does not declare `Pos` fails to elaborate with a
  refinement-discharge error pointing at the `/has Pos` predicate.

### RE2 -- Documentation + comparison-table refresh

Refresh `docs/guides/ecs-guide.md` and
`docs/guides/ecs-vs-haskell-ecs.md` to add the refinement-typed
columns/rows. The comparison table's "polymorphism" row gains a third
encoding (c), and the "aliveness" row picks up a compile-time entry
alongside the runtime-checked default. Both guides exist; this is a
content edit, not a new file.

## Validation -- targets when shipping

- Spice-side regression tests under
  `../turmeric-spices/spices/ecs/tests/` covering both refined surfaces
  (unsized + sized variants).
- Negative cases under `tests/errors/` for the elaborator-rejection
  paths.
- The raylib demo at
  `../turmeric-spices/spices/ecs-raylib/` does **not** need to migrate;
  the refined APIs are opt-in. A small follow-up demo that exercises
  `entity-alive!` would prove the ergonomics.

## Out of scope

- Anything not gated on refinement types -- already shipped in the
  parent plan.
- Cross-world systems / `World-Mirror` -- tracked separately in
  [`ecs-cross-world-systems-plan.md`](ecs-cross-world-systems-plan.md).
- Routing `defcomponent-accessors` through `StorageOps` -- separate
  follow-up tied to struct-element projection, not refinement types.

## References

- Parent plan (archived): [`ecs-spice-plan`](../../archive/ecs-spice-plan.md)
- Prerequisite: [`refinement-types-plan`](refinement-types-plan.md)
- `docs/guides/ecs-guide.md`
- `docs/guides/ecs-vs-haskell-ecs.md`
- `docs/guides/substructural-types-guide.md`
- `docs/guides/sized-types-guide.md`
