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
- [x] Define ownership and acceptance criteria for all Phase 15-19 follow-up tasks (especially v2-tagged items).
  - Decision: ownership is the project maintainer. Acceptance criteria for each task: (a) all checklist items in the phase section checked; (b) fixture tests pass; (c) codegen snapshots updated where required; (d) v2-tagged items are accepted only when their upstream dependencies are stable and documented.
- [x] Confirm fixture conventions for deferred continuation/effect negative tests (runtime panic vs compile-time error expectations).
  - Decision: compile-time errors → `expected.diag` in `tests/fixtures/errors/<name>/`; runtime failures → `expected.exit: nonzero` + `expected.stderr` substring(s) in `tests/fixtures/<name>/`. Effect and continuation negative tests that detect static problems (e.g., unhandled `perform` at top level) use the `errors/` subtree; those that detect runtime problems (e.g., double-resume, continuation escape) use `expected.exit: nonzero` + `expected.stderr`.
- [x] Decide snapshot scope for effect lowering/runtime integration tests (which files must be golden-checked).
  - Decision: golden-check `expected.c` for fixtures exercising `defeffect`, `defhandler`, `perform`, and `with-handler` forms. Do NOT golden-check interpreter or runtime output beyond `expected.stdout`. The codegen snapshot file is `expected.c` in the fixture directory, consistent with all other fixture conventions.
- [x] Define compatibility policy for new syntax sugar so existing v1 syntax remains stable.
  - Decision: all new sugar forms are additive. Existing surface syntax never changes semantics between phases. New stdlib forms are introduced with a `;;; SINCE: PhaseN` comment. Breaking changes are prohibited without a deprecation period; deprecated forms require `--allow-deprecated` to compile without a warning.

### Phase 15 prerequisites (Typeclasses)
- [x] Decide v2 design boundary for higher-kinded type syntax reservation and diagnostics (parse-only vs kind-check stub).
  - Decision: parse-only in v1. The `: * -> *` kind syntax and `^f` HKT variable prefix are parsed but all use sites emit a "not yet supported" diagnostic. The elaborator does not attempt kind-checking in v1. The kind-check pass is Phase H0 in the HKT roadmap.
- [x] Define effect-row integration model for typeclass methods before implementing row-aware constraints.
  - Decision: typeclass method signatures carry optional effect-row annotations using the same `{Effect1 Effect2}` return-position syntax as standalone `defn`. In v1, annotations are stored in the AST but ignored by elaboration and codegen. Effect-row enforcement for typeclass methods is a Phase 19 Section C (effect-row checking pass) item, not a Phase 15 item.

### Phase 16 prerequisites (Capability passing)
- [x] Define how effect-polymorphic capability fields are represented in function types and dictionaries.
  - Decision: effect-polymorphic capability fields are stored as function pointers in the capability struct. The function pointer type carries a row variable `{e}` in the signature, which is erased to a plain function pointer at codegen in v1. In the dictionary struct (vtable), each polymorphic method is a function pointer that takes an implicit `void *env` first argument for captured state.
- [x] Decide whether capability effect polymorphism depends on full Phase 19 effect-row checking or can ship in a minimal subset.
  - Decision: depends on Phase 19. Capability effect polymorphism ships as part of Phase 19 Section A (surface syntax and declaration model), not as a standalone Phase 16 feature. Phase 16 ships plain (non-row-polymorphic) capability passing; Phase 19 adds row-variable polymorphism.

### Phase 17 prerequisites (Exceptions)
- [x] Finalize sugar design for `throw!`, `throw-error`, and `throw-io-error` to avoid conflict with existing typeclass/stdlib naming.
  - Decision: `throw!` is a special form handled in the elaborator (`elab_throw_bang` in `src/elab.c`); `throw-error` and `throw-io-error` are stdlib helper functions in `stdlib/exn.tur`. These names do not conflict with typeclass/stdlib naming. Design is final as-implemented; no renaming or refactoring is needed.
- [x] Define test-runner contract for expected uncaught runtime failures so `exception-uncaught` can be automated.
  - Decision: the contract is already defined and implemented in `tests/run.sh`. `expected.exit: nonzero` triggers a nonzero-exit check; `expected.stderr` (one substring per line) triggers stderr substring matching. The `exception-uncaught` fixture already uses this contract. The contract is also documented in `docs/test-runner-contract.md`.

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

### Thread Safety prerequisites (Phases T19–T21)
- [x] Confirm C11 `<threads.h>` availability on all supported platforms (GCC 4.9+, Clang 3.3+, macOS via libc++).
  - Decision: C11 `<threads.h>` is NOT available on macOS (confirmed: Apple Clang 17.0.0 does not ship it). Use POSIX `<pthread.h>` as the threading primitive on all supported platforms. Introduce a `TUR_THREAD_LOCAL` macro in `src/platform.h` that expands to `_Thread_local` on C11-capable compilers and `__thread` as a GNU-extension fallback.
- [x] Decide migration strategy for `global_handler_chain` and `global_effect_handler_chain` to `__thread` TLS storage.
  - Decision: single-pass change — prefix both globals with `TUR_THREAD_LOCAL` in `src/exn.c` and `src/effect.c` respectively. This is gated on the platform.h macro. Migration happens in the same commit that adds `src/platform.h`; no intermediate states.
- [x] Define `Send`/`Sync` marker trait integration boundary: are they typeclass traits, built-in markers, or a hybrid?
  - Decision: built-in marker types, not typeclass traits. `Send` and `Sync` are compiler-checked properties enforced by the elaborator, not implementable by user code. They are automatically derived when all fields satisfy the constraint. User code cannot manually implement `Send`/`Sync` in v1.
- [x] Confirm `Arc<T>` memory layout and whether it reuses `src/rc.{c,h}` or gets a new file.
  - Decision: new file `src/arc.{c,h}`. `Arc<T>` requires `_Atomic uint64_t` for reference counts (vs. plain `uint64_t` in `rc.c`). The control block layout is otherwise identical to `RcControlBlock`. `arc.h` mirrors `rc.h` with an `AtomicRcControlBlock` using `_Atomic` fields and `atomic_fetch_add`/`atomic_fetch_sub` operations.
- [x] Define test runner support for multi-threaded fixtures (determinism, ThreadSanitizer integration).
  - Decision: multi-threaded fixtures may include an `expected.timeout` file (integer seconds; default 10); tests that exceed the timeout fail. ThreadSanitizer is opt-in: set `TUR_TSAN=1` in the build to add `-fsanitize=thread`. Fixtures that require TSAN to be meaningful include an empty `requires.tsan` file; `tests/run.sh` skips these when `TUR_TSAN` is not set.
- [x] Decide `Mutex` poison detection strategy: `Result`-based or exception-based.
  - Decision: `Result`-based, consistent with the Hybrid Result + Limited Panic roadmap. `mutex-lock` returns `(result T MutexPoisonError)`. On panic unwinding through a held mutex the mutex is marked poisoned. `mutex-lock-unchecked` is an `Unsafe` variant that skips poison detection.
- [x] Define linker flag policy: `tur build` should automatically add `-pthread` when thread primitives are used.
  - Decision: the elaborator sets a `needs_pthread` flag on the `Emit` context whenever a thread primitive (`spawn`, `Mutex`, `Arc`, `RwLock`, etc.) is encountered. The final link command in `src/emit.c` appends `-pthread` to the compiler invocation when `needs_pthread` is set. No user action required.

### Unsafe Operations prerequisites (Phases U1–U5)
- [x] Confirm `Unsafe` integrates with Phase 19 effect rows before Phase U1 begins (effect-row infrastructure must be stable).
  - Decision: confirmed. `Unsafe` is a Phase 19 effect, written as `{Unsafe}` in the effect row position. Phase U1 depends on Phase 19 Section A (surface syntax and declaration model) being stable. Phase U1 does not begin until Phase 19 Section A items are complete.
- [x] Define the `;;; SAFETY:` comment convention and whether it is syntactic (parser-recognized) or documentary only.
  - Decision: documentary only. The parser does not recognize `;;; SAFETY:` as syntax. It is a source-code convention for human readers. A future lint pass (post-MVP) may warn on `unsafe` blocks missing a `;;; SAFETY:` comment, but no enforcement in v1.
- [x] Decide whether `unsafe` block size limit is a configurable lint or a hard compiler error.
  - Decision: configurable lint. Default threshold is 20 lines. Exceeding it emits a warning. Promote to error with `--deny unsafe-large-block`. Configure threshold with `--warn-unsafe-block-size=N`. No hard compiler error.
- [x] Confirm `reinterpret`/`transmute` size-equality check is done at compile time for all supported types.
  - Decision: compile-time check in the elaborator for all types with statically-known sizes (all primitives, fixed-size structs, pointer types). Variable-size types (slices, strings) are rejected by `transmute` with a "cannot transmute unsized type" diagnostic. Pointer types always have equal size on a given target (checked via `sizeof` in emitted C).
