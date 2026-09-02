# A by-value sum monomorph with a WIDE payload still heap-boxes it, and nothing frees the box

**Severity: medium.** On the DEFAULT path, post-SR2a, with no experiment
involved -- so it affects ordinary `(Result MyStruct MyErr)` code rather than
an erased corner. Filed 2026-08-30, found sweeping the leaks RM1's measurement
turned up.

## Summary

SR2a's narrowing is recorded in
[carrier-sum-option-boxes-have-no-owner](carrier-sum-option-boxes-have-no-owner.md)
as:

> A CONCRETE `(Option T)` / `(Result T E)` monomorph now flows by value, so its
> constructor is a struct literal and there is no box to own.

**That holds only for payloads of 8 bytes or less.** A wider by-value payload
is stored in the arm as a POINTER (the B4 "wide by-value element" rule,
`type_is_wide_byval_adt`), so the specialized constructor mallocs the payload
and hands back a monomorph that owns it -- and nothing releases it.

```c
/* `Rational` is {int64 num; int64 den} = 16 bytes, so the arm holds a pointer */
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
   wide-payload arm is the same kind of owning field. The blocker is the one
   `elab_effects.c:30` states: the gate is `n_ctors == 1`, so it never fires
   for a sum. Widening that gate is the shape of the fix.
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

## Related

- [carrier-sum-option-boxes-have-no-owner](carrier-sum-option-boxes-have-no-owner.md)
  -- the erased-path sibling, whose SR2a narrowing this report qualifies.
- [reclamation-plan.md](../upcoming/reclamation-plan.md) -- RM1 (built, erased
  path) and RM2 (the recursive spine, which `re-string`'s `ctor_RxCons` leaks
  belong to, not to this report).
