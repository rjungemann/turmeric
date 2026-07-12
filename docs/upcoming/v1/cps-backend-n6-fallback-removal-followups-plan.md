---
title: CPS backend N6 (gate item 7) -- remaining work to delete the fallback
category: Planning
status: STALE -- see cps-backend-n6-fallback-removal-followups-findings.md. A
  re-measurement found both tasks rest on premises that no longer hold: Task 1
  (resuming-shift) is vacuous (base shift is abortive-only; a resuming base shift
  is not expressible), and Task 2 (delete the fallback) would hard-error ~130+
  fixtures of ordinary effect/tier-C/sized/session programs, not just the
  delimited-control carve-out. Rescope before resuming.
description: N6.1-N6.4 landed -- general control-op-free delegation, multi-case handle, multi-arg effects, shift0, capturing continuations (N6.3a-h), indirect calls, and the signature widening (nil/void return, effect-free and effectful TY_FN params). The colored-generic-monomorph lever shipped in its own plan. Two items remain before cps-backend satisfies graduation gate item 7: the resuming-SHIFT-body lowering (the sole open control-flow shape), and N6.5 -- deleting the general whole-function fallback with an explicit delimited-control carve-out.
---

# CPS backend N6 -- remaining work

> **STALE (2026-07-12).** A re-measurement against the graduated, always-on
> cps-backend found both tasks below rest on premises that no longer hold. Task 1
> is vacuous and Task 2 would break ~130+ fixtures. Read
> [cps-backend-n6-fallback-removal-followups-findings.md](cps-backend-n6-fallback-removal-followups-findings.md)
> and rescope before doing any work here.

## Context

The measurement, the delegation lever, and the N6.1-N6.4 landings live in the
archived parent,
[cps-backend-n6-fallback-removal-plan.md](../../archive/cps-backend-n6-fallback-removal-plan.md).
Gate item 7 (from
[cps-backend-non-scalar-values-plan.md](../../archive/cps-backend-non-scalar-values-plan.md))
makes the CPS backend the **sole** lowering for colored (may-capture) functions:
no `CT_UNSUPPORTED` whole-function bail-out, no direct-vs-CPS dual path.

What already landed (all green, full suite passing at each slice):

- **N6.1** general delegation of control-op-free, colored-call-free subexpressions
  via `CT_LETRAW` (`safe_to_delegate`, `src/passes/cps_ir.c`).
- **N6.2** multi-case `handle` (`CHandleCase`), multi-arg effects, `shift0`.
- **N6.3a-h** capturing continuations (perform / handle / intermediate /
  handler-case / shift-body / non-scalar / fn-value / owning captures).
- **N6.4** indirect calls; signature widening -- nil/void return, effect-free
  `TY_FN` params, and effectful `TY_FN` callback params
  ([archived](../../archive/cps-backend-effectful-callbacks-plan.md)). The
  colored-generic-monomorph classification (the last large sig lever) shipped in
  its own [archived plan](../../archive/cps-backend-generic-monomorph-classification-plan.md).

Until N6.5 lands the fallback stays and coverage grows monotonically under the
(now graduated, always-on) backend.

## Task 1 -- resuming SHIFT bodies (the open N6.3 control shape)

**State: not started -- the sole remaining control-flow gap.** The current shift
lowering (`cps_shift_body`) applies the receiver to the shift **body value** and
delivers the result to the prompt; the captured continuation `subk` is passed to
the shift-body helper but **ignored** (abortive-only). A `shift` whose receiver
actually *invokes* the captured continuation cannot be expressed in the
receiver-applied-to-body form.

**Approach.** Add a lowering that **binds `subk`** and threads it into the receiver
(so the receiver can `dk_invoke` it), selected when the receiver references the
continuation. Keep the abortive form as the fast path when the receiver does not.
Fixture: a `shift` whose receiver calls its continuation (e.g. a generator/step
shape), `direct == cps`, LeakSanitizer-clean.

## Task 2 -- N6.5: delete the general whole-function fallback

**State: not started (blocked only by Task 1 for full coverage).** Remove the
`CT_UNSUPPORTED` whole-function bail-out and the direct-vs-CPS dual path from
`emit_cps_ir.c` and the classifier. Any residual form becomes a **hard error with
a form-named diagnostic** (the measurement patch that annotates `CT_UNSUPPORTED`
with the `Expr` kind is the seed). Re-run the full suite and the stackless
sign-off probe with the general fallback gone.

**The one deliberate carve-out is the delimited-control tail** -- cloneable /
serial / async, and the raw multi-shot reset/shift they build on. Those are owned
by the program-level `emit_cps.c` whole-program transform (gated on
`emit_cps_program_uses_cloneable_dk` / `_uses_delimited` / `_contains_serial`),
which already carries the DK deep-clone (`dk_copy_range`) and capture clone/drop
glue, and they are **verified correct there** (`cloneable-context-if`,
`serial-context-do`, `async-await-basic` all match the direct path). N6.5 keeps
routing *those specific forms* to `emit_cps.c` -- a named, intentional carve-out --
rather than porting them into the N6 CT-IR backend (a large, no-correctness-benefit
effort) or hard-erroring them.

So "delete the fallback" means: delete the **general** whole-function fallback for
colored functions, leaving only the delimited-control forms on their existing
owner (`emit_cps.c`).

**Ordering.** Task 1 should land before Task 2 so resuming-shift functions
CPS-emit rather than turning into hard errors at fallback deletion. If any other
residual surfaces during N6.5, either cover it or add it to the named carve-out
with justification -- do not silently re-introduce a general fallback.

Only after Task 2 does `cps-backend` satisfy gate item 7.

## Depends on / reuses

- `cps_shift_body` and the DK shift lowering (`src/compiler/emit_cps_ir.c`).
- `CT_LETRAW` delegation + `safe_to_delegate` (`src/passes/cps_ir.c`).
- `emit_cps.c` whole-program transform (the delimited-control carve-out owner).
- The `CT_UNSUPPORTED` form-named-diagnostic seed from the measurement patch.

## Out of scope

- Uncolored functions -- never CPS-emitted; N6 is only about colored functions.
- Owning-field aggregate / carrier crossings -- gate item 4, see the
  [owning-pointers follow-ups](cps-backend-owning-pointers-followups-plan.md).
- Porting cloneable / serial / async into the CT-IR backend -- explicitly the
  carve-out, not a goal.
