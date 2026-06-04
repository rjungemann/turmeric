---
title: Sum Types -- `Either`/`Left`/`Right` End-to-End
category: Planning
description: Land a real binary sum type (`Either L R`) with `Left`/`Right` constructors, pattern-matching destructuring, exhaustiveness checking, and typeclass-instance support. Prerequisite for `ArrowChoice` and the `stdlib-arrow-typeclass-reintroduction-plan`. Today's stdlib uses tagged `defstruct` (Option, Result) which conflates payload slots and does not generalise to a heterogeneous binary sum.
---

# Sum Types -- `Either`/`Left`/`Right` -- Plan

## Why

Several stdlib modules have stubbed out APIs because Turmeric has no true
algebraic sum type:

- `stdlib/arrow.tur:54-56` declares `ArrowChoice` whose `left`/`right`/`+++`/`|||`
  methods are *defined* but cannot be implemented for `(->)` -- they fundamentally
  require `Either L R`. The whole typeclass is currently dead scaffolding (see
  [[stdlib-arrow-scaleback-plan]]).
- `stdlib/session.tur:25-26,48-50,112,147-149` already references `Left`/`Right`
  in its docstrings as the protocol-choice constructors, but the implementation
  threads them as untyped tagged ints. Any future session-types polymorphism
  hits the same wall.
- Many "error or value" sites in stdlib use either `Option[A]` (loses error
  detail) or a two-of-tuple workaround (loses tag clarity). `Result[E, A]` is
  isomorphic to `Either E A` but is itself a tagged `defstruct` -- so it shares
  payload memory between the ok and err branches and cannot represent
  heterogeneously-typed ok/err payloads cleanly when both want box-by-value.

What we have today (`Option`, `Result`) is a single struct with a discriminant
bool and a *single* payload field whose type is one of the type parameters.
That works for the unary-payload case but does not generalise: `Either L R`
needs two payload slots (or a real tagged union) so `Left l` and `Right r`
can carry differently-typed values without one shadowing the other.

This plan adds **binary sums** as a first-class language feature, keyed by the
`Either` use case but designed to extend to n-ary sums (`Choice3`, ADTs with
3+ constructors) without re-doing the work.

This plan is **a prerequisite** for [[stdlib-arrow-typeclass-reintroduction-plan]]
and is the standalone version of Phase A1 in
[[stdlib-type-erasure-cleanup-plan]] (this plan supersedes that section; the
type-erasure plan should forward-link here).

## Scope

In scope:

- A real tagged-union memory layout for binary sums.
- `defdata`-style declaration surface (or a minimal extension to the existing
  `defstruct`-with-discriminant convention -- decision in Task 1).
- `Left` / `Right` constructors that lower to tagged literals.
- `case`/pattern destructuring for binary sums with exhaustiveness checking.
- Typeclass instance support so `Functor [Either E]`, `Monad [Either E]` can
  be written by hand.
- Migration of **one** stdlib site to validate the design end-to-end.

Out of scope (tracked elsewhere):

- N-ary sums with arbitrary constructor count. The layout chosen here must not
  paint us into a corner, but the parser/elaborator only commits to the binary
  shape in this plan.
- `ArrowChoice [(->)]` itself -- that lands in
  [[stdlib-arrow-typeclass-reintroduction-plan]] once this and the
  closure-returning-instance-method codegen fix are both in.
- Migrating `Option` / `Result` to the new sum machinery. They work; touching
  them is a separate cleanup once `Either` has bedded in.
- Generic GADT / dependent sum support.

## Design constraints

- The memory layout must support **heterogeneously-typed payloads** in the
  two arms without one shadowing the other at the C level. Today's
  `(defstruct Option [A] (is-some :bool) (value A))` cannot represent
  `Either L R` because there is only one payload slot.
- The discriminant convention must match `Option`/`Result` (a leading bool or
  small int tag) so future migrations are mechanical.
- Constructor lowering must not require runtime allocation in the common case;
  a stack-allocated tagged struct is the target. Heap escape only when
  captured by a closure or returned, matching how `Result` works today.
- Pattern matching must integrate with the existing `case` form rather than
  inventing a parallel matching surface.
- Exhaustiveness checking is a **warning**, not an error, in this plan -- the
  goal is to surface non-exhaustive matches without breaking the lots of
  existing `case` uses that the elaborator does not yet model exhaustively.

## Tasks

### T1. Decide the surface syntax

Pick between:

