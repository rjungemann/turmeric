---
title: Variadic HKT rows (kind-level type lists) are missing from the elaborator
category: Expressiveness hole
severity: Forces an arity-N macro-family workaround for any "typed heterogeneous row" feature (ECS queries, relational rows, data-frame schemas). Not blocking for v1 work that accepts the cap.
description: The HKT machinery supports fixed-arity type constructors but has no kind-level list-of-types, so a `Query in out` parameterised by a row of components, a relation parameterised by a tuple of columns, or any other variadic-row abstraction has to be macro-generated as `Query1`..`QueryN` -- the same retreat Bevy/Specs/aztecs took. The downstream plans (ECS v1) have absorbed this by capping at arity 5 with a documented migration path.
---

# Variadic HKT rows are missing

## Summary

Turmeric's HKT support (per `docs/guides/hkt-guide.md`) lets a type
constructor be abstracted over and partially applied at the type level,
but **the kind system has no `List Type` (or `Row :: List Type`)**. Every
HKT-using abstraction today is parameterised by a fixed-arity tuple of
type arguments. There is no way to write `Query (cons Pos (cons Vel
nil)) (cons Pos nil)` and have the elaborator see the row structurally.

Workarounds in practice are macro-generated families
(`Query1`/`Query2`/.../`QueryN`), capped at some arity N. This is the
pattern Specs / Bevy / aztecs all settled on in Haskell/Rust; the whole
pitch of the ECS plan (and of `stats-formula-plan`, and any future
relational layer) is that we would not have to.

## Where this bites

- `docs/upcoming/v1/ecs-spice-plan.md` ships `queryN` (arity 1..5) on
  the v1 track and reserves the variadic `query` form for v2 E1, gated
  on this work. The arity cap is the same retreat Bevy/Specs took; we
  accept it for v1 with a documented migration path, but the original
  pitch ("more invariants at the type level than Haskell") is weaker
  until it lifts.
- `stats-formula-plan` and any future data-frame work want the same
  primitive (rows of columns) and would have to re-implement the same
  macro family, then re-migrate when this lands.
- A general relational / Datalog-results layer over heterogeneous tuples
  is currently expressible only via untyped cons-lists with downcasts at
  the use site, or via the same arity-N macro pattern.

## Observed vs. expected

**Observed.** No `Row` / `List Type` kind. No way to write a type
parameter that ranges over "a tuple of N types for any N." The HKT plan
in the archive treats associated types and type-level lists as
"deferred / not started."

**Expected (for the plans that depend on this).** A kind-level
`List Type` (or a dedicated `Row` kind), a `Row` HKT, and the elaborator
machinery to:

- check membership (`Pos in row`) against a row term,
- form unions/intersections of rows for query joins,
- propagate row arguments through partial application of a higher-kinded
  constructor.

## Root cause

The elaborator's kind language is fixed: `Type`, `Type -> Type`,
`Type -> Type -> Type`, etc. Adding a `List Type` kind is a real
elaborator project (new kind constructor, unification rules for cons /
nil at kind level, normalisation, fixture work). It is not a small
patch.

## Proposed directions

1. **Fund the elaborator work.** Add a `Row` (or `List Type`) kind,
   row-level cons/nil, structural row equality, and `in`/`++`
   primitives. Validate against `tests/fixtures/hkt-row-*` and an ECS
   query fixture.
2. **Document the macro-arity fallback as the v1 reality.** If we are
   not funding (1) in this release window, update `hkt-guide.md`,
   `ecs-spice-plan.md`, and `stats-formula-plan` to say so explicitly,
   so downstream plans stop quoting variadic rows as available.
3. **Hybrid: ship `Row` as runtime-only first.** A list-of-`TypeRep`
   row would unblock query macros at the cost of giving up the
   compile-time membership check. This is what Haskell ECSes do; we
   would be no worse off than they are.

## Validation of a fix

- A query / row fixture that declares `Query [Pos Vel] [Pos]` and
  `Query [Pos] [Pos]` and gets a compile error from
  `Query [Pos Vel] [Foo]` when `Foo` is unregistered.
- Row-polymorphic identity: `(defn id-row [r] (fn [x : Row r] x))`
  type-checks and is callable at distinct row instantiations.
- Stats / data-frame fixture: `Frame [Name :str Age :int]` zip with
  `Frame [Age :int Name :str]` is a row-permutation no-op, not a type
  error.

## Related

- `docs/upcoming/v1/ecs-spice-plan.md` D1
- `docs/guides/hkt-guide.md`
- `docs/archive/` HKT plan (associated types deferred)
