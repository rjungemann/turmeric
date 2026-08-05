/* E2 KILL-PROBE (docs/upcoming/v2/cps-dk-sole-effect-lowering-plan.md Sec 9)
 *
 * Question: can a fat-closure fn-value carry a DK*-threading thunk so that
 *   (a) an effect performed inside an INDIRECTLY-called effectful callback
 *       reaches the CALLER's DK handler (handler search crosses the fn-value), and
 *   (b) a deep (1e6) tail-resume recursion through that callback stays FLAT
 *       (no O(N) C stack) -- "the whole point of DK".
 *
 * The DK runtime below is copied VERBATIM from src/compiler/emit_dk_runtime.c
 * (emit_cps_runtime_prelude) so the probe tests the real substrate, not a model.
 *
 * We build the SAME scenario two ways:
 *   run_naive : the current DK resume model -- dk_perform calls the handler which
 *               calls dk_run(sub, v) INLINE. Each iteration nests. Expect O(N).
 *   run_tramp : a trampolined TAIL-resume -- perform unwinds to a driver loop that
 *               re-enters the resumed chain from the top. Expect FLAT.
 *
 * In BOTH, the effectful callback is reached through a fat-closure box carrying a
 * second thunk slot  int64_t (*fn_cps)(void *env, int64_t arg, DK *__kont)  -- the
 * exact E2 ABI addition. The callback's body performs Tick against __kont, so the
 * perform's handler search walks the caller-provided chain: (a) is proven if the
 * results are correct; (b) is measured by the C-stack address delta.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <setjmp.h>

/* ============================ DK runtime (verbatim) ====================== */
#define DK_ROOT_TAG 0
typedef struct DK DK;
typedef intptr_t (*DKFrame)(intptr_t env, intptr_t value);
typedef intptr_t (*DKBody)(intptr_t env, DK *subk);
typedef intptr_t (*DKHandler)(intptr_t env, intptr_t arg, DK *subk);
typedef intptr_t (*DKResumeFrame)(intptr_t env, intptr_t value, DK *rest);
typedef enum { DKK_DONE, DKK_FRAME, DKK_PROMPT, DKK_SHIFT, DKK_SHIFT0, DKK_HANDLER, DKK_RESUME_FRAME } DKKind;
struct DK {
    DKKind kind; DKFrame fn; intptr_t env; int tag;
    DKBody body; intptr_t body_env;
    DKHandler handler; intptr_t handler_env; bool shallow;
    DKResumeFrame rfn; DK *next;
};
static DK *dk_new(DKKind kind, DK *next) {
    DK *k = (DK *)calloc(1, sizeof(DK)); k->kind = kind; k->next = next; return k;
}
static DK *dk_done(void) { return dk_new(DKK_DONE, NULL); }
static DK *dk_frame(DKFrame fn, intptr_t env, DK *next) {
    DK *k = dk_new(DKK_FRAME, next); k->fn = fn; k->env = env; return k;
}
static DK *dk_prompt(int tag, DK *next) {
    DK *k = dk_new(DKK_PROMPT, next); k->tag = tag; return k;
}
static DK *dk_handler_impl(int tag, DKHandler fn, intptr_t env, bool shallow, DK *next) {
    DK *k = dk_new(DKK_HANDLER, next); k->tag = tag; k->handler = fn; k->handler_env = env; k->shallow = shallow; return k;
}
static DK *dk_handler(int tag, DKHandler fn, intptr_t env, DK *next) {
    return dk_handler_impl(tag, fn, env, false, next);
}
static DK *dk_copy_node(const DK *n) {
    DK *c = dk_new(n->kind, NULL); c->fn = n->fn; c->env = n->env; c->tag = n->tag;
    c->body = n->body; c->body_env = n->body_env;
    c->handler = n->handler; c->handler_env = n->handler_env; c->shallow = n->shallow;
    c->rfn = n->rfn; return c;
}
static DK *dk_copy_enclosing_handlers(const DK *from) {
    DK *head = NULL, *tail = NULL;
    for (const DK *p = from; p && p->kind != DKK_DONE; p = p->next) {
        if (p->kind != DKK_HANDLER) continue;
        DK *c = dk_copy_node(p);
        if (!head) head = tail = c; else { tail->next = c; tail = c; }
    }
    DK *done = dk_done();
    if (!head) return done;
    tail->next = done; return head;
}
static DK *dk_copy_range(const DK *from, const DK *stop) {
    DK *head = NULL, *tail = NULL;
    for (const DK *p = from; p && p != stop; p = p->next) {
        DK *c = dk_copy_node(p);
        if (!head) head = tail = c; else { tail->next = c; tail = c; }
    }
    return head;
}
static DK *dk_append(DK *a, DK *b) {
    if (!a) return b;
    DK *p = a;
    while (p->next) p = p->next;
    p->next = b;
    return a;
}
static void dk_free(DK *k) { while (k) { DK *n = k->next; free(k); k = n; } }
__attribute__((unused)) static void dk_free_node(DK *k) { free(k); }

