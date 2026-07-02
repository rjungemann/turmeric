---
title: EF-4 -- Lower session / project / global / role types as struct/ADT fields
category: Planning
description: Group the eight remaining session-typing heads. Each gets a route + storage + fixture + typecheck-parity review. Umbrella at structdef-exotic-field-forms-plan.md.
---

# EF-4: session / project / global / role heads as struct/ADT fields

Predecessor: [structdef-exotic-field-forms-plan.md](structdef-exotic-field-forms-plan.md).
Sequenced last -- the exotic corner. Do EF-1 and EF-2 first to
establish the route/storage pattern before applying it eight times.

Heads in scope (grouped for triage):

- **Session core**: `session_type`, `Send`, `Recv`, `Choose`, `Branch`,
  `Rec`, `Timeout`.
- **Choreography**: `project`, `global`, `role`.

Cross-reference: [project_session_types_phase.md] in user memory --
session types shipped through SS0b; SS1 is next and may reshape what
"session field" means. Coordinate.

## Phase 1 -- Triage (per-head keep-or-drop)

For each head, decide:

- **Lower** -- has a plausible field use case; add route + storage.
- **Reject permanently** -- has no meaningful field representation
  (e.g. `role` may only make sense as a top-level participant tag, not
  a first-class field value).
- **Defer past SS1** -- SS1 may change the answer; leave rejected with a
  pointer to the session-types phase.

Deliverable: an ASCII table appended to this doc, one row per head,
one of `{lower, reject, defer}` plus one-line rationale.

## Phase 2 -- Route (per head marked `lower`)

- **P2.T1.** Drop the head from the DS-A4 rejection block.
- **P2.T2.** Add (or extend) a dispatch to `type_expr_from_form`. The
  session heads share a family type kind (grep `TY_SESSION*`); one
  combined dispatch is likely enough.
- **P2.T3.** Confirm `type_expr_from_form` parses each head. Session
  heads with sub-forms (e.g. `Send`, `Recv`, `Choose`, `Branch`, `Rec`,
  `Timeout`) each have distinct arity -- verify the elaborator handles
  each shape.

## Phase 3 -- Storage (per head marked `lower`)

- **P3.T1.** Add explicit `case TY_SESSION*:` (whichever kinds emerge
  from Phase 2) mappings to the int64 carrier in
  `struct_field_storage_from_type`. Do not rely on `default:`.
- **P3.T2.** If any session kind carries an inner type worth preserving
  (analogous to `TY_LREF`/`TY_REF`), keep the inner slot instead of
  collapsing to a bare carrier.

## Phase 4 -- Typecheck-parity review (per head marked `lower`)

- **P4.T1.** Session types come with linearity / usage-once obligations
  (a session endpoint typically may not be dropped or duplicated).
  Audit what the legacy `StructDef` path enforced for session-typed
  fields; reproduce on the ADT path.
- **P4.T2.** `project` / `global` fields may have participant-set /
  role-consistency invariants. Reproduce.
- **P4.T3.** Keep every diagnostic id (`TUR-Exxxx`) stable across the
  move.

## Phase 5 -- Fixtures (per head marked `lower`)

One sub-fixture per head, minimum: construct the struct, read the
field, exercise the head-specific semantics (send/recv, project onto a
role, etc.). Group under `tests/fixtures/defstruct-field-session-*/`.

## Phase 6 -- Reject-path record (per head marked `reject` / `defer`)

- **P6.T1.** For `reject`, keep the rejection but move the diagnostic
  wording to a permanent-unsupported note (as EF-3 Phase 5).
- **P6.T2.** For `defer`, keep the rejection but the diagnostic
  references the session-types SS1 plan instead of this file.

## Phase 7 -- Umbrella + close

- **P7.T1.** Update the umbrella status table row-by-row.
- **P7.T2.** Full-suite run, DS-B assert green, snapshots regenerated.
- **P7.T3.** Archive this plan once every head has landed in `lower`,
  `reject`, or `defer`.
