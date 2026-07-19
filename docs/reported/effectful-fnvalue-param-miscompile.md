# Effectful fn-value passed to a fn-value-param function miscompiles

Summary: passing an EFFECTFUL callback (a fn that `perform`s) as a `(-> int int)`
argument to a function whose signature takes a fn-value, then invoking it under a
`handle`, emits C that does not compile. Severity: medium (blocks the E2
effect-through-fn-value shape from source; pre-existing, orthogonal to E7).

## Minimal repro

```turmeric
(defeffect Ask [] :int)
(defn apply-cb [f : (-> int int) x : int] : int (f x))
(defn cb [x : int] : int (+ x (perform (Ask))))
(defn run [] : int
  (handle (apply-cb cb 10)
    (Ask [] k) (resume k 5)))
(defn main [] : int (println (run)) 0)
```

`tur build` (with OR without `--enable=cps-tramp-resume` -- identical failure, so
this is NOT E7-related) emits:

```
error: incompatible types when assigning to type 'void *' from type 'tur_poly_fn_t'
error: incompatible type for argument 1 of 'apply_hycb'
error: '__t155' undeclared (first use in this function)
```

A NON-effectful fn-value param works fine:

```turmeric
(defn apply-cb [f : (-> int int) x : int] : int (f x))
(defn dbl [x : int] : int (* x 2))
(defn main [] : int (println (apply-cb dbl 21)) 0)   ;; => 42, compiles + runs
```

So the trigger is the interaction of: (a) `apply-cb` SIG-REJECT (non-scalar
signature -- a fn-value parameter), (b) the callback `cb` performing an effect,
(c) the enclosing `handle`. The fiber emission of the fn-value-parameter call in
that combination produces a `tur_poly_fn_t` vs `void*` mismatch and an undeclared
temp.

## Root cause direction

`apply-cb` evicts SIG-REJECT (non-scalar signature) and is emitted by the direct/
fiber path; the fn-value-parameter carrier bridge in that path mis-handles a
`tur_poly_fn_t` value (assigns it to a `void*`) and drops a temp declaration when
the fn-value is effectful. This is exactly the non-scalar-signature carrier-ABI
crossing that v2 plan **E1 (Stage C)** rewrites, and the effect-through-fn-value
channel **E2 (Stage E)** builds on. Fixing it belongs to those stages; recorded
here so the shape is not forgotten.

## Confirmed emit trace (2026-07-19)

Compiling the repro pins the exact shape and shows a compile-only patch is NOT
enough -- it would trade the build error for a runtime miscompile:

```c
static int64_t apply_hycb__cps(tur_poly_fn_t f, int64_t x, DK *__kont) {
    __auto_type __ps_154 = (((int64_t (*)(void*, int64_t))f.fn)(f.env, x)); /* calls f.fn -- the DIRECT thunk */
    ...
}
static int64_t run__cps(DK *__kont) {
    void * __t1;                                                       /* (A) declared void*  */
    __t1 = (tur_poly_fn_t){ NULL, (int64_t(*)(void*,int64_t))__poly_1285 };  /* (B) assign tur_poly_fn_t -> void*  */
    return apply_hycb__cps((int64_t)(intptr_t)__t1, INT64_C(10), __h0);      /* (C) pass int64 into tur_poly_fn_t param */
}
static int64_t __poly_1285(void *env, int64_t x0) { return cb(x0); }   /* (D) poly thunk calls DIRECT cb, not cb__cps */
```

Three distinct defects, only the first two of which are the compile error:

1. **Binder C type (A/B/C).** The temp holding the `tur_poly_fn_t` literal is
   declared `void *` (its `binder_ctype_full` reads `TY_FN` -> `void*`), so the
   struct literal assignment and the `(int64_t)(intptr_t)` arg cast both mismatch
   the `tur_poly_fn_t` param. Declaring the temp `tur_poly_fn_t` and passing it
   unwrapped fixes the two `cc` errors.

2. **`fn_cps` slot never populated (D).** The `tur_poly_fn_t` literal sets only
   `{ env, fn=__poly_1285 }`; the third field `fn_cps` is left zero, and no
   `__poly_1285__cps` variant is emitted (though `cb__cps` DOES exist and `cb` is
   registered via `__tur_cps_register((intptr_t)cb, cb__cps)`).

3. **Callee dispatches the direct slot.** `apply_hycb__cps` invokes `f.fn`
   directly -- never `f.fn_cps` nor the `__tur_cps_register` registry -- so the
   callback's `(perform (Ask))` runs OUTSIDE the DK trampoline installed by the
   enclosing `handle`. Even with (1) fixed, the effect would perform on the wrong
   fiber.

So this is genuinely the **E2** feature: emit a `__poly_N__cps` variant for an
effectful poly-wrap thunk (or register `__poly_N`), populate the literal's
`fn_cps`, and have a fn-value-param `__cps` callee dispatch through `fn_cps` /
the registry when a DK is active. A fix that only corrects the binder type would
compile a program that then performs the effect off-trampoline -- a silent
miscompile, strictly worse than the current build error. Not point-fixable.

## Fix directions

- Short term: audit the fiber emission of a fn-value (`tur_poly_fn_t`) argument to
  a SIG-REJECT callee -- the `void*` assignment (`incompatible types ...
  tur_poly_fn_t`) and the missing temp declaration. **But do not ship this alone:
  without defects 2+3 it converts the build error into a runtime miscompile.**
- Structural: E1 carrier-ABI `__cps` emission for non-scalar signatures lets
  `apply-cb` CPS-emit under the carrier ABI, and E2's `__fn_cps` slot threads the
  DK through the effectful callback -- both remove this direct-path crossing.
