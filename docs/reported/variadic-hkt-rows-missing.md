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

## Progress (Direction 1 -- funding the elaborator work)

Direction 1 is the chosen path. It is a multi-layer elaborator project; the
layers are being landed incrementally, each validated on its own before the
next builds on it, so no half-finished layer can silently miscompile.

### Layer 1 -- kind-level `List Type` in the kind language [LANDED]

The kind language was fixed at `KIND_STAR` and the `KIND_ARROW{N}` ladder
(plus the `KIND_ROW` *effect*-row sentinel, an unrelated concept). The first
layer adds a distinct kind-level list-of-types sentinel so the rest of the
machinery has a kind to attach a row to:

- `KIND_TYPEROW ((Kind)0xFFFE)` in `src/compiler/types.h` -- the kind of a
  *row of types* (`[Pos Vel]`). Disjoint from `KIND_ROW` (0xFFFF, effect rows)
  and from every arrow arity.
- `kind_to_string` / `kind_parse` round-trip it as `"[*]"`
  (`src/compiler/types.c`).
- `kind_apply_one` treats it as inert (identity), like `KIND_ROW`: a row is a
  first-class kind, not a constructor you apply.
- `kind_is_arrow_ladder` (`src/passes/kind_check.c`) excludes it, so
  `kind_unify` rejects `[*]` vs `*` / `* -> *` / `Row` with `TUR-E0012`, and
  `kind_of_type_app` rejects applying a row as a constructor.
- Unit target `tur_kind_row_unit` (`tests/compiler/test_kind_row.c`, registered
  in `src/CMakeLists.txt` + root `CMakeLists.txt`) pins the parse/print
  round-trip, sentinel disjointness, the apply/unify algebra, and the
  diagnostic counts for the mismatch cases.

### Remaining layers (not yet started)

2. **`TY_TYPEROW` type variant** -- a `Type` carrying an arena array of element
   `Type`s, with `hkt_kind == KIND_TYPEROW`. Type equality compares rows
   structurally (and a permutation-aware variant for the data-frame use case in
   the validation section).
3. **Surface syntax** -- read `[Pos Vel]` in *type-annotation* position into a
   `TY_TYPEROW`. The parser already special-cases `[...]` in binding position;
   this must only fire where a type is expected, and must not perturb the
   existing `[...]`-as-binding / `vec-of` lowering (regression risk -- gate
   behind the row contexts only).
4. **Row HKT + `Row r` parameter** -- let a constructor parameter carry kind
   `[*]`, so `Query` is functorial over a row and `(defn id-row [r] (fn [x : Row r] x))`
   type-checks at distinct instantiations.
5. **Row operations** -- membership (`Pos in row`), union/`++` (query joins),
   propagation through partial application. These are what the ECS query and
   relational layers actually call.
6. **Codegen erasure** -- rows are compile-time only; decide the runtime
   representation (erased to the element tuple / iterator) and add an ECS query
   fixture plus the `hkt-row-*` fixtures named in the validation section.

When all layers land, `ecs-spice-plan.md` E1 (the variadic `query` form) is
unblocked and the `queryN` macro family becomes a deprecation alias per that
plan's D1.

## Related

- `docs/upcoming/v1/ecs-spice-plan.md` D1
- `docs/guides/hkt-guide.md`
- `docs/archive/` HKT plan (associated types deferred)