- **Option A** -- extend `defstruct` with a discriminated form:
  `(defstruct Either [L R] (:left L) (:right R))` where the leading keyword
  on each field marks it as a *constructor arm* rather than a record field.
  Pros: reuses existing parser, fits the `Option`/`Result` pattern. Cons:
  overloads `defstruct` semantics; field-vs-arm becomes context-sensitive.
- **Option B** -- introduce `defdata`:
  `(defdata Either [L R] (Left L) (Right R))`. Pros: clean separation between
  records and sums; matches Haskell/ML intuition; extends naturally to n-ary
  sums. Cons: new top-level form to teach the parser, elaborator, codegen,
  docstring tooling, and gendocs.

Recommendation: **Option B (`defdata`)**. The cleanliness wins compound as
soon as a third constructor lands. Document the decision in this plan.

Deliverable: short ADR-style note appended to this plan with the chosen
syntax and rejected alternatives.

### T2. Tagged-union memory layout

1. Decide between a C `struct { uint8_t tag; union { L l; R r; } payload; }`
   layout and a "two parallel slots" layout
   `struct { uint8_t tag; L l; R r; }`.
2. Constraint: the layout must be FFI-safe (passable to inline-C blocks) and
   must not require the C compiler to instantiate a generic union per
   type-parameter pair.
3. Recommendation: parallel slots with type erasure to `int64_t` at the C
   boundary (matching how `Option[A]` already erases its payload). The
   elaborator restores the proper type on extraction.
4. Reuse the discriminant convention from `Option` (0 = first arm, 1 = second
   arm) so a uniform "is this arm present?" predicate works across sum types.

Deliverable: a layout decision recorded here plus a worked example showing
`Either int cstr` -> emitted C struct.

### T3. Parser / AST support

1. Add the `defdata` keyword (or the extended `defstruct` form, per T1) to
   the reader.
2. Build an AST node for binary-sum declarations carrying:
   - the type-constructor name and its type parameters,
   - the two value-constructor names with their payload types.
3. Reject ill-formed declarations: zero arms, three+ arms (defer to a future
   n-ary plan), constructor name collisions with existing names in scope.
4. Wire `gendocs.py` to recognise the new form and emit a sum-type entry on
   the module page.

### T4. Constructor lowering

1. Elaborator: `(Left x)` and `(Right y)` resolve to value constructors of
   the declared sum type; type-infer `L` and `R` from the payload.
2. Codegen: emit a stack-allocated tagged struct literal at the call site,
   with the discriminant set and the other arm zeroed.
