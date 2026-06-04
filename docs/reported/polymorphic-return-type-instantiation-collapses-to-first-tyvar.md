# Polymorphic accessor return type collapses to the wrong tyvar when consumed without an expected type

**Summary:** `tuple2-2nd : [A B] (Tuple2 A B) -> B` infers its result as `A`
(not `B`) when the call result is consumed *without* an explicit expected type
(e.g. as an argument to another call, or as a `let` binding later re-used).
With a direct return annotation the same expression checks correctly, so the
bug is in result-type instantiation under inference, not in the signature.

**Severity:** Medium -- type-level miscompile surfaced as a confusing
`TUR-E0001` ("expected Tuple2, got int") on the *outer* call; can also mask a
genuine error. Discovered while landing the `[T1..Tn]` tuple-type surface sugar;
**not caused by it** -- reproduces with the explicit `(Tuple2 ...)` annotation.

## Minimal repro

```turmeric
;; t : Tuple2 int (Tuple2 cstr int); want the inner Tuple2's 2nd element.
(defn inner [t : (Tuple2 int (Tuple2 cstr int))] : int
  (tuple2-2nd (tuple2-2nd t)))      ; inner call's B = (Tuple2 cstr int)
```

## Observed

```
error [TUR-E0001]: function 'tuple2-2nd' arg 1:
  expected (type-app (type-app Tuple2 tyvar) tyvar), got int
  (tuple2-2nd (tuple2-2nd t)))
              ^^^^^^^^^^^^^^
```

The inner `(tuple2-2nd t)` is inferred to have type `int` (the value of `A`)
instead of `(Tuple2 cstr int)` (the value of `B`). The outer call then
correctly complains that `int` is not a `Tuple2`.

A `let`-bound intermediate does **not** help -- same error:

```turmeric
(let [m (tuple2-2nd t)]   ; m inferred as int, should be (Tuple2 cstr int)
  (tuple2-2nd m))
```

## Expected (and the asymmetry that localizes the bug)

With a direct return annotation, the *single* application checks fine:

```turmeric
(defn inner [t : (Tuple2 int (Tuple2 cstr int))] : (Tuple2 cstr int)
  (tuple2-2nd t))                   ; OK -- B resolves to (Tuple2 cstr int)
```

So when an explicit expected type drives unification, `B` is resolved
correctly. When the result is consumed in an inference context (argument
position / let binding), the instantiation picks `A` instead of `B`.

## Root-cause direction

The return-type instantiation for a polymorphic call substitutes the wrong
type-variable binding when no expected type is present. Likely the
substitution map for `tuple2-2nd`'s declared return `B` is being keyed by
positional tyvar index and mis-indexed (B's slot reads A's binding), and the
bug is only masked when a top-down expected type forces the correct unification
afterward. Note it manifests specifically when `B` is itself a *composite*
(`Tuple2 cstr int`); when `B` is a primitive (e.g. `cstr`) the same accessor
works in inference context (`tuple2-2nd` of a `[int cstr]` returns `cstr`
correctly and runs).

Pointers to chase:
- the call-result instantiation path for polymorphic `defn`s (where the
  declared return type's tyvars are substituted with the inferred argument
  tyvar bindings);
- how that path differs when an expected/checked type is threaded in vs. when
  the result is synthesized bottom-up;
- whether the substitution is keyed by tyvar *identity* or by positional
  index (a positional mix-up would explain A<->B).

## Validation of a fix

- The `inner` repro (double application) checks and, once the sibling TupleN
  parameter-ABI bug is also fixed, runs returning `2`.
- A primitive-`B` regression (`tuple2-2nd` of `[int cstr]` in argument
  position) still yields `cstr`.
- Add a fixture chaining `tuple2-2nd`/`tuple2-1st` over a nested tuple and
  asserting the extracted value.
