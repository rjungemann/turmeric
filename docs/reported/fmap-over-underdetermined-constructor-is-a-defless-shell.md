# `fmap` over a constructor with an open type parameter yields `(? ?)`

**Severity: low (expressiveness hole; loud, never silent).** Filed
2026-09-28 while making `stdlib/either.tur` generic
([stdlib-int-stand-in-audit](stdlib-int-stand-in-audit.md), S3).

## Repro

```turmeric
(defn inc [x : int] : int (+ x 1))
(defn main [] : int
  (println (ok-val (fmap (Ok 41) inc)))
  0)
```

```
error [TUR-E0001]: function 'ok-val' arg 1: expected (Result A B), got (? ?)
```

The same with `(Right 41)` and a generic `from-right` (the shape
`tests/fixtures/sum-either-functor-instance` pins, which passes today only
because `either.tur`'s accessors erase their argument to `:int`).  Annotating
the receiver makes both work:
`(let [r : (Result int int) (Ok 41)] (ok-val (fmap r inc)))` prints 42.

## Root cause

A constructor application of a parametric sum whose parameters are not all
determined by its arguments -- `(Ok 41)` fixes `A` but not `B` -- is typed as
the BARE ADT (`Result`, no applied arguments; a `let` annotation mismatch
reports it as "got adt").  The typeclass dispatch grounds a method's `(f b)`
result against the receiver's TY_APP chain
(hkt-carrier-result-loses-payload-types, `m7_app_replace_slot` in
src/compiler/elab_typeclasses.c), so with no chain there is nothing to ground
and the result stays the def-less `(type-app ? ?)` carrier shell, which no
`(Result A B)` parameter unifies with.

## Fix directions

1. Type such a constructor application as the application with its open
   parameters as fresh type variables -- `(Result int ?B)` -- so the dispatch
   has a chain to ground and a generic consumer unifies (`B` stays open, which
   `ok-val` never needs).  Wide blast radius: every bare-ADT consumer of a
   constructor result sees an application instead.
2. Narrower: let a generic parameter `(T A B)` accept the def-less shell when
   the call site has nothing better, binding nothing.  Cheaper, but it types
   less.

Landing either unblocks the generic `either.tur` recorded in the audit.
