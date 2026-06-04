# Plan Ordering -- Working Scratch

## Format

This file is an ordered, grouped checklist of the plans under `docs/upcoming/`
(excluding `docs/upcoming/v1/`) and the bug reports under `docs/reported/`,
sorted so that each entry's prerequisites appear above it. Each entry has:

- `N. <path>.md` -- the plan or report file, relative to `docs/`
  (`upcoming/...` or `reported/...`)
- A `Kind:` line -- `plan` or `report`
- A `Goal:` line -- one-line summary of what the plan does (or, for a
  report, the bug being tracked)
- A `Deps:` line -- either `independent` or a list of earlier entries it depends on
- A `Status:` line -- current implementation status as recorded in the plan,
  or the disposition of the report (open / resolved-by-N / etc.)

Phases are coarse groupings, not strict gates: items within a phase have no
ordering between them, but every item in phase N has all its dependencies
satisfied by phases <= N. When editing this file, keep the format intact --
do not collapse phases, do not reword entries into prose, and update the
`Deps:` lines if you move things around.

---

## Phase 0 -- Foundation & syntax

1. `upcoming/positional-nominal-type-identity-fix-plan.md`
   - Kind: plan
   - Goal: Enforce nominal type identity for struct/ADT/opaque args at call sites
   - Deps: independent
   - Status: Complete (landed in commit 2ae2b6d4)

2. `reported/positional-nominal-type-identity-not-checked.md`
   - Kind: report
   - Goal: Positional args type-checked by coarse TypeKind only -- distinct struct/opaque/ADT values of the same kind silently accepted
   - Deps: 1
   - Status: Resolved by 1

3. `reported/partial-application-skips-captured-arg-type-check.md`
   - Kind: report
   - Goal: `elab_partial_apply` binds provided args via `type_from_kind` only and never checks them against the parameter -- under-saturated calls accept args the saturated call rejects
   - Deps: 1
   - Status: Complete

4. `upcoming/ptr-generic-parameterised-type-plan.md`
   - Kind: plan
   - Goal: Make `:ptr<T>` a first-class parameterised type for any T
   - Deps: independent
   - Status: Complete (landed in commit def6724f; G5 spike validated)

5. `upcoming/spaced-type-annotation-migration-plan.md`
   - Kind: plan
   - Goal: Migrate codebase from fused `(name:type)` to spaced `(name : type)`
   - Deps: independent
   - Status: Complete

## Phase 1 -- Compiler & closure ABI fixes

6. `upcoming/closure-returning-instance-method-codegen-plan.md`
   - Kind: plan
   - Goal: Fix dict-field type resolution (void* -> int64_t) for closure-returning instance methods
   - Deps: independent
   - Status: Complete

7. `upcoming/bare-fat-result-type-inference-plan.md`
   - Kind: plan
   - Goal: Infer result type for bare-^fat closures so they can return non-int (`:float`, etc.)
   - Deps: independent (external closure-rep prereqs not in this set)
   - Status: Complete - Phase B is a bigger change that can be done another time

8. `upcoming/bare-fat-lambda-param-plan.md`
   - Kind: plan
   - Goal: Allow bare `^fat g` parameter binders on `(fn ...)` lambdas (already supported on top-level `defn`), unblocking the lambda-retype path for closure-returning instance methods
   - Deps: 6, 7
   - Status: Complete (found 2026-06-03 while wiring closure-returning instance method retype)

9. `upcoming/poly-to-fat-typed-shim-plan.md`
   - Kind: plan
   - Goal: Generalize poly-to-fat shim for non-int64 typeclass method signatures
   - Deps: 7
   - Status: In Progress

10. `reported/nested-closure-transitive-capture.md`
    - Kind: report
    - Goal: Closures nested two levels deep that reference a grandparent-scoped variable never thread it through the middle closure's env -- generated C references an undeclared local
    - Deps: independent
    - Status: In Progress

11. `reported/intra-instance-method-dispatch-unsupported.md`
    - Kind: report
    - Goal: `(.other self ...)` from inside another method's body of the same instance fails elaboration with "no typeclass method found"; external dispatch on the receiver works
    - Deps: independent (touches dispatch logic adjacent to 6)
    - Status: In Progress

12. `reported/fn-typed-return-lowered-to-result-type.md`
    - Kind: report
    - Goal: A `defn` whose declared return type is `(fn [...] T)` emits a C signature using the inner `T` instead of a closure-box pointer -- register-class miscompile risk (e.g. `(fn [:float] :float)` → `double` while body returns `void *`)
    - Deps: independent (surfaced by 14 / G2 spike)
    - Status: Open

