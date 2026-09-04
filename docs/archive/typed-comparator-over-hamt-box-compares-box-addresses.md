---
title: Comparator conventions over niche elements are still inconsistent in three places
category: Archive
description: The *-eq? family splits on whether a helper passes its comparator the stored word or a HAMT box. RESOLVED 2026-09-04 -- the box half's typed comparator now arrives as the carrier and unboxes at body entry, and fn types print their full parameter types instead of collapsing to bare kinds. One residue, the pair-eq? macro, is refiled on its own.
---

# Comparator conventions over niche elements are still inconsistent in three places

**Severity: medium** -- silent wrong answers on the DEFAULT path in two of the
three, for spellings the docs recommend.

**RESOLVED 2026-09-04** for items 1 and 3; item 2 (the `pair-eq?` macro) is
refiled on its own as
[pair-eq-macro-applies-comparator-directly](../reported/pair-eq-macro-applies-comparator-directly.md),
because it is a different shape rather than a smaller piece of the same one.
Resolution notes are at the bottom.  Found 2026-09-04 while verifying a
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


## Resolution (2026-09-04)

### 1. The typed comparator over Map/Set -- FIXED

The mirror image needed a mirrored mechanism, not a new idea.  The word half
marks an ERASED parameter so its ascription reinterprets; the box half marks a
parameter DECLARED as a niche option so it arrives as the carrier and unboxes
at body entry.  `Binding.arrives_as_carrier_box` is set at the call that
establishes the convention (`call_comparator_gets_carrier_box`: `map-eq?` and
`set-eq-cmp?` at argument 2, `map-eq-raw-k?` at 3), and emit declares such a
parameter `int64_t __tur_nbox_<p>` and materializes it at entry:

```c
static bool __fn_2009(int64_t __tur_nbox_p, int64_t __tur_nbox_q) {
    void * p = (__tur_nbox_p ? (void *)(intptr_t)tur_opt_value_checked(__tur_nbox_p) : (void *)0);
    ...
```

That is deliberately the same shape as the B4 wide-by-value box load two loops
above it, and it borrows the same way: the box belongs to the container that
stored it, so nothing is freed here.  The read is the CHECKED one for the same
reason the carrier->niche bridge uses it -- a box can hold `Some(NULL)`, and
the unchecked value would silently turn a value `some?` calls true into
`(none)`.

Two things this cost that the report did not anticipate:

- **The forward declaration has to move with the definition.**  The first
  build failed at `cc` with the prototype still spelling `void *` -- B4 keeps
  its two sides in step through a TYPE-level predicate
  (`type_is_b4box_closure_slot`), which a per-binding mark cannot reuse, so
  `emit_fn_forward_decls` needed the branch explicitly.
- **Only a lambda written at the call can be marked**, the same limit the word
  half has and for the same reason: a named comparator is elaborated once and
  may be called elsewhere with a value that really is the niche word.  Unlike
  the word half there is no diagnostic for the named case here, because
  TUR-E0715 needs an element type from a `(Vec A)` receiver and `map-eq?`
  declares `m1 : int`.

### 3. `option-eq?`'s same-type-twice diagnostic -- FIXED

Not a convention problem at all, and wider than `option-eq?`: `type_name_buf`'s
`TY_FN` case rendered each parameter as `type_from_kind(arg_kinds[i])`,
discarding `arg_full_types`.  That collapses every composite to its constructor
and a type variable to nothing, which is how two genuinely different types
printed identically.  It now prints the full parameter and result types when
recorded:

```
expected (fn [tyvar 'A' tyvar 'A'] : bool), got (fn [int int] : bool)
```

The rejection itself is correct -- `A` binds to `(Option String)` from the
receiver and the lambda declares `int` -- and the typed spelling works.  Only
the message was unactionable.  This improves every fn-type diagnostic in the
compiler, and no fixture depended on the collapsed form (2786 passed, zero
`expected.diag` changes).

### Validation

`bash tests/run.sh` 2786 passed / 0 failed, zero snapshot drift; option-niche
seam 10/0, sr2 55/0, sr4 24/0, leak-check 79/0.
`tests/fixtures/niche-elem-comparator-conventions` now carries both halves of
the family -- five Map and five Set assertions alongside the word-passing ones,
each group ending with the same-pointer case that makes the verdict a fact
rather than a judgement about intended semantics.
