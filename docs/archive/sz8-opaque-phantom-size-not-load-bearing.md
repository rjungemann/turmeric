---
title: SZ8 cross-parameter size unification does not cover non-GADT phantom indices (P1 finding for ECS E2c)
category: Expressiveness hole / elaborator gap
severity: Central elaborator gap blocking ECS E2c's "bounded-capacity world API" design path. Without it, no opaque/struct-carried phantom size index can witness static rectangularity, so the planned `Dense<n, T>` substrate cannot be built.
description: P1 of `docs/reported/ecs-e2c-sized-dense-needs-bounded-world.md` asked whether the SZ8 cross-parameter size-variable unification that landed on 2026-06-10 fires on a `defopaque` (or `defstruct`) carrier where the size index is a phantom rather than a GADT-constructor-witnessed index. The answer is no, on two independent counts. (1) Vacuous-accept: `(zip (mk-dense) (mk-dense))` against `zip [xs : (Dense n A) ys : (Dense n B)]` compiles trivially because both calls produce fresh `?n` unification vars; nothing rectangular is witnessed. (2) Reject-unreachable: pinning `n` to a Size literal in a function's return-type position -- e.g. `(defn mk-dense-2 [A] [] : (Dense (Static 2) A) ...)` -- is rejected at PARSE time with "unsupported type expression form (expected symbol, keyword, or list)". The Size GADT's `Static` literal is only accepted inside GADT-constructor return-type signatures, not inside arbitrary type-app slots. Until that parser/elaborator gap is closed, no opaque- or struct-carried phantom size index can carry a load-bearing size, and the bounded-capacity world API described in the parent report's Option 2 cannot be built.
status: CLOSED 2026-06-12. Both halves were fixed in one pass: (1) `type_expr_from_form` and `fn_type_from_form_impl` now accept `(Static N)`, `(Add s s)`, and `(Mul s s)` as first-class arguments in any type-application slot (lowered to a TY_INT placeholder; the actual size info rides on the retained Form). (2) `Type.as.fn.result_type_form` now stores the raw return-type annotation Form on TY_FN, and `sz_cross_param_unify` recovers a call expression's size index from the callee's declared return form when no GADT-constructor witness is available. Companion fixtures: `tests/fixtures/sized-cross-param-opaque-accept/` (literal-vs-literal unification) and `tests/fixtures/errors/sized-cross-param-opaque-reject/` (TUR-E0260 on `(Dense (Static 2) ...)` vs `(Dense (Static 3) ...)`).
---

# SZ8 cross-parameter unification does not cover non-GADT phantom size indices

> **Resolution (2026-06-12).** Both the parser gap (Probe 2) and the
> elaborator gap (Probe 1) were closed in one pass. The remainder of
> this document is preserved as the original P1 investigation; the
> sections below describe the situation *before* the fix.
>
> - Parser: `type_expr_from_form` (`src/compiler/elab_types.c`) and
>   `fn_type_from_form_impl` (`src/compiler/elab_fns.c`) now accept
>   `(Static N)` / `(Add s s)` / `(Mul s s)` as first-class type-app
>   arguments, lowered to a TY_INT placeholder; the real size info
>   rides on the retained Form.
> - Inference: a new `result_type_form` field on TY_FN
>   (`src/compiler/types.h`) retains the callee's declared return
>   annotation; `sz_cross_param_unify` (`src/compiler/elab_call.c`)
>   recovers the call expression's size index from that form when no
>   GADT-constructor witness is available.
> - Witnesses: `tests/fixtures/sized-cross-param-opaque-accept`
>   (literal-vs-literal accept) and
>   `tests/fixtures/errors/sized-cross-param-opaque-reject`
>   (TUR-E0260 on `(Static 2)` vs `(Static 3)`).

## Summary

`docs/reported/ecs-e2c-sized-dense-needs-bounded-world.md` listed P1 as
the central compiler-side question: does the SZ8 cross-parameter size
unification fixture
([`tests/fixtures/sized-cross-param-accept`](../../tests/fixtures/sized-cross-param-accept/input.tur))
generalize from GADT-witnessed indices to phantom indices on a
`defopaque` or `defstruct` carrier?

The empirical answer is **no**, on two independent counts.

## Probe 1 -- Vacuous accept (`defopaque Dense [n A]`)

Probe:

```turmeric
(defgadt Size []
  (Static (int) : (Size))
  (Add (Size) (Size) : (Size))
  (Mul (Size) (Size) : (Size)))

(defopaque Dense [n A] :int)

(defn mk-dense [n A] [] : (Dense n A)
  (:: 0 :Dense))

(defn zip [A B] [xs : (Dense n A) ys : (Dense n B)] : int 0)

(defn main [] : int
  (println (zip (mk-dense) (mk-dense)))
  0)
```

