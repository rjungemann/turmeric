# Poly HOF with primitive tyvar pinned by a value arg *before* the fn arg fails to type-check

**Status:** OPEN 2026-06-18.
**Severity:** Hard compile error (TUR-E0001 at elaboration). Loud, not a
silent miscompile -- the program never lowers. An ergonomics/expressiveness
gap, not a codegen defect.
**Discovered:** while fixing
[`docs/archive/poly-hof-constrained-arg-baked-carrier.md`](../archive/poly-hof-constrained-arg-baked-carrier.md)
(the original-argument-order variant, now resolved). This is the residual
reversed-order + primitive corner of the same feature.

## Repro

```turmeric
(defstruct Box [n : int])
(defclass Size [a] (size [x] : int))
(definstance Size [int] (size [x] -1))
(definstance Size [Box] (size [x : Box] 7))

(defn count-it [^Size A] [x : A] : int (size x))

;; NOTE the parameter order: the value `a : A` comes BEFORE the fn `f`.
(defn apply-it [A] [a : A f : (fn [A] int)] : int (f a))

(defn main [] : int
  (println (apply-it (make-struct Box 0) count-it))   ; OK now -> 7
  (println (apply-it 42 count-it))                    ; FAILS to type-check
  0)
```

## Observed

```
error [TUR-E0001]: function 'apply-it' arg 2: expected (fn [int] : int),
got (fn [tyvar] : int)
   (println (apply-it 42 count-it))
```

The struct call `(apply-it (make-struct Box 0) count-it)` now compiles and
returns 7 (eta-expansion fires for the non-primitive pin). The primitive
call `(apply-it 42 count-it)` does not: `A` is bound to `int` by the leading
value argument, and the trailing bare constrained-generic `count-it`
(type `(fn [tyvar] int)`) fails to unify against the now-concrete
`(fn [int] int)`.

## Expected

`(apply-it 42 count-it)` should compile and print `-1` (the `Size[int]`
instance), matching the original-order form `(apply-it count-it 42)` which
already works.

## Root-cause direction

Two interacting pieces, both in `src/compiler/elab_call.c`:

1. **Unification direction in `call_collect_type_bindings`.** With the value
   arg first, `A -> int` is recorded before the fn arg is checked. Collecting
   bindings for the fn parameter then compares `expected (fn [A=int] int)`
   against `actual (fn [tyvar_countit] int)`; in the `TY_TYVAR` case the
   prior binding is the concrete `int` (not a tyvar), so it falls through to
   `type_eq(int, tyvar_countit)` -> false and the argument is rejected. A
   bare actual tyvar should be accepted against an already-pinned concrete
   binding (the fn value will be specialized to that concrete type), the
   mirror of the spurious-self-tyvar refinement.

2. **The eta-expansion look-ahead is gated to non-primitive pins.**
   `try_eta_expand_generic_fn_arg` requires the pinned parameter type to be
   a `TY_STRUCT`/`TY_ADT`/`TY_APP` (`pins_nonprim`). For a primitive pin
   (`A -> int`) it declines, so `count-it` is never eta-expanded and stays a
   bare constrained-generic value. For the *original* order the primitive
   case happens to work via the carrier base (`A` stays abstract and the
   int representative instance is correct); reversed order pins `A` early
   and loses that escape hatch.

The poly-HOF eta look-ahead added in the resolved report (the `arg_done[]`
sibling pre-elaboration block) only feeds `try_eta_expand_generic_fn_arg`,
which still bails on a primitive pin -- so it does not cover this corner.

## Proposed fix directions

- Relax the `TY_TYVAR` unification in `call_collect_type_bindings` to accept
  a bare actual tyvar against a concrete prior binding (refine/accept rather
  than `type_eq`-reject), so the fn argument unifies.
- And/or extend the eta-expansion to fire for a primitive pin when the value
  argument is a bare constrained-generic global fn, so `count-it` resolves to
  its concrete instance (`count_it__spec__int_int`) instead of the bare
  representative.

Care is required not to disturb the `m5-lambda-aft-tyvar-prior-accepts-concrete`
relay behavior (the enclosing-generic abstract-tyvar case must still stay
abstract for the relay path).

## Validation

Add `tests/fixtures/poly-hof-reversed-order-primitive-pin/` mirroring the
repro; expected output:

```
7
-1
```

(The struct line already passes today; the suite fixture guards the
primitive line once fixed.)
