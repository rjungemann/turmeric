# An `(Option rc<A>)` can be returned, matched and destructured -- but not constructed in Turmeric

**Severity: medium.** An expressiveness hole with a measurable cost already
paid in stdlib, and a stated rationale that is factually stale. Filed
2026-08-30, split out of
[inline-c-option-carrier-box-leaks](../archive/inline-c-option-carrier-box-leaks.md),
whose fix made it non-blocking but did not address it -- that report called
this "probably the more interesting half".

## Summary

`(Option rc<A>)` is a first-class type in every position except the one that
builds it:

| operation | accepted? |
|---|---|
| declare it as a return type (`: (Option rc<A>)`) | **yes** -- `stdlib/weak.tur:97` ships it |
| build it in inline C (`tur_some_ptr(cb)`) | **yes** -- same function |
| match on it (`(match o (Some s) s ...)`) | **yes** -- `stdlib/weak.tur:124` |
| build it in Turmeric (`(some x)`) | **NO** |
| read it with the generic `unwrap` | **NO** |

The two rejections give the same diagnostic:

```
error: cannot store an owning value (rc) in a collection: elements go through
an int64 carrier that cannot hold a reference the collection would have to
own. Store a plain handle, or keep the value outside the collection
```

There is no collection anywhere in the program.

## Repro

Construction:

```turmeric
(defn weak-up-raw [A] [^borrow w : weak<A>] : rc<A>
  ```c
  return rc_upgrade(w);
  ```)

(defn weak-up [A] [^borrow w : weak<A>] : (Option rc<A>)
  (if (weak-try-upgrade w) (some (weak-up-raw w)) (none)))
;;                          ^^^^^^^^^^^^^^^^^^^^ error, at the `some` argument
```

Reading, in the same shape:

```turmeric
(defn g [A] [o : (Option rc<A>)] : rc<A> (unwrap o))
;;                                        ^^^^^^^^ same error
```

`Result` is affected identically -- `(ok (rc/of 7))` at
`: (Result rc<int> int)` is the same diagnostic.

## Root cause

`elab_call.c:5613`: when a callee's expected parameter is a **tyvar**
(`some [A] [x : A]`, `unwrap [A] [o : (Option A)]`), the argument is
reinterpreted to `TY_INT` -- the erased carrier word -- and
`call_wrap_reinterpret_owning` (`elab_call.c:1045`) rejects an owning source
kind with `OWN_CARRY_REJECT` (the diagnostic at `:1066`).

So the check fires on the **generic signature's parameter erasure**, not on
anything about collections. `vec-push!` and `map-set!` reach it the same way,
which is where the wording comes from, and the rejection is correct for them.

## Why the stated rationale is stale

The diagnostic's claim is "elements go through an int64 carrier that cannot
hold a reference". **Measured against what the compiler actually emits for a
concrete monomorph, that is false** -- the payload slot is a typed pointer, not
a carrier word:

```c
typedef struct tur_adt_Option__rc_struct {
    int tag;
    union {
        struct { } None;
        struct { RcControlBlock * _0; } Some;   /* <- not an int64 carrier */
    } as;
} tur_adt_Option__rc_struct;
```

That is SR2a: a concrete `(Option T)` / `(Result T E)` monomorph flows BY
VALUE, so there is no carrier erasure for the constructed value to lose a
count through. The check predates that and was never revisited.

The honest caveat, and why this is a report rather than a patch: an erased
`(Option A)` base -- a fully generic body that never specializes -- still rides
the int64 carrier, and storing an owning rc there really would drop the count.
So the rejection is right for the erased path and wrong for the monomorphic
one, and the fix has to tell them apart rather than simply deleting the check.

## What it costs today

- **`stdlib/weak.tur` hand-rolls what stdlib already provides.**
  `weak/unwrap` matches in place with a comment explaining that it cannot
  delegate to the generic `unwrap` (`stdlib/weak.tur:121-125`).
- **It pushes authors toward inline C.** `weak/upgrade` builds its Option in a
  C body because the Turmeric form is rejected. Until 2026-08-30 that form
  leaked a box per call
  ([now fixed](../archive/inline-c-option-carrier-box-leaks.md)), so the
  language was rejecting the safe spelling and accepting the leaking one.
- **The documented split does not generalise.** `arc.tur` and `env.tur` both
  solve the leak by keeping inline C to a raw predicate and constructing the
  Option in Turmeric; that pattern is unavailable for any owning payload.
  `arc-upgrade` escapes only because `Arc` is a `defopaque` handle rather than
  an `rc`.

## Fix directions

1. **Admit the construction when the monomorph is by-value.** The narrow,
   correct-by-measurement fix: at `elab_call.c:5613`, an argument reaching a
   tyvar parameter of a SUM CONSTRUCTOR whose monomorph lowers by value with a
   typed-pointer payload slot is not erased and needs no carry decision.
   Requires the by-value/erased distinction to be decidable at elaboration,
   which is the part to establish first -- spec selection happens later, so
   this may need the conservative form ("by value for every instantiation",
   true of `rc<A>` since its representation does not depend on `A`).