static intptr_t dk_run_impl(DK *k, intptr_t v, bool root) {
    while (k) {
        switch (k->kind) {
            case DKK_DONE: return v;
            case DKK_PROMPT: case DKK_HANDLER: k = k->next; break;
            case DKK_FRAME: v = k->fn(k->env, v); k = k->next; break;
            case DKK_RESUME_FRAME: return k->rfn(k->env, v, k->next);
            case DKK_SHIFT:
            case DKK_SHIFT0: {
                DK *P = k->next;
                while (P && !(P->kind == DKK_PROMPT && P->tag == k->tag)
                         && P->kind != DKK_DONE) P = P->next;
                bool to_root = (!P || P->kind == DKK_DONE);
                bool reinstall = (k->kind == DKK_SHIFT);
                DK *sub = dk_copy_range(k->next, P);
                DK *tail = reinstall
                    ? dk_prompt(to_root ? DK_ROOT_TAG : k->tag, dk_done()) : dk_done();
                sub = dk_append(sub, tail);
                intptr_t bodyval = k->body(k->body_env, sub);
                dk_free(sub);
                if (to_root) return bodyval;
                k = P->next; v = bodyval; break;
            }
        }
    }
    (void)root; return v;
}
static intptr_t dk_run(DK *k, intptr_t v)      { return dk_run_impl(k, v, false); }
static intptr_t dk_invoke(DK *sub, intptr_t w) {
    DK *c = dk_copy_range(sub, NULL); intptr_t r = dk_run_impl(c, w, false);
    dk_free(c); return r;
}
static intptr_t dk_perform(int tag, intptr_t arg, DK *k) {
    DK *H = k;
    while (H && !(H->kind == DKK_HANDLER && H->tag == tag) && H->kind != DKK_DONE) H = H->next;
    if (!H || H->kind == DKK_DONE) { fprintf(stderr, "tur: unhandled effect (tag %d)\n", tag); abort(); }
    DK *sub = dk_copy_range(k, H);
    DK *tail;
    if (H->shallow) {
        tail = dk_copy_enclosing_handlers(H->next);
    } else {
        const DK *ge = H;
        while (ge && ge->kind == DKK_HANDLER) ge = ge->next;
        tail = dk_append(dk_copy_range(H, ge), dk_copy_enclosing_handlers(ge));
    }
    sub = dk_append(sub, tail);
    intptr_t r = H->handler(H->handler_env, arg, sub);
    dk_free(sub);
    return dk_run_impl(H->next, r, false);
}

/* ==================== fat-closure fn-value ABI (E2 shape) ================= */
/* The current fn-value carrier ABI is {env, fn(env,int64)}. E2 adds fn_cps. */
typedef struct {
    void   *env;
    int64_t (*fn)(void *env, int64_t arg);                 /* legacy carrier slot */
    int64_t (*fn_cps)(void *env, int64_t arg, DK *__kont); /* E2: DK*-threading slot */
} fatfn;

/* ============================ stack-depth probe ========================== */
static char *g_stack_base = NULL;
static long  g_max_depth  = 0;
static inline void stack_note(void) {
    char probe;
    long d = g_stack_base - &probe;      /* stack grows down on this platform */
    if (d < 0) d = -d;
    if (d > g_max_depth) g_max_depth = d;
}

/* effect tag */
#define TICK_TAG 1

/* ============ the effectful callback, reached via the fn-value ============ */
/* callback(x) performs (Tick x); the handler resumes with x+1. Its ONLY channel
 * to the enclosing handler is the __kont threaded in by the indirect caller. */
