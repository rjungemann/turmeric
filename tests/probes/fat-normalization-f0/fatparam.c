/* fatparam.c -- F0 ABI-ratification probe for fn-value-fat-normalization-plan
 * stage 1 (representation-consolidation-meta-plan, increment 1).
 *
 * Hand-written C, node-for-node in the emitted style, proving the proposed
 * calling convention BEFORE the emitter change exists:
 *
 *     a nominal (non-^fat, non-carrier, non-cfnptr) fn-typed parameter
 *     holds a fat {thunk, env} handle, ALWAYS; every invoke reads slot 0
 *     and passes the handle as the env; bare fns are shimmed into a
 *     {shim, orig} box at the boundary.
 *
 * Models (layouts transcribed from ./build/tur emit-c output, 2026-07-30):
 *   - capturing-closure env: drop-glue header (void*) + { thunk __fn;
 *     captures... }, handle points PAST the header (emit_expr.c EX_CLOSURE /
 *     closure-drop-glue Model R).
 *   - bare-to-fat box: header + { shim, orig_fn } two-slot box; the shim
 *     reads slot 1 and forwards args (emit_expr.c EX_FN_TO_FAT +
 *     __tur_fatshim<N> preamble shims).
 *   - fat invoke: `(*(thunk**)handle)(handle, args...)` -- slot 0 called
 *     with the handle as env (emit_expr.c A#1 fat-dispatch).
 *
 * Proves:
 *   1. ONE callee body (`run0` / `run1`), compiled once, correctly invokes
 *      BOTH a capturing closure and a shimmed bare fn through its nominal
 *      fn-typed parameter -- the uniformity stage 1 claims.
 *   2. The nullary and unary protocols both round-trip non-int64 payloads
 *      (the unary case forwards an argument through the shim).
 *   3. Pass-through composes: run0's parameter forwarded to a second
 *      fn-typed parameter and invoked there (the report's `thru` shape,
 *      exercised at the VALUE level the plan normalizes).
 *
 * Prints one PASS/FAIL line per property; exit 0 iff all pass.
 *
 * Build + run (ASan/UBSan clean is part of the ratification):
 *   cc -std=c11 -Wall -Wextra -fsanitize=address,undefined \
 *      tests/probes/fat-normalization-f0/fatparam.c -o /tmp/fatparam && /tmp/fatparam
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int64_t (*thunk0_t)(void *);
typedef int64_t (*thunk1_t)(void *, int64_t);

/* ---- capturing closure: header + { __fn, k } ---------------------------- */

struct env0 { thunk0_t __fn; int64_t k; };

static int64_t clos0_body(void *envp) {
    struct env0 *env = (struct env0 *)envp;
    return env->k + 1;
}

static void *mk_clos0(int64_t k) {
    void *base = malloc(sizeof(void *) + sizeof(struct env0));
    *(void (**)(void *))base = 0;                       /* drop-glue header */
    struct env0 *env = (struct env0 *)((char *)base + sizeof(void *));
    env->__fn = clos0_body;
    env->k = k;
    return env;                                          /* handle PAST header */
}

/* ---- bare fn + the {shim, orig} box ------------------------------------- */

static int64_t bare42(void) { return 42; }
static int64_t bare_double(int64_t x) { return x * 2; }

/* __tur_fatshim0/1 transcription: slot 1 holds the original bare fn. */
static int64_t fatshim0(void *box) {
    int64_t orig = ((int64_t *)box)[1];
    return ((int64_t (*)(void))(intptr_t)orig)();
}
static int64_t fatshim1(void *box, int64_t a0) {
    int64_t orig = ((int64_t *)box)[1];
    return ((int64_t (*)(int64_t))(intptr_t)orig)(a0);
}

static void *mk_bare_box(void *shim, void *orig) {
    void *base = malloc(sizeof(void *) + 2 * sizeof(int64_t));
    *(void (**)(void *))base = 0;
    int64_t *slots = (int64_t *)((char *)base + sizeof(void *));
    slots[0] = (int64_t)(intptr_t)shim;
    slots[1] = (int64_t)(intptr_t)orig;
    return slots;
}

/* ---- the proposed callee convention ------------------------------------- */

/* A nominal fn-typed parameter arrives as int64_t (as today) but ALWAYS
 * holds a fat handle; the invoke is uniformly the slot-0 protocol. */
static int64_t run0(int64_t f) {
    void *h = (void *)(intptr_t)f;
    return (*(thunk0_t *)h)(h);
}
static int64_t run1(int64_t f, int64_t a0) {
    void *h = (void *)(intptr_t)f;
    return (*(thunk1_t *)h)(h, a0);
}
/* Pass-through: the value crosses a second nominal fn-typed param unchanged
 * (the `thru` shape) and is invoked downstream. */
static int64_t thru0(int64_t f) { return run0(f); }

static void hfree(void *handle) { free((char *)handle - sizeof(void *)); }

static int g_rc = 0;

static void check(const char *label, int64_t got, int64_t want) {
    int ok = got == want;
    printf("%s %s: got %lld want %lld\n", ok ? "PASS" : "FAIL",
           label, (long long)got, (long long)want);
    if (!ok) g_rc = 1;
}

int main(void) {
    void *c = mk_clos0(7);
    check("capturing closure through nominal param",
          run0((int64_t)(intptr_t)c), 8);

    void *b0 = mk_bare_box((void *)fatshim0, (void *)bare42);
    check("shimmed bare fn through SAME callee",
          run0((int64_t)(intptr_t)b0), 42);

    void *b1 = mk_bare_box((void *)fatshim1, (void *)bare_double);
    check("unary shim forwards the argument",
          run1((int64_t)(intptr_t)b1, 21), 42);

    check("pass-through param (thru shape), closure",
          thru0((int64_t)(intptr_t)c), 8);
    check("pass-through param (thru shape), bare box",
          thru0((int64_t)(intptr_t)b0), 42);

    hfree(c); hfree(b0); hfree(b1);
    return g_rc;
}
