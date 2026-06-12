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
phantom indices?** RESOLVED 2026-06-12 (yes, after the fix landed in
[docs/reported/sz8-opaque-phantom-size-not-load-bearing.md](sz8-opaque-phantom-size-not-load-bearing.md)).
The original probe found two blockers: the parser rejected Size GADT
literals (`(Static N)`) in non-GADT type-app slots, and even with the
parse fix the cross-parameter unifier walked only GADT-constructor
witnesses. Both were closed in one pass: `type_expr_from_form` /
`fn_type_from_form_impl` now accept Size literals as first-class
type-app arguments, and `sz_cross_param_unify` recovers a call's size
index from the callee's declared return form when no GADT witness is
available. Witnesses:
[`tests/fixtures/sized-cross-param-opaque-accept`](../../tests/fixtures/sized-cross-param-opaque-accept/input.tur)
(literal-vs-literal accept) and
[`tests/fixtures/errors/sized-cross-param-opaque-reject`](../../tests/fixtures/errors/sized-cross-param-opaque-reject/input.tur)
(TUR-E0260 on mismatched literals). P2 is now unblocked.

**P2 -- Does `defstruct` unify size variables across fields?**
RESOLVED 2026-06-12 at the structural-threading level. The world
struct declared as
`(defstruct World [m A B] (pos (Dense m A)) (vel (Dense m B)))`
threads `m` into both field types: a function declared on
`(World (Static 2) A B)` projects `.pos` and `.vel` to types whose
shared size variable unifies cleanly across `zip`'s two parameters.
Witness:
[`tests/fixtures/sized-struct-field-share-accept`](../../tests/fixtures/sized-struct-field-share-accept/input.tur).

Two follow-up gaps surfaced while answering P2:

- [sz8-projection-size-recovery-gap.md](sz8-projection-size-recovery-gap.md)
  -- SZ8 cross-param unification does not yet *statically reject*
  size mismatches across struct projections (the Size form is lost
  at the `TY_INT` placeholder step). The structural threading is
  enough to make the matched case type-check; closing this gap
  promotes the unmatched case from a runtime-only error to a
  compile-time TUR-E0260.
- [make-struct-phantom-typeparam-lowering.md](make-struct-phantom-typeparam-lowering.md)
  -- a pre-existing make-struct codegen bug mis-lowers a struct
  whose type parameter is used only through a type application
  (`(Dense m A)`) rather than as a bare field type. Closing this
  unblocks runtime construction of `World`-shaped carriers.

Neither follow-up blocks E2c's "design plan" gate -- they belong in
the spice-side phase. The structural P2 answer (the load-bearing
question of this section) is YES.

**P3 -- Generalise `SizedBuf` with a phantom `n` parameter.**
RESOLVED 2026-06-12. `stdlib/sized-buf.tur` was lifted to a
phantom-indexed `(SizedBuf n)` defopaque whose C carrier is still
`:int`. The public surface (`sized-buf-new`, `sized-buf-len`,
`sized-buf-get`, `sized-buf-set!`, `sized-buf-fill!`,
`sized-buf-copy!`, `sized-buf-sum`, `sized-buf-min`, `sized-buf-max`,
`sized-buf-size`, `sized-buf-free`, `sized-buf-from-sized-vec`) now
takes and returns `(SizedBuf n)`; the original inline-C bodies live
on as private `__sized-buf-*-raw` helpers wrapped by ascription. The
load-bearing case `sized-buf-copy!` shares `n` across `dst` and
`src`, so the SZ8 cross-parameter unifier (P1) rejects mismatched
literal lengths at compile time. Witnesses:
[`tests/fixtures/sized-buf-cross-param-accept`](../../tests/fixtures/sized-buf-cross-param-accept/input.tur)
and
[`tests/fixtures/errors/sized-buf-cross-param-reject`](../../tests/fixtures/errors/sized-buf-cross-param-reject/input.tur)
(TUR-E0260 on `(Static 2)` vs `(Static 3)`).

The reject fixture passes call expressions directly
(`(sized-buf-copy! (mk-2) (mk-3))`); a `let`-bound version of the
same mismatch silently compiles and aborts at runtime. That is the
EX_VAR sibling of the EX_GET_FIELD gap tracked under
[sz8-projection-size-recovery-gap.md](sz8-projection-size-recovery-gap.md);
closing that report would promote the let-bound path to a static
rejection too.

**P4 -- An existential lift for runtime-cap worlds.**
RESOLVED 2026-06-12 at the stdlib-pattern level. The
`vec-existential.tur` pattern was generalised into
[`stdlib/sized-handle-existential.tur`](../../stdlib/sized-handle-existential.tur)
with two type-agnostic macros, `pack-sized` and `open-sized`, that
thin-wrap native `pack` (EX1c) and `open` (EX1d / EX2-3) for any
phantom-indexed sized handle. Callers supply the binder vector and
body type, so the same pair lifts `(SizedVec n int)`,
`(SizedBuf n)`, or a hypothetical bounded-capacity `(World n)`
without per-type duplication. Witness:
[`tests/fixtures/sized-handle-existential-pack-open`](../../tests/fixtures/sized-handle-existential-pack-open/input.tur)
(SizedVec round-trip; prints `3`).

The phantom-defopaque elaborator gap surfaced and closed during the
same session: `elab_open` (`src/compiler/elab_types.c`) now preserves
the applied form when the existential body's head is a defopaque, so
`(SizedBuf n)` round-trips correctly through pack/open. Witness:
[`tests/fixtures/sized-buf-existential-pack-open`](../../tests/fixtures/sized-buf-existential-pack-open/input.tur).
Full resolution trail in
[pack-open-phantom-opaque-body-type-collapses.md](pack-open-phantom-opaque-body-type-collapses.md).

P4 is fully landed for both SizedVec and SizedBuf shapes; the macros
will lift a hypothetical bounded-capacity `(World n)` defopaque
unchanged once the world-API redesign produces one.

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
