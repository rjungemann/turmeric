# Lowered Option/Result `#{Construct}` carrier base returns by-value into int64

## RESOLVED 2026-06-28 -- return-tail-scoped carrier-return bridge

**This is fixed in the current tree and verified by a force-lower sweep.** The
carrier base of a lowered parametric Option/Result `#{Construct}`
(`some`/`none`/`ok`/`err`) now compiles and runs correctly: when the C return
type is the uniform `int64_t` carrier but the body tail produces a by-value
`tur_adt_Option__A` / `tur_adt_Result__A__B` aggregate (the lowered
make-struct -> monomorph ctor), the function-return path heap-spills the
aggregate and returns its pointer as the int64 carrier.

Root fix (landed by #572, `9b4dab0`): the concrete->carrier **return bridge**
at `src/compiler/emit_fns.c:1569-1603`, gated by
`fn_body_tail_emits_byvalue_carrier_abi` / `fn_body_tail_byvalue_carrier_type`
(`src/compiler/emit_expr.c`). It emits:

```c
static int64_t none() {
    { tur_adt_Option__A *__tur_ret_p = (tur_adt_Option__A *)malloc(sizeof(tur_adt_Option__A));
      *__tur_ret_p = ctor_Option__A(false, (int64_t){0});
      return (int64_t)(intptr_t)__tur_ret_p; }
}
```

The heap-boxed pointer is byte-compatible with the carrier convention every
consumer already derefs (`{is_some,value}` / `{is_ok,ok_val,err_val}`), so
producer and consumer agree without a global representation flip.

### Why this succeeded where the report's two attempts (below) regressed

Both abandoned attempts touched the wrong seam:

- **Option 1 (sentinel-preserving)** rewrote the *base's representation*
  (`return 0` / `tur_box_some`), creating a carrier shape that disagreed with
  the by-value consumers the same `(Option int)` reaches elsewhere -> desynced
  the `option-*` cluster.
- **Option 2 (boxed-aggregate)** broadened the *general* expression predicate
  `expr_emits_byvalue_carrier_abi`, so the heap-box bridge over-fired in
  non-return positions -- e.g. `(some 42)` in a let-init was boxed into an
  aggregate-typed binding ("invalid initializer").

The landed fix instead heap-spills the **same** by-value aggregate the by-value
world uses, **only at the function-return tail** (`fn_body_tail_*`, not the
general expr predicate). That keeps one representation (a heap pointer to the
by-value aggregate) and never fires in expression position, sidestepping both
failure modes. The report's "no piecemeal fix is possible" conclusion was
premature: the piecemeal fix exists -- it just had to land at the return seam,
not the base body or the general predicate.

### Verification

- Reproduced the original failure mode with a temporary `TUR_FORCE_LOWER`
  bypass at the top of `defstruct_lowers_to_adt` (forces every well-formed
  `defstruct` to lower), then removed it.
- Under that force-lower probe, all fixtures this report named build **and**
  run with correct stdout: `positional-opaque-ok`, `positional-pap-opaque-ok`,
  `kleisli-arrow-instance`, `hkt-ap-fn-in-container`,
  `hkt-stdlib-option-result-instances`, plus the entire regressed cluster the
  two attempts broke -- `option-basic`, `option-of-tvec-eq`,
  `option-construct-byvalue-return-spec`, `option-consumers-byvalue-arg`,
  `option-map-capturing-closure`, `option-map-literal-none-unannotated-lambda`,
  `list-length-byvalue-aggregate-element`,
  `list-homog-byvalue-aggregate-element`,
  `constrained-instance-dispatch-parametric-container-element`.
- The remaining force-lower-only failures are unrelated to this report
  (compound / applied-type struct fields the field-lowerability gate
  legitimately keeps on the struct path, plus two negative-test diagnostics) --
  not the carrier-base bug.
- Default gate green: `bash tests/run.sh` => `1871 passed, 0 failed`.

The two `hkt-*` fixtures' separate root (a `some` of a `(fn [float] float)`
element mistyping its spec arg as `double`) remains tracked in
`docs/reported/some-of-fn-element-spec-arg-mistyped.md` if still open.

---

(Original report below, retained for the paper trail.)

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
FORCE-LOWER sweep, not just the default suite.

## Option 1 (sentinel-preserving) ALSO regressed -- TRIED 2026-06-28, ABANDONED

This report previously recommended option 1 (the lowered ctor-form carrier base
reproduces the legacy carrier exactly -- `return 0` for none,
`tur_box_some`/`tur_box_ok` for the rest -- the same synth the make-struct M2b
path emits at default) as the lower-risk path. **That hypothesis is wrong.** A
ctor-form sibling of the M2b synth was implemented (emit_fns.c) and validated
with a force-lower-targeted regression test: it fixed `positional-opaque-ok` /
`-pap` (+2) but RE-REGRESSED the entire `option-*` cluster (-6: option-basic,
option-of-tvec-eq, option-construct-byvalue-return-spec,
option-consumers-byvalue-arg, option-map-capturing-closure,
option-map-literal-none-unannotated-lambda) -- net-negative, the SAME failure
mode as option 2. The change was abandoned (not committed).

Why option 1 fails too: under lowering the by-value path is ALSO live (a
`some__spec` returns the `tur_adt_Option__int` aggregate directly, and
consumers read it by value), so making the carrier BASE emit the legacy boxed
carrier creates two coexisting representations for the same `(Option int)`.
`option-basic` mixes them in one program -- `some?(none())` routes through the
carrier base while `some?(some(42))` routes through the by-value spec -- so any
carrier-base representation that differs from what the by-value consumers expect
desyncs. At DEFAULT this never bites because Option/Result are not lowered, so
the legacy carrier is the ONLY representation; the make-struct M2b synth is
correct there precisely because there is no by-value path to disagree with.

**Conclusion: there is no piecemeal carrier-base fix.** Both options regress
`option-*`. The carrier base cannot be touched independently of the global
representation decision. The only correct path is the atomic flip (every
producer + consumer in one change, force-lower-swept). Until then the 2
`positional-*` fixtures (whose `none()` base is dead but must compile) stay
blocked -- a dead-base compile-only shim that is NEVER reachable at runtime
might unblock just those without a representation change, but proving
unreachability across the lowered program is itself non-trivial.

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
