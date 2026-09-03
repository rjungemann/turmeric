# An Option built inside inline C allocates a box the elaborator never releases

**Severity:** medium. The leaking pattern is the one the docs recommend, so
every user who follows the inline-C results guide leaks a box per call.

**Status:** OPEN, and its central claim is CORRECTED below (2026-09-02). The
hazard is documented now -- fix direction 3's documentation half -- but the
underlying allocation is untouched, so this stays open for direction 1.

**Corrected 2026-09-02 by measurement.** Two things this report asserts turned
out to be wrong, and both change what the right advice is:

1. **The box IS ownable.** "no elaborated expression corresponds to it, so
   nothing can be given ownership of it and nothing frees it" is half right --
   nothing frees it *automatically*, but `option-free` / `result-free` on the
   returned carrier frees it correctly. Measured on `result-bimap`: freeing
   nothing leaks 32 bytes (the input box and the returned one), freeing the
   input alone leaves 16, freeing both is **clean**. So this is the general
   [carrier-sum-option-boxes-have-no-owner](carrier-sum-option-boxes-have-no-owner.md)
   situation reached through inline C, not a distinct unownability.

2. **The split does not fix an erased-path site.** Fix direction 3 says "convert
   both stdlib sites to the split", but a Turmeric `(ok x)` in a GENERIC
   function allocates the same box the builder does. The split works for
   `arc-upgrade` because `Arc` is a `defopaque` and the Option monomorphizes
   BY VALUE -- no allocation at all. It removes an allocation only where the
   result is concrete, which is not the case for `result-bimap` or
   `weak/upgrade`.

**And one hazard neither this report nor the guide had.** `result-free` /
`option-free` are correct ONLY on the erased carrier. On a concrete monomorph
the value flows by value, and freeing it is a bad free:

```
ERROR: AddressSanitizer: attempting free on address which was not malloc()-ed
```

So "free what the builder returned" is not safe advice on its own; it needs the
erased-vs-monomorph rule, which
[docs/guides/inline-c-results-guide.md](../guides/inline-c-results-guide.md)
("Who frees the box") and `CLAUDE.md` now carry.

Found by the widened `requires.leak-check` gate. It is the residue of
[rc-ref-conversion-and-weak-upgrade-leak](../archive/rc-ref-conversion-and-weak-upgrade-leak.md)
bug 2 -- fixing the `(upgrade w)` *builtin* left `stdlib/weak.tur`'s
`weak/upgrade` *function* still leaking, and that turned out to be a different
bug with a wider blast radius.

## The pattern

```turmeric
(defn weak/upgrade [A] [^borrow w : weak<A>] : (Option rc<A>)
  ```c
  RcControlBlock *cb = rc_upgrade(w);
  return cb ? tur_some_ptr(cb) : tur_none();
  ```)
```

```
SUMMARY: AddressSanitizer: 16 byte(s) leaked in 1 allocation(s).
    #1 tur_box_some
    #2 tur_some_ptr
    #3 weak_slupgrade
```

`tur_some_ptr` -> `tur_box_some` mallocs the Option carrier. Because the
allocation happens inside an inline-C body, no elaborated expression
corresponds to it, so nothing can be given ownership of it and nothing frees
it.

## This is already known -- in a code comment, not a report

`stdlib/arc.tur:254` documents it exactly, having hit it during development:

> The Option is built in ordinary Turmeric, not with `tur_some_ptr` in the
> inline-C body. A `some` constructed inside inline C allocates a box the
> elaborator never sees and therefore never releases -- LeakSanitizer caught
> exactly that (16 bytes per successful upgrade) on the first draft.

Its fix is a split: keep the inline C down to a raw predicate, construct the
Option in Turmeric.

```turmeric
(defn arc-try-upgrade [^borrow w : ArcWeak] : bool ...)      ; inline C
(defn arc-weak->arc   [^borrow w : ArcWeak] : Arc  ...)      ; inline C
(defn arc-upgrade [^borrow w : ArcWeak] : (Option Arc)
  (if (arc-try-upgrade w) (some (arc-weak->arc w)) (none)))  ; Turmeric
```

`stdlib/env.tur` uses the same split for `env/get` over `env/get-raw`.

## Why the known fix does not transfer to `weak/upgrade`

Applying the same split verbatim fails:

```
stdlib/weak.tur:121:34: error: cannot store an owning value (rc) in a
collection: elements go through an int64 carrier that cannot hold a reference
the collection would have to own.
```

`arc-upgrade` escapes this because `Arc` is a `defopaque` handle; `weak/upgrade`
returns `(Option rc<A>)`, and `rc` is an owning builtin that an Option element
slot rejects.

**That is its own inconsistency, and probably the more interesting half of this
report:** `(Option rc<A>)` is accepted as a *return type* and can be built from
inline C, but the same value cannot be constructed in Turmeric. One of the two
is wrong.

## The documentation recommends the leaking form

[docs/guides/inline-c-results-guide.md](../guides/inline-c-results-guide.md)
presents `tur_some_ptr` / `tur_ok_ptr` / `tur_box_*` as the way for an inline-C
body to return `option`/`result`, and `CLAUDE.md` calls it first-class:

