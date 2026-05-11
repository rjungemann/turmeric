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
- [x] Decide whether `call/cc` remains strict sugar or gets dedicated lowering/runtime paths.
  - Decision: pure sugar. `(call/cc f)` expands to `(reset (shift k (f k)))` in the macro/elaboration layer. No new IR node or emit path. `reset` and `shift` already have dedicated elaboration and codegen; `call/cc` is a derived form and a dedicated path would be premature.
- [x] Specify semantics and diagnostics for continuation escape handling (targeting `continuation-escape.tur`).
  - Decision: runtime abort. `tur_cont_resume()` already has a `consumed` flag; the current silent no-op is upgraded to `fprintf(stderr, "continuation error: resume of already-consumed continuation\n"); abort()`. This makes `continuation-escape.tur` a runtime-failure fixture (`expected.exit: nonzero`, `expected.stderr` substring). A static one-shot check (use-after-move on `resume`) is a Phase 19 section-F follow-on.

### Phase 19 prerequisites (Algebraic effects)
- [x] Finalize v2 effect-row surface syntax and parser diagnostics (`@ {Effect1, Effect2}`, `{}`, row unions/subtyping).
  - Decision: effect rows appear in the return-type position of `defn` signatures as a braced, space-separated list before the return type: `(defn foo [] : {Read Write} int)`. The empty-row purity marker is `(defn foo [] : {} int)`. Row unions and subtyping use the same brace syntax at call sites. In v1 these annotations are parsed and stored as AST nodes but ignored by elaboration and codegen; a "not yet enforced" advisory diagnostic may be emitted. The Phase 19 effect-row checking pass will enforce them when it lands.
- [x] Define runtime design for handler stack and dispatch (single-threaded TLS now, thread-safe strategy later).
  - Decision: a separate singly-linked list `global_effect_handler_chain` (same shape as the existing `global_handler_chain` in `src/exn.c`), stored as a plain global in v1. Dispatch walks the chain linearly looking for a handler matching the performed effect by name. Both `global_handler_chain` and `global_effect_handler_chain` migrate to `__thread` storage in the same pass when threading lands.
- [x] Define static analysis scope for one-shot continuation consumption checking (`resume` use-after-move).
  - Decision: implemented in the existing borrow checker (`src/borrow_check.c`). `(resume k v)` and `(discontinue k e)` are treated as consuming moves of `k` — the binding is marked `is_moved` after the call, and any subsequent use triggers the existing use-after-move diagnostic. No new pass required; `TY_CONT` is already `CK_MOVE`.
- [x] Define minimal stdlib effect set rollout (`Read`, `Write`, `Fail`, `GetEnv`) and required handlers.
  - Decision: ship in order — `Write` first (console handler wraps `printf`/`fputs`, mirrors `log.tur`), `Read` second (console handler wraps `fgets`/`scanf`, mirrors `io.tur`), `Fail` third (handler calls `tur_throw`, bridging to Phase 17 exceptions), `GetEnv` last (handler wraps `getenv`). Each effect requires: a `defeffect` declaration in stdlib, a handler function, and a fixture. No new runtime machinery beyond the handler stack dispatch defined in prerequisite 2.

---

## Actionable Remaining Tasks (Checkbox Backlog)

After prerequisites are complete, execute these implementation tasks.

