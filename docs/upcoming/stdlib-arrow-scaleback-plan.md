---
title: stdlib/arrow Scale-back Plan
category: Planning
description: Scale stdlib/arrow.tur back to a minimal pragmatic core. Remove the disabled typeclass declarations (Arrow, ArrowZero, ArrowPlus, ArrowChoice, ArrowLoop, ArrowApply) because their instances are intentionally omitted with a known codegen bug and nothing dispatches against them. Keep the bare-function combinators (arr, >>>, arrow-first, arrow-second, par-comp, arrow-split, arrow-const, arrow-dup) that are already proven by tests/fixtures/stdlib-arrow and tests/fixtures/arrow-capturing-closure.
---

# `stdlib/arrow` Scale-back -- Plan

> **Superseded by the typeclass reintroduction (2026-06-05).** The future-work
> block at the end of this plan ("reintroducing the typeclass layer later") has
> landed: the Arrow hierarchy is back as a **parallel, dispatch-driven surface**
> in `stdlib/arrow-class.tur` (a separate module, since a typeclass method and
> the bare free `arr` / `>>>` cannot share a name in one file). The
> bare-function core in `stdlib/arrow.tur` is unchanged and stays the simple
> default. See
> [stdlib-arrow-typeclass-reintroduction-plan](stdlib-arrow-typeclass-reintroduction-plan.md)
> for the delivered classes, instances, fixtures, and the A3 operator-mangling
> completion that lets `>>>` / `<<<` coexist.

> **Codegen prerequisite has landed (2026-06-03).** The closure-returning
> instance-method codegen bug referenced below (dict field types resolving to a
> non-`int64_t` carrier) is **fixed** -- see
> [closure-returning-instance-method-codegen-plan](closure-returning-instance-method-codegen-plan.md).
> Closure-returning `definstance` methods now carry the `int64_t` fat-closure
> handle at all three codegen sites, and a throwaway `Arrow`-style `arr`
> instance compiles and runs. Reintroducing the real Arrow typeclass hierarchy
> is unblocked.

## Why

`stdlib/arrow.tur` currently mixes two layers:

1. A **typeclass hierarchy** -- `Arrow`, `ArrowZero`, `ArrowPlus`,
   `ArrowChoice`, `ArrowLoop`, `ArrowApply` (lines 31-77) -- with **no
   instances** for `(->)` or anything else. The file's own comment
   (lines 91-100) explains why: closure-returning instance methods tripped
   a codegen bug (dict field types resolving to `void *` instead of
   `int64_t`) and several stub bodies depended on language features that
   do not exist yet (`Either`/`Left`/`Right`, full Tuple feedback).
2. A **set of plain functions** -- `arr`, `>>>`, `arrow-first`,
   `arrow-second`, `par-comp`, `arrow-split`, `arrow-const`, `arrow-dup`
   -- plus the inline-C heap-Tuple2 helpers (`__arrow_pair_first`,
   `__arrow_pair_second`, `__arrow_pair_par`, `__arrow_pair_split`,
   `__arrow_pair_dup`). These do work; they're already exercised by
   `tests/fixtures/stdlib-arrow` (captureless arrows through the bare
   functions) and `tests/fixtures/arrow-capturing-closure` (capturing
   closures via the same path, post-closure-unification fix).

Every consumer in the tree (the two fixtures, the now-removed signal
spice, `docs/guides/arrows-guide.md`) uses the **plain functions**, not
the typeclasses. The typeclass declarations are unreachable scaffolding.
They are read by `tur check` (slowing it slightly), they appear in
generated docs as if they were usable, and they actively mislead readers
into thinking there is a typeclass-dispatched arrow API to lean on.

The signal spice was the closest thing to a "real" consumer that would
have used typeclass dispatch, and it never did -- because the instances
don't exist. With signal being rebuilt from scratch
([[tur-signal-rebuild-plan]]), there is no remaining argument for keeping
the disabled scaffolding.

This plan deletes the typeclass layer, keeps the bare-function layer
exactly as-is, and tightens the docstring + guide to reflect what
actually works.

## Scope decision: scale back, not full removal

Three options were on the table:

