---
title: Captureless closure returned from a defn is a bare fn pointer, not a fat box -- segfaults when dispatched through a ptr<void> ^fat boundary
category: Reported
severity: high
description: A closure that captures nothing (e.g. the SF returned by a nullary `(invert)`) is codegen'd as a bare C function pointer, not a `{ thunk, env }` fat box. When that value is cast to `:ptr<void>` and passed to a `^fat` parameter, elab_call's ^fat handling assumes a TY_PTR_VOID argument is already a fat box and passes it through unshimmed. The consumer then reads the function pointer's code address as if it were slot 0 of a fat box and calls garbage -- segfault. This is distinct from (and survives) the fix in fat-shim-void-ptr-calls-bare-not-fat.md, which fixed *capturing* closures only. It blocks `>>>` (and the existing `__chain-loop`/`__apply-sf` path) for any captureless Signal Function (invert, abs-sf, ...).
---

# Captureless closure not boxed at a `ptr<void>` `^fat` boundary

## Summary

**Severity: High.** A closure with no captured variables is emitted as a
**bare C function pointer**. A *capturing* closure is emitted as a
heap-allocated `{ thunk, env... }` fat box. These two representations are not
interchangeable, but the `^fat` call-site machinery treats any `:ptr<void>`
argument as "already a fat box" and passes it through without a shim. When the
`:ptr<void>` value is actually a captureless closure (a bare fn pointer), the
`^fat` consumer dereferences the code address as slot 0 of a fat box and
fat-calls garbage -> **segfault**.

