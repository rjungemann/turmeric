# Emitted C makes indirect calls through function pointers of the wrong type

**Severity: high (WASM), medium (native).** Undefined behaviour in C on every
target. On SysV/AAPCS most instances are ABI-benign: `bool` vs `int64_t`, or a
pointer vs `int64_t`, rides the same register. But WebAssembly's
`call_indirect` checks the exact signature, so each one is a trap on the web
build. And the same *mechanism* is what produced this repo's longest-running
silent-wrong-answer family, when the type that differs is a `double` or a
16-byte tagged `any`. Filed 2026-09-30 with the P0 representation-confusion
work.

**Status: OPEN, being swept (56 trapping fixtures as of 2026-10-01).** The detector is armed in the four source
fuzzers (`tests/fuzz_arm.py`, report-only `FNPTR_TRAP` until this reaches
zero). It is **not** yet a gate on the fixture suite.

## How to see it

clang's `-fsanitize=function` compares every indirect call with the callee's
definition. Trap mode needs no UBSan runtime:

```sh
CC=clang TUR_CC_FLAGS="-O2 -std=c99 -Wall -Wfloat-conversion \
  -Werror=implicit-function-declaration -fno-strict-aliasing \
  -fsanitize=function -fsanitize-trap=function -L$PWD/build/src" \
  timeout 720 bash tests/run.sh
```

A trap is SIGILL (exit 132). To find the site, build the fixture at **`-O0
-g`** with the same flags and run it under gdb. At `-O1` the trap is
attributed to whatever got inlined around the call: the first clustering of
this sweep blamed `dk_free` for what was an E2a call two frames away.

The detector is EXACT. `char *` vs `void *`, `long` vs `long long`, and
`bool` vs `long` results all trap. It cannot tell a benign mismatch from a
harmful one, so the corpus has to reach zero before it can gate.

## Where the corpus stands

| Sweep | Trapping fixtures | What changed |
| --- | --- | --- |
| first | 328 | -- |
| second | 165 | zero-parameter functions emitted `(void)`, not the unprototyped `()` the sanitizer hashes as a different type |
| third | 106 | session thread wrapper calls `void (*)(void *)`; stdlib `seq-call-bool-fn1` and the comparator calls (vec/map/set/mutmap/pair/result eq) cast slot 0 to the `int64_t` it returns for a narrow result (narrow-closure-result-read-through-int64-carrier) |
| fourth (2026-10-01) | 66 | a variadic's fat-box shim casts the rest slot to the definition's typed chain pointer (`ensure_variadic_rest_fatshim`); a fat closure packed into the `tur_poly_fn_t` carrier gets a word adapter when its thunk is not all-word (`ensure_fat_word_adapter`); a dictionary slot whose class-variable parameter is a non-word scalar holds a converting wrapper (`dict_slot_param_is_word_scalar`); a bare function boxed behind the generic word shim gets a bare-call word adapter keyed on its recorded signature.  **Two of these were silent wrong answers when the type was a `double`** -- see `docs/archive/dict-classvar-float-param-value-converted.md` |

Clusters at `-O0` as of the second sweep (165 fixtures; counts are
fixtures). The session, `seq-call-bool-fn1` and comparator rows are fixed in
the third sweep:

| Count | Site | Kind |
| --- | --- | --- |
| 38 | `tur_session_thread_wrapper`: `thunk(arg)` calls a `(fn [] nil)` fat slot 0 as `int64_t (*)(void *)` | runtime typedef, benign |
| 13 | poly-fn carrier `((int64_t (*)(void*, int64_t))g.fn)(g.env, x)` against a wrapper of another signature | M4 |
| 12 | `__tur_fatshim_tur_tagged_t_int64_t` casts a variadic callee's rest slot `int64_t` where the definition spells `tur_adt_Cons__any *` | pointer vs int |
| 11 | `__inst_Functor_fmap_Identity` | M4 |
| 9 | stdlib `seq-call-bool-fn1`: `TUR_APPLY1_T(bool, int64_t, f, x)` on a generic `int64_t` fatshim | bool vs int64 |
| 12 | stdlib comparators (`vec-eq?`, map/set/result eq): `bool(*)(void*, int64_t, int64_t)` | benign/M4 |
| ~70 | long tail: typed-field boxes on the generic shim for pointer signatures, E2a pointer args, reactor callbacks, serializer hooks | mixed |

