---
title: CPS backend N6 (gate item 7) -- remaining work to delete the fallback
category: Planning
status: ARCHIVED / SUPERSEDED -- split into two follow-on plans (see "Findings 2026-07"). N6.1-N6.4 landed.
description: ARCHIVED. N6.1-N6.4 landed. The two remaining items were found blocked as scoped and were split into their own plans -- resuming SHIFT (docs/upcoming/v1/cps-backend-n6-resuming-shift-plan.md) and N6.5 fallback deletion (docs/upcoming/v1/cps-backend-n6-fallback-deletion-plan.md). Full analysis in docs/reported/cps-backend-n6-fallback-followups-blocked.md.
---

# CPS backend N6 -- remaining work (ARCHIVED)

## Findings 2026-07 -- both remaining tasks split into their own plans

Executing this plan established that both remaining items are blocked as
originally scoped; each is now its own follow-on plan:

- **Task 1 -> [cps-backend-n6-resuming-shift-plan.md](../upcoming/v1/cps-backend-n6-resuming-shift-plan.md)**
- **Task 2 (N6.5) -> [cps-backend-n6-fallback-deletion-plan.md](../upcoming/v1/cps-backend-n6-fallback-deletion-plan.md)**

Full analysis + measurements:
[cps-backend-n6-fallback-followups-blocked.md](../reported/cps-backend-n6-fallback-followups-blocked.md).

- **Task 1 (resuming SHIFT) is not expressible.** Turmeric `shift` is abortive
  on every path (interp `eval_abortive_shift`, direct `emit_effects_shift`,
  CT-IR `cps_shift_body_kf` -- all emit `receiver(body_value)`), and the type
  rule enforces the receiver's param type == the *body's* type, not a
  continuation type. A receiver that invokes the continuation fails to
  type-check (`TUR-E0001`). Making it work requires a non-abortive `shift`
  across the type rule + interpreter + direct emitter -- the separate
  first-class-continuations plan, not a CT-IR extension. No `direct == cps`
  fixture is constructible until that lands.

- **Task 2 (N6.5 delete the general fallback) is premature.** The general
  whole-function fallback is still load-bearing for ~100 distinct colored
  functions across the corpus (measured via the new `TUR_TRACE_EVICT` trace):
  86 `BODY-STRUCT-OR-TAINT` + 15 `BODY-UNSUPPORTED`, whose residual forms are
  ordinary (`while`/`set!` loops, capturing closures, `match`, ...) -- none are
  resuming shifts or the delimited-control carve-out. (Plus ~428 permanent
  `SIG-*` routings, which are ABI, not the fallback.) Deleting the fallback now
  would hard-error ~100 working functions. N6.5 is gated on first closing the
  `BODY-*` coverage gap; the trace is the readiness gate.

Seed work that DID land: form-named `CT_UNSUPPORTED` diagnostics
(`cps_form_name`/`unsupported_form`, `src/passes/cps_ir.c`) and the
`TUR_TRACE_EVICT` categorized eviction trace (`src/compiler/emit_cps_ir.c`) --
both internal, no codegen/fixture change.

## Context

The measurement, the delegation lever, and the N6.1-N6.4 landings live in the
archived parent,
[cps-backend-n6-fallback-removal-plan.md](cps-backend-n6-fallback-removal-plan.md).
Gate item 7 (from
[cps-backend-non-scalar-values-plan.md](cps-backend-non-scalar-values-plan.md))
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
  ([archived](cps-backend-effectful-callbacks-plan.md)). The
  colored-generic-monomorph classification (the last large sig lever) shipped in
  its own [archived plan](cps-backend-generic-monomorph-classification-plan.md).

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
