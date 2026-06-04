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

6. `upcoming/fn-type-bare-identifier-plan.md`
   - Kind: plan
   - Goal: Drop leading `:` on types inside `(fn [...] ...)` type expressions -- `(fn [:float] :float)` → `(fn [float] float)`. Position already implies type; the colon is redundant noise. Parser-lenient transition then deprecation then removal, matching the spaced-type-annotation cadence
   - Deps: 5
   - Status: Complete

## Phase 1 -- Compiler & closure ABI fixes

7. `upcoming/closure-returning-instance-method-codegen-plan.md`
   - Kind: plan
   - Goal: Fix dict-field type resolution (void* -> int64_t) for closure-returning instance methods
   - Deps: independent
   - Status: Complete

8. `upcoming/bare-fat-result-type-inference-plan.md`
   - Kind: plan
   - Goal: Infer result type for bare-^fat closures so they can return non-int (`:float`, etc.)
   - Deps: independent (external closure-rep prereqs not in this set)
   - Status: Complete - Phase B is a bigger change that can be done another time

9. `upcoming/bare-fat-lambda-param-plan.md`
   - Kind: plan
   - Goal: Allow bare `^fat g` parameter binders on `(fn ...)` lambdas (already supported on top-level `defn`), unblocking the lambda-retype path for closure-returning instance methods
   - Deps: 7, 8
   - Status: Complete (found 2026-06-03 while wiring closure-returning instance method retype)

10. `upcoming/poly-to-fat-typed-shim-plan.md`
    - Kind: plan
    - Goal: Generalize poly-to-fat shim for non-int64 typeclass method signatures
    - Deps: 8
    - Status: Complete

11. `reported/nested-closure-transitive-capture.md`
    - Kind: report
    - Goal: Closures nested two levels deep that reference a grandparent-scoped variable never thread it through the middle closure's env -- generated C references an undeclared local
    - Deps: independent
    - Status: Complete

12. `reported/intra-instance-method-dispatch-unsupported.md`
    - Kind: report
    - Goal: `(.other self ...)` from inside another method's body of the same instance fails elaboration with "no typeclass method found"; external dispatch on the receiver works
    - Deps: independent (touches dispatch logic adjacent to 7)
    - Status: Complete

13. `reported/fn-typed-return-lowered-to-result-type.md`
    - Kind: report
    - Goal: A `defn` whose declared return type is `(fn [...] T)` emits a C signature using the inner `T` instead of a closure-box pointer -- register-class miscompile risk (e.g. `(fn [:float] :float)` → `double` while body returns `void *`)
    - Deps: independent (surfaced by 20 / G2 spike)
    - Status: Complete

14. `reported/poly-defn-shares-inner-closure-body-across-monomorphizations.md`
    - Kind: report
    - Goal: A polymorphic `(defn f [A] ... (fn ... : A val))` emits one shared inner C body returning `int64_t`; the `:float` specialisation invokes it through a `double (*)(...)` pointer and reads the parameter (xmm0) instead of the captured value -- silent miscompile
    - Deps: independent (surfaced by 20 / G2 spike; same family as 8 and 13)
    - Status: Complete

15. `reported/fat-closure-dispatch-does-not-handle-struct-return.md`
    - Kind: report
    - Goal: A closure declared `: (Pair float float)` emits an inner body returning the struct directly but a dispatcher that casts to `int64_t (*)(...)`; `cc` rejects the generated C outright. Typed-thunk family covers `:float`/`:cstr` but not aggregates
    - Deps: independent (surfaced by 20 / G4 spike; same family as 8 but for aggregate returns)
    - Status: Complete

16. `reported/instance-method-closure-return-lowered-to-result-type.md`
    - Kind: report
    - Goal: A `definstance` method declared `(fn [...] T)` lowers BOTH its dict-field type AND its C impl signature to the function's *result* type `T`, not a function-pointer type. Impl body returns a bare `T (*)(...)`. For non-`int64_t`-compatible `T` (e.g. `:float`) this is a hard `cc` error; for `int64_t`-compatible `T` (e.g. `:int`) it "works by luck" with `-Wint-conversion`. Instance-method mirror of 13
    - Deps: independent (adjacent to 7; found 2026-06-04 while fixing 13)
    - Status: Complete

