---
title: Two-level SF closure return miscompiles the let-bound result (out binding typed as a raw C function pointer)
category: Reported
severity: high
description: A Signal-Function shape `(make-sf)` -> `(fn [^fat sig] (fn [t] (sig t)))`, let-bound and piped (`(let [sf (make-sf) out (sf input)] (out t))`), now type-checks (after the let-bound-sf-loses-outer-arg-type fix) but fails native codegen. `emit-c` declares the let binding `out` as a bare C function pointer `double (*out)(double)` and casts the producing call `(sf input)` to return a scalar `double`, even though `(sf input)` returns a *capturing closure* (fat box). The generated C is rejected by the C compiler ("incompatible types ... using type 'double'") for the no-capture case, and miscompiles + segfaults for the capturing case. The tree-walking interpreter runs the same source correctly.
---

# Two-level SF closure return miscompiles the let-bound `out` result

> **Resolution:** Fixed. Two coordinated changes:
> 1. `src/compiler/elab_call.c` (`^fat` argument handling): an already-fat
>    value -- a `^fat` parameter (or let-alias of one), marked `is_fat`, whose
>    declared type is a concrete `(fn ...)` -- is now retyped to the
>    `:ptr<void>` carrier before the bare-fn auto-shim. Previously it took the
>    `ak == TY_FN && !boxed` shim branch and got wrapped in a *second*
>    `__tur_fatshim` adapter (double-boxing), so the capturing variant
>    dispatched through the inner box's first word as code and segfaulted. It
>    now flows through the already-fat pass-through, exactly like a bare `^fat`
>    parameter (which carries `TY_PTR_VOID` and already worked).
> 2. `src/compiler/emit_expr.c` (thin local-fn call codegen): when such a call
>    returns a *function value* (`(sf input)` whose static result is the inner
>    `(fn [float] float)`), the cast's return type now uses the matching fn-ptr
>    typedef (`tur_fnptr_double_double_t`) instead of `type_c_name(TY_FN)`,
>    which collapsed a non-boxed primitive-result fn to `double` and produced
>    the `cc` "incompatible types ... using type 'double'" error at the `out`
>    binding (no-capture variant). Boxed-closure results already lower to
>    `void *` and are untouched.
>
> Validated by `tests/fixtures/sf-let-bind-with-inner-call/` (build+run, prints
> `7`) alongside the existing check-time guard
> `tests/check-sf-let-bind-inner-call.sh`. The
> `voice`/`voice-sf`/`poly-synth` SF-composition bodies in
> `../turmeric-spices/spices/signal/src/signal/synth.tur` can now be restored.

## Summary

After the type-checker fix for
[let-bound-sf-loses-outer-arg-type-when-inner-captures](let-bound-sf-loses-outer-arg-type-when-inner-captures.md),
the idiomatic SF "build it, let-bind it, pipe a signal in, sample the
result" shape passes `tur check` but does **not** survive `tur emit-c` /
`tur build`. The native backend treats the closure *returned by* a
let-bound call as a thin C function pointer instead of a fat closure box.

Distilled while executing the above report. The type-check half is fixed;
this is the remaining codegen half that blocks restoring the
`voice`/`voice-sf`/`poly-synth` SF-composition bodies in
`../turmeric-spices/spices/signal/src/signal/synth.tur`.

Severity: **high.** It is a hard `cc` error (no-capture variant) or a
silent miscompile + segfault (capturing variant) on a shape the language
now accepts at check time, so the program is un-buildable.

## Observed vs. expected

### Expected

For

```turmeric
(defn make-sf-no-call []
  (fn [^fat sig : (fn [float] float)]
    (fn [t : float] : float 0.0)))
(defn scale2 [x : float] : float (* x 2.0))
(defn drive [^fat input : (fn [float] float)] : float
  (let [sf  (make-sf-no-call)
        out (sf input)]
    (out 3.5)))
(defn main [] : int (println (drive scale2)) 0)
```

`out`'s C type should be the closure carrier (a fat box / `void*` /
`int64_t`), `(sf input)` should be called so it returns that carrier, and
`(out 3.5)` should fat-dispatch through the box's slot 0. The interpreter
already does the right thing:

```
$ ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret /tmp/sf.tur
7
```

### Observed

