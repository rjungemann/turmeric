/* E7 FULL-FIDELITY PROBE -- trampolined tail-resume against the REAL dk_perform
 * handle-chain layout (docs/archive/cps-dk-sole-effect-lowering-plan.md E7).
 *
 * The e2-killprobe proved the core mechanism but simplified the chain (handler on
 * dk_done, no handle-continuation frame). This probe reproduces the ACTUAL layout
 * emit_handle builds:
 *     __h = dk_handler(tag, case, caseenv, dk_frame(kname, hkenv, enclosing))
 * i.e. HANDLER(tag) -> FRAME(kname = handle continuation) -> enclosing handlers,
 * and drives it through a trampoline that preserves the FRAME(kname) delivery
 * (dk_perform's post-handler `dk_run_impl(H->next, r)`).
 *
 * It tests, at N = 1e6:
 *   1. deep tail-recursion flat (stack constant),
 *   2. the handle-continuation frame runs exactly once with the body's result,
 *   3. an ENCLOSING handle for a DIFFERENT effect still catches an effect the inner
 *      handle does not handle (propagation through the appended enclosing markers),
 *   4. correctness of accumulated side effects and the final delivered value.
 *
 * Runtime below is verbatim from src/compiler/emit_dk_runtime.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <setjmp.h>

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
    DK *p = a; while (p->next) p = p->next; p->next = b; return a;
}
static void dk_free(DK *k) { while (k) { DK *n = k->next; free(k); k = n; } }

/* stack probe */
static char *g_base = NULL; static long g_max = 0;
static inline void note(void){ char p; long d = g_base-&p; if(d<0)d=-d; if(d>g_max)g_max=d; }

/* ---- trampoline: heap meta-stack of pending deliveries ------------------ *
 * The e7 v1 attempt (append H->next to sub and flatten) LOST delivery ordering:
 * with an enclosing handle, the nested performs' deliveries and the outermost
 * handle-continuation must run in nesting (LIFO) order, which flattening destroys.
 * The correct stackless technique keeps the "run H->next after the resume settles"
 * obligations on an explicit HEAP stack (a meta-continuation stack), so C stack
 * stays flat while delivery order is preserved. A delivery consisting only of
 * HANDLER/DONE nodes is a no-op (it can perform nothing -- it runs only after a
 * DONE) and is ELIDED, which keeps the meta-stack O(nesting), not O(N): a deep
 * tail-resume loop's reinstalled-handler deliveries are all no-ops. */
typedef int64_t (*Transform)(int64_t arg);
static jmp_buf g_jb;
static DK     *g_chain = NULL;
static int64_t g_val = 0;

/* meta-stack of pending delivery chains (LIFO) */
static DK   **g_meta = NULL;
static size_t g_meta_n = 0, g_meta_cap = 0;
static size_t g_meta_hwm = 0;                     /* high-water mark (for the report) */
static void meta_push(DK *d) {
    if (g_meta_n == g_meta_cap) { g_meta_cap = g_meta_cap ? g_meta_cap*2 : 16;
        g_meta = (DK**)realloc(g_meta, g_meta_cap*sizeof(DK*)); }
    g_meta[g_meta_n++] = d;
    if (g_meta_n > g_meta_hwm) g_meta_hwm = g_meta_n;
}
static bool delivery_is_noop(const DK *d) {       /* only HANDLER/DONE -> identity */
    for (const DK *p = d; p; p = p->next)
        if (p->kind != DKK_HANDLER && p->kind != DKK_DONE) return false;
    return true;
}
static intptr_t dk_run_impl(DK *k, intptr_t v);   /* fwd */

/* trampolined perform for a DEEP tail-resume handler. Builds the SAME resumed
 * chain the real dk_perform does (captured ++ reinstall ++ enclosing), pushes the
 * handler's H->next delivery onto the meta-stack (unless it is a no-op), and
 * yields (resumed_chain, transform(arg)) to the driver. */
