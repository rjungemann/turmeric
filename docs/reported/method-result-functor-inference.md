# Class-method result functor is not inferred from the receiver

**Severity:** low (ergonomic; a one-token ascription or a fixed-index
constructor is the standing workaround, and no shipping feature depends on it).

## Summary

A bare class-method call whose declared result is `(f b)` does not recover the
functor `f` from the receiver's concrete type. Calling `fmap` on a `(Box int)`
value:

```
(defopaque Box [a] :int)
(defn mk-box [A] [x : A] : (Box A) ```c return (int64_t)x; ```)
(defn un-box [A] [b : (Box A)] : A ```c return (int64_t)b; ```)
(definstance Functor [Box]
  (fmap [b g] (mk-box (g (un-box b)))))

(defn main [] : int
  (let [r (fmap (mk-box 5) (fn [x : int] : int (+ x 1)))]
    (println (un-box r)))     ; wanted: 6
  0)
```

fails to type-check:

```
error [TUR-E0001]: function 'un-box' arg 1: expected (type-app Box tyvar 'A'),
got (type-app ? ?)
```

The `fmap` result comes back as a def-less `(type-app ? ?)` -- the functor `Box`
was not recovered from the `(Box int)` receiver, so the later `un-box` cannot
match its `(Box A)` parameter.

## Workarounds (both ship today)

- Ascribe the method result: `(:: (fmap (mk-box 5) f) (Box int))`.
- Use a fixed-index constructor whose full result type is explicit, e.g.
  `(defn mk-const [A] [x : A] : (Const A A) ...)` -- this is exactly why the van
  Laarhoven lens fixtures (`tests/fixtures/van-laarhoven-lens-*`) type-check
  without ascriptions.

## Root cause direction

When a class method's declared result is `(f b)` and the receiver argument has a
concrete `(F c)` type, the method-call result-type inference should pin `f := F`
(and `b` from the mapping function's result) instead of leaving both indices
`?`. This is the method-call analogue of the rank-2 result instantiation already
done in `elab_poly_call` (the `rfull->kind == TY_APP` branch,
`call_instantiate_type`) -- the same structural unify, applied at an ordinary
constrained-method call site rather than a rank-2 poly-fn call.

## History

Split out of the (now resolved) van Laarhoven generic-inference report as its
one remaining, independent sub-item. See
[docs/archive/van-laarhoven-generic-inference-gap.md](../archive/van-laarhoven-generic-inference-gap.md).
