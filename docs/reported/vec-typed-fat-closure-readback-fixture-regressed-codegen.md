---
title: vec-typed-fat-closure-readback fixture regressed at C codegen (4 int<->ptr<void> conversion errors)
category: Reported
severity: medium
description: `tests/fixtures/vec-typed-fat-closure-readback/` -- the fixture PR #288 added to lock in the Vec<typed fat closure> fold shape -- fails on `main` (commit 6a5f149a, on a fresh Debug `build/tur`). Elaboration succeeds, but the emitted C produces four `-Wint-conversion` errors at the apply-sf body, the let-binding store, and both `vec-push!` call sites. The library-side definition of `effects-chain` itself (in `../turmeric-spices/spices/signal/src/signal/compose.tur`) checks clean -- only the end-to-end caller pattern (push SF results into a Vec, then fold-apply) regresses.
---

# `vec-typed-fat-closure-readback` fixture regressed at codegen

> **Status (2026-06-05): mostly RESOLVED.**
> - The `tests/fixtures/vec-typed-fat-closure-readback/` fixture **passes** on
>   current HEAD (the original four `-Wint-conversion` mismatches are gone).
> - The broader **`^fat`-arg call-slot** gap (the first Update below: applying
>   `(sine ...)` to a `:ptr<void>` signal box through a `^fat` param) is now
>   **fixed**. The closure-dispatch path in `emit_expr.c` only bridged
>   `:ptr<void>`/`:fn` actuals when the formal kind was `TY_INT`; a `^fat`
>   closure param is declared `TY_FN` but emitted as the `int64_t` carrier
>   (`emit_fns.c`: closure params of kind `TY_FN` -> `int64_t`), so the void*
>   flowed into the int64_t slot uncast. The fix treats `TY_FN` formals as
>   int64_t carriers in that bridge. Regression fixture:
>   `tests/fixtures/sf-apply-fat-arg-bridge/`.
> - The **third pattern** (a closure that *returns* a typed aggregate, applied
>   through a dynamically-dispatched `:ptr<void>` box, then field-accessed) is
>   **still broken** -- see the "Remaining gap" section at the bottom. It is a
>   distinct code path (fat-dynamic-dispatch struct readback), not the
>   `^fat`-arg-slot path fixed above, and now manifests as a runtime
>   **segfault** rather than the compile error originally reported.

## Update (2026-06-05): the same gap blocks any caller of an `^fat`-taking SF

The same int↔ptr<void> carrier-bridge gap surfaces in the **simplest**
downstream caller pattern -- not just inside a Vec fold. Writing an
example in `../turmeric-spices/spices/signal/examples/` of the form

```turmeric
(defmodule signal/examples/probe
  (import signal/core :refer [constant sample])
  (import signal/osc  :refer [sine]))

(defn main [] : int
  (let [unused (constant 0.0)
        s1     ((sine 1.0 0.0) unused)]   ;; SF application
    (println (sample s1 0.25)))
  0)
```

produces the same `incompatible integer to pointer conversion passing
'int64_t' to parameter of type 'void *'` error -- this time at the SF
application site `((sine 1.0 0.0) unused)`:

```
__t58 = signal__osc____fn_941(
    (void *)(intptr_t)(_un_uncall_unhead_un994_995),  ;; env (bridged)
    unused_993);                                       ;; sig (NOT bridged)
note: passing argument to parameter 'sig' here
static void * signal__osc____fn_941(void * __env_p, void * sig) { ... }
```

So the scope is broader than originally reported: **any call site
that passes a `^fat`-typed closure as an argument to another closure
declared `(fn [ptr<void>] ...)`** misses the int->void* bridge cast
on the value slot. The Vec-fold case is just the first place it was
caught.

In practice this blocks the rebuild plan's Phase 2-5 examples
(oscillators, filters, shapers, envelopes -- anything that applies
an SF to a signal). The library-side `tur check` is clean for all of
this; the gap is exclusively in the carrier-bridge emit pass.

A diagnostic data point: the `tests/fixtures/typed-signal-smoke/`
fixture (G8 readiness probe) **does** exercise this exact shape and
passes. It uses leading-colon annotations `(fn [:float] #{} :float)`
and bypasses defmodule scoping; the failing callers use the spaced
form `(fn [float] #{} float)` and live inside a `(defmodule ...
(import ...))`. Worth checking whether one of those two differences
gates the bridge cast.

A third call-site pattern is also blocked by what looks like the
same gap: a closure that *returns* a typed aggregate (e.g.
`(Pair float float)`) loses the struct return type at the caller
side. Example:

```turmeric
(let [ps (pair-signals (constant 1.0) (constant 2.0))
      p  (ps 0.0)]            ;; ps returns (Pair float float)
  (println (pair-fst p)))     ;; expects Pair__float__float, gets int64_t
```

