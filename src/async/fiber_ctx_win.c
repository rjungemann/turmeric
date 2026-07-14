/*
 * fiber_ctx_win.c - Windows x86-64 fiber context switch: NOT IMPLEMENTED.
 *
 * fiber_ctx_x64.S is SysV-only and cannot simply be reused here.  Two separate
 * reasons, either one fatal:
 *
 *   1. It reads its first argument from %rdi.  The Windows x64 ABI passes it in
 *      %rcx, so the assembly would dereference garbage.
 *   2. The Windows x64 ABI additionally requires XMM6-XMM15, %rdi and %rsi to be
 *      preserved across a call.  tur_ctx_t (see fiber_ctx.h) has no slots for
 *      them, so even a corrected register mapping would silently corrupt state.
 *
 * A correct port therefore needs both a new assembly file and a wider context
 * struct -- that is WIN3 in docs/upcoming/v1/windows-support-plan.md, and is
 * deliberately deferred.
 *
 * These stubs exist so libturi links.  They abort rather than no-op: a context
 * switch that quietly does nothing does not fail, it corrupts control flow and
 * surfaces somewhere unrecognisable much later.
 *
 * Note this does NOT affect the interpreter's generators, effect handlers, or
 * call/cc -- those go through ucontext, which IS implemented on Windows over
 * Win32 Fibers (platform_ucontext_win.h).  Only the async scheduler's stackful
 * coroutines (spawn/channels) route through here, and those are already
 * unavailable on Windows because io_iocp.c is a stub.
 */

#include "fiber_ctx.h"

#include <stdio.h>
#include <stdlib.h>

static void tur_fiber_ctx_unimplemented(const char *fn) {
    fprintf(stderr,
            "turmeric: %s is not implemented on Windows -- the async fiber "
            "scheduler requires a Windows x64 context switch that does not "
            "exist yet (WIN3 in docs/upcoming/v1/windows-support-plan.md).\n",
            fn);
    abort();
}

void tur_ctx_swap(tur_ctx_t *from, tur_ctx_t *to) {
    (void)from;
    (void)to;
    tur_fiber_ctx_unimplemented("tur_ctx_swap");
}

void fiber_entry_shim(void) {
    tur_fiber_ctx_unimplemented("fiber_entry_shim");
}
