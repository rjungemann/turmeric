# method-result-functor-inference -- fix paper trail

Resolved report: [../method-result-functor-inference.md](../method-result-functor-inference.md).

## Symptom

A bare class-method call whose declared result is `(f b)` did not recover the
functor `f` from the receiver's concrete type:

```
(defopaque Box [a] :int)
(defn mk-box [A] [x : A] : (Box A) ```c return (int64_t)x; ```)
(defn un-box [A] [b : (Box A)] : A ```c return (int64_t)b; ```)
(definstance Functor [Box]
  (fmap [b g] (mk-box (g (un-box b)))))

(fmap (mk-box 5) (fn [x : int] : int (+ x 1)))   ; result came back (type-app ? ?)
```

`un-box r` then failed with TUR-E0001: `expected (type-app Box tyvar 'A'), got
(type-app ? ?)`.

## Root cause (file:line)

`src/compiler/elab_typeclasses.c`, `elab_method_call`. The M7 HKT block collected
the tyvar bindings (`f := Box`, `a := int`, `b := int`) and computed the
substituted result `(Box int)`, but committed it only under `m7_body_byvalue_ok`
-- true only when the instance body constructs its `(f b)` result in-body via an
ADT/`#{Construct}` constructor. `Box`'s Functor body delegates to the global
carrier helper `mk-box`, so `m7_body_constructs_byvalue` returned false and the
grounded result was dropped; the call fell back to `type_from_kind(TY_APP)` =
`(type-app ? ?)`.

## Fix

Added helper `m7_result_is_int_carrier(Type)` (true for an opaque newtype head or
a transparent int-record newtype -- both lower to `int64_t`). Extended the M7
commit: when the substituted result fully grounds but the body is not
by-value-constructible, still commit the precise applied result type when
(a) the incoming `result_type.kind == TY_APP` (we are refining the def-less
carrier shell, not a scalar-overriding instance) and (b)
`m7_result_is_int_carrier(substituted)`. `m7_byvalue_grounded` is left unset so
dispatch stays on the uniform carrier ABI and no by-value spec is minted -- the
representation is already the carrier `int64_t`.

## Guard rationale

- Guard (a) protects `tests/fixtures/typeclass-instance-float-return/`: an
  instance overriding the class's `(f b)` with `: float` has `result_type ==
  float` here; without the `TY_APP` gate the recovered `(BoxW int)` (a
  transparent int record) would clobber the float back to 6 (was 6.5).
- Guard (b) keeps genuine by-value aggregates (Option/Result) on the by-value
  spec path, avoiding the carrier-vs-by-value layout mismatch.

## Verification

- New regression fixture `tests/fixtures/method-result-functor-inference/`
  (prints 6).
- `tests/fixtures/typeclass-instance-float-return/` still prints 6.5.
- Option (by-value aggregate) `fmap` still round-trips (6).
- Full suite: `bash tests/run.sh` -> 1925 passed, 0 failed.
