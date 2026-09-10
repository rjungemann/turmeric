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

/* Run fn(arg) on a thread with a large, explicitly sized stack and return its
 * result.
 *
 * The emitter's expression walk is plain structural recursion over the AST
 * (emit_value -> emit_value_dispatch -> emit_builtin), and the depth it needs
 * is a property of the SOURCE, not of anything the compiler chooses -- one
 * macro that expands to a nested chain can want more levels than a default
 * 8 MiB thread stack has, especially on a Debug+ASan build where a frame is
 * ~40x its normal size.  Bounding the walk with a constant was tried and does
 * not work: a constant sized against one host's frames stops being a ceiling
 * the moment a frame grows (docs/archive/emit-depth-guard-loses-race-with-asan-stack.md).
 *
 * So give the walk a stack that matches the job instead of rationing it.  This
 * is what production compilers do for the same reason -- rustc runs
 * compilation on a spawned thread with an explicit stack size, tunable through
 * RUST_MIN_STACK, precisely because AST recursion depth follows the input --
 * and it is what this tree already does one layer down: jit_engine.c runs a
 * JIT'd program's entry on a pthread sized by TUR_JIT_STACK_MB.  TUR_STACK_MB
 * is the same knob for the compiler's own recursion.
 *
 * `tur` trampolines its whole driver onto this stack, not just emission.  The
 * emitter is where the overflow was first observed, but it is not the only
 * recursive walk over the AST: elab_call -> elab_form recurses per nesting
 * level too, and unlike the emitter it had no guard at all, so a deeply nested
 * call aborted the compiler with no diagnostic.  One sized stack underneath
 * the whole front end covers reader, elaborator and emitter together.
 *
 * Size comes from TUR_STACK_MB when set (clamped to [8, 8192]), else
 * TUR_STACK_MB_DEFAULT.  The stack is virtual and faulted in lazily, so
 * an unused reservation costs address space, not memory.
 *
 * Re-entrant-safe: if the caller is already running on a stack this function
 * created, fn is invoked directly rather than nesting another thread.  If the
 * thread cannot be created the call degrades to a direct invocation, so the
 * worst case is the behaviour that existed before -- tur_stack_nearly_exhausted
 * is still the backstop underneath. */
int tur_run_on_big_stack(int (*fn)(void *), void *arg);

#define TUR_STACK_MB_DEFAULT 256

#endif /* TUR_STACK_GUARD_H */
