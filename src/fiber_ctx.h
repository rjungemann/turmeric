#ifndef TUR_FIBER_CTX_H
#define TUR_FIBER_CTX_H

#include <stdint.h>
#include "platform.h"

#if defined(TUR_ARCH_X64)

typedef struct {
    uintptr_t rip;
    uintptr_t rsp;
    uintptr_t rbx;
    uintptr_t rbp;
    uintptr_t r12;
    uintptr_t r13;
    uintptr_t r14;
    uintptr_t r15;
} tur_ctx_t;

#elif defined(TUR_ARCH_ARM64)

typedef struct {
    uintptr_t x19;
    uintptr_t x20;
    uintptr_t x21;
    uintptr_t x22;
    uintptr_t x23;
    uintptr_t x24;
    uintptr_t x25;
    uintptr_t x26;
    uintptr_t x27;
    uintptr_t x28;
    uintptr_t fp;
    uintptr_t lr;
    uintptr_t sp;
} tur_ctx_t;

#endif

void tur_ctx_swap(tur_ctx_t *from, tur_ctx_t *to);
void fiber_entry_shim(void);

#endif /* TUR_FIBER_CTX_H */
