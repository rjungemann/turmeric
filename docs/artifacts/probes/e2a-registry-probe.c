/* E2a REGISTRY PROBE (docs/upcoming/v2/cps-dk-sole-effect-lowering-plan.md)
 *
 * Proves the tier-`now` threading mechanism the E2 coloring identified: a
 * captureless effectful fn-value carried as a BARE int64 direct-entry fn-ptr
 * (representation 1) can thread the DK continuation to the CALLER's handler via a
 * direct->cps REGISTRY -- the mechanism E2a's emitter must generate.  The E2
 * kill-probe already proved a fat-closure `fn_cps` SLOT threads soundly; this
 * probe proves the OTHER path the coloring needs: getting the __cps entry by
 * LOOKING UP the bare direct-entry pointer (no struct slot available -- the
 * carrier IS the direct-entry address).
 *
 * It models `tests/fixtures/effect-fn-type-annot` verbatim:
 *   (defn call-writer [f :(fn [cstr] #fx{Write} nil)] (f "fn type annot test"))
 *   (defn main []
 *     (handle (do (call-writer (fn [s] (perform (Write s)))) 0)
 *       (Write [s] k) (do (println s) (resume k nil))))
 * The confirmed emitted representation (flag-on) is: the lambda __fn_1282 is a
 * bare `static void __fn_1282(int64_t s)` and is passed as `(int64_t)__fn_1282`.
 * So the threaded call site cannot read a slot -- it must look the pointer up.
 *
 * Build/run: cc -O2 -o /tmp/e2a docs/upcoming/v2/probes/e2a-registry-probe.c && /tmp/e2a
 * Expected: prints the callback string ONCE, and result == 0 (main's handle value).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

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
        if (tail) tail->next = c; else head = c;
        tail = c;
    }
    DK *d = dk_done();
    if (tail) tail->next = d; else head = d;
    return head;
}
static DK *dk_copy_range(const DK *from, const DK *stop) {
    DK *head = NULL, *tail = NULL;
    for (const DK *p = from; p && p != stop && p->kind != DKK_DONE; p = p->next) {
        DK *c = dk_copy_node(p);
        if (tail) tail->next = c; else head = c;
        tail = c;
    }
    DK *d = dk_done();
    if (tail) tail->next = d; else head = d;
    return head;
}
static DK *dk_append(DK *a, DK *b) {
    if (!a) return b;
    DK *p = a;
    while (p->next && p->next->kind != DKK_DONE) p = p->next;
    if (p->next) { free(p->next); }
    p->next = b;
    return a;
}
static void dk_free(DK *k) { while (k) { DK *n = k->next; free(k); k = n; } }

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
__attribute__((unused)) static intptr_t dk_run(DK *k, intptr_t v) { return dk_run_impl(k, v, false); }
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

/* ==================== E2a: direct-entry -> CPS-entry registry ============= */
/* The tier-`now` carrier is the bare direct-entry fn-ptr; the threaded call site
 * has only that pointer, so it recovers the __cps variant here.  Registered at
 * startup for every threadable fn-value; looked up at a statically-bare call.
 * A CPS entry for a `(fn [cstr] #fx{Write} nil)` lambda has this shape: */
typedef intptr_t (*cps1_t)(intptr_t arg, DK *__kont);
static struct { intptr_t direct; cps1_t cps; } g_cps_reg[64];
static int g_cps_reg_n = 0;
static void tur_cps_register(intptr_t direct, cps1_t cps) {
    if (g_cps_reg_n < 64) { g_cps_reg[g_cps_reg_n].direct = direct;
        g_cps_reg[g_cps_reg_n].cps = cps; g_cps_reg_n++; }
}
static cps1_t tur_cps_lookup(intptr_t direct) {
    for (int i = 0; i < g_cps_reg_n; i++)
        if (g_cps_reg[i].direct == direct) return g_cps_reg[i].cps;
    return NULL;
}

/* ===== the effect and the (bare-fn-ptr) effectful lambda __fn_1282 ======== */
#define WRITE_TAG 1
static int g_print_count = 0;

/* __fn_1282's DIRECT entry (fiber shape) -- present only to be the registry key;
 * the DK path never calls it.  In the real emitter this is the existing
 * `static void __fn_1282(int64_t s)` that performs Write on the fiber. */
static void fn_1282(int64_t s) { (void)s; /* fiber body -- unused on the DK path */ }

/* __fn_1282's CPS entry -- what E2a emits: perform Write(s) threading __kont so
 * the effect reaches the CALLER's handler.  Tail perform: continuation is __kont. */
static intptr_t fn_1282__cps(intptr_t s, DK *__kont) {
    return dk_perform(WRITE_TAG, s, __kont);
}

/* ===== call-writer's CPS entry: the THREADED indirect call ================ */
/* (defn call-writer [f] (f "fn type annot test"))  -- f is the bare fn-ptr.
 * The tail call threads __kont to f's __cps entry, recovered from the registry. */
static intptr_t call_writer__cps(intptr_t f, DK *__kont) {
    cps1_t cps = tur_cps_lookup(f);
    if (!cps) { fprintf(stderr, "tur: fn-value has no cps entry\n"); abort(); }
    return cps((intptr_t)"fn type annot test", __kont);
}

/* ===== main's handler: (Write [s] k) (do (println s) (resume k nil)) ====== */
static intptr_t write_handler(intptr_t env, intptr_t arg, DK *subk) {
    (void)env;
    printf("%s\n", (const char *)arg);   /* (println s) */
    g_print_count++;
    return dk_invoke(subk, 0);           /* (resume k nil) -- nil == 0 */
}
/* the "0" tail of the do: (do (call-writer ...) 0) -- ignore call-writer's value */
static intptr_t const_zero(intptr_t env, intptr_t v) { (void)env; (void)v; return 0; }

int main(void) {
    /* startup: register the threadable fn-value's direct->cps mapping */
    tur_cps_register((intptr_t)fn_1282, fn_1282__cps);

    /* main body: (handle (do (call-writer __fn_1282) 0) (Write [s] k) ...) */
    DK *k = dk_handler(WRITE_TAG, write_handler, 0, dk_done());
    DK *body_cont = dk_frame(const_zero, 0, k);   /* after call-writer -> 0 -> deliver */
    intptr_t result = call_writer__cps((intptr_t)fn_1282, body_cont);
    dk_free(body_cont);

    printf("result=%lld  prints=%d\n", (long long)result, g_print_count);
    /* Expected: the string once, result 0, exactly one print. */
    bool ok = (result == 0) && (g_print_count == 1);
    printf("%s\n", ok ? "E2a-REGISTRY: PASS" : "E2a-REGISTRY: FAIL");
    return ok ? 0 : 1;
}
