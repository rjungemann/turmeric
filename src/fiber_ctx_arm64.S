/* AArch64 context switch for Turmeric fibers. */

    .text

#if defined(__APPLE__)
    .globl _tur_ctx_swap
_tur_ctx_swap:
#else
    .globl tur_ctx_swap
    .type tur_ctx_swap, %function
tur_ctx_swap:
#endif
    stp    x19, x20, [x0,  #0]
    stp    x21, x22, [x0, #16]
    stp    x23, x24, [x0, #32]
    stp    x25, x26, [x0, #48]
    stp    x27, x28, [x0, #64]
    stp    x29, x30, [x0, #80]
    mov    x9, sp
    str    x9, [x0, #96]

    ldp    x19, x20, [x1,  #0]
    ldp    x21, x22, [x1, #16]
    ldp    x23, x24, [x1, #32]
    ldp    x25, x26, [x1, #48]
    ldp    x27, x28, [x1, #64]
    ldp    x29, x30, [x1, #80]
    ldr    x9, [x1, #96]
    mov    sp, x9
    ret

#if !defined(__APPLE__)
    .size tur_ctx_swap, .-tur_ctx_swap
#endif

#if defined(__APPLE__)
    .globl _fiber_entry_shim
_fiber_entry_shim:
    mov    x0, x19
    b      _tur_fiber_entry
#else
    .globl fiber_entry_shim
    .type fiber_entry_shim, %function
fiber_entry_shim:
    mov    x0, x19
    b      tur_fiber_entry
    .size fiber_entry_shim, .-fiber_entry_shim
#endif
