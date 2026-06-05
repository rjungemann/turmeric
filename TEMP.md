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

  ## Phase 4 -- Stdlib hardening

  5.  `reported/poly-to-fat-drops-args-beyond-first-multiarg-method.md`
      - Kind: report
      - Goal: `tur_poly_fn_t` is unary by construction; multi-arg method
        dispatch silently drops args >= 2 through the poly->fat shim
      - Deps: independent (same family as 10 / 14)
      - Status: In Progress (severity: high -- silent miscompile)

  24. `upcoming/stdlib-session-typed-channels-plan.md`
      - Kind: plan
      - Goal: Thin generic `SChan<p>` wrapper over (opaque, linear) `Chan`
        that carries a protocol phantom advanced by each send/recv/close.
        Worker-pool and RPC-pipe protocols become compile-time checks
      - Deps: 21, 23, plus generic-defopaque compiler support (plan writes
        `(defopaque SChan p ptr<void> :linear)` but no stdlib defopaque
        carries type parameters today and the parser doesn't accept them;
        stacks 23's `:linear`-on-defopaque gap with a separate
        "parameterised defopaque" gap)
      - Status: In Progress -- blocked on 23 + generic-defopaque infra;
        draft otherwise

  26. `upcoming/stdlib-refinement-collections-plan.md`
      - Kind: plan
      - Goal: Replace partial functions in list/vec/slice with total
        versions guarded by `NonEmpty<A>` and `BoundedIdx<n>`. Rewrite
        `range.tur`'s bound-kind sentinel ints as a `Bound A` GADT. Smart
        constructors keep existing partial APIs working
      - Deps: generic-defopaque compiler support (shares 24's blocker --
        plan writes `(defopaque NonEmpty A int)` and
        `(defopaque BoundedIdx n int)`; parameterised defopaque is not
        supported). The `(defadt Bound A ...)` half is fine -- `defdata`
        already takes type params (`stdlib/free.tur:21`). Otherwise
        independent; sized.tur / sized-buf.tur / gadt-vec.tur are in tree
        (SZ0 complete)
      - Status: Complete -- may be partially blocked on generic-defopaque
        infra; the `Bound A` GADT rewrite portion is unblocked

  27. `upcoming/stdlib-hkt-consolidation-plan.md`
      - Kind: plan
      - Goal: Delete hand-rolled monad interfaces in parsec/logic/backtrack
        via real `Monad`/`Alternative` instances. Add `Bifunctor` /
        `MonadError` on `Result` so httpd/csv/json stop open-coding
        `result-map`. Pure consolidation -- no new typeclass hierarchies
      - Deps: independent (HKT phases S1-S8 are complete per
        `project_hkt_phase.md`; every instance has a hand-written analogue
        already in tree)
      - Status: In Progress (split from former umbrella
        stdlib-advanced-typing plan)

  28. `reported/io-file-open-untyped-params-default-to-int.md`
      - Kind: report
      - Goal: `io/file-open`, `read-file`, `write-file`, `file-exists?` in
        `stdlib/io.tur` declare params without type annotations; checker
        resolves unannotated params to `:int`, so the documented `:cstr`
        path call fails `TUR-E0001`. Public linear-resource `FileHandle`
        open path unusable with a real string path
      - Deps: 21 (stdlib opaque-handle territory; one-line `: cstr`
        annotations close it)
      - Status: Complete (severity: high -- documented API unusable;
        latent because no fixture loads it)

  29. `reported/taskgroup-wrapper-macros-emit-nil-head.md`
      - Kind: report
      - Goal: All `task-group-*` convenience macros in
        `stdlib/taskgroup.tur` (`task-group-with`, `-with-timeout`,
        `-with-cancellation`, `task-group-async`) expand to a `(nil ...)`
        head and fail with "expression in call head has type 'nil', which
        is not callable". Hard compile error; entire documented
        structured-concurrency macro surface unusable. Latent (no fixture
        exercises them)
      - Deps: independent (surfaced 2026-06-03 while typing
        TaskGroup/TaskHandle handles for 21)
      - Status: Complete (severity: high)

  30. `reported/inline-c-hardcoded-mangled-names.md`
      - Kind: report
      - Goal: Several stdlib inline-C blocks call sibling defns by spelling
        out their mangled C identifier as a string literal (e.g.
        `tur_int_carrier_eq_`, `httpd_resp_body_`). Silently couples stdlib
        source to the exact name-mangling scheme; any mangling change
        breaks them with no Turmeric-level signal until `cc` fails.
        Near-miss: only signal that `httpd-mw-cors-is-preflight` calls the
        predicate `httpd-req-header?` (not the accessor
        `httpd-req-header`) was a trailing `_` in the hand-written mangled
        name
      - Deps: independent (related to 32's mangling work; the better fix
        per 32's open-question #1 is a name-reference splice rather than
        self-delimiting mangling)
      - Status: Complete (severity: medium -- ergonomics gap / latent
        fragility)

  ## Phase 5 -- Stdlib API cleanup

  32. `upcoming/stdlib-type-erasure-cleanup-plan.md`
      - Kind: plan
      - Goal: Replace int64-erased typeclass stubs with real instances
      - Deps: 7, 19 (plus operator-name mangling fix tracked inside the
        plan; see 30)
      - Status: Complete -- Phase A complete; Phases B and C are WIP

      - A4 Effect-handler closure capture — not started

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

  ## Other notes

  One cosmetic note: re-canonicalizing surfaced some pre-existing top-level
  blank-line/section-comment shuffling in httpd-compress.tur (e.g. an extra
  blank before a docstring'd defn). I confirmed it's lossless, idempotent,
  and that gendocs.py still associates those docstrings (blank lines don't
  reset its doc buffer). Tightening that top-level blank-line handling
  would be a separate, larger change -- happy to take it on if you'd like
  it cleaner.

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

