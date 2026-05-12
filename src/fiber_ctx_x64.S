/* x86-64 SysV context switch for Turmeric fibers. */

#if defined(__ELF__)
    .section .note.GNU-stack,"",@progbits
#endif

    .text

#if defined(__APPLE__)
    .globl _tur_ctx_swap
_tur_ctx_swap:
#else
    .globl tur_ctx_swap
    .type tur_ctx_swap, @function
tur_ctx_swap:
#endif
    mov    (%rsp), %rax
    mov    %rax,   0(%rdi)
    lea    8(%rsp), %rax
    mov    %rax,   8(%rdi)
    mov    %rbx,  16(%rdi)
    mov    %rbp,  24(%rdi)
    mov    %r12,  32(%rdi)
    mov    %r13,  40(%rdi)
    mov    %r14,  48(%rdi)
    mov    %r15,  56(%rdi)

    mov    56(%rsi), %r15
    mov    48(%rsi), %r14
    mov    40(%rsi), %r13
    mov    32(%rsi), %r12
    mov    24(%rsi), %rbp
    mov    16(%rsi), %rbx
    mov     8(%rsi), %rsp
    jmp    *0(%rsi)

#if !defined(__APPLE__)
    .size tur_ctx_swap, .-tur_ctx_swap
#endif

#if defined(__APPLE__)
    .globl _fiber_entry_shim
_fiber_entry_shim:
    mov    %r12, %rdi
    call   _tur_fiber_entry
    call   _abort
#else
    .globl fiber_entry_shim
    .type fiber_entry_shim, @function
fiber_entry_shim:
    mov    %r12, %rdi
    call   tur_fiber_entry
    call   abort
    ud2
    .size fiber_entry_shim, .-fiber_entry_shim
#endif
