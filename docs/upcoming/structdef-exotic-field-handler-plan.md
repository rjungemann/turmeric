---
title: EF-2 -- Lower `(handler E V R)` as a struct/ADT field
category: Planning
description: Route the `handler` head onto the record-ADT field path and add a TY_HANDLER carrier storage case. Umbrella at structdef-exotic-field-forms-plan.md.
---

# EF-2: `handler` as a struct/ADT field

Predecessor: [structdef-exotic-field-forms-plan.md](structdef-exotic-field-forms-plan.md).
Sequenced after [EF-1](structdef-exotic-field-arrow-plan.md) (same routing
pattern, but adds a new storage case).

## What makes this different from EF-1

A handler value is a runtime object. Unlike `fn`, it does not already have
a "field storage kind" entry in `struct_field_storage_from_type` -- so
this task adds one (map `TY_HANDLER` to the int64 carrier) rather than
just riding an existing case. Handler-specific effect and lifetime
checks that the legacy struct path applied also need to be reproduced on
the ADT path.

## Phase 1 -- Route the head

- **P1.T1.** In `struct_field_type_from_form`
  (`src/compiler/elab_structs.c`), drop `e->sym_handler_type` from the
  DS-A4 rejection block.
- **P1.T2.** Add a dispatch immediately below the fn dispatch: if
  `head == e->sym_handler_type`, call `type_expr_from_form` to build a
  `TY_HANDLER`. Confirm `type_expr_from_form` parses `(handler E V R)`
  correctly; if not, first extend it.

## Phase 2 -- Storage

- **P2.T1.** In `struct_field_storage_from_type`, add an explicit
  `case TY_HANDLER:` mapping to `*out_kind = TY_INT;` (int64 carrier). Do
  NOT rely on the `default:` fall-through -- it emits `t->kind`, which
  the field-lowering path may not accept as a storage slot.
- **P2.T2.** If the record-ADT field-lowering path has a "valid carrier
  storage kind" whitelist, add `TY_HANDLER` there too (grep for the set
  used around the DS-A3 borrow-family landing).

## Phase 3 -- Effect / lifetime check parity

- **P3.T1.** Audit what the residual `StructDef` path did for handler
  fields (search for `TY_HANDLER` uses in `elab_structs.c` and callers).
  Common candidates: effect-row propagation onto the containing struct,
  linear/affine handling of the handler resource, capability check on
  read.
- **P3.T2.** For each check found, reproduce it on the ADT path -- see
  DS-A3's `:copy`/linear + `TUR-E0102` reproduction (CONV-S5 block) as
  precedent.
- **P3.T3.** If any handler-specific diagnostic loses its old error code,
  keep the code stable (use the same `TUR-Exxxx` id at the ADT site).

## Phase 4 -- Fixture + verify

- **P4.T1.** Add `tests/fixtures/defstruct-field-handler/` with:
  - `input.tur` -- `(defstruct HRow [h : (handler E V R)])`, store a
    handler, invoke it, print the result.
  - Second sub-fixture (or negative case) exercising the effect-row /
    lifetime check reproduced in Phase 3, so a regression shows up as a
    diagnostic-content or error-code drift.
  - `expected.c` regenerated.
- **P4.T2.** Full-suite run under the 10-minute timeout; DS-B assert must
  stay green.

## Phase 5 -- Umbrella update

- **P5.T1.** Remove `handler` from the DS-A4 rejection block comment and
  the umbrella plan's status table.
