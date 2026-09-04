---
title: Comparator conventions over niche elements are still inconsistent in three places
category: Reported
description: The *-eq? family splits on whether a helper passes its comparator the stored word or a HAMT box. The word half is fixed; the box half (map-eq?, set-eq-cmp?) breaks the TYPED spelling, the pair-eq? macro applies its comparator directly so nothing can mark it, and option-eq? rejects a hand-written lambda with a diagnostic that prints the same type twice.
---

# Comparator conventions over niche elements are still inconsistent in three places

**Severity: medium** -- silent wrong answers on the DEFAULT path in two of the
three, for spellings the docs recommend.  Found 2026-09-04 while verifying a
scope claim in
[erased-closure-param-over-niche-vec-slot-reads-box](../archive/erased-closure-param-over-niche-vec-slot-reads-box.md).
That claim -- "`vec-eq?` is the one helper that hands over raw words, every
other `*-eq?` passes a box" -- was **wrong**, and this is the part of what it
missed that is not yet fixed.

## The split, measured

A niche `(Option P)` is carried as its payload pointer.  The family divides on
whether a helper walks its own payload slots or iterates a HAMT:

| container | helper | hands the comparator | |
|---|---|---|---|
| Vec | `vec-eq?` | `a->data[i]` | word |
| Cons | `list-eq?` | `(list-head l)` | word |
| Option | `option-eq?` | the `(Some v)` binder | word |
| Result | `result-eq?` | the sum's payload slot | word |
| Pair | `pair-eq?` | `a->fst` / `a->snd` | word |
| Map | `map-eq?` | the HAMT value | **box** |
| Set | `set-eq-cmp?` | the HAMT key | **box** |

Fixed 2026-09-04 (see the archived report): the synthesized `(eq? a b)`
comparator now marks its parameters for every word-passing container rather
than for `Vec` alone, and a user comparator written inline at `vec-eq?`,
`list-eq?` or `result-eq?` takes the same mark.  Pinned by
`tests/fixtures/niche-elem-comparator-conventions`.

Three things are left.

## 1. A TYPED comparator over Map/Set compares box addresses

The strongest form: the **same** `String` pointer on both sides.  No
convention can make a value unequal to itself.

```turmeric
(load "stdlib/string.tur")
(load "stdlib/map.tur")
(defn main [] : int
  (let [x  (:: (some (string/from-cstr "aa")) (Option String))
        m1 (:: (map-assoc (map-new) 1 x) (Map int (Option String)))
        m2 (:: (map-assoc (map-new) 1 x) (Map int (Option String)))]
    (println (if (map-eq? m1 m2
                   (fn [p q] : bool
                     (eq? (:: p (Option String)) (:: q (Option String)))))
               "untyped-eq" "untyped-ne"))          ;; untyped-eq  (correct)
    (println (if (map-eq? m1 m2
                   (fn [p : (Option String) q : (Option String)] : bool
                     (eq? p q)))
               "typed-eq" "typed-ne"))              ;; typed-ne    (WRONG)
    0))
```

`set-eq-cmp?` is identical -- substitute `(set-add-eq-o (set-new) 1 x 0 0)`
and the same two lines print `untyped-eq` / `typed-ne`.

The comparator receives a box, so an erased parameter's `(:: p (Option
String))` unboxes and is right.  A parameter DECLARED `(Option String)` takes
the closure-boundary bridge instead, which reinterprets the word as the niche
pointer without unboxing: it compares the two boxes' addresses, and two
distinct boxes never match even around one payload.

**This is the mirror image of the defect just fixed, and it matters more than
its severity suggests:** the advice the tree now ships for the word-passing
helpers -- "declare the parameter types" -- is exactly the spelling that
breaks here, and TUR-E0715 actively pushes users toward it.  Whatever else
happens, the two families must stop recommending opposite things.

Root cause is a different site from the fixed one.  There the parameter was
erased and had no mark; the fix was to give it one.  Here the parameter is
fully typed and the CLOSURE BOUNDARY mis-bridges: a `(Option String)`
parameter receiving an int64 carrier argument should take the carrier->niche
crossing (unbox, hand over the payload) and instead reinterprets.  The
comparator is invoked through the helper's inline-C
`((bool(*)(void*, int64_t, int64_t))...)` cast, so nothing C-side can tell the
conventions apart -- the decision has to be made where the closure's
parameters are materialized.

That the HAMT stores a box where a Vec slot stores the word is CE4
(`docs/upcoming/container-element-form-plan.md`, "decide Map/Set/HAMT by
evidence, not momentum"), still deferred.  This report is about the read side
being inconsistent with itself either way and does not depend on how CE4 lands.

## 2. `pair-eq?` is a macro, so there is no call to hang a contract on

```turmeric
(pair-eq? p1 p2 (fn [a b] : bool (eq? (:: a (Option String)) (:: b (Option String))))
                (fn [a b] : bool (= a b)))
```

over a `(Pair (Option String) int)` holding `"aa"` and `"bb"` answers
`untyped-eq`.  Wrong, and for a reason the mark cannot reach:

```turmeric
(defmacro pair-eq? [p1 p2 fst-cmp snd-cmp]
  `(if (~fst-cmp (.fst ~p1) (.fst ~p2)) (~snd-cmp (.snd ~p1) (.snd ~p2)) false))
```

The lambda is spliced into CALL position and applied directly to a field read.
There is no callee whose parameter contract could say "this one gets words",
which is what both existing mechanisms key on.  The synthesized `Eq [Pair]`
path IS fixed (it goes through `build_comparator_lambda`), and the legacy
`pair-eq-carrier?` helper would fit the existing mechanism; the macro itself
is the gap.

Generalizing properly means answering "an erased lambda parameter receives a
niche-word value" at the parameter, not at the call -- which is the general
form of this whole family and a bigger change than either fix so far.

## 3. `option-eq?` rejects a hand-written comparator, printing one type twice

```
error [TUR-E0001]: function 'option-eq?' arg 3:
  expected (fn [int int] : bool), got (fn [int int] : bool)
```

A user simply cannot call `option-eq?` with a lambda; the synthesized path is
the only one that works.  Whatever the real mismatch is, it is invisible in
the two rendered types, so the message cannot be acted on.  Probably a
distinct defect from the convention question -- `option-eq?`'s comparator is
declared `(fn [A A] bool)` against a lambda elaborated at the carrier -- but
it is what stopped this shape from being probed at all, so it is filed here
rather than lost.

## What is verified

All seven helpers probed with both parameter spellings against three inputs:
identical pointer, equal text in distinct allocations, and different text.
The identical-pointer case is what makes the Map/Set verdict unambiguous
rather than a judgement about intended semantics; the different-text case is
what catches a comparator that is not comparing at all (the broken paths
answered "eq" for every input, so an equal-only probe reports success).

## Guides to update when fixed

- `stdlib/vec.tur` -- the `vec-eq?` docstring's "declare the parameter types"
  advice needs its Map/Set counter-note (plus `stdlib/docstrings.tur` regen).
- `src/compiler/diag.c` -- the TUR-E0715 explanation, same reason.
- `docs/upcoming/container-element-form-plan.md` -- CE3 is recorded closed for
  the word-passing half; this is the rest of the same question.
