---
title: `stdlib/arrow` Typeclass Reintroduction
category: Planning
description: Reintroduce the `Arrow`, `ArrowZero`, `ArrowPlus`, `ArrowChoice`, `ArrowLoop`, `ArrowApply` typeclass hierarchy on top of `(->)` once the codegen and sum-type prerequisites land. The bare-function combinators from `stdlib/arrow.tur` stay; the typeclass layer is added back as a parallel, dispatch-driven surface. Gated on [[sum-types-either-plan]] and the closure-returning instance method codegen fix.
---

# `stdlib/arrow` Typeclass Reintroduction -- Plan

## Why

[[stdlib-arrow-scaleback-plan]] removed the disabled `defclass` declarations
for the Arrow hierarchy because they were unreachable scaffolding -- their
instances could not be written due to two language gaps:

1. **Closure-returning instance method codegen bug.** A `definstance` method
   whose return type is a closure (e.g. `arr :: (a -> b) -> arr a b` where
   `arr = (->)`, so the return is itself a function) has its dict field type
   resolved to `void *` instead of `int64_t`, causing C compile failures.
   Tracked as section A2 of [[stdlib-type-erasure-cleanup-plan]].
2. **Missing sum types.** `ArrowChoice`'s `left`, `right`, `+++`, `|||`
   methods all return arrows over `Either L R`. Without real binary sums
   these are uninhabitable. Tracked in [[sum-types-either-plan]].

The scale-back plan committed to reintroducing the typeclass layer in a
separate plan once both gaps are closed. **This is that plan.**

The bare-function combinators (`arr`, `>>>`, `arrow-first`, `arrow-second`,
`par-comp`, `arrow-split`, `arrow-const`, `arrow-dup`) **stay** -- this plan
adds the typeclass layer alongside them, it does not replace them. The
fixtures `tests/fixtures/stdlib-arrow` and
`tests/fixtures/arrow-capturing-closure` continue to lock in the bare path.

## Prerequisites -- hard gates

This plan cannot start until **all** of these are merged:

- [[sum-types-either-plan]] -- `Either L R` with `Left`/`Right` constructors,
  `case` destructuring, and `Functor [(Either E)]` instance proven by fixture.
- [[closure-returning-instance-method-codegen-plan]] -- proven by a fixture
  where a `definstance` method returns a closure capturing a free variable,
  with snapshot-stable `expected.c`.
- Section A3 of [[stdlib-type-erasure-cleanup-plan]] (operator-name C
  identifier mangling) -- needed for `Category`'s `<<<`/`>>>` pair to coexist.
  Soft gate: if this slips, the plan can land without `<<<` and add it later;
  but losing `<<<` again is a regression from the typeclass intent.

Soft prerequisites (nice but not blocking):

- Tuple feedback / full polymorphic Tuple2 -- the current heap-Tuple2 helpers
  work, but `ArrowLoop`'s `loop :: arr (b, d) (c, d) -> arr b c` is cleaner
  with a real product type than with the inline-C heap pair. Document the
  shape; do not block on it.

Before starting **Task 1**, write a short prerequisite-readiness check:
verify each of the above by name, fixture, and `expected.c`. If any is
absent, stop and return to the prerequisite's plan.

## Scope

In scope:

- `defclass Arrow`, `ArrowZero`, `ArrowPlus`, `ArrowChoice`, `ArrowLoop`,
  `ArrowApply` declarations in `stdlib/arrow.tur` (restored, but with method
  signatures matching what now compiles).
- `definstance` forms for each, instantiated at `(->)` (the function arrow).
- A `Category [(->)]` instance carrying `id`/`>>>`/`<<<` if `Category` exists
  by this point; otherwise inline the id/composition methods into `Arrow`.
- Fixtures proving each typeclass method dispatches via the dict, *not* via
  the bare-function path.
- Guide updates reframing `docs/guides/arrows-guide.md` around dispatch.

Out of scope:

- Removing or deprecating the bare-function combinators. They are a stable
  public API; users on them keep working.
- Other arrow instances (`Kleisli`, `Static`). Future plans.
- Arrow notation syntax (`proc`/`-<`). Future plan if there is demand.

## Tasks

### T1. Prerequisite-readiness check

1. Grep stdlib + fixtures to confirm `Either`/`Left`/`Right` are usable from
   client code. Confirm the `Functor [(Either E)]` fixture exists and passes.
2. Locate the codegen-bug-fix fixture from A2; confirm it exists and the
   `expected.c` shows the `int64_t` dict-field type.
3. Locate the operator-mangling fixture from A3; confirm both `>>>` and `<<<`
   are callable in the same module.
4. If any gate fails, **stop** -- this plan does not start.

