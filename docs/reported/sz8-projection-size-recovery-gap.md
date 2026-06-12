---
title: SZ8 cross-parameter unification does not fire on EX_GET_FIELD args (follow-up to P1 fix)
category: Expressiveness hole / elaborator gap
severity: Medium. Type substitution through `defstruct` projection works (P2 of ecs-e2c is structurally answered), but SZ8's static-mismatch rejection does not reach struct field projections. Blocks the final payoff of P2 for ECS E2c -- the "bounded-capacity world" only fully rejects mismatched literals when projections carry inferred size indices.
description: After the SZ8 non-GADT fix (sz8-opaque-phantom-size-not-load-bearing.md), cross-parameter size unification covers two arg kinds: GADT constructor calls (existing) and plain function calls whose callee declares a return-type Size form (newly added via TY_FN.result_type_form). It does not cover struct field projections: `(zip (.pos w) (.vel w))` where `w : (World (Static 3) A B)` type-checks even when zip's shared `n` should reject mismatched literal indices. The root cause is the placeholder representation used for size literals in type-app slots -- a Size literal lowers to a plain TY_INT placeholder, so the original `(Static k)` Form is not recoverable from the projected field's Type. To close the gap, the projection result needs to carry the receiver's size form (either by attaching the Form to the TY_APP node and threading it through struct_field_instantiate_type, or by walking the receiver Expr's binding back to its declared type-annotation Form at the unifier site).
status: OPEN. Filed 2026-06-12 as a P2 follow-up. Recommended next step: extend the SizeTerm-recovery path in `sz_cross_param_unify` (src/compiler/elab_call.c) to handle EX_GET_FIELD args by walking through the struct value's TY_APP chain (or binding-annotation Form) to recover the projected field's size index.
---

# SZ8 cross-parameter unification does not fire on struct field projections

## Summary

The non-GADT SZ8 extension that landed alongside the P1 fix
([sz8-opaque-phantom-size-not-load-bearing.md](sz8-opaque-phantom-size-not-load-bearing.md))
infers a call expression's `size_index` from the callee's declared
return-type form. It does not yet do the analogous inference for
struct field projections **or for let-bound variables** -- both lose
the size index because the recovery only fires for `EX_CALL`-kind
arguments. Surfaced first by the P2 World probe (EX_GET_FIELD), then
by the P3 sized-buf-copy! probe (EX_VAR through a `let`).

## Probes

### A. Struct field projection

```turmeric
(defgadt Size []
  (Static (int) : (Size))
  (Add (Size) (Size) : (Size))
  (Mul (Size) (Size) : (Size)))

(defopaque Dense [n A] :int)
(defstruct Pair2 [A B] (fst A) (snd B))

(defn zip [A B] [xs : (Dense n A) ys : (Dense n B)] : int 0)

; MISMATCH: fst is (Dense (Static 3) A); snd is (Dense (Static 5) B).
; SHOULD fail because zip's shared `n` cannot unify 3 with 5.
(defn use-pair [A B]
    [p : (Pair2 (Dense (Static 3) A) (Dense (Static 5) B))] : int
  (zip (.fst p) (.snd p)))

(defn main [] : int 0)
```

`./build/tur -Xsized-types check` accepts the program. The expected
behavior is a TUR-E0260 at the call to `zip` -- the same diagnostic
that fires today for `(zip (mk-dense-2) (mk-dense-3))`.

### B. Let-bound variable

```turmeric
(load "stdlib/sized.tur")
(load "stdlib/sized-buf.tur")

(defn mk-2 [] : (SizedBuf (Static 2))
  (:: (sized-buf-new-zeroed 2) :SizedBuf))
(defn mk-3 [] : (SizedBuf (Static 3))
  (:: (sized-buf-new-zeroed 3) :SizedBuf))

(defn main [] : int
  (let [a (mk-2)
        b (mk-3)]
    ; Direct-call form `(sized-buf-copy! (mk-2) (mk-3))` correctly
    ; raises TUR-E0260.  The let-bound version below silently compiles
    ; and aborts at runtime with "sized-buf-copy!: length mismatch".
    (sized-buf-copy! a b)
    0))
```

Same root cause: `a` and `b` are `EX_VAR` args, not `EX_CALL`, so
neither the GADT-witness path nor the P1 result-type-form path fires
on them.

## Root cause

`sz_cross_param_unify` in `src/compiler/elab_call.c` reads each arg's
inferred `size_index` from one of two sources:

1. `args[i]->as.call_.size_index`, populated by
   `sz8_infer_ctor_size_index` for sized-GADT constructor calls.
2. The arg's callee `result_type_form` (the non-GADT path added with
   the P1 fix).

Neither path fires for `args[i]` of kind `EX_GET_FIELD`. The projected
field's `Type` was computed by `elab_struct_field_use_type`, which
substitutes the struct's type args (raw `Type`s extracted from the
receiver's `TY_APP` chain) into the field's declared body. The
substitution preserves type *shape*, but a Size literal
(`(Static 3)`) lowers via the SZ8 parser fix to a bare `TY_INT`
placeholder; the original `Form` is not attached to the `TY_APP` arg,
so the projected field's `Type` has no recoverable Size form.

## Proposed fix directions

Two viable paths:

1. **Attach the Size Form to `TY_APP`.** Add a `const Form *arg_form`
   slot on `TY_APP`'s union (default NULL). Populate it from the SZ8
   parser fix where a Size literal lowers to a placeholder. Extend
   `struct_field_instantiate_type` to propagate the slot when the
   field's declared type variable maps to a struct type arg whose
   `TY_APP` wrapper carries an `arg_form`. Read it back at the
   unifier site for EX_GET_FIELD args.

2. **Walk back from the receiver binding.** Store the parameter's
   declared type annotation Form on `Binding`. At the unifier site,
   walk EX_GET_FIELD -> receiver (EX_VAR) -> binding -> annotation
   Form, then match the struct def's field annotation form to identify
   which struct type param the field's size slot references, and look
   up that param in the binding's annotation form.

Option 1 is local to the type system; option 2 keeps the type system
simpler but adds Form-tracking to Binding.

## Validation of a fix

- The probe above raises TUR-E0260 at the `zip` call.
- The matched companion
  (`tests/fixtures/sized-struct-field-share-accept` -- the P2 accept
  fixture) continues to type-check and run.
- A new fixture pair
  `tests/fixtures/errors/sized-struct-field-share-reject` exercises
  the mismatch case for regression coverage.

## Related

- [sz8-opaque-phantom-size-not-load-bearing.md](sz8-opaque-phantom-size-not-load-bearing.md)
  (the P1 fix; landed 2026-06-12)
- [ecs-e2c-sized-dense-needs-bounded-world.md](ecs-e2c-sized-dense-needs-bounded-world.md)
  (P2 -- parent question; structurally answered, this report tracks
  the remaining static-rejection gap)
- `tests/fixtures/sized-struct-field-share-accept/input.tur` (the
  matched accept fixture; demonstrates threading works)
