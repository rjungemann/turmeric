---
title: The pair-eq? macro applies its comparator directly, so no convention can reach it
category: Reported
description: pair-eq? splices the user's lambda into call position against a field read, so unlike every other *-eq? there is no callee whose parameter contract could say which convention the value arrives in. An erased comparator over a niche Pair element is silently wrong, and the two existing mechanisms both key on a call that does not exist here.
---

# The `pair-eq?` macro applies its comparator directly, so no convention can reach it

**Severity: low-medium** -- a silent wrong answer on the DEFAULT path, for one
spelling of one helper.  Narrower than its two siblings (both resolved
2026-09-04) because the synthesized `Eq [Pair]` path is correct and only the
hand-written macro call is affected.

Filed 2026-09-04 as the last residue of
[typed-comparator-over-hamt-box-compares-box-addresses](../archive/typed-comparator-over-hamt-box-compares-box-addresses.md).

## Repro

```turmeric
(load "stdlib/string.tur")
(load "stdlib/pair.tur")
(defn main [] : int
  (let [x  (:: (some (string/from-cstr "aa")) (Option String))
        y  (:: (some (string/from-cstr "bb")) (Option String))
        p1 (:: (pair x 0) (Pair (Option String) int))
        p2 (:: (pair y 0) (Pair (Option String) int))]
    (println (if (pair-eq? p1 p2
                   (fn [a b] : bool
                     (eq? (:: a (Option String)) (:: b (Option String))))
                   (fn [a b] : bool (= a b)))
               "eq" "ne"))            ;; prints "eq" -- WRONG, "aa" vs "bb"
    0))
```

`(eq? p1 p2)` -- the synthesized path -- is correct, and so is a comparator
whose parameters are declared as the element type.  Only the erased spelling
through the macro is wrong.

## Why the two existing mechanisms cannot reach it

```turmeric
(defmacro pair-eq? [p1 p2 fst-cmp snd-cmp]
  `(if (~fst-cmp (.fst ~p1) (.fst ~p2)) (~snd-cmp (.snd ~p1) (.snd ~p2)) false))
```

The lambda is spliced into CALL position and applied directly to a field read.
Both mechanisms that resolve this convention elsewhere key on *a call to a
named helper with a comparator in a known argument slot*:

- `call_comparator_gets_slot_words` (elab_call.c) marks the parameters of a
  comparator passed to `vec-eq?` / `list-eq?` / `result-eq?`.
- `call_comparator_gets_carrier_box` does the mirror for `map-eq?` /
  `set-eq-cmp?` / `map-eq-raw-k?`.

Here there is no such callee.  `.fst` yields the stored word, the erased
parameter's ascription unboxes it as a box that is not there, and nothing in
between can say otherwise.

Note the legacy `pair-eq-carrier?` helper (the inline-C one behind
`Eq [Pair]`) WOULD fit the existing mechanism -- it takes its two comparators
at argument indices 2 and 3 and passes `a->fst` / `a->snd`.  Adding it costs
one line.  It was deliberately not added, because it does not fix the macro
and would imply coverage this does not have.

## Fix directions

1. **Answer the question at the parameter, not at the call.**  The general
   form of this whole family is "an erased lambda parameter receives a value
   whose convention its own signature does not state".  Deciding that where
   the parameter is materialized would subsume all three mechanisms
   (`__cmp_slot_`, `arrives_as_carrier_box`, and this) instead of adding a
   fourth. Largest, and the only one that closes the shape rather than the
   instance.
2. **Route the macro through `pair-eq-carrier?`** so there IS a callee to hang
   the contract on.  Small, and it makes `pair-eq?` consistent with its six
   siblings -- but it changes what the macro expands to, so the by-value Path A
   advantage the direct field reads currently have would need measuring first.
3. **Diagnose it.**  A `(.fst p)` whose field type is a niche option, feeding
   an erased lambda parameter, is the same undecidable shape TUR-E0715 refuses
   at `vec-eq?`.  Cheapest, and it converts a silent wrong answer into a
   one-line fix at the site, but it rejects a spelling that reads perfectly
   natural.

## Guides to update when fixed

- `stdlib/pair.tur` -- the `pair-eq?` docstring, whose example uses exactly the
  erased spelling that breaks here.
