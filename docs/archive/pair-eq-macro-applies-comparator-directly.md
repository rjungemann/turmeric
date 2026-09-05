---
title: The pair-eq? macro applies its comparator directly, so no convention can reach it
category: Archive
description: pair-eq? splices the user's lambda into call position against a field read, so no callee contract could reach it. RESOLVED 2026-09-04 -- a direct application needs no contract, because the argument is at the call site; the convention is read off the value being passed.
---

# The `pair-eq?` macro applies its comparator directly, so no convention can reach it

**Severity: low-medium** -- a silent wrong answer on the DEFAULT path, for one
spelling of one helper.  Narrower than its two siblings (both resolved
2026-09-04) because the synthesized `Eq [Pair]` path is correct and only the
hand-written macro call is affected.

**RESOLVED 2026-09-04**, the same day, by none of the three filed directions
-- see the Resolution below.  Filed as the last residue of
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


## Resolution (2026-09-04)

None of the three filed directions, because all three took the report's own
framing at face value: that the missing thing was a *contract*.  It was not.

A direct application does not need a callee to promise a convention, because
**the argument is right there at the call**.  This is the one site in the
family where the convention can be read off the value being passed rather than
promised by a callee -- and a concrete niche-typed argument IS the payload
word, since that is what the representation means.  So
`mark_direct_apply_niche_word_params` (elab_call.c, hooked into
`elab_call_head_expr` just after the call is built) marks each ERASED parameter
whose corresponding argument is a concrete niche option, reusing the same
`__cmp_slot_` rename the two call-keyed rules use.

Per PARAMETER, not per lambda -- `pair-eq?`'s own two comparators are the
reason: the first takes the niche component and the second an `int`, and
marking the second would be wrong.  The fixture's third pair-line varies only
the int component, so it pins that the second comparator still sees an ordinary
integer.

The report's direction 1 ("answer at the parameter, not the call") was the
right instinct pointed one step too far: the answer is at the *argument*, which
is cheaper than a general parameter-level analysis and decidable here in a way
it is not for a name.  Directions 2 and 3 are moot -- the macro is untouched
and nothing is refused.

A named comparator is still out of reach, for the unchanged reason: it is
elaborated once and may be applied elsewhere to a value that really is a box.

### Found on the way, filed separately

`(eq? p1 p2)` -- the SYNTHESIZED path over a `(Pair (Option String) int)` --
segfaults, and has nothing to do with this.  The `Eq[Pair]` dispatch mints an
`Eq[Option]` specialization with mismatched parameter representations
(`__inst_Eq_eq_qu_Option__spec__bool_void___int64_t`, one `void *` and one
`int64_t`), so `Eq[String]` receives an integer where it wants a pointer.
Verified pre-existing against a compiler built from `581902f6` in a separate
worktree, and unchanged by removing either of this series' marks:
[eq-on-pair-of-niche-option-segfaults](../reported/eq-on-pair-of-niche-option-segfaults.md).

### Validation

`bash tests/run.sh` 2786 passed / 0 failed, zero snapshot drift.
`tests/fixtures/niche-elem-comparator-conventions` group 5 carries the macro:
three erased-comparator lines (equal, different payload, different int) and two
typed ones.