static intptr_t dk_perform_tramp(int tag, intptr_t arg, DK *k) {
    DK *H = k;
    while (H && !(H->kind == DKK_HANDLER && H->tag == tag) && H->kind != DKK_DONE) H = H->next;
    if (!H || H->kind == DKK_DONE) { fprintf(stderr, "unhandled effect tag %d\n", tag); abort(); }
    DK *sub = dk_copy_range(k, H);
    const DK *ge = H;
    while (ge && ge->kind == DKK_HANDLER) ge = ge->next;
    DK *reinstall = dk_append(dk_copy_range(H, ge), dk_copy_enclosing_handlers(ge));
    sub = dk_append(sub, reinstall);
    DK *delivery = dk_copy_range(H->next, NULL);   /* what dk_run_impl(H->next,r) would run */
    if (delivery_is_noop(delivery)) { dk_free(delivery); }
    else meta_push(delivery);
    Transform g = (Transform)(intptr_t)H->handler_env;
    int64_t rv = g ? g(arg) : arg;
    g_chain = sub; g_val = rv;
    longjmp(g_jb, 1);
    return 0;
}

static DK *dk_frame_resume(DKResumeFrame fn, intptr_t env, DK *next) {
    DK *k = dk_new(DKK_RESUME_FRAME, next); k->rfn = fn; k->env = env; return k;
}
static intptr_t dk_run_impl(DK *k, intptr_t v) {
    while (k) {
        switch (k->kind) {
            case DKK_DONE: return v;
            case DKK_PROMPT: case DKK_HANDLER: k = k->next; break;
            case DKK_FRAME: v = k->fn(k->env, v); k = k->next; break;
            /* a RESUME_FRAME receives its run-time downstream chain `rest` (this
             * node's ->next: the reinstalled-handler tail) and CONSUMES it -- so a
             * tail-recursive effectful loop threads the REINSTALLED handler (no-op
             * delivery) rather than the original handle chain, and the handle
             * continuation is reached exactly once via the outermost delivery. */
            case DKK_RESUME_FRAME: return k->rfn(k->env, v, k->next);
            default: fprintf(stderr, "probe: unexpected node\n"); abort();
        }
    }
    return v;
}
/* driver: run the active chain to DONE; then pop the meta-stack and run the next
 * pending delivery on the result -- in LIFO (nesting) order. A perform inside any
 * of these longjmps back here with a fresh chain. C stack stays flat. */
static int64_t dk_run_root_tramp(int64_t (*body)(void *), void *arg) {
    if (setjmp(g_jb) == 0) return body(arg);   /* first entry (longjmps on 1st perform) */
    int64_t v;
    for (;;) {
        DK *chain = g_chain; int64_t rv = g_val;
        if (setjmp(g_jb) == 0) {
            v = dk_run_impl(chain, rv);        /* run to DONE (or longjmp on perform) */
            dk_free(chain);
            if (g_meta_n == 0) return v;       /* nothing pending: done */
            g_chain = g_meta[--g_meta_n];      /* pop delivery, run it on v */
            g_val = v;
            continue;
        }
        dk_free(chain);                        /* yielded again mid-run: loop, flat */
    }
}

/* ======================= scenario ======================= */
/* Effects: INNER (tag 1), OUTER (tag 2). Inner handle handles INNER and tail-
 * resumes (+1). Its body ALSO performs OUTER once at the end, which the inner
 * handle does NOT handle -> must propagate to the enclosing OUTER handle. */
#define INNER 1
#define OUTER 2

/* fat-closure fn-value with the E2 DK* slot */
typedef struct { void *env; int64_t (*fn)(void*,int64_t); int64_t (*fn_cps)(void*,int64_t,DK*); } fatfn;

static int64_t inner_g(int64_t x){ return x + 1; }     /* INNER resume transform */
static int64_t outer_g(int64_t x){ return x + 1000; }  /* OUTER resume transform */

static int64_t g_sum = 0;
static int64_t g_kframe_runs = 0;   /* handle-continuation must run exactly once */
static int64_t g_outer_seen = 0;

/* the effectful callback, reached indirectly */
static int64_t cb_cps(void *env, int64_t x, DK *k){ (void)env; return dk_perform_tramp(INNER, x, k); }
static int64_t cb_leg(void *env, int64_t x){ (void)env; return x; }

/* deep loop body continuation. The perform continuation is a RESUME_FRAME so it
 * threads its run-time `rest` (the reinstalled-handler tail) into the recursion,
 * NOT the original handle chain -- so subsequent performs find the reinstalled
 * handler (no-op delivery) and kframe is reached exactly once. */