```
$ ./build/tur run /tmp/sf.tur
.../sf.c:4489:37: error: incompatible types when initializing type
  'double (*)(double)' using type 'double'
 4489 |   double (*out)(double) = ((double (*)(void *))(intptr_t)sf)(__t27);
```

The capturing variant (inner body `(sig t)`) gets past `cc` with an
`-Wint-conversion` warning and then **segfaults** at runtime.

## Root cause analysis

`emit-c` lowers the offending `let` body (from `tur emit-c`) to:

```c
static int64_t make_hysf_hyno_hycall() {
        return __fn_895;                 /* outer fn, a thin fn pointer */
}
static double drive(int64_t input) {
        double __t25;
        {
            int64_t sf = (int64_t)(intptr_t)(make_hysf_hyno_hycall());
            int64_t *__t26 = malloc(2 * sizeof(int64_t));
            __t26[0] = (int64_t)(intptr_t)__tur_fatshim_double_double;
            __t26[1] = (int64_t)(intptr_t)input;
            void *__t27 = __t26;                       /* fat box for sig */
            double (*out)(double) =                    /* (1) WRONG C type */
                ((double (*)(void *))(intptr_t)sf)(__t27);  /* (2) WRONG result type */
            __t25 = ((double (*)(double))(intptr_t)out)(3.5);  /* (3) WRONG dispatch */
        }
        return __t25;
}
```

Three related mistakes, all stemming from the backend not recognizing that
`(sf input)` yields a **closure value**, not a scalar:

1. `out` is declared `double (*out)(double)` -- a bare C function pointer
   for `(fn [float] float)` -- but the value is a capturing closure (fat
   box), whose carrier is `void*`/`int64_t`.
2. The producing call casts `sf` to `double (*)(void*)`, i.e. a function
   *returning `double`*. `sf : (fn [(fn float float)] : (fn float float))`
   returns a closure, so the cast should return the closure carrier.
3. `(out 3.5)` is emitted as a thin `double(*)(double)` call. A capturing
   closure must fat-dispatch through slot 0 (env + arg), as the `^fat`
   call sites elsewhere already do.

The elaborator now records the right *types* (the fix routes `sf` through
`returns_closure_fn_binding`, so `(sf input)`'s static result is the inner
`(fn [float] float)`), but the inner result type's **`boxed`** flag and/or
the `out` binding's `closure_fn_binding` are not making it to the emit
path, so emit picks the unboxed function-pointer representation.

Files to inspect:

- `src/compiler/emit_expr.c` -- where a `let` init that is a call
  returning a `(fn ...)` chooses the C type of the binding and the cast of
  the producing call. Look for where a boxed TY_FN result should select the
  fat-box carrier + fat dispatch instead of `R (*)(args)`.
- `src/compiler/emit_expr.c:emit_expr_closure_fn_binding` and the fat-call
  dispatch -- whether they fire for a binding whose init is an EX_CALL whose
  callee `returns_closure_fn_binding`.
- The `boxed` propagation on `result_full_type` through the two closure
  levels (`src/compiler/elab_fns.c` lines ~3443-3454 set `clo_ty.boxed =
  true` on the inner closure; confirm it survives onto
  `make-sf`'s `result_full_type->result_full_type`).

## Minimal reproducer

The no-capture variant (`0.0` inner body) gives the clean `cc` error; the
capturing variant (`(sig t)` inner body) gives the segfault. Both share the
`drive` shape above. The type-check guard
`tests/check-sf-let-bind-inner-call.sh` already pins the check-time
behavior; a build+run fixture under
`tests/fixtures/sf-let-bind-with-inner-call/` should be added once this is
fixed (it was intentionally NOT added now because it cannot build).

## Validation of a fix

- `./build/tur run /tmp/sf.tur` prints `7` (matches `--interpret`).
- Add `tests/fixtures/sf-let-bind-with-inner-call/` (input + `expected.stdout`
  of `7`) and confirm `bash tests/run.sh` is green.
- The synth-side stubs in
  `../turmeric-spices/spices/signal/src/signal/synth.tur` (Phase 0c note)
  can be restored to their full SF-composition bodies.

## Related

- `docs/reported/let-bound-sf-loses-outer-arg-type-when-inner-captures.md`
  -- the type-check half, now fixed; this is the codegen half it deferred.
- `docs/reported/stdlib-poly-codegen-undeclared-identifier.md` -- a
  different standalone-build codegen defect in the same neighborhood.