## Phase 2 -- Type system expansion

13. `upcoming/sum-types-either-plan.md`
    - Kind: plan
    - Goal: Land binary sum types (`Either L R`) with constructors, pattern matching, exhaustiveness, typeclass support
    - Deps: independent
    - Status: Complete

## Phase 3 -- Readiness investigation

14. `upcoming/language-readiness-for-typed-signal-plan.md`
    - Kind: plan
    - Goal: Spike-validate that the type system can support a typed signal library; gate remaining gaps
    - Deps: 4, 6, 7, 9, 13
    - Status: Draft (G1-G8 spikes designed with verdict template; no verdicts filled yet)

## Phase 4 -- Stdlib hardening

15. `upcoming/stdlib-opaque-handle-types-plan.md`
    - Kind: plan
    - Goal: Wrap bare `:int`/`:ptr<void>` resource handles in `defopaque` newtypes
    - Deps: 1
    - Status: In Progress

16. `upcoming/stdlib-inline-c-deworkaround-plan.md`
    - Kind: plan
    - Goal: Replace inline-C workarounds in stdlib with idiomatic Turmeric
    - Deps: 15
    - Status: In progress (Phase 1 complete as of 2026-06-03; Phases 2-4 outstanding)

17. `upcoming/stdlib-advanced-typing-plan.md`
    - Kind: plan
    - Goal: Add linearity/affinity, session types, effects, refinement, typeclass consolidation to stdlib
    - Deps: 15
    - Status: Draft (Phases L, S, E, R, T designed, not implemented)

## Phase 5 -- Stdlib API cleanup

18. `upcoming/stdlib-arrow-scaleback-plan.md`
    - Kind: plan
    - Goal: Remove disabled Arrow typeclass scaffolding; keep only bare-function combinators
    - Deps: 14
    - Status: Draft (mechanical steps defined, awaiting approval)

19. `upcoming/stdlib-type-erasure-cleanup-plan.md`
    - Kind: plan
    - Goal: Replace int64-erased typeclass stubs with real instances
    - Deps: 6, 13 (plus operator-name mangling fix tracked inside the plan)
    - Status: Draft (Phases A-C with subtasks defined, not implemented)

## Phase 6 -- Typeclass reintroduction

20. `upcoming/stdlib-arrow-typeclass-reintroduction-plan.md`
    - Kind: plan
    - Goal: Reintroduce Arrow / ArrowChoice / ArrowLoop typeclasses on `(->)`
    - Deps: 6, 13, 19
    - Status: Draft, blocked on prerequisites (T1-T12 tasks defined)

## Phase 7 -- Signal rebuild

21. `reported/signal-spice-broken-build.md`
    - Kind: report
    - Goal: tur-signal spice does not compile -- references removed `__arrow_call1` plus un-imported stdlib symbols; the existing rebuild plan understates the scope by assuming a half-done `:float` migration baseline
    - Deps: independent (prerequisite finding for 22)
    - Status: Open (severity: high)

22. `upcoming/tur-signal-rebuild-plan.md`
    - Kind: plan
    - Goal: Rebuild the tur-signal spice on top of the modern typed infrastructure
    - Deps: 14, 18, 21
    - Status: Draft, blocked on plan 14 (Phases 1-6 designed)

## Other notes

One cosmetic note: re-canonicalizing surfaced some pre-existing top-level
blank-line/section-comment shuffling in httpd-compress.tur (e.g. an extra blank
before a docstring'd defn). I confirmed it's lossless, idempotent, and that
gendocs.py still associates those docstrings (blank lines don't reset its doc
buffer). Tightening that top-level blank-line handling would be a separate,
larger change — happy to take it on if you'd like it cleaner.

Investigate Phase B of upcoming/bare-fat-result-type-inference-plan.md later

* docs/upcoming/stdlib-opaque-handle-types-plan.md
    * stm error fixture — the STM-block guard (TUR-E0009) fires before argument
    type-checking, so a standalone wrong-handle case can't reach the TUR-E0001. The
    typing is validated by compile + suite.
    * tur/ref — its handle is an :int with unannotated (effectively polymorphic)
    params and a Clone instance returning raw :int; clean newtyping needs more than
    a signature pass.

docs/reported/poly-to-fat-drops-args-beyond-first-multiarg-method.md


