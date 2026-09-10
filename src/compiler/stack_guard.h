/* stack_guard.h -- real C-stack headroom, the second trigger for the
 * compiler's recursion depth limits.
 *
 * Every deep structural walk in the compiler is bounded by a depth COUNTER
 * (macro expansion: ELAB_MAX_MACRO_EXPANSION_DEPTH; the emitter's expression
 * walk: EMIT_MAX_EXPR_DEPTH).  A counter is only a proxy for what actually
 * runs out, which is stack, and the two disagree by a large factor between
 * build types: on a Debug + ASan build the redzone-inflated frames can
 * exhaust the real stack before the counter trips, so the process aborts with
 * a sanitizer stack-overflow report instead of printing the diagnostic the
 * bound exists to print.  Both walks have been observed doing exactly that on
 * macOS/arm64 -- see docs/archive/macro-depth-guard-loses-race-with-asan-stack.md
 * (macro expansion, fixed 2026-08-18) and
 * docs/archive/emit-depth-guard-loses-race-with-asan-stack.md (the emitter).
 *
 * So a bounded walk pairs its counter with tur_stack_nearly_exhausted():
 * raise the SAME diagnostic when EITHER the counter hits its cap OR a genuine
 * recursion is under way and the real headroom has run down.  Re-tuning the
 * constant is not a fix -- it only moves the cliff for the next frame that
 * grows.
 *
 * The non-obvious part, and the reason this is one shared implementation
 * rather than a probe copied per call site: under ASan the address of a local
 * does NOT approximate the stack pointer (address-taken locals live on the
 * sanitizer's fake stack), so a hand-rolled probe silently degrades to the
 * depth counter and reproduces the bug it was written to fix.  This reads the
 * SP register directly. */
#ifndef TUR_STACK_GUARD_H
#define TUR_STACK_GUARD_H

#include <stdbool.h>
#include <stddef.h>

/* Remaining C-stack headroom (bytes) for the calling thread, or SIZE_MAX when
 * the platform cannot tell us.  `total_out` (optional) gets the thread's total
 * stack size, 0 when unknown. */
size_t tur_stack_headroom(size_t *total_out);

/* True when so little stack remains that another level of a recursion cycle
 * risks a hard stack-overflow abort before a depth counter can trip.  The
 * margin scales with the thread's stack (an eighth), clamped to
 * [256 KiB, 1 MiB]: far above one recursion cycle, far below a healthy stack,
 * and lenient enough that a deliberately tiny `ulimit -s` does not trip it at
 * trivial depth.  Returns false when headroom is unknown, so a caller falls
 * back to its depth counter alone. */
bool tur_stack_nearly_exhausted(void);

#endif /* TUR_STACK_GUARD_H */
