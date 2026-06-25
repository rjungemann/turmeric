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

## Root cause (suspected)

The match field-bind for an outer carrier field of type `(Pair2 a a)`
substitutes the outer app args, but the *inner* match against that binding does
not re-resolve `a` from the value's construction site -- the element stays a
residual tyvar. This is an elaboration concern, independent of the codegen
by-value crossings (parametric-adt-byvalue-plan P2-P4), which only need the
ascribed/concrete types to lower correctly.