- **Full removal of `stdlib/arrow.tur`.** Rejected: the bare-function
  combinators are useful (composition, parallel application, tuple
  splits) and are stably proven by two fixtures. Removing them costs more
  than it saves.
- **Status quo + future-proof comments.** Rejected: the typeclass
  declarations are not future-proofing, they are dead code that has been
  blocking on the same codegen issue since the file was written.
  Future-proofing belongs in a plan doc, not in the stdlib.
- **Scale back.** Selected. Keep what works; delete what's intentionally
  disabled.

If and when the typeclass-instance codegen bug is fixed and the language
gets `Either`/`Left`/`Right`, the typeclass layer can be reintroduced in
its own plan. Until then it does not belong in `stdlib/`.

## What stays

Everything currently between lines 102 and 263 of `stdlib/arrow.tur`. In
particular:

| Symbol | Kind | Proven by |
|---|---|---|
| `arr` | identity for function arrows; `^fat`-normalising shim | stdlib-arrow fixture |
| `>>>` | sequential composition | stdlib-arrow, arrow-capturing-closure |
| `arrow-first` | apply f to `e1` of a heap Tuple2 | both fixtures |
| `arrow-second` | apply f to `e2` of a heap Tuple2 | both fixtures |
| `par-comp` | parallel application over Tuple2 | both fixtures |
| `arrow-split` | duplicate input, apply two arrows, return Tuple2 | both fixtures |
| `arrow-const` | constant arrow (ignore input) | stdlib-arrow |
| `arrow-dup` | duplicate into Tuple2 | stdlib-arrow |
| `__arrow_pair_first` / `_second` / `_par` / `_split` / `_dup` | inline-C heap-Tuple2 helpers (internal) | indirect via the above |

The `^fat` parameter normalisation and the `TUR_APPLY1`-based dispatch
inside the inline-C helpers stay exactly as they are. That is the layer
that the closure-representation-unification work blessed and that the
fixtures lock in.

## What goes

Lines 17-100 of `stdlib/arrow.tur`:

- `defclass Arrow`, `ArrowZero`, `ArrowPlus`, `ArrowChoice`,
  `ArrowLoop`, `ArrowApply` declarations.
- The big "Instances for Function Arrow (->) -- omitted" comment block,
  since it documents the absence of something that will no longer be
  declared.
- The "Closure dispatch convention" comment can stay (lines 78-89) --
  it documents a fact about the bare-function helpers that is still true
  and useful.

The "Note: Full Arrow typeclass dispatch requires Turmeric features still
in development [...]" block in the module docstring (lines 12-16) is
updated to match the new shape: "Provides bare-function arrow combinators
over the `(->)` function arrow. The Haskell-style Arrow typeclass
hierarchy was scaled back in [[stdlib-arrow-scaleback-plan]] -- see
that plan for the rationale and the conditions under which it would be
reintroduced."

## What the docs need to do

`docs/guides/arrows-guide.md` is currently 503 lines and mostly already
talks about the bare-function path. Audit pass: any reference to the
typeclass declarations or to a `(definstance Arrow [(->)])`-style mental
model becomes a forward-pointer to this plan and to
[[language-readiness-for-typed-signal-plan]]. Concretely:

1. Remove the "Arrow typeclass hierarchy" overview if it makes the
   typeclass story sound functional. Replace with a one-paragraph
   "Why bare functions, not typeclass dispatch" with a link here.
2. Keep all worked examples that use `arr`, `>>>`, `arrow-first`, etc. --
   those still work and are the recommended path.
3. Drop any code samples that pretend to use `Arrow [(->)]` instance
   methods directly.

`docs/archive/stdlib-arrow-typeclass-plan.md` and
`docs/archive/arrow-thin-call-segfaults-capturing-closures.md` stay where
they are -- they are correct historical records. Add a forward pointer
from the latter to this plan.

The generated API docs (`tools/gendocs.py` output under `docs/api/`)
regenerate naturally once the typeclass declarations are removed.

## Mechanical steps

1. Edit `stdlib/arrow.tur`:
   - Delete lines 17-77 (the `defclass` block for all six classes plus
     their docstrings).
   - Delete lines 91-100 (the "Instances [...] omitted" comment block).
   - Rewrite lines 1-16 (module docstring) to match the new shape.
