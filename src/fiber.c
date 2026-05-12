#include <stdint.h>
#include <stdlib.h>
#include "fiber.h"

static void *tur_align_down_16(void *p) {
    uintptr_t v = (uintptr_t)p;
    v &= ~(uintptr_t)0xF;
    return (void *)v;
}

void tur_fiber_entry(TurFiber *f) {
    f->done = false;
    f->fn(f);
    f->done = true;
    tur_ctx_swap(&f->ctx, &f->caller_ctx);
    abort();
}

TurFiber *tur_fiber_new(void (*fn)(TurFiber *), size_t stack_size) {
    if (!fn) return NULL;
    if (stack_size == 0) stack_size = 1024 * 1024;

    TurFiber *f = (TurFiber *)calloc(1, sizeof(TurFiber));
    if (!f) return NULL;

    f->stack = malloc(stack_size);
    if (!f->stack) {
        free(f);
        return NULL;
    }

    f->stack_size = stack_size;
    f->fn = fn;

    void *sp = tur_align_down_16((char *)f->stack + stack_size);

#if defined(TUR_ARCH_X64)
    f->ctx.rip = (uintptr_t)fiber_entry_shim;
    f->ctx.rsp = (uintptr_t)sp;
    f->ctx.r12 = (uintptr_t)f;
#elif defined(TUR_ARCH_ARM64)
    f->ctx.lr = (uintptr_t)fiber_entry_shim;
    f->ctx.sp = (uintptr_t)sp;
    f->ctx.x19 = (uintptr_t)f;
#endif

    return f;
}

void *tur_fiber_resume(TurFiber *f, void *arg) {
    if (!f || f->done) return f ? f->result : NULL;
    f->arg = arg;
    tur_ctx_swap(&f->caller_ctx, &f->ctx);
    return f->result;
}

void tur_fiber_yield(TurFiber *f, void *value) {
    if (!f) return;
    f->result = value;
    tur_ctx_swap(&f->ctx, &f->caller_ctx);
}

bool tur_fiber_done(TurFiber *f) {
    return f ? f->done : true;
}

void tur_fiber_free(TurFiber *f) {
    if (!f) return;
    free(f->stack);
    free(f);
}
