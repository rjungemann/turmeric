#ifndef TUR_FIBER_CTX_H
#define TUR_FIBER_CTX_H

#include <stdint.h>
#include "platform.h"

#if defined(TUR_ARCH_WASM)

/* WASM: no native context switching. Provide a placeholder type so that
 * TurFiber compiles; the compiler-level fiber (src/fiber.c) is never called
 * from the WASM interpreter path. */
typedef struct { char _placeholder[1]; } tur_ctx_t;
static inline void tur_ctx_swap(tur_ctx_t *from, tur_ctx_t *to) { (void)from; (void)to; }
static inline void fiber_entry_shim(void) {}

#elif defined(TUR_ARCH_X64) && defined(_WIN32)

/* Windows x64 saves more callee-saved state than SysV: RSI, RDI and XMM6-XMM15
 * are non-volatile on Windows but not on SysV.  fiber_ctx_x64_win.S relies on
 * this exact field order (offsets 0,8,16,...); keep them in lockstep.  The TEB
 * stack fields are deliberately NOT saved: MinGW/GCC's stack probe
 * (___chkstk_ms) touches pages directly and does not consult TEB->StackLimit,
 * so a malloc'd fiber stack works without them. */
typedef struct {
    uintptr_t rip;   /* 0  */
    uintptr_t rsp;   /* 8  */
    uintptr_t rbx;   /* 16 */
    uintptr_t rbp;   /* 24 */
    uintptr_t rsi;   /* 32 */
    uintptr_t rdi;   /* 40 */
    uintptr_t r12;   /* 48 */
    uintptr_t r13;   /* 56 */
    uintptr_t r14;   /* 64 */
    uintptr_t r15;   /* 72 */
    unsigned char xmm[10 * 16]; /* 80..240: XMM6-XMM15 (movups, no align needed) */
} tur_ctx_t;

#elif defined(TUR_ARCH_X64)

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
