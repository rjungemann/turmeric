# Fiber Assembly Context-Switch Plan

**Goal:** Replace the `ucontext_t`/`makecontext`/`swapcontext` POSIX API (deprecated on macOS) with a
minimal hand-rolled assembly context switch that owns no external dependencies and works on every
supported target (macOS arm64, macOS x86-64, Linux x86-64, Linux arm64).

This plan is the implementation roadmap for the fallback described in T21-A of
`deferred-tasks-T19-T21.md`.

---

## Motivation

`ucontext_t` is deprecated on macOS (POSIX.1-2008 removed it from the standard; Apple annotates it
`__DARWIN_C_LEVEL < __DARWIN_C_FULL`).  It still compiles today — suppressing the warning is fine
short-term — but the deprecation will eventually become a hard removal.  The assembly replacement is
~10–15 instructions per architecture, easier to audit than any third-party coroutine library, and
has zero license friction.

---

## What a fiber context switch needs to save

Under both the System V AMD64 ABI and the ARM64 (AArch64) ABI the **callee-saved** registers must
survive a function call.  The return address / link register and the stack pointer complete the set.

| x86-64 (SysV) | arm64 (AArch64) |
|---|---|
| `rbx`, `rbp`, `r12`–`r15` | `x19`–`x28`, `x29` (fp), `x30` (lr) |
| `rsp` | `sp` |
| `rip` (via return address on stack) | `pc` (implicit — `ret` loads `x30`) |

Floating-point callee-saved registers (`xmm6`–`xmm15` on Windows x64; none required on SysV or
arm64 ABI) are **not** saved — Turmeric's fiber bodies do not rely on caller-preserved FP state
across a yield boundary.

---

## New files

```
src/fiber_ctx.h      — tur_ctx_t struct + tur_ctx_swap declaration
src/fiber_ctx_x64.S  — x86-64 implementation
src/fiber_ctx_arm64.S — arm64 implementation
src/fiber.h          — TurFiber struct (replaces ucontext_t fields)
src/fiber.c          — fiber lifecycle functions
```

The existing `platform.h` gains a `TUR_ARCH_*` detection block.

---

## Step 1 — Architecture detection in `src/platform.h`

Add at the end of `src/platform.h`:

```c
/* ── Architecture tags ────────────────────────────────────────────────────── */
#if defined(__aarch64__) || defined(_M_ARM64)
#  define TUR_ARCH_ARM64 1
#elif defined(__x86_64__) || defined(_M_X64)
#  define TUR_ARCH_X64 1
#else
#  error "Unsupported architecture: only x86-64 and arm64 are supported"
#endif
```

---

## Step 2 — `src/fiber_ctx.h`

```c
/* fiber_ctx.h — minimal register-save context for cooperative fiber switching.
 *
 * Only the callee-saved registers + stack pointer + return address are stored.
 * The layout MUST match the assembly in fiber_ctx_x64.S / fiber_ctx_arm64.S.
 */
#ifndef TUR_FIBER_CTX_H
#define TUR_FIBER_CTX_H

#include <stddef.h>
#include "platform.h"

#if defined(TUR_ARCH_X64)

typedef struct {
    void   *rip;    /* return address (instruction pointer)   offset  0 */
    void   *rsp;    /* stack pointer                          offset  8 */
    void   *rbx;    /*                                        offset 16 */
    void   *rbp;    /*                                        offset 24 */
    void   *r12;    /*                                        offset 32 */
    void   *r13;    /*                                        offset 40 */
    void   *r14;    /*                                        offset 48 */
    void   *r15;    /*                                        offset 56 */
} tur_ctx_t;        /* total: 64 bytes */

#elif defined(TUR_ARCH_ARM64)

typedef struct {
    void   *x19;    /* offset   0 */
    void   *x20;    /* offset   8 */
    void   *x21;    /* offset  16 */
    void   *x22;    /* offset  24 */
    void   *x23;    /* offset  32 */
    void   *x24;    /* offset  40 */
    void   *x25;    /* offset  48 */
    void   *x26;    /* offset  56 */
    void   *x27;    /* offset  64 */
    void   *x28;    /* offset  72 */
    void   *fp;     /* x29, frame pointer   offset  80 */
    void   *lr;     /* x30, link register   offset  88 */
    void   *sp;     /* stack pointer        offset  96 */
} tur_ctx_t;        /* total: 104 bytes */

#endif

/* Save current registers into *from, then restore registers from *to and jump.
 * Signature matches the ABI-mandated argument registers on both platforms:
 *   x64:   rdi = from, rsi = to
 *   arm64: x0  = from, x1  = to
 */
void tur_ctx_swap(tur_ctx_t *from, tur_ctx_t *to);

#endif /* TUR_FIBER_CTX_H */
```