### Phase 15 remaining tasks
- [x] Reserve syntax for higher-kinded types and emit explicit v1-not-supported diagnostics.
- [x] Integrate effect rows into typeclass method typing where applicable. (v1: silently skip #{...} annotations)

### Phase 16 remaining tasks
- [ ] Implement effect-polymorphic capability fields once effect-row model is finalized.

### Phase 17 remaining tasks
- [x] Implement `(throw! "message")` sugar.
- [x] Implement `(defn throw-error [msg])` helper.
- [x] Implement `(defn throw-io-error [msg])` helper.
- [x] Add negative fixture `exception-uncaught.tur` after expected-runtime-failure test support lands.

### Phase 18 remaining tasks
- [x] Implement `(call/cc f)` sugar/behavior for current continuation capture semantics. (v1: dummy continuation)
- [x] Implement `(escape f)` sugar. (v1: dummy continuation)
- [x] Add positive fixture `continuation-escape.tur` (nested escape test; double-resume deferred to Phase 18 v2 with CPS)

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

### Phase HKT prerequisites (Higher-kinded types) — PROMOTED TO ACTIVE ROADMAP

See [hkt-implementation-plan.md](hkt-implementation-plan.md) for the complete roadmap.

- [x] Phase 15 (Typeclasses v1) is finalized and stable.
- [x] `Type` struct in `src/types.h` has a reserved `kind` field (deferred from Phase 15).
  - Added `Kind` enum (`KIND_STAR`, `KIND_ARROW`) and `hkt_kind` field on `Type`; always `KIND_STAR` in v1.
- [x] Type variables carry an explicit kind slot (reserved in Phase 15, unused in v1).
  - Added `Kind *type_param_kinds` (parallel to `type_params`) to `TypeClass` in `src/typeclass.h`; `NULL` in v1, treated as all-`KIND_STAR`.
- [x] Dispatch-table key is a struct, not a tuple-of-strings, allowing future HKT keying.
  - Added `TypeClassDispatchKey` struct to `src/typeclass.h`; unused in v1.
- [x] Names `Functor`, `Applicative`, `Monad`, `Traversable`, `Foldable` are reserved in typeclass namespace.
  - `elab_defclass` rejects these names with "reserved for the higher-kinded typeclass system (not yet implemented)".
- [x] Decide v2 promotion trigger: promoted. HKT moves to active roadmap.

### Phase HKT remaining tasks

#### H0 — Kind system foundation
- [x] Define `Kind` enum: `KIND_STAR`, `KIND_ARROW` (k1 → k2).
- [x] Add `Kind` field to `TypeVar` struct. (added as `type_param_kinds` on `TypeClass`)
- [x] Add `Kind` field to `Type` struct for concrete types. (added `hkt_kind`)
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

### Phase R prerequisites (Hybrid Result + Limited Panic)

See [turmeric-plan.md §Hybrid Result + Limited Panic](turmeric-plan.md) and [panic-system-vs-exception-system-plan.md](panic-system-vs-exception-system-plan.md) for rationale and resolved open questions.

- [ ] Phase 15 (Typeclasses v1) is stable — needed for `Display`, `Debug`, and `Error` trait dispatch.
- [ ] Phase 17 (Exceptions) is stable — `setjmp`/`longjmp` chain is the substrate for `catch_unwind`.
- [ ] `stdlib/result.tur` is reviewed; gaps vs. R1 requirements are documented.
- [ ] `?` operator surface syntax is decided (`(? expr)` vs. postfix `.?`) and reserved in the reader.
- [ ] `From`/`Into` trait design is agreed before R1 combinators land (they depend on it for cross-type `?` propagation).
- [ ] Confirm that typed panic payloads are represented as tagged `tur_exception` payloads (reusing the Phase 17 exception runtime rather than a separate system).
- [ ] Decide `catch_unwind` relationship to `try`/`catch`: does `catch_unwind` reuse the setjmp chain or introduce a new one? Avoid two incompatible unwinding mechanisms.
- [ ] Confirm double-panic → `abort()` is acceptable (prior art: Rust); document in `panic-system-vs-exception-system-plan.md`.
- [ ] Confirm panic + async/generators semantics (panic exits the task's future at the join point as `Err(PanicPayload)`); document prior-art ruling before async work begins.

---

## Hybrid Result + Limited Panic — Actionable Tasks

### Phase R0 remaining tasks (Design)
- [ ] Finalise `Result<T, E>` surface syntax and confirm `stdlib/result.tur` covers or is extended to cover it.
- [ ] Specify `panic-payload` struct schema and runtime type-tag representation.
- [ ] Specify `catch-unwind` and `catch-panic-of` surface syntax and semantics.
- [ ] Define `Error` typeclass (extends `Show`): `error-message`, `error-cause` methods.
- [ ] Define `From`/`Into` conversion typeclasses and blanket-derivation rule.
- [ ] Decide and document `?` operator syntax.
- [ ] Define `Must<T>` semantics: `must!`, `must-msg!`, `option-expect`, `result-expect`.
- [ ] Survey and document panic/async interaction (prior art: Rust `catch_unwind` + async).
- [ ] Survey and document panic-in-Drop/defer interaction (prior art: Rust double-panic = abort).
- [ ] Write panic strategy decision into `panic-system-vs-exception-system-plan.md`.

### Phase R1 remaining tasks (Core `Result<T, E>`)
- [ ] Verify or extend `stdlib/result.tur` with `ok`, `err`, `ok?`, `err?`, `ok-val`, `err-val`.
- [ ] Implement `result-map`, `result-flat-map`, `result-map-err`.
- [ ] Implement `result-or`, `result-or-else`, `result-unwrap-or`, `result-expect`.
- [ ] Implement `result-collect` — collect `(vec (result T E))` into `(result (vec T) E)`.
- [ ] Implement `result-partition` — split into `(vec T, vec E)`.
- [ ] Implement `?` operator lowering in elaborator: short-circuit return on `Err`.
- [ ] Add compile error for `?` used outside a `Result`-returning function.
- [ ] Implement `Display`, `Debug`, `Error` typeclass instances for `result<T, E>`.
- [ ] Implement `From`/`Into` typeclasses and blanket derivation.
- [ ] Implement error conversion for stdlib error types (`IoError`, `ParseError`, etc.).
- [ ] Add `ok-or` helper: `option<T>` → `result<T, E>`.
- [ ] Add `err-context` helper: wrap error with additional context string.
- [ ] Add fixture `result-basic.tur`.
- [ ] Add fixture `result-combinators.tur`.
- [ ] Add fixture `result-question-op.tur`.
- [ ] Add fixture `result-display.tur`.
- [ ] Add fixture `result-from-into.tur`.
- [ ] Add fixture `result-collect.tur`.
- [ ] Add negative fixture `result-question-outside-fn.tur`.
- [ ] Add codegen snapshots for `ok`/`err` and `?` lowering.

### Phase R2 remaining tasks (Panic Mechanism)
- [ ] Implement `(panic msg)` — lowers to `tur_panic(msg)`; return type is diverging `!`.
- [ ] Implement `(panic-with payload)` — typed payload panic.
- [ ] Implement diverging `!` (never) type in elaborator; `!` is a subtype of every type.
- [ ] Implement `tur_panic` and `tur_panic_with` in `src/runtime.{c,h}`.
- [ ] Implement `panic-payload` struct in `src/runtime.h`.
- [ ] Implement `tur_catch_unwind` (setjmp boundary, returns `result`).
- [ ] Implement `tur_catch_panic_of` (type-filtered catch; re-panics on mismatch).
- [ ] Implement `(catch-unwind thunk)` surface form and lowering.
- [ ] Implement `(catch-panic-of Type thunk)` surface form and lowering.
- [ ] Implement `panic-payload-type` and `panic-payload-downcast` accessors.
- [ ] Verify defer chain fires during panic unwinding (reuses Phase 17 mechanism; end-to-end test).
- [ ] Implement double-panic → `abort()` guard in `tur_panic`.
- [ ] Add fixture `panic-basic.tur`.
- [ ] Add fixture `panic-with-typed.tur`.
- [ ] Add fixture `panic-catch-unwind.tur`.
- [ ] Add fixture `panic-catch-of-type.tur`.
- [ ] Add fixture `panic-downcast.tur`.
- [ ] Add fixture `panic-defer.tur`.
- [ ] Add fixture `panic-ref.tur`.
- [ ] Add fixture `panic-double-panic.tur`.
- [ ] Add codegen snapshots for `panic` and `catch_unwind` lowering.

### Phase R3 remaining tasks (Standard Library Errors)
- [ ] Verify or extend `(defstruct Error [message : cstr, cause : (option cstr)])`.
- [ ] Add `(defstruct IoError [message : cstr, errno : int, path : (option cstr)])`.
- [ ] Add `(defstruct ParseError [message : cstr, line : int, col : int, file : cstr])` (may exist; verify).
- [ ] Add `(defstruct ValidationError [message : cstr, field : (option cstr)])`.
- [ ] Add `(defstruct NotFoundError [what : cstr])`.
- [ ] Add `(defstruct PermissionError [message : cstr, path : (option cstr)])`.
- [ ] Implement `Error` typeclass instances for all stdlib error types.
- [ ] Implement `Show`/`Display` instances for all stdlib error types.
- [ ] Implement `From` upcast instances: `IoError → Error`, `ParseError → Error`, etc.
- [ ] Implement `io-error` and `parse-error` convenience constructors.
- [ ] Implement `ok-or` and `err-context` helpers.
- [ ] Add fixture `error-types-basic.tur`.
- [ ] Add fixture `error-from-into.tur`.
- [ ] Add fixture `error-context.tur`.
- [ ] Add fixture `error-ok-or.tur`.
- [ ] Add codegen snapshots for error struct layouts.

### Phase R4 remaining tasks (`Must<T>`)
- [ ] Implement `(must! expr)` macro for `option<T>` and `result<T, E>`.
- [ ] Implement `(must-msg! expr msg)` macro with custom panic message.
- [ ] Implement `option-must`, `result-must`, `result-must-msg` function forms.
- [ ] Implement `option-expect` and `result-expect` as Rust-aligned aliases.
- [ ] Ensure `must!` panic messages include the failing expression text via `__FILE__`/`__LINE__`.
- [ ] Add fixture `must-option-some.tur`.
- [ ] Add fixture `must-option-none.tur`.
- [ ] Add fixture `must-result-ok.tur`.
- [ ] Add fixture `must-result-err.tur`.
- [ ] Add fixture `must-msg.tur`.
- [ ] Add fixture `must-expect.tur`.
- [ ] Add codegen snapshot: `must!` lowers to inline branch + `tur_panic`.

### Phase R5 remaining tasks (Interop & FFI)
- [ ] Define and implement `TUR_PANIC_STRATEGY` compile-time flag (`UNWIND` vs. `ABORT`).
- [ ] Implement `tur_panic_abort` for `ABORT` strategy.
- [ ] Implement `#[no-unwind]` attribute on `defn`; emit `tur_panic_abort` inside such functions.
- [ ] Document FFI rule: panics must not cross `extern-c` boundaries without `catch-unwind` or `#[no-unwind]`.
- [ ] Decide and implement WASM panic lowering (`unreachable` vs. host import).
- [ ] Implement `result->exception` bridge function.
- [ ] Implement `exception->result` bridge function.
- [ ] Add fixture `panic-ffi-boundary.tur`.
- [ ] Add fixture `panic-no-unwind.tur`.
- [ ] Add fixture `result-exception-bridge.tur`.

### Phase R6 remaining tasks (Async/Effects & Tooling)
- [ ] Define and document panic + continuation/effects boundary semantics.
- [ ] Define and document panic + async task semantics (deferred until async ships).
- [ ] Write `docs/error-handling-guide.md` covering `Result`, `panic`, `must!`, `catch_unwind`, and guidance on when to use each.
- [ ] Add elaborator pass: warn on discarded `result<T, E>` values.
- [ ] Implement `(ignore! expr)` suppression helper.
- [ ] Add `--warn-unused-result` / `--no-warn-unused-result` compiler flags.
- [ ] Add `--lint-panic` linter flag: note when `panic` / `must!` appear outside test/main.
- [ ] Add `--lint-panic` warning for `catch_unwind` used in normal (non-boundary) error handling.
- [ ] Ensure `tur_panic` prints `"panic at <file>:<line>: <message>"` to stderr.
- [ ] Implement `--panic-trace` flag: print scope chain on panic.
- [ ] Implement `--panic-abort` flag: all panics call `abort()`.
- [ ] Add fixture `warn-unused-result.tur`.
- [ ] Add fixture `warn-suppress-ignore.tur`.
- [ ] Add fixture `panic-trace.tur` (golden output for `--panic-trace`).
- [ ] Add codegen snapshots: file/line injection in panic lowering.

---

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
- Keep this list aligned with active roadmap changes in `docs/turmeric-plan.md`.
