---
title: An untyped closure param fed a niche Vec slot word reads it back as a carrier box
category: Archive
description: A vec-eq? comparator with erased parameters unboxed a raw Vec slot word as a carrier box, answering silently wrong on the default path. RESOLVED 2026-09-04 -- a comparator written inline at the call now carries the same slot-word mark the synthesized one does, and the one shape that cannot carry it is refused with TUR-E0715.
---

# An untyped closure param fed a niche Vec slot word reads it back as a carrier box

**Severity: medium** -- a silent wrong read on the DEFAULT path (since the
Option niche graduated, 2026-09-03), for one shape the checker admits.  The
residue the [container-element-form plan](../upcoming/container-element-form-plan.md)
CE3 recorded; it became a default-path fact with the graduation.

**RESOLVED 2026-09-04.**  Both fix directions were built, each where the other
could not reach: direction 1 (mark by helper contract) for a comparator written
at the call, direction 2 (refuse the ambiguity) for the one shape that provably
cannot be marked.  Every path is now either correct or loud.

## Repro

```turmeric
(load "stdlib/string.tur")
(defn main [] : int
  (let [v (:: (vec-new) (Vec (Option String)))
        w (:: (vec-new) (Vec (Option String)))]
    (vec-push! v (some (string/from-cstr "aa")))
    (vec-push! w (some (string/from-cstr "bb")))
    ;; UNTYPED params, ascribed back to the element inside the body
    (println (if (vec-eq? v w (fn [a b] : bool
                                (eq? (:: a (Option String)) (:: b (Option String)))))
               "eq" "ne"))
    0))
```

