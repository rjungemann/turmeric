/* tur_tls.c -- thread-local runtime state for front ends without TLS.
 *
 * jit-engine-plan / findings 14.3 and 15.  Multi-threading under the JIT is a
 * requirement (owner decision, 2026-07-29), and c2mir has no thread-local
 * storage at all: it parses `_Thread_local`, warns "Thread local is not
 * implemented", and then treats the variable as an ordinary global.  Under
 * that reading every spawned thread shares one slot -- which is how 8 STM
 * workers ended up sharing one transaction descriptor and losing updates
 * (stm-stress), and why gc-registry-growth SIGSEGVed.
 *
 * The emitted preamble declares 11 thread-local variables.  Under a
 * GNU-family compiler they stay exactly what they were -- plain
 * `TUR_THREAD_LOCAL` file-scope variables, zero indirection.  Under any other
 * front end the emitter #defines each NAME to a deref of one of these
 * accessors (emit_rt_tls, src/compiler/emit_module.c): the host runtime is
 * compiled by a real cc, so `__thread` here is genuine per-thread storage, and
 * each accessor returns the calling thread's instance by address.  Same
 * host-residency pattern as tur_atomics.c, applied to state instead of
 * operations.
 *
 * Slots are deliberately typed void* / int / bool / int64_t / jmp_buf: the pointee
 * types (STM_Transaction, FiberBlock, TurThreadState, ...) are preamble-
 * private structs this TU has no business knowing.  The emitted macro casts
 * the slot back to the precise type at every use site.  All initializers in
 * the preamble are zero, and `__thread` storage zero-initializes per new
 * thread, so the semantics match.
 *
 * jmp_buf is the one non-scalar: its layout is fixed by libc's <setjmp.h>,
 * which both this TU and the emitted program include, so handing the buffer
 * across the boundary is well-defined.
 */

#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>

static __thread void *tur_tls_stm_current_tx = 0;
void **tur_tls_stm_current_tx_ptr (void) { return &tur_tls_stm_current_tx; }

static __thread void *tur_tls_handler_chain = 0;
void **tur_tls_handler_chain_ptr (void) { return &tur_tls_handler_chain; }

static __thread int tur_tls_panicking = 0;
int *tur_tls_panicking_ptr (void) { return &tur_tls_panicking; }

static __thread void *tur_tls_cur_shift_reset = 0;
void **tur_tls_cur_shift_reset_ptr (void) { return &tur_tls_cur_shift_reset; }

static __thread void *tur_tls_current_fiber = 0;
void **tur_tls_current_fiber_ptr (void) { return &tur_tls_current_fiber; }

static __thread bool tur_tls_fiber_cancelled_flag = false;
bool *tur_tls_fiber_cancelled_flag_ptr (void) { return &tur_tls_fiber_cancelled_flag; }

static __thread void *tur_tls_current_thread_state = 0;
void **tur_tls_current_thread_state_ptr (void) { return &tur_tls_current_thread_state; }

static __thread jmp_buf tur_tls_cancel_jmpbuf;
jmp_buf *tur_tls_cancel_jmpbuf_ptr (void) { return &tur_tls_cancel_jmpbuf; }

static __thread int tur_tls_cancel_jmpbuf_valid = 0;
int *tur_tls_cancel_jmpbuf_valid_ptr (void) { return &tur_tls_cancel_jmpbuf_valid; }

static __thread void *tur_tls_current_scheduler_mt = 0;
void **tur_tls_current_scheduler_mt_ptr (void) { return &tur_tls_current_scheduler_mt; }

static __thread int64_t tur_tls_rtv = 0;
int64_t *tur_tls_rtv_ptr (void) { return &tur_tls_rtv; }
