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

/* --- Apple SDK header predefines (macOS hosts only) ----------------------
 *
 * Unlike everything above, these are NOT about Turmeric's emitted C -- they are
 * about the SDK headers the emitted C includes.  Apple's headers auto-detect
 * the compiler and #error out when they cannot identify it; c2mir identifies as
 * nothing in particular, so three separate headers fail before a generated line
 * is parsed.  9 fixtures fail this way.
 *
 * MEASURED RESULT: this block fixes NONE of them.  Full corpus is 1409/1680
 * with it and 1410/1680 without (the one-fixture delta is stm-stress, which
 * flakes on the atomic lowerings above, not on anything here).  What it does is
 * move all 9 past the header gate and into a DEEPER failure -- which is worth
 * keeping, because one of those deeper failures is the most serious thing the
 * macOS sweep found:
 *
 *   - 5 fixtures: TargetConditionals.h now parses, and they fail later on
 *     `syntax error on typedef`, an ordinary c2mir subset gap.
 *   - 3 fixtures: mach/message.h:543 and :569 now reach their
 *     `xnu_static_assert_struct_size` checks and FAIL them.  Root cause is NOT
 *     mach-specific: c2mir silently ignores `#pragma pack` AND
 *     `__attribute__((packed))`, so it computes 64/72 where XNU demands 60/68.
 *     Those 3 fixtures are not miscompiled -- nothing in stdlib/ or
 *     src/runtime/ ever uses a mach_msg struct (stdlib/image.tur only wants
 *     _NSGetExecutablePath), so XNU's assert turns a layout bug into a clean
 *     compile error.  The real exposure is user inline-C with a packed struct
 *     that the host runtime also sees: offsets would diverge with NO diagnostic
 *     (the pragma at least warns "unknown pragma"; the attribute is silent).
 *     Host-independent -- macOS is louder, not more broken, because XNU ships
 *     _Static_assert ABI locks and glibc does not.  See finding 4 in the report.
 *     Invisible without __arm64__ having a value, which is why the predefine
 *     earns its place.
 *   - 1 fixture: _OSSwapInt16 stays unresolved.  libkern/_OSByteOrder.h only
 *     emits the static-inline bodies under __GNUC__; the fallback path declares
 *     them extern and libSystem exports no such symbol.  Defining __GNUC__ is
 *     the obvious next probe and was not tried -- it would also switch a lot of
 *     other headers onto GNU-builtin paths c2mir lacks.
 *
 * See docs/reported/jit-macos-full-corpus-extension-and-atexit.md.
 * Guarded on __APPLE__, which c2mir does predefine, so the shim stays a single
 * file across hosts.
 */
#ifdef __APPLE__

/* c2mir predefines __arm64__ / __aarch64__ with an EMPTY replacement list, so
 * mach/port.h:100's `#if __arm64__` is an empty controlling expression rather
 * than a true one, and c2mir rejects it outright.  Give them values. */
#undef __arm64__
#define __arm64__ 1
#undef __aarch64__
#define __aarch64__ 1

/* TargetConditionals.h:398 `#error unknown compiler`.  Its detection cascade
 * needs __is_target_arch / __is_target_os (clang builtins c2mir lacks), and the
 * header itself documents setting the TARGET_CPU_ and TARGET_OS_ macros as the
 * supported escape hatch for exactly this case. */
#define TARGET_CPU_ARM64 1
#define TARGET_OS_MAC 1

/* libkern/OSByteOrder.h:314 `#error Unknown endianess.`  Neither endianness
 * macro is predefined, and every Apple arm64 target is little-endian. */
#define __LITTLE_ENDIAN__ 1

#endif /* __APPLE__ */

/* The GCC __builtin_* family, PROTOTYPED.  c2mir does not know these names, so
 * an undeclared `__builtin_sqrt(x)` gets an implicit declaration returning
 * `int` -- the call then reads the integer return register while the harness
 * shim delivers the result in xmm0, and floor(sqrt(25.0)) came out as 1
 * (load-in-imported-module, the last unexplained wrong-output in the sweep).
 * With prototypes, c2mir emits correctly-typed calls and the resolver's shim
 * table supplies the addresses.
 *
 * `unsigned long` rather than size_t on purpose: this shim is prepended ahead
 * of the TU's own #includes, so size_t is not in scope yet (LP64 targets only,
 * which is what the spike runs on). */
double __builtin_pow (double, double);
double __builtin_sqrt (double);
double __builtin_ceil (double);
double __builtin_floor (double);
double __builtin_fabs (double);
void __builtin_trap (void);
unsigned long __builtin_strlen (const char *);
int __builtin_popcount (unsigned int);
void *__builtin_memcpy (void *, const void *, unsigned long);

#endif /* TUR_JIT_SUBSET_SHIM_H */