2. **Give the diagnostic an accurate message either way.** Independent of any
   semantic change, and cheap: it names a collection when none exists, and
   states a representation fact that no longer holds for the monomorph. Even
   if the restriction stands, the text should say the value would cross an
   erased generic boundary that cannot carry the count.
3. **Extend `OwnCarry` to the sum constructors.** `own_carry_for_arg` already
   encodes per-callee ownership policy for `vec-push!` / `map-set!`
   (`OWN_CARRY_RETAIN` / `BORROW` / `REJECT`). `some` / `ok` / `err` could take
   a policy row instead of falling to the default reject -- the mechanism
   exists, it just has no entry for them.

## Related

- [inline-c-option-carrier-box-leaks](../archive/inline-c-option-carrier-box-leaks.md)
  -- the leak this inconsistency used to compound, fixed 2026-08-30.
- `docs/archive/rc-ref-conversion-and-weak-upgrade-leak.md` -- the original
  weak-upgrade leak, of which this is a sibling residue.

## Resolution (2026-08-30) -- construction fixed, extraction deliberately not

**The construction half is fixed** by fix direction 3, which turned out to be
the *correct* framing rather than merely the cheap one. Fix direction 2 landed
alongside it. Direction 1 was investigated and is **not** the right fix -- see
below.

### What the investigation changed about the diagnosis

The report proposed direction 1 (admit the construction when the monomorph is
by-value) as the principled fix, on the evidence that the payload slot is a
typed pointer. Probing what the emitter actually does for an allowed case
sharpened that:

```c
/* (some x) where x : H, a pointer defopaque */
static tur_adt_Option__H some__spec__tur_adt_Option__H_void__(void *);
tur_adt_Option__H __ps_184 = (ctor_Some__H(x));
```

The call **specializes**: the spec takes `void *` and calls
`ctor_Some__H(void *)`. There is no int64 carrier anywhere on that path -- so
the reinterpret the rejection guards is vestigial for a specializing call,
which is stronger than "the slot is a pointer".

But by-value-ness is the wrong PROPERTY to key on, and that is the real
finding. What actually distinguishes a safe crossing from an unsafe one is not
the representation, it is **whether the callee stores the value somewhere with
an independent lifetime**. A collection outlives the call and needs its own
count. A sum constructor wraps the value in a result the CALLER receives and
owns -- one owner before, one owner after.

That property is decidable at elaboration by callee identity, which is exactly
what the existing `OwnCarry` mechanism does. So direction 3's "the mechanism
exists, it just has no entry for them" was the whole fix.

### The change

`own_carry_for_arg` (elab_call.c) gains three rows: `some` / `ok` / `err` at
argument 0 are **`OWN_CARRY_BORROW`** -- "crossing moves the existing reference
and mints nothing", by that enum's own definition.

`RETAIN` would be wrong in the other direction, and the asymmetry is worth
recording: nothing releases an Option's payload when the value goes out of
scope, so a count minted at construction would never come back down. BORROW
keeps exactly one owner, which is the contract `weak/upgrade` and
`weak/unwrap` already document ("the caller owns it and must `rc/drop` it").

### Extraction is still rejected, on purpose

`(unwrap o)` over an `(Option rc<A>)` remains an error, and this is a decision
rather than an oversight. The result crossing has no safe carry:

- **BORROW** moves the reference out -- but an Option is a copyable value that
  can be unwrapped twice, and two moves of one count is a double drop.
- **RETAIN** mints a count per read -- and nothing ever releases the Option's
  own reference, so every read leaks.

Neither is right because the missing piece is drop glue for a by-value sum's
payload, which is [RM1](../upcoming/reclamation-plan.md)'s subject. stdlib's
`weak/unwrap` already works around it with a `match`, and a match is correct
because it binds the payload once.

So the report's framing -- "`(Option rc<A>)` is accepted as a return type but
the same value cannot be constructed; one of the two is wrong" -- is resolved
in favour of accepting construction. The remaining asymmetry (construct yes,
generic-accessor read no) is a real ownership question with a real answer
pending, not an inconsistency.

### The diagnostic (fix direction 2)

The error line is unchanged -- it is accurate for the collection case, which is
where it usually fires, and `tests/fixtures/errors/collection-rejects-owning-element`
pins it. Two notes now follow it saying what the rule actually is (a GENERIC
boundary, not collections as such) and naming the spellings that work (the sum
constructors; `match` for reading out).

### Not done, deliberately

`stdlib/weak.tur` is unchanged. `weak/upgrade` keeps its inline-C form, which
is correct and no longer leaks, and rewriting working stdlib to exercise a
newly-available spelling would be churn. The point of this fix is that the
Turmeric form is now *available* to anyone who wants it.

### Validation

`tests/fixtures/option-rc-payload-turmeric-construction` pins the COUNTS, not
just that it compiles -- a wrong carry is silent, since neither an extra retain
nor a missing one changes the printed `some?`. It asserts the same
count sequence (1 / true / 2 / 1 / false) that `weak-upgrade-after-drop`
asserts through the inline-C form, so both spellings are held to one contract,
and it is `requires.leak-check` because the leak half is what a stdout diff
cannot see.

Full suite 2748 passed / 0 failed; leak-check 61 passed / 0 failed /
0 known-open; option-niche seam 9/0; sr4 seam 24/0.
