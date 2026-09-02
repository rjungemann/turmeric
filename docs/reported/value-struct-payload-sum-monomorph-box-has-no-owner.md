# A stdlib `Result`/`Option` monomorph over a value-struct payload heap-boxes it, and nothing frees the box

**Severity: medium.** On the DEFAULT path, post-SR2a, with no experiment
involved -- so it affects ordinary `(Result MyStruct MyErr)` code rather than
an erased corner. Filed 2026-08-30, found sweeping the leaks RM1's measurement
turned up. **Retitled the same day** (was `wide-payload-...`): the first draft
blamed the B4 ">8 bytes" rule, and that was wrong -- see "The rule, precisely".

## Summary

SR2a's narrowing is recorded in
[carrier-sum-option-boxes-have-no-owner](carrier-sum-option-boxes-have-no-owner.md)
as:

> A CONCRETE `(Option T)` / `(Result T E)` monomorph now flows by value, so its
> constructor is a struct literal and there is no box to own.

**That holds for scalar and pointer payloads, not for value-struct payloads.**
When `T` is a non-parametric by-value product (a `defstruct`, or a
single-variant `defdata`), the monomorph's arm stores it as a POINTER and the
specialized constructor mallocs a fresh copy of its argument -- and nothing
releases it.

```c
/* (Result Rational ArithError): both payloads are value-structs */
typedef struct tur_adt_Result__Rational__ArithError {
    int tag;
    union {
        struct { tur_adt_Rational  * _0; } Ok;
        struct { tur_adt_ArithError * _0; } Err;
    } as;
} tur_adt_Result__Rational__ArithError;

static tur_adt_Result__Rational__ArithError
ok__spec__tur_adt_Result__Rational__ArithError_tur_adt_Rational(tur_adt_Rational x) {
    tur_adt_Rational *__t459 = (tur_adt_Rational *)malloc(sizeof(tur_adt_Rational));
    *__t459 = (x);                                   /* <- leaked, one per (ok r) */
    return ctor_Ok__Rational__ArithError(__t459);
}
```

The Result value itself is by value, exactly as SR2a says. The payload under
it is not.

## The rule, precisely

Width is irrelevant. Probed with an 8-byte `(defstruct S8 [a : int])` and a
16-byte `S16`: **both** arms are `tur_adt_S* * _0` and **both** spec ctors
malloc. The rule is `adt_field_is_ros_pointer_box` (types.c):

- the OWNER is `Result` or `Option` **by name** -- a user-defined
  `(defdata MyEither [A B] ...)` over the same payload does NOT take it; and
- the resolved payload is a non-`:heap`, non-parametric, by-value product.

It is deliberate, and `adt_field_c_type`'s comment gives the two reasons: the
monomorph then references `T` only by pointer, so a guarded forward typedef
suffices (the struct-with-Option-field typedef-ordering blocker), and the
8-byte slot matches the carrier-box layout the preamble helpers
(`tur_box_ok`/`tur_box_some`) produce. The malloc that fills the slot is the
`box_adt` promotion in emit_expr.c's ctor-argument path, keyed on the same
three conditions. So "do not box at all" (fix direction 2 below) would be
undoing a layout contract, not just removing an allocation.

"ros" is not width and not read-only-string: it is this rule's own name for
"stdlib Result/Option value-struct payload, stored boxed".

## Repro

`tests/fixtures/rational-overflow`, built against an ASan libturi:

```
Direct leak of 96 byte(s) in 6 object(s) allocated from:
    #1 ok__spec__tur_adt_Result__Rational__ArithError_tur_adt_Rational
Direct leak of 16 byte(s) in 4 object(s) allocated from:
    #1 err__spec__tur_adt_Result__Rational__ArithError_tur_adt_ArithError
```

Any `(ok <struct wider than 8 bytes>)` reproduces it.

## Population, measured

A static sweep of 2182 emitted fixtures finds **11** that emit a specialized
sum constructor whose body mallocs its payload. Running those under
LeakSanitizer, **9 leak with a `*__spec__*` constructor in the trace**, 465
bytes total:

| fixture | bytes |
|---|---:|
| `rational-overflow` | 112 |
| `constrained-instance-element-dispatch` | 109 |
| `byvalue-option-over-parametric-monomorph` | 72 |
| `conv-defstruct-result-struct-field-typedef-order` | 64 |
| `polymorphic-ok-err-value-struct-payload` | 32 |
| `result-over-struct-with-option-field-typedef-order` | 24 |
| `rational-basics` | 20 |
| `instance-method-return-carrier-bridge` | 16 |
| `nested-construct-byvalue-decode` | 16 |

(`rational-arith` emits the shape but runs clean; one more leaks with a
different trace.) Small per fixture, and per CONSTRUCTION -- a loop building
`(Result Rational E)` grows without bound.

## Why this is not the erased-path residue

Distinct from [RM1](../upcoming/reclamation-plan.md)'s subject in every way
that matters for a fix:

- It is the **specialized** path, not the erased base -- the monomorph is
  by-value and the constructor is a `*__spec__*` clone.