---

## Step 3 — x86-64 assembly (`src/fiber_ctx_x64.S`)

```asm
/* fiber_ctx_x64.S — x86-64 SysV ABI cooperative context switch
 *
 * void tur_ctx_swap(tur_ctx_t *from /*rdi*/, tur_ctx_t *to /*rsi*/);
 *
 * Field offsets (bytes from struct base) must match fiber_ctx.h:
 *   rip=0  rsp=8  rbx=16  rbp=24  r12=32  r13=40  r14=48  r15=56
 */

#if defined(__ELF__)
    .section .note.GNU-stack,"",@progbits   /* mark stack non-executable */
#endif

    .text
#if defined(__APPLE__)
    .globl _tur_ctx_swap
_tur_ctx_swap:
#else
    .globl tur_ctx_swap
    .type  tur_ctx_swap, @function
tur_ctx_swap:
#endif
    /* Save 'from' context ------------------------------------------------- */
    mov    (%rsp), %rax          /* return address                           */
    mov    %rax,   0(%rdi)       /* from->rip                                */
    lea    8(%rsp), %rax         /* caller's rsp (before the call instruction) */
    mov    %rax,   8(%rdi)       /* from->rsp                                */
    mov    %rbx,  16(%rdi)
    mov    %rbp,  24(%rdi)
    mov    %r12,  32(%rdi)
    mov    %r13,  40(%rdi)
    mov    %r14,  48(%rdi)
    mov    %r15,  56(%rdi)

    /* Restore 'to' context ------------------------------------------------ */
    mov    56(%rsi), %r15
    mov    48(%rsi), %r14
    mov    40(%rsi), %r13
    mov    32(%rsi), %r12
    mov    24(%rsi), %rbp
    mov    16(%rsi), %rbx
    mov     8(%rsi), %rsp        /* switch stack                             */
    jmp    *0(%rsi)              /* jump to to->rip                          */

#if !defined(__APPLE__)
    .size tur_ctx_swap, .-tur_ctx_swap
#endif
```

---

## Step 4 — arm64 assembly (`src/fiber_ctx_arm64.S`)

```asm
/* fiber_ctx_arm64.S — AArch64 cooperative context switch
 *
 * void tur_ctx_swap(tur_ctx_t *from /*x0*/, tur_ctx_t *to /*x1*/);
 *
 * Field offsets must match fiber_ctx.h:
 *   x19=0 x20=8 x21=16 x22=24 x23=32 x24=40 x25=48 x26=56
 *   x27=64 x28=72 fp=80 lr=88 sp=96
 */

    .text
#if defined(__APPLE__)
    .globl _tur_ctx_swap
_tur_ctx_swap:
#else
    .globl tur_ctx_swap
    .type  tur_ctx_swap, %function
tur_ctx_swap:
#endif
    /* Save 'from' ---------------------------------------------------------  */
    stp    x19, x20, [x0,  #0]
    stp    x21, x22, [x0, #16]
    stp    x23, x24, [x0, #32]
    stp    x25, x26, [x0, #48]
    stp    x27, x28, [x0, #64]
    stp    x29, x30, [x0, #80]   /* fp, lr                                  */
    mov    x9,  sp
    str    x9,       [x0, #96]   /* sp                                      */

    /* Restore 'to' --------------------------------------------------------  */
    ldp    x19, x20, [x1,  #0]
    ldp    x21, x22, [x1, #16]
    ldp    x23, x24, [x1, #32]
    ldp    x25, x26, [x1, #48]
    ldp    x27, x28, [x1, #64]
    ldp    x29, x30, [x1, #80]   /* fp, lr                                  */
    ldr    x9,       [x1, #96]
    mov    sp,  x9               /* switch stack                            */
    ret                          /* branches to restored x30 (lr)           */

#if !defined(__APPLE__)
    .size tur_ctx_swap, .-tur_ctx_swap
#endif
```

---

## Step 5 — `src/fiber.h` (replaces ucontext_t fields in T21-A)

Replace the `ucontext_t ctx` / `ucontext_t caller_ctx` fields with `tur_ctx_t`:

