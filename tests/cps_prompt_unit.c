/* CPS5 (cps-transform-plan): unit tests for the multi-prompt delimited-control
 * machine (src/runtime/cps_prompt.c).
 *
 * Programs are written as CPS continuation chains (head = innermost). Each test
 * checks a delimited-control law on the substrate:
 *   - CPS5.1: reset/shift capture + compose; shift vs shift0 re-install.
 *   - CPS5.2: a captured sub-continuation is multi-shot (invoke N times).
 *   - CPS5.3: an implicit root prompt lets an undelimited capture reach entry.
 *
 * Prints PASS/FAIL; exits nonzero on any failure.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "cps_prompt.h"

#define TAG 1

static int g_pass = 0, g_fail = 0;
static void check(int cond, const char *name, const char *msg) {
    if (cond) { g_pass++; printf("PASS %s\n", name); }
    else      { g_fail++; printf("FAIL %s -- %s\n", name, msg); }
}

/* ---- frames ---- */
static intptr_t add1(intptr_t env, intptr_t v) { (void)env; return v + 1; }
static intptr_t mul2(intptr_t env, intptr_t v) { (void)env; return v * 2; }

/* ---- shift bodies ---- */

/* reset { 1 + shift(k => 2 + k(k(3))) }  -- k = (1 + []).  k(3)=4, k(4)=5 => 7 */
static intptr_t body_2_plus_kk3(intptr_t env, DK *sub) {
    (void)env;
    intptr_t a = dk_invoke(sub, 3);     /* 4 */
    intptr_t b = dk_invoke(sub, a);     /* 5 */
    return 2 + b;                       /* 7 */
}

/* reset { shift(k => k(10) + k(20)) * 2 }  -- multi-shot: k=( []*2 ) => 20+40=60 */
static intptr_t body_k10_plus_k20(intptr_t env, DK *sub) {
    (void)env;
    return dk_invoke(sub, 10) + dk_invoke(sub, 20); /* 20 + 40 = 60 */
}

/* probe whether the captured sub-cont carries a re-installed prompt */
static bool g_sub_has_prompt;
static intptr_t body_probe(intptr_t env, DK *sub) {
    (void)env;
    g_sub_has_prompt = dk_has_prompt(sub);
    return 0;
}

/* root capture: shift to ROOT with no enclosing reset; k = (1 + []) up to entry.
 * k(100) = 101; + 1000 = 1101. */
static intptr_t body_root(intptr_t env, DK *sub) {
    (void)env;
    return dk_invoke(sub, 100) + 1000;
}

/* ---- effect handler cases (F2: deep vs shallow) ---- */

/* Probe whether dk_perform re-installed the handler on the captured sub: a
 * deep handler's sub carries a fresh TAG handler (a re-perform re-finds it); a
 * shallow handler's sub does not. */
static bool g_handler_reinstalled;
static intptr_t handler_probe(intptr_t env, intptr_t arg, DK *sub) {
    (void)env; (void)arg;
    g_handler_reinstalled = dk_has_handler(sub, TAG);
    return 42;
}

int main(void) {
    /* ---- CPS5.1: reset/shift capture + compose ---- */
    {
        DK *chain = dk_shift(TAG, body_2_plus_kk3, 0,
                       dk_frame(add1, 0,
                         dk_prompt(TAG, dk_done())));
        intptr_t r = dk_run(chain, 0);
        check(r == 7, "cps5-shift-compose", "reset/shift(k(k(3))) != 7");
        dk_free(chain);
    }

    /* ---- CPS5.2: multi-shot sub-continuation ---- */
    {
        DK *chain = dk_shift(TAG, body_k10_plus_k20, 0,
                       dk_frame(mul2, 0,
                         dk_prompt(TAG, dk_done())));
        intptr_t r = dk_run(chain, 0);
        check(r == 60, "cps5-multishot", "k(10)+k(20) over []*2 != 60");
        dk_free(chain);
    }

    /* ---- CPS5.1: shift RE-INSTALLS the prompt on the captured sub ---- */
    {
        g_sub_has_prompt = false;
        DK *chain = dk_shift(TAG, body_probe, 0,
                       dk_frame(add1, 0, dk_prompt(TAG, dk_done())));
        dk_run(chain, 0);
        check(g_sub_has_prompt, "cps5-shift-reinstall",
              "shift did not re-install the prompt on the sub-continuation");
        dk_free(chain);
    }

    /* ---- CPS5.1: shift0 does NOT re-install the prompt ---- */
    {
        g_sub_has_prompt = true;
        DK *chain = dk_shift0(TAG, body_probe, 0,
                        dk_frame(add1, 0, dk_prompt(TAG, dk_done())));
        dk_run(chain, 0);
        check(!g_sub_has_prompt, "cps5-shift0-no-reinstall",
              "shift0 wrongly re-installed the prompt");
        dk_free(chain);
    }

    /* ---- CPS5.3: implicit root prompt (undelimited capture to entry) ---- */
    {
        /* no explicit reset; shift targets ROOT */
        DK *chain = dk_shift(DK_ROOT_TAG, body_root, 0,
                       dk_frame(add1, 0, dk_done()));
        intptr_t r = dk_run_root(chain, 0);
        check(r == 1101, "cps5-root-prompt",
              "undelimited capture to the implicit root prompt != 1101");
        dk_free(chain);
    }

    /* ---- F2: a DEEP handler re-installs itself on the captured sub ---- */
    {
        g_handler_reinstalled = false;
        DK *k = dk_frame(add1, 0, dk_handler(TAG, handler_probe, 0, dk_done()));
        intptr_t r = dk_perform(TAG, 0, k);
        check(g_handler_reinstalled, "dk-deep-handler-reinstall",
              "deep dk_perform did not re-install the handler on the sub");
        check(r == 42, "dk-deep-handler-result", "deep handler result != 42");
        dk_free(k);
    }

    /* ---- F2: a SHALLOW handler does NOT re-install itself (shift0 twin) ---- */
    {
        g_handler_reinstalled = true;
        DK *k = dk_frame(add1, 0, dk_handler_shallow(TAG, handler_probe, 0, dk_done()));
        intptr_t r = dk_perform(TAG, 0, k);
        check(!g_handler_reinstalled, "dk-shallow-handler-no-reinstall",
              "shallow dk_perform wrongly re-installed the handler on the sub");
        check(r == 42, "dk-shallow-handler-result", "shallow handler result != 42");
        dk_free(k);
    }

    printf("cps_prompt summary: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
