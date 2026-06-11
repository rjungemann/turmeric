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
;; caught cheaply at runtime, not as use-after-free memory corruption.
(defopaque Entity :int)
```

Internally an `Entity` packs `index : u32` + `generation : u32`. We
expose only `entity=?`, `entity-alive?`, and constructors via the
`World` API. **Dead-entity reads return `(none)`; this is the only API
surface in v1** -- a "strict-aliveness" surface that turns
use-after-despawn into a *type* error would require refinement types,
which are not shipping (see
[docs/reported/refinement-types-not-implemented.md](../../reported/refinement-types-not-implemented.md)).
The `option`-returning forgiving surface is what every shipping ECS in
every language exposes anyway; we are not giving anything up in
practice.

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

Two recovery paths for systems that should be polymorphic over any
world containing a given component set:

**(a) Monomorphic systems against a concrete world (v1 default).** Every
`defsystem` names its concrete world type. For a single-game project
this is fine -- the world is declared once and reused. The raylib demo
in E3 uses this path.

**(b) Typeclass-bounded systems (v1 optional, lands with E2).** Each
component generates a `HasComponent` class (`HasPos`, `HasVel`, ...).
`defworld` emits the corresponding instances automatically. A system
that wants polymorphism states it via class constraints:

```turmeric
defn integrate [W] [(HasPos W) (HasVel W)] [w : W dt : float] :void
  ...