- The box is the **payload**, not the sum carrier, so making None null or
  freeing a carrier box does not touch it.
- Ownership is **unambiguous**: the constructor mallocs a fresh copy of its
  argument, so the monomorph that holds the pointer owns it outright. There is
  no `alt-or`-style pass-through hazard here, because the box did not exist
  before the call.

That last point is what makes this more tractable than RM1's residue rather
than less: the freshness question RM1 needed a whole analysis for is settled
by construction.

## Fix directions

1. **Drop glue on the monomorph.** The payload box is owned by exactly one
   monomorph value, so releasing it belongs with that value's lifetime --
   `needs_drop_glue` already exists for `TY_RC`/`TY_REF`/`TY_WEAK`/boxed-fn
   fields and for a nested owning aggregate (`elab_structs.c`), and a
   wide-payload arm is the same kind of owning field.

   **Correction (2026-08-30): `elab_effects.c:30`'s `n_ctors == 1` is NOT the
   blocker**, as an earlier draft of this report claimed. That predicate is
   `owning_byvalue_agg`, which governs effect/multishot admissibility. The
   drop-glue EMITTER already handles multi-variant defs -- `adt_glue_is_tagged`
   (`def->n_ctors > 1`) emits a `switch (s->tag)` with per-variant field loops.

   The real gap is one level earlier, in what MARKS a def as owning
   (`elab_structs.c`): a field is flagged `drop_inner_def` only when the INNER
   def itself `needs_drop_glue`. That tracks boxes whose *contents* need
   releasing; it has no rule for a box that is merely an owned allocation, which
   is what a wide payload is (`Rational` is two int64s and needs no glue of its
   own). And for a parametric monomorph the arms come from tyvars, so this
   field-level machinery never sees `Rational` at all -- `elab_structs.c:1425`
   already flags monomorph glue as "separate work".
2. **Do not box at all.** Store the wide payload inline in the union and let
   the monomorph be wider than 16 bytes. Removes the allocation instead of
   owning it, at the cost of a bigger by-value value on every crossing -- the
   same trade SR4 measured (7-13% time for 2.2x memory) and deliberately did
   not take, so it wants measuring before it is chosen.
3. **Scope-exit drop.** RM1's mechanism, keyed on the monomorph binding
   rather than the carrier. Cheapest of the three, and the narrowest: it
   covers a let-bound result whose uses are accessor-only, and leaves the
   escaping cases (returned, stored) alone.

Direction 1 is the one that scales, because the box has a single owner and the
machinery for single-owner release already exists.

## Direction 3, built: it reaches 2 of the 9 (2026-08-30)

The scope-exit drop (RM1's mechanism keyed on the monomorph BINDING, freeing
the live arm's payload box through a `switch` on the tag) is in, and it is
kept because it is measured rather than assumed: **465 -> 361 bytes**, the
9 leaking fixtures down to 7. It fires in exactly two --
`polymorphic-ok-err-value-struct-payload` (both arms; now fully ASan-clean and
carrying `requires.leak-check`, so the fix has 32 bytes of teeth) and
`byvalue-option-over-parametric-monomorph`. Predicate:
`adt_field_is_ros_pointer_box` over the substituted field type and nothing
else; conditions and escape walk identical to RM1's carrier drop (fresh
producer, by-value binding, accessor-only uses, trailing-only). Snapshot churn:
one fixture. Suite 2748/0 + that one regenerated; leak-check 63/0/0.

A first version of this was reverted the same day after measuring 465 -> 465.
Two findings from that round explain both the miss and the residue:

**The consumption shape is mostly not a let binding.** `rational-basics`
leaks through `(res-ok? (rat/of 3 4))` -- the Result is an argument to a USER
function, not a binding. RM1's accessor-argument mechanism does not reach it
either, and correctly so: `res-ok?` is not a known reader, and a by-value
struct argument could in principle have its arm pointer retained by the
callee. Freeing there needs a non-retention fact about the callee, which is
what `nonretain_param_mask` supplies for closures and would have to be
extended to cover this.

**The predicate discrepancy is resolved, and it was self-inflicted.** The
first attempt tested `type_is_wide_byval_adt(fres) &&
!adt_field_is_ros_pointer_box(def, &fres)` at the drop site -- copied from the
monomorph ctor emitter's `wide_box[]`, which is the B4 rule for a DIFFERENT
boxing. The arm in question is boxed by exactly the predicate that expression
NEGATES. The correct drop-site test is `adt_field_is_ros_pointer_box(def,
&resolved)` over the substituted field type, and nothing else; asked that way
it agrees with the typedef and the ctor-argument path (all three key on the
same rule).

## Related

- [carrier-sum-option-boxes-have-no-owner](carrier-sum-option-boxes-have-no-owner.md)
  -- the erased-path sibling, whose SR2a narrowing this report qualifies.
- [reclamation-plan.md](../upcoming/reclamation-plan.md) -- RM1 (built, erased
  path) and RM2 (the recursive spine, which `re-string`'s `ctor_RxCons` leaks
  belong to, not to this report).
