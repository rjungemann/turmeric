# Plan Ordering -- Working Scratch

## Format

This file is an ordered, grouped checklist of the plans under `docs/upcoming/`
(excluding `docs/upcoming/v1/`), sorted so that each plan's prerequisites appear
above it. Each entry has:

- `N. filename.md` -- the plan file, relative to `docs/upcoming/`
- A `Goal:` line -- one-line summary of what the plan does
- A `Deps:` line -- either `independent` or a list of earlier entries it depends on
- A `Status:` line -- current implementation status as recorded in the plan

Phases are coarse groupings, not strict gates: items within a phase have no
ordering between them, but every item in phase N has all its dependencies
satisfied by phases <= N. When editing this file, keep the format intact --
do not collapse phases, do not reword entries into prose, and update the
`Deps:` lines if you move things around.

---

## Phase 0 -- Foundation & syntax

1. `positional-nominal-type-identity-fix-plan.md`
   - Goal: Enforce nominal type identity for struct/ADT/opaque args at call sites
   - Deps: independent
   - Status: Complete (landed in commit 2ae2b6d4)

2. `ptr-generic-parameterised-type-plan.md`
   - Goal: Make `:ptr<T>` a first-class parameterised type for any T
   - Deps: independent
   - Status: Complete (landed in commit def6724f; G5 spike validated)

3. `spaced-type-annotation-migration-plan.md`
   - Goal: Migrate codebase from fused `(name:type)` to spaced `(name : type)`
   - Deps: independent
   - Status: In progress (Phase 1a-bis complete; compiler readiness done; Phase 1b-1c pending; Phases 2-7 outstanding)

## Phase 1 -- Compiler & closure ABI fixes

4. `closure-returning-instance-method-codegen-plan.md`
   - Goal: Fix dict-field type resolution (void* -> int64_t) for closure-returning instance methods
   - Deps: independent
   - Status: Draft (T1-T8 tasks defined, not started)

5. `bare-fat-result-type-inference-plan.md`
   - Goal: Infer result type for bare-^fat closures so they can return non-int (`:float`, etc.)
   - Deps: independent (external closure-rep prereqs not in this set)
   - Status: Draft (Phase A & B described, not implemented)

6. `poly-to-fat-typed-shim-plan.md`
   - Goal: Generalize poly-to-fat shim for non-int64 typeclass method signatures
   - Deps: 5
   - Status: Draft (Phase 0 & 1 designed, not implemented)

## Phase 2 -- Type system expansion

7. `sum-types-either-plan.md`
   - Goal: Land binary sum types (`Either L R`) with constructors, pattern matching, exhaustiveness, typeclass support
   - Deps: independent
   - Status: Draft (T1-T10 tasks defined, not implemented)

## Phase 3 -- Readiness investigation

8. `language-readiness-for-typed-signal-plan.md`
   - Goal: Spike-validate that the type system can support a typed signal library; gate remaining gaps
   - Deps: 2, 4, 5, 6, 7
   - Status: Draft (G1-G8 spikes designed with verdict template; no verdicts filled yet)

## Phase 4 -- Stdlib hardening

9. `stdlib-opaque-handle-types-plan.md`
   - Goal: Wrap bare `:int`/`:ptr<void>` resource handles in `defopaque` newtypes
   - Deps: 1
   - Status: Draft (Tier 1-3 inventory defined; Phase 1-3 rollout not started)

10. `stdlib-inline-c-deworkaround-plan.md`
    - Goal: Replace inline-C workarounds in stdlib with idiomatic Turmeric
    - Deps: 9
    - Status: In progress (Phase 1 complete as of 2026-06-03; Phases 2-4 outstanding)

11. `stdlib-advanced-typing-plan.md`
    - Goal: Add linearity/affinity, session types, effects, refinement, typeclass consolidation to stdlib
    - Deps: 9
    - Status: Draft (Phases L, S, E, R, T designed, not implemented)

## Phase 5 -- Stdlib API cleanup

12. `stdlib-arrow-scaleback-plan.md`
    - Goal: Remove disabled Arrow typeclass scaffolding; keep only bare-function combinators
    - Deps: 8
    - Status: Draft (mechanical steps defined, awaiting approval)

13. `stdlib-type-erasure-cleanup-plan.md`
    - Goal: Replace int64-erased typeclass stubs with real instances
    - Deps: 4, 7 (plus operator-name mangling fix tracked inside the plan)
    - Status: Draft (Phases A-C with subtasks defined, not implemented)

## Phase 6 -- Typeclass reintroduction

14. `stdlib-arrow-typeclass-reintroduction-plan.md`
    - Goal: Reintroduce Arrow / ArrowChoice / ArrowLoop typeclasses on `(->)`
    - Deps: 4, 7, 13
    - Status: Draft, blocked on prerequisites (T1-T12 tasks defined)

## Phase 7 -- Signal rebuild

15. `tur-signal-rebuild-plan.md`
    - Goal: Rebuild the tur-signal spice on top of the modern typed infrastructure
    - Deps: 8, 12
    - Status: Draft, blocked on plan 8 (Phases 1-6 designed)
