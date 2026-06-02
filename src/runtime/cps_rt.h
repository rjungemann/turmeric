#ifndef TUR_CPS_RT_H
#define TUR_CPS_RT_H

#include <stdbool.h>
#include <stdint.h>

/* =========================================================================
 * CPS4 (cps-transform-plan): heap-reified continuations + trampoline runtime.
 *
 * This replaces the bounded fiber-frame snapshot (tur_cont_alloc, capped at
 * TUR_CONT_MAX_CAPTURED_FRAMES = 16) for the CPS path. A continuation is a heap
 * closure chain (TurKont):
 *
 *   - Capture is O(1) and unbounded: take the current TurKont* pointer. The
 *     chain may be arbitrarily long; depth is bounded by the heap, not by a
 *     fixed array (this is what lets capture reach the implicit root prompt).
 *   - Resume is a call: tur_kont_resume / tur_trampoline.
 *   - Native stack stays O(1) regardless of chain length, because a frame that
 *     would tail-call the next frame instead returns a *step* (a thunk) to the
 *     trampoline driver, which loops.
 *   - defer/teardown is attached per frame and fires exactly once (on normal
 *     resume, on abort/free, never twice on a re-resume of a multi-shot frame).
 *
 * The runtime is standalone (not yet wired into codegen; that is CPS5). It is
 * exercised directly by tests/cps_rt_unit.c.
 * ========================================================================= */

typedef struct TurKont TurKont;

/* A trampoline step: either a final value (done) or a thunk to invoke next. */
typedef struct TurStep TurStep;
struct TurStep {
    bool         done;
    intptr_t     value;            /* meaningful when done */
    TurStep    (*fn)(void *state); /* when !done: the next step */
    void        *state;
};

/* A continuation frame's behavior: given the resumed value, do this frame's
 * work and return a step (typically resuming `self->next`). */
typedef TurStep (*TurKontFn)(TurKont *self, intptr_t value);

struct TurKont {
    TurKontFn  fn;          /* what to do with the resumed value */
    TurKont   *next;        /* the rest of the continuation (NULL = halt) */
    intptr_t   env;         /* captured datum for this frame */
    void     (*defer)(void *defer_ctx); /* per-frame teardown, or NULL */
    void      *defer_ctx;   /* argument to defer */
    bool       defer_fired; /* defer fires exactly once */
};

/* Allocate a frame. `next`/`defer`/`defer_ctx` may be NULL. */
TurKont *tur_kont_new(TurKontFn fn, intptr_t env, TurKont *next,
                      void (*defer)(void *), void *defer_ctx);

/* Fire a frame's defer if it has not fired yet (idempotent). */
void tur_kont_fire_defer(TurKont *k);

/* Step constructors used by TurKontFn implementations. */
TurStep tur_step_done(intptr_t value);
TurStep tur_step_resume(TurKont *k, intptr_t value); /* resume k(value) via the driver */

/* Resume a continuation directly (one frame); returns its step. */
TurStep tur_kont_resume(TurKont *k, intptr_t value);

/* Drive a continuation to completion in O(1) native stack. */
intptr_t tur_trampoline(TurKont *k, intptr_t value);

/* Free a continuation chain, firing any not-yet-fired defers exactly once
 * (abort/teardown path). Safe on NULL. */
void tur_kont_free_chain(TurKont *k);

/* A ready-made frame behavior: add `env` to the resumed value, continue. Used
 * by tests to build deep chains; also a minimal example of a TurKontFn. */
TurStep tur_kont_add_fn(TurKont *self, intptr_t value);

#endif
