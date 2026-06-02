#include "cps_rt.h"

#include <stdlib.h>

/* =========================================================================
 * CPS4 (cps-transform-plan): heap-reified continuations + trampoline.
 * See cps_rt.h for the model.
 * ========================================================================= */

TurKont *tur_kont_new(TurKontFn fn, intptr_t env, TurKont *next,
                      void (*defer)(void *), void *defer_ctx) {
    TurKont *k = malloc(sizeof(TurKont));
    k->fn = fn;
    k->next = next;
    k->env = env;
    k->defer = defer;
    k->defer_ctx = defer_ctx;
    k->defer_fired = false;
    return k;
}

void tur_kont_fire_defer(TurKont *k) {
    if (!k || k->defer_fired) return;
    k->defer_fired = true;          /* set first: fires exactly once even on re-entry */
    if (k->defer) k->defer(k->defer_ctx);
}

TurStep tur_step_done(intptr_t value) {
    TurStep s;
    s.done = true;
    s.value = value;
    s.fn = NULL;
    s.state = NULL;
    return s;
}

/* The trampoline state for a pending resume: which frame, with what value. */
typedef struct ResumeState {
    TurKont *k;
    intptr_t v;
} ResumeState;

static TurStep resume_thunk(void *state) {
    ResumeState *rs = (ResumeState *)state;
    TurKont *k = rs->k;
    intptr_t v = rs->v;
    free(rs);                       /* one heap step per bounce; freed here */
    return k->fn(k, v);             /* exactly one frame's work, then return */
}

TurStep tur_step_resume(TurKont *k, intptr_t value) {
    if (!k) return tur_step_done(value);
    ResumeState *rs = malloc(sizeof(ResumeState));
    rs->k = k;
    rs->v = value;
    TurStep s;
    s.done = false;
    s.value = 0;
    s.fn = resume_thunk;
    s.state = rs;
    return s;
}

TurStep tur_kont_resume(TurKont *k, intptr_t value) {
    if (!k) return tur_step_done(value);
    return k->fn(k, value);
}

intptr_t tur_trampoline(TurKont *k, intptr_t value) {
    TurStep s = tur_step_resume(k, value);
    while (!s.done) {
        s = s.fn(s.state);          /* no nesting -> O(1) native stack */
    }
    return s.value;
}

void tur_kont_free_chain(TurKont *k) {
    while (k) {
        tur_kont_fire_defer(k);     /* abort path: fire teardown once */
        TurKont *next = k->next;
        free(k);
        k = next;
    }
}

TurStep tur_kont_add_fn(TurKont *self, intptr_t value) {
    tur_kont_fire_defer(self);
    intptr_t nv = value + self->env;
    if (self->next) return tur_step_resume(self->next, nv);
    return tur_step_done(nv);
}
