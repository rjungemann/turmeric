---
title: Turmeric ECS Spice Plan
category: Planning
description: An Entity-Component-System library for Turmeric, inspired by Haskell's `aztecs` and `apecs`, paired with `tur-raylib`, designed to push compile-time type safety further than Haskell's holistic constraint-based approach.
---

# `tur-ecs` -- Plan

## Goal

Build an ECS (Entity-Component-System) spice that lives at
`../turmeric-spices/spices/ecs/` and pairs cleanly with `tur-raylib` for
real-time games and simulations. The core idea: **take aztecs's structural
clarity and apecs's ergonomic queries, but lean on Turmeric's
type-system features (HKTs, typeclasses with coherent dispatch,
substructural/refinement types, sized types) to push more invariants to
compile time** than Haskell's typeclass-soup approach can.

This is partly a library, partly an empirical comparison: how far can a
language with strict, dispatch-coherent typeclasses and refinement types
get with the ECS pattern before the compiler has to "give up" and start
trusting the programmer?

## Inspiration: aztecs vs. apecs vs. us

| Concern | apecs (Haskell) | aztecs (Haskell) | tur-ecs (proposed) |
|---|---|---|---|
| Component declaration | `instance Component Pos where type Storage Pos = Map Pos` | `Component` typeclass + `derive`d storages | `defcomponent` macro that lowers to a `definstance Component` + chosen storage |
| Storage selection | Type family `Storage c` | Associated type / `Storage c` | Associated *type member* on `Component` typeclass; default = dense `vec`, override per component |
| Query language | `cmap`, `cfold` over tuples `(Pos, Vel)` | Arrow-like `Query` ADT, runnable on a `World` | First-class `(query [Pos Vel])` macro that elaborates to a typed iterator with HKT'd row type |
| World | Monolithic `World` record type | Heterogeneous map keyed by `TypeRep` | Generated `World` struct (one storage field per registered component), known at compile time |
| Systems | `System w a` reader-monad style | Arrow / `runSystem` | Plain `defn`s over a `World` handle; scheduler is a separate module |
| Parallelism | Implicit / careful | `aztecs-stm` / scheduler awareness | Explicit "read-set / write-set" types on `defsystem`; scheduler can statically prove non-conflict |
| Error mode | Runtime: missing storage = crash | Runtime: missing component = `Nothing` | Compile-time: missing storage is unbound name; missing component is a type error |

Where Haskell uses one big `Has w c => System w c -> ...` constraint and
lets GHC unify, we instead **bake the component set into the world type**
and let elaboration check membership directly. This is what "pushing
holistic to compile-time" means here.

## Concrete shape

### Entities

```turmeric
;; Opaque handle. Generationally-versioned so dangling references are
;; a refinement violation, not a use-after-free.
(defopaque Entity :int)
```

Internally an `Entity` packs `index : u32` + `generation : u32`. We
expose only `entity=?`, `entity-alive?`, and constructors via the
`World` API. Dead-entity reads return `(none)` instead of stale rows.

### Components

```turmeric
#lang sweet-exp

defstruct Pos [x : float y : float]
defstruct Vel [x : float y : float]
defstruct Hp  [cur : int  max : int]

;; defcomponent picks a storage and registers the component name into
;; a compile-time component registry consulted by defworld.
defcomponent Pos :storage :dense
defcomponent Vel :storage :dense
defcomponent Hp  :storage :sparse        ;; few entities have HP
```

`defcomponent` lowers to:

1. A `definstance Component <T>` that ties `T` to its storage's
   associated type member.
2. A registration hook so `defworld` can see `T` was declared.

Storage backends in v1: `:dense` (parallel vector keyed by entity
index), `:sparse` (HAMT from `Entity` -> `T`), `:tag` (zero-payload
"has component X" set, useful for `Dead`, `Frozen`, etc.).

### World

```turmeric
defworld GameWorld
  [Pos Vel Hp Sprite Player Enemy]
```

`defworld` is a macro that:

