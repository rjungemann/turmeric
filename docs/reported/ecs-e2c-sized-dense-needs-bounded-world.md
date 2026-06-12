---
title: ECS E2c (sized-rectangular dense iteration) needs a bounded-capacity world API, not a transparent spice-side update
category: Planning gap / scope mismatch
severity: Planning correction. SZ6-SZ8 shipped (2026-06-10) and the ECS spice plan claims E2c is "now a transparent spice-side update." It isn't: the dense storage's runtime-grown semantics are incompatible with cross-parameter size unification as currently shipped, so E2c requires a sized-world API redesign before any wiring can land.
description: The ECS spice plan asserts that E2c is unblocked by SZ6-SZ8. In practice, the shipped cross-parameter size unification only succeeds when the size index is derived from a GADT constructor chain (e.g. `SVCons int (SizedVec n) : (SizedVec (Add 1 n))`). Dense storage in `ecs/storage` is a malloc'd buffer that grows in place via `dense-set!`; the handle is an opaque `int` with no constructor chain, so any size index attached to it would be a true phantom -- never unifiable in any load-bearing way. Lifting `__fe-min-cap` to a static check requires a world-API redesign (bounded-capacity worlds with a single size parameter threaded through every storage), which is not a transparent spice-side update.
status: OPEN. Recommended next step: redesign the world API before queueing further spice-side work on E2c.
---

# E2c needs a bounded-capacity world API

## Summary

`docs/upcoming/ecs-spice-plan.md` lists E2c (sized-rectangular dense
iteration) as "spice-side wiring pending (prereq landed)" -- claiming
that sized types SZ6-SZ8 shipping on 2026-06-10 is enough, and that
the spice-side change is "transparent."

It is not. The shipped cross-parameter size unification
([`tests/fixtures/sized-cross-param-accept`](../../tests/fixtures/sized-cross-param-accept/input.tur))
works because `SizedVec` is a GADT whose constructors index `n`:

```turmeric
(defgadt SizedVec [n]
  (SVNil  : (SizedVec (Static 0)))
  (SVCons int (SizedVec n) : (SizedVec (Add (Static 1) n))))
```

Every concrete `SizedVec` value's size index is derivable from its
constructor chain; the elaborator unifies `n` across parameters
because each side has a statically-derivable witness.

ECS dense storage has no such structure. From
`../turmeric-spices/spices/ecs/src/ecs/storage.tur`:

```turmeric
(defn dense-new []                  : int  ... calloc ...)
(defn dense-set! [A] [s : int idx : int val : A] : nil ...)  ;; mutates in place
(defn dense-get  [A] [s : int idx : int]         : A   ...)
```

The handle is `:int` -- a heap pointer to a control block. `dense-set!`
grows the buffer via `realloc`. There is no constructor chain that
could carry a size index, and the storage's length is not knowable
until runtime.

## Where the plan overclaims

`docs/upcoming/ecs-spice-plan.md` lines 320-326:

> **Statically-rectangular sized iteration.** Sized-types SZ6-SZ8
> landed 2026-06-10 -- size indices participate in type equality and
> cross-parameter unification (see ...). Dense-storage zip still checks
> length at runtime; lifting to a compile-time check is now a
> transparent spice-side update.

And lines 421-427:

> **E2c -- sized-rectangular dense iteration.** Sized types SZ6-SZ8
> shipped 2026-06-10 ... Dense-vs-dense zip in the spice still does a
> runtime length check; lifting it to a `SizedVec<n, T>`
> static-rectangularity check is now a transparent spice-side update.

The unblocked-by-SZ6-SZ8 framing reads as if a small refactor to
`for-each`'s `__fe-min-cap` macro suffices. In fact the only paths
forward are:

1. **Reframe dense as a GADT chain.** `dense-set!` would return a new
   `Dense<n+1, A>` handle. Breaks mutation and every existing test;
   doesn't match ECS slot-indexed semantics. Not viable.
2. **Bounded-capacity world API.** The world commits to a capacity
   `n` at construction (`(defworld GameWorld n [Pos Vel])`); every
   storage field is typed `Dense<n, T>` with `n` as the single source
   of truth threaded through `spawn`, `despawn`, and every accessor.
   The `for-each` macro then unifies `n` across all listed components
   via the world's type. This is a real world-API redesign, not
   transparent wiring.
3. **Phantom-only size index.** Wrap the int handle in `Dense<n, A>`
   where `n` is purely cosmetic. Two storages "unify" because they
   have no constraints, so the loop bound is still the runtime cap.
   This adds type-noise without delivering rectangularity; it is
   worse than the status quo.

Option 2 is the only honest landing. It also opens design questions
the plan has not addressed:

- Is the cap fixed at world creation, or can it grow (with a
  re-binding of `n`)?
- How does this interact with sparse storage (which has no capacity
  notion) and tag storage (a bitset)?
- Does `spawn` need to be fallible when the entity count would exceed
  `n`?
- Does `defcomponent-accessors`' cap-gating story change when the
  storage is sized?

## Recommended directions

