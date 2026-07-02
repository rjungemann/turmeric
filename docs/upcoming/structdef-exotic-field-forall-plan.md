---
title: EF-3 -- Decide + (maybe) lower `(forall [a] T)` as a struct/ADT field
category: Planning
description: First a design decision (is a rank-N field meaningful?), then, if yes, route + carrier + fixture. Umbrella at structdef-exotic-field-forms-plan.md.
---

# EF-3: `forall` as a struct/ADT field

Predecessor: [structdef-exotic-field-forms-plan.md](structdef-exotic-field-forms-plan.md).
Sequenced after [EF-2](structdef-exotic-field-handler-plan.md); shares
the "add a storage case" pattern but is gated by a design decision.

## Why this needs a design phase

The DS-A4 gate comment historically flagged `forall` as "not a
value-carrying field form." Rank-N polymorphism in a field means storing
a value whose *type* is `forall a. T[a]` -- concretely a poly-wrapper.
Whether Turmeric wants first-class poly-fields at v1 is not settled. This
plan starts with that decision, then either lowers or moves the form to
a permanent "unsupported" note.

## Phase 1 -- Decide

- **P1.T1.** Write up (in this doc, appended) what a `forall` field
  would mean semantically:
  - Does it store a poly-wrapper (closure over a type parameter)?
  - Or is it always instantiated at struct-construction time (and thus
    equivalent to the concrete type)?
  - How does it interact with monomorphization (the end-to-end
    monomorphization north-star)?
- **P1.T2.** Survey producers: grep the stdlib, spices, and fixture tree
  for any user that *wants* a `forall`-typed field but has to work
  around it. If none, weigh whether to ship the feature ahead of a
  concrete demand.
- **P1.T3.** Decide: **lower**, **permanent-reject**, or **shelve**.

Phases 2-4 are conditional on `lower`. If `permanent-reject`, jump to
Phase 5.

## Phase 2 -- Route the head (conditional: lower)

- **P2.T1.** Drop `e->sym_forall` and `e->sym_forall_u` from the DS-A4
  rejection block.
- **P2.T2.** Add a dispatch to `type_expr_from_form` (mirrors `exists`
  at line 226-228 -- `exists`-pack fields already lower via the same
  path).

## Phase 3 -- Storage (conditional: lower)

- **P3.T1.** `struct_field_storage_from_type` already maps `TY_FORALL`
  to `TY_INT` (line 158). Verify no additional whitelist entry is
  needed on the record-ADT field-lowering path.
- **P3.T2.** Confirm the value representation: a `forall`-typed field
  needs a runtime carrier that survives instantiation. Cross-check with
  the end-to-end monomorphization plan -- a poly-field is one of the
  cases that plan explicitly reasons about.

## Phase 4 -- Fixture + verify (conditional: lower)

- **P4.T1.** `tests/fixtures/defstruct-field-forall/` -- construct a
  struct holding a poly-value, instantiate at two different types,
  print both.
- **P4.T2.** Regenerate snapshots, full-suite run.

## Phase 5 -- Permanent-reject path (conditional)

- **P5.T1.** Move the `forall` bullet out of the umbrella
  `structdef-exotic-field-forms-plan.md` into a "permanently
  unsupported field forms" note (create if none exists).
- **P5.T2.** Keep the rejection in `struct_field_type_from_form` but
  reword the diagnostic to reference the new note instead of the
  umbrella plan.
- **P5.T3.** Delete this plan file (or move to `docs/archive/`).

## Phase 6 -- Umbrella update

- **P6.T1.** In either outcome, remove `forall` from the umbrella
  status table and update its "rejected forms" list.
