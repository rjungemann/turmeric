---
title: RM2's spine residue, categorized by where a boundary could go (2026-09-05)
category: Artifact
description: The ~900 B of per-node spine boxes split into "no bracket exists but the natural one would work", "the spine IS the result", and "escapes into a returned structure". Answers whether a with-region surface form is the whole answer or half of it -- and turned up two defects that make the question moot until they are fixed.
---

# RM2's spine residue, categorized (2026-09-05)

[reclamation-plan.md](../upcoming/reclamation-plan.md)'s RM2 section records its
gate's evidence and its blocker, but not the one number a decision needs: of the
spine bytes still leaking, **how many sit inside a scope a region could bracket,
and how many do not.** That split decides whether RM3's missing surface form is
the whole answer or half of it. This is that split, measured.

Method: full ASan leak stacks (`malloc_context_size=30`,
`fast_unwind_on_malloc=0`) for every spine-bearing fixture in the RM1 sweep, read
frame by frame against the source to find the activation each allocation dies
in, then that activation's result type checked against
`region_type_reaches_node` (emit_expr.c) -- the static lock a bracket has to
pass to rewind rather than retire.

## The split

| category | bytes | where |
|---|---:|---|
| **1. No bracket exists, and the natural one would work** | **496** | `constrained-defn-cons-return-monomorphize` 432, `refined-nonempty` 64 |
| **2. A bracket exists and is the right one -- but is silently a no-op** | **312** | `re-string`: `re-find-from` / `re-step-class` cells |
| **3. Escapes into a returned structure; no boundary contains it** | **96** | `re-string`: `re-parse-class-items` 64, `demo`'s cons list 32 |

Total 904 B. (The plan's "~990" also counted `re-string`'s strings, which are
String ownership, not spine.)

### Category 1 -- 496 B, workable today

Both fixtures build spines that are pure intermediates of a statement whose own
value is a scalar.

`constrained-defn-cons-return-monomorphize` is the clean case: every one of its
eight allocations is a `(Cons A)` built by `rec`/`wrap` and consumed immediately
by `list-length` or `thead` --

```turmeric
(println (list-length (:: (rec 0 3) (Cons int))))
```

-- so the enclosing statement's value is an `int` and the spine is dead at the
semicolon. `refined-nonempty` is the same shape rooted one level up: its four
cells are `main`-scoped and live to exit.

A bracket around the statement (or around `main`'s body) returns a scalar, which
`region_type_reaches_node` accepts, and `region-scope-value-survives` already
proves that exact shape rewinds. **Nothing is missing here but the spelling** --
these programs have no reason to write `bt-scope`, which is a backtracking
primitive. This is precisely the `with-region` surface form RM3's R5 named as
its graduation blocker.

### Category 2 -- 312 B, and the bracket is a no-op (partly fixed 2026-09-05)

`stdlib/re.tur`'s `re-find-from` is the textbook region and it does not work.

```
re-step-class -> re-run-k -> re-find-from -> re-replace-go -> re/replace-all -> demo
```

The `RxPos` cells are built by `re-step-*`, consumed by `re-pos-max` inside
`re-find-from`'s own `let`, and dead when it returns. Its result is
`RxPair` = `(RxIP :int :int)` -- a non-heap ADT of two `:int` fields, which the
static walk ACCEPTS.

So a bracket belongs there, the walk would let it rewind, and putting one there
today reclaims nothing: a `bt-scope` whose result is a by-value record emitted
**zero** `tur_region_push()` calls. It compiled, ran, printed the right answer,
and silently did not open a region.

**Half of that is fixed** -- the bracket is emitted now
([archived](../archive/region-bracket-lost-when-bt-scope-specializes.md)) -- and
the other half is that the walk does NOT in fact accept `(RxIP :int :int)`, as
this section claimed. It refuses at field 0, because a plain `:int` field
records no `full_type`. See step 1 of the order of work below and
[region-walk-refuses-every-adt-result](../reported/region-walk-refuses-every-adt-result.md).

### Category 3 -- 96 B, genuinely RM2's

`re-re-parse-class-items` builds `RxCls` cells that go into `(RClass neg acc)`
and out as part of the compiled `Regex`. They escape by construction, nothing
frees a `Regex`, and no boundary contains them. This is the residue the plan
describes as "spines that escape their region", and it is 11% of the total.

## The answer to the question

**The surface form is about 90% of the answer, not half** -- 808 of 904 bytes are
in a scope a region could bracket. But two defects have to be fixed first, and
one of them is not about regions at all.

Categorizing this turned up a **silent wrong answer on default flags** (since
fixed): a polymorphic function instantiated at two result types with different
representations, called from a CPS-lowered caller, was emitted against the wrong
specialization. Through `bt-scope`, which is stdlib:

```turmeric
(defn s-int [n : int] : int   (bt-scope (fn [] (+ n 20))))    ;; prints a pointer
(defn s-rec [n : int] : PPair (bt-scope (fn [] (PIP n 20))))  ;; prints 28
```

The neighbouring shape (`float` + record) is a hard C build failure instead. Both
faces were the CPS emitter's call arms not selecting by result type; fixed and
archived as
[cps-call-arm-ignores-abi-specialization](../archive/cps-call-arm-ignores-abi-specialization.md).
The region bracket going missing is the same arm losing a different thing, and
is a separate fix -- see step 1 below.

**Why none of it was red:** every `bt-scope` site in the tree returns
`int`, `bool` or `void`. All are carrier-transparent, so the erased base and
every specialization agree and the mis-selection is invisible. The three
`region-scope-*` fixtures are all scalar-or-void results too -- the blind spot is
exactly one fixture wide.

## Order of work, if RM2 is taken up

1. ~~**Fix the CPS specialization arm.**~~ **Done 2026-09-05** -- the call's own
   result type is now a discriminator in `find_mono_clone_for_call`, and both
   faces are pinned by a fixture each. The region bracket did NOT ride along, as
   this step predicted it would; it was
   [fixed separately](../archive/region-bracket-lost-when-bt-scope-specializes.md)
   the same day by routing a region boundary to `cps->direct`, the only arm the
   bracket can live on.

   **Category 2 is still not reclaimed, and the blocker moved rather than
   closing.** `re-find-from` gets a bracket now, and that bracket RETIRES rather
   than rewinding, because the static walk refuses on a ctor field with no
   recorded `full_type` -- which is every ordinary `:int`, not just the spine its
   comment describes. Deciding by the field's kind is unsound and was measured
   so (a genuine `:int` and a carrier-erased ADT field both report `TY_INT`;
   accepting on kind reclaimed a live mutually-recursive spine and printed 0
   instead of 42). Now
   [region-walk-refuses-every-adt-result](../reported/region-walk-refuses-every-adt-result.md),
   with the source of truth named: `c->field_forms[fi]` is populated for plain
   defdata, so this is a layering question, not a missing fact.
2. **Add a `with-region` bracket.** Category 1's 496 B is unreachable without it
   and needs nothing else; category 2's 312 B needs it plus the region-bracket fix in step 1's follow-up.
3. **Then re-measure.** What is left should be category 3 and whatever step 2
   turns up -- a far smaller set than the phase as scoped, and one where per-node
   ownership may be inferable, since the escaping cases are the ones a promotion
   walk has to identify anyway.

Refcounting (option A in the plan's assessment) is still the wrong tool for
category 3 and nothing here changes that pricing.
