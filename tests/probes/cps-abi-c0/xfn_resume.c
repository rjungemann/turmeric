/* =========================================================================
 * CPS-IR-to-C backend, Phase C0 -- ABI de-risking sketch #2 of 2.
 *
 * Throwaway, HAND-WRITTEN C proving the one capability the whole backend
 * exists to unlock, and which no current substrate covers:
 *
 *   a `shift` in a CALLEE, delimited by a `reset` in its CALLER, capturing a
 *   sub-continuation that spans BOTH functions' frames, resumed MULTIPLE
 *   times (multi-shot).
 *
 * The syntactically-local emit_cps_reset rejects exactly this shape (the shift
 * and its reset live in different functions), so it is the load-bearing claim
 * the C0 convention has to support. Sketch #1 covers the boundary edges; this
 * one covers cross-function resumable capture under the same DK-threading ABI.
 *
 * Modelled program (in the ABI, no direct-style equivalent needed):
 *
 *   caller: reset tag 1 around  ( callee() * 2 )        ; `* 2` frame is caller's
 *   callee: ( shift k. (k 1) + (k 10) ) + 100           ; `+ 100` frame is callee's
 *
 * The captured continuation k = \v. ((v + 100) * 2), re-delimited. Resuming it
 * from the shift body:
 *   k(1)  = (1  + 100) * 2 = 202
 *   k(10) = (10 + 100) * 2 = 220
 *   sum   = 422    -- delivered to the prompt's outer continuation (caller's ret).
 *
 * The point: `+ 100` lives in callee, `* 2` lives in caller, yet a single
 * captured DK chain threads through both -- because caller passed callee a
 * continuation `k` that already carried caller's post-call frame(s) and the
 * prompt. That is precisely CT_TAILCALL threading `k` across the call ABI.
 *
 * Expected: prints "422", process exit 0.
 * ========================================================================= */

#include <stdint.h>
#include <stdio.h>
#include "../../../src/runtime/cps_prompt.h"

/* callee's post-shift frame: (+ 100) -- part of the captured continuation. */
static intptr_t add100(intptr_t env, intptr_t v) { (void)env; return v + 100; }

/* caller's post-call frame: (* 2) -- also part of the captured continuation,
 * sitting BELOW callee's frame in the chain (closer to the prompt). */
static intptr_t times2(intptr_t env, intptr_t v) { (void)env; return v * 2; }

/* callee's shift body: resume the captured sub-continuation twice and sum.
 * This is the multi-shot, cross-function resume dk_invoke gives for free. */
static intptr_t resume_twice(intptr_t env, DK *subk) {
    (void)env;
    intptr_t a = dk_invoke(subk, 1);    /* (1  + 100) * 2 = 202 */
    intptr_t b = dk_invoke(subk, 10);   /* (10 + 100) * 2 = 220 */
    return a + b;                        /* 422 -> prompt's outer continuation */
}

/* ---- colored: callee(k) --------------------------------------------- *
 * k is the continuation caller threaded in: [times2] -> prompt(1) -> ret.
 * callee prepends its own post-shift frame (add100), then shifts on tag 1.
 * The shift captures [add100, times2] + a re-installed prompt (multi-shot). */
static int64_t callee(DK *k) {
    DK *k2 = dk_frame(add100, 0, k);
    DK *chain = dk_shift(1, resume_twice, 0, k2);
    int64_t r = (int64_t)dk_run(chain, 0);   /* seed 0 is consumed by the shift */
    dk_free(chain);                          /* frees the whole threaded chain incl. root */
    return r;
}

/* ---- colored: caller(k) --------------------------------------------- *
 * Installs the reset (prompt tag 1) and threads a continuation carrying its
 * own post-call frame (times2) above the prompt, then tail-calls callee. */
static int64_t caller(DK *ret) {
    DK *k = dk_frame(times2, 0, dk_prompt(1, ret));
    return callee(k);                        /* CT_TAILCALL: thread k across the call */
}

/* ---- direct->cps entry trampoline ----------------------------------- */
int main(void) {
    DK *root = dk_done();
    int64_t r = caller(root);                /* == 422; callee freed root as part
                                              * of its chain, so do not free here. */
    printf("%lld\n", (long long)r);
    return 0;
}
