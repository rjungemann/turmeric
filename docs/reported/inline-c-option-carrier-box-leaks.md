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
3. ~~**Document the hazard**~~ -- **documentation half DONE 2026-09-02.** The
   guide gained a "Who frees the box" section with the measurements, the
   erased-vs-monomorph free rule, and the bad-free hazard; `CLAUDE.md`'s
   "first-class" paragraph now says who frees it. The *conversion* half is
   struck: per correction 2 above, the split does not remove the allocation at
   either of these two sites, so there is nothing to convert. It leaves the trap
   documented rather than disarmed, which is why this report stays open.

## What actually closes this

Direction 1, or the general carrier campaign reaching these sites. Both stdlib
instances are on the ERASED path -- a generic signature whose payload is a type
variable -- and the narrowing note on
[carrier-sum-option-boxes-have-no-owner](carrier-sum-option-boxes-have-no-owner.md)
says the residue "shrinks further with each site that monomorphizes; end-to-end
monomorphization is where it reaches zero". These two are that residue, reached
through inline C rather than through `(some x)`.

Direction 2 stays worth doing on its own merits -- not as a leak fix (it is not
one, per correction 2) but because the inconsistency is real: `(Option rc<A>)`
is a legal return type and buildable from inline C, while `(some r)` on the same
value is rejected. The mechanism is now located: `own_carry_for_arg` in
`src/compiler/elab_call.c` is an allowlist mapping a callee name + arg index to
an ownership decision (`vec-push!` and `map-assoc-eq-o` are RETAIN,
`tur-vec-homog__` is BORROW), and everything not on it is `OWN_CARRY_REJECT`,
which is what rejects `(some r)`. Adding `some`/`ok`/`err` would be a one-line
change per entry, but the carry has to be BORROW rather than RETAIN -- nothing
releases an Option's payload when the Option dies, so a retain would leak a
count -- and a discarded Option would then leak the moved rc. That is a real
ownership decision, not a mechanical fix, which is why it is not taken here.