This is the captureless sibling of
[fat-shim-void-ptr-calls-bare-not-fat.md](fat-shim-void-ptr-calls-bare-not-fat.md)
(resolved for *capturing* closures by PR #302, commit `cdd88a6`). The capturing
case works now; the captureless case still segfaults.

## Minimal repro

`/tmp/sf_direct.tur` -- **segfaults** (captureless SF):

```turmeric
(defn invert []                          ;; captures nothing -> bare fn ptr
  (fn [^fat sig : (fn [float] float)]
    (fn [t : float] : float (- 0.0 (sig t)))))
(defn konst [k : float] (let [kv k] (fn [t : float] : float kv)))

;; apply an SF to a signal through the ptr<void> carrier (the __apply-sf shape)
(defn apply-sf [^fat sf  : (fn [ptr<void>] #{} ptr<void>)
                ^fat sig : (fn [float]     #{} float)] : ptr<void>
  (sf sig))

(defn main [] : int
  (let [^fat out : (fn [float] #{} float)
             (apply-sf (:: (invert) :ptr<void>) (:: (konst 0.5) :ptr<void>))]
    (println (out 0.0)))                  ;; expected -0.5, actual: Segmentation fault
  0)
```

Control -- **works** (force a capture so `invert` becomes a fat box):

```turmeric
(defn invert-cap []
  (let [z 0.0]                            ;; dummy capture -> fat box
    (fn [^fat sig : (fn [float] float)]
      (fn [t : float] : float (- z (sig t))))))
;; ... same apply-sf / konst / main ...   ;; prints -0.5, exit 0
```

The *only* difference is whether the outer `(fn ...)` captures a variable. The
`>>>` compositor from `stdlib/arrow.tur` segfaults the same way on a captureless
SF (`(>>> (gain 2.0) (invert))`), because it stores `f`/`g` and fat-dispatches
them identically.

## Observed vs expected

- **Observed**: `Segmentation fault` (exit 139) at the first fat dispatch of the
  captureless closure.
- **Expected**: `-0.5` (the capturing control prints exactly this).

## Root cause

### Codegen: captureless closure returns a bare fn pointer

`tur emit-c /tmp/sf_direct.tur` produces:

```c
static int64_t invert() {
    return (int64_t)(intptr_t)__fn_898;          // BARE function pointer
}
static void * konst(double k) {
    ...
    struct __env_905 *__t28 = malloc(sizeof(struct __env_905));
    __t28->__fn = (tur_thunk_double_double_t)__fn_903;   // fat box, thunk in slot 0
    __t28->kv  = kv_901;
    return __t28;
}
static void * apply_hysf(int64_t sf, int64_t sig) {
    // dispatches sf as a fat box: reads slot 0 as the thunk
    return (*(tur_thunk_void___void___t *)((void *)(intptr_t)(sf)))(
               (void *)(intptr_t)(sf), (void *)(intptr_t)(sig));
}
```

`apply_hysf` casts `sf` to a thunk-pointer-pointer and reads `*sf` as the thunk.
For `konst` that is the fat box's slot 0 (correct). For `invert` `sf` IS the code
address `__fn_898`; `*sf` reads the first 8 bytes of executable code as a
function pointer and calls it -> segfault.

### Elaboration: the `:ptr<void>` pass-through assumes "already fat"

`src/compiler/elab_call.c:2903-2916` -- the `^fat` argument handler. When the
argument type is `TY_PTR_VOID` it takes the pass-through branch:

```c
} else if (ak == TY_PTR_VOID || (ak == TY_FN && args[i]->type.as.fn.boxed) ||
           ak == TY_NIL || ...) {
    /* Pass through unchanged: a fat closure (TY_PTR_VOID), nil, ... */
}
```

The comment asserts a `TY_PTR_VOID` value "is a fat closure", but
`(:: (invert) :ptr<void>)` is a captureless closure cast to `:ptr<void>` -- a
bare fn pointer wearing a `:ptr<void>` type. The ascription suppresses the
bare-fn auto-shim (the `ak == TY_FN && !boxed` branch at 2884) that would
otherwise box it, and the pass-through emits it raw.

The deeper invariant violation: **captureless closures escaping as values are
not uniformly boxed.** The fat-dispatch ABI (and the `__tur_poly_to_fat*` shim
table that already exists for exactly this purpose, `tur` runtime preamble) want
every closure value reaching a `^fat`/`ptr<void>` boundary to be a `{ thunk,
env }` box.

## Proposed fix directions

1. **Box captureless closures when they escape as a value** (not when directly
   called). A captureless `(fn ...)` returned from / stored by a defn should
   emit a static (or heap) fat box `{ __fn, /*no env*/ }` instead of a bare fn
   pointer, so all closure values share one representation. Most principled;
   widest blast radius (touches closure codegen + the
   closure-representation-unification work).
2. **Shim at the `:ptr<void>` -> `^fat` boundary by provenance.** In
   `elab_call.c` ~2903, when the `:ptr<void>` argument's provenance is a
   captureless closure (a `(:: <captureless-fn-result> :ptr<void>)`, or any
   value the elaborator knows is a bare fn pointer), route it through the
   existing `EX_FN_TO_FAT` / `__tur_poly_to_fat*` shim instead of the
   pass-through. Narrower, but the elaborator must track "this `:ptr<void>` is
   really a bare fn pointer", which is lost once a vec/`::` erases it (see the
   tur-signal `__vec-get-i` read -- the provenance is gone by then).
3. **Make captureless closures fat at the defn return boundary** specifically
   for closure-typed returns (`: ptr<void>` / `(fn ...)` results), the spot
   where they most often escape into fat dispatch. A middle ground.

Direction 1 is the real fix; 2/3 are targeted mitigations.

## How to validate

- `/tmp/sf_direct.tur` (above) prints `-0.5` and exits 0.
- `(>>> (gain 2.0) (invert))` from `stdlib/arrow.tur`, applied to a signal,
  returns the negated, gained sample.
- Add a fixture under `tests/fixtures/` that composes a capturing SF with a
  captureless SF through both `__apply-sf`-style direct dispatch and `>>>`, and
  asserts the numeric result.
- `bash tests/run.sh` stays green (watch the closure/arrow fixtures for
  double-boxing regressions -- the symmetric failure mode of direction 1/2).

## Impact

Blocks `tur-signal` Phase 5 from using `>>>` (and even the interim
`__chain-loop`/`__apply-sf` fold) with the captureless Tier-1 shapers `invert`
and `abs-sf`. Capturing SFs (`gain`, `offset`, `low-pass`, `hard-clip`, ...)
work; the spice's existing `test_compose` only exercised capturing SFs, so the
hole went unnoticed. See `docs/upcoming/tur-signal-rebuild-plan.md` Phase 5.
