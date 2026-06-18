# Poly HOF with primitive tyvar pinned by a value arg *before* the fn arg fails to type-check

**Status:** RESOLVED 2026-06-18 (fixed in the same session, as the final
M5 residual). See the "Resolution" section at the bottom; validated by
`tests/fixtures/poly-hof-reversed-order-primitive-pin/`.

**Status (original):** OPEN 2026-06-18.
**Severity:** Hard compile error (TUR-E0001 at elaboration). Loud, not a
silent miscompile -- the program never lowers. An ergonomics/expressiveness
gap, not a codegen defect.
**Discovered:** while fixing
[`docs/archive/poly-hof-constrained-arg-baked-carrier.md`](poly-hof-constrained-arg-baked-carrier.md)
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

## Resolution

Landed 2026-06-18 as the final M5 residual (the last open M5-class gap
after `poly-hof-constrained-arg-baked-carrier` resolved the original-order
variant). **Fix direction 1 alone was sufficient** -- no eta-expansion
change was needed.

The single change is in `call_collect_type_bindings`
(`src/compiler/elab_call.c`, `TY_TYVAR` case). The function already had a
special case for a prior *tyvar* binding accepting a *concrete* actual
(the `m5-eq-vec-rewrite-fn-arg-loses-annotation` relay path). It lacked
the **mirror**: a prior *concrete* binding accepting a bare *actual
tyvar*. For the reversed-order call `(apply-it 42 count-it)`, by the time
the trailing `count-it` argument (type `(fn [tyvar] int)`) is checked, the
leading value arg `42` has already pinned `A -> int`, so collecting
`(fn [int] int)` (the param) against `(fn [tyvar] int)` (the arg) reaches
the `TY_TYVAR` case with a *concrete* prior binding (`int`) and a bare
*actual* tyvar -- and fell through to `type_eq(int, tyvar)` -> false,
rejecting the argument. The mirror now accepts it:

```c
if (actual.kind == TY_TYVAR && bindings[idx].type.kind != TY_TYVAR) {
    return true;
}
```

This is sound: a bare actual tyvar means the function value
(`count-it`, a constrained-generic) is itself still polymorphic, so it is
specialized/dispatched to the concrete type at the call site. The
narrow gate (`actual.kind == TY_TYVAR`) leaves concrete-vs-concrete
mismatches still rejected, so it does not loosen real type errors. The
existing relay special case (prior tyvar + concrete actual, kept
abstract) is untouched, so the `m5-lambda-aft-tyvar-prior-accepts-concrete`
behavior the report flags is preserved.

No eta-expansion was required because the primitive case dispatches
correctly through the **carrier base** -- exactly as the working
original-order primitive call `(apply-it count-it 42)` already does.
Emitted C for the repro:

- Struct call -> `apply_it__spec__int64_t_Box_int64_t((Box){.n=0}, <eta
  wrapper around count_it__spec__int64_t_Box>)` -> `__inst_Size_size_Box`
  -> `7`.
- Primitive call -> carrier base `apply_hyit(42, count_hyit)` ->
  `count_hyit` -> `__inst_Size_size_int` -> `-1`.

Validated by `tests/fixtures/poly-hof-reversed-order-primitive-pin/`
(output `7` / `-1`; a wrong instance would print `-1` / `-1`). Full suite
green (`bash tests/run.sh`: 1680 passed, 0 failed), zero codegen-snapshot
drift.