- [x] Define FFI calling convention documentation requirements for `c-call` and `extern-c`.
  - Decision: documentary only in v1. `c-call` and `extern-c` forms should be preceded by a `;;; FFI:` comment block documenting: the C function name, its C-level signature, calling convention (default: C cdecl), and ownership semantics for any returned pointer. No syntactic enforcement in v1; a future lint pass may enforce presence of `;;; FFI:` comments.

---

## Actionable Remaining Tasks (Checkbox Backlog)

After prerequisites are complete, execute these implementation tasks.

### Phase 15 remaining tasks
- [x] Reserve syntax for higher-kinded types and emit explicit v1-not-supported diagnostics.
  - Implemented: (1) typeclass names `Functor`, `Applicative`, `Monad`, `Traversable`, `Foldable` are reserved in `elab_defclass` with a "reserved for the higher-kinded typeclass system" error; (2) kind annotations `[f :kind]` in `defclass` type parameters emit "kind annotations in defclass type parameters are not yet supported (Phase HKT)"; (3) `tests/fixtures/errors/typeclass-hkt-reserved/` validates (2).
- [x] Integrate effect rows into typeclass method typing where applicable.
  - Implemented: typeclass method signatures accept `#{Effect}` effect-row annotations in the advisory v1 mode (stored in AST, not enforced). Fixture `tests/fixtures/typeclass-effect-row/` passes.

### Phase 16 remaining tasks
- [ ] Implement effect-polymorphic capability fields once effect-row model is finalized.

### Phase 17 remaining tasks
- [x] Implement `(throw! "message")` sugar.
  - Implemented: `elab_throw_bang` in `src/elab.c`; fixture `tests/fixtures/exception-throw-bang/` passes.
- [x] Implement `(defn throw-error [msg])` helper.
  - Implemented: `throw-error` in `stdlib/exn.tur` as sugar for `(throw (Error. msg nil))`.
- [x] Implement `(defn throw-io-error [msg])` helper.
  - Implemented: `throw-io-error` in `stdlib/exn.tur` as sugar for `(throw (IoError. msg 0))`.
- [x] Add negative fixture `exception-uncaught.tur` after expected-runtime-failure test support lands.
  - Fixture `tests/fixtures/exception-uncaught/` exists with `expected.exit: nonzero` and `expected.stderr` substring.

### Phase 18 remaining tasks
- [x] Implement `(call/cc f)` sugar/behavior for current continuation capture semantics.
  - Implemented: `elab_call_cc` in `src/elab.c`; fixture `tests/fixtures/continuation-callcc/` passes.
- [x] Implement `(escape f)` sugar.
  - Implemented: `elab_escape` in `src/elab.c`; fixtures `tests/fixtures/continuation-escape/` and `tests/fixtures/continuation-escape-fn/` pass.
- [x] Add negative fixture `continuation-escape.tur` with agreed diagnostics/runtime behavior.
  - Runtime abort: `tur_cont_resume()` aborts with "continuation error: resume of already-consumed continuation" on double-resume. Static borrow-check: `tests/fixtures/errors/effect-double-resume/` covers the compile-time use-after-move diagnostic (Phase 19 Section F). Runtime abort path is defense-in-depth.

### Phase 19 remaining tasks

#### A) Surface syntax and declaration model
- [x] Implement `(try-with body handler)` sugar for `(reset (handle body handler))`.
  - Implemented: `elab_try_with` in `src/elab.c`; delegates directly to `elab_handle`.
- [x] Implement effect-row syntax in `defn` signatures.
  - Implemented: `{Effect1 Effect2}` brace syntax in return-type position is parsed and stored; advisory "not yet enforced" diagnostic emitted in v1 (see `src/elab.c` lines ~3597, ~3780).
- [x] Implement empty-row purity marker (`{}`).
  - Implemented: `{}` in return-type position is parsed and stored; treated as advisory in v1 alongside the rest of the effect-row annotation.
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
- [x] Add unhandled-effect diagnostics policy at top level. (TUR-E0008: static error for unhandled perform at top level; fn_body_depth exempt)
- [ ] Implement/decide advisory behavior for effect rows on `extern-c`.

#### D) Handler scoping semantics
- [ ] Implement handler-parameter shadowing behavior.
- [ ] Ensure continuation binding `k` is fresh per handler case.
- [ ] Implement deep-handler continuation capture semantics.

#### E) Stdlib effects and handlers
- [x] Implement `Read` effect.
  - Implemented: `(defeffect Read [] :int)` in `stdlib/effects.tur`; fixture `tests/fixtures/effect-with-read-console/` passes.
- [x] Implement `Write` effect.
  - Implemented: `(defeffect Write [s :cstr] :nil)` in `stdlib/effects.tur`; fixture `tests/fixtures/effect-with-write/` passes.
- [x] Implement `Fail` effect.
  - Implemented: `(defeffect Fail [msg :cstr] :nil)` in `stdlib/effects.tur`; fixture `tests/fixtures/effect-with-fail/` passes.
- [x] Implement `GetEnv` effect.
  - Implemented: `(defeffect GetEnv [key :cstr] :cstr)` in `stdlib/effects.tur`; fixture `tests/fixtures/effect-with-getenv/` passes.
- [x] Implement console handler for `Read`/`Write`.
  - Implemented: `with-read-console` and `with-write` macros in `stdlib/effects.tur`.
- [x] Implement exception bridge handler for `Fail`.
  - Implemented: `with-fail-throw` macro in `stdlib/effects.tur`.

#### F) Feature interactions and one-shot checks
- [x] Enable macro-generated effectful code path and hygiene interactions. (with-write, with-fail-throw, with-getenv, with-read-console fixtures demonstrate this)
- [ ] Implement module-scoped effect handling/linking behavior.
- [ ] Integrate borrow-check constraints for effect handlers that capture references.
- [x] Add static one-shot check: `resume` consumes continuation and rejects second use.
  - Implemented in borrow checker (`src/borrow_check.c`): `(resume k v)` marks `k` as moved. Fixture `tests/fixtures/errors/effect-double-resume/` validates the use-after-move diagnostic.
- [x] Implement `cont?` predicate. (EX_CONT_PRED; Phase 18 tur_cont* checks tur_cont_consumed; Phase 19 k always true; effect-cont-pred fixture)

#### G) Optimizations (post-MVP, optional)
- [ ] Handler inlining when statically known.
- [ ] Monomorphic `perform` fast path.
- [ ] Frame fusion for adjacent non-capturing scopes.
- [ ] Escape analysis for non-escaping scopes.

#### H) Deferred effect fixtures
- [x] Add `effect-declaration.tur` fixture.
  - Fixture `tests/fixtures/effect-declaration/` added: `(defeffect Increment [n :int] :int)` with perform and handle; expected output `15`.
- [x] Add `effect-handler.tur` fixture.
  - Fixture `tests/fixtures/effect-handler/` added: two-effect handler with `Add` and `Mul`, parameter binding, multi-clause; expected output `104`.
- [x] Add `effect-multiple.tur` fixture.
  - Covered by `tests/fixtures/effect-multiple/` (Ask + Tell, already passing).
- [x] Add `effect-nested.tur` fixture.
  - Covered by `tests/fixtures/effect-nested/` (nested handle blocks, already passing).
- [x] Add `effect-defer.tur` fixture.
  - Covered by `tests/fixtures/effect-defer/` (perform inside defer body, already passing).
- [x] Add `effect-ref.tur` fixture.
  - Fixture `tests/fixtures/effect-ref/` added: ref live across perform/resume boundary; expected output `142`.
- [x] Add `effect-rc.tur` fixture.
  - Covered by `tests/fixtures/effect-rc/` (rc value live during perform, already passing).
- [x] Add `effect-oneshot.tur` fixture.
  - Fixture `tests/fixtures/effect-oneshot/` added: two sequential performs, each getting an independent k; expected output `30`.
- [x] Add `effect-console.tur` fixture.
  - Fixture `tests/fixtures/effect-console/` added: combined Write + Read handlers with fixed-value Read; expected output `result:\n42`.
- [x] Add `effect-fail.tur` fixture.
  - Covered by `tests/fixtures/effect-with-fail/` (Fail bridged to exception via with-fail-throw, already passing).
- [x] Add negative fixture `effect-unhandled.tur`.
  - Covered by `tests/fixtures/errors/effect-unhandled/` (TUR-E0008 unhandled perform diagnostic, already passing).
- [x] Add negative fixture `effect-double-resume.tur`.
  - Covered by `tests/fixtures/errors/effect-double-resume/` (TUR-E0005 use-after-move on double resume, already passing).

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
- [x] Implement `kind_eq()`, `kind_to_string()`, `kind_parse()`.
- [ ] Implement kind inference pass (`src/kind_check.c`).
- [ ] Reserve kind syntax (`: * -> *` annotations, `^f : * -> *` in `defn`/`defclass`).
- [ ] Error on kind syntax use in v1 mode.
- [x] Add fixtures: `kinds-basic.tur`, `kinds-error.tur`. (`kinds-inference.tur` deferred — requires kind inference pass)

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

