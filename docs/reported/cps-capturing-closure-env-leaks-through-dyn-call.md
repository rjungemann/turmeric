# A capturing closure passed to a dynamic call from a CPS-lowered function leaks its env

**Severity: low** (a leak, bounded by the number of such calls; no wrong answer).
Found 2026-09-23 while landing proper-tail-calls T6; pre-existing on `main`
(reproduced with the T6 changes stashed).

## Repro

```turmeric
#lang saffron
(defn apply-to [g x] (g x))
(defn step [acc x] (apply-to (fn [y] (+ y x)) acc))
(defn main [] : int
  (println (vec-fold [1 2 3 4 5] 100 step))
  0)
```

Built under AddressSanitizer (`tests/run-leak-check.sh`'s flags), LeakSanitizer
reports `160 byte(s) leaked in 5 allocation(s)`, allocated in `step__cps`: one
32-byte closure env per `vec-fold` element.

## Where

`step` makes a dynamic call (through `apply-to`'s result being CPS-colored),
so it is CPS-lowered. The lambda captures `x`, so it is a heap fat closure
built inside `step__cps` and handed to `apply-to` as an argument. The direct
emitter frees a non-escaping closure env at its `let`'s scope end; the CPS
backend's `reap_env` path (`cps_closure_env_freeable`, `emit_cps_ir.c`) only
covers a leaf-admitted, provably non-escaping env, and a closure passed as a
call argument is neither, so nothing releases it.

## Fix directions

Register the env for the DK entry-boundary reap when the callee is known not
to retain it (the same "non-retaining sink" facts `fn-value-fat-normalization`
uses for stack boxes), or drop it after the consuming call the way the direct
emitter's argument-hoist drain does -- the CPS deferred-drop table already has
the shape (`cps_deferred_capture`).

`tests/fixtures/tailcall-dyn-leak` passes a top-level function instead of a
capturing lambda for exactly this reason; switch it back when this is fixed.
