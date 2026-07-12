---
title: Owning pointers on the CPS-IR-to-C backend -- remaining follow-ups
category: Planning
status: archived -- fully dispatched: O1-a landed; O1-b and O3 spun out to their own plans
description: The owning-pointer analysis + landing (O1 owning-value locals, O2 owning fields inside aggregates via N3, O3 guarded by the zero-capture cut) shipped and re-scoped graduation gate item 4; see the archived parent. This doc carried the small, deliberately-deferred remainder and is now archived because every item has a home: O1-a (non-atomic owning-op operand delegation) landed; O1-b (ref<T> scope-exit auto-drop / EX_DEFER) is planned in cps-backend-ref-scope-exit-drop-plan.md; O3 (captured owning values) is planned in cps-backend-env-capture-owning-values-plan.md. Both remaining items are safe on the fallback today and off the critical path for cps-backend graduation. Kept as the index that ties O1-a's landing to the two follow-on plans.
---

# Owning pointers on the CPS backend -- remaining follow-ups

## Context

The analysis and the landed work live in the archived parent,
[cps-backend-owning-pointers-plan.md](cps-backend-owning-pointers-plan.md).
Its four findings still hold and are the frame for everything below:

1. Bare owning pointers (`ref` / `rc` / `weak` / `lref`) are never bare slot
   values -- they cross the DK slot only as fields of a struct / ADT, so there
   is **no `slot_ty` row to add**.
2. The real crossing is the enclosing aggregate; its ownership discipline is the
   aggregate's drop glue. That landed **as part of N3** (O2).
3. Multi-shot double-free-via-capture is excluded by the backend's zero-capture
   cut (`has_capture` / `has_capture_case` in `emit_cps_ir.c`).
4. The genuine CPS-specific work is faithful **drop-node translation**; O1
   landed it for explicit `rc` ops with atomic operands, guarded by the
   `owning_dropped_before_control` liveness check (`term_core_ok`,
   `emit_cps_ir.c`).

**Everything that remains is safe on the fallback today** -- no naive bit-copy of
an owning pointer through the slot occurs, and no memory-safety bug is open. These
are missed-coverage items, not correctness gaps.

## Task O1-a -- non-atomic owning-op operands

**State: landed.** `cps_ir.c` translated an owning-value op
(`EX_RC_OF` / `EX_RC_CLONE` / `EX_RC_DROP` / `EX_RC_COUNT` / `EX_RC_PTR`) to a
`CT_LETRAW` delegated node **only when its operand is atomic**
(`is_delegatable_owning`, `src/passes/cps_ir.c`). The atomic restriction is what
guaranteed no control operator hides inside the operand and gets emitted in
direct style inside a CPS function. So `(rc/of (compute))` -- a non-atomic but
often control-op-free operand -- fell back whole.

**Landed.** `operand_uses_control` (`src/passes/cps_ir.c`) is a sound
"operand MIGHT contain a control op" scan that mirrors the control-op seed set
and structural recursion of `cps_directly_uses_control` (`src/passes/cps.c`) but
inverts the default -- a node kind it does not positively recognize as
control-free is treated as possibly-control (not delegatable). `is_delegatable_owning`
now admits a non-atomic operand that passes it (`!operand_uses_control(arg)`). A
missed control op falls back, never delegates -- the existing liveness guard
(`owning_dropped_before_control` / `letraw_ok`, `emit_cps_ir.c`) still applies to
the resulting node, so the straight-line-drop-before-control invariant is
preserved. The `has_capture_rec` CT_LETRAW case (`emit_cps_ir.c`) was switched
from the shallow direct-var enumeration to the complete `collect_free_vars`
walker (matching `collect_caps_rec`), so a non-atomic operand's nested free vars
are seen and a genuinely-capturing op is never wrongly admitted into a lifted
zero-capture body.

**Fixture.** `tests/fixtures/cps-backend-rc-nonatomic-operand`: a colored
function does `(rc/of (+ a b))` before a `shift`, drops it on the straight-line
path, delivers a scalar; `direct == cps` (7) and LeakSanitizer-clean.

## Task O1-b -- `ref<T>` scope-exit auto-drop (`EX_DEFER`)

**State: deferred, planned in
[cps-backend-ref-scope-exit-drop-plan.md](../upcoming/v1/cps-backend-ref-scope-exit-drop-plan.md).**
`ref<T>`'s auto-drop is injected as `(defer (drop! r))` at **scope exit**
(`elab_forms.c` ~1000). In a colored function that scope-exit drop lands *after*
the control op; an abortive `shift` discards its continuation, so the drop lands
in the discarded region -- exactly the case `owning_dropped_before_control`
rejects. A `ref` whose scope ends *before* the control op could translate, but a
`ref` that crosses the control op is captured by the continuation (fallback). So
a `ref` local in a colored function almost always falls back regardless of
`EX_DEFER` translation.

That plan splits the spectrum: P1 hoists the drop to last-use for a `ref` that
does **not** cross a control op (self-contained, no runtime -- the one tractable
slice); P2 (abortive-crossing) and P3 (resumable-crossing) ride the DK-teardown /
owning-capture clone-drop substrate from the env-capture plan. Landing P1 is the
standalone O1-b win; P2/P3 stay gated on a real crossing-`ref` case, per the
original caveat.

## Task O3 -- captured owning values

**State: deferred, planned in
[cps-backend-env-capture-owning-values-plan.md](../upcoming/v1/cps-backend-env-capture-owning-values-plan.md).**
If a future phase lifts the zero-capture cut and lets a continuation capture live
locals, an owning capture must be deep-cloned / incref'd per `dk_copy` (DK
continuations are multi-shot, so a shallow copy of a frame holding an owning
pointer would double-free). Until the cut is lifted this is unreachable, so O3
stays off the critical path for graduation. The env-capture work that owns O3 now
has a concrete plan (gate relaxation + per-capture clone/drop glue in the CT-IR
backend, phased E1-E4); see that document.

## Depends on / reuses

- `is_delegatable_owning` + `CT_LETRAW` delegation (`src/passes/cps_ir.c`).
- `owning_dropped_before_control` liveness guard (`term_core_ok`,
  `src/compiler/emit_cps_ir.c`).
- The zero-capture cut (`has_capture` / `has_capture_case`, `emit_cps_ir.c`).
- Drop injection + move analysis (`elab_forms.c`); drop emission
  (`rc_strong_decrement` / `drop!`, `emit_expr.c` / `emit_module.c`).

## Out of scope

- Adding `ref` / `rc` / `weak` / `lref` to `slot_ty` -- Findings 1-2 show there is
  nothing to hook; owning pointers never cross the slot bare.
- Lifting the zero-capture cut itself -- that is the env-capture story, planned
  in
  [cps-backend-env-capture-owning-values-plan.md](../upcoming/v1/cps-backend-env-capture-owning-values-plan.md);
  O3 rides it.
