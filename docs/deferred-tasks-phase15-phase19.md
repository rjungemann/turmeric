# Deferred Tasks Backlog — Phases 15 through 19

Purpose: track deferred items from Phases 15, 16, 17, 18, and 19, why they were deferred, and what to investigate next.

## Why These Tasks Were Initially Deferred

These tasks were deferred for recurring reasons:

1. MVP-first phase exits
Phases 15 through 19 were marked complete at v1 once core behavior and baseline fixtures were working. v2 features were intentionally postponed to avoid phase creep.

2. Dependency ordering
Many deferred items depend on infrastructure not fully implemented in v1, especially effect-row typing, handler dispatch runtime details, and stronger continuation analysis.

3. Test harness and UX limits
A few deferred fixtures require richer test-runner support for expected runtime-failure scenarios.

---

## Actionable Prerequisite Tasks (Checkbox Backlog)

Use this section first. These items unblock the deferred work below.

### Cross-cutting prerequisites
- [ ] Define ownership and acceptance criteria for all Phase 15-19 follow-up tasks (especially v2-tagged items).
- [ ] Confirm fixture conventions for deferred continuation/effect negative tests (runtime panic vs compile-time error expectations).
- [ ] Decide snapshot scope for effect lowering/runtime integration tests (which files must be golden-checked).
- [ ] Define compatibility policy for new syntax sugar so existing v1 syntax remains stable.

### Phase 15 prerequisites (Typeclasses)
- [ ] Decide v2 design boundary for higher-kinded type syntax reservation and diagnostics (parse-only vs kind-check stub).
- [ ] Define effect-row integration model for typeclass methods before implementing row-aware constraints.

### Phase 16 prerequisites (Capability passing)
- [ ] Define how effect-polymorphic capability fields are represented in function types and dictionaries.
- [ ] Decide whether capability effect polymorphism depends on full Phase 19 effect-row checking or can ship in a minimal subset.

### Phase 17 prerequisites (Exceptions)
- [ ] Finalize sugar design for `throw!`, `throw-error`, and `throw-io-error` to avoid conflict with existing typeclass/stdlib naming.
- [ ] Define test-runner contract for expected uncaught runtime failures so `exception-uncaught` can be automated.

### Phase 18 prerequisites (Delimited continuations)
- [ ] Decide whether `call/cc` remains strict sugar or gets dedicated lowering/runtime paths.
- [ ] Specify semantics and diagnostics for continuation escape handling (targeting `continuation-escape.tur`).

### Phase 19 prerequisites (Algebraic effects)
- [ ] Finalize v2 effect-row surface syntax and parser diagnostics (`@ {Effect1, Effect2}`, `{}`, row unions/subtyping).
- [ ] Define runtime design for handler stack and dispatch (single-threaded TLS now, thread-safe strategy later).
- [ ] Define static analysis scope for one-shot continuation consumption checking (`resume` use-after-move).
- [ ] Define minimal stdlib effect set rollout (`Read`, `Write`, `Fail`, `GetEnv`) and required handlers.

---

## Actionable Remaining Tasks (Checkbox Backlog)

After prerequisites are complete, execute these implementation tasks.

### Phase 15 remaining tasks
- [ ] Reserve syntax for higher-kinded types and emit explicit v1-not-supported diagnostics.
- [ ] Integrate effect rows into typeclass method typing where applicable.

### Phase 16 remaining tasks
- [ ] Implement effect-polymorphic capability fields once effect-row model is finalized.

### Phase 17 remaining tasks
- [ ] Implement `(throw! "message")` sugar.
- [ ] Implement `(defn throw-error [msg])` helper.
- [ ] Implement `(defn throw-io-error [msg])` helper.
- [ ] Add negative fixture `exception-uncaught.tur` after expected-runtime-failure test support lands.

### Phase 18 remaining tasks
- [ ] Implement `(call/cc f)` sugar/behavior for current continuation capture semantics.
- [ ] Implement `(escape f)` sugar.
- [ ] Add negative fixture `continuation-escape.tur` with agreed diagnostics/runtime behavior.

### Phase 19 remaining tasks

#### A) Surface syntax and declaration model
- [ ] Implement `(try-with body handler)` sugar for `(reset (handle body handler))`.
- [ ] Implement effect-row syntax in `defn` signatures.
- [ ] Implement empty-row purity marker (`{}`).
- [ ] Implement effect-row polymorphism.
- [ ] Implement row union propagation at call sites.
- [ ] Implement row-subtyping checks.
- [ ] Add effect scoping controls (module-private/exported effects).
- [ ] Add effect re-opening support.