This compiles and runs (`./build/tur -Xsized-types run`) printing `0`.
But the unification is vacuous: each `(mk-dense)` produces a `(Dense ?n A)`
with a fresh unification variable, and the call site equates `?n1 = ?n2 = ?n_zip`
without ever pinning `n` to a literal. This is exactly Option 3 in the
parent report -- "Two storages 'unify' because they have no constraints,
so the loop bound is still the runtime cap." Captured as a fixture at
`tests/fixtures/sized-cross-param-opaque-accept/`.

## Probe 2 -- Reject unreachable (parse failure on `(Static k)` in type-app)

The reject companion needs callers that pin `n` to *distinct literal
sizes*, e.g.:

```turmeric
(defn mk-dense-2 [A] [] : (Dense (Static 2) A)
  (:: 0 :Dense))

(defn mk-dense-3 [A] [] : (Dense (Static 3) A)
  (:: 0 :Dense))
```

This fails at PARSE time, not at unification:

```
error: unsupported type expression form (expected symbol, keyword, or list)
11 | (defn mk-dense-2 [A] [] : (Dense (Static 2) A)
   |                                          ^
```

The pointer is at the int literal `2` -- the parser walks the type-app
`(Dense ...)` and rejects the inner `(Static 2)` because the `2` is an
int literal in a position where it expects a "symbol, keyword, or list."

For comparison, `(SizedVec (Static 0))` *does* parse, but only because
it appears in the return-type signature of a GADT constructor
(`SVNil : (SizedVec (Static 0))`), which goes through a distinct,
permissive parse path. Outside that narrow context the same form is
rejected. So even before SZ8 unification has a chance to fire, the
surface syntax cannot express a literal-pinned non-GADT size.

## Why this matters for ECS E2c

The parent report's Option 2 (bounded-capacity world API) requires:

```turmeric
(defopaque Dense [n A] :int)
(defstruct World n
  [pos : (Dense n Pos)
   vel : (Dense n Vel)])
(world-new 1024) : (exists n. (World n))
```

with every storage handle typed `(Dense n A)` and `n` threaded through.
That depends on:

1. The parser accepting Size literals (or any kind-correct Size
   expression) anywhere a sized type-app appears -- not just inside
   GADT constructor signatures.
2. The SZ8 unification machinery extending to *non-GADT* phantom
   indices, so that two distinct sites that pin `n` to mismatched
   sizes are rejected at compile time.

Today (1) fails before (2) gets a chance to run, so the answer to
P1 is unambiguously negative.

## Root-cause directions

The parse-time error originates in the type-expression reader (the
"expected symbol, keyword, or list" message string). The fix is
likely a small generalization of the type-app argument reader to
accept the Size GADT constructors (`Static`, `Add`, `Mul`) as
first-class type-expression atoms wherever a type-app slot is parsed
-- the same treatment that GADT-constructor return-type signatures
already get.

Once parsing is unblocked, the second question is whether SZ8's
unification machinery is GADT-specific. The existing reject
fixture `tests/fixtures/errors/sized-cross-param-reject` triggers
TUR-E0260 via the constructor-chain-derived witness on `SizedVec`;
a non-GADT phantom would need a different witness path (the
declared return type of `mk-dense-2`). If that path is missing,
filling it is the second sub-fix.

## Proposed fix directions

1. **Lift Size GADT applications to first-class type-expression
   elements** in the type-app argument reader. Search for the
   "unsupported type expression form" diagnostic to find the rejection
   site; permit application heads whose nominal type is `Size`.
2. **Extend SZ8 cross-parameter unification** to fire when the
   index witness comes from a function's declared return type
   (not a GADT constructor chain). The existing fixture pair
   `sized-cross-param-{accept,reject}` becomes the GADT regression;
   a new pair using `defopaque Dense [n A]` becomes the non-GADT
   regression.
3. **Update `docs/reported/ecs-e2c-sized-dense-needs-bounded-world.md`**
   to record that P1's answer is "no, central elaborator gap" -- and
   to keep P2 (struct field-share unification) blocked behind this one.

## Validation of a fix

- `(defn mk-dense-2 [A] [] : (Dense (Static 2) int) (:: 0 :Dense))`
  parses and elaborates.
- A new fixture
  `tests/fixtures/errors/sized-cross-param-opaque-reject` with
  matching code raises TUR-E0260 when callers mismatch `n` literally.
- `tests/fixtures/sized-cross-param-opaque-accept` continues to
  compile -- and ideally tightens from "vacuous unification on fresh
  vars" to "literal-vs-literal unification at (Static 2)".

## Related

- `docs/reported/ecs-e2c-sized-dense-needs-bounded-world.md` (parent
  report -- P1 owner)
- `docs/archive/history/sized-types-phantom-index.md` (the original
  SZ6-SZ8 gap; resolved for GADT-witnessed indices only)
- `tests/fixtures/sized-cross-param-accept/input.tur` (GADT reference;
  passes)
- `tests/fixtures/errors/sized-cross-param-reject/input.tur` (GADT
  reject; passes -- TUR-E0260 fires)
- `tests/fixtures/sized-cross-param-opaque-accept/input.tur` (this
  report's witness fixture; passes vacuously)
