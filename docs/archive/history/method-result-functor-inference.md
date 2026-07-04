# Class-method result functor is not inferred from the receiver

**Status:** RESOLVED. A bare `(fmap (mk-box 5) inc)` now recovers `f := Box`
from the receiver and grounds its result to `(Box int)`, so the downstream
`(un-box r)` matches `(Box A)` with no ascription. Fix in `elab_method_call`
(`src/compiler/elab_typeclasses.c`); regression fixture
`tests/fixtures/method-result-functor-inference/`.

**Severity (was):** low (ergonomic; a one-token ascription or a fixed-index
constructor was the standing workaround, and no shipping feature depended on it).

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

## Root cause

When a class method's declared result is `(f b)` and the receiver argument has a
concrete `(F c)` type, the method-call result-type inference should pin `f := F`
(and `b` from the mapping function's result) instead of leaving both indices
`?`. This is the method-call analogue of the rank-2 result instantiation already
done in `elab_poly_call` (the `rfull->kind == TY_APP` branch,
`call_instantiate_type`) -- the same structural unify, applied at an ordinary
constrained-method call site rather than a rank-2 poly-fn call.

`elab_method_call`'s M7 HKT block already collected the bindings
(`f := Box`, `a := int`, `b := int`) and computed the substituted result
`(Box int)`, but committed it *only* when the instance body was
by-value-constructible (`m7_body_byvalue_ok`). `Box`'s Functor body delegates to
the global carrier helper `mk-box`, so `m7_body_constructs_byvalue` returned
false and the grounded result was discarded -- the call fell back to the def-less
`type_from_kind(TY_APP)` = `(type-app ? ?)`.

## Fix

In `elab_method_call` (`src/compiler/elab_typeclasses.c`), when the substituted
result fully grounds but the body is not by-value-constructible, still commit the
precise applied result type **if** (a) the current `result_type` is the def-less
`(type-app ? ?)` carrier shell we are trying to refine, and (b) the grounded
result is representationally the int64 carrier -- an opaque newtype
(`(defopaque Box [a] :int)`) or a transparent int-record newtype (helper
`m7_result_is_int_carrier`). For those the static type `(Box int)` and the
carrier `int64_t` the method returns are ABI-identical, so committing the type is
safe and `m7_byvalue_grounded` is deliberately left unset (dispatch stays on the
uniform carrier ABI; no by-value spec is minted).

Both guards matter: (a) protects an instance that overrode the class's `(f b)`
with a concrete scalar return (`fmap ... : float`, whose `result_type` is already
`float` here) from being clobbered -- see
`tests/fixtures/typeclass-instance-float-return/`; (b) keeps a genuine by-value
aggregate (Option/Result) on the by-value-spec path, avoiding the
carrier-vs-by-value layout mismatch.

## History

Split out of the (now resolved) van Laarhoven generic-inference report as its
one remaining, independent sub-item. See
[docs/archive/van-laarhoven-generic-inference-gap.md](../archive/van-laarhoven-generic-inference-gap.md).
