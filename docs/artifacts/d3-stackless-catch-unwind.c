/* D3 prototype -- stackless (trampolined) lowering of nested catch-unwind.
 *
 * Reference artifact for docs/archive/history/compiled-catch-unwind-stackless-plan.md.
 * This is NOT compiled into tur or the test suite; it is a hand-lowered model of
 * what a selective-CPS lowering (direction (a) in the D3 plan) would emit, used
 * to de-risk the codegen work the way the D1a hand-prototypes did.  Build:
 *
 *     gcc -O2 docs/artifacts/d3-stackless-catch-unwind.c -o /tmp/d3proto
 *     /tmp/d3proto 1000000
 *     ( ulimit -s 64; /tmp/d3proto 1000000 )   # proves C-stack independence
 *
 * It models two turmeric shapes, integrating the shipped D1 heap handler chain
 * and the D1a tur_panicking signal so the lowering is representative:
 *
 *   (defn cu-rec [n] (if (= n 0) 0 (do (catch-unwind (fn [] (cu-rec (- n 1)))) n)))
 *   (defn cu-rec-p [n] (if (= n 0) 0
 *                        (do (catch-unwind (fn [] (panic "x"))) (+ 1 (cu-rec-p (- n 1))))))
 *
 * The wall D1 and D1a both hit is frame COUNT: each nesting level keeps two live
 * C frames -- the catch boundary's own frame and the "after the catch"
 * continuation (`... n`, `1 + ...`).  Here both move onto a heap continuation
 * chain driven by one flat trampoline frame, so depth is bounded by heap, not
 * the native stack.  Measured: both shapes run 20,000,000 deep, and 1,000,000
 * deep under a 64 KiB stack ulimit -- categorical C-stack independence.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

/* --- D1 handler chain + D1a signal (same shape as the compiled runtime) --- */
typedef struct HNode { struct HNode *parent; } HNode;
static __thread HNode   *g_handlers = NULL;
static __thread int      tur_panicking = 0;
static __thread int64_t  g_payload = 0;

static void do_panic(int64_t p) {            /* D1a: set flag + stage payload, return */
    if (g_handlers) { g_payload = p; tur_panicking = 1; return; }
    fprintf(stderr, "uncaught panic\n"); abort();
}

/* --- heap continuation chain (the piece D3 adds) --- */
typedef enum { K_DONE, K_CUREC_AFTER, K_ADD1 } KTag;
typedef struct Cont {
    KTag          tag;
    int64_t       n;          /* K_CUREC_AFTER: value to return after the catch */
    HNode        *boundary;   /* catch boundary to pop when this cont resumes    */
    struct Cont  *next;
} Cont;

typedef enum { A_DESCEND, A_RETURN } Act;

/* cu-rec: nested catch-unwind, no panic (the depth-wall shape). */
static int64_t run_cu_rec(int64_t n0) {
    Cont *done = (Cont*)malloc(sizeof(Cont)); done->tag = K_DONE; done->next = NULL;
    Act act = A_DESCEND; int64_t n = n0; Cont *k = done; int64_t v = 0;
    for (;;) {
        if (act == A_DESCEND) {
            if (n == 0) { v = 0; act = A_RETURN; continue; }
            /* catch-unwind(fn(): cu-rec(n-1)); then return n */
            HNode *b = (HNode*)malloc(sizeof(HNode)); b->parent = g_handlers; g_handlers = b;
            Cont *k2 = (Cont*)malloc(sizeof(Cont));
            k2->tag = K_CUREC_AFTER; k2->n = n; k2->boundary = b; k2->next = k;
            n = n - 1; k = k2;                 /* DESCEND(n-1, k2) */
        } else {                               /* A_RETURN v -> k */
            if (k->tag == K_DONE) { int64_t r = v; free(k); return r; }
            /* K_CUREC_AFTER: the catch-unwind of the thunk completed with v */
            g_handlers = k->boundary->parent; free(k->boundary);
            if (tur_panicking) { tur_panicking = 0; }   /* consume (none here) */
            int64_t out = k->n; Cont *kk = k->next; free(k);
            v = out; k = kk;                   /* RETURN(out, kk) */
        }
    }
}

/* cu-rec-p: each level catches a real per-level panic (D1a signal) AND recurses
 * non-tail (1 + ...), all stackless. */
static int64_t run_cu_rec_p(int64_t n0) {
    Cont *done = (Cont*)malloc(sizeof(Cont)); done->tag = K_DONE; done->next = NULL;
    Act act = A_DESCEND; int64_t n = n0; Cont *k = done; int64_t v = 0;
    for (;;) {
        if (act == A_DESCEND) {
            if (n == 0) { v = 0; act = A_RETURN; continue; }
            /* catch-unwind(fn(): (panic "x")) -- thunk panics inline, caught here */
            HNode *b = (HNode*)malloc(sizeof(HNode)); b->parent = g_handlers; g_handlers = b;
            do_panic(42);                                   /* the thunk body */
            g_handlers = b->parent; free(b);
            if (tur_panicking) { tur_panicking = 0; }        /* catch consumes -> (err) */
            Cont *k2 = (Cont*)malloc(sizeof(Cont));
            k2->tag = K_ADD1; k2->next = k;
            n = n - 1; k = k2;                               /* DESCEND(n-1, k2) */
        } else {
            if (k->tag == K_DONE) { int64_t r = v; free(k); return r; }
            /* K_ADD1: return 1 + v */
            int64_t out = 1 + v; Cont *kk = k->next; free(k);
            v = out; k = kk;
        }
    }
}

int main(int argc, char **argv) {
    int64_t d = argc > 1 ? atoll(argv[1]) : 1000000;
    printf("cu-rec(%lld)   = %lld\n", (long long)d, (long long)run_cu_rec(d));
    printf("cu-rec-p(%lld) = %lld\n", (long long)d, (long long)run_cu_rec_p(d));
    return 0;
}
