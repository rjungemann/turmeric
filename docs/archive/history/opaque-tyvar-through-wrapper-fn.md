# Concrete opaque rejected as a generic value parameter when the same tyvar is bound through a wrapper opaque

**Component:** elaborator / type unification (TUR-E0001)
**Severity:** blocked end-to-end use of user `defopaque` types as generic carrier payloads
**Status:** RESOLVED

## Summary

A generic function whose type parameter `T` appears both inside a wrapper
opaque argument (`(Box T)`) and as a bare value parameter (`item : T`) failed
to type-check when called with a concrete user-defined `defopaque` as `T`.
The unifier reported `expected tyvar, got <Opaque>`. Builtin/primitive carriers
(`int`, `cstr`, scalars) worked in the identical position, so the defect was
specific to nominal `defopaque`/struct types instantiating a bare-tyvar value
parameter.

## Minimal reproduction

```turmeric
(defmodule wt
  (export main)
  (defopaque Box [T] :ptr<void> :linear)
  (defopaque Tag :int)
  (defn tag-of [n : int] : Tag ```c return n; ```)
  (defn b-new  [T] [^fat cb : (fn [T] void)] : (Box T) ```c return (void*)(intptr_t)cb; ```)
  (defn b-sub  [T] [^borrow b : (Box T) item : T] : void ```c (void)b; (void)item; ```)
  (defn b-free [T] [b : (Box T)] : void ```c (void)b; ```)
  (defn main [] : int
    (let [b (b-new (fn [t : Tag] : void nil))]
      (do (b-sub b (tag-of 9)) (b-free b) 0))))
```

```
error [TUR-E0001]: function 'b-sub' arg 2: expected tyvar, got Tag
```

## Root cause

`b`'s type was built as `(Box <struct-with-NULL-def>)` rather than `(Box Tag)`.
The corruption originated in the `b-new` call: `T` was bound through the closure
argument `(fn [t : Tag] : void nil)`, but that closure's `TY_FN` type carried no
`arg_full_types`, so `call_collect_type_bindings` (the `TY_FN` branch in
`src/compiler/elab_call.c`) fell back to `type_from_kind(actual.as.fn.arg_kinds[0])`.
For an opaque/struct kind that yields a def-less `TY_STRUCT` placeholder, losing
the nominal `Tag` identity. `T` was bound to that placeholder; the later
`item : T` value parameter then compared the placeholder (struct def `NULL`)
against the real `Tag` struct and failed `type_eq`.

Primitives were unaffected because `type_from_kind(TY_INT)` fully reconstructs
the type.

The closure lacked `arg_full_types` because the lambda elaborator
(`src/compiler/elab_fns.c`) only recorded `param_full_types` for `TY_APP`,
`TY_TYVAR`, `TY_FN`, or named-tyvar-bearing annotations -- nominal `TY_STRUCT` /
`TY_ADT` (and opaque newtypes, which lower to `TY_STRUCT`) were omitted.

## Fix

`src/compiler/elab_fns.c`: extend the `param_full_types` recording condition to
include `TY_STRUCT` and `TY_ADT` annotations, so a closure parameter typed with
a nominal/opaque type preserves its full type on `arg_full_types`. The unifier
then binds the generic tyvar to the real opaque type rather than a def-less
placeholder.

## Regression test

`tests/fixtures/opaque-tyvar-through-wrapper-fn/`
