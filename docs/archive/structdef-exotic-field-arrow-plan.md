---
title: EF-1 -- Lower `(arrow ...)` / `(-> ...)` as a struct/ADT field
category: Planning
description: Route the `arrow` / `->` head through the fn-type elaborator so `(-> A B)` can be a defstruct/defdata field. Umbrella at structdef-exotic-field-forms-plan.md.
---

# EF-1: `arrow` / `->` as a struct/ADT field

Predecessor: [structdef-exotic-field-forms-plan.md](structdef-exotic-field-forms-plan.md)
(umbrella; DS-A4 rejection background, precedent from DS-A3 borrow-family lowering).

## Why this is first

`(-> A B)` is just an alternate spelling of `(fn [A] B)`. `fn` already
lowers as a struct field (see the `sym_fn` / `sym_c_fn` dispatch in
`src/compiler/elab_structs.c:237`), so `arrow` is the shortest possible
delta: one extra head symbol on the existing route, one storage case that
is already covered by the `TY_FN` default in
`struct_field_storage_from_type`.

## Phase 1 -- Route the head

- **P1.T1.** In `struct_field_type_from_form`
  (`src/compiler/elab_structs.c:199`), remove `e->sym_arrow` from the
  DS-A4 rejection block at lines ~211-217.
- **P1.T2.** Extend the existing fn dispatch at line 237 so
  `head == e->sym_arrow` also routes to `type_expr_from_form`. The arrow
  head shares the fn-type parser -- confirm `type_expr_from_form` already
  recognises `(-> A B)`; if not, teach it (mirror `sym_fn`).
- **P1.T3.** Update the umbrella doc's "rejected forms" list and the
  DS-A4 comment to drop `arrow`/`->`.

## Phase 2 -- Storage

- **P2.T1.** Verify `struct_field_storage_from_type` produces a valid
  carrier slot for the arrow-typed field. `TY_FN` currently falls through
  to `default: *out_kind = t->kind` (line 173-175); if the ADT field-lowering
  path treats `TY_FN` as a valid storage kind (as it does for the `fn` field
  today), no change is needed. If not, add an explicit `case TY_FN:` mapping
  to the int64 carrier.
- **P2.T2.** Confirm the arrow head produces `TY_FN` (not a distinct
  `TY_ARROW`) after `type_expr_from_form`. If a distinct kind exists, add its
  case too.

## Phase 3 -- Fixture + verify

- **P3.T1.** Add `tests/fixtures/defstruct-field-arrow/` with:
  - `input.tur` -- `(defstruct Cell [f : (-> int int)])`, construct with a
    lambda, invoke `(.f cell 41)`, print result.
  - `expected.out` -- `42`.
  - `expected.c` -- regenerated from `tur emit-c`.
- **P3.T2.** Add a negative-turned-positive companion: whatever fixture
  previously asserted the DS-A4 rejection for arrow (if any) is deleted
  or repurposed.
- **P3.T3.** Regenerate any snapshot that shifts, then
  `bash tests/run.sh 2>&1 | grep '^FAIL'` must be empty (10-minute
  timeout, per CLAUDE.md).

## Phase 4 -- Invariant check

- **P4.T1.** The DS-B assert in `elab_register_struct_def` must stay green
  (no `defstruct` reaches the residual `StructDef` path). Full-suite run
  covers this.
- **P4.T2.** Remove `arrow`/`->` from the umbrella plan's status table
  (mark shipped, link this file's landing commit).
