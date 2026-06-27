# Nested carrier `match` loses the concrete element type

**Severity:** low (pre-existing; has a clean `::` ascription workaround).

## Summary

When a parametric carrier ADT's field is itself a parametric type and you
`match` through both layers, the concrete element type is not threaded into the
inner field bindings -- they surface as `<struct>`/tyvar instead of the resolved
type, so an arithmetic/operator use on them fails `operator lookup`.

## Minimal repro

```turmeric
(defdata Pair2 [a b] (MkPair2 a b))
(defdata Nest [a]    (N (Pair2 a a)))

(defn main [] : int
  (let [n (N (MkPair2 3 4))]
    (match n
      (N inner)
        (match inner
          (MkPair2 x y) (println (+ x y))))))   ; x, y not seen as :int
```

```
error [TUR-E0006]: operator lookup failed for '+': got 2 arg(s),
first arg type <struct>
```

## Workaround

Ascribe the inner bindings to their known concrete type:

```turmeric
(MkPair2 x y) (println (+ (:: x :int) (:: y :int)))
```

This is the same workaround the in-suite fixture
`tests/fixtures/defdata-applied-type-field/input.tur` already uses, and the new
`tests/fixtures/conv-byval-adt-app-pair/input.tur` (Crossing B arm) adopts.

## Root cause (CONFIRMED 2026-06-27) -- and why a full fix is a dedicated pass

The original "suspected" cause was only the surface. A full attempt (made and
**reverted** -- it cascaded into a memory-ownership rework disproportionate to
this low-severity, workaround-having issue) pinned the true chain:

1. **Construction, not the match, loses the type arg** (elab_call.c ~L2137, the
   Phase-G0 ctor-call result inference / "TP5" pass).  `(N (MkPair2 3 4))` should
   produce `(Nest int)`, but the TP5 pass that grounds the ADT's type params only
   records a binding when a field's `full_type` is a **bare TY_TYVAR**.  `N`'s sole
   field is the parametric `(Pair2 a a)` (TY_APP), so it is skipped, `n_bound == 0`,
   and the result stays a bare `Nest` (no TY_APP).  The match then has no app args
   to thread, so `inner`/`x`/`y` surface as residual tyvars.  (`(MkPair2 3 4)`
   alone works because its fields ARE bare tyvars.)
   - Fix that worked: ground EVERY type param by unifying each field's declared
     full_type against the argument's actual type, descending into TY_APP/TY_FN
     (reuse `adt_field_collect_type_args` from the fn-field-inference fix, made
     non-static).  Also broaden the match field-bind (elab_structs.c ~L4015) to
     substitute app args for a TY_APP/TY_FN field type, not just a bare TY_TYVAR.

2. **Once (1) lands, three codegen seams surface in turn** (each fixed in the
   reverted attempt, all correct on their own):
   - **Typedef ordering** (types.c `emit_registered_adt_app_rec`): the ADT-app
     emitter had no dependency pre-pass, so `tur_adt_Nest__int` (whose field is
     `tur_adt_Pair2__int__int`) was emitted before that nested monomorph's
     typedef -> `unknown type name`.  Mirror the struct-app emitter's pre-pass,
     recursing into both `g_adt_apps` and `g_struct_apps`.
   - **Match extraction** (emit_expr.c ~L7411): a NON-wide by-value ADT field is
     stored INLINE in the enclosing (even carrier) struct (the typedef emitter
     writes the aggregate type, not int64, when `!type_is_wide_byval_adt`), but
     the match B3 branch deref-unboxed it (`*(T*)(intptr_t)(agg)` -> cc "aggregate
     value used where an integer was expected").  Gate the deref on
     `type_is_wide_byval_adt`; read a non-wide field inline.

3. **THE BLOCKER -- pre-existing latent leaks in `substitute_adt_app_type`
   callers** (types.c).  `(Nest int)` flowing as a concrete app newly drives
   `adt_app_is_byvalue_product` / `adt_field_c_type` / `adt_app_byval_pass_by_ptr`
   (all reached from the hot `type_uses_carrier_abi`), each of which calls
   `substitute_adt_app_type` and never frees the malloc'd spine -> the
   compile/emit path leaks and the suite's leak gate fails (`defdata-applied-type-
   field`, `conv-byval-adt-app-pair`).  But `free_struct_app_type` on the result
   is **unsafe**: `substitute_adt_app_type` returns a PARTIALLY-ALIASED structure
   (a tyvar that substitutes to a nested app `return args[idx]` shares that arg's
   spine), so freeing it double-frees the shared nodes and corrupts codegen (18
   `conv-defstruct-*-lowering` fixtures build-fail).  A safe fix means giving
   `substitute_adt_app_type` clear ownership (deep-clone the substituted arg, e.g.
   `return clone_struct_app_type(args[idx])`) and then auditing/​freeing at every
   caller -- a function-contract change with broad blast radius.

## Fix direction (dedicated pass)

Land (1) + (2) together, then do the `substitute_adt_app_type` ownership rework
(3) as its own carefully-verified step (deep-clone results; free at every
caller; re-run the full suite for new leaks AND double-frees).  Until then the
`::` ascription workaround stays the supported path.  The original suspected
cause below was incomplete -- it is a construction-site inference gap, not just a
match concern.