emits:

```
error: passing 'int64_t' to parameter of incompatible type
       'Pair__float__float'
4546 | printf("%g\n", (double)(pair_fst__spec__double_Pair__float__float(p_922)));
note: passing argument to parameter here
2664 | static double pair_fst__spec__double_Pair__float__float(Pair__float__float);
```

The let-bound `p` is in the `int64_t` carrier but the
struct-returning `pair-fst` specialisation needs the unboxed struct.
Same shape gap as the `^fat`-param case: the value arrives as the
int64 carrier at a call site that wants the typed shape, and no
bridge cast is emitted. (The G4 fixture
`tests/fixtures/pair-signals-typed/` passes -- it doesn't go through
an imported module + let-binding combination, which is the trigger
here.)

## Summary

The fixture added by PR #288 to validate the Vec<typed fat closure>
fold shape -- the natural `effects-chain` / SF-pipeline shape -- now
fails. The five elaboration gaps PR #288 fixed remain fixed (after a
fresh build of `tur` against the current `src/`, elaboration of the
SF/loop/chain trio succeeds). The regression is on the codegen side:
the emitted C contains four `-Wint-conversion` mismatches around the
fat-closure carrier (`int64_t` vs `void *`) at the apply-sf body and
the `vec-push!` of an SF result.

## Severity

