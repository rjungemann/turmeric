# Polymorphic accessor return type collapses to the wrong tyvar when consumed without an expected type

> **Status:** FIXED 2026-06-04. Root cause and fix below; regression coverage
> in `tests/fixtures/poly-nested-tuple-accessor`.
>
> **Correction to the original title/summary:** the result did not collapse to
> "the wrong tyvar" (A vs B). The instantiation was actually *correct* (B bound
> to its concrete composite); the bug was a later step that discarded the full
> composite type and reported the int *carrier*, which only happened to render
> as "int" -- the same kind A held here. See the root cause.

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

## Resolution (FIXED)

Root cause (`elab_call.c`, the EX_CALL result-type step, ~line 2680): the
declared result of a polymorphic accessor like `tuple2-2nd` is a *bare type
variable* (`:B`), so `fn_type.as.fn.result_kind == TY_TYVAR`. The call path
already instantiated the *full* result type correctly via
`call_instantiate_type` (`result_type` came out as the concrete
`(Tuple2 cstr int)`, kind `TY_APP` -- verified by tracing). But a downstream
guard then unconditionally did:

```c
call_result_type = TYPE_INT;                       /* int64 carrier */
wrap_generic_result = (result_type.kind != TY_INT);
...
return call_wrap_reinterpret(e, out, result_type.kind, ...);
```

This is correct for a *scalar* instantiation (e.g. B = float): the ABI carries
the result as int64 and the reinterpret bitcasts it back to the scalar's
register class. But for a *composite* (`TY_APP` / struct / ADT),
`type_size_bytes` is 0, so `call_wrap_reinterpret` no-ops and returns the call
with type `TYPE_INT` -- the full `(Tuple2 cstr int)` is silently discarded. The
outer accessor then sees an `int` argument where it expects a `Tuple2` and
errors. With an explicit return annotation the loss was masked: the body was
checked top-down against the declared composite type, which drove unification
before the lossy carrier collapse mattered.

Fix: collapse to the int carrier (and emit the reinterpret) only when the
instantiated `result_type` is a scalar. When it is a concrete composite
(`TY_APP`, or a `TY_ADT`/`TY_STRUCT` with a non-NULL def), keep the full
`result_type` as the call's type -- the carrier is already int64 at the ABI
level, and the ABI-specialization bindings saved on the call drive emit to
produce the concrete-by-value clone. (This is also why the sibling TupleN
by-parameter ABI fix had to land first: chained accessors now genuinely pass a
`TupleN` result to the next accessor.)

Validation: `deep-2nd` returns `2`, `deep-1st` returns `"hi"`, the mirror
`tuple2-1st`-returning-composite case works, let-bound intermediates work, and
the primitive-`B`-in-arg-position case is unchanged. Full suite 1432 passed /
0 failed; `run-turi` (124), `run-flags` (77), `run-stdlib-checks` (31) all
green.
