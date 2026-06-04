---
title: defopaque cannot carry phantom type parameters
category: Reported
description: defopaque only accepts `(defopaque Name :basetype [:linear|:affine])`. There is no way to declare a parameterized opaque newtype like `NonEmpty A` or `BoundedIdx n`, so refinement newtypes that want to track an element type or a length in their spelling must fall back to a monomorphic carrier-level opaque over :int.
---

# `defopaque` cannot carry phantom type parameters

> **Severity:** expressiveness gap (ergonomics; no miscompile).
> **Found:** 2026-06-04, executing
> [stdlib-refinement-collections-plan](../upcoming/stdlib-refinement-collections-plan.md)
> phases R1/R2.
> **Status:** FIXED 2026-06-04 (fix direction #1). `defopaque` now accepts an
> optional `[A ...]` phantom type-parameter vector; `stdlib/refined.tur`'s
> `NonEmpty` is parameterized as `(NonEmpty A)`. Two latent codegen bugs that
> the feature surfaced were fixed alongside it (see "Fix" below).

## Summary

The plan calls for parameterized refinement newtypes:

```turmeric
(defopaque NonEmpty A int)        ;; element type A
(defopaque BoundedIdx n int)      ;; length index n
```

`defopaque` accepts only `(defopaque Name :basetype)` with an optional trailing
`:linear` / `:affine` keyword. It takes no type-parameter vector, so the `A` /
`n` spellings above do not parse. (`defstruct` and `defgadt` *do* accept a
`[A ...]` parameter vector; `defopaque` is the odd one out.)

## Root cause

`elab_defopaque` (`src/compiler/elab_structs.c`, around the
`(defopaque Name :int)` parser) reads `items[1]` as the name and `items[2]` as
the base type, then only inspects `items[3]` for a `:linear` / `:affine`
keyword. There is no branch for a leading `[A]` type-parameter vector and the
resulting `StructDef` carries no type params.

## Impact / workaround

`stdlib/refined.tur` models `NonEmpty` and `BoundedIdx` as monomorphic
carrier-level opaques over `:int`:

```turmeric
(defopaque NonEmpty :int)     ;; a non-empty cons-list pointer
(defopaque BoundedIdx :int)   ;; an index proven in [0, n)
```

This is the same carrier-level idiom the rest of stdlib uses (e.g.
`list-head : int`), and it composes cleanly with the int-carrier `Option` and
the list/vec/slice helpers. What is lost is *type-level* tracking: the element
type `A` and the length `n` live at the value level (enforced by the smart
constructors `ne-from?` / `bidx-of?`) instead of in the type spelling, so the
checker cannot reject e.g. a `BoundedIdx` validated against length 5 used to
index a length-3 vector. The runtime guard in the smart constructor still makes
the access total.

## Proposed fix directions

1. **Accept an optional `[A ...]` vector in `defopaque`**, storing the params on
   the `StructDef` as phantoms (carrier stays the declared base type). Then
   `NonEmpty<A>` / `BoundedIdx<n>` could be spelled and the accessors typed
   `A` instead of `:int`, closing the gap with the plan's design block.
2. Failing that, document the carrier-level convention as the intended form for
   refinement newtypes.

## Validation

`(defopaque NonEmpty [A] :int)` should parse, and
`(defn ne-head [A] [ne : (NonEmpty A)] :A ...)` should typecheck, with
`(ne-head (ne-of 1.5 (tnil)))` returning a `float`.

## Fix (implemented)

Fix direction #1 was taken.

1. **Parsing (`src/compiler/elab_structs.c`, `elab_defopaque`)** -- an optional
   `[A ...]` vector between the name and the base type is now parsed into
   `def->type_params` / `def->n_type_params`; the binding's `hkt_kind` is set
   to `kind_for_arity(n)` so `(Name A)` annotations kind-check. The carrier is
   still int64_t. The top-level forward pre-pass
   (`src/compiler/elab_toplevel.c`) records the arity on the stub so
   annotations resolved before the full elaboration get the right arrow kind.

2. **Two latent codegen bugs surfaced and fixed** (these were real defects, not
   defopaque-specific, but a polymorphic `: A` return whose body is an
   ascription to the carrier is the first thing to hit them):
   - **Clobbered call-site instantiation** (`src/compiler/elab_call.c`): the
     G3 ADT and LT4 struct return-type patches overwrote a call's already
     *instantiated* result type with the function's *uninstantiated* declared
     return type. They now only fire when `result_full_type` is actually a
     `TY_ADT` / `TY_STRUCT` (a real def to recover), not when it is a bare
     named tyvar.
   - **Spurious carrier-bridge dereference** (`src/compiler/emit_expr.c`,
     `EX_ASCRIBE`): `(:: <int> :A)` where `A` is a def-less `TY_STRUCT` /
     `TY_TYVAR` carrier was treated as a by-value aggregate and emitted a
     `*(int64_t *)(value)` deref (segfault). The opaque-relabel guard now also
     covers the tyvar/def-less-struct carrier.

3. **`stdlib/refined.tur`** -- `NonEmpty` is now `(defopaque NonEmpty [A] :int)`;
   `ne-of` / `ne-singleton` / `ne-head` / `ne-tail` / `ne->list` / `ne-len`
   carry the element type `A`, so `ne-head` returns `A` instead of a bare
   `:int`. The smart-constructor recovery path (`ne-from?` / `ne-unwrap`)
   operates on a bare int-carrier list, so it stays concrete at
   `(NonEmpty int)`. `BoundedIdx` stays a bare carrier (its bound `n` is a
   value-level fact, not a trackable type).

Covered by the `defopaque-phantom-param` fixture and the existing
`refined-nonempty` / `refined-bounded-idx` fixtures.

### Known limitation

A non-`int`/non-pointer element (e.g. a `float`) carried through a polymorphic
carrier function still round-trips incorrectly at runtime, because the shared
monomorphized body stores/loads the element through the int64 carrier with a C
numeric conversion rather than a bit reinterpret. This is the pre-existing
poly-carrier float-register-class limitation (cf. `TUR-E0705`), independent of
the phantom-parameter spelling: the *type* `(ne-head (ne-of 1.5 (tnil)))` is
`float` as required, but the value truncates. Integer/pointer element types
round-trip correctly.