#### B) Runtime handler pipeline
- [ ] Implement per-fiber handler stack representation.
- [ ] Implement matching handler dispatch walk.

#### C) Effect-row checking pass
- [ ] Add pass scheduling after elaboration and before codegen.
- [ ] Union effect rows per function from call sites.
- [ ] Validate inferred rows against declared rows.
- [ ] Add unhandled-effect diagnostics policy at top level.
- [ ] Implement/decide advisory behavior for effect rows on `extern-c`.

#### D) Handler scoping semantics
- [ ] Implement handler-parameter shadowing behavior.
- [ ] Ensure continuation binding `k` is fresh per handler case.
- [ ] Implement deep-handler continuation capture semantics.

#### E) Stdlib effects and handlers
- [ ] Implement `Read` effect.
- [ ] Implement `Write` effect.
- [ ] Implement `Fail` effect.
- [ ] Implement `GetEnv` effect.
- [ ] Implement console handler for `Read`/`Write`.
- [ ] Implement exception bridge handler for `Fail`.

#### F) Feature interactions and one-shot checks
- [ ] Enable macro-generated effectful code path and hygiene interactions.
- [ ] Implement module-scoped effect handling/linking behavior.
- [ ] Integrate borrow-check constraints for effect handlers that capture references.
- [ ] Add static one-shot check: `resume` consumes continuation and rejects second use.
- [ ] Implement `cont?` predicate.

#### G) Optimizations (post-MVP, optional)
- [ ] Handler inlining when statically known.
- [ ] Monomorphic `perform` fast path.
- [ ] Frame fusion for adjacent non-capturing scopes.
- [ ] Escape analysis for non-escaping scopes.

#### H) Deferred effect fixtures
- [ ] Add `effect-declaration.tur` fixture.
- [ ] Add `effect-handler.tur` fixture.
- [ ] Add `effect-multiple.tur` fixture.
- [ ] Add `effect-nested.tur` fixture.
- [ ] Add `effect-defer.tur` fixture.
- [ ] Add `effect-ref.tur` fixture.
- [ ] Add `effect-rc.tur` fixture.
- [ ] Add `effect-oneshot.tur` fixture.
- [ ] Add `effect-console.tur` fixture.
- [ ] Add `effect-fail.tur` fixture.
- [ ] Add negative fixture `effect-unhandled.tur`.
- [ ] Add negative fixture `effect-double-resume.tur`.

### Phase HKT prerequisites (Higher-kinded types, v2-targeted)

See [hkt-implementation-plan.md](hkt-implementation-plan.md) for the complete roadmap.

- [ ] Phase 15 (Typeclasses v1) is finalized and stable.
- [ ] `Type` struct in `src/types.h` has a reserved `kind` field (deferred from Phase 15).
- [ ] Type variables carry an explicit kind slot (reserved in Phase 15, unused in v1).
- [ ] Dispatch-table key is a struct, not a tuple-of-strings, allowing future HKT keying.
- [ ] Names `Functor`, `Applicative`, `Monad`, `Traversable`, `Foldable` are reserved in typeclass namespace.
- [ ] Decide v2 promotion trigger: if ≥2 of (1) repeated boilerplate, (2) library-author demand, (3) user feedback demand HKTs, promote to active roadmap.

### Phase HKT remaining tasks (if promoted)

#### H0 — Kind system foundation
- [ ] Define `Kind` enum: `KIND_STAR`, `KIND_ARROW` (k1 → k2).
- [ ] Add `Kind` field to `TypeVar` struct.
- [ ] Add `Kind` field to `Type` struct for concrete types.
- [ ] Implement `kind_eq()`, `kind_to_string()`, `kind_parse()`.
- [ ] Implement kind inference pass (`src/kind_check.c`).
- [ ] Reserve kind syntax (`: * -> *` annotations, `^f : * -> *` in `defn`/`defclass`).
- [ ] Error on kind syntax use in v1 mode.
- [ ] Add fixtures: `kinds-basic.tur`, `kinds-inference.tur`, `kinds-error.tur`.

#### H1 — Kind-polymorphic typeclasses
- [ ] Extend `TypeClassParam` to store `Kind` alongside name.
- [ ] Implement kind constraint validation in instances.
- [ ] Support kind syntax in `defclass` and `definstance`.
- [ ] Implement kind-aware constraint propagation.
- [ ] Reserve `Functor`, `Applicative`, `Monad`, `Traversable`, `Foldable` with "not yet defined" diagnostics.
- [ ] Add fixtures: `hkt-typeclass-declare.tur`, `hkt-typeclass-instance.tur`, `hkt-typeclass-kind-error.tur`.

