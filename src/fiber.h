#ifndef TUR_FIBER_H
#define TUR_FIBER_H

#include <stdbool.h>
#include <stddef.h>
#include "fiber_ctx.h"

typedef struct TurFiber TurFiber;

struct TurFiber {
    tur_ctx_t ctx;
    tur_ctx_t caller_ctx;
    void *stack;
    size_t stack_size;
    bool done;
    void *result;
    void *arg;
    /* Phase P19-8: Per-fiber effect handler chain (mirrors FiberBlock.effect_handler_chain
     * in the generated C runtime).  Typed void * because EffectHandlerFrame is defined
     * only inside generated programs; cast to EffectHandlerFrame * at call sites. */
    void *effect_handler_chain;
    void (*fn)(TurFiber *);
};

TurFiber *tur_fiber_new(void (*fn)(TurFiber *), size_t stack_size);
void *tur_fiber_resume(TurFiber *f, void *arg);
void tur_fiber_yield(TurFiber *f, void *value);
bool tur_fiber_done(TurFiber *f);
void tur_fiber_free(TurFiber *f);

#endif /* TUR_FIBER_H */
