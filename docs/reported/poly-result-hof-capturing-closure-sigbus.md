# Polymorphic-result HOF + capturing closure -> runtime SIGBUS

**Severity:** medium (miscompile: clean compile, crashes at run time; a
monomorphic-result workaround exists).

## Summary

A **capturing** closure passed to a higher-order function whose result type is
a **type variable** (`(fn [] R) ... : R`) compiles without error and then
crashes at run time (SIGBUS, exit 138). A non-capturing closure is fine, and a
monomorphic result (`(fn [] int) ... : int`) with the same capture is fine --
so the trigger is specifically *result-tyvar HOF + closure environment*.

## Minimal repro

```turmeric
(defmodule polybug (export)
;; Polymorphic HOF whose result is the type variable R.
(defn run [R] [body : (fn [] R)] : R (body))
(defn main [] : int
  (let [k 7]
    ;; Capturing closure (captures k) invoked through run: crashes.
    (let [r (run (fn [] : int (+ k 1)))]
      (do (println r) 0))))
)
```

```
$ tur run polybug.tur   ; compiles clean, then:
rc=138   (SIGBUS), no output
```

## Controls (isolate the trigger)

| variant | result |
| --- | --- |
| result is a tyvar `R`, closure **captures** `k` | **rc=138 SIGBUS** |
| result monomorphic `int`, closure captures `k` (`(defn run [body : (fn [] int)] : int ...)`) | rc=0, prints 8 |
| result tyvar `R`, **non-capturing** closure (`(fn [] : int 42)`) | rc=0, prints 42 |

Keeping an *argument/phantom* type variable is fine; only the **closure
result** being a tyvar triggers it. E.g.
`(defn f [W] [^borrow t : (Cap W) body : (fn [] int)] : int (body))` runs; the
same with `[W R] ... (fn [] R) : R` crashes.

## Where it bites

Discovered building phase B1 of the stateful-refinement plan
(`docs/upcoming/v1/refine-stateful-measures-plan.md`): a `with-frozen` region
combinator wants the natural polymorphic signature
`[W R] [^borrow tok : (DespawnCap W) body : (fn [] R)] : R`, but that shape
crashes with a capturing region body (the common case -- the body reads the
world it closed over). Worked around there by fixing the body result to `int`
(`turmeric-spices/spices/ecs/src/ecs/freeze.tur`), which is why B2's region form
should not be a HOF (or this must be fixed first).

## Likely root cause (direction, not verified)

The crash smells like the fat-closure ABI colliding with the
per-monomorphization result register class: when the HOF's result is a tyvar,
the closure's boxed environment and the erased int64-carrier result path are
reconciled differently than for a ground result, and the env pointer or the
result slot is read at the wrong width/offset. Compare the working monomorphic
path in `emit_expr.c`'s closure-call lowering against the tyvar-result path;
the memory notes on poly-closure-result specialization
(`closure_return_dispatches*` on `Binding`) are the neighbouring machinery.

## Reproduce

`tur run` the snippet above with any current `build/tur` (seen at v0.31.0).
The monomorphic control in the table is the fastest confirmation it is the
result tyvar and not the capture.
