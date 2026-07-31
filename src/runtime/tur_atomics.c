/* tur_atomics.c -- atomic primitives for front ends without the GCC builtins.
 *
 * jit-engine-plan section 4 / findings recommendation 6(a).
 *
 * The emitted runtime preamble needs atomics in 18 places (STM's version clock
 * and per-TVar version/value, the scheduler's cancel flag, and select's
 * winner-claim compare-exchange).  Every one of them was emitted as a literal
 * `__atomic_*` GCC builtin.  That is fine for `cc` and fatal for a C11 front
 * end that does not implement them: c2mir has zero occurrences of the whole
 * family anywhere in its tree, so the JIT spike had to #define them away to
 * non-atomic reads and writes -- correct only because the fixtures it ran were
 * single-threaded, and silent refcount corruption under `spawn` otherwise.
 *
 * The fix the plan calls for is to keep atomics in the HOST runtime, compiled
 * by a real cc and resolved by address, exactly as `hamt.c` already is.  These
 * are those functions.  The emitted preamble routes through a `TUR_ATOMIC_*`
 * macro layer that expands to the builtins directly under __GNUC__/__clang__
 * -- so the `cc` path emits and compiles precisely what it always did, with no
 * call overhead on the STM commit path -- and to these calls otherwise.
 *
 * Memory orders are NOT threaded through.  Every operation here is seq_cst,
 * which is a strengthening of each order the preamble asks for (relaxed,
 * acquire, release, acq_rel): it forbids strictly more reordering, so no
 * program can observe a behaviour it would not also have been allowed under the
 * requested order.  Passing the order instead would mean a switch per call,
 * because GCC requires the order argument to be a compile-time constant -- and
 * this path is only ever taken by a front end that has no builtins to be fast
 * with in the first place.
 */

#include <stdint.h>

uint64_t tur_atomic_load_u64 (const volatile uint64_t *p) {
    return __atomic_load_n (p, __ATOMIC_SEQ_CST);
}

void tur_atomic_store_u64 (volatile uint64_t *p, uint64_t v) {
    __atomic_store_n (p, v, __ATOMIC_SEQ_CST);
}

uint64_t tur_atomic_add_fetch_u64 (volatile uint64_t *p, uint64_t v) {
    return __atomic_add_fetch (p, v, __ATOMIC_SEQ_CST);
}

void *tur_atomic_load_ptr (void *const volatile *p) {
    return __atomic_load_n (p, __ATOMIC_SEQ_CST);
}

void tur_atomic_store_ptr (void *volatile *p, void *v) {
    __atomic_store_n (p, v, __ATOMIC_SEQ_CST);
}

int tur_atomic_load_int (const volatile int *p) {
    return __atomic_load_n (p, __ATOMIC_SEQ_CST);
}

/* Strong compare-exchange.  Returns 1 on success; on failure writes the
 * observed value through `expected`, matching __atomic_compare_exchange_n's
 * contract (the emitted select loop relies on that write-back). */
int tur_atomic_cas_int (volatile int *p, int *expected, int desired) {
    return __atomic_compare_exchange_n (p, expected, desired, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 1 : 0;
}