static int64_t g_side_sum = 0;   /* observable side effect to prove it ran */
static int64_t cb_fn_cps(void *env, int64_t x, DK *__kont) {
    (void)env;
    /* tail perform: continuation is __kont as-is (the caller's after-frame chain) */
    return dk_perform(TICK_TAG, x, __kont);
}
static int64_t cb_fn(void *env, int64_t x) { (void)env; return x + 1; } /* legacy */

/* ======================= VERSION A: naive recursive ====================== */
/* run_naive(f, n, k): for i in n..1 apply f(i) (effectful), tail-recurse. */
typedef struct { fatfn *f; int64_t n; DK *k; } RunEnv;
static int64_t naive_after(intptr_t env_i, intptr_t v);

static int64_t run_naive_cps(fatfn *f, int64_t n, DK *k) {
    stack_note();
    if (n == 0) return dk_run(k, 0);
    /* continuation after the callback's effect resumes: run_naive(f, n-1, k) */
    RunEnv *e = (RunEnv *)malloc(sizeof(RunEnv));
    e->f = f; e->n = n; e->k = k;
    DK *cont = dk_frame(naive_after, (intptr_t)e, k);
    /* INDIRECT effectful call through the fn-value's DK* slot (E2) */
    return f->fn_cps(f->env, n, cont);
}
static int64_t naive_after(intptr_t env_i, intptr_t v) {
    RunEnv *e = (RunEnv *)env_i;
    g_side_sum += v;                       /* observe the resumed value (x+1) */
    fatfn *f = e->f; int64_t n = e->n; DK *k = e->k;
    return run_naive_cps(f, n - 1, k);     /* TAIL recursion (nested via resume) */
}
/* tail-resume handler: resume(k, x+1). In the naive model this calls dk_run inline. */
static int64_t tick_handler_naive(intptr_t env, intptr_t arg, DK *subk) {
    (void)env;
    return dk_invoke(subk, arg + 1);       /* resume with x+1 */
}

/* ===================== VERSION B: trampolined tail-resume ================= */
/* The insight: a TAIL-resume handler need not nest. Represent "perform reached a
 * tail-resume handler" by unwinding to a driver loop that re-enters the resumed
 * chain from the top. We implement this with a tiny yield channel: the callback's
 * perform, when the nearest handler is a registered tail-resume handler, does NOT
 * recurse -- it records (resume_chain, resume_value) and longjmps to the driver.
 * The driver re-enters dk_run on the resumed chain. Stack stays O(1) per step. */
static jmp_buf g_tramp_jb;
static DK     *g_tramp_resume_chain = NULL;
static int64_t g_tramp_resume_val   = 0;
static bool    g_tramp_active       = false;

/* trampolining perform: same handler search as dk_perform, but a tail-resume
 * handler yields to the driver instead of calling dk_run inline. */
static int64_t dk_perform_tramp(int tag, intptr_t arg, DK *k) {
    DK *H = k;
    while (H && !(H->kind == DKK_HANDLER && H->tag == tag) && H->kind != DKK_DONE) H = H->next;
    if (!H || H->kind == DKK_DONE) { fprintf(stderr, "tur: unhandled effect (tag %d)\n", tag); abort(); }
    /* deep, single-case tail-resume: the resumed continuation is
     * copy_range(k,H) ++ reinstall(H) ++ enclosing, exactly as dk_perform builds,
     * and the "handler action" for a tail-resume is just value := transform(arg).
     * Here transform(arg) = arg+1 (the Tick case). */
    DK *sub = dk_copy_range(k, H);
    const DK *ge = H;
    while (ge && ge->kind == DKK_HANDLER) ge = ge->next;
    DK *tail = dk_append(dk_copy_range(H, ge), dk_copy_enclosing_handlers(ge));
    sub = dk_append(sub, tail);
    /* yield to the driver: do NOT call dk_run here (that is what nests) */
    g_tramp_resume_chain = sub;
    g_tramp_resume_val   = arg + 1;         /* the tail-resume transform */
    longjmp(g_tramp_jb, 1);
    return 0; /* unreachable */
}
static int64_t cb_fn_cps_tramp(void *env, int64_t x, DK *__kont) {
    (void)env;
    return dk_perform_tramp(TICK_TAG, x, __kont);
}