```c
#ifndef TUR_FIBER_H
#define TUR_FIBER_H

#include <stddef.h>
#include <stdbool.h>
#include "fiber_ctx.h"

typedef struct TurFiber {
    tur_ctx_t  ctx;           /* fiber execution context            */
    tur_ctx_t  caller_ctx;    /* context to return to on yield      */
    void      *stack;         /* heap-allocated stack               */
    size_t     stack_size;    /* default: 1 MiB                     */
    bool       done;          /* true after fiber closure returns   */
    void      *result;        /* return value (set when done)       */
    void      *arg;           /* argument passed on first resume    */
    void      *handler_chain; /* fiber-local effect handler chain   */
} TurFiber;

TurFiber *tur_fiber_new(void (*fn)(TurFiber *), size_t stack_size);
void      tur_fiber_resume(TurFiber *f, void *arg);
void      tur_fiber_yield(TurFiber *f, void *value);
bool      tur_fiber_done(TurFiber *f);
void      tur_fiber_free(TurFiber *f);

#endif /* TUR_FIBER_H */
```

---

## Step 6 — `src/fiber.c` — fiber lifecycle using `tur_ctx_swap`

Setting up a fiber's initial context is the only non-trivial piece.  Instead of `makecontext`,
write the stack manually:

```c
#include <stdlib.h>
#include <string.h>
#include "fiber.h"
#include "platform.h"

/* Trampoline: called when a fiber starts for the first time.
 * 'f' is passed as the first argument by tur_fiber_resume via the initial
 * stack frame set up in tur_fiber_new.
 */
static void fiber_entry(TurFiber *f) {
    f->done   = false;
    ((void (*)(TurFiber *))f->arg)(f);  /* fn stored in arg on first entry */
    f->done   = true;
    /* Return to caller via saved caller_ctx. */
    tur_ctx_swap(&f->ctx, &f->caller_ctx);
    /* Unreachable. */
    abort();
}

TurFiber *tur_fiber_new(void (*fn)(TurFiber *), size_t stack_size) {
    if (stack_size == 0) stack_size = 1024 * 1024; /* 1 MiB default */

    TurFiber *f = calloc(1, sizeof(TurFiber));
    if (!f) return NULL;

    f->stack = malloc(stack_size);
    if (!f->stack) { free(f); return NULL; }
    f->stack_size = stack_size;
    f->done       = false;

    /* Point the initial 'arg' at fn so the trampoline can call it. */
    f->arg = (void *)fn;

    /* ── Build the initial stack frame ─────────────────────────────────── */
    char *sp = (char *)f->stack + stack_size;

#if defined(TUR_ARCH_X64)
    /* x86-64: push a fake return address (abort) so the stack is 16-byte
     * aligned before fiber_entry is called, then set rip = fiber_entry
     * and rsp = sp - 8 (the pushed slot).
     */
    sp -= sizeof(void *);
    *(void **)sp = (void *)abort;   /* sentinel return address */
    f->ctx.rip   = (void *)fiber_entry;
    f->ctx.rsp   = sp;
    /* Pass 'f' as the first argument in rdi — but tur_ctx_swap does not set
     * argument registers.  Use a wrapper that reads the TurFiber pointer from
     * a well-known location instead.
     *
     * Simplest approach: store f in rbx (callee-saved), have fiber_entry
     * read it from there via a tiny asm shim, OR push f onto the stack and
     * have the trampoline pop it.  The implementation below pushes f so
     * fiber_entry_x64_shim (a one-liner) can mov it to rdi before calling
     * fiber_entry.
     */
    sp -= sizeof(void *);
    *(void **)sp = (void *)f;
    f->ctx.rsp   = sp;
    /* r12 is callee-saved and available; store f there for the shim. */
    f->ctx.r12   = (void *)f;
    f->ctx.rip   = (void *)fiber_entry_shim;  /* see note below */
#elif defined(TUR_ARCH_ARM64)
    /* arm64: set lr to fiber_entry (ret branches to lr), sp to top of stack,
     * x19 holds 'f' (callee-saved; fiber_entry reads it as its first arg via
     * the shim).
     */
    f->ctx.lr    = (void *)fiber_entry_shim;
    f->ctx.sp    = sp;
    f->ctx.x19   = (void *)f;
#endif

    return f;
}
```

> **Note on argument passing:** `tur_ctx_swap` does not touch argument registers (`rdi`/`x0`).
> A one-instruction shim is needed to move the fiber pointer into the argument register before
> `fiber_entry` is called.  Add these in the respective `.S` files:
>
> **x64** — `fiber_entry_shim`: `mov %r12, %rdi; jmp fiber_entry`
>
> **arm64** — `fiber_entry_shim`: `mov x0, x19; b fiber_entry`

