#ifndef TUR_FIBER_CTX_H
#define TUR_FIBER_CTX_H

#include "platform.h"

#if defined(TUR_ARCH_X64)

typedef struct {
    void *rip;
    void *rsp;
    void *rbx;
    void *rbp;
    void *r12;
    void *r13;
    void *r14;
    void *r15;
} tur_ctx_t;

#elif defined(TUR_ARCH_ARM64)

typedef struct {
    void *x19;
    void *x20;
    void *x21;
    void *x22;
    void *x23;
    void *x24;
    void *x25;
    void *x26;
    void *x27;
    void *x28;
    void *fp;
    void *lr;
    void *sp;
} tur_ctx_t;

#endif

void tur_ctx_swap(tur_ctx_t *from, tur_ctx_t *to);
void fiber_entry_shim(void);

#endif /* TUR_FIBER_CTX_H */
