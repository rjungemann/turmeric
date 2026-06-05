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

## Phase 5 -- Stdlib API cleanup

32. `upcoming/stdlib-type-erasure-cleanup-plan.md`
    - Kind: plan
    - Goal: Replace int64-erased typeclass stubs with real instances
    - Deps: 7, 19 (plus operator-name mangling fix tracked inside the
      plan; see 30)
    - Status: Complete -- Phase B6 deferred

    - B6 deferred

## Phase 6 -- Typeclass reintroduction

33. `upcoming/stdlib-arrow-typeclass-reintroduction-plan.md`
    - Kind: plan
    - Goal: Reintroduce Arrow / ArrowChoice / ArrowLoop typeclasses on
      `(->)`
    - Deps: 7, 19, 32
    - Status: Draft -- blocked on prerequisites (T1-T12 tasks defined)

## Phase 7 -- Signal rebuild

35. `upcoming/tur-signal-rebuild-plan.md`
    - Kind: plan
    - Goal: Rebuild the tur-signal spice on top of the modern typed
      infrastructure
    - Deps: 14, 15, 20, 31, 34
    - Status: Draft -- blocked on 14 + 15 (polymorphic `constant` and
      struct-returning closures) if needed on critical path; otherwise
      blocked only on 20 + 31 (Phases 1-6 designed)

## docs/reported/result-param-order-blocks-functor-monad.md — OPEN

- T1 of stdlib-hkt-consolidation-plan landed (8157f053): Option got
  Functor/Applicative/Monad/Alternative; Result got Bifunctor only.
- MonadError[Result] deferred: Result's `[A=ok B=err]` order conflicts
  with Turmeric's left-biased instance heads (partial application fixes
  the leftmost param, e.g. `(Either E)`). A right-biased `(Result _ E)`
  head is not expressible, so Functor/Monad/MonadError instances on the
  ok arm cannot be written.
- Proposed fix directions (per report):
  1. Add instance-head syntax for fixing a trailing parameter.
  2. Reorder Result to `[B=err A=ok]` (breaking change).
- No resolving commits since filing; gap remains.

## Other notes

One cosmetic note: re-canonicalizing surfaced some pre-existing top-level
blank-line/section-comment shuffling in httpd-compress.tur (e.g. an extra
blank before a docstring'd defn). I confirmed it's lossless, idempotent,
and that gendocs.py still associates those docstrings (blank lines don't
reset its doc buffer). Tightening that top-level blank-line handling
would be a separate, larger change

Investigate Phase B of `upcoming/bare-fat-result-type-inference-plan.md`
later.

`docs/upcoming/stdlib-opaque-handle-types-plan.md` follow-ups:
- stm error fixture -- the STM-block guard (TUR-E0009) fires before
  argument type-checking, so a standalone wrong-handle case can't reach
  the TUR-E0001. The typing is validated by compile + suite.
- tur/ref -- its handle is an `:int` with unannotated (effectively
  polymorphic) params and a Clone instance returning raw `:int`; clean
  newtyping needs more than a signature pass.

`upcoming/stdlib-type-erasure-cleanup-plan.md` follow-ups:
- Mangling scheme: `- -> _hy`, `_ -> __ (or _us)`, `/ -> _sl`
- Full reversible scheme implemented but demangler omitted because the
  `-`/`/`/`_` -> `_` folding makes the encoding non-self-delimiting (a
  general inverse would mis-decode `foo_bar` -> `foo|r`), and diagnostics
  already report source names, not mangled ones. Documented in
  mangle.h. Resolves the plan's open question #1.
- If we ever do want demangling (pretty cc-error passthrough), the
  better fix is option (1) from the report -- a name-reference splice so
  inline-C stops hardcoding mangled names at all -- rather than making
  the whole scheme self-delimiting just to enable demangling.

docs/reported/generic-struct-opaque-element-miscompile.md
- Variant 2 (phantom element type recoverable only from an opaque argument, e.g. generic recv returning (Pair T (SChan R))) is the harder case the report flags as needing phantom-element recovery in emit_abi_instantiate_type (fix direction #3). It's not exercised by any shipping code — the real stdlib/schan.tur recv was already respecified to avoid the shape — so I updated the report to mark variant 1 resolved and variant 2 as a tracked limitation rather than risk a speculative inference change.

Do an inline-c reduction pass for the new typeclass work and related