static int64_t tramp_after(intptr_t env_i, intptr_t v);
static int64_t run_tramp_cps(fatfn *f, int64_t n, DK *k) {
    stack_note();
    if (n == 0) return dk_run(k, 0);
    RunEnv *e = (RunEnv *)malloc(sizeof(RunEnv));
    e->f = f; e->n = n; e->k = k;
    DK *cont = dk_frame(tramp_after, (intptr_t)e, k);
    return f->fn_cps(f->env, n, cont);      /* fn_cps = cb_fn_cps_tramp */
}
static int64_t tramp_after(intptr_t env_i, intptr_t v) {
    RunEnv *e = (RunEnv *)env_i;
    g_side_sum += v;
    fatfn *f = e->f; int64_t n = e->n; DK *k = e->k;
    free(e);
    return run_tramp_cps(f, n - 1, k);
}
/* driver: re-enters the resumed chain from the top after each perform-yield. */
static int64_t run_tramp_driver(fatfn *f, int64_t n, DK *k0) {
    if (setjmp(g_tramp_jb) == 0) {
        g_tramp_active = true;
        return run_tramp_cps(f, n, k0);     /* first entry */
    }
    /* landed here from a perform-yield: resume the captured chain, flat */
    int64_t v;
    for (;;) {
        DK *chain = g_tramp_resume_chain;
        int64_t rv = g_tramp_resume_val;
        if (setjmp(g_tramp_jb) == 0) {
            v = dk_run(chain, rv);          /* run one slice to the next perform/done */
            dk_free(chain);
            break;                          /* reached DONE: no further yield */
        }
        dk_free(chain);                     /* yielded again: loop, flat */
    }
    return v;
}

/* ================================ main ================================== */
int main(void) {
    char anchor;
    g_stack_base = &anchor;

    long N = 1000000;   /* 1e6-deep, per plan Sec 8 */

    /* ---- Version B: trampolined tail-resume (expected FLAT + correct) ---- */
    g_side_sum = 0; g_max_depth = 0;
    fatfn fB = { NULL, cb_fn, cb_fn_cps_tramp };
    DK *kB = dk_handler(TICK_TAG, tick_handler_naive /*unused in tramp*/, 0, dk_done());
    int64_t rB = run_tramp_driver(&fB, N, kB);
    long depthB = g_max_depth;
    /* expected side sum: sum_{i=1..N} (i+1) = N(N+1)/2 + N */
    long long expect = (long long)N * (N + 1) / 2 + N;
    printf("[tramp] N=%ld result=%lld side_sum=%lld expect=%lld  max_stack_bytes=%ld\n",
           N, (long long)rB, (long long)g_side_sum, expect, depthB);
    bool tramp_correct = (g_side_sum == expect);
    bool tramp_flat    = (depthB < 65536);   /* O(1)-ish: under 64 KiB regardless of N */

    /* ---- Version A: naive recursive resume (expected O(N) -> crash) ---- *
     * Run at a SMALL N first to measure the per-iteration stack slope, so we can
     * report the growth rate without necessarily crashing the probe. */
    long depths[2]; long Ns[2] = { 2000, 4000 };
    for (int t = 0; t < 2; t++) {
        g_side_sum = 0; g_max_depth = 0;
        fatfn fA = { NULL, cb_fn, cb_fn_cps };
        DK *kA = dk_handler(TICK_TAG, tick_handler_naive, 0, dk_done());
        int64_t rA = run_naive_cps(&fA, Ns[t], kA);
        depths[t] = g_max_depth;
        printf("[naive] N=%ld result=%lld  max_stack_bytes=%ld\n", Ns[t], (long long)rA, g_max_depth);
    }
    double slope = (double)(depths[1] - depths[0]) / (double)(Ns[1] - Ns[0]);
    printf("[naive] per-iteration stack growth ~= %.1f bytes/element -> at N=1e6 ~= %.1f MB\n",
           slope, slope * 1e6 / (1024.0 * 1024.0));

    printf("\n=== VERDICT ===\n");
    printf("(a) handler search crosses the fn-value __fn_cps slot: %s\n",
           tramp_correct ? "YES (results correct)" : "NO");
    printf("(b) deep (1e6) recursion stays flat (trampolined tail-resume): %s\n",
           tramp_flat ? "YES" : "NO");
    printf("(b') naive recursive resume is O(N) stack: %s (%.1f B/elt)\n",
           slope > 1.0 ? "YES (confirmed -- why the current DK backend evicts it)" : "inconclusive",
           slope);

    return (tramp_correct && tramp_flat) ? 0 : 1;
}
