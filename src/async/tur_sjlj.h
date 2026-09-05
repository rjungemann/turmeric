/* Non-local jump primitive for the DK trampoline on win64.
 *
 * See src/async/tur_sjlj_x64_win.S for why libc setjmp/longjmp cannot be used
 * on a fiber stack there, and why this has to be an ordinary extern call rather
 * than a compiler builtin.
 *
 * Windows-only: everywhere else the emitted preamble uses plain
 * setjmp/longjmp, which works on a fiber stack, so no symbol is needed and
 * none is defined.
 */
#ifndef TUR_SJLJ_H
#define TUR_SJLJ_H

#ifdef _WIN32

/* 240 bytes: rip, rsp, rbx, rbp, rsi, rdi, r12-r15, xmm6-xmm15.  Must match the
 * offsets in tur_sjlj_x64_win.S.  Spelled in pointer-sized words because that
 * is how the emitted C declares the buffer, and BOTH halves of an S2 split
 * program have to agree on the size -- the buffer is a by-value member of
 * structs that cross the boundary. */
#define TUR_SJLJ_WORDS 30

/* Returns 0 when called directly, 1 when resumed by tur_sjlj_jump. */
int tur_sjlj_set (void *buf);

/* Resumes the tur_sjlj_set that filled `buf`.  Does not return. */
void tur_sjlj_jump (void *buf);

#endif /* _WIN32 */

#endif /* TUR_SJLJ_H */
