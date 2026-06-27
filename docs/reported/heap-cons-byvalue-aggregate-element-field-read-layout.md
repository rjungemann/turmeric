# :heap ADT field read at by-value aggregate element needs consistent concrete-app recognition

**Severity:** medium (seam-4 / defstruct-as-defadt graduation blocker; not a
default-path bug). 2 fixtures.

## One-line summary

A direct field read on a `:heap` record-ADT receiver whose element is a by-value
aggregate -- `(.head xs)` / `(.tail xs)` where `xs : (Cons (Option int))` and
`Cons` is `(defstruct Cons :heap [A] (head A) (tail :int))` -- emits a cast to
the GENERIC carrier `tur_adt_Cons` (`->as.Cons._0` is `int64_t`) and then C-casts
that int64 to the aggregate `tur_adt_Option__int` -> `error: conversion to
non-scalar type requested`.  It should cast the heap pointer to the MONOMORPH
`tur_adt_Cons__Option__int *` and read the inline aggregate field.

## Minimal repro

`tests/fixtures/list-length-byvalue-aggregate-element`,
`tests/fixtures/list-homog-byvalue-aggregate-element` under the force-lower probe.

```turmeric
(defn opt-or [o : (Option int) d : int] : int (if (.is-some o) (.value o) d))
(defn main [] : int
  (let [xs (:: (list (some 42) (some 7)) (Cons (Option int)))]
    (let [h0 (.head xs)]                      ;; <- conversion to non-scalar type
      (println (opt-or h0 0))))
  0)
```

Both pass at default (where `Cons`/`Option` are int64-carrier heap handles).

## Root cause (located) and why it is not a leaf fix

The construction side IS already correct under lowering: `tcons-of` monomorphizes
to `tur_adt_Cons__Option__int *` and stores the `Option__int` aggregate INLINE
(`union { struct { tur_adt_Option__int _0; int64_t _1; } Cons; }`), and the
element-aware `tlength` spec reads the chain correctly.  Only the DIRECT
`(.head xs)` / `(.tail xs)` reads in `main` mislower.

The EX_GET_FIELD ADT branch (emit_expr.c) has no `:heap`-ADT-receiver case: the
`adt_recv_byvalue` branch is gated on `emit_type_is_byvalue_adt`, which is false
for a `:heap` ADT (a pointer, not by-value), so the read falls to the generic
carrier-pointer-cast fallback.

Adding a `:heap`-ADT-receiver branch that casts to the monomorph requires
`type_c_name((Cons (Option int)))` to yield `tur_adt_Cons__Option__int *`.  That
in turn requires `adt_app_is_byvalue_product((Cons (Option int)))` to be true,
which fails because the element `(Option int)` is rejected by
`type_has_concrete_codegen_layout` -- whose `TY_APP` arm only handles
struct-headed apps, never ADT-headed apps (even though the bare `TY_ADT` case
returns true).  Two fix attempts, both unsatisfactory:

1. **Broaden `type_has_concrete_codegen_layout` to accept concrete ADT-apps.**
   Makes both fixtures pass (producer + the new monomorph read agree), but has a
   FAR-reaching blast radius: recursive-functor HKT monomorphs
   (`(ExprF Expr)` -> `ExprF__Expr`) are then named by `type_c_name` but never
   registered/emitted, so 9 default fixtures regress with `unknown type name
   'ExprF__Expr'` (hkt-cata-*, hkt-fmap-byvalue-sum-element,
   fn-typed-match-arm-capture, conv-defstruct-return-bridge-inline-c).

2. **Narrow: only teach `adt_app_is_byvalue_product` to accept a nested by-value
   ADT-app element/arg** (recurse on `adt_app_is_byvalue_product`), leaving the
   global predicate untouched.  The two fixtures then BUILD (the monomorph read
   fires) but SEGFAULT at runtime: some other emit path still gates on
   `type_has_concrete_codegen_layout((Option int)) == false` and emits carrier
   code, so producer and consumer disagree on the cell ABI.

The clean fix needs a single notion of "this ADT-app has a concrete, actually-
emitted monomorph layout" that (a) is true for non-recursive concrete apps like
`(Option int)` / `(Cons (Option int))`, (b) is false for recursive-functor
monomorphs that are never emitted, and (c) is used uniformly by `type_c_name`,
`type_has_concrete_codegen_layout`, the construction path, and the field-read
path.  That is a layout-registration consolidation, not a leaf patch -- hence
reported rather than forced.

## Fix directions

- Give `type_has_concrete_codegen_layout` an ADT-app arm gated on
  non-recursion (an `AdtDef` recursion flag, or "the monomorph is registered in
  the adt-app registry"), so the recursive HKT monomorphs stay carrier while
  `(Option int)` / `(Cons (Option int))` become concrete -- then add the
  `:heap`-ADT-receiver branch to EX_GET_FIELD that casts to the monomorph and
  reads the inline aggregate field (mirroring the existing :heap-STRUCT receiver
  handling at the struct path).

## Notes

- Default suite is unaffected (only the lowered representation triggers it).
- Distinct from the (landed) vec-element carrier<->by-value read bridge: that was
  an inline-C generic-result deref; this is the cons-cell field read using the
  wrong (generic vs monomorph) struct layout.
