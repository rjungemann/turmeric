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