See [turmeric-plan.md §Hybrid Result + Limited Panic](turmeric-plan.md) and [panic-system-vs-exception-system-plan.md](archive/panic-system-vs-exception-system-plan.md) for rationale and resolved open questions.

- [x] Phase 15 (Typeclasses v1) is stable — needed for `Display`, `Debug`, and `Error` trait dispatch.
  - Confirmed: Phase 15 is complete; typeclass dispatch is stable and all Phase 15 fixtures pass.
- [x] Phase 17 (Exceptions) is stable — `setjmp`/`longjmp` chain is the substrate for `catch_unwind`.
  - Confirmed: Phase 17 is complete; `tur_throw`/`ExceptionHandler` chain is stable and all Phase 17 fixtures pass.
- [x] `stdlib/result.tur` is reviewed; gaps vs. R1 requirements are documented.
  - Review complete: present: `ok`, `err`, `ok?`, `result-unwrap`, `result-unwrap-or`, `result-free`. Absent: `err?`, `ok-val`, `err-val`, `result-map`, `result-flat-map`, `result-map-err`, `result-or`, `result-or-else`, `result-expect`, `result-collect`, `result-partition`, `ok-or`, `err-context`. All gaps are Phase R1 work items below.
- [x] `?` operator surface syntax is decided (`(? expr)` vs. postfix `.?`) and reserved in the reader.
  - Decision: `(? expr)` prefix form. Consistent with Turmeric's S-expression style. The symbol `?` is reserved by the elaborator (rejects with "? operator is not yet implemented (Phase R1)") until the lowering pass lands. Postfix `.?` is not pursued.
- [x] `From`/`Into` trait design is agreed before R1 combinators land (they depend on it for cross-type `?` propagation).
  - Decision: `From` and `Into` are typeclasses defined in `stdlib/typeclass.tur`. `From[A B]` has method `(from [x :A] :B)`; `Into[A B]` is a derived typeclass with blanket instance `Into[A B]` whenever `From[A B]` is defined. The `?` operator uses `From` for cross-type error conversion. Both typeclasses are stubbed (name reserved, "not yet implemented") until R1 lands.
- [x] Confirm that typed panic payloads are represented as tagged `tur_exception` payloads (reusing the Phase 17 exception runtime rather than a separate system).
  - Decision: confirmed. `tur_panic` wraps its payload in a `tur_exception` with `payload_type = TY_CSTR` for string panics and `TY_PTR_VOID` for typed payloads. The existing `ExceptionHandler` chain is reused; `tur_catch_unwind` is a setjmp boundary that converts the caught `tur_exception` into a `Result`.
- [x] Decide `catch_unwind` relationship to `try`/`catch`: does `catch_unwind` reuse the setjmp chain or introduce a new one? Avoid two incompatible unwinding mechanisms.
  - Decision: reuses the setjmp chain. `tur_catch_unwind` calls `exn_push_handler()`, sets up a setjmp, and on longjmp converts the caught `tur_exception` into a heap-allocated result struct. `try`/`catch` and `catch-unwind` are both served by the same `ExceptionHandler` chain; no second unwinding mechanism.
- [x] Confirm double-panic → `abort()` is acceptable (prior art: Rust); document in `panic-system-vs-exception-system-plan.md`.
  - Decision: confirmed acceptable. `tur_panic` sets a `tur_panic_in_progress` flag before longjmping; if `tur_panic` is re-entered while the flag is set, it calls `abort()` immediately with "double panic: aborting" on stderr. Documented as a note in `panic-system-vs-exception-system-plan.md`.