> Returning `option<T>` / `result<T,E>` from an **inline-C** body is
> first-class -- there is no "inline-C can't easily build a result, so I'll
> return `:int`" escape hatch.

That guidance is what produces the leak. Whatever the fix, the guide and
`CLAUDE.md` need a paragraph saying the box has no owner and pointing at the
split, or the underlying issue needs fixing so the advice becomes true.

## In-tree instances

Only two, both in stdlib -- but the exposure is the documented idiom, not the
call count:

| site | leaks |
|---|---|
| `stdlib/weak.tur:100` (`weak/upgrade`) | yes -- `tests/fixtures/weak-upgrade-after-drop`, marked `known-leak` |
| `stdlib/result.tur:262` (`tur_box_ok` in `result-bimap`) | **yes, measured 2026-09-02** -- 16 bytes per call; clean once the caller `result-free`s the returned carrier. Note the function is named `result-bimap`, not `result/bimap`, and its answers are correct: its hand-rolled `struct { int tag; int64_t payload; }` still matches the post-SR2b layout |

## Fix directions

1. **Make the elaborator own an inline-C-returned carrier** when the declared
   return type is `option`/`result` and the body is inline C. Fixes the class
   and makes the documented advice true. Needs a way to know the callee boxed
   rather than returned a by-value monomorph.
2. **Lift the restriction that blocks the split**, so `(some rc)` is
   expressible and `weak/upgrade` can use arc's pattern. Narrower, and it
   resolves the return-type/construction inconsistency above.
3. **Document the hazard** and convert both stdlib sites to the split. Cheapest,
   but leaves the trap armed for users.

## Resolution (2026-08-30) -- fix direction 1, the class closed

**Fixed**, and by the direction the report ranked first: the compiler now owns
an inline-C-returned carrier. The documented advice is true rather than
annotated, so directions 2 and 3 were not needed.

**"Needs a way to know the callee boxed rather than returned a by-value
monomorph"** was the open question, and the answer was already in the tree:
ownership is recorded ONCE where the fact is known and consumed at ONE place.

- **Mark** (`emit_value`, emit_expr.c): a call-result temp is marked owning when
  the callee's body is inline C, its DECLARED return type is an Option/Result
  application, and the temp is spelled `int64_t` -- that last condition being
  what says the callee handed back the BOX rather than a by-value monomorph,
  which allocates nothing.
- **Consume** (`emit_carrier_bridge`, emit_core.c): the carrier->concrete
  readback already copied the box's contents into an aggregate and abandoned
  it. It now materializes the copy into a temp and frees the box. Both arms:
  the Option branch (null-guarded, because `tur_none()` is the null carrier and
  allocates nothing) and the Result branch (no guard -- both variants carry a
  payload, which is also why SR3's niche is Option-only, and `result/bimap`'s
  `tur_box_ok` is the site this branch covers).

Routing it through the bridge is why every consumer position gets it at once --
let binding, call argument, match scrutinee, ctor argument, container element --
without each needing its own rule. Marks are cleared when consumed, so a temp
bridged twice cannot double-free.

**The declared-vs-resolved distinction is the whole safety argument, and it
cost a double free to find.** The first draft keyed on the call's RESOLVED
type. But `vec-get [A] (v : (Vec A)) : A` is ALSO an inline-C function, and
`(:: (vec-get v 0) (Option int))` resolves to an Option app -- while the box
belongs to the VECTOR, which frees it in `vec-free`.
`tests/fixtures/vec-app-element-box-lifecycle` aborted with "double free
detected in tcache 2", which is exactly the fixture that exists to catch this
("a double-free or a use-after-free here aborts instead of printing"). Keying
on the callee's `result_full_type` fixes it: a declared `: A` is borrow-shaped,
a declared `: (Option T)` is the minting shape.

**The contract is now documented** rather than implied, in
[inline-c-results-guide.md](../guides/inline-c-results-guide.md) ("Who owns the
box") and in `CLAUDE.md`: return a fresh box, and give a borrowed box a
borrow-shaped signature.

**The two open sub-questions are answered by being made moot, not by being
fixed:**

- The `(Option rc<A>)` return-vs-construction inconsistency (direction 2)
  **still stands** -- `(some rc)` is still rejected while the inline-C form is
  accepted. It no longer blocks anything, because `weak/upgrade` keeps its
  inline-C form and no longer leaks. Worth its own report if anyone wants the
  split available; the rejection's stated rationale ("elements go through an
  int64 carrier") is measurably stale for a by-value monomorph, whose `Some`
  arm is a typed `RcControlBlock *`.
- Converting the two stdlib sites (direction 3) is unnecessary; both are
  correct as written.

**Validation.** `tests/fixtures/weak-upgrade-after-drop` was the `known-leak`
fixture and is now clean, so its marker is deleted -- and the harness enforces
that, failing a `known-leak` fixture that runs clean, which is how the fix was
confirmed before the marker came off. Leak-check 60 passed / 0 failed /
**0 known-open** (was 59/0/1). Full suite 2747/0 after regenerating three
snapshots that gained the free; option-niche seam 9/0; sr4 seam 24/0.
