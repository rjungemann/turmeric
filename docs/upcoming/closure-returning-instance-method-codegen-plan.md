---
title: Closure-Returning Instance-Method Codegen Fix
category: Planning
description: Fix the `definstance` codegen bug where a method whose return type is a closure has its dict field type resolved to `void *` instead of `int64_t` (the fat-closure handle type), causing C compile failures. This is the load-bearing language gap that prevents reintroducing the Arrow typeclass hierarchy and blocks several other stdlib instances (`mw-cors` currying, `Functor [(Either E)]`).
---

# Closure-Returning Instance-Method Codegen Fix -- Plan

## Why

When a `definstance` declares a method whose return type is a closure -- e.g.
`arr :: (b -> c) -> (->) b c` where the return value is itself a function --
the typeclass-dict lowering pass resolves that dict field's C type to
`void *` instead of `int64_t` (the fat-closure handle type the rest of the
language uses). The generated C then fails to compile because the call site
expects to invoke a fat closure via `TUR_APPLY1`/`TUR_APPLY2`, but the dict
field is a raw pointer with no callable shape.

This is the single most expensive language gap in stdlib right now:

- `stdlib/arrow.tur:91-100` -- the whole Arrow typeclass hierarchy was
  scaffolded but never instantiated specifically because of this bug.
  [[stdlib-arrow-scaleback-plan]] deleted the disabled declarations rather
  than carry them, and [[stdlib-arrow-typeclass-reintroduction-plan]] is
  blocked on this fix.
- `stdlib/httpd.tur:1688` -- `(mw-cors opts)` currying: the `__pap` wrapper
  stores `CorsOpts` (a struct) into an `int64_t` env field, also failing.
  This is a sibling bug; the fix here may or may not cover the struct case
  (see Task A5 / cross-link below).
- `Functor [(Either E)]` (planned in [[sum-types-either-plan]]) -- `fmap`
  returns a closure; the same bug will bite if not pre-fixed.

This plan is the standalone version of **Section A2** of
[[stdlib-type-erasure-cleanup-plan]]. That section stays as a coordinating
index; this plan supersedes it for execution.

## Scope

In scope:

- Reproduce the bug in a minimal fixture.
- Locate the dict-field type-resolution site in the typeclass lowering pass.
- Fix the propagation so closure-returning dict fields carry `int64_t`.
- Audit sibling dict-field types (struct returns, opaque returns) for the
  same bug class; either fix them in this plan or file a focused follow-up.
- Cover with fixtures and snapshot-stable `expected.c`.

Out of scope:

- The struct-in-`__pap`-env case (`mw-cors`) -- tracked in section A5 of
  [[stdlib-type-erasure-cleanup-plan]]. **Decision point in Task 4**: if
  the root cause is the same, fold A5 in; if not, leave it separate.
- Operator-name C identifier mangling (`>>>` / `<<<` collision) -- a
  different bug, tracked in section A3 of the type-erasure plan.
- Anything Arrow-specific. This is a pure compiler fix.

## Tasks

### T1. Minimal repro

1. Write a tiny `defclass` with a single method returning a closure:
   `(defclass HasArr [a] (arr-of [f] : (a int int)))` where `a` is the
   arrow constructor and the method returns a value of `a`-shape.
2. Write a `definstance HasArr [(->)]` whose method body returns a
   closure capturing a free variable.
3. Build it with `./build/tur emit-c`. Confirm the emitted C either fails
   to compile or that the dict-field type is `void *`.
4. Land the repro as `tests/fixtures/instance-closure-return-bug/` with a
   `requires.compiler-bug` marker (a new skip marker that PASS-skips the
   fixture until the bug is fixed). Add the marker to `tests/run.sh` if
   not present.

Deliverable: a failing repro that becomes a passing fixture once T4 lands.

### T2. Locate the resolution site

1. Grep the codegen pass for dict-field type resolution -- start with the
   typeclass elaborator and walk to C emission.
2. Identify the exact function/lines where a method's return type is
   inspected to decide the dict-field's C type.
3. Trace why a closure return type erases to `void *`. Likely candidates:
   a missing `case` arm for the closure type kind; a wrong default when the
   return is a type-variable; an early `void *` fallback in a type-map
   helper.
4. Record findings in a short note appended to this plan.

### T3. Audit sibling cases

Before fixing, enumerate every dict-field type that the resolver could see:

- closure (the bug),
- primitive `:int`/`:bool`/`:cstr`,
- struct value,
- opaque newtype,
- another typeclass dict (higher-rank),
- type variable bound by an outer instance head.

For each, write a one-liner: does the resolver handle it correctly today?
This is the audit T3 of A2 -- do it before the fix so the fix can address
the class, not just the symptom.

### T4. Fix the type propagation

1. Patch the resolution site so closure-returning dict fields are emitted
   as `int64_t` (the fat-closure handle type).