17. `reported/curried-fn-typed-param-second-application-not-callable.md`
    - Kind: report
    - Goal: A parameter typed `(fn [A] (fn [B] C))` loses the inner fn type at the application site -- `(f 1)` type-checks but `((f 1) 2)` fails `TUR-E0002: ... returns ?, which is not callable`. Hard error (not a miscompile), but blocks an entire class of curried higher-order signatures, forcing the `:ptr<void>` + annotated-`^fat` workaround
    - Deps: 6 (surfaced 2026-06-04 while implementing the curried fixture case of fn-type-bare-identifier-plan)
    - Status: Complete

18. `reported/stale-fn-gensym-snapshots-on-main.md`
    - Kind: report
    - Goal: Two `expected.c` snapshots (`fn-typed-return-thin-closure`, `closure-transitive-grandparent-capture`) committed stale on `main` with a uniform off-by-one `__fn_<N>` gensym shift -- byte-identical otherwise but `tests/run.sh` reports "codegen mismatch" FAILs on a clean checkout. Points at a snapshot-generation hygiene gap
    - Deps: independent
    - Status: Complete - Fixed in-session (snapshots regenerated); filed for the record -- severity: low

## Phase 2 -- Type system expansion

19. `upcoming/sum-types-either-plan.md`
    - Kind: plan
    - Goal: Land binary sum types (`Either L R`) with constructors, pattern matching, exhaustiveness, typeclass support
    - Deps: independent
    - Status: Complete

## Phase 3 -- Readiness investigation

20. `upcoming/language-readiness-for-typed-signal-plan.md`
    - Kind: plan
    - Goal: Spike-validate that the type system can support a typed signal library; gate remaining gaps
    - Deps: 4, 7, 8, 10, 19
    - Status: Complete (G1, G3, G5, G6, G7, G8 green; G2 red → 14; G4 red → 15)

## Phase 4 -- Stdlib hardening

21. `upcoming/stdlib-opaque-handle-types-plan.md`
    - Kind: plan
    - Goal: Wrap bare `:int`/`:ptr<void>` resource handles in `defopaque` newtypes
    - Deps: 1
    - Status: Complete

22. `upcoming/stdlib-inline-c-deworkaround-plan.md`
    - Kind: plan
    - Goal: Replace inline-C workarounds in stdlib with idiomatic Turmeric
    - Deps: 21
    - Status: In progress (Phase 1 complete as of 2026-06-03; Phases 2-4 outstanding)

23. `upcoming/stdlib-linearity-affinity-plan.md`
    - Kind: plan
    - Goal: Promote stdlib resource-handle newtypes (Mutex, Chan, Promise, Future, TaskGroup, Reactor, TmpFile, ChildHandle, Bytes) to `:linear` / `:affine`. Catches double-free, use-after-free, missing-wait, and double-fulfill at compile time. Phases L1 (concurrency core) / L2 (taskgroup+reactor) / L3 (audit+serial)
    - Deps: 21, **plus an unwritten compiler-infra plan**: `elab_defopaque` (src/compiler/elab_structs.c:964) only accepts `(defopaque Name :base)` and hardcodes `is_copy = true` -- it silently ignores the `:linear` the plan writes. `:affine` as a type-def annotation does not exist at all (`is_affine` only flows from `^affine` param binders and effect-handler continuations). Inline-C ops can't hold linear params, and no borrow story exists for `Mutex`/`Chan` callers. The current linear-handle examples (`FileHandle`, `Socket`, `MutexGuard`) all use `defstruct :linear`, not `defopaque`; switching the concurrency core to defopaque first would erase their existing linearity discipline
    - Status: Complete - May be blocked on compiler infra (defopaque `:linear`/`:affine` parsing + propagation + borrow rules); plan is otherwise draft