```c
void *tur_fiber_resume(TurFiber *f, void *arg) {
    f->arg = arg;
    tur_ctx_swap(&f->caller_ctx, &f->ctx);
    return f->result;
}

void tur_fiber_yield(TurFiber *f, void *value) {
    f->result = value;
    tur_ctx_swap(&f->ctx, &f->caller_ctx);
}

bool tur_fiber_done(TurFiber *f) {
    return f->done;
}

void tur_fiber_free(TurFiber *f) {
    free(f->stack);
    free(f);
}
```

---

## Step 7 — Makefile changes

The Makefile currently globs `src/*.c` for sources.  Add assembly source handling:

```makefile
SRCS    := $(wildcard src/*.c)
ASM_SRCS:= $(wildcard src/*.S)
OBJS    := $(patsubst src/%.c, build/%.o,  $(SRCS)) \
           $(patsubst src/%.S, build/%.o,  $(ASM_SRCS))

build/%.o: src/%.S | build
	$(CC) $(CFLAGS) -c -o $@ $<
```

No other Makefile changes are required — the `.S` files use the standard C preprocessor via `$(CC)`,
so `TUR_ARCH_X64` / `TUR_ARCH_ARM64` defined by `platform.h` are available (include it with
`#include "platform.h"` at the top of each `.S` file under a `#ifdef __ASSEMBLER__` guard, or rely
on the architecture-specific filenames and the Makefile only building the file that matches the
current target — the latter is simpler).

Simplest approach: use filename-based selection so only the correct `.S` is compiled:

```makefile
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),arm64)
    ASM_SRCS := src/fiber_ctx_arm64.S
else
    ASM_SRCS := src/fiber_ctx_x64.S
endif
```

---

## Step 8 — Remove `ucontext_t` dependency entirely

Once `src/fiber.c` and the `.S` files are wired up and all fiber fixtures pass:

1. Remove `#include <ucontext.h>` from `src/fiber.c` (and any other file that pulled it in).
2. Remove the `#pragma clang diagnostic ignored "-Wdeprecated-declarations"` suppression.
3. Update the T21-A checklist in `deferred-tasks-T19-T21.md` to reference `tur_ctx_t` instead of
   `ucontext_t`.
4. Update the "Key Design Decisions" block in `deferred-tasks-T19-T21.md`:
   - Change: *`ucontext_t` / `makecontext` / `swapcontext` (POSIX; …suppress with `-Wno-deprecated-declarations`)*
   - To: *Hand-rolled assembly context switch via `tur_ctx_swap` in `src/fiber_ctx_{x64,arm64}.S`; no POSIX ucontext dependency.*

---

## Step 9 — Test fixtures to validate the switch

These overlap with the T21-C / T21-D fixtures but should pass before any higher-level fiber API
is implemented:

| Fixture | What it checks |
|---|---|
| `tests/fixtures/fiber-ctx-basic/` | Create a fiber, resume once, verify it returns a value |
| `tests/fixtures/fiber-ctx-yield/` | Fiber yields three values; caller collects them in order |
| `tests/fixtures/fiber-ctx-stack/` | Nested function calls inside fiber (verifies stack integrity) |
| `tests/fixtures/fiber-ctx-stress/` | 10 000 fibers each yielding once (checks for leaks/corruption) |

Run under ASan (`make debug`) to catch stack overflows and use-after-free.

---

## Migration path summary

```
Phase 0 (now)
  Use ucontext_t + #pragma suppression — already works, no urgency.

Phase 1
  Implement Steps 1–7 (fiber_ctx.h, .S files, Makefile) in parallel with T21-A.
  Keep ucontext_t path behind #ifdef TUR_USE_UCONTEXT for bisection safety.

Phase 2
  All fiber-ctx-* fixtures pass on both arm64 and x86-64.
  Enable the asm path unconditionally; remove ucontext_t path (Step 8).

Phase 3
  T21-A continues as normal using TurFiber / tur_ctx_swap.
  No further changes needed for T21-B through T21-F.
```

---

## References

- System V AMD64 ABI — §3.2.1 (callee-saved registers)
- ARM Architecture Reference Manual — AArch64 Procedure Call Standard, §6.1.1
- Typical coroutine library assembly (boost.context, minicoro, libco) for cross-reference
