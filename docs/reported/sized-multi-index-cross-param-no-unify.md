---
title: Cross-parameter size unification does not fire for multi-index (>=2 phantom param) opaques, so sized matrices can't enforce shape
category: Sized types (SZ8 cross-parameter unification) -- soundness/coverage gap
severity: Medium. Not a miscompile -- it is a missing static check. A single-index
  (SizedBuf n) / (Vec n) correctly rejects a length mismatch with TUR-E0260, but
  the same discipline on a two-index (Mat m n) opaque silently accepts a shape
  mismatch. This blocks the dimension-correct matrix surface that Track C / U4
  wants (mat-mul, transpose, etc.).
status: OPEN
---

# Multi-index phantom-opaque size variables don't cross-unify (sized matrices)

## One-line summary

The SZ8 cross-parameter size unifier that powers TUR-E0260 fires for a phantom
opaque with **one** size index (`(defopaque SizedBuf [n] :int)`), but **not**
for one with **two or more** size indices (`(defopaque Mat [m n] :int)`). A
shared size variable that appears across two multi-index parameters is never
contradicted, so a shape mismatch compiles clean.

## Works today -- single index (control)

```turmeric
(load "stdlib/sized.tur")
(defopaque LaVecN [n] :int)
(defn vmk [n] [k : int] : (LaVecN n) (:: (unsafe (__z k)) :LaVecN))
(defn __z [k : int] #{Unsafe} : int ```c (void)k; return 0; ```)
(defn vdot [n] [a : (LaVecN n) b : (LaVecN n)] : float ```c (void)a;(void)b; return 0.0; ```)
(defn mk2 [] : (LaVecN (Static 2)) (vmk 2))
(defn mk3 [] : (LaVecN (Static 3)) (vmk 3))
(defn vbad [] : float (vdot (mk2) (mk3)))
;; error [TUR-E0260]: function 'vdot' shares size variable 'n' across
;;   parameters, but argument 1 has size 2 while argument 2 has size 3
```

(Note: avoid the name `dot` -- it collides with the stdlib `dot` macro.)

## Fails -- two indices (the bug)

### (a) shared inner dimension in matrix multiply

```turmeric
(load "stdlib/sized.tur")
(defopaque LaMatN [m n] :int)
(defn mnew [m n] [r : int c : int] : (LaMatN m n) (:: (unsafe (__n r c)) :LaMatN))
(defn __n [r : int c : int] #{Unsafe} : int ```c (void)r;(void)c; return 0; ```)
(defn mul [m k n] [a : (LaMatN m k) b : (LaMatN k n)] : (LaMatN m n)
  (:: (unsafe (__mm (:: a :int) (:: b :int))) :LaMatN))
(defn __mm [a : int b : int] #{Unsafe} : int ```c (void)a;(void)b; return 0; ```)
(defn mk23 [] : (LaMatN (Static 2) (Static 3)) (mnew 2 3))
(defn mk54 [] : (LaMatN (Static 5) (Static 4)) (mnew 5 4))
;; (2x3) * (5x4): inner 3 != 5 -> SHOULD be TUR-E0260 (k := 3 vs k := 5)
(defn bad [] : (LaMatN (Static 2) (Static 4)) (mul (mk23) (mk54)))
;; accepted -- no error
```

### (b) shared full shape in matrix add

```turmeric
(defn madd [m n] [a : (LaMatN m n) b : (LaMatN m n)] : (LaMatN m n)
  (:: (unsafe (__mm (:: a :int) (:: b :int))) :LaMatN))
(defn mk23 [] : (LaMatN (Static 2) (Static 3)) (mnew 2 3))
(defn mk24 [] : (LaMatN (Static 2) (Static 4)) (mnew 2 4))
;; n: 3 != 4 -> SHOULD be TUR-E0260
(defn mbad [] : (LaMatN (Static 2) (Static 3)) (madd (mk23) (mk24)))
;; accepted -- no error
```

Both (a) and (b) compile with no diagnostic. The single-index control above,
written and checked the same way, correctly rejects. Confirmed empirically
against `tur` built from this branch (Debug):
`./build/tur -Xsized-types check` -> exit 0 for (a) and (b), exit 1 (TUR-E0260)
for the control.

## Root cause (confirmed, file:line)

`sz_cross_param_unify` in `src/compiler/elab_call.c:964` only ever inspects the
**first** size index of each parameter, on both sides of the comparison:

- Template side (`elab_call.c:982-987`): the inner loop `break`s at the first
  annotation arg that parses as a size term. For `(LaMatN m k)` it captures only
  `m` (index position 0) and never `k` (position 1).
- Argument side (`elab_call.c:1002-1003`): the recovered size index comes from
  `sz_first_size_term` (`elab_call.c:941`), which likewise returns only the
  *first* size term of the arg's type Form.

So only index position 0 is ever folded into the per-call `subst[]` table:

- case (a) `mul [m k n] a:(LaMatN m k) b:(LaMatN k n)`: position 0 collects
  `m := 2` (arg1) and `k := 5` (arg2) -- distinct names, no contradiction. The
  shared `k` lives at position 1 of `a` (= 3) and position 0 of `b` (= 5); the
  position-1 slot of `a` is never visited, so `k := 3` vs `k := 5` is never seen.
- case (b) `add [m n] a:(LaMatN m n) b:(LaMatN m n)`: position 0 gives
  `m := 2` for both (consistent); the real clash is at position 1
  (`n := 3` vs `n := 4`), never visited.
- single-index control rejects correctly -- it has only position 0.

This matches the report's own diagnosis: the indices exist and are tracked
individually (construction/ascription infers `(LaMatN (Static r) (Static c))`
fine), but the cross-param walk stops at the head/first index and never
descends per-index-position.

## Fix direction

Extend the SZ8 cross-parameter walk so that, for an opaque with index list
`[i0 i1 ...]`, each argument contributes a size binding **per index position**
(arg.i0 -> size, arg.i1 -> size, ...) and a size variable shared across
parameters is unified position-wise. Concretely, replace the "first size term"
shortcut on both template and arg side with a positional zip over the
annotation Form's argument list (positions 1..len-1) paired against the arg's
recovered type Form's argument list at the same positions. The single-index
case is the degenerate form; the contradiction machinery (`subst[]` +
`size_term_eval`) already exists and just needs to run for every index slot.

Open question to resolve while fixing: the arg side currently leans on a single
`a->as.call_.size_index` (one SizeTerm) from the GADT path, plus the
`sz_recover_type_form` fallback. For multi-index opaques the GADT single-term
field is insufficient; the positional recovery should drive off the recovered
type Form (which does carry both indices) rather than the single `size_index`.

A regression fixture pair mirroring the existing single-index ones:
- `errors/sized-matrix-cross-param-reject` -- the (2x3)*(5x4) inner-dimension
  clash -> TUR-E0260 naming `k`.
- `sized-matrix-cross-param-accept` -- (2x3)*(3x4) -> clean, result (2x4).

## Scope / impact

Spice-side, this is the one thing blocking a dimension-correct sized-matrix
surface in `turmeric-spices/spices/linalg` (Track C / U4). Sized vectors
(`linalg/sized`, single index `(LaVecN n)`) shipped and enforce correctly; the
matrix module is deferred pending this fix. Verified on `tur` built from
turmeric `main` (post-SZ9). Closely related to the SZ6-SZ8 cross-parameter
unification work.