Medium. The shape is the documented one for SF pipelines (the rebuild
plan's Phase 5, [[tur-signal-rebuild-plan]]); the library-side
`compose.tur` definition of `effects-chain` checks clean and can ship,
but no caller can actually push SFs into a Vec and invoke the chain
until this is unblocked.

## Observed vs. expected

### Observed

```
$ TUR_TEST_FILTER='^vec-typed-fat-closure-readback$' bash tests/run.sh
...
/tmp/tur-build/.../input_tur.c:4488:98: error: incompatible integer to pointer
  conversion passing 'int64_t' (aka 'long long') to parameter of type 'void *'
    [-Wint-conversion]
 4488 | return (*( tur_thunk_void___void___t *)((void *)(intptr_t)(sf)))
        ((void *)(intptr_t)(sf), sig);
/tmp/tur-build/.../input_tur.c:4494:19: error: incompatible integer to pointer
  conversion assigning to 'void *' from 'int64_t' (aka 'long long')
    [-Wint-conversion]
 4494 | __t27 = sig;
/tmp/tur-build/.../input_tur.c:4532:34: error: incompatible pointer to integer
  conversion passing 'void *' to parameter of type 'int64_t' (aka 'long long')
    [-Wint-conversion]
 4532 | vec_hypush_ex(v_914, gain(2.0));
note: passing argument to parameter 'val' here
 3695 | static void vec_hypush_ex(int64_t v, int64_t val) {

summary: 0 passed, 1 failed
  - vec-typed-fat-closure-readback (build failed)
```

### Expected

PASS (the fixture expects `6` on stdout: `2 * 3 * 1.0`).

## Reproducer

`tests/fixtures/vec-typed-fat-closure-readback/input.tur` exactly as
shipped in PR #288. No edit required to repro.

To re-run just this fixture:

```sh
cmake --build build -j --config Debug
TUR_TEST_FILTER='^vec-typed-fat-closure-readback$' bash tests/run.sh 2>&1 | tail -25
```

## Root-cause analysis (best guess)

The four errors split into two groups:

1. **`apply-sf` body (lines 4488, 4494).** The fixture declares
   `^fat sf : (fn [:ptr<void>] #{} :ptr<void>)` and
   `^fat sig : (fn [:float] #{} :float)`. The emitted call site casts
   slot 0 to `void *` and slot 1 to the SF's parameter ABI -- but the
   carrier for `sig` is being passed as `int64_t` (the thunk-ABI
   carrier) into a `void *` slot, then later assigned in the reverse
   direction. The fat-closure carrier picks `int64_t` on the call-site
   side and `void *` on the parameter-binding side; a recent bridge
   commit (#287 "Bridge int<->pointer carrier casts for closure
   return/arg emit") closes most of this but is apparently missing the
   `^fat`-parameter case here.

2. **`vec-push! v (gain N)` (lines 4532, 4533).** `gain` is declared
   to return `:ptr<void>` (the canonical fat-closure box type per
   PR #288's pattern), but the homogeneous-vec push helper
   `vec_hypush_ex` takes `int64_t`. The Vec's element-type carrier
   needs an int↔ptr<void> bridge cast at the push site -- the same
   shape as the dispatch-side bridge in #287, but on the push side.

The fact that PR #288's fix lands on the elaborator side and #287's
carrier-bridge fix lands on closure return/arg emit, and the surviving
gaps are at `^fat`-param call slots and the homogeneous-vec push slot,
strongly suggests a missed call/push pathway in the carrier-bridge
pass.

## Proposed fix direction

Extend the int↔ptr<void> bridge cast emission to cover:

1. The `^fat`-param-call-site slot for closures whose param ABI is
   `:ptr<void>` (so `(sf sig)` in `apply-sf` casts `sig` from its
   `int64_t` carrier to `void *`).
2. The `vec_hypush_ex` value slot when the source expression has type
   `:ptr<void>` (cast to `int64_t` at the push site).

A non-intrusive option for (2) is to emit a `(int64_t)(intptr_t)`
cast on the value argument inside `vec_hypush_ex_emit_call` whenever
the source TypeKind is a pointer; that mirrors what the dispatch-side
bridge already does for the closure result.

## Validation of a fix

- `bash tests/run.sh 2>&1 | grep FAIL` is empty.
- `TUR_TEST_FILTER='^vec-typed-fat-closure-readback$' bash tests/run.sh`
  exits 0, stdout matches `expected.stdout` (`6`).
- An end-to-end smoke from `../turmeric-spices/spices/signal/`:
  `vec-push!` two `(gain ...)` SFs into a vec, run
  `(effects-chain v (constant 1.0))`, re-bind the result with
  `^fat out : (fn [float] float)`, and `(out 0.0)` returns the product
  of the gains. No `-Wint-conversion` in the emitted C.

## Remaining gap (still open): aggregate-returning closure read back through a `:ptr<void>` box

A closure that **returns a typed aggregate** (e.g. `(Pair float float)`),
stored as a `:ptr<void>` box, applied via dynamic fat-dispatch, and then
field-accessed, is still broken. Minimal standalone repro (no spices repo
needed; `Pair`/`pair`/`pair-fst` are auto-loaded from `stdlib/pair.tur`):

```turmeric
(defn pair-signals [a : float b : float] : ptr<void>
  (fn [t : float] : (Pair float float) (pair a b)))

(defn main [] : int
  (let [ps (pair-signals 1.5 2.5)
        p  ((:: ps (fn [float] #{} (Pair float float))) 0.0)]
    (println (pair-fst p)))
  0)
```

- **Observed (current HEAD):** compiles, then **segfaults** at runtime
  (exit 139). This is a *change* from the originally-reported symptom (a
  hard `-Wint-conversion` / `incompatible type 'Pair__float__float'`
  compile error); intervening commits (#289-#291) moved it from a compile
  error to a silent miscompile + crash, which is arguably worse.
- **Expected:** prints `1.5`.

This is a **different code path** from the `^fat`-arg-slot bridge fixed
above. The apply site `((:: ps ...) 0.0)` dispatches through the
`TY_PTR_VOID` fat-dynamic-dispatch branch in `emit_expr.c` (the
`(*( thunk_typedef *)(fn_ptr))(fn_ptr, args...)` path), and the struct
return value comes back through the int64_t carrier without being
reconstituted into the by-value `Pair__float__float` the `pair-fst`
specialization expects. The G4 fixture `tests/fixtures/pair-signals-typed/`
passes because it does not go through the dynamic `:ptr<void>` box +
`::`-ascribed re-application combination that triggers this.

Confirmed independent of the `^fat`-arg-slot fix: the segfault reproduces
with that `emit_expr.c` change reverted, so this is a pre-existing,
orthogonal gap in the fat-dynamic-dispatch struct-return readback.

**Fix direction (next session):** in the `TY_PTR_VOID` fat-dispatch branch
of `emit_expr.c`, when the dispatched closure's result type is a by-value
carrier-ABI aggregate, bridge the int64_t carrier result back to the
concrete struct (mirror `emit_carrier_bridge` `CK_CARRIER -> CK_CONCRETE`
the way the direct-call path does) before the field access consumes it.

## Related

- PR #288 (commit `6a5f149a`) -- shipped the fixture and the
  five-bug elaborator fix.
- PR #287 (commit `9c9a90bf`) -- "Bridge int<->pointer carrier casts
  for closure return/arg emit"; covers part of the same surface.
- PR #285 (commit `1a4bff30`) -- "Phase 3: deprecate leading colons
  inside `(fn ...)` types". The fixture uses leading colons; that
  emits TUR-D0001 but is not the source of the codegen error
  (rewriting to spaced annotations does not change the result).
- [[tur-signal-rebuild-plan]] -- Phase 5 of the rebuild needs this
  shape to ship cleanly.
- `../turmeric-spices/spices/signal/src/signal/compose.tur` (commit
  in the spices repo) -- ships `effects-chain` against this shape;
  library-side `tur check` clean; caller-side blocked by this report.