```

This is the apecs `Has w c` encoding, lifted into Turmeric's coherent
class system. We *don't* need refinement types for this -- it falls out
of the typeclass machinery we already have. The only thing we lose
relative to a refinement-typed `/has` is that the class dispatch is
resolved at call sites by dictionary lookup rather than by literal
field-access elaboration; that is the same trade-off Haskell makes and
the runtime cost is one indirection per call.

Refinement-typed world bounds (`/has Pos /has Vel` as a predicate on
the world type) remain a v2 ambition; see
[docs/reported/refinement-types-not-implemented.md](../../reported/refinement-types-not-implemented.md).

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

These are the v1 wins -- claims that hold against the type-system
machinery actually shipping today (no refinement types required).

1. **Closed world type.** `defworld` produces a single named struct.
   "Does this world have `Hp`?" is field lookup, not constraint
   solving. Adding a component is an explicit change to the world
   declaration; you cannot accidentally add one via instance import.
   *Querying a component the world doesn't declare is an
   unbound-name error from the elaborator -- compile-time membership
   checking without needing refinements.*
2. **Coherent typeclass dispatch.** `Component T` has exactly one
   instance per `T`. No orphan-instance footguns; storage choice is a
   property of the component, not the import graph.
3. **Static read/write effects.** `:reads`/`:writes` become first-class
   substructural capabilities (we have `-Xsubstructural` shipping
   today). apecs's `cmap` does not have this; it trusts the programmer.
   The scheduler proves non-conflict statically and the elaborator only
   exposes `set-X!` for `X` in `:writes`. **This is the single biggest
   delta vs. Haskell ECSes and it needs zero new type-system work.**
4. **Generational entity safety.** Use-after-despawn cannot corrupt
   memory: a stale handle's generation counter mismatches the slot's
   current generation, so `get-Pos` returns `(none)`. This is a
   *runtime* check (one u32 compare per access), not a type error --
   but every shipping ECS in every language does it this way. The
   compile-time version (refinement-typed alive-set) is a v2 ambition,
   not a v1 advantage.
5. **No runtime `TypeRep` lookup.** aztecs's heterogeneous map is fast
   in practice but ultimately a hash-by-typerep. Our equivalent is a
   struct field access resolved by the elaborator -- zero overhead, and
   a typo on a component name fails to compile rather than returning
   `Nothing`.
6. **Typeclass-bounded polymorphism, coherently dispatched.** Systems
   that want to be world-polymorphic state it via `(HasPos W)
   (HasVel W)` constraints (encoding (b) under "Concrete shape > World"
   above). Closer to Rust trait bounds than Haskell free-form
   constraints; checked at the call site, not by global instance search.

The cost: less ad-hoc polymorphism than Haskell, and no compile-time
"this handle is alive" / "this world has component X" *refinement*
guarantees. We get the latter via struct-field membership (which is
arguably stronger -- it's structural, not predicate-based) and accept
the former as runtime-checked.

### Deferred to v2 (gated on refinement types)

These were originally pitched as v1 advantages but require type-system
features that are not shipping. See
[docs/reported/refinement-types-not-implemented.md](../../reported/refinement-types-not-implemented.md).

- **Refinement-typed entities** (`entity-alive!` strict API). The
  forgiving `option`-returning API is the only v1 surface.
- **Refinement-typed world bounds** (`/has Pos` as a predicate on `W`).
  Use the typeclass encoding in v1.
- **Statically-rectangular sized iteration.** `SizedVec<n, T>` is
  shipping but the size index is currently phantom (see
  [docs/reported/sized-types-phantom-index.md](../../reported/sized-types-phantom-index.md));
  dense-storage zip checks length at runtime in v1. Lifts to compile
  time when SZ6 lands -- no plan-level rewrite needed at that point.

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

The phasing below is split into a **v1 track** (everything that lands
against the type system as it ships today) and a **v2 track** (gated on
elaborator prerequisites). The split lets the v1 track ship the raylib
demo and the comparison writeup *without* waiting on variadic HKT rows
or refinement types.

### v1 track -- ships against today's type system

**E0 -- skeleton (1-2 days):** `defcomponent`, `defworld`, dense
storage only, manual `spawn`/`get`/`set`, no queries yet. Smoke test:
spawn 1000 entities, mutate `Pos` in a `for`. Goal: prove the
elaborator integration and the macro-driven world-registry pattern.

**E1' -- fixed-arity queries (3-4 days):** `(query1 [...])` ...
`(query5 [...])` macro family (arity capped at 5), both imperative
`for-each` and functional `run-query!`. Sparse + tag storages.
`with`/`without` filters. **No kind-level row type required** -- each
arity is a separately-generated macro that elaborates to tuple-typed
iterators. Documented as the v1 surface; the variadic HKT replacement
is a drop-in API change later (see E1).

**E2 -- systems and scheduler (3-4 days):** `defsystem` with
`:reads`/`:writes`, sequential scheduler first, then a parallel
scheduler that proves non-conflict and runs disjoint systems on the
fiber pool (`stdlib/fiber.tur`). Substructural capabilities back the
write access -- this is the load-bearing v1 win and it works today.
Auto-generated `HasComponent` classes (the typeclass-bounded
polymorphism path) land here.

**E3 -- raylib companion (2-3 days):** `tur-ecs-raylib` with standard
components, `integrate-2d`, `render-sprites`, `with-game-loop`. Ship a
small demo (`asteroids.tur` or similar) and a `docs/guides/ecs-guide.md`.
Aliveness is `option`-returning throughout; demo uses
typeclass-bounded systems where it makes sense and monomorphic systems
where it doesn't.

**E4 -- comparison writeup:** a guide,
`docs/guides/ecs-vs-haskell-ecs.md`, that walks a small game in
apecs/aztecs/tur-ecs side by side and tabulates what moved from
runtime to compile time. The v1 column is honest about what's still
runtime-checked (aliveness, query joins past arity 5, dense-storage
length matching pre-SZ6) and what is *not* (component membership,
read/write conflict detection, storage choice).

### v2 track -- gated on elaborator prerequisites

**E1 -- variadic queries (gated by D1).** Replace the arity-N macro
family with a single `(query [...])` macro elaborating to row-typed
iterators. **Hard prereq: variadic HKT rows** (see
[docs/reported/variadic-hkt-rows-missing.md](../../reported/variadic-hkt-rows-missing.md)).
The v1 `queryN` API is a deprecation alias once E1 lands.

**E2b -- refinement-typed APIs (gated by RT0+).** Add the `entity-alive!`
strict-aliveness surface (lifts the runtime check out of the inner loop
when the elaborator can prove it) and the `/has Pos` refinement on
world bounds (lets a polymorphic system state its world requirements
without the `HasPos` class dictionary). Both are pure surface
additions over the v1 substrate; nothing in v1 has to change to
accommodate them. See
[docs/reported/refinement-types-not-implemented.md](../../reported/refinement-types-not-implemented.md).

**E2c -- sized-rectangular dense iteration (gated by SZ6).** Dense-vs-dense
zip becomes statically rectangular. Until SZ6 the runtime length check
stays. See
[docs/reported/sized-types-phantom-index.md](../../reported/sized-types-phantom-index.md).

**E2d -- associated-type storage projection (gated by associated types).**
Replace the parallel macro-registry for storage selection with a real
`type Storage : Type` member on `Component`. See
[docs/reported/typeclass-associated-types-missing.md](../../reported/typeclass-associated-types-missing.md).
v1's macro-registry stays callable; v2 swaps the elaboration path.

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

**Implication: D1 is a hard prerequisite for the variadic E1, not for
shipping queries at all.** The v1 track ships an arity-capped
`queryN` macro family (E1') against today's HKT machinery; the
variadic single-`query` form lands as E1 once D1 does. The arity cap
is acknowledged as the retreat-to-runtime move we wanted to avoid --
we are taking it explicitly and time-boxed, with a documented
migration path, rather than indefinitely.

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
