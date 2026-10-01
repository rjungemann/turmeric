# Emitted C makes indirect calls through function pointers of the wrong type

**Severity: high (WASM), medium (native).** Undefined behaviour in C on every
target. On SysV/AAPCS most instances are ABI-benign: `bool` vs `int64_t`, or a
pointer vs `int64_t`, rides the same register. But WebAssembly's
`call_indirect` checks the exact signature, so each one is a trap on the web
build. And the same *mechanism* is what produced this repo's longest-running
silent-wrong-answer family, when the type that differs is a `double` or a
16-byte tagged `any`. Filed 2026-09-30 with the P0 representation-confusion
work.

**Status: OPEN, being swept (16 trapping fixtures as of 2026-10-01, sixth sweep; every fixture now counted).** The detector is armed in the four source
fuzzers (`tests/fuzz_arm.py`, report-only `FNPTR_TRAP` until this reaches
zero). It is **not** yet a gate on the fixture suite.

## How to see it

clang's `-fsanitize=function` compares every indirect call with the callee's
definition. Trap mode needs no UBSan runtime:

```sh
# An unsanitized libturi.a, so clang can link the fixtures that use it
# (the Debug one is built with gcc's ASan, which clang cannot link):
cmake -S . -B build-nosan -DCMAKE_BUILD_TYPE=Debug -DTUR_DEBUG_SANITIZE=OFF
cmake --build build-nosan -j --target libturi
CC=clang TUR_CC_FLAGS="-O2 -std=c99 -Wall -Wfloat-conversion \
  -Werror=implicit-function-declaration -fno-strict-aliasing \
  -fsanitize=function -fsanitize-trap=function -L$PWD/build-nosan/src" \
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
| sixth (2026-10-01) | 57 -> 16 | **First sweep that counts every fixture**: `-L` points at an UNSANITIZED `libturi.a` (`cmake -S . -B build-nosan -DTUR_DEBUG_SANITIZE=OFF && cmake --build build-nosan --target libturi`), so the ~56 fixtures that failed to link under clang before are in -- 57 trapping at the start.  Fixed: a rank-2 `__poly_N` wrapper / capturing closure packed into a FORALL or erased carrier-base sink gets an adapter at the call site's convention (narrow "phase F" or word, per position; `ensure_named_call_adapter` / `ensure_call_adapter_ex`); a bare fn boxed for a `^fat` sink is spelled as the callee THE CALL SELECTS reads it -- a concrete parameter type, a spec clone's instantiated type, or words at the type-variable positions of a carrier base / inline-C body (`fn_to_fat_.sink_fn_type`, `ctx->fat_box_sink_type`) -- whenever the box's default choice would spell it differently; runtime callbacks (timer wheel `tur_scheduler_unpark_cb`, serial-registry ser/deser adapters, capability FileSystem vtable, image registry, `future-then`, a `nil` `(async ...)` body); a cloneable continuation's named receiver is called at its recorded result type; fixtures' own inline C calls each closure at its emitted type |

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

Remaining after the sixth sweep (16 traps; callee read with gdb `info symbol`
on the slot the trapping call reads).  Every one is the same disagreement the
fix direction names -- ONE slot, and a producer and a consumer that each
decided its spelling from different information:

| Fixtures | Producer in the slot | Consumer's cast | Why they disagree |
| --- | --- | --- | --- |
| `option-niche-vec-closure-cmp`, `niche-elem-comparator-conventions` | typed fatshim `int64_t (void*, void*, void*)` (Option niche elements are pointers) | stdlib inline C (`vec-eq?`; `map-eq-raw?` behind `map-eq?`), all-word | the parameter is an UNTYPED `^fat cmp-fn`, so nothing says how the C calls it (one inline-C body calls an untyped `^fat` at `TUR_APPLY1_T(double, ...)` -- treating untyped as all-word produced a wrong answer in `tur-apply-t-fatshim-float` and was reverted); `map-eq?` forwards the same box from a Turmeric generic into the inline C.  `list-eq?` (typed sink) is fixed |
| `fat-captureless-closure-ptr-void`, `fat-dispatch-parametric-monomorph-return` | generic `__tur_fatshim1` | typed (`tur_thunk_void___void___t`, `tur_adt_Box2__int (*)(...)`) | typed shim declined: a <= 16-byte app result keeps the word shim for rank-2 erased consumers -- two consumers, one slot |
| `vec-captureless-fat-closure-readback`, `vec-typed-fat-closure-readback`, `fat-shim-void-ptr-arrow-compose` | the lambda's own thunk (`void *__fn_25(void *, int64_t)`) | typed at the CALL's argument types (`void *(*)(void*, void*)`) | the lambda's parameter was emitted as the word, the call site spells it from the argument |
| `region-scope-void-body` | a `void` lambda thunk | `bt-scope`'s carrier base, `int64_t (*)(void *)` | a `nil` result into an erased `(fn [] A)` |
| `hkt-cata-fmap-byvalue-carrier` | `__tur_widen_*` (slot-0 narrow widen, returns `int64_t`) | a by-value spec, `bool (*)(void*, tur_adt_Re)` | the typed carrier arm casts a narrow result unwidened.  An adapter at the pack site does not reach it: the closure is a hoisted `__borrowc` let, so EX_POLY_WRAP takes the raw slot-0 branch with no thunk binding in hand (tried and reverted) -- the consumer is the side to change: spell the result with `thunk_result_slot_c_spelling` and narrow, as the fat-call fallback does |
| `fn-value-carrier-fat-seams` | `bool __fn(void *)` | `__tur_poly_to_fat0_bool` reads slot 1 as `bool (*)(void *)` with the env | poly-to-fat shim spelling vs the thunk |
| `fn-field-carrier-shim-read-typed` | generic shim | `TUR_APPLY1_T(tur_adt_Option__float, double, ...)` | as the <= 16-byte row |
| `lens-compose-wide-byvalue-get-put`, `stdlib-lens-record-field` | lens thunks | typed thunk typedef in a by-value lens spec | lens mono bodies call the setter at the spec's types |
| `annotated-fat-lambda-param`, `sf-let-bind-with-inner-call` | a lambda / let-bound fn | a THIN call through `run` / `sf` as `int64_t (*)(void *, int64_t)` / `void *(*)(void *)` | thin-vs-fat call spelling at an annotated lambda parameter |
| `van-laarhoven-lens-wide-compose` | closure into `__inst_Functor_fmap_Identity__spec__...` | that spec, word | a per-spec instance clone whose `g` stayed erased: `poly_wrap_callee_carrier` is false for a spec, but this one still calls word |

One clang-only wrong answer turned up and was a FIXTURE bug, not a compiler
one: `rc-of-byvalue-aggregate-payload`'s own `tag-of` read an 8-byte word where
the tag is a C `int`, taking in four bytes of struct padding that a struct copy
need not preserve (clang at `-O1`+ copies member-wise).

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
