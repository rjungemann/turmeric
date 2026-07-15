/* x86-64 Windows context switch for Turmeric fibers (WIN3 tier C).
 *
 * The SysV switch (fiber_ctx_x64.S) is not reusable on Windows: the Windows x64
 * ABI passes the first argument in RCX (not RDI), and treats RSI, RDI and
 * XMM6-XMM15 as callee-saved (SysV does not). Reusing the SysV code would read
 * the wrong argument register and clobber registers the caller expects
 * preserved -- which is exactly why fiber_ctx_win.c was a loud abort() stub
 * until this landed.
 *
 * Offsets below MUST match the Windows tur_ctx_t in fiber_ctx.h:
 *   0 rip  8 rsp  16 rbx  24 rbp  32 rsi  40 rdi
 *   48 r12  56 r13  64 r14  72 r15  80..240 xmm6..xmm15
 *
 * TEB stack fields are intentionally not saved -- see fiber_ctx.h.
 */

    .text

/* void tur_ctx_swap(tur_ctx_t *from, tur_ctx_t *to);  from=RCX, to=RDX */
    .globl tur_ctx_swap
    .def   tur_ctx_swap; .scl 2; .type 32; .endef
tur_ctx_swap:
    /* Save the resume point: the return address on the stack becomes rip, and
     * rsp is captured as it will be *after* this function returns. */
    mov    (%rsp), %rax
    mov    %rax,   0(%rcx)
    lea    8(%rsp), %rax
    mov    %rax,   8(%rcx)
    mov    %rbx,  16(%rcx)
    mov    %rbp,  24(%rcx)
    mov    %rsi,  32(%rcx)
    mov    %rdi,  40(%rcx)
    mov    %r12,  48(%rcx)
    mov    %r13,  56(%rcx)
    mov    %r14,  64(%rcx)
    mov    %r15,  72(%rcx)
    movups %xmm6,  80(%rcx)
    movups %xmm7,  96(%rcx)
    movups %xmm8, 112(%rcx)
    movups %xmm9, 128(%rcx)
    movups %xmm10,144(%rcx)
    movups %xmm11,160(%rcx)
    movups %xmm12,176(%rcx)
    movups %xmm13,192(%rcx)
    movups %xmm14,208(%rcx)
    movups %xmm15,224(%rcx)

    /* Restore the target context (rdx is preserved throughout). */
    movups 224(%rdx), %xmm15
    movups 208(%rdx), %xmm14
    movups 192(%rdx), %xmm13
    movups 176(%rdx), %xmm12
    movups 160(%rdx), %xmm11
    movups 144(%rdx), %xmm10
    movups 128(%rdx), %xmm9
    movups 112(%rdx), %xmm8
    movups  96(%rdx), %xmm7
    movups  80(%rdx), %xmm6
    mov    72(%rdx), %r15
    mov    64(%rdx), %r14
    mov    56(%rdx), %r13
    mov    48(%rdx), %r12
    mov    40(%rdx), %rdi
    mov    32(%rdx), %rsi
    mov    24(%rdx), %rbp
    mov    16(%rdx), %rbx
    mov     8(%rdx), %rsp
    jmp    *0(%rdx)

/* void fiber_entry_shim(void);
 * Entered via tur_ctx_swap into a fresh fiber. fiber.c seeds r12 with the
 * TurFiber* and rsp with a 16-aligned stack top. Pass r12 as the Windows first
 * arg (rcx), reserve the 32-byte shadow space the ABI requires, and call the C
 * entry. tur_fiber_entry never returns (it swaps back out), so abort() marks a
 * bug rather than silently running off the stack. */
    .globl fiber_entry_shim
    .def   fiber_entry_shim; .scl 2; .type 32; .endef
fiber_entry_shim:
    mov    %r12, %rcx
    sub    $32, %rsp
    call   tur_fiber_entry
    call   abort
    ud2