Deliverable: a one-paragraph status block appended to the top of this file
recording the commit hashes of each prerequisite at start time.

### T2. Restore the `defclass` declarations

Edit `stdlib/arrow.tur`. Restore the six classes that
[[stdlib-arrow-scaleback-plan]] removed, but with method signatures that
match what the language now actually compiles:

1. `Arrow [a]` -- `arr :: (b -> c) -> a b c`, `>>>` and `<<<`, `first` and
   `second` over a real product type (or the existing heap Tuple2; decide
   in T3).
2. `ArrowZero [a]` -- `zero-arrow :: a b c`.
3. `ArrowPlus [a]` -- `plus-arrow :: a b c -> a b c -> a b c` (inherits
   `ArrowZero`).
4. `ArrowChoice [a]` -- `left :: a b c -> a (Either b d) (Either c d)`,
   `right`, `+++`, `|||`. **Requires sum types.**
5. `ArrowLoop [a]` -- `loop :: a (b, d) (c, d) -> a b c`.
6. `ArrowApply [a]` -- `app :: a (a b c, b) c`.

Each declaration carries a `;;;` docstring per the standard.

### T3. Decide the product representation

The bare-function layer uses heap-allocated Tuple2 (`__arrow_pair_*`). The
typeclass layer can either:

- **Option A** -- reuse the same heap Tuple2. Pros: shares machinery, no new
  code. Cons: heap allocation per composition; arrow programs allocate
  heavily.
- **Option B** -- use a real stack-allocated product struct (if one has
  landed by this point). Pros: zero-allocation composition. Cons: pulls in
  another prerequisite.

Recommendation: ship **A** first, file an optimisation follow-up for B.
Document the decision here.

### T4. `Arrow [(->)]` instance

1. Write `definstance Arrow [(->)]`:
   - `arr = identity`
   - `>>>` = function composition
   - `<<<` = flipped composition (gated on A3 / mangling fix)
   - `first f = \(b,d) -> (f b, d)`
   - `second f = \(b,d) -> (b, f d)`
2. Confirm the closure-returning-method codegen path is exercised: at least
   one method returns a closure capturing a free variable.
3. Snapshot the emitted C. The dict-field types must be `int64_t`, not
   `void *`. If `void *` appears, the A2 fix has regressed -- file a
   `docs/reported/` entry.

### T5. `ArrowZero [(->)]` and `ArrowPlus [(->)]` instances

Technically `(->)` does not have a sensible `zero` (a total function from
any `b` to any `c` requires `c` to be inhabited / chosen). Decide:

- **Option A** -- omit these two instances entirely; the typeclasses exist
  but `(->)` does not implement them. Document why.
- **Option B** -- implement them only for specific monoidal `c` (e.g.
  numeric zero/plus). This needs a constrained instance.

Recommendation: **A**. Declare the classes, do not instantiate them at
`(->)`. A future `Kleisli` instance is the natural home for these.

### T6. `ArrowChoice [(->)]` instance

1. Implement `left f = \case Left b -> Left (f b); Right d -> Right d`.
2. Implement `right`, `+++`, `|||` analogously.
3. Confirm `case` pattern matching from [[sum-types-either-plan]] handles
   the `Either` destructuring inside a `definstance` method body. If it
   does not -- file the gap and pause this task.
4. Fixture: `arrow-instance-choice/` -- declare, dispatch, observe.

### T7. `ArrowLoop [(->)]` instance

1. The Haskell definition uses lazy evaluation for the feedback edge.
   Turmeric is strict. Decide whether to:
   - **Implement only the non-recursive subset** -- `loop f` for `f` that
     does not actually read the fed-back value. (Useful for the threading
     pattern even without true feedback.)
   - **Wait for explicit lazy values / thunks** to land, then implement
     properly.

Recommendation: **non-recursive subset**, with a docstring warning. File a
follow-up for the lazy case.

### T8. `ArrowApply [(->)]` instance

1. `app ((f, b)) = f b`. Trivial for `(->)`.
2. Fixture confirms dispatch.

### T9. `Category` (if it exists)

If `Category` has landed by this point, write `Category [(->)]` and have
`Arrow` inherit composition from it (matching the Haskell hierarchy).
Otherwise skip and inline composition into `Arrow` as in T4.

### T10. Fixtures

Each is a directory under `tests/fixtures/` with `input.tur` + `expected.c`:

1. `arrow-instance-basic/` -- `Arrow [(->)]` dispatch: build a small arrow
   network via `arr`/`>>>`/`first`/`second` *through typeclass dispatch*
   (not the bare functions). Observe the right behaviour.
2. `arrow-instance-vs-bare/` -- same pipeline, written once with bare
   functions and once with dispatch; assert identical output.