- Generates a `defstruct GameWorld` with one field per component
  (typed to that component's storage).
- Generates `world-new`, `spawn`, `despawn`, and per-component
  `get-Pos`, `set-Pos`, `remove-Pos`, ... accessors. (These are not the
  user-facing API; queries are -- but they exist so queries elaborate
  to plain field access.)
- Records the component set on the type so the elaborator can reject
  a `(query [Foo])` if `Foo` was not listed.

Two worlds with overlapping but distinct component sets are
*different types*. This is the central typing trick: we sacrifice
"polymorphic system over any world that has `Pos`" (which is what
apecs's `Has w c` constraint buys) in exchange for "the world is a
named, closed type and the compiler knows every cell of it."

We can recover some polymorphism by stating constraints structurally:

```turmeric
defn integrate [w :W] [W /has Pos /has Vel] :void
  ...
```

`/has` is a refinement on the world type that elaborates to a
"contains this component" predicate -- statically resolved against the
registry on `W`. This is closer to Rust's trait bounds than to Haskell's
free-form constraints: every `/has` is checked when `integrate` is
*called*, not solved at the top level by a global instance search.

### Queries

The ergonomic centerpiece. Two surfaces:

**Imperative** (apecs `cmap` style):

```turmeric
for-each [(p :Pos) (v :Vel)] world
  fn [e p v]
    set-Pos! world e
      Pos {p.x + {v.x * dt}}
          {p.y + {v.y * dt}}
```

**Functional** (aztecs `Query` style, arrows-flavored):

```turmeric
def integrate-q
  query
    in  [Pos Vel]
    out [Pos]
    fn [p v]
      Pos {p.x + {v.x * dt}}
          {p.y + {v.y * dt}}

run-query! integrate-q world
```

Both lower to the same primitive: a typed iterator over the
intersection of the relevant storages. `in` and `out` are declared
because they feed system scheduling (below).

A query's row type is an HKT'd tuple -- this is where Turmeric's
existing HKT machinery (see `docs/guides/hkt-guide.md`) earns its
keep. `Query in out` is a functor over `out`, an applicative when
joining queries, and (per the comonad story) extendable via focus on a
single entity.

### Filters

```turmeric
query
  in   [Pos]
  with [Player]            ;; tag filter: only entities with Player
  without [Dead]           ;; tag filter: exclude Dead
  fn [p] ...
```

`with`/`without` are not in the row -- they constrain the iterator,
not its yielded tuple. Statically, both must be registered components
on the world.

### Systems and scheduling

A `defsystem` is a `defn` plus declared read/write sets:

```turmeric
defsystem physics [w :GameWorld dt :float] :void
  :reads  [Pos Vel]
  :writes [Pos]
  ...

defsystem render [w :GameWorld] :void
  :reads  [Pos Sprite]
  :writes []
  ...
```

The scheduler (`stage`) takes a list of systems and a world and runs
them in dependency order. **Two systems with disjoint write sets and
no write/read overlap can run in parallel**, and the scheduler can
*prove* that statically from the declared sets -- no runtime conflict
detection, no STM. Mis-declaring (writing to a component you didn't
list) is a compile-time error because the elaborator only exposes
`set-X!` capabilities for `X` in `:writes`.

This is the substructural-types angle: `:reads` and `:writes` define
linear capabilities the system body is allowed to use. See
`docs/guides/substructural-types-guide.md`.

### Raylib integration

`tur-ecs-raylib` is a thin companion spice that provides:

- Standard components: `Pos2 {x y :float}`, `Vel2`, `Rot`, `Sprite
  {tex :Texture w h :float}`, `Color`, `Collider2`.
- Standard systems: `integrate-2d`, `render-sprites`, `tick-input`.
- A `with-game-loop` macro:

```turmeric
with-game-loop world :title "Demo" :size [800 600] :fps 60
  stage
    tick-input
    physics
    render-sprites
```

`with-game-loop` wraps `init-window` / `window-should-close` /
`begin-drawing` / `end-drawing` / `close-window`, and the body runs the
scheduler each frame.

## Where Turmeric's types pull ahead

1. **Closed world type.** `defworld` produces a single named struct.
   "Does this world have `Hp`?" is field lookup, not constraint
   solving. Adding a component is an explicit change to the world
   declaration; you cannot accidentally add one via instance import.
2. **Coherent typeclass dispatch.** `Component T` has exactly one
   instance per `T`. No orphan-instance footguns; storage choice is a
   property of the component, not the import graph.
3. **Static read/write effects.** `:reads`/`:writes` become first-class
   substructural capabilities. apecs's `cmap` does not have this; it
   trusts the programmer.
4. **Refinement-typed entities.** Generational `Entity` + alive-set
   refinements turn "use-after-despawn" into a type error in the
   strict API (`entity-alive!`) and a `(none)` result in the
   forgiving API (`get-Pos`).
5. **Sized-types-backed dense storage.** A dense storage of length `n`
   yields a `Vec<n, T>`; zip of two dense storages over a shared
   `n` is statically rectangular. See
   `docs/guides/sized-types-guide.md`. (Resizes punch out of the
   sized world via existentials; that is fine -- the per-frame inner
   loop doesn't resize.)
6. **No runtime `TypeRep` lookup.** aztecs's heterogeneous map is fast
   in practice but ultimately a hash-by-typerep. Our equivalent is a
   struct field access resolved by the elaborator.

The cost: less ad-hoc polymorphism. You cannot write a system that
"works for any world containing `Pos`" without saying so via `/has`,
and there is no global "instance Component (Maybe T)" sleight of
hand. We think that is a fair trade for a game engine -- the world is
known and finite.

## Out of scope (v1)

- **Reactive / arrow-style systems** as the *only* surface. We provide
  the functional `Query` form because it composes, but the imperative
  `for-each` is first-class. No FRP layer in v1.
- **Hierarchies / parent-child transforms.** Add as a follow-up spice
  (`tur-ecs-scene`) once raylib usage shows what shape is needed.
- **Serialization / save-state.** Likely a follow-up using the
  `application-image-dumps` work.
- **STM-style optimistic concurrency.** The static scheduler is the
  story; if it does not suffice, we will revisit.

## Phasing

**E0 -- skeleton (1-2 days):** `defcomponent`, `defworld`, dense
storage only, manual `spawn`/`get`/`set`, no queries yet. Smoke test:
spawn 1000 entities, mutate `Pos` in a `for`. Goal: prove the
elaborator integration.

**E1 -- queries (3-4 days, *gated by D1*):** `(query [...])` macro,
both imperative `for-each` and functional `run-query!`. Sparse + tag
storages. `with`/`without` filters. Fixture suite under
`tests/fixtures/ecs-*/`. **Cannot start until variadic HKT rows
(D1) land in the elaborator** -- if that work slips, E0 is the only
publishable milestone in the interim.

**E2 -- systems and scheduler (3-4 days):** `defsystem` with
`:reads`/`:writes`, sequential scheduler first, then a parallel
scheduler that proves non-conflict and runs disjoint systems on the
fiber pool (`stdlib/fiber.tur`). This is also the right point to wire
substructural capabilities for write access.

**E3 -- raylib companion (2-3 days):** `tur-ecs-raylib` with standard
components, `integrate-2d`, `render-sprites`, `with-game-loop`. Ship a
small demo (`asteroids.tur` or similar) and a `docs/guides/ecs-guide.md`.

**E4 -- comparison writeup:** a guide,
`docs/guides/ecs-vs-haskell-ecs.md`, that walks a small game in
apecs/aztecs/tur-ecs side by side and tabulates what moved from
runtime to compile time.

## Validation

- Fixture tests under `tests/fixtures/ecs-*/` covering:
  - declaring world with components, spawn/despawn, generational
    safety,
  - queries (presence, intersection, with/without filters),
  - system scheduling order and parallel non-conflict,
  - rejection cases: query over non-registered component, system
    writing outside its `:writes` set, double-despawn.
- A `tur-ecs-raylib` demo that does something visible at 60 FPS with a
  few thousand entities, runnable via `tur run` from the spice
  directory.
- Bench: dense `Pos`/`Vel` integration loop, 100k entities, compared
  against a hand-rolled equivalent. Target: within 2x of hand-rolled.

## Resolved design decisions

These were the v1 open questions; all four are now committed. Precedent
comparison notes live in [docs/guides/ecs-vs-haskell-ecs.md](../guides/ecs-vs-haskell-ecs.md)
once that lands in E4.

### D1 -- Query rows use true variadic HKT

`Query in out` is parameterised by *type-level cons-lists of types*, not by
fixed-arity tuples. This requires extending the elaborator to support
type-level lists at kind level (a `Row :: List Type` kind) and a `Row`
HKT that queries are functorial over.

Precedent rejected: macro-generated tuple arities 1..N (Bevy/Specs) and
arrow composition with nested pairs (aztecs). Both work; both would have
shipped sooner. We are paying the implementation cost now because:

- Variadic rows generalise to records, relations, and the future
  data-frame work in `stats-formula-plan` -- not paying once means
  paying repeatedly.
- The whole pitch of the project is "more invariants at the type level
  than Haskell." Capping query arity at 5 via macro is exactly the
  retreat-to-runtime move we are trying to avoid.

**Implication: D1 is a hard prerequisite for E1.** E0 (skeleton, no
queries) can ship against the existing HKT machinery; queries cannot.
The phasing below is updated accordingly.

### D2 -- Per-component storage, small Storage trait

`Storage` typeclass exposes `insert`, `remove`, `get`, `iter`, plus an
associated type `Elem`. Three v1 backends: `:dense` (vec), `:sparse`
(HAMT), `:tag` (bitset). Component picks its storage at `defcomponent`.

Precedent rejected:
- Archetype storage (Flecs, modern Bevy). Faster iteration, but
  archetypes are themselves type-level sets of components -- so
  archetype storage and variadic HKT rows interact in nontrivial
  ways. We do not want both elaborator projects in the same release.
  Designing the `Storage` trait so an archetype backend *could* be
  swapped in later (E5+) without breaking the user-facing API is a
  soft goal; we will not contort v1 to guarantee it.
- Single sparse-set (EnTT). Removes the "storage choice is visible in
  the type" lever, which is a feature we want to keep.

`iter` for `:dense` returns a sized iterator (`Vec<n, (Entity, T)>`);
for `:sparse` and `:tag`, an unsized iterator. Sized witnesses
propagate through query joins where both sides are dense.

### D3 -- Tags are an explicit storage choice

`defcomponent Player :storage :tag` lowers to a bitset over entity
index space. A zero-field struct without `:storage :tag` still gets
dense storage -- no auto-detection, no silent perf cliffs from adding
a field. Tags participate in the same `Storage` trait as everything
else; `iter` on a tag yields `Entity` only (the associated `Elem` is
the unit-like singleton, optimised away).

Precedent rejected:
- EnTT-style auto-detection. Too magical; adding one field to a tag
  silently moves it from O(1) bit to O(n) words. We might add a lint
  in a later phase.
- Flecs-style `deftag` as a separate kind. Cleaner semantically but
  doubles the registry surface. We can revisit if real usage shows
  that tag-vs-component confusion is a footgun in practice.

### D4 -- Single-world v1

A `defsystem` reads/writes one world. Multiple worlds in one process
are values and may coexist; cross-world systems are not a thing in v1.

Precedent rejected, deferred to a post-v1 follow-up plan:
- World-Mirror primitive for render-extract / client-prediction
  patterns. Real use cases exist (Bevy SubApp, Unity DOTS) but none of
  them are blockers for the raylib demo. We will revisit when the
  first concrete user shows up.
- Full cross-world `:reads-from`/`:writes-to`. Significant elaborator
  work; not justified until D4-revisit happens.

## References

- aztecs: <https://hackage-content.haskell.org/package/aztecs-0.17.1/docs/Aztecs-ECS.html>
- apecs: <https://github.com/jonascarpay/apecs>
- `docs/guides/hkt-guide.md`
- `docs/guides/substructural-types-guide.md`
- `docs/guides/sized-types-guide.md`
- `../turmeric-spices/spices/raylib/`