#### H2 — HKT dispatch table
- [ ] Generalize dispatch-table key to `(class_name, [arg_types], constructor_kind)`.
- [ ] Implement two-level lookup: constructor by kind, method by types.
- [ ] Cache dictionary structs per unique key.
- [ ] Ensure no performance regression for kind-`*` code paths.
- [ ] Add fixtures: `hkt-dispatch-basic.tur`, `hkt-dispatch-nested.tur`, `hkt-dispatch-mixed.tur`.

#### H3 — Built-in HKT typeclasses
- [ ] Define `Functor` typeclass with `map` method.
- [ ] Define `Applicative` typeclass (extends `Functor`).
- [ ] Define `Monad` typeclass (extends `Applicative`) with `bind`/`pure`.
- [ ] Define `Traversable` typeclass.
- [ ] Define `Foldable` typeclass.
- [ ] Implement instances for stdlib types (`option`, `vec`, `slice`, `ref`, `rc`).
- [ ] Add fixtures for laws and behavioral tests.

#### H4 — Kind-polymorphic functions
- [ ] Support kind-variable parameters in `defn` signatures.
- [ ] Implement implicit kind inference from usage.
- [ ] Implement kind constraint propagation for typeclass constraints on kind variables.
- [ ] Implement dictionary passing for kind-polymorphic functions.
- [ ] Implement optional monomorphization (`-O` flag) to avoid code bloat.
- [ ] Add fixtures: `hkt-fn-kind-param.tur`, `hkt-fn-implicit-kind.tur`, `hkt-fn-constraints.tur`.

#### H5 — Advanced kinds
- [ ] Support binary type constructors (`* -> * -> *`).
- [ ] Implement partial application: `(result int) : * -> *`.
- [ ] Implement kind aliases (`defkind`).
- [ ] Support higher-kinded data types (e.g., `Fix`, `Free` monad).
- [ ] Add fixtures: `hkt-binary-ctor.tur`, `hkt-kind-alias.tur`, `hkt-recursive-type.tur`, `hkt-free-monad.tur`.

#### H6 — Integration & polish
- [ ] Write user-facing HKT guide: `docs/hkt-guide.md`.
- [ ] Update README with HKT examples.
- [ ] Migrate stdlib to use HKT typeclasses where applicable.
- [ ] Add `do` notation macro for any `Monad`.
- [ ] Add `for` comprehension macro using `Monad`/`Traversable`.
- [ ] Benchmark dictionary passing overhead for HKT code.
- [ ] Add `-O` performance option documentation.
- [ ] Implement `tur explain` support for kind errors.
- [ ] Add `--dump-kinds` debugging flag.
- [ ] Add fixtures for typeclass laws (functor, monad, traversable).
- [ ] Add integration tests: HKTs + closures + defers + refs.
- [ ] Add negative tests: invalid kind usage, orphan instances.

### Closeout tasks
- [ ] Re-run targeted fixtures for Phases 15-19 deferred follow-up work.
- [ ] Re-run relevant codegen snapshots for typeclass/effect/continuation lowering paths.
- [ ] Update `docs/turmeric-plan.md` with promoted or completed deferred items.
- [ ] Add a short completion note to this file once deferred clusters are resolved.

---

## Suggested Execution Order (Later)

1. Cross-cutting prerequisites and acceptance criteria
2. Phase 17 test-runner/runtime-failure support + exception sugar
3. Phase 18 continuation sugar and escape semantics
4. Phase 19 effect-row syntax + checking pass skeleton
5. Phase 19 handler-stack dispatch runtime
6. Phase 19 stdlib effect set and core fixtures
7. Phase 15/16 row-aware typeclass and capability integration
8. Phase 19 one-shot static checks and optional optimizations
9. **[If promoted per decision rule]** Phase HKT H0–H2: kind system and dispatch
10. **[If promoted]** Phase HKT H3–H6: built-in typeclasses, stdlib migration, and polish
11. Final fixture/snapshot sweep and roadmap closeout

---

## Notes

- Phases 15 through 19 are marked complete at v1 in the active plan; this file tracks deferred v2+ follow-up work only.
- Phase HKT (H0–H6 roadmap) is planned for v2+ and will only move to the active roadmap if the promotion decision rule is met (see [hkt-implementation-plan.md](hkt-implementation-plan.md) for details).
- Keep this list aligned with active roadmap changes in `docs/turmeric-plan.md`.