How the fourth sweep was measured, since the trap gives no message without a
UBSan runtime: build each trapping fixture at `-O0 -g` with the trap flags,
run it under `gdb -batch`, and read frame 0 and its emitted C line. The 56
fixtures that failed to LINK under clang (libturi is built with gcc's ASan)
are environmental and not counted.

A fifth pass (56): an E2a registry entry declared with a pointer or narrow
parameter registers an adapter in the call site's word convention (`<fn>__e2w`).

Remaining clusters (56 traps, at `-O0`):

| Count | Site | Kind |
| --- | --- | --- |
| ~10 | a typed fat-closure call (`TUR_APPLY1_T(tur_adt_Option__float, double, ...)`, `(void * (*)(void*, int64_t))f[0]`) whose slot 0 holds the generic word shim: the typed shim is declined for a <= 16-byte app result because rank-2 erased consumers call the SAME slot through the word cast -- two consumers, one slot, a design question rather than a missed bridge | typed vs erased consumer |
| ~8 | fixtures' own inline C (`call-thin`, `call-s`, `call-pred`, `call-fat`) casting a closure to a signature of its choosing | user inline C |
| 5 | `__inst_Functor_fmap_Identity`: `g.fn(g.env, x)` on a carrier whose fn is not all-word (a path the fat-box adapter does not see yet) | M4 |
| ~10 | runtime callbacks: timer wheel, serializer `r->ser`, image registry `TUR_APPLY0`, `tur_async_fiber`, `fs-write` | runtime typedefs |
| ~8 | `TUR_APPLY1_T` / thin `call-*` helpers in fixtures' own inline C | typed slot vs erased callee |
| rest | one-offs: existential witness `puts(...)`, `apply-mw`, `future-then`, `__tur_poly_to_fat1` | mixed |

## Fixed on the way (not open)

The sweep and the widened fuzzers found these, and all are fixed with
fixtures:

- `saffron-dyn-witness-fn-arity-defaults-unary` (archived): the unary
  `g : fn` case printed a TYPE TAG.
- a bare `fn` struct field returned a float's BITS; storing a float-signature
  function there, or calling one with a float argument, is now rejected, as
  the `:fn` parameter carrier already was
  (`errors/fn-field-bare-float-{store,arg}-rejected`);
- an effectful TYPED fn field aborted ("no CPS entry registered"), and its E2a
  call value-converted a float argument into the wrong register
  (`fn-field-typed-float`).
- a lambda literal lifted ONCE with the carrier signature was called in a spec
  through a pointer typed from the arguments' resolved types:
  `((fn [y : A] : A y) x)` printed `0` at float32 (the float went in `xmm0`, the
  thunk read `rdi`), and a by-value struct argument segfaulted.  The call now
  follows the callee's EMITTED signature -- the fix direction below, applied to
  the call-head path -- and a closure literal's fat dispatch takes its return
  slot from the clone's recorded signature
  (`generic-spec-carrier-crossings-2`).

## Fix direction

One decision per call shape, keyed on the callee's EMITTED signature, not on
the erased type at the call. That is the same lesson as
[repr-decision-function-plan](../archive/repr-decision-function-plan.md). The
concrete moves, largest cluster first:

1. The session wrapper calls `void (*)(void *)`: `session-spawn` takes
   `(fn [] nil)`.
2. The boxing site picks a typed fatshim whenever the call site casts typed
   (`TUR_APPLY<N>_T`). Today `ensure_typed_fatshim` declines every all-word
   signature, and the call site still casts `bool` or a pointer type.
3. A variadic's rest slot has one C spelling, at the definition and in the
   shim.
4. The stdlib inline-C comparators and callbacks take a real function type
   (the `stdlib-int-stand-in-audit` S1 subset is the same list).

When the sweep is at zero: add the flags above as a Linux CI leg, flip
`TUR_FUZZ_FNSAN_STRICT` on by default, and ratchet it like the
`-Wfloat-conversion` check in `tests/run.sh`.
