# Lowered Option/Result `#{Construct}` carrier base returns by-value into int64

**RESOLVED (boxed-aggregate).** Fixed in `seam-4: boxed-aggregate carrier base
for lowered Option/Result constructs`. The carrier of a lowered parametric
Option/Result is now a heap pointer to the record-ADT aggregate (layout-
identical to the legacy `tur_option_t`/`tur_result_box_t`, so the canonical
readback, `.is-some`/`.value`, and the legacy helpers all read it):

- `emit_fns.c`: the M2b carrier-synth gained a branch for the lowered ctor-call
  `#{Construct}` body -- it heap-boxes the aggregate and returns the pointer as
  the int64 carrier; an Option `none` (discriminator literal false) stays the
  NULL carrier so a bare `== 0` test keeps working.
- `emit_expr.c` (`fn_body_tail_byvalue_carrier_type`): a carrier-returning
  closure whose tail is a `some`/`ok` spec now heap-boxes the by-value spec
  result (accepts a concrete non-:heap aggregate, not just nominal carrier-ABI
  types).

Fixes `positional-opaque-ok`, `positional-pap-opaque-ok`,
`kleisli-arrow-instance` under force-lower. The other two fixtures the original
report listed (`hkt-ap-fn-in-container`, `hkt-stdlib-option-result-instances`)
turned out to have a DIFFERENT root -- the `some` spec arg type for a
`(fn [float] float)` element is mistyped as `double` instead of the int64
fat-closure handle -- now tracked separately in
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