1. **Update `docs/upcoming/ecs-spice-plan.md` E2c entry** to reflect
   that E2c is gated on a sized-world API design, not on SZ6-SZ8 alone.
   Move it from "spice-side wiring pending (prereq landed)" to a new
   "design pending" subsection alongside E2b.
2. **Open a follow-up `ecs-sized-world-plan.md`** under
   `docs/upcoming/` capturing the design questions above before any
   spice-side code lands.
3. **Leave the runtime `__fe-min-cap` check in place** -- it is the
   correct fallback for unsized worlds and will continue to be the
   correct fallback for unsized components in sized worlds.

## Prereqs that would make Option 2 tractable

Before committing to the world-API redesign, several compiler-side
questions should be answered with focused fixtures. Some may already
be green; others are real gaps. Either way, getting them resolved
ahead of time keeps the spice-side work tightly scoped.

**P1 -- Does SZ8 cross-parameter unification fire on non-GADT
phantom indices?** The shipped reference fixture
([`tests/fixtures/sized-cross-param-accept`](../../tests/fixtures/sized-cross-param-accept/input.tur))
unifies `n` across parameters because `SizedVec` is a GADT whose
constructors index `n`. The dense-handle wrapper would be a
`defopaque` (or a regular `defstruct`) with `n` as a phantom
parameter -- no constructor chain to witness it. A new pair of
fixtures:

```turmeric
(defopaque Dense [n A] :int)
(defn zip [xs : (Dense n A) ys : (Dense n B)] : nil ...)
```

with one caller passing matching `n` and one passing mismatched
literals would tell us whether SZ8 already covers the opaque case.
If yes, P3+P4 are mostly stdlib work; if no, this is the central
elaborator gap to file before any spice work.

**P2 -- Does `defstruct` unify size variables across fields?**
The world struct would declare:

```turmeric
(defstruct World n
  [pos : (Dense n Pos)
   vel : (Dense n Vel)])
```

The load-bearing question is whether `(.pos w)` and `(.vel w)`
recover the same `n` through field access -- i.e. whether the
elaborator threads the struct's size parameter into each field's
type at projection time. A fixture that constructs a `World n`,
extracts both fields, and feeds them into a `zip` from P1 would
answer this directly. This is the *real* compiler prereq for the
"bounded-capacity world" design; without it, the world type can
have a size parameter but no two fields can be proven to share it.

**P3 -- Generalise `SizedBuf` with a phantom `n` parameter.**
`stdlib/sized-buf.tur` already provides a flat heap-allocated
int64_t array with a runtime length -- structurally identical to
dense storage's control block. Its public type is just `:int`
today, with no phantom size index. Lifting it to
`(SizedBuf n)` (handle still `:int` at the C level, but the type
carries `n`) gives the spice a natural substrate to build dense
storage on, instead of inventing a parallel abstraction. This is a
stdlib-only change, gated on P1.

**P4 -- An existential lift for runtime-cap worlds.**
Worlds are constructed with a runtime cap (e.g. `(world-new 1024)`),
but the for-each macros need to unify against a static `n`. The
`pack`/`open` mechanism in
[`stdlib/vec-existential.tur`](../../stdlib/vec-existential.tur)
already solves the analogous problem for `SizedVec` -- generalising
it to:

```turmeric
(world-new 1024) : (exists n. (World n))
```

would let callers `open-world` once and then run all systems under
the fresh abstract `n`. Once P1+P2 hold, this is a stdlib-pattern
duplication, not new elaborator work.

**P5 (stretch, refinement-types-gated) -- `Fin n` for in-bounds
indices.** For the full payoff (static OOB rejection on
`dense-set!`/`dense-get`), accessors would take `idx : (Fin n)`
rather than `idx : int`. This is refinement-types-gated and lines
up with E2b's existing block on refinements; not required for
Option 2 to ship the for-each unification win, but worth naming so
the design plan can stage it explicitly.

The minimal critical path is **P1 -> P2 -> design plan -> spice
work**. P1 and P2 are pure-compiler fixtures (each one file, no
spice changes) and can be answered in a single short session.
Their results determine whether E2c is a stdlib-and-spice exercise
or whether a fresh elaborator gap needs filing first.

## Validation of a fix

- The plan's E2c entry no longer claims SZ6-SZ8 is sufficient.
- A `docs/upcoming/ecs-sized-world-plan.md` exists with concrete
  answers (or open questions clearly labelled) to the design points
  above.
- No spice-side wiring change lands before the design plan is
  resolved.

## Related

- `docs/upcoming/ecs-spice-plan.md` (E2c entry, lines 320-326 and
  421-427)
- `docs/archive/history/sized-types-phantom-index.md` (the original
  SZ6-SZ8 gap report, now resolved)
- `tests/fixtures/sized-cross-param-accept/input.tur` (the SZ8
  reference fixture; demonstrates the GADT-constructor-chain
  requirement)
- `../turmeric-spices/spices/ecs/src/ecs/storage.tur` (the
  mutation-based dense storage that cannot carry a load-bearing
  size index)
