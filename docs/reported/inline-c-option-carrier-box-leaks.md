# An Option built inside inline C allocates a box the elaborator never releases

**Severity:** medium. The leaking pattern is the one the docs recommend, so
every user who follows the inline-C results guide leaks a box per call.

**Status:** OPEN. Known and worked around in one place already; the obvious
workaround does not transfer to the other.

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
| `stdlib/result.tur:262` (`tur_box_ok` in `result/bimap`) | not yet probed |

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