typedef struct { fatfn *f; int64_t n; } Env;   /* NB: no k -- rest is the runtime chain */
static int64_t loop_after_rf(intptr_t e, intptr_t v, DK *rest);
static int64_t g_final_outer = 0;

static int64_t loop_cps(fatfn *f, int64_t n, DK *k){
    note();
    if (n == 0) {
        /* body finished: perform OUTER once (unhandled by inner handle) */
        return dk_perform_tramp(OUTER, 42, k);
    }
    Env *e = (Env*)malloc(sizeof(Env)); e->f=f; e->n=n;
    DK *cont = dk_frame_resume(loop_after_rf, (intptr_t)e, k);
    return f->fn_cps(f->env, n, cont);
}
static int64_t loop_after_rf(intptr_t e_, intptr_t v, DK *rest){
    Env *e = (Env*)e_; g_sum += v; fatfn *f=e->f; int64_t n=e->n; free(e);
    return loop_cps(f, n-1, rest);   /* thread the reinstalled-handler tail */
}
/* handle continuation frame (kname): receives the body's completion value */
static int64_t kframe(intptr_t env, intptr_t v){ (void)env; g_kframe_runs++; return v; }
/* outer handle continuation frame */
static int64_t outer_kframe(intptr_t env, intptr_t v){ (void)env; g_final_outer = v; return v; }

/* build: OUTER handle wraps INNER handle wraps loop body.
 * outer chain: HANDLER(OUTER,outer_g) -> FRAME(outer_kframe) -> done
 * inner chain: HANDLER(INNER,inner_g) -> FRAME(kframe) -> <enclosing: OUTER markers> */
static long g_N;
static int64_t body_entry(void *arg){
    fatfn *f = (fatfn*)arg;
    /* enclosing (outer) handler markers copied past the inner handle-cont frame */
    DK *outer_tail = dk_handler(OUTER, NULL, (intptr_t)(Transform)outer_g,
                        dk_frame(outer_kframe, 0, dk_done()));
    DK *inner = dk_handler(INNER, NULL, (intptr_t)(Transform)inner_g,
                    dk_frame(kframe, 0, outer_tail));
    return loop_cps(f, g_N, inner);
}

int main(void){
    char anchor; g_base = &anchor; g_N = 1000000;
    fatfn f = { NULL, cb_leg, cb_cps };
    g_sum = 0; g_kframe_runs = 0; g_final_outer = 0; g_max = 0;
    int64_t r = dk_run_root_tramp(body_entry, &f);

    long long expect_sum = (long long)g_N*(g_N+1)/2 + g_N; /* sum(i+1) */
    long long expect_outer = 42 + 1000;                    /* outer_g(42) */
    printf("N=%ld result=%lld sum=%lld expect_sum=%lld kframe_runs=%lld outer_final=%lld expect_outer=%lld max_stack=%ld\n",
           g_N, (long long)r, (long long)g_sum, expect_sum,
           (long long)g_kframe_runs, (long long)g_final_outer, expect_outer, g_max);

    bool ok_sum   = (g_sum == expect_sum);
    bool ok_kfr   = (g_kframe_runs == 1);
    bool ok_outer = (g_final_outer == expect_outer);
    bool ok_flat  = (g_max < 65536);
    bool ok_meta  = (g_meta_hwm < 64);   /* O(nesting), NOT O(N) */
    printf("meta-stack high-water=%zu (want O(nesting), not O(N=%ld))\n", g_meta_hwm, g_N);
    printf("\n=== E7 FULL-FIDELITY VERDICT ===\n");
    printf("deep tail-recursion flat (real chain layout): %s (max_stack=%ld)\n", ok_flat?"YES":"NO", g_max);
    printf("handle-continuation frame runs exactly once:  %s (runs=%lld)\n", ok_kfr?"YES":"NO", (long long)g_kframe_runs);
    printf("enclosing OUTER handle catches propagated eff: %s\n", ok_outer?"YES":"NO");
    printf("accumulated side effects correct:             %s\n", ok_sum?"YES":"NO");
    printf("meta-stack bounded by nesting (heap O(1) C):  %s (hwm=%zu)\n", ok_meta?"YES":"NO", g_meta_hwm);
    return (ok_sum && ok_kfr && ok_outer && ok_flat && ok_meta) ? 0 : 1;
}
