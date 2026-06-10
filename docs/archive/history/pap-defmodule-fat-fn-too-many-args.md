---
title: Sibling forward-reference inside defmodule generates PAP wrapper with one extra arg vs callee signature
severity: hard error -- C compile fails with "too many arguments to function call"
status: fixed
discovered: 2026-06-09
fixed: 2026-06-10
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

## Root cause (confirmed)

The defmodule pass-1 forward-declaration in `elab_module.c` (and the
identical top-level pre-pass in `elab_toplevel.c`) counted arity by walking
the params vector and incrementing for every form that is not a type
annotation:

```c
if (p->tag != F_KEYWORD && p->tag != F_TYPE_ANN)
    param_arity++;
```

A `^fat` (or `^mut`, `^borrow`, `^linear`, ...) annotation is an `F_SYM`, not
a keyword or type-ann, so it was **counted as a parameter slot**.  For
`(defn sample [^fat sig : (fn [float] float) t : float] ...)` this yields
arity **3** (`^fat`, `sig`, `t`) instead of 2.

When the sibling `apply-twice` -- declared *earlier* in the same defmodule --
forward-references `sample`, it sees the pass-1 binding (the HRT5 early-update
that would fix the arity has not run yet, because `sample` is elaborated
later).  A saturated 2-arg `(sample f x)` then looks under-saturated (2 of 3),
so `elab_partial_apply` synthesises a PAP wrapper.  The wrapper closes over the
fat fn `f` (one logical arg) plus `x`, then appends the "remaining" arg, and
calls the real arity-2 `sample` with three scalars -- the `cc` error.

A second, latent defect was hiding behind the first: the forward decl filled
all `arg_kinds` with the `TY_INT` placeholder.  Once the arity was corrected,
the now-saturated call type-checked `x : float` against `int` and reported
`arg 2: expected int, got float`.  The wrong arity had been masking this hole
by routing the call through the partial-application path, which skips the
positional kind check.

## Likely root cause (original triage)

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

## Fix

A shared helper `fwd_decl_scan_params` (in `elab_core.c`, declared in
`elab_internal.h`) now performs the pre-pass param scan in one place:

- skips `^`-prefixed marker symbols so they no longer inflate the arity, and
- records primitive scalar argument kinds (float/bool/cstr/sized numerics/...)
  from each param's annotation, closing the `TY_INT`-placeholder hole;
  compound/unknown types stay `TY_INT` (resolved later by the HRT5 early
  update).

All three previously-duplicated pre-pass loops call it: the defmodule pass-1
forward decl (`elab_module.c`), the top-level pre-pass (`elab_toplevel.c`),
and the `letrec` fn-literal pre-register (`elab_forms.c`).

## Workaround (no longer needed)

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

- Added fixture `tests/fixtures/defmodule-pap-forward-ref-fat-fn/` exercising
  the apply-twice shape above; output is `4` and the generated C contains no
  `__pap` wrappers (the call lowers to a direct saturated
  `sig__probe__sample(...)`).
- Full `bash tests/run.sh` passes with zero `FAIL` lines.

## Cross-references

- `[[defmodule-export-scoping-track]]` Defect A -- the elab-side fix for that
  defect (always populate `param_poly_types` for `^fat` TY_FN) is what made
  this PAP shape *try* to compile; before it, the forward-ref errored out at
  elab.  So this is "next layer" of the same workflow.
