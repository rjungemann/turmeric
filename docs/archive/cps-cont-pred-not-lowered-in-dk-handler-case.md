# `(cont? k)` is not lowered in the CPS/DK backend -> BODY-UNSUPPORTED (effect-cont-pred, effect-deep-handler)

**STATUS: RESOLVED.** `EX_CONT_PRED` now lowers in the CPS/DK backend via a DK
`consumed` flag.  Both `effect-cont-pred` (`true`/`42`) and `effect-deep-handler`
(`3`) DK-lower with zero `eff=1` and correct output.  Flag-off byte-identical
(all fixture snapshots unchanged); flag-on effect soundness sweep clean; full
suite green.

## What was wrong

`EX_CONT_PRED` had a direct-emitter lowering (emit_effects.c
`emit_effects_cont_pred`: fiber `!fiber->done` / `!tur_cont_consumed`) but NO
CPS-IR translation -- `cps_tail`/`cps_bind` (src/passes/cps_ir.c) had no case, so
it hit the default and evicted `CT_UNSUPPORTED`.  The two top-level-handle
fixtures whose handler case uses `(cont? k)` (folded into a d2b main by the
synthesized-main fold) then reported `unsupported form: EX_CONT_PRED` and stayed
on the fiber.

In the DK model `k` is a delimited continuation (`DK *`, bound in the handler
case as `subk`), which carried no consumed/done state.

## The fix (a DK `consumed` flag; sound, general)

1. **struct DK** (emit_dk_runtime.c, inside the `if (tramp)` block so flag-off is
   byte-identical): add `bool consumed;`.  `dk_new` calloc-zeroes it, so a
   freshly captured k starts unconsumed.
2. **resume** (emit_cps_ir.c `emit_resume`): set `((DK *)k)->consumed = 1;` at the
   user resume site (before both the tail `dk_tail_resume` and the normal
   `dk_invoke`), flag-gated.
3. **cont?** (src/passes/cps_ir.c): a `build_cont_pred` helper lowers
   `EX_CONT_PRED` to a `CT_LETPRIM` with a sentinel spec (c_op
   "TUR_DK_CONT_PRED"); intercepted before the `cps_tail`/`cps_bind` switches so
   flag-off flows to the historical default unchanged.  The CT emitter
   (emit_cps_ir.c `CT_LETPRIM`) special-cases that spec to
   `!((DK *)(intptr_t)k)->consumed` -- the `(DK *)(intptr_t)` cast normalizes k
   whether the case binds it as `int64_t` (re-opening) or `DK *`.
4. All flag-gated on `g_opt_cps_tramp_resume`.

Reusing `CT_LETPRIM` (rather than a new CT node) means every CT analysis pass
(has_capture / collect_caps / first_unsupported / admission) handles it with no
extra surface.

This matches the fiber `cont?` semantics: true while unconsumed, false after a
resume of the same k -- so it is correct for a post-resume `cont?` too, not just
the pre-resume idiom the two fixtures use.

## Context

A residual of the top-level-handle synthesized-main fold
(docs/reported/cps-toplevel-synthesized-main-bypasses-dk.md), which routed these
two into a d2b main and exposed the missing `EX_CONT_PRED` CPS lowering.
