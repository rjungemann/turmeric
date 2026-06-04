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
