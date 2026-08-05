# Decision: session-effects / session-mp-effects are permanent fiber clients (WONT-FIX as CPS targets)

Resolves `docs/archive/cps-session-effects-permanently-fiber-bound.md`.

## Outcome

This report is an analysis whose own recommendation is **not to make a compiler
change** ("Do NOT treat these as CPS-backend admission targets ... Do not land
such an admission").  Executing it = confirming the analysis and enacting the
decision durably, not writing code.

## What was done

1. **Reproduced the analysis verbatim** on this branch:
   - `session-effects`: `SIG-REJECT exchange` (session-typed param, TY_SESSION) +
     `SIG-INLINE-C main` (pthread + inline-C session ops; handles `SessionLog`).
   - `session-mp-effects`: `SIG-REJECT role-a` (TY_ROLE) + `SIG-INLINE-C main`
     (handles `MpLog`).
   - Both still run correctly on the fiber (`sending 99` / `received 99` / `99`).

2. **Recorded the decision** (accept as permanent fiber clients; scope out of the
   DK deletion) in the archived report.

3. **Added a durable "permanent fiber client -- NOT a CPS/DK migration target"
   note** to each fixture header (`tests/fixtures/session-effects/input.tur`,
   `tests/fixtures/session-mp-effects/input.tur`) explaining the compound
   eviction (SIG-INLINE-C main permanently handles the effect; the session/role
   param SIG-REJECTs and would only reclassify to SIG-TAINT if admitted) and
   pointing at the archived report -- so the analysis is not re-derived and the
   net-zero admission is not re-attempted.  ASCII-only; comments only, so no
   codegen/output change (both fixtures still pass).

4. **Corrected the deletion plan** `docs/upcoming/v2/cps-dk-sole-effect-lowering-plan.md`
   (this branch is part of the same cps-runtime-finish-plan line -- the plan is
   ours to maintain):
   - **Sec 4 / W5:** added the correction that the two SIG-INLINE-C session mains
     are NOT "covered by E3" -- they HANDLE a real effect and their inline-C is
     load-bearing pthread/session runtime, so they are permanent fiber clients of
     a separate concurrency subsystem, out of scope for the deletion.
   - **Sec 2c (decisive gate)** and **Sec 7 (done)**: qualified the
     "no effectful colored fn falls back" invariant with the session/thread
     carve-out.
   - **Sec 6 step 1 (build-time assertion):** must whitelist that carve-out.
   - **Sec 6 step 3 + Sec 7 (measured blocker):** the two fixtures' emitted C
     actually USES `tur_effect_perform` / `global_effect_handler_chain` /
     `EffectHandlerFrame` / `tur_handler_dispatch` (verified), so "delete the
     fiber effect runtime C" and "grep = 0" **cannot be reached** until they are
     rewritten or bucketed out. Recorded the three resolution options.

## What was deliberately NOT done

- **No compiler change.** A TY_SESSION/TY_ROLE param signature widening moves
  zero fixtures (the SIG-INLINE-C `main` still permanently taints the effect) and
  adds gated signature surface for no gain -- exactly the reverted opaque-carrier
  slice.  The report is explicit that such an admission must not land.

## The framing (for the fiber-runtime deletion)

"Delete the fiber effect runtime" means "delete the delimited-continuation DK
effect machine's fiber path."  The pthread + inline-C session/channel runtime is
a SEPARATE concurrency subsystem; these two fixtures are its effect clients and
are out of scope for the CPS/DK lowering.  The fiber effect runtime cannot be
deleted to ZERO without separately addressing the thread-based session
concurrency model, which is not a CPS-lowering task.