`vec-eq?` (stdlib/vec.tur, inline-C) hands the comparator `a->data[i]` --
the raw slot word, which for a niche element is the String pointer itself
(CE2).  Inside the closure `a` is an erased `int64_t`, and
`(:: a (Option String))` is the carrier->niche direction of the bridge
(`emit_carrier_bridge`'s niche row, emit_core.c), which UNBOXES:
`(a ? tur_opt_value_checked(a) : 0)`.  That reads the String's first words as
a tag and a payload.

**The filed repro understated it.**  It pushed `"aa"` into both vecs and noted
that the probe "printed `eq` for the wrong reason".  Pushing DIFFERENT strings,
as above, prints `eq` too: a demonstrably wrong answer, not just a right answer
reached the wrong way.  Equal payloads is the one input under which the broken
path cannot be caught, which is worth remembering when writing the probe for a
representation bug -- it is the same trap as leading a float probe with `7.0`.

## Root cause

The bridge decides word-vs-box by the VALUE, never by the type alone (the
plan's Risks section): a hoisted `vec-get` temp is marked in the slot-word
table, and the synthesized `(eq? v w)` comparator's params carry the
`__cmp_slot_` prefix for the same reason.  A user closure's param has no
mark -- the closure is elaborated on its own, before anything knows which
helper will call it -- and an unmarked int64 word ascribed to a niche Option
is, by the carrier contract, a box.  TUR-E0714 covers the STORE side only
(a niche element stored through a fully erased receiver); nothing on the
read side could see an erased closure param.

## Resolution

The residue split along one line the report did not draw: **whether the
comparator is written AT the call.**

### Written at the call -- marked (fix direction 1)

A lambda in that argument position is minted for that argument and reachable
from nowhere else, so it can be given the same mark the synthesized comparator
carries.  `elab_call.c` renames its parameter bindings into the `__cmp_slot_`
namespace when the callee is `vec-eq?`; `emit_slot_word_is` already recognises
that prefix, so the existing bridge row reinterprets the word instead of
unboxing it.  The emitted body goes from

```c
static bool __fn_1996(int64_t a, int64_t b) {
    int64_t __t81 = (int64_t)(intptr_t)(a);
    ... __inst_Eq_eq_qu_Option__spec__(
          (__t81 ? (void *)(intptr_t)tur_opt_value_checked(__t81) : (void *)0), ...
```

to

```c
static bool __fn_1996(int64_t __cmp_slot_a, int64_t __cmp_slot_b) {
    ... __inst_Eq_eq_qu_Option__spec__(((void *)(intptr_t)(__cmp_slot_a)), ...
```

which is exactly the code the typed-param spelling already produced.

Three things about the mechanism are worth keeping:

- **The mark is the emitted name, deliberately.**  A `Binding` flag plus
  `emit_slot_word_mark(pn)` at the parameter's declaration was the obvious
  alternative and is wrong: that table is program-scoped (`emit_localvar_reset`
  runs per module, not per function), so marking a parameter spelled `a` would
  make every unrelated local named `a` look like a slot word to the same bridge
  row -- trading one silent wrong answer for a family of them.  A name carries
  its own scope.
- **It has to survive mangling.**  `raw_name_for_binding` passes a reserved-
  prefix name through verbatim only when it is already a pure C identifier;
  a parameter spelled `my-arg` would go through the injective mangler, which
  rewrites the prefix itself and silently loses the mark.  Those fall back to
  `__cmp_slot_p<idx>`.
- **Two spellings, two routes.**  A capturing lambda arrives as an
  `EX_CLOSURE` literal; a captureless one has already been lifted to a
  file-scope `__fn_N` and arrives as an `EX_VAR`, which is why the marker keys
  on `is_lifted_lambda` rather than on the expression kind alone.  Without
  that second route the common spelling -- a comparator that captures nothing
  -- would look exactly like a named function.

The mark is unconditional for a lambda in this position, and **accurate rather
than merely inert** for a non-niche element: a `vec-eq?` comparator parameter
is a raw slot word whatever the element type is.  It is consulted by exactly
one bridge row, whose sink form must be CE_WORD, so every other element type
reaches byte-identical code -- borne out by zero snapshot drift across the
suite.

### Named -- refused (fix direction 2, TUR-E0715)

A named function cannot carry the mark.  It is elaborated once, and another
caller may hand it genuine carrier boxes, so marking its parameters would
corrupt that caller instead of fixing this one.  With erased parameters over a
niche element it has no decidable convention, so it is refused, naming the two
fixes (declare the parameter types, or inline the comparator).  This is the
read-side twin of the store side's TUR-E0714, and it fired on **zero** of the
2784 existing fixtures.

The refusal is narrow on purpose.  It requires all three of: the callee is
`vec-eq?`, the comparator's first parameter kind is the erased carrier
(`TY_INT` -- a comparator declared over the element type already bridges at the
closure boundary and is correct), and the element is `adt_app_is_niche_option`.
A named comparator that genuinely wants to compare the words as integers over a
non-niche element is untouched.

## Validation

- `bash tests/run.sh` -- **2784 passed, 0 failed**, zero snapshot drift.
- `tests/fixtures/option-niche-vec-closure-cmp` gained shape 4: the untyped
  comparator, captureless and capturing, over equal and differing payloads.
  The differing-payload lines are the ones that matter -- equal payloads
  answered `eq` even while broken.
- `tests/fixtures/errors/vec-eq-named-erased-comparator` pins TUR-E0715.
- `tests/run-option-niche-seam.sh` 10/0 and `tests/run-sr2-seam.sh` 55/0.
- The OFF path (`TUR_OPTION_NICHE=0`) was checked directly on the repro: an
  Option element is CE_BOX there, so the marked row never fires and the answer
  is unchanged.

## What this does not cover

Only `vec-eq?`.  It is the one stdlib helper that hands a comparator raw slot
words -- every other `*-eq?` (map/option/list/pair/result/set-eq-cmp?) passes a
value that has crossed the carrier boundary, i.e. a box, so an erased parameter
there is correct as it stands.  The predicate keying this off the callee name
must stay in step with `build_comparator_lambda`'s `strcmp(sd->name, "Vec")`:
the two encode the same single fact, that Vec is the slot-word container, and a
second such helper would have to update both.