3. `arrow-instance-choice/` -- `ArrowChoice` over `Either`.
4. `arrow-instance-loop-nonrecursive/` -- `ArrowLoop` in the non-recursive
   subset.
5. `arrow-instance-apply/` -- `ArrowApply` dispatch.
6. `arrow-instance-closure-capture/` -- a method returning a closure that
   captures a free variable; lock the codegen path.

All fixtures ASCII-only.

### T11. Existing fixtures must still pass unchanged

`tests/fixtures/stdlib-arrow` and `tests/fixtures/arrow-capturing-closure`
exercise the bare-function path. They MUST continue to pass with
**snapshot-stable** `expected.c`. If the snapshots drift, the bare layer
has changed -- root-cause it before regenerating.

### T12. Docs

1. Rewrite `docs/guides/arrows-guide.md` to cover both layers:
   - Bare functions -- the simple path; what you use when you do not need
     polymorphism over the arrow constructor.
   - Typeclass dispatch -- when you want code parametric over any `Arrow`
     instance.
2. Cross-reference [[sum-types-either-plan]] from the `ArrowChoice`
   section.
3. Update `docs/api/` (regenerated by `tur run docs`).
4. Add a status-update block to [[stdlib-arrow-scaleback-plan]] noting
   that the reintroduction has landed and pointing here.
5. Move [[stdlib-arrow-scaleback-plan]] to `docs/archive/` (the
   `churn-docs` skill is the standard mover) once this plan has been
   stable for one release cycle.

## Validation

- `bash tests/run.sh` -- zero `FAIL` lines.
- All T10 fixtures pass with `expected.c` snapshot-stable on rerun.
- T11: `tests/fixtures/stdlib-arrow` and
  `tests/fixtures/arrow-capturing-closure` pass unchanged.
- `tools/gendocs.py` output regenerated; the Arrow page lists the
  reintroduced classes and instances with correct cross-links.
- Manual: a 30-line arrow pipeline written via dispatch produces the same
  output as the bare-function equivalent.

## Acceptance checklist

- [ ] Prerequisite-readiness check (T1) recorded with commit hashes.
- [ ] All six `defclass` forms restored in `stdlib/arrow.tur`.
- [ ] `Arrow [(->)]` instance compiles and dispatches.
- [ ] `ArrowChoice [(->)]` instance compiles and dispatches over `Either`.
- [ ] `ArrowLoop [(->)]` (non-recursive subset) and `ArrowApply [(->)]`
      instances compile and dispatch.
- [ ] `ArrowZero`/`ArrowPlus` decision recorded (omitted for `(->)`, by
      default).
- [ ] All T10 fixtures pass with snapshot-stable `expected.c`.
- [ ] T11: existing bare-function fixtures pass unchanged.
- [ ] `docs/guides/arrows-guide.md` rewritten to cover both layers.
- [ ] `docs/api/` regenerated; no broken cross-links.
- [ ] [[stdlib-arrow-scaleback-plan]] annotated with a "superseded by" note.

## Non-goals

- Removing the bare-function combinators. They stay.
- `Kleisli`, `Static`, or other arrow instances beyond `(->)`.
- Arrow notation (`proc`/`-<`).
- True lazy `ArrowLoop` feedback. Tracked separately.
- Reintroducing typeclass dispatch for any other stdlib module. Each gets
  its own plan once its own prerequisites are clear.

## If a prerequisite regresses mid-plan

Any of: `Either`/`Left`/`Right` broken; closure-returning method codegen
regresses to `void *`; operator mangling collision returns.

Stop. File a `docs/reported/` entry citing the fixture that broke. Do not
work around -- the whole point of the scale-back was to avoid carrying
disabled scaffolding. If the prerequisite cannot be restored, revert this
plan and stay scaled back.

## Cross-references

- **Supersedes** the future-work block at the end of
  [[stdlib-arrow-scaleback-plan]] (`Reintroducing the typeclass layer
  later`).
- **Hard prerequisite**: [[sum-types-either-plan]].
- **Hard prerequisite**: [[closure-returning-instance-method-codegen-plan]].
- **Soft prerequisite**: section A3 of
  [[stdlib-type-erasure-cleanup-plan]] (operator-name mangling) -- needed
  for `<<<`.
- **Coordinates with** [[language-readiness-for-typed-signal-plan]] (the
  same codegen and sum-type gaps are readiness items there).
- **Coordinates with** [[tur-signal-rebuild-plan]] -- the rebuild deliberately
  consumes the bare-function arrow API; this plan does not change that.
- **Historical context**: `docs/archive/stdlib-arrow-typeclass-plan.md`
  (original "make typeclass dispatch work" plan that ran aground on the
  codegen bug) and `docs/archive/arrow-thin-call-segfaults-capturing-closures.md`.
