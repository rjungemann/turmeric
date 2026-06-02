/* CPS4 (cps-transform-plan): unit tests for the heap-reified continuation +
 * trampoline runtime (src/runtime/cps_rt.c).
 *
 * Exercises:
 *   - CPS4.1 / CPS4.4: capture (O(1) pointer take) + resume of a continuation
 *     whose depth is far beyond the old 16-frame fiber ceiling.
 *   - CPS4.2: the trampoline runs an arbitrarily deep chain in O(1) native
 *     stack (a recursive resume of this depth would overflow).
 *   - CPS4.3: per-frame defer fires exactly once on normal resume, exactly once
 *     on abort/free, and is never double-fired.
 *
 * Prints PASS/FAIL lines; exits nonzero if any check fails.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "cps_rt.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(int cond, const char *name, const char *msg) {
    if (cond) { g_pass++; printf("PASS %s\n", name); }
    else      { g_fail++; printf("FAIL %s -- %s\n", name, msg); }
}

/* defer that bumps a counter through its ctx. */
static void bump(void *ctx) { (*(long *)ctx)++; }

/* Build an N-frame "add 1" chain; element 0 is the head (resumed first). The
 * chain is linked head -> ... -> tail(next=NULL). Each frame carries `defer`
 * bumping *counter. Returns the head. */
static TurKont *build_chain(long n, long *counter) {
    TurKont *next = NULL;
    for (long i = 0; i < n; i++)
        next = tur_kont_new(tur_kont_add_fn, /*env=*/1, next, bump, counter);
    return next; /* last allocated is the head */
}

int main(void) {
    /* ---- CPS4.1 / CPS4.4 / CPS4.2: deep capture + constant-stack resume ---- */
    {
        const long N = 500000; /* >> 16; a recursive resume would blow the stack */
        long defers = 0;
        TurKont *k = build_chain(N, &defers);

        /* "capture" is just taking the pointer (O(1), unbounded). */
        TurKont *captured = k;

        intptr_t result = tur_trampoline(captured, 0);
        check(result == (intptr_t)N, "cps4-deep-resume",
              "deep chain did not sum to N");
        check(defers == N, "cps4-deep-defer-once",
              "each frame's defer did not fire exactly once on resume");

        tur_kont_free_chain(k); /* already-fired defers must not re-fire */
        check(defers == N, "cps4-free-no-refire",
              "freeing a resumed chain re-fired defers");
    }

    /* ---- CPS4.3: abort path fires defers exactly once ---- */
    {
        const long N = 1000;
        long defers = 0;
        TurKont *k = build_chain(N, &defers);
        /* abort without resuming: free should fire every defer once. */
        tur_kont_free_chain(k);
        check(defers == N, "cps4-abort-defer-once",
              "abort/free did not fire each defer exactly once");
    }

    /* ---- CPS4.3: explicit double-fire guard ---- */
    {
        long defers = 0;
        TurKont *k = tur_kont_new(tur_kont_add_fn, 7, NULL, bump, &defers);
        tur_kont_fire_defer(k);
        tur_kont_fire_defer(k); /* second call is a no-op */
        check(defers == 1, "cps4-defer-idempotent",
              "fire_defer was not idempotent");
        tur_kont_free_chain(k);
        check(defers == 1, "cps4-defer-idempotent-free",
              "free re-fired an already-fired defer");
    }

    /* ---- small functional check: a short chain resumes to the right value ---- */
    {
        long defers = 0;
        TurKont *k = build_chain(5, &defers); /* +1 five times */
        intptr_t r = tur_trampoline(k, 100);
        check(r == 105, "cps4-small-resume", "short chain miscomputed");
        tur_kont_free_chain(k);
    }

    printf("cps_rt summary: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
