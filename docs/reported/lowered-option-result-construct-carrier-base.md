# Lowered Option/Result `#{Construct}` carrier base returns by-value into int64

**STILL OPEN. A boxed-aggregate attempt was made and REVERTED -- it was net-
negative under force-lower** (commits `seam-4: boxed-aggregate carrier base ...`
+ `... recognize lowered by-value Option/Result spec result ...`, reverted by
`seam-4: revert boxed-aggregate carrier base (net regression ...)`).

What was tried (boxed-aggregate, the report's option 2):

- `emit_fns.c` M2b carrier-synth: a branch for the lowered ctor-call
  `#{Construct}` body that heap-boxed the aggregate (none -> NULL, some/ok/err
  -> malloc'd box) and returned the pointer as the int64 carrier.
- `emit_expr.c`: broadened `expr_emits_byvalue_carrier_abi` /
  `fn_body_tail_byvalue_carrier_type` to treat ANY concrete non-:heap aggregate
  spec result as a by-value carrier producer (not just nominal carrier-ABI
  types), so the return/closure bridges would heap-box a `some`/`ok` spec
  result.

Why it regressed (force-lower only; the default suite does NOT lower
Option/Result, so it stayed 1863/0 and could not catch this -- a force-lower
sweep is required to see it):

- It fixed 4 (`positional-opaque-ok`/`-pap`, `kleisli-arrow-instance`,
  `typeclass-return-dispatch-result-wrapped`) but regressed ~9 previously-passing
  fixtures: a whole `option-*` cluster (`option-basic`, `option-of-tvec-eq`,
  `option-construct-byvalue-return-spec`, `option-consumers-byvalue-arg`,
  `option-map-capturing-closure`, `option-map-literal-none-unannotated-lambda`)
  plus `list-length`/`list-homog-byvalue-aggregate-element` and
  `constrained-instance-dispatch-parametric-container-element`.
- Two distinct over-reaches: (a) the M2b heap-box changed the lowered carrier
  representation in a way option consumers did not agree with (the carrier and
  by-value worlds for the SAME `(Option int)` must agree, and the construct base
  is only one of many producers); (b) the predicate broadening made heap-box
  bridges over-fire -- e.g. `(some 42)` in a let-init was boxed into an
  aggregate-typed binding (`tur_adt_Option__int x = (int64_t)(intptr_t)p;` ->
  "invalid initializer").

Lesson for the next attempt: the carrier vs by-value representation of a lowered
parametric Option/Result is ONE global decision -- changing only the construct
base (or only one predicate) desynchronizes producers and consumers. A correct
boxed-aggregate pass must flip the representation atomically across every
producer (construct base AND every `some`/`ok` call site) AND every consumer
(`.is-some`/`.value`, match, the carrier<->concrete bridges, the legacy
`tur_option_t`/`tur_result_box_t` helpers), and must be validated by a
FORCE-LOWER sweep, not just the default suite. Given that blast radius, option 1
(sentinel-preserving: the carrier base reproduces the legacy carrier exactly --
`return 0` for none, `tur_box_some`/`tur_box_ok` for the rest -- which is what
the make-struct M2b path already emits at default; the lowered ctor-form body
just needs the same legacy-carrier synth) is the lower-risk path and should be
tried first.

The two `hkt-*` fixtures the original report listed have a DIFFERENT root (`some`
of a `(fn [float] float)` element mistypes its spec arg as `double`), tracked in
`docs/reported/some-of-fn-element-spec-arg-mistyped.md`.

---

(Original report below.)

**Severity:** medium (force-lower only; blocks the `none`/`some`/`ok`/`err`
carrier-base + any program that consumes a polymorphic Option/Result through
the int64 carrier under defstruct-as-defadt). Default path unaffected.

## Summary

Under `defstruct-as-defadt` lowering, the stdlib `#{Construct}` templates
`some`/`none`/`ok`/`err` emit their CARRIER BASE (the generic, element-erased
`int64_t`-returning function) by calling the by-value record-ADT ctor and
returning the aggregate from an `int64_t` function:

```c
static int64_t none() {
        return ctor_Option__A(false, (int64_t){0});   /* tur_adt_Option__A is an aggregate */
}
```

`tur_adt_Option__A` is a by-value aggregate (`{bool; int64}`), so returning it
from an `int64_t` function is a hard cc error
(`incompatible types when returning type 'tur_adt_Option__A' but 'int64_t'`).
On the default path the same base lowers to `return 0;` (the carrier sentinel),
because make-struct of a *carrier* Option produces the carrier representation
directly.

## Affected fixtures (force-lower)

- `positional-opaque-ok`, `positional-pap-opaque-ok` -- `none()` base is DEAD
  (the program never calls it) but still must compile.
- `kleisli-arrow-instance` -- `(Result a cstr)` flowing through the carrier.
- `hkt-ap-fn-in-container`, `hkt-stdlib-option-result-instances` -- a
  `some__spec__...Option__opaque`/`Option__bool` arg ABI mismatch (the consumer
  side of the same carrier-vs-by-value split).

## Root cause

The carrier base of a parametric `#{Construct}` whose result is `(Option A)` /
`(Result A B)` with an UNGROUNDED element must produce the int64 carrier, but
the lowered make-struct -> ctor rewrite emits the by-value monomorph ctor
(`ctor_Option__A`) and no carrier bridge fires at the return:

- `fn_body_tail_emits_byvalue_carrier_abi` / `fn_body_tail_byvalue_carrier_type`
  (emit_fns.c) do not classify the degenerate ungrounded-`A` aggregate as a
  by-value carrier producer, so the concrete->carrier return spill is skipped
  and the raw aggregate is returned.

## Why this is a dedicated pass (not folded into the cluster fixes)

The deeper question is the carrier REPRESENTATION of a lowered Option/Result:
the legacy carrier helpers (`tur_none()` = `TUR_NONE` sentinel `0`,
`tur_is_some`, `tur_opt_value`, and the `tur_option_t`/`tur_result_box_t`
layouts) assume the SENTINEL/legacy-box convention, while a lowered record ADT
carried as a boxed `tur_adt_Option__A *` pointer is a DIFFERENT convention.
Making the base merely heap-box the aggregate fixes the two dead-`none` fixtures
but, for the live consumers (kleisli / hkt-*), the producer and every consumer
(`.is-some`/`.value`/pattern match/the dict-ABI slot) must agree on one
convention. That is a cross-cutting representation decision spanning stdlib
option.tur/result.tur, the carrier-bridge, and the legacy `tur_option_t`
helpers -- worth its own focused pass rather than a point patch.

## Fix direction

Pick one carrier convention for a lowered parametric Option/Result and apply it
end to end:

1. **Sentinel-preserving** (matches default): the carrier base produces the
   legacy carrier (`0` for none, the existing boxed form for some) and every
   lowered consumer keeps reading the legacy `tur_option_t`/`tur_result_box_t`
   layout off the carrier. Lowest blast radius; keeps the legacy helpers
   authoritative for the carrier path.
2. **Boxed-aggregate**: the carrier is a heap `tur_adt_Option__A *`; retire the
   legacy helpers on the lowered path and route every consumer through the ADT
   deref (the canonical readback at emit_core.c ~L3493 already exists for the
   carrier->concrete direction). Larger, but removes the legacy box entirely.

Until then the force-lower carrier base of these constructs does not compile;
the default by-value path (which constructs the aggregate directly and never
takes the carrier base) is unaffected.