2. Confirm the audit cases from T3 either still work or are explicitly
   marked as separate bugs.
3. **Decision point**: if the struct-return case (`mw-cors` / A5) shares
   the same root, fold the fix here. Otherwise, write a one-paragraph
   note explaining why it is genuinely different and leave A5 standing.
4. Regenerate the affected snapshots in `tests/fixtures/`. Review the
   diff for unintended changes (snapshots in unrelated areas should be
   untouched).

### T5. Remove the skip marker from T1's fixture

The bug fixture now passes; delete `requires.compiler-bug` from its dir.
Confirm `bash tests/run.sh` includes it in the live suite.

### T6. Add coverage fixtures

Each is a `tests/fixtures/` directory with `input.tur` + `expected.c`:

1. `instance-closure-return-simple/` -- method returns a closure with no
   captures.
2. `instance-closure-return-capture-int/` -- method returns a closure
   capturing an `int`.
3. `instance-closure-return-capture-struct/` -- method returns a closure
   capturing a struct field.
4. `instance-closure-return-nested/` -- method returns a closure that
   itself returns a closure (curried).
5. `instance-closure-return-via-other-method/` -- method A returns a
   closure built by calling method B on the same dict.

All fixtures ASCII-only.

### T7. Re-enable downstream consumers (smoke test, not part of this plan's deliverable)

Verify (do **not** land here, but confirm the door is open):

1. A throwaway `definstance Arrow [(->)]` for `arr`/`>>>` compiles and runs.
   Do not commit -- [[stdlib-arrow-typeclass-reintroduction-plan]] owns
   landing the real instances.
2. A throwaway `Functor [(Either E)]` instance whose `fmap` returns a
   closure compiles. Same -- do not commit; [[sum-types-either-plan]]
   owns landing the real instance.

If either smoke test fails, the fix is incomplete -- iterate on T4.

### T8. Docs

1. Short note in `docs/guides/typeclass-internals-guide.md` (create if
   absent) describing the dict-field type-resolution rule and the
   closure-handle convention.
2. Annotate [[stdlib-arrow-scaleback-plan]] with a "the codegen
   prerequisite has landed" status line.
3. Annotate Section A2 of [[stdlib-type-erasure-cleanup-plan]] as
   "superseded by [[closure-returning-instance-method-codegen-plan]]".
4. Move `docs/archive/arrow-thin-call-segfaults-capturing-closures.md`
   stays where it is, but add a forward-pointer to this plan.

## Validation

- `bash tests/run.sh` -- zero `FAIL` lines.
- The T1 repro (now in T5 form) passes without `requires.compiler-bug`.
- All T6 fixtures pass with snapshot-stable `expected.c`.
- T7 smoke tests succeed in a throwaway worktree (not committed).
- Manual: search the emitted C for `void *` in dict struct definitions
  produced by a closure-returning method -- expect zero hits.

## Acceptance checklist

- [ ] T1 repro fixture exists and (initially) demonstrates the bug.
- [ ] T2 root-cause note appended to this plan.
- [ ] T3 sibling-case audit completed and recorded.
- [ ] T4 fix landed; dict-field type for closure returns is `int64_t`.
- [ ] T4 decision recorded on whether A5 (struct-in-`__pap`-env) shares
      the fix.
- [ ] T5: skip marker removed; fixture in live suite.
- [ ] All T6 fixtures pass with snapshot-stable `expected.c`.
- [ ] T7 smoke tests pass in a throwaway worktree.
- [ ] Docs updated; A2 marked superseded.

## Non-goals

- Reintroducing the Arrow typeclass hierarchy. That is
  [[stdlib-arrow-typeclass-reintroduction-plan]].
- Implementing `Either`. That is [[sum-types-either-plan]].
- Fixing the struct-in-`__pap`-env case in general. Only folded in if the
  root cause is shared (T4 decision point).
- Operator-name mangling. Section A3 of
  [[stdlib-type-erasure-cleanup-plan]].

## Cross-references

- **Supersedes** section A2 of [[stdlib-type-erasure-cleanup-plan]].
- **Unblocks** [[stdlib-arrow-typeclass-reintroduction-plan]] (hard
  prerequisite).
- **Unblocks** the `Functor [(Either E)]` instance step of
  [[sum-types-either-plan]] (T7 there).
- **Coordinates with** section A5 of [[stdlib-type-erasure-cleanup-plan]]
  (struct-in-`__pap`-env): possibly the same fix; decision in T4.
- **Coordinates with** [[language-readiness-for-typed-signal-plan]] (this
  bug is one of the readiness items there).
- **Historical context**:
  `docs/archive/arrow-thin-call-segfaults-capturing-closures.md` -- the
  original encounter with the bug.
- **Triggered by** the analysis in
  `docs/reported/signal-spice-broken-build.md` and
  [[stdlib-arrow-scaleback-plan]].
