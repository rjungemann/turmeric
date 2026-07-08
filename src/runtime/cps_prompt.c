#include "cps_prompt.h"

#include <stdio.h>
#include <stdlib.h>

/* =========================================================================
 * CPS5 (cps-transform-plan): multi-prompt delimited-control machine.
 * See cps_prompt.h for the model.
 * ========================================================================= */

typedef enum {
    DKK_DONE,
    DKK_FRAME,
    DKK_PROMPT,
    DKK_SHIFT,
    DKK_SHIFT0,
    DKK_HANDLER,
} DKKind;

struct DK {
    DKKind    kind;
    DKFrame   fn;        /* DKK_FRAME */
    intptr_t  env;       /* DKK_FRAME */
    int       tag;       /* DKK_PROMPT / DKK_SHIFT* / DKK_HANDLER */
    DKBody    body;      /* DKK_SHIFT* */
    intptr_t  body_env;  /* DKK_SHIFT* */
    DKHandler handler;   /* DKK_HANDLER */
    intptr_t  handler_env; /* DKK_HANDLER */
    DK       *next;
};

static DK *dk_new(DKKind kind, DK *next) {
    DK *k = calloc(1, sizeof(DK));
    k->kind = kind;
    k->next = next;
    return k;
}

DK *dk_done(void) { return dk_new(DKK_DONE, NULL); }

DK *dk_frame(DKFrame fn, intptr_t env, DK *next) {
    DK *k = dk_new(DKK_FRAME, next);
    k->fn = fn; k->env = env;
    return k;
}

DK *dk_prompt(int tag, DK *next) {
    DK *k = dk_new(DKK_PROMPT, next);
    k->tag = tag;
    return k;
}

static DK *dk_shift_impl(DKKind kind, int tag, DKBody body, intptr_t env, DK *next) {
    DK *k = dk_new(kind, next);
    k->tag = tag; k->body = body; k->body_env = env;
    return k;
}

DK *dk_shift(int tag, DKBody body, intptr_t env, DK *next) {
    return dk_shift_impl(DKK_SHIFT, tag, body, env, next);
}
DK *dk_shift0(int tag, DKBody body, intptr_t env, DK *next) {
    return dk_shift_impl(DKK_SHIFT0, tag, body, env, next);
}

DK *dk_handler(int tag, DKHandler fn, intptr_t env, DK *next) {
    DK *k = dk_new(DKK_HANDLER, next);
    k->tag = tag; k->handler = fn; k->handler_env = env;
    return k;
}

/* Shallow-copy one node (next set to NULL). */
static DK *dk_copy_node(const DK *n) {
    DK *c = dk_new(n->kind, NULL);
    c->fn = n->fn; c->env = n->env; c->tag = n->tag;
    c->body = n->body; c->body_env = n->body_env;
    c->handler = n->handler; c->handler_env = n->handler_env;
    return c;
}

DK *dk_copy(const DK *k) {
    if (!k) return NULL;
    DK *head = NULL, *tail = NULL;
    for (const DK *p = k; p; p = p->next) {
        DK *c = dk_copy_node(p);
        if (!head) head = tail = c;
        else { tail->next = c; tail = c; }
    }
    return head;
}

/* Copy frames in [from, stop) (stop exclusive; stop==NULL copies to the end).
 * Returns a fresh chain whose tail->next is NULL, or NULL if the range is
 * empty. */
static DK *dk_copy_range(const DK *from, const DK *stop) {
    DK *head = NULL, *tail = NULL;
    for (const DK *p = from; p && p != stop; p = p->next) {
        DK *c = dk_copy_node(p);
        if (!head) head = tail = c;
        else { tail->next = c; tail = c; }
    }
    return head;
}

/* Append b to the end of a; returns the head. */
static DK *dk_append(DK *a, DK *b) {
    if (!a) return b;
    DK *p = a;
    while (p->next) p = p->next;
    p->next = b;
    return a;
}

void dk_free(DK *k) {
    while (k) { DK *n = k->next; free(k); k = n; }
}

bool dk_has_prompt(const DK *k) {
    for (const DK *p = k; p; p = p->next)
        if (p->kind == DKK_PROMPT) return true;
    return false;
}

/* Core machine. `root` enables the implicit root prompt: a shift whose target
 * prompt is not found is delimited at program end (DONE). */
static intptr_t dk_run_impl(DK *k, intptr_t v, bool root) {
    while (k) {
        switch (k->kind) {
            case DKK_DONE:
                return v;
            case DKK_PROMPT:
            case DKK_HANDLER:
                k = k->next;            /* transparent to a returning value */
                break;
            case DKK_FRAME:
                v = k->fn(k->env, v);
                k = k->next;
                break;
            case DKK_SHIFT:
            case DKK_SHIFT0: {
                /* find the nearest enclosing prompt with this tag */
                DK *P = k->next;
                while (P && !(P->kind == DKK_PROMPT && P->tag == k->tag)
                         && P->kind != DKK_DONE)
                    P = P->next;
                bool to_root = (!P || P->kind == DKK_DONE);
                bool reinstall = (k->kind == DKK_SHIFT);
                /* reify sub-continuation: frames between the shift and the
                 * prompt (P is the matching prompt, or the trailing DONE when
                 * capturing to root), then (for shift) a re-installed prompt,
                 * then DONE. */
                DK *sub = dk_copy_range(k->next, P);
                DK *tail = reinstall
                    ? dk_prompt(to_root ? DK_ROOT_TAG : k->tag, dk_done())
                    : dk_done();
                sub = dk_append(sub, tail);
                intptr_t bodyval = k->body(k->body_env, sub);
                dk_free(sub);
                /* deliver bodyval to the prompt's outer continuation */
                if (to_root) return bodyval;       /* root: value escapes program */
                k = P->next;
                v = bodyval;
                break;
            }
        }
    }
    return v;
}

intptr_t dk_run(DK *k, intptr_t v)      { return dk_run_impl(k, v, false); }
intptr_t dk_run_root(DK *k, intptr_t v) { return dk_run_impl(k, v, true); }

intptr_t dk_invoke(DK *sub, intptr_t w) {
    DK *c = dk_copy(sub);          /* fresh copy: multi-shot */
    intptr_t r = dk_run_impl(c, w, false);
    dk_free(c);
    return r;
}

intptr_t dk_perform(int tag, intptr_t arg, DK *k) {
    /* Find the nearest enclosing handler for this effect tag. */
    DK *H = k;
    while (H && !(H->kind == DKK_HANDLER && H->tag == tag) && H->kind != DKK_DONE)
        H = H->next;
    if (!H || H->kind == DKK_DONE) {
        fprintf(stderr, "tur: unhandled effect (tag %d)\n", tag);
        abort();
    }
    /* Reify the sub-continuation from the perform point up to the handler, then
     * re-install the handler on the captured copy so a resume re-delimits (deep
     * handler). */
    DK *sub = dk_copy_range(k, H);
    sub = dk_append(sub, dk_handler(tag, H->handler, H->handler_env, dk_done()));
    intptr_t r = H->handler(H->handler_env, arg, sub);
    dk_free(sub);
    /* Deliver the handler-case result to the handler's outer continuation. */
    return dk_run_impl(H->next, r, false);
}
