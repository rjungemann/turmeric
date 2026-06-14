# turi: native HOFs swallow value-level errors from their turi_call callback

**One-line:** Under `tur --interpret`, a native higher-order function
(`option-map`, and most of the ~29 `turi_call` sites in `src/main.c`) propagates
a *throw* from its callback (via `env->throwing`) but **swallows a value-level
error** (a `turi_error(...)` return, e.g. `eval: recursion limit exceeded`):
it reads `rv.as_int` without checking `turi_is_error(rv)`, boxing the error
value into a garbage result. Silent wrong-output (rc=0). Pre-existing; surfaced
while auditing re-entrancy for T3.3 of the eval-trampoline plan.

**Severity:** medium. Not a crash. The common trigger is deep recursion *through*
a native HOF (the recursion-limit error fires inside the callback and is
swallowed -> garbage instead of a clean limit error), but it affects **any**
value-level error a callback returns.

## Minimal repro

```turmeric
;; deep recursion through option-map: the recursion-limit error fires inside the
;; callback at ~1071 cycles and is swallowed by native_option_map.
(defn deep [n : int] : int
  (unwrap-or (option-map (some n)
                         (fn [x : int] : int (if (= x 0) 0 (+ 1 (deep (- x 1))))))
             0))
(defn main [] : int (println (deep 1100)) 0)
```

| n    | output |
| ---- | --- |
| 1050 | `1050` (correct) |
| 1100 | `88167089526540` (garbage, rc=0) -- **wrong**; should be a clean `eval: recursion limit exceeded` |

Contrast a callback that **throws** (sets `env->throwing`), which *does*
propagate cleanly:

```turmeric
(defn boom [x : int] : int (vec-get (vec-new) 999))   ;; out-of-bounds -> throw
(defn main [] : int (println (unwrap-or (option-map (some 5) boom) 0)) 0)
;; prints "vec index out of bounds", rc=1  (propagates fine)
```

So the gap is specifically **value-level `turi_error` returns**, not throws.

## Root cause

`native_option_map` (`src/main.c:5783`):

```c
TuriValue rv = turi_call(env, a[1], &arg, 1);
int64_t *r = (int64_t *)malloc(2 * sizeof(int64_t));
r[0] = 1; r[1] = rv.as_int;            /* <- reads as_int even if rv is an error */
return turi_int((int64_t)(intptr_t)r); /* boxes garbage; error tag lost */
```

`turi_call` -> `eval_apply` -> ... -> `eval_expr`, whose depth guard returns
`turi_error("eval: recursion limit exceeded")` -- a **value**, not a throw, so
`env->throwing` is not set. The HOF only effectively honors `env->throwing`
(checked by the caller after the native returns); a `turi_error` *value* is read
as `rv.as_int` and boxed, losing the error. Throws survive only because the
`env->throwing` flag is checked downstream regardless of the box.

The same pattern appears across the native HOFs / comparators that call
`turi_call` (~29 sites in `src/main.c`: `option-map`/`result-map`-style maps,
sort/`min-by` comparators, `seq`/`gen` drivers, `fold`-style reducers, etc.).
Any that build a result from `rv` without first checking `turi_is_error(rv)`
(and `env->returning`) have the same hole.

## Relationship to the trampoline (T3.2b)

Pre-existing and orthogonal. T3.2b (folding non-tail calls onto the driver
work-stack) only changed the *depth* at which the recursion-limit error fires
for HOF-mediated recursion (the fold removed the per-cycle `eval_depth`
increments the direct call used to add), so the swallowed-error garbage now
appears at ~1100 instead of a lower threshold. The interpreter does **not**
SIGSEGV here -- the `eval_depth` guard (via the re-entered `eval_expr`) bounds
normal-frame HOF re-entry safely; this is purely the error-swallowing defect.

(Residual, unverified: a native HOF with a *very large* C frame could in
principle overflow the C stack before the `eval_depth` guard trips at ~1071
cycles. Not observed with the small-frame HOFs tested; noted for completeness.)

## Proposed fix

After every `turi_call` in a native that consumes the result, check for an
error / pending control signal before using it:

```c
TuriValue rv = turi_call(env, fn, args, n);
if (turi_is_error(rv) || env->throwing || env->returning) return rv;
/* ... only now read rv ... */
```

Audit the ~29 `turi_call` sites in `src/main.c` and apply the guard to each that
builds a result from `rv`. A shared helper macro would keep them consistent.

## How to validate a fix

- The `deep 1100` repro above prints `eval: recursion limit exceeded` (rc!=0),
  not a garbage integer.
- A callback returning a `turi_error` value (not a throw) surfaces that error to
  the HOF's caller.
- Full `run-turi.sh` / `run.sh` stay green; `tools/check_turi_parity.py` 0-gaps.