24. `upcoming/stdlib-session-typed-channels-plan.md`
    - Kind: plan
    - Goal: Thin generic `SChan<p>` wrapper over (opaque, linear) `Chan` that carries a protocol phantom advanced by each send/recv/close. Worker-pool and RPC-pipe protocols become compile-time checks
    - Deps: 21, 23, **plus generic-defopaque compiler support**: the plan writes `(defopaque SChan p ptr<void> :linear)`, but no stdlib defopaque carries type parameters today and the parser doesn't accept them. Stacks 23's `:linear`-on-defopaque gap with a separate "parameterised defopaque" gap
    - Status: Blocked on 23 + generic-defopaque infra; draft otherwise

25. `upcoming/stdlib-effect-rows-plan.md`
    - Kind: plan
    - Goal: Annotate I/O-touching stdlib (fs, process, env, net, random, log, time, httpd, csv, json) with coarse effect tags (#{FS}, #{Net}, #{Proc}, #{Rand}). One-pass annotation -- no inference, no masking, opt-in checking
    - Deps: independent (the new tags and `requires-effect-checking` directive don't exist yet, but the plan owns introducing them in E1, so no external prereq)
    - Status: Complete

26. `upcoming/stdlib-refinement-collections-plan.md`
    - Kind: plan
    - Goal: Replace partial functions in list/vec/slice with total versions guarded by `NonEmpty<A>` and `BoundedIdx<n>`. Rewrite `range.tur`'s bound-kind sentinel ints as a `Bound A` GADT. Smart constructors keep existing partial APIs working
    - Deps: **generic-defopaque compiler support** (shares 24's blocker -- the plan writes `(defopaque NonEmpty A int)` and `(defopaque BoundedIdx n int)`; parameterised defopaque is not supported). The `(defadt Bound A ...)` half is fine -- `defdata` already takes type params (`stdlib/free.tur:21`). Otherwise independent; sized.tur / sized-buf.tur / gadt-vec.tur are in tree (memory: SZ0 complete)
    - Status: In Progress - May be partially blocked on generic-defopaque infra; the `Bound A` GADT rewrite portion is unblocked

27. `upcoming/stdlib-hkt-consolidation-plan.md`
    - Kind: plan
    - Goal: Delete hand-rolled monad interfaces in parsec/logic/backtrack via real `Monad`/`Alternative` instances. Add `Bifunctor`/`MonadError` on `Result` so httpd/csv/json stop open-coding `result-map`. Pure consolidation -- no new typeclass hierarchies
    - Deps: independent (HKT phases S1-S8 are complete per `project_hkt_phase.md`; every instance has a hand-written analogue already in tree)
    - Status: Draft (split from former umbrella stdlib-advanced-typing plan)

28. `reported/io-file-open-untyped-params-default-to-int.md`
    - Kind: report
    - Goal: `io/file-open`, `read-file`, `write-file`, `file-exists?` in `stdlib/io.tur` declare params without type annotations; the checker resolves unannotated params to `:int`, so the documented `:cstr` path call fails `TUR-E0001`. The public, linear-resource `FileHandle` open path is unusable with a real string path
    - Deps: 21 (lives in stdlib opaque-handle territory; one-line `: cstr` annotations close it)
    - Status: Complete (severity: high -- documented API unusable; latent because no fixture loads it)

29. `reported/taskgroup-wrapper-macros-emit-nil-head.md`
    - Kind: report
    - Goal: All `task-group-*` convenience macros in `stdlib/taskgroup.tur` (`task-group-with`, `-with-timeout`, `-with-cancellation`, `task-group-async`) expand to a `(nil ...)` head and fail with `expression in call head has type 'nil', which is not callable`. Hard compile error; entire documented structured-concurrency macro surface unusable. Latent (no fixture exercises them)
    - Deps: independent (surfaced 2026-06-03 while typing TaskGroup/TaskHandle handles for 21)
    - Status: Open (severity: high)

30. `reported/inline-c-hardcoded-mangled-names.md`
    - Kind: report
    - Goal: Several stdlib inline-C blocks call sibling defns by spelling out their *mangled* C identifier as a string literal (e.g. `tur_int_carrier_eq_`, `httpd_resp_body_`). Silently couples stdlib source to the exact name-mangling scheme; any mangling change breaks them with no Turmeric-level signal until `cc` fails. Near-miss: only signal that `httpd-mw-cors-is-preflight` calls the *predicate* `httpd-req-header?` (not the accessor `httpd-req-header`) was a trailing `_` in the hand-written mangled name
    - Deps: independent (related to 32's mangling work; the better fix per 32's open-question #1 is a name-reference splice rather than self-delimiting mangling)
    - Status: Open (severity: medium -- ergonomics gap / latent fragility)

## Phase 5 -- Stdlib API cleanup

31. `upcoming/stdlib-arrow-scaleback-plan.md`
    - Kind: plan
    - Goal: Remove disabled Arrow typeclass scaffolding; keep only bare-function combinators
    - Deps: 20
    - Status: Complete (mechanical steps defined, awaiting approval)

32. `upcoming/stdlib-type-erasure-cleanup-plan.md`
    - Kind: plan
    - Goal: Replace int64-erased typeclass stubs with real instances
    - Deps: 7, 19 (plus operator-name mangling fix tracked inside the plan; see 30)
    - Status: Complete (Phase A complete; Phases B and C are WIP)

## Phase 6 -- Typeclass reintroduction

33. `upcoming/stdlib-arrow-typeclass-reintroduction-plan.md`
    - Kind: plan
    - Goal: Reintroduce Arrow / ArrowChoice / ArrowLoop typeclasses on `(->)`
    - Deps: 7, 19, 32
    - Status: Draft, blocked on prerequisites (T1-T12 tasks defined)

## Phase 7 -- Signal rebuild

34. `reported/signal-spice-broken-build.md`
    - Kind: report
    - Goal: tur-signal spice does not compile -- references removed `__arrow_call1` plus un-imported stdlib symbols; the existing rebuild plan understates the scope by assuming a half-done `:float` migration baseline
    - Deps: independent (prerequisite finding for 35)
    - Status: Obsolete (severity: high)

35. `upcoming/tur-signal-rebuild-plan.md`
    - Kind: plan
    - Goal: Rebuild the tur-signal spice on top of the modern typed infrastructure
    - Deps: 14, 15, 20, 31, 34
    - Status: Draft, blocked on 14 + 15 (polymorphic `constant` and struct-returning closures) if needed on critical path; otherwise blocked only on 20 + 31 (Phases 1-6 designed)

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

docs/reported/poly-to-fat-drops-args-beyond-first-multiarg-method.md (referenced but no file in tree)

docs/archive/httpd-conn-struct-consolidation-plan.md

* upcoming/stdlib-type-erasure-cleanup-plan.md
  * `-  -> _hy        _  -> __ (or _us)        /  -> _sl`
  * You chose the full reversible scheme; I implemented it but omitted a demangler
because the -///_→_ folding makes the encoding non-self-delimiting (a general
inverse would mis-decode foo_bar→foo|r), and diagnostics already report source
names, not mangled ones. Documented in mangle.h. This resolves the plan's open
question #1.
  * If we ever do want that (pretty cc-error passthrough), the better fix is option
(1) from my report — a name-reference splice so inline-C stops hardcoding
mangled names at all — rather than making the whole scheme self-delimiting just
to enable demangling.

IN PROGRESS docs/reported/stdlib-future-linearity-aliasing.md

IN PROGRESS docs/reported/defgadt-copy-and-shared-bounds.md
docs/reported/defgadt-malformed-pattern-segfault.md
docs/reported/parameterized-defopaque.md