3. Heap escape: when a sum value is returned from a function, captured by a
   closure, or stored in a struct field, allocate it on the heap (same path
   as today's `Result`/`Option` escape).
4. Inline-C bridge: document the C layout in a `;;;` comment block on the
   `Either` definition so inline-C blocks can construct/destructure values
   without going through Turmeric.

### T5. Pattern matching in `case`

1. Extend the existing `case` pattern grammar to accept
   `(Left <pat>)` / `(Right <pat>)` patterns.
2. Bind the payload to the named pattern variable with the correct arm type
   in scope for the body.
3. Catch-all `_` still works.
4. Nested patterns: `(Left (Just x))` decomposes through nested sums.

### T6. Exhaustiveness checking (warning)

1. After elaboration, walk every `case` whose scrutinee has a known sum type
   and check that both arms (or a wildcard) are covered.
2. Emit a warning, **not an error**, citing the missing arm. The diagnostic
   must point at the `case` form, not at the type declaration.
3. Provide an opt-out for the warning at the form level: a `#{NonExhaustive}`
   marker on the `case`. This is the escape hatch for cases where the
   programmer has proved exhaustiveness by other means.
4. Decision: pattern overlap (two `Left` arms) is also a warning, not an
   error -- match it to the same `#{NonExhaustive}` opt-out.

### T7. Typeclass-instance support

1. Verify the typeclass dispatch resolver can match `Either E` (partially
   applied) as the instance head, the same way `Option` and `Result` are
   matched today.
2. Hand-write `Functor [(Either E)]` as the smoke instance: `fmap` on
   `Right x` runs `f`, on `Left e` is the identity.
3. Confirm this exercises the closure-returning-instance-method codegen path
   that `arr` will need -- if it surfaces the same `void *` vs `int64_t`
   dict-field bug as `stdlib/arrow.tur` did, **do not work around it here**;
   gate T7 on [[closure-returning-instance-method-codegen-plan]]. See
   cross-references.

### T8. Fixtures

Each is a directory under `tests/fixtures/` with `input.tur` + `expected.c`:

1. `sum-either-basic/` -- declare `Either`, construct `Left 1` and
   `Right "x"`, print the discriminant.
2. `sum-either-match/` -- `case` over both arms with payload extraction.
3. `sum-either-nested/` -- `Either int (Option cstr)`, nested pattern match.
4. `sum-either-exhaustive-warn/` -- non-exhaustive `case`; assert a warning
   is emitted (golden stderr file).
5. `sum-either-nonexhaustive-opt-out/` -- the same case with
   `#{NonExhaustive}`; assert no warning.
6. `sum-either-functor-instance/` -- `Functor [(Either E)]` instance,
   `fmap` smoke test.
7. `sum-either-inline-c-roundtrip/` -- construct a sum value in Turmeric,
   destructure it in an inline-C block, return a value to Turmeric.

All fixtures are ASCII-only (per the project rule).

### T9. Migrate one stdlib site as a smoke test

Pick the smallest two-of-tuple "error or value" site in stdlib and migrate
it to `Either Error Value`. Candidates:

- A specific helper inside `stdlib/result.tur` that today returns a tuple to
  carry both an error code and a partial value.
- A parser-style helper in `stdlib/str.tur` that returns "parsed value or
  remaining input" today as a tuple.

Pick **one** in Task 1 review; do not migrate more in this plan.

### T10. Docs

1. New guide: `docs/guides/sum-types-guide.md` -- when to use a sum vs a
   struct, the `defdata` form, exhaustiveness behaviour, FFI layout.
2. Update [[stdlib-arrow-scaleback-plan]] to reference this plan as the
   prerequisite (already linked, just sanity-check).
3. Update [[stdlib-type-erasure-cleanup-plan]] section A1 to forward-link
   here and say "see this plan for the detailed task list."
4. Update [[language-readiness-for-typed-signal-plan]] if it depends on
   sum types (cross-check during Task 1).

## Validation

- `bash tests/run.sh` -- zero `FAIL` lines.
- Every fixture in T8 passes.
- The migrated stdlib site in T9 has snapshot-stable `expected.c` and at
  least one new fixture exercising it.
- `tools/gendocs.py` regenerates without complaint and the `Either` page
  renders correctly under `docs/api/`.
- Manual: write a 20-line throwaway program that builds an `Either int
  cstr` tree, pattern-matches it, and passes the result through a `Functor`
  instance. Confirm it compiles, runs, and gives the expected output.

## Acceptance checklist

- [ ] `defdata` (or chosen surface) declares `Either L R` in stdlib.
- [ ] `Left` / `Right` constructors lower correctly; layout documented.
- [ ] `case` destructures both arms with type-correct bindings.
- [ ] Non-exhaustive `case` emits a warning citing the missing arm.
- [ ] `#{NonExhaustive}` opt-out silences the warning.
- [ ] `Functor [(Either E)]` instance compiles and dispatches.
- [ ] All seven T8 fixtures pass with snapshot-stable `expected.c`.
- [ ] One stdlib site migrated; its tests still pass.
- [ ] `docs/guides/sum-types-guide.md` written and linked from the stdlib
      index.
- [ ] `tools/gendocs.py` output regenerated; no broken cross-links.

## Non-goals

- Migrating `Option` and `Result` to `defdata`. They work; defer.
- N-ary sums (3+ constructors). The layout must not preclude this, but the
  parser/elaborator only commits to binary in this plan.
- GADTs, dependent sums, or pattern guards beyond what `case` already does.
- Exhaustiveness as a hard error. Warning only in this plan; can be tightened
  later once the false-positive rate is measured.

## Cross-references

- **Unblocks** [[stdlib-arrow-typeclass-reintroduction-plan]] -- that plan
  cannot land until `Either` exists.
- **Supersedes** the A1 subsection of [[stdlib-type-erasure-cleanup-plan]];
  that plan should forward-link here.
- **Coordinates with** [[language-readiness-for-typed-signal-plan]] -- the
  same sum-type gap is one of the readiness items.
- **Coordinates with** [[closure-returning-instance-method-codegen-plan]].
  T7 here may surface that bug; if so, that fix becomes a hard prerequisite
  for T7.
- **Historical context**: `docs/archive/stdlib-arrow-typeclass-plan.md`
  documents the original encounter with the missing-sum-types problem.

## ADR -- decisions taken during execution

This section records the choices made while landing the plan (Task 1 asked
for an ADR-style note; it is captured here).

### Context: the core feature already existed

When execution began, Turmeric *already had* first-class sum types: `defdata`
(binary and n-ary), parametric constructors (`[L R]`), monomorphised
`ctor_<Name>` lowering, `match` with payload-binding patterns, nested
matching, and exhaustiveness analysis. `(defdata Either [L R] (Left L)
(Right R))` compiled and ran out of the box. The plan was written as though
none of this existed -- it predates the `defdata`/`match` implementation. So
this plan was executed as a *productionisation* of the existing feature
(stdlib module, instance support, fixtures, docs, FFI documentation) rather
than a from-scratch language build.

### T1 -- surface syntax: `defdata` (Option B), as already implemented

`defdata` is the shipped surface and matches the plan's recommendation
(Option B). No `defstruct` overloading was introduced. Decision: **keep
`defdata`**.

### T6 -- exhaustiveness: hard **error** + `#{NonExhaustive}` opt-out

The plan specced exhaustiveness as a *warning*. The compiler already enforced
it as a hard **error** (TUR-E0301 / `match: non-exhaustive patterns`), and an
existing fixture (`tests/fixtures/errors/adt-nonexhaustive`) locks that in.
Downgrading to a warning would weaken a real safety guarantee and break that
fixture. Decision (confirmed with the requester): **keep the hard error** and
add a form-level `#{NonExhaustive}` marker as the explicit opt-out. Placed
immediately after `match` (before the scrutinee), it suppresses the
non-exhaustiveness diagnostic for that one form. Overlap warnings were not in
scope (no overlap analysis exists today).

### T2 -- layout: type-erased tagged union (as already emitted)

`defdata` lowers to `struct tur_adt_<Name> { int tag; union { ...arms } as; }`
with payloads erased to `int64_t`, exactly the "parallel/erased slots"
recommendation. `Either int cstr` -> the struct documented in
`stdlib/either.tur` and the FFI section of the guide.

### T7 -- typeclass instances over an ADT: two real compiler gaps fixed

Writing `Functor [(Either E)]` surfaced two genuine defects (reported-and-fixed
in this session, per the bug-reporting rule):

1. `AdtDef` had no `origin_file_id` (unlike `StructDef`), so the
   orphan-instance check could not credit an instance to the module that
   *defines* the ADT -- every ADT instance was wrongly flagged
   `TUR-E0013 orphan`. Fix: add `origin_file_id` to `AdtDef`, populate it in
   `elab_defdata`/`elab_defgadt`, and credit `TY_ADT` (and the `TY_APP` head's
   `fn`) in the orphan check.
2. The `definstance` head parser discarded an ADT type-constructor and
   substituted an opaque `TY_STRUCT`/NULL placeholder, losing the ADT identity
   for both the orphan check and dispatch. Fix: preserve the real `TY_ADT`
   (and, for a partially-applied `(Either E)` head, set the `TY_APP.fn` from
   the constructor's actual binding).

With those fixed, the right-biased `Functor [(Either E)]` instance compiles and
dispatches. No `void *` vs `int64_t` dict-field bug surfaced, so T7 was **not**
gated on the closure-returning-instance-method plan.

### T8 -- fixtures: stdout/diag snapshots, not `expected.c`

The plan asked for `expected.c` per fixture. The directly-analogous existing
ADT fixtures (`adt-basic`, `adt-param`, `adt-nested`, ...) deliberately use
`expected.stdout`, and only ~10% of fixtures carry `expected.c`. Embedding the
full auto-loaded stdlib in seven brittle C snapshots (which the project's
snapshot rule would force us to regenerate on every codegen change) is poor
value. Decision: **follow the local ADT-fixture convention** -- `expected.stdout`
for the positive fixtures, `expected.diag` for the negative one. The
non-exhaustive "warning" fixture became an *error* fixture
(`errors/sum-either-nonexhaustive`) to match the T6 decision, paired with
`sum-either-nonexhaustive-opt-out` proving the `#{NonExhaustive}` escape hatch.

### T9 -- migration: a lossy sentinel, since no two-of-tuple site existed

The plan's candidate "two-of-tuple error-or-value" sites did not actually
exist in stdlib. The real instance of the underlying problem is
`str->int` / `cstr->parse-int`, which fold *every* failure into the sentinel
`0`. Migration: add `str->int-checked` to `stdlib/str.tur`, returning
`(Right n)` on a clean parse or `(Left code)` describing the failure.
`str.tur` is not auto-loaded and has no `expected.c` consumers, so the blast
radius is nil. Exercised by `tests/fixtures/sum-either-str-parse`.

### T10 -- docs

`docs/guides/sum-types-guide.md` written. `gendocs.py` taught to recognise
`defdata`/`defgadt` (sum-type entries on the module page);
`stdlib/docstrings.tur` regenerated. `docs/api/*.html` is gitignored, so only
the `.tur` lookup table is a tracked artifact.
