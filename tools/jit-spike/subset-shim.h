/* c2mir subset shim -- J0 JIT spike.
 *
 * THIS FILE IS A FINDING, NOT A FIX.
 *
 * Turmeric's fixed runtime preamble emits three constructs that c2mir does not
 * accept.  All three appear in EVERY emitted program -- including `arith`, the
 * most trivial fixture in the corpus -- and none of them is guarded by a
 * platform `#if` in src/compiler/emit_module.c, so this is not macOS-specific.
 * Without this shim, c2mir rejects the preamble and no fixture compiles at all.
 *
 *   1. `__thread`.  c2mir registers only the C11 spelling `_Thread_local`
 *      (c2mir.c kw_add, ~line 5453); there is no kw_add for the GNU spelling on
 *      any target.  emit_module.c emits `__thread` as a literal string in ~9
 *      places.  Real fix: emit `_Thread_local`, which every supported cc also
 *      accepts.  Cheap and unambiguous.
 *
 *   2. The GCC atomic builtins.  `__atomic_load_n` / `__atomic_store_n` /
 *      `__atomic_add_fetch` / `__atomic_compare_exchange_n` and the
 *      `__ATOMIC_*` memory orderings have ZERO occurrences anywhere in the MIR
 *      tree.  The plan flagged "C11 minus atomics" as a known risk; the J0
 *      findings then concluded the subset gap was "narrower than feared".  That
 *      conclusion is wrong -- ~17 atomic builtins are emitted per program.
 *      Real fix: these belong in the HOST runtime, compiled by cc and resolved
 *      by address through dlsym(RTLD_DEFAULT), exactly like hamt.c already is.
 *      They should never reach c2mir as text.  See plan section 3.2 step 4.
 *
 *   3. `__auto_type`.  normalize-c11-subset.py rewrites most occurrences but
 *      cannot infer them all; the residue is the single largest parse-failure
 *      class in the fixture sweep (25 of 34).
 *
 * The atomic lowerings below are NOT correct -- they drop atomicity outright.
 * They are sound only because the fixtures exercised here are single-threaded.
 * Shipping them would silently corrupt the refcount under `spawn`.  Fix (2)
 * properly before any of this graduates past a spike.
 *
 * The atomic list below is INCOMPLETE by construction -- it covers what the
 * fixture sweep actually hit.  A builtin that is missing here shows up as
 * `jit-spike: unresolved import: __atomic_...` at link time rather than as a
 * parse error, which is a clean signal: add it and re-run.
 */
#ifndef TUR_JIT_SUBSET_SHIM_H
#define TUR_JIT_SUBSET_SHIM_H

#define __thread _Thread_local

#define __ATOMIC_RELAXED 0
#define __ATOMIC_CONSUME 1
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __ATOMIC_ACQ_REL 4
#define __ATOMIC_SEQ_CST 5

/* Spike-only, single-threaded-only.  See item (2) above. */
#define __atomic_load_n(p, o) (*(p))
#define __atomic_store_n(p, v, o) ((void) (*(p) = (v)))
#define __atomic_add_fetch(p, v, o) (*(p) += (v))
#define __atomic_sub_fetch(p, v, o) (*(p) -= (v))
/* fetch_* return the value BEFORE the operation.  `p` is evaluated more than
   once; every call site in the emitted preamble passes a plain address, so this
   is safe here but would not be in general. */
#define __atomic_fetch_add(p, v, o) (*(p) += (v), *(p) - (v))
#define __atomic_fetch_sub(p, v, o) (*(p) -= (v), *(p) + (v))
#define __atomic_compare_exchange_n(p, e, d, weak, succ, fail) \
  (*(p) == *(e) ? ((*(p) = (d)), 1) : ((*(e) = *(p)), 0))

#endif /* TUR_JIT_SUBSET_SHIM_H */
