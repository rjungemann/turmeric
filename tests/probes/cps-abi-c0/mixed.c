/* =========================================================================
 * CPS-IR-to-C backend, Phase C0 -- ABI de-risking sketch #1 of 2.
 *
 * Throwaway, HAND-WRITTEN C. It is NOT emitted by the compiler; it is the
 * "compile one colored function under the proposed ABI and run it" step the
 * C0 plan calls for, written before the emitter exists so the calling
 * convention is proven rather than assumed.
 *
 * It transcribes tests/fixtures/cps-mixed-coloring/input.tur by hand into the
 * proposed ABI, node-for-node against the `tur check --dump-cps` output. That
 * fixture is chosen because one program exercises all three direct<->CPS edges
 * plus a (syntactically-local) reset/shift and two join points:
 *
 *   (defn twice [x : int] : int          (* 2 x))           ; uncolored (direct)
 *   (defn shift-then-twice [x : int] : int
 *     (twice (reset (shift (fn [v] v) x))))                  ; colored
 *   (defn run [n : int] : int            (+ 1 (shift-then-twice n)))  ; colored
 *   (defn main [] : int (println (run 20)) 0)               ; colored entry
 *
 * Source semantics: reset(shift id 20) = 20, twice => 40, run adds 1 => 41.
 * Expected: prints "41", process exit 0.
 *
 * The --dump-cps IR this file transcribes:
 *
 *   cps-fn shift-then-twice [x] k:cont<int> internal
 *     reset t0 { shift k'. (<prompt> x) }
 *     let t1 = call twice(t0)   ; cps->direct
 *     (k t1)
 *   cps-fn run [n] k:cont<int> internal
 *     letcont j2(t0) = let t1 = (+ 1 t0) in (k t1)
 *     tailcall shift-then-twice(n j2)   ; cps->cps
 *   cps-fn main [] k:cont<int> entry
 *     letcont j2(t1) = let t0 = (println t1) in (k 0)
 *     tailcall run(20 j2)   ; cps->cps
 *
 * ABI under test (the C0 proposal):
 *   - continuation value = DK *   (the cps_prompt.h multi-prompt chain)
 *   - colored `f a b`     = int64_t f(int64_t a, int64_t b, DK *k)   (KK_RET appended)
 *   - CT_TAILCALL f(a,k)  = return f(a, k);                          (cps->cps: thread k)
 *   - CT_APPCONT (k v)    = return dk_run(k, v);                     (deliver to return cont)
 *   - CT_LETCONT j(x)=..  = DK *j = dk_frame(<jbody frame>, env, outer_k);  (join point)
 *   - CT_LETCALL x=g(a)   = ordinary call, g uncolored               (cps->direct)
 *   - direct->cps entry   = seed dk_done() root, call colored fn      (trampoline)
 * ========================================================================= */

#include <stdint.h>
#include <stdio.h>
#include "../../../src/runtime/cps_prompt.h"

/* ---- uncolored, direct-style: (defn twice [x] (* 2 x)) ---------------- */
static int64_t twice(int64_t x) { return 2 * x; }

/* ---- join-point frame bodies (letcont jbody, lowered to a DKFrame) ---- */

/* run's j2(t0) = let t1 = (+ 1 t0) in (k t1)
 * As a DK frame: add 1, then the chain flows into the threaded outer k. */
static intptr_t run_j2(intptr_t env, intptr_t v) { (void)env; return v + 1; }

/* main's j2(t1) = let t0 = (println t1) in (k 0)
 * As a DK frame: println, then deliver 0 (chain flows into outer k = root). */
static intptr_t main_j2(intptr_t env, intptr_t v) {
    (void)env;
    printf("%lld\n", (long long)v);
    return 0;
}

/* ---- shift-then-twice's reset/shift body -----------------------------
 * shift k'. (<prompt> x): the shift ignores the captured sub k' (abortive,
 * matching (fn [v] v) delivering its argument) and delivers x to the prompt.
 * body_env carries x. */
static intptr_t stt_shift_body(intptr_t env, DK *subk) {
    (void)subk;               /* k' captured but not resumed (abortive) */
    return env;               /* deliver x to the prompt's outer continuation */
}

/* ---- colored: shift-then-twice(x, k) --------------------------------- */
static int64_t shift_then_twice(int64_t x, DK *k) {
    /* reset t0 { shift k'. (<prompt> x) }
     * reset = dk_prompt(tag, done); shift = dk_shift(tag, body, x, <chain>). */
    DK *delim = dk_shift(1, stt_shift_body, (intptr_t)x, dk_prompt(1, dk_done()));
    int64_t t0 = (int64_t)dk_run(delim, 0);   /* == x == 20 */
    dk_free(delim);

    int64_t t1 = twice(t0);                   /* cps->direct: ordinary call, == 40 */

    return (int64_t)dk_run(k, (intptr_t)t1);  /* CT_APPCONT (k t1): deliver 40 to k */
}

/* ---- colored: run(n, k) ---------------------------------------------- */
static int64_t run(int64_t n, DK *k) {
    /* letcont j2(t0) = (+ 1 t0) ; (k t1)  -> a frame prepended onto outer k */
    DK *j2 = dk_frame(run_j2, 0, k);
    return shift_then_twice(n, j2);           /* CT_TAILCALL: thread the continuation */
}

/* ---- colored entry: main(k) ------------------------------------------ */
static int64_t tur_main(DK *k) {
    DK *j2 = dk_frame(main_j2, 0, k);
    return run(20, j2);                       /* CT_TAILCALL: thread the continuation */
}

/* ---- direct->cps entry trampoline (uncolored C main enters colored) --- */
int main(void) {
    DK *root = dk_done();                     /* dk_done()-terminated root continuation */
    int64_t r = tur_main(root);               /* == 0 after "41" is printed */
    dk_free(root);
    return (int)r;
}