2. Run `tur run docs` (or `python3 tools/gendocs.py stdlib/ --out docs/api/
   --emit-tur stdlib/docstrings.tur`) to refresh the generated docs and
   the runtime lookup table.
3. Run `bash tests/run.sh` and confirm `tests/fixtures/stdlib-arrow` and
   `tests/fixtures/arrow-capturing-closure` still pass. They should --
   they only ever exercised the bare-function path. If they fail,
   investigate immediately; do not regenerate snapshots without
   understanding why.
4. Edit `docs/guides/arrows-guide.md` per the rules above.
5. Add a backward-pointer in
   `docs/archive/arrow-thin-call-segfaults-capturing-closures.md`
   referencing this plan.

## Validation

- `bash tests/run.sh` -- zero `FAIL` lines.
- `grep -n "defclass Arrow\|defclass ArrowZero\|defclass ArrowPlus\|defclass ArrowChoice\|defclass ArrowLoop\|defclass ArrowApply" stdlib/arrow.tur`
  is empty.
- `grep -rn "Arrow\b" stdlib/` does not turn up anything outside
  `arrow.tur` and `docstrings.tur` (the auto-generated lookup table).
- `tools/gendocs.py` output regenerates without referring to deleted
  classes.
- Manual read of `docs/guides/arrows-guide.md` -- every code sample
  compiles when pasted into a `.tur` file that imports stdlib.

## Acceptance checklist

- [ ] `stdlib/arrow.tur` has no `defclass` forms.
- [ ] Module docstring updated; no claims about typeclass dispatch.
- [ ] `tests/fixtures/stdlib-arrow` and
      `tests/fixtures/arrow-capturing-closure` still pass.
- [ ] `docs/api/` regenerated; no broken cross-links.
- [ ] `docs/guides/arrows-guide.md` matches the new shape.
- [ ] No regression in either fixture's `expected.c` snapshot beyond
      what the docstring change forces.

## Non-goals

- Adding `definstance Arrow [(->)]`. That is gated on the typeclass
  codegen bug fix and is not part of this scale-back.
- Implementing `Either` / `Left` / `Right`. Tracked separately in
  [[sum-types-either-plan]] (now landed: `stdlib/either.tur` provides
  `Either`/`Left`/`Right`, a right-biased `Functor [(Either E)]` instance, and
  `match` destructuring -- the prerequisite this scale-back was waiting on).
- Adding `<<<` (reverse composition). The `___` mangling collision with
  `>>>` is real (`stdlib/arrow.tur:231-233`) and a separate problem.
- Touching `arrow-first` / `arrow-second` / `par-comp` semantics. They
  work; leave them alone.

## Reintroducing the typeclass layer later

If at some point the Turmeric typeclass machinery supports
closure-returning instance methods cleanly (the codegen bug noted in the
old file), the reintroduction plan is:

1. Verify with a standalone fixture that an `Arrow [(->)]` instance can
   be declared and its methods invoked via typeclass dispatch -- not via
   the bare functions.
2. File a new plan `docs/upcoming/stdlib-arrow-typeclass-reintroduction-plan.md`
   that lays out the dependent type machinery (sum types for `ArrowChoice`,
   feedback for `ArrowLoop`) and what fixtures gate the reintroduction.
3. Only at that point reintroduce the `defclass` declarations and write
   the corresponding `definstance` forms.

Nothing in the current scale-back precludes that future work; it just
stops pretending it is already partly there.

## Cross-references

- Triggered by the analysis in
  `docs/reported/signal-spice-broken-build.md`.
- Coordinates with [[language-readiness-for-typed-signal-plan]] (the
  same typeclass codegen bug is one of the gaps that plan investigates).
- Coordinates with [[tur-signal-rebuild-plan]] -- the rebuild explicitly
  consumes the bare-function arrow API and **does not** rely on
  typeclass dispatch.
- Historical context in `docs/archive/stdlib-arrow-typeclass-plan.md`
  (the original "make typeclass dispatch work" plan that ran aground on
  the codegen bug).