- [x] Confirm panic + async/generators semantics (panic exits the task's future at the join point as `Err(PanicPayload)`); document prior-art ruling before async work begins.
  - Decision: deferred until async lands. The prior-art ruling (Rust `catch_unwind` + async) is noted: panics in async tasks are caught at the task boundary via `catch-unwind` and surface as `Err(PanicPayload)` at the join point. This must be documented in the async design doc before async work begins; it is not a blocker for Phase R1–R5.

---

## Hybrid Result + Limited Panic — Actionable Tasks

### Phase R0 remaining tasks (Design)
- [x] Finalise `Result<T, E>` surface syntax and confirm `stdlib/result.tur` covers or is extended to cover it.
  - Decision: `result<T, E>` is the surface type; at the value level `(ok v)` and `(err e)` are the constructors. `stdlib/result.tur` is the canonical location. The underlying struct `{ bool is_ok; int64_t ok_val; int64_t err_val; }` (heap-allocated, returned as `ptr<void>`) is the v1 representation until a native tagged-union type is available. All R1 combinators extend `stdlib/result.tur` using this representation.
- [x] Specify `panic-payload` struct schema and runtime type-tag representation.
  - Decision: `panic-payload` is a `tur_exception` wrapping a `void*` payload tagged with `TypeKind`. For `(panic msg)` the payload is a `TY_CSTR` pointer; for `(panic-with val)` the payload is `TY_PTR_VOID`. `panic-payload-type` returns the `TypeKind` integer; `panic-payload-downcast` casts and returns the `void*`. Schema is defined in `src/runtime.h` as part of the `tur_exception` struct.
- [x] Specify `catch-unwind` and `catch-panic-of` surface syntax and semantics.
  - Decision: `(catch-unwind thunk)` calls thunk with no arguments inside a setjmp boundary and returns `(ok nil)` on success or `(err payload)` on panic. `(catch-panic-of Type thunk)` additionally filters by type tag; re-panics if the caught exception's type tag does not match `Type`. Both forms lower to `exn_push_handler` + setjmp in the emitter.
- [x] Define `Error` typeclass (extends `Show`): `error-message`, `error-cause` methods.
  - Decision: `(defclass Error [a] (error-message [x] :cstr) (error-cause [x] :ptr<void>))` defined in `stdlib/typeclass.tur`. Extends `Show` by constraint. `error-cause` returns `nil` (null pointer) for errors with no cause. Instances are defined alongside each error struct in `stdlib/exn.tur`.
- [x] Define `From`/`Into` conversion typeclasses and blanket-derivation rule.
  - Decision: `(defclass From [a b] (from [x] :b))` and `(defclass Into [a b] (into [x] :b))` defined in `stdlib/typeclass.tur`. Blanket rule: an `Into[A B]` instance is automatically created whenever a `From[A B]` instance exists. Both names are reserved now and will emit "not yet implemented" until R1 lands.
- [x] Decide and document `?` operator syntax.
  - Decision: `(? expr)` — documented above (prerequisite item). Lowers to: evaluate `expr`, if `(err? result)` then early-return `(err (from (err-val result)))`, else yield `(ok-val result)`.
- [x] Define `Must<T>` semantics: `must!`, `must-msg!`, `option-expect`, `result-expect`.
  - Decision: `(must! expr)` is a macro that expands to an inline branch: if `expr` is `none` or `(err ...)`, call `(panic "must! failed")`. `(must-msg! expr msg)` expands to the same but with a custom message string. `option-expect` and `result-expect` are function-form aliases taking an explicit message string. All four are defined in `stdlib/result.tur`.
- [x] Survey and document panic/async interaction (prior art: Rust `catch_unwind` + async).
  - Decision: deferred until async ships. Ruling noted: panics in async tasks are caught at the task join point via `catch-unwind` and surface as `Err(PanicPayload)`. Document this in the async design doc before async work begins.
- [x] Survey and document panic-in-Drop/defer interaction (prior art: Rust double-panic = abort).
  - Decision: if a defer thunk calls `tur_panic` while `tur_panic_in_progress` is set, the double-panic guard fires `abort()` with "double panic: aborting". This mirrors Rust's behaviour (panic in drop = abort). Defer thunks should never panic; a "defer body must not panic" advisory lint is future work.
- [x] Write panic strategy decision into `panic-system-vs-exception-system-plan.md`.
  - Decision: `tur_panic` reuses the Phase 17 `ExceptionHandler` chain. Strategy is `UNWIND` by default; `TUR_PANIC_STRATEGY=ABORT` replaces `tur_panic` with a direct `fprintf + abort()`. The double-panic guard, `tur_panic_in_progress` flag, and defer interaction are documented in `panic-system-vs-exception-system-plan.md` as design decisions.

### Phase R1 remaining tasks (Core `Result<T, E>`)
- [x] Verify or extend `stdlib/result.tur` with `ok`, `err`, `ok?`, `err?`, `ok-val`, `err-val`.
  - Done: all six are implemented with proper `:ptr<void>` type annotations and inline C bodies. `ok`/`err` accept `:int` v1 payloads. `err?` is a new addition (previously absent).
- [x] Implement `result-map`, `result-flat-map`, `result-map-err`.
  - Done: all three implemented in `stdlib/result.tur` using inline C with `((int64_t (*)(int64_t))f)(val)` function pointer calling convention (required because Turmeric v1 only supports 0-arg `ptr<void>` calls from Turmeric; higher-order calls go through inline C).
- [x] Implement `result-or`, `result-or-else`, `result-unwrap-or`, `result-expect`.
  - Done: all four implemented. `result-unwrap-or` uses inline C for type-safety; `result-expect` aborts with custom message; `result-or`/`result-or-else` use inline C for `ptr<void>` branch return.
- [x] Implement `result-collect` — collect `(vec (result T E))` into `(result (vec T) E)`.
- [x] Implement `result-partition` — split into `(vec T, vec E)`.
- [ ] Implement `?` operator lowering in elaborator: short-circuit return on `Err`.
- [ ] Add compile error for `?` used outside a `Result`-returning function.
- [ ] Implement `Display`, `Debug`, `Error` typeclass instances for `result<T, E>`.
- [ ] Implement `From`/`Into` typeclasses and blanket derivation.
- [ ] Implement error conversion for stdlib error types (`IoError`, `ParseError`, etc.).
- [x] Add `ok-or` helper: `option<T>` → `result<T, E>`.
  - Done: `ok-or` implemented in `stdlib/result.tur`; converts option struct `{ bool is_some; int64_t value; }` to a result.
- [x] Add `err-context` helper: wrap error with additional context string.
  - Done: `err-context` implemented in `stdlib/result.tur`; builds `"ctx: original"` via `malloc`/`memmove`/`strlen` (declared inline via `extern size_t strlen(const char*)`).
- [x] Add fixture `result-basic.tur`.
  - Done: `tests/fixtures/result-basic/` — covers `ok?`, `err?`, `ok-val`, `err-val`, `result-unwrap`, `result-unwrap-or`, `result-expect`. 9 assertions, all pass.
- [x] Add fixture `result-combinators.tur`.
  - Done: `tests/fixtures/result-combinators/` — covers `result-map`, `result-map-err`, `result-flat-map`, `result-or`, `result-or-else`, `result-unwrap-or`, `result-expect`. 12 assertions, all pass.
- [ ] Add fixture `result-question-op.tur`.
- [ ] Add fixture `result-display.tur`.
- [ ] Add fixture `result-from-into.tur`.
- [ ] Add fixture `result-collect.tur`.
- [ ] Add negative fixture `result-question-outside-fn.tur`.
- [ ] Add codegen snapshots for `ok`/`err` and `?` lowering.

### Phase R2 remaining tasks (Panic Mechanism)
- [x] Implement `(panic msg)` — lowers to `tur_panic(msg)`; return type is diverging `!`.
- [ ] Implement `(panic-with payload)` — typed payload panic.
- [ ] Implement diverging `!` (never) type in elaborator; `!` is a subtype of every type.
- [x] Implement `tur_panic` and `tur_panic_with` in `src/runtime.{c,h}`. (`tur_panic` done; `tur_panic_with` deferred)
- [ ] Implement `panic-payload` struct in `src/runtime.h`.
- [ ] Implement `tur_catch_unwind` (setjmp boundary, returns `result`).
- [ ] Implement `tur_catch_panic_of` (type-filtered catch; re-panics on mismatch).
- [ ] Implement `(catch-unwind thunk)` surface form and lowering.
- [ ] Implement `(catch-panic-of Type thunk)` surface form and lowering.
- [ ] Implement `panic-payload-type` and `panic-payload-downcast` accessors.
- [ ] Verify defer chain fires during panic unwinding (reuses Phase 17 mechanism; end-to-end test).
- [x] Implement double-panic → `abort()` guard in `tur_panic`.
- [x] Add fixture `panic-basic.tur`.
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

### Thread Safety remaining tasks

#### T19 — Thread primitives (v1)
- [ ] Add `Send` and `Sync` marker traits to type system (`src/types.{c,h}`, `src/typeclass.{c,h}`).
- [ ] Implement `Arc<T>` — atomic reference counting (`src/rc.{c,h}` or new file + `stdlib/arc.tur`).
- [ ] Implement `Atomic<T>` for primitive types with all memory ordering options (`stdlib/atomic.tur`).
- [ ] Implement `Mutex<T>` with poison detection and `defer`-integrated `with-lock` (`stdlib/mutex.tur`).
- [ ] Implement `RwLock<T>` with scoped read/write variants (`stdlib/rwlock.tur`).
- [ ] Implement `Condvar` with `wait`/`notify-one`/`notify-all` (`stdlib/condvar.tur`).
- [ ] Implement `Once` (one-time init) and `Barrier` (N-thread sync) (`stdlib/sync.tur`).
- [ ] Implement `Thread`/`JoinHandle` with spawn, join, detach, and thread ID (`stdlib/thread.tur`).
- [ ] Implement thread-local storage: `thread-local`, `thread-local-get`, `thread-local-set!`.
- [ ] Implement `Chan<T>` — synchronous channel (`stdlib/chan.tur`).
- [ ] Implement `AsyncChan<T>` — buffered async channel with blocking and non-blocking variants.
- [ ] Implement `Select` — multi-channel select with optional `default` branch.
- [ ] Extend borrow checker to track `Send`/`Sync` and reject non-`Send` closures passed to `thread`.
- [ ] Migrate `global_handler_chain` and `global_effect_handler_chain` to `__thread` TLS storage.
- [ ] Add fixtures: `thread-basic.tur`, `arc-basic.tur`, `mutex-basic.tur`, `mutex-poison.tur`, `rwlock-basic.tur`, `atomic-basic.tur`.
- [ ] Add fixtures: `channel-basic.tur`, `async-channel.tur`, `select-basic.tur`, `barrier.tur`, `once.tur`, `thread-arc.tur`.
- [ ] Add integration fixtures: `threaded-fizzbuzz.tur`, `producer-consumer.tur`.
- [ ] Add stress fixtures: `thread-stress.tur`, `mutex-stress.tur`, `atomic-stress.tur`.
- [ ] Add negative fixtures: `thread-send-ref.tur`, `thread-send-cont.tur`.
- [ ] Add codegen snapshots for thread spawn, `Arc` refcount, `Mutex` lock/unlock.
- [ ] Run all thread fixtures under ThreadSanitizer (TSan).

#### T20 — Thread pool and higher-level abstractions
- [ ] Implement `ThreadPool::new` with fixed size and `submit`/`shutdown`.
- [ ] Implement `ThreadPool::new-dynamic` with auto-scaling.
- [ ] Implement `Future<T>` and `Promise<T>` with `get`, `done?`, `fulfill`, `fail`.
- [ ] Implement `WorkQueue<T>` — bounded and unbounded thread-safe queue.
- [ ] Implement `Semaphore` counting semaphore.
- [ ] Add fixtures: `thread-pool-basic.tur`, `thread-pool-dynamic.tur`, `future-basic.tur`, `future-error.tur`, `work-queue.tur`, `semaphore.tur`.
- [ ] Add integration fixture: `raytracer.tur` (parallel ray-tracer using thread pool).

#### T21 — Fibers and effects integration (v2)
- [ ] Implement `Fiber<T>` type with `Fiber::new`, `Fiber::resume`, `Fiber::yield`, `Fiber::done?`.
- [ ] Implement fiber-local storage: `fiber-local`, `fiber-local-get`, `fiber-local-set!`.
- [ ] Implement cooperative scheduler: `Scheduler::new`, `Scheduler::spawn`, `Scheduler::run-to-completion`.
- [ ] Integrate fibers with channels: fiber blocked on `Chan::recv` yields to scheduler.
- [ ] Implement `reset`/`shift` integration: continuations scoped to fiber stack; cross-fiber resume is runtime error.
- [ ] Implement `async`/`await` sugar: `(async expr)` → `Future<T>`, `(await future)` → `T`.
- [ ] Implement `ThreadPool::submit-async` for async tasks.
- [ ] Add fixtures: `fiber-basic.tur`, `fiber-yield.tur`, `fiber-scheduler.tur`, `async-await-basic.tur`, `async-await-channel.tur`.
- [ ] Add negative fixture: `fiber-cross-resume.tur` (cross-fiber resume panics).

### Unsafe Operations remaining tasks

#### U1 — `Unsafe` effect in type system
- [ ] Register `Unsafe` as a built-in effect constant in `src/effect.{c,h}`.
- [ ] Propagate `Unsafe` through call sites via the existing effect-row mechanism.
- [ ] Emit compile error when unsafe function is called from safe context without `unsafe` block.
- [ ] Support explicit `@ {Unsafe}` annotation on `defn` to mark an entire function unsafe.
- [ ] Add fixture `unsafe-effect-row.tur`.
- [ ] Add negative fixture `unsafe-leak.tur`.

#### U2 — `unsafe { }` block sugar
- [ ] Parse `(unsafe expr...)` form in reader.
- [ ] Desugar to `try_with` with an `Unsafe` handler that discharges the effect within the block.
- [ ] Enforce containment: `unsafe` block cannot leak `Unsafe` to caller.
- [ ] Warn on empty and oversized `unsafe` blocks (configurable threshold).
- [ ] Add fixtures: `unsafe-basic.tur`, `unsafe-nested.tur`, `unsafe-defer.tur`.
- [ ] Add negative fixture: `unsafe-empty.tur` (warning on empty block).
- [ ] Add codegen snapshots for `unsafe` block lowering.

#### U3 — Unsafe primitive operations
- [ ] Implement pointer operations: `ptr-deref`, `ptr-write`, `ptr-add`, `ptr-sub`, `ptr-null?`, `ptr-of`.
- [ ] Implement type casting: `unsafe-cast`, `reinterpret` (with compile-time size check), `transmute`.
- [ ] Implement unchecked array ops: `array-get-unchecked`, `array-set-unchecked`.
- [ ] Implement raw memory management: `raw-malloc`, `raw-free`, `raw-realloc`, `raw-memcpy`, `raw-memset`.
- [ ] Implement FFI primitives: `c-call`, `dlopen`, `dlsym`, `dlclose`.
- [ ] Add fixtures: `unsafe-ptr-deref.tur`, `unsafe-ptr-arith.tur`, `unsafe-cast.tur`, `unsafe-reinterpret.tur`, `unsafe-array-unchecked.tur`, `unsafe-malloc.tur`, `unsafe-memcpy.tur`.
- [ ] Add negative fixture: `unsafe-reinterpret-size-mismatch.tur`.
- [ ] Add codegen snapshots for all unsafe primitives.

#### U4 — Safe standard library wrappers
- [ ] Implement bounds-checked `array-get`, `array-set`, `array-slice` returning `Option`.
- [ ] Verify/extend `Vec<T>` operations use `unsafe` blocks internally for raw memory operations.
- [ ] Implement safe FFI helpers: `with-c-string`, `from-c-string`.
- [ ] Implement `box`/`unbox` for heap allocation via `ref<T>`.
- [ ] Implement arena allocator: `arena-new`, `arena-alloc`, `arena-free`.
- [ ] Add fixtures: `safe-array-bounds.tur`, `safe-vec-ops.tur`, `safe-c-string.tur`, `safe-box.tur`, `safe-arena.tur`.

#### U5 — Linting, auditing, and tooling
- [ ] Implement unsafe block linter: size threshold warning (`--lint-unsafe-max-lines N`), nested block warning.
- [ ] Implement `#[safety "..."]` attribute parsing and `;;; SAFETY:` comment enforcement (`--require-unsafe-docs`).
- [ ] Implement trusted-code coverage metric (`--unsafe-stats` flag).
- [ ] Add `--lint-unsafe` flag to enable all unsafe lints.
- [ ] Add fixtures: `lint-unsafe-size.tur`, `lint-unsafe-doc.tur`, `lint-unsafe-nested.tur`, `stats-unsafe.tur`.

---

### STM prerequisites (Phase 20)
- [ ] Confirm Phase 19 algebraic effects infrastructure is stable (handler stack, `perform`/`handle` lowering, continuation runtime all complete).
  - Decision: Phase 19 v1 is complete per roadmap. STM phase 20 may begin once T19 thread primitives (`Mutex<T>`, condition variables, `Arc<T>`) land — those are the only additional prerequisites.
- [ ] Confirm POSIX condition variables (`pthread_cond_t`) are available on all supported platforms for TVar wait queues.
  - Decision: use POSIX `pthread_cond_t` consistently with the T19 thread primitives decision (no C11 `<threads.h>` on macOS). Same `TUR_THREAD_LOCAL` macro pattern applies to STM globals.
- [ ] Decide whether STM transaction context is thread-local or explicitly passed.
  - Decision: thread-local in v1. A single `__thread STM_Transaction *current_tx` pointer tracks the active transaction per-thread. No explicit passing needed for the simple v1 global-lock implementation.
- [ ] Decide `stm` form: special form in elaborator or macro?
  - Decision: special form handled in `elab_stm` in `src/elab.c`, analogous to `elab_handle`. The `dosync` shorthand is a stdlib macro.
- [ ] Decide how `defer` inside `stm` blocks interacts with transaction commit/abort.
  - Decision: defers inside `stm` blocks execute at transaction commit (success path) or transaction abort (failure path), not at `stm` lexical exit. The transaction maintains a defer stack; defers are fired by `tur_stm_commit` or `tur_stm_abort` respectively.
- [ ] Define `TVar` naming: `TVar` (Haskell style) vs. `TxVar`.
  - Decision: `TVar` — matches Haskell and is the most widely recognized naming.
- [ ] Define exception behavior inside `stm` blocks.
  - Decision: if an exception escapes an `stm` block, the transaction aborts (all writes discarded); the exception then propagates normally to the enclosing `try`/`catch`. Transaction defers fire (abort path) before the exception propagates.
- [ ] Confirm `TVar` Send/Sync marker policy.
  - Decision: `TVar<T>` is `Sync` (shared across threads via `Arc<TVar<T>>`) but not `Send` (ownership of the `TVar` itself is not moved). `Arc<TVar<T>>` is `Send + Sync` when `T` is `Send`.

### STM remaining tasks (Phase 20)

#### S1 — Runtime data structures (`src/stm.{c,h}`)
- [ ] Define `TVar` struct: `{ TypeInfo *type; void *value; uint64_t version; STM_WaitQueue waiters; }`.
- [ ] Define `STM_Transaction` struct: `{ TVar **read_set; uint64_t *read_versions; int read_count; TVar **write_set; void **new_values; int write_count; bool retry_requested; tur_frame_t *defer_stack; }`.
- [ ] Define `STM_State` global: `{ mtx_t global_lock; }`.
- [ ] Define `STM_WaitQueue` struct: `{ STM_Transaction **waiters; int count; pthread_cond_t cond; }`.
- [ ] Implement `tur_tvar_new(TypeInfo *type, void *initial_value) → TVar *`.
- [ ] Implement `tur_tvar_read(STM_Transaction *tx, TVar *tv) → void *` (records read in transaction log).
- [ ] Implement `tur_tvar_write(STM_Transaction *tx, TVar *tv, void *value)` (records write in transaction log).
- [ ] Implement `tur_stm_validate(STM_Transaction *tx) → bool` (checks all read versions are still current).
- [ ] Implement `tur_stm_commit(STM_Transaction *tx) → bool` (applies writes, increments versions, fires commit-path defers).
- [ ] Implement `tur_stm_abort(STM_Transaction *tx)` (discards writes, fires abort-path defers).
- [ ] Implement `tur_stm_retry(STM_Transaction *tx)` (adds transaction to wait queues of all read TVars, blocks on condition variable).
- [ ] Implement `tur_stm_check(bool condition)` (calls `tur_stm_abort` if false).
- [ ] Implement `tur_atomically(stm_fn_t fn, void *env) → void *` (outer retry loop with global lock).
- [ ] Migrate `STM_State.global_lock` and thread-local `current_tx` to `TUR_THREAD_LOCAL` in the same pass that adds thread safety to effect handler chains.

#### S2 — Elaborator and codegen (`src/elab.{c,h}`, `src/emit.{c,h}`)
- [ ] Implement `elab_stm` in `src/elab.c`: delimit a transaction block, type-check body, verify `TVar::read`/`TVar::write` calls are inside `stm`.
- [ ] Implement `elab_atomically`: validate that argument is an `stm` block or returns `(STM a)`.
- [ ] Implement `elab_retry`: only valid inside `stm`; lowers to `tur_stm_retry(current_tx)`.
- [ ] Implement `elab_check`: only valid inside `stm`; lowers to `tur_stm_check(cond)`.
- [ ] Implement `elab_or_else`: takes two `stm` blocks; tries the first, retries with second if first calls `retry`.
- [ ] Emit `TVar::read` calls as `tur_tvar_read(current_tx, tv)`.
- [ ] Emit `TVar::write` calls as `tur_tvar_write(current_tx, tv, value)`.
- [ ] Emit `stm` block as a closure passed to `tur_atomically`.
- [ ] Emit `TVar::modify` as inline `read → apply fn → write` within same transaction.
- [ ] Emit `TVar::swap` as inline `read → write → return old`.
- [ ] Add static check: `TVar::read`/`TVar::write` outside an `stm` block is a compile error (TUR-E0009).

#### S3 — Synchronization primitives stdlib (`stdlib/stm.tur`)
- [ ] Implement `TMVar<T>`: opaque struct wrapping a `TVar<(option T)>`.
- [ ] Implement `TMVar::new [value]`: creates a full `TMVar`.
- [ ] Implement `TMVar::new-empty []`: creates an empty `TMVar`.
- [ ] Implement `TMVar::put [mvar value]`: blocks (via `retry`) until empty, then writes value.
- [ ] Implement `TMVar::try-put [mvar value] : bool`: non-blocking; returns false if already full.
- [ ] Implement `TMVar::take [mvar]`: blocks until full, then empties and returns value.
- [ ] Implement `TMVar::try-take [mvar] : (option a)`: non-blocking; returns `none` if empty.
- [ ] Implement `TMVar::read [mvar]`: read without taking (blocks until full).
- [ ] Implement `TMVar::is-empty [mvar] : bool`.
- [ ] Implement `TChan<T>`: opaque struct wrapping a `TVar` of a list head and tail pointer.
- [ ] Implement `TChan::new []`.
- [ ] Implement `TChan::write [chan value]`.
- [ ] Implement `TChan::read [chan]`: blocks via `retry` until non-empty.
- [ ] Implement `TChan::try-read [chan] : (option a)`.
- [ ] Implement `TChan::peek [chan]`: read without consuming.
- [ ] Implement `TChan::try-peek [chan] : (option a)`.
- [ ] Implement `TSem`: opaque struct wrapping a `TVar<int>`.
- [ ] Implement `TSem::new [count]`.
- [ ] Implement `TSem::wait [sem]`: blocks until count > 0, then decrements.
- [ ] Implement `TSem::try-wait [sem] : bool`.
- [ ] Implement `TSem::signal [sem]`: increments count.

#### S4 — Convenience macros stdlib (`stdlib/stm.tur`)
- [ ] Implement `(with-tvar [name init] & body)` macro: creates TVar, runs body in `atomically`.
- [ ] Implement `(dosync & body)` macro: shorthand for `(atomically (stm ...))`.
- [ ] Implement `(stm-when cond & body)` macro: conditional inside `stm`.
- [ ] Implement `(stm-unless cond & body)` macro.
- [ ] Implement `TVar::cas [tv old new] : bool`: compare-and-swap within a transaction.
- [ ] Implement `TVar::update [tv f & args]`: alias for `TVar::modify` with extra args.
- [ ] Implement `(atomically-batch & txs)`: run multiple `stm` closures in one transaction.

#### S5 — Fixtures (`tests/fixtures/stm/`)
- [ ] Add `stm-tvar-basic.tur` — `TVar::new`, `TVar::read`, `TVar::write` in a single-threaded transaction.
- [ ] Add `stm-tvar-modify.tur` — `TVar::modify`, `TVar::swap`, `TVar::cas`.
- [ ] Add `stm-atomicity.tur` — verify writes are not visible until commit.
- [ ] Add `stm-retry.tur` — `retry` blocks and retries when a TVar changes (single-threaded: change TVar from another `atomically` call).
- [ ] Add `stm-check.tur` — `check` aborts transaction when condition is false.
- [ ] Add `stm-or-else.tur` — `or-else` falls through to second branch when first calls `retry`.
- [ ] Add `stm-tmvar.tur` — `TMVar::put`, `TMVar::take`, `TMVar::read`, `TMVar::is-empty`.
- [ ] Add `stm-tchan.tur` — `TChan::write`, `TChan::read`, `TChan::peek`.
- [ ] Add `stm-tsem.tur` — `TSem::new`, `TSem::wait`, `TSem::signal`.
- [ ] Add `stm-defer.tur` — `defer` inside `stm` fires at commit (success) or abort (failure) rather than lexical exit.
- [ ] Add `stm-exception.tur` — exception inside `stm` aborts the transaction; exception propagates normally.
- [ ] Add `stm-dosync.tur` — `dosync` macro shorthand.
- [ ] Add `stm-with-tvar.tur` — `with-tvar` macro.
- [ ] Add concurrency fixture `stm-concurrent-writes.tur` — multiple threads writing to same TVar (requires T19).
- [ ] Add concurrency fixture `stm-concurrent-transfers.tur` — concurrent bank transfers; verify no money lost or created (requires T19).
- [ ] Add stress fixture `stm-stress.tur` — high-contention increment benchmark (requires T19).
- [ ] Add integration fixture `stm-with-arc.tur` — `Arc<TVar<T>>` shared across threads.
- [ ] Add integration fixture `stm-with-threads.tur` — STM combined with `Thread` spawn/join.
- [ ] Add negative fixture `stm-read-outside-transaction.tur` — `TVar::read` outside `stm` is a compile error (TUR-E0009).
- [ ] Add negative fixture `stm-write-outside-transaction.tur` — `TVar::write` outside `stm` is a compile error.
- [ ] Add codegen snapshots: `stm` block lowers to closure + `tur_atomically`; `TVar::read`/`write` lower to `tur_tvar_read`/`tur_tvar_write`.

#### S6 — Phase 21: Scalable STM (V2, deferred)
- [ ] Implement per-TVar locking (replace global lock with `mtx_t` in each `TVar`).
- [ ] Implement lock ordering: acquire TVar locks in address order during commit to prevent deadlocks.
- [ ] Implement lock stripping: group TVars into N lock buckets (default: 64) to reduce contention.
- [ ] Add performance benchmarks: `stm-counter` (< 100ns/op v1), `stm-transfer` (< 200ns/transfer v1), `stm-bank` (concurrent bank simulation, linear scalability v2+).
- [ ] Add `stm-tchan-throughput.tur` benchmark: `TChan` write/read throughput (target > 1M ops/sec).
- [ ] Run all STM fixtures under ThreadSanitizer after fine-grained locking lands.

---

### Closeout tasks
- [ ] Re-run targeted fixtures for Phases 15-19 deferred follow-up work.
- [ ] Re-run relevant codegen snapshots for typeclass/effect/continuation lowering paths.
- [ ] Update `docs/turmeric-plan.md` with promoted or completed deferred items.
- [ ] Add a short completion note to this file once deferred clusters are resolved.

---

## HAMT prerequisites (Phases P1–P4)

See [hamt-feasibility.md](archive/hamt-feasibility.md) and [turmeric-plan.md §Persistent Collections](turmeric-plan.md) for rationale and design decisions.

- [x] Phase 15 (Typeclasses v1) is stable — needed for `Eq` and `Hash` dispatch for key comparison.
  - Confirmed: Phase 15 is complete; `Eq` typeclass is available for user-defined key types.
- [ ] Decide `Hash` typeclass design: mirror `Eq` / `Ord` pattern (`(defclass Hash [a] (hash [x : a] : uint64))`) vs. a free function per type.
  - Pending decision. Recommendation: `Hash` typeclass alongside `Eq` in `stdlib/typeclass.tur`; primitive instances use `tur_siphash13` or `tur_xxhash64` under the hood.
- [ ] Decide hash function: `tur_siphash13` (cryptographic, DoS-resistant) vs. `tur_xxhash64` (speed-optimised, non-cryptographic).
  - Pending decision. Document in a comment at the top of `src/hamt.c` once resolved.
- [ ] Confirm `^persistent` annotation syntax does not conflict with any reserved annotation in `src/reader.{c,h}`.
  - Pending check. Annotations beginning with `^` are reader-level metadata; confirm `persistent` is not taken.
- [ ] Define `hamt` surface type name: `hamt<K V>` vs. `persistent-map<K V>` vs. `pmap<K V>`.
  - Pending decision. Recommendation: `hamt<K V>` as the canonical type; `persistent-map` as a stdlib alias.
- [ ] Confirm linker flag policy for dead-code stripping: `-dead_strip` (macOS), `-Wl,--gc-sections` with `-ffunction-sections -fdata-sections` (Linux).
  - Confirmed per [hamt-feasibility.md §Dead Code / Inclusion Strategy](archive/hamt-feasibility.md). These flags are emitted by `tur build` when targeting those platforms; no emit-phase gating required.
- [ ] Define whether Phase P3 (`^persistent` lowering) requires Phase 19 effect-row infrastructure (for `@ {Unsafe}` propagation through `hamt.c`).
  - Pending decision. Recommendation: Phase P1–P2 are pure library work; Phase P3 (lowering pass) can begin once Phase 19 Section A (effect-row surface syntax) is stable. Phase P4 (transient mode) has no dependency on effect rows.

---

## HAMT remaining tasks (Phases P1–P4)

### Phase P1 remaining tasks (Core HAMT C implementation)
- [ ] Define node types: `HAMT_NODE_BITMAP`, `HAMT_NODE_ARRAY`, `HAMT_NODE_COLLISION`.
- [ ] Define `HamtNode` tagged union and `Hamt` root struct.
- [ ] Implement `hamt_new`, `hamt_set`, `hamt_del`, `hamt_has`, `hamt_get`, `hamt_count`, `hamt_free`.
- [ ] Implement `hamt_node_retain` / `hamt_node_release` ref-counting helpers.
- [ ] Implement `hamt_merge` — merge two maps; `b` wins on collision.
- [ ] Implement `hamt_iter_init` / `hamt_iter_next` — in-order iteration.
- [ ] Choose and integrate hash function (`tur_siphash13` or `tur_xxhash64`).
- [ ] Implement `hamt_dump(Hamt *m, FILE *out)` — pretty-print node tree.
- [ ] Add `hamt_alloc` / `hamt_free_node` wrappers; no raw `malloc`/`free` calls outside them.
- [ ] Add `tests/fixtures/hamt/hamt-basic.tur`.
- [ ] Add `tests/fixtures/hamt/hamt-sharing.tur`.
- [ ] Add `tests/fixtures/hamt/hamt-delete.tur`.
- [ ] Add `tests/fixtures/hamt/hamt-collision.tur`.
- [ ] Add `tests/fixtures/hamt/hamt-iteration.tur`.
- [ ] Add `tests/fixtures/hamt/hamt-merge.tur`.
- [ ] Add `tests/fixtures/hamt/hamt-large.tur` (10 000 keys).
- [ ] Add `tests/fixtures/hamt/hamt-memory.tur` (ASan clean).
- [ ] Verify clean under AddressSanitizer and Valgrind.

### Phase P2 remaining tasks (Lisp bindings)
- [ ] Implement `stdlib/hamt.tur` with full public API: `hamt/new`, `hamt/set`, `hamt/get`, `hamt/get-or`, `hamt/del`, `hamt/has?`, `hamt/count`, `hamt/keys`, `hamt/vals`, `hamt/entries`, `hamt/merge`, `hamt/merge-with`, `hamt/map`, `hamt/filter`, `hamt/reduce`, `hamt/from-vec`, `hamt/to-vec`.
- [ ] Implement `Show` instance for `hamt`.
- [ ] Implement `Eq` instance for `hamt`.
- [ ] Add `tests/fixtures/hamt/hamt-lisp-basic.tur`.
- [ ] Add `tests/fixtures/hamt/hamt-lisp-snapshot.tur`.
- [ ] Add `tests/fixtures/hamt/hamt-lisp-map-filter.tur`.
- [ ] Add `tests/fixtures/hamt/hamt-lisp-merge-with.tur`.
- [ ] Add `tests/fixtures/hamt/hamt-lisp-show.tur`.
- [ ] Add `tests/fixtures/hamt/hamt-lisp-eq.tur`.
- [ ] Add `tests/fixtures/hamt/hamt-lisp-from-to-vec.tur`.
- [ ] Add codegen snapshots for HAMT function call lowering.

### Phase P3 remaining tasks (Compiler lowering pass)
- [ ] Add `^persistent` annotation handling in elaborator: lower map literals to `hamt/from-vec`.
- [ ] Lower `(assoc m k v)` on `^persistent` binding to `hamt/set`.
- [ ] Lower `(dissoc m k)` on `^persistent` binding to `hamt/del`.
- [ ] Lower `(get m k)` on `^persistent` binding to `hamt/get`.
- [ ] Lower `(count m)` on `^persistent` binding to `hamt/count`.
- [ ] Propagate `^persistent` through `let` bindings and function return types.
- [ ] Emit type-mismatch diagnostic (TUR-E00XX) on `^persistent` ↔ mutable map mismatch.
- [ ] Set `needs_hamt` flag on `Emit` context; emit `#include "hamt.h"` when set.
- [ ] Emit platform-appropriate dead-code linker flags (`-dead_strip` / `-Wl,--gc-sections`) in `tur build`.
- [ ] Add `tests/fixtures/hamt/hamt-lowering-basic.tur`.
- [ ] Add `tests/fixtures/hamt/hamt-lowering-propagate.tur`.
- [ ] Add `tests/fixtures/hamt/hamt-lowering-mutable-unchanged.tur`.
- [ ] Add negative fixture `tests/fixtures/hamt/hamt-lowering-type-mismatch.tur`.
- [ ] Add codegen snapshots: `assoc`/`dissoc`/`get` on `^persistent` lower to `hamt_set`/`hamt_del`/`hamt_get`.

### Phase P4 remaining tasks (Optimization and tooling)
- [ ] Define `HamtTransient` struct with owner token.
- [ ] Implement `hamt_transient`, `hamt_transient_set`, `hamt_transient_del`, `hamt_persistent`.
- [ ] Implement Lisp API: `hamt/transient`, `hamt/transient-set!`, `hamt/transient-del!`, `hamt/persistent!`.
- [ ] Implement `hamt_dump_dot` — DOT format output for Graphviz.
- [ ] Implement `(hamt/dump m)` Lisp debug form (debug builds only).
- [ ] Add `tests/fixtures/hamt/hamt-transient-basic.tur`.
- [ ] Add `tests/fixtures/hamt/hamt-transient-isolation.tur`.
- [ ] Add `tests/fixtures/hamt/hamt-transient-invalidated.tur`.
- [ ] Add benchmark `tests/benchmarks/hamt/hamt-bench-insert.tur` (100 000 inserts; < 3× mutable baseline).
- [ ] Add benchmark `tests/benchmarks/hamt/hamt-bench-lookup.tur`.
- [ ] Add benchmark `tests/benchmarks/hamt/hamt-bench-snapshot.tur`.
- [ ] Add benchmark `tests/benchmarks/hamt/hamt-bench-transient.tur`.

---

## Backtracking prerequisites (Phases B1–B5)

See [backtracking-cloneable-continuations-plan.md](archive/backtracking-cloneable-continuations-plan.md) and [turmeric-plan.md §Backtracking with Cloneable Continuations](turmeric-plan.md) for rationale and design decisions.

- [x] Phase 15 (Typeclasses v1) is stable — needed for `Clone` trait dispatch.
  - Confirmed: Phase 15 complete; typeclass dictionary passing is stable.
- [x] Phase 18 (Delimited continuations) is stable — `shift`/`reset` is the substrate for cloneable continuations.
  - Confirmed: Phase 18 complete; `tur_cont_alloc`/`tur_cont_resume`/`tur_cont_drop` are implemented.
- [x] Phase 19 (Algebraic effects v1) is stable — handler infrastructure is in place for hybrid integration.
  - Confirmed: Phase 19 v1 complete; `defeffect`/`defhandler`/`perform`/`handle` all work.
- [ ] Decide `Clone` vs `Copy` distinction: is `Clone` always deep? Is there a separate zero-cost `Copy` marker for bit-copyable types?
  - Pending decision. Recommendation: `Clone` is always deep (allocates new memory); `Copy` is a future zero-cost marker (`int`, `bool`, `cstr` are `Copy`; `Copy` implies `Clone`). Ship `Clone` only in B1; reserve `Copy` for a later phase.
- [ ] Decide `rc<T>` clone semantics: refcount increment (shallow) vs. deep clone of pointed-to value.
  - Pending decision. See Phase B1 design: recommendation is refcount-increment for `rc<T>` (shared ownership), deep clone for `ref<T>` (independent ownership).
- [ ] Confirm `cloneable-reset` / `cloneable-shift` syntax does not conflict with Phase 18 `reset`/`shift` in the reader or elaborator.
  - Pending check. Both surface forms can coexist as separate elaboration branches (`elab_reset` vs. `elab_cloneable_reset`).
- [ ] Define error codes TUR-E00YY (non-`Clone` capture) and TUR-E00YZ (`cloneable-shift` outside `cloneable-reset`).
  - Pending: assign next available error codes when B1 elaborator work begins.
- [ ] Decide whether `stdlib/logic.tur` depends on Phase P2 HAMT (`stdlib/hamt.tur`) for the persistent substitution map, or falls back to an association list.
  - Pending decision. Recommendation: use association list in B4 v1; add a `(with-hamt-subst ...)` optimised variant when HAMT ships.
- [ ] Define `--backtrack-depth N` flag design: per-call-site cap, global cap, or both?
  - Pending decision. Recommendation: global cap applied at every `run-backtrack` call; per-call override via keyword argument `:depth N`.

---

## Backtracking remaining tasks (Phases B1–B5)

### Phase B1 remaining tasks (Clone trait infrastructure)
- [ ] Define `(defclass Clone [a] (clone [x : a] : a))` in `stdlib/typeclass.tur`.
- [ ] Implement `Clone` instances for: `int`, `int8`–`int64`, `uint8`–`uint64`, `float`, `double`, `bool`, `cstr`.
- [ ] Implement `(definstance Clone (Pair a b) [Clone a, Clone b])`.
- [ ] Implement `(definstance Clone (option a) [Clone a])`.
- [ ] Implement `(definstance Clone (list a) [Clone a])`.
- [ ] Implement `(definstance Clone (vec a) [Clone a])`.
- [ ] Implement `(definstance Clone (rc a) [Clone a])` — refcount increment (shallow; document clearly).
- [ ] Implement `(definstance Clone (ref a) [Clone a])` — deep clone into new heap allocation.
- [ ] Add `check_cloneable_capture` in `src/elab.c`; emit TUR-E00YY on non-`Clone` capture.
- [ ] Add `tests/fixtures/backtrack/clone-primitives.tur`.
- [ ] Add `tests/fixtures/backtrack/clone-pair.tur`.
- [ ] Add `tests/fixtures/backtrack/clone-option.tur`.
- [ ] Add `tests/fixtures/backtrack/clone-list.tur`.
- [ ] Add `tests/fixtures/backtrack/clone-vec.tur`.
- [ ] Add `tests/fixtures/backtrack/clone-rc.tur`.
- [ ] Add `tests/fixtures/backtrack/clone-ref.tur`.
- [ ] Add negative fixture `tests/fixtures/backtrack/clone-non-clone-capture.tur`.

### Phase B2 remaining tasks (Cloneable continuation runtime + CPS)
- [ ] Parse `(cloneable-reset body)` and `(cloneable-shift k expr)` surface forms.
- [ ] Parse `(call/cc* f)` sugar.
- [ ] Add `TY_CLONEABLE_CONT` to `src/types.{c,h}`.
- [ ] Define `CloneableContinuation` struct and `CloneEnvEntry` in `src/runtime.{c,h}`.
- [ ] Implement `tur_cont_clone`, `tur_cont_resume_cloneable`, `tur_cont_drop_cloneable`.
- [ ] One-shot `tur_cont_resume` aborts with diagnostic if called on a `is_cloneable = true` continuation.
- [ ] Implement `DEFER_SUSPENDED` and `DEFER_REPLAY` defer modes; update `src/runtime.{c,h}`.
- [ ] Implement `needs_cloneable_cps` in `src/cps.{c,h}`.
- [ ] Implement `emit_capture_environment(..., cloneable=true)` in `src/cps.{c,h}`: record `clone_fn`/`drop_fn` per binding.
- [ ] Add `tests/fixtures/backtrack/cloneable-basic.tur`.
- [ ] Add `tests/fixtures/backtrack/cloneable-multi-resume.tur`.
- [ ] Add `tests/fixtures/backtrack/cloneable-defer-suspend.tur`.
- [ ] Add `tests/fixtures/backtrack/cloneable-defer-replay.tur`.
- [ ] Add `tests/fixtures/backtrack/cloneable-ref.tur`.
- [ ] Add `tests/fixtures/backtrack/cloneable-rc.tur`.
- [ ] Add negative fixture `tests/fixtures/backtrack/cloneable-shift-outside-reset.tur`.
- [ ] Add codegen snapshots for cloneable continuation lowering.

### Phase B3 remaining tasks (Backtracking monad)
- [ ] Implement `stdlib/backtrack.tur`: `mzero`, `mreturn`, `mplus`, `mbind`, `run-backtrack`, `run-backtrack-depth`, `choice`, `guard`, `fresh`, `once`, `interleave`.
- [ ] Implement `(backtrack-do ...)` sequencing macro.
- [ ] Add `tests/fixtures/backtrack/backtrack-basic.tur`.
- [ ] Add `tests/fixtures/backtrack/backtrack-mzero.tur`.
- [ ] Add `tests/fixtures/backtrack/backtrack-bind.tur`.
- [ ] Add `tests/fixtures/backtrack/backtrack-guard.tur`.
- [ ] Add `tests/fixtures/backtrack/backtrack-fresh.tur`.
- [ ] Add `tests/fixtures/backtrack/backtrack-depth.tur`.
- [ ] Add `tests/fixtures/backtrack/backtrack-once.tur`.
- [ ] Add `tests/fixtures/backtrack/backtrack-interleave.tur`.
- [ ] Add `tests/fixtures/backtrack/backtrack-nested.tur`.
- [ ] Add `tests/fixtures/backtrack/backtrack-ref.tur`.
- [ ] Add codegen snapshots for `run-backtrack` and `mbind` lowering.

### Phase B4 remaining tasks (Standard library integration)
- [ ] Implement `stdlib/logic.tur`: `LVar`, `UState`, `Term`, `Goal`, `unify`, `unify-var`, `walk`, `fresh-lvar`, `conjoined`, `disjoined`, `run-logic`, `reify`.
- [ ] Implement `stdlib/parsec.tur`: `Input`, `Parser<a>`, `pure`, `pfail`, `item`, `pchar`, `pstring`, `or-parser`, `bind-parser`, `then-parser`, `many`, `many1`, `optional`, `run-parser`, `run-parser-full`.
- [ ] Add `tests/fixtures/backtrack/logic-unify-basic.tur`.
- [ ] Add `tests/fixtures/backtrack/logic-unify-fail.tur`.
- [ ] Add `tests/fixtures/backtrack/logic-fresh.tur`.
- [ ] Add `tests/fixtures/backtrack/logic-conjoined.tur`.
- [ ] Add `tests/fixtures/backtrack/logic-disjoined.tur`.
- [ ] Add `tests/fixtures/backtrack/logic-occurs-check.tur`.
- [ ] Add `tests/fixtures/backtrack/logic-reify.tur`.
- [ ] Add `tests/fixtures/backtrack/logic-query.tur`.
- [ ] Add `tests/fixtures/backtrack/parsec-basic.tur`.
- [ ] Add `tests/fixtures/backtrack/parsec-or.tur`.
- [ ] Add `tests/fixtures/backtrack/parsec-many.tur`.
- [ ] Add `tests/fixtures/backtrack/parsec-sequence.tur`.
- [ ] Add `tests/fixtures/backtrack/parsec-full.tur`.
- [ ] Add `tests/fixtures/backtrack/parsec-json-subset.tur`.
- [ ] Add codegen snapshots for `or-parser` and `run-logic` lowering.

### Phase B5 remaining tasks (Testing, benchmarks, optimization)
- [ ] Add `--backtrack-depth N` global flag to `src/main.c` / `src/emit.{c,h}`.
- [ ] Add warning for `run-backtrack` (no depth limit) outside test context; add `:allow-infinite true` suppression.
- [ ] Add `--dump-clone-plan` debug flag.
- [ ] Add `tests/benchmarks/backtrack/bench-clone-overhead.tur`.
- [ ] Add `tests/benchmarks/backtrack/bench-backtrack-n-queens.tur`.
- [ ] Add `tests/benchmarks/backtrack/bench-parsec-json.tur`.
- [ ] Add `tests/benchmarks/backtrack/bench-logic-query.tur`.
- [ ] Add `tests/fixtures/backtrack/backtrack-n-queens.tur` (92 solutions).
- [ ] Add `tests/fixtures/backtrack/backtrack-sudoku.tur`.
- [ ] Add `tests/fixtures/backtrack/backtrack-memory.tur` (ASan clean).
- [ ] Add negative fixture `tests/fixtures/backtrack/backtrack-depth-exceeded.tur`.
- [ ] Add `tests/fixtures/backtrack/backtrack-integration-effects.tur`.
- [ ] Add `tests/fixtures/backtrack/backtrack-integration-stm.tur` (requires Phase 20).
- [ ] Write `docs/backtracking-guide.md`.
- [ ] Update `backtracking-cloneable-continuations-plan.md` with resolved open questions and benchmark results.


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
11. Final fixture/snapshot sweep and roadmap closeout (Phases 15–19 + HKT + R)
12. **[Thread Safety]** T19 prerequisite decisions → `Send`/`Sync` traits + `Arc`/`Mutex`/`Atomic` primitives
13. **[Thread Safety]** T19 channels + `Select` + borrow checker integration → full T19 fixture sweep (TSan)
14. **[Thread Safety]** T20 thread pool + `Future`/`Promise` + `Semaphore`
15. **[Thread Safety, deferred]** T21 fibers + `async`/`await` (after Phase 19 effects stabilize)
16. **[Unsafe Ops]** U1 prerequisite decisions → `Unsafe` effect in type system
17. **[Unsafe Ops]** U2 `unsafe` block sugar → U3 primitives → U4 safe wrappers
18. **[Unsafe Ops]** U5 linting, auditing, and tooling
19. **[HAMT]** P1 `Hash` typeclass + core C implementation → P2 Lisp bindings
20. **[HAMT]** P3 compiler lowering pass (`^persistent` annotation) → P4 transient mode and benchmarks
21. **[Backtracking]** B1 `Clone` trait + elaborator enforcement → B2 cloneable continuation runtime + CPS
22. **[Backtracking]** B3 backtracking monad → B4 `stdlib/logic.tur` + `stdlib/parsec.tur`
23. **[Backtracking]** B5 testing, benchmarks, safety tooling, and user guide

---

## Notes

- Phases 15 through 19 are marked complete at v1 in the active plan; this file tracks deferred v2+ follow-up work only.
- Phase HKT (H0–H6 roadmap) is planned for v2+ and will only move to the active roadmap if the promotion decision rule is met (see [hkt-implementation-plan.md](hkt-implementation-plan.md) for details).
- Phase HAMT (P1–P4 roadmap) is planned for v2+. P1 and P2 are pure library work with no compiler prerequisites beyond Phase 15. P3 (lowering pass) requires Phase 19 Section A (effect-row surface syntax) to be stable. See [hamt-feasibility.md](archive/hamt-feasibility.md) for the full feasibility analysis.
- Phase Backtracking (B1–B5 roadmap) is planned for v2+. B1 depends only on Phase 15 (Typeclasses). B2 depends on Phase 18 (Delimited continuations). B3–B4 are pure library work. B5 adds tooling and the optional STM integration fixture requires Phase 20. See [backtracking-cloneable-continuations-plan.md](archive/backtracking-cloneable-continuations-plan.md) for the full design.
- Keep this list aligned with active roadmap changes in `docs/turmeric-plan.md`.
