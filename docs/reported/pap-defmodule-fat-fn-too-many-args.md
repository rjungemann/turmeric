---
title: Sibling forward-reference inside defmodule generates PAP wrapper with one extra arg vs callee signature
severity: hard error -- C compile fails with "too many arguments to function call"
status: open
discovered: 2026-06-09
discovered-in: defect-A regression fixture (defmodule-fat-fn-param-export)
---

# PAP wrapper for a sibling forward-reference in a defmodule passes one extra arg

## Summary

When a `(defn A ...)` inside `(defmodule ...)` forward-references a sibling
`(defn B [^fat f : (fn [float] float) x : float] : float ...)` declared later
in the same defmodule and passes its own `^fat f` parameter through to `B`,
the elaborator synthesises a partial-application (PAP) wrapper whose body
calls `B` with **three** scalar arguments while `B`'s C prototype takes
**two**.  `cc` rejects the file.

## Repro

```turmeric
(defmodule sig/probe
  (export sample apply-twice)

  ;; Forward-references sample, defined later in this defmodule.
  (defn apply-twice [^fat f : (fn [float] float) x : float] : float
    (sample f (sample f x)))

  (defn sample [^fat sig : (fn [float] float) t : float] : float
    (sig t)))

(defn main [] : int
  (let [double-it (fn [x : float] : float (* x 2.0))]
    (println (apply-twice double-it 1.0))    ;; expects 4
    0))
```

Observed:

```
error: too many arguments to function call, expected 2, have 3
        return sig__probe__sample(__env->__papc892, __env->__papc894, __papr896);
               ~~~~~~~~~~~~~~~~~~                                     ^~~~~~~~~
note: 'sig__probe__sample' declared here
static double sig__probe__sample(int64_t, double);
```

The PAP body is closing over **two** captures (`__papc892`, `__papc894`) and
adding the final arg `__papr896`, totalling three -- but `sample`'s real
signature is `(int64_t, double)`.

Expected output: `4`.

## Likely root cause

The pap-wrapper synthesis treats `sample` as having arity-3 because the
forward-decl binding it consulted (pass-1 / early-update) records an
arity-2 fn whose *full type* is not yet correct (`apply-twice`'s body sees
`sample` before pass 2 finalises the signature).  Specifically the PAP
analysis appears to mis-count by including the `t` parameter twice -- once
as a capture and once as the final arg.

Alternative theory: the PAP env was sized for an arity-3 caller (e.g. some
typeclass-method path is firing) but the eventual call goes to the
direct-defn rather than a dispatch trampoline.

Either way the PAP body needs to match the resolved callee's true arity,
which means the synthesis must read the **post-pass-2** binding type, not
the pass-1 forward-decl shape.

## Workaround

Inline the call site -- do not introduce a partial application of a
sibling forward-reference inside a defmodule.  Single-file mode with both
defns in dependency order also avoids the bug.

## Proposed fix directions

1. Defer PAP wrapper synthesis until after pass 2 fills in the resolved
   binding's `arg_full_types` / arity.
2. Or: when emitting the PAP body, look up the callee binding fresh and
   trust its current `type.as.fn.arity`, not whatever arity the PAP env
   was sized for.
3. Add an internal assertion in the PAP emitter:
   `assert(callsite_arity == callee->type.as.fn.arity)` so future
   instances surface as a hard error rather than as `cc` cascade.

## Validation

- Add a fixture `tests/fixtures/defmodule-pap-forward-ref-fat-fn/` exercising
  the apply-twice shape above; expect output `4`.
- Re-run the signal spice (`tur build .` in `../turmeric-spices/spices/signal`)
  and verify no fixtures regress.

## Cross-references

- `[[defmodule-export-scoping-track]]` Defect A -- the elab-side fix for that
  defect (always populate `param_poly_types` for `^fat` TY_FN) is what made
  this PAP shape *try* to compile; before it, the forward-ref errored out at
  elab.  So this is "next layer" of the same workflow.
