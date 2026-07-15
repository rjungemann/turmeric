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

## Fix directions

- Short term: audit the fiber emission of a fn-value (`tur_poly_fn_t`) argument to
  a SIG-REJECT callee -- the `void*` assignment (`incompatible types ...
  tur_poly_fn_t`) and the missing temp declaration.
- Structural: E1 carrier-ABI `__cps` emission for non-scalar signatures lets
  `apply-cb` CPS-emit under the carrier ABI, and E2's `__fn_cps` slot threads the
  DK through the effectful callback -- both remove this direct-path crossing.
