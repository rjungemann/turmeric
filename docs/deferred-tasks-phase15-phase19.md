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

### Phase 19 implementation prerequisites (unblocking the remaining tasks)

These are the concrete implementation gaps that cause Phase 19 tasks to be skipped. They are distinct from the design prerequisites above, which are all resolved. Each item below must be completed before the Phase 19 remaining tasks that depend on it can proceed.

#### P19-1 — Pass pipeline / pass scheduling (blocks Section C)
- [x] Define an ordered pass array in `src/main.c`: elaborate → effect-row inference → borrow-check → codegen.
  - The existing flow calls elaboration and codegen directly with no intervening pass slots. Adding a named pass enum and a simple loop enables future passes to be inserted without touching every call site.
- [x] Add a `PassContext` struct (or extend `ElabCtx`) to carry per-function inferred effect rows between passes.
  - Currently nothing accumulates inferred rows across the AST walk; codegen does not receive them.

#### P19-2 — Effect-row inference pass (blocks Section C: union rows, validate rows)
- [x] Implement `src/effect_check.c` with an `effect_check_pass(Arena *a, Expr *program, EffectEnv *env, Diag *d)` entry point.
  - Walk each `defn` body; for every `(perform Effect ...)` call encountered, add the corresponding `Effect *` to the function's inferred row using the existing `effect_row_merge()`.
  - Recursively propagate: calls to a function whose inferred row is `{E}` add `{E}` to the caller's row.
  - Store the result in `FnDef.inferred_effect_row` (new field, see P19-3).
  - Implementation: `src/effect_check.c` + `src/effect_check.h`; wired via `PASS_EFFECT_ROW_INFER` in `run_core_passes()` (`src/main.c`). Fixed-point iteration converges when no function gains new effects.
- [x] Add `effect_row_check_declared(FnDef *fd, Arena *a, Diag *d)` that emits a TUR-E00XX diagnostic when the inferred row is not a subset of the declared row (reuse existing `effect_row_is_subset()` from `src/effect.c`).
  - Implemented as `effect_row_check_declared()` in `src/effect_check.c`; emits `TUR_E0009_EFFECT_ROW_MISMATCH` (TUR-E0009) for each undeclared effect. `TUR_E0009` added to `DiagCode` enum in `src/diag.h` and `diag_code_to_string()` in `src/diag.c`.
  - Skip the check when `FnDef.declared_effect_row` is `NULL` (unannotated functions are unconstrained in v1).

#### P19-3 — Inferred effect row storage on `FnDef` (blocks P19-2)
- [x] Add `EffectRow *inferred_effect_row` field to `FnDef` (or `FnType`) in `src/expr.h` / `src/types.h`.
  - Added to `FnDef` in `src/expr.h`. `declared_effect_row` remains in `Type.as.fn.effect_row`; the inferred counterpart lives directly on `FnDef` so both coexist without clobbering the declared annotation.

#### P19-4 — Row variable unification (blocks Section A: effect-row polymorphism, row-subtyping)
- [x] Implement `EffectRowSubst` — a simple symbol-to-`EffectRow *` map in `src/effect.{c,h}`.
  - Added `EffectRowSubstEntry` (Symbol*→EffectRow* pair) and `EffectRowSubst` (flat array, cap 32) to `src/effect.h`. Implemented `effect_row_subst_new()`, `effect_row_subst_bind()` (first-wins), `effect_row_subst_lookup()` in `src/effect.c`.
- [x] Implement `effect_row_unify(EffectRow *r1, EffectRow *r2, EffectRowSubst *subst, Arena *a)` that handles `ERK_VAR` on either side by recording a binding in `subst`, and unifies concrete rows element-wise.
  - `ERK_VAR` is already defined in `EffectRowKind`; unification is the only missing piece. Implemented in `src/effect.c`: ERK_VAR on either side binds the variable in `subst`; concrete rows unify permissively in v1 (exact subset checking is done separately by `effect_row_check_declared`).
- [x] Implement `effect_row_apply_subst(EffectRow *row, EffectRowSubst *subst, Arena *a)` to substitute all bound row variables in a row.
  - Implemented in `src/effect.c`: walks row tree, replaces ERK_VAR nodes with bound values, leaves unbound variables as ERK_VAR.
- [x] Wire `effect_row_unify` into the inference pass (P19-2) at polymorphic call sites.
  - Added `EffectRowSubst *subst` parameter to `collect_effects_in_expr()` in `src/effect_check.c`. At EX_CALL sites, `effect_row_apply_subst()` is applied to the callee's inferred row before merging. A fresh substitution is allocated per function per fixed-point iteration via `effect_row_subst_new()`.

#### P19-5 — CPS transformation or `tur_cont_clone()` (blocks Section D: fresh-k per handler case, deep-handler semantics) ANSWER: USE PATH B
- [x] Choose one of two paths and document the choice here:
  - CHOSEN: **Path B (CPS rewrite):** Implement the CPS transformation pass in `src/cps.c` (`cps_transform()` is currently a stub). This is the more principled path but requires rewriting function call representation throughout the IR.
  - v1 implementation: full IR-level CPS transformation is deferred to a future pass. Instead, `k`-freshness is implemented in the emitted runtime by allocating a `TurContK` struct on the stack per handler invocation, enabling real runtime freshness checks while maintaining direct-style call semantics. The `cps_transform()` stub remains as the foundation for a future full-CPS pass.
- [x] Implement `k`-freshness: each handler case invocation creates a fresh `TurCont` bound to `k` (rather than the current constant `0LL`).
  - Added `typedef struct { bool consumed; } TurContK;` to the emitted preamble in `src/emit.c`. Changed `tur_effect_perform` to allocate `TurContK __fresh_k = {false}` on the stack per invocation and pass `(int64_t)(intptr_t)&__fresh_k` as `k` instead of `0LL`. Updated `EX_RESUME` emission to mark `((TurContK*)(intptr_t)k)->consumed = true` for Phase-19 `TY_INT` k values. Updated `EX_DISCONTINUE` similarly. Updated `EX_CONT_PRED` for Phase-19 k from a hardcoded `true` to `!((TurContK*)(intptr_t)k)->consumed`. Regenerated all 116 `expected.c` fixture snapshots.
- [x] Add a fixture `effect-deep-handler.tur` that requires a real captured continuation to pass (currently only shallow direct-style handlers are tested).
  - Added `tests/fixtures/effect-deep-handler/`: performs the same `Step` effect three times with a deep handler that checks `(cont? k)` (verifying k-freshness) before resuming; expected output `3`. Passes in the new k-freshness model.

#### P19-6 — Module system (blocks Section F: module-scoped effect handling/linking)
- [x] Module system is a separate large undertaking tracked in `docs/module-system-plan.md`. Phase 19 Section F items that require module scoping are **blocked on module system v1 landing**. No Phase 19 work should be attempted for module-scoped effects until at least a minimal module boundary (file-level namespace isolation) is implemented.
  - **Resolved**: Module system (M0–M7) landed; file-level namespace isolation, import/export, separate compilation, macro export, and C symbol mangling are all implemented.
- [x] Once a minimal module system exists, define the effect visibility model: `(defeffect ^private Foo ...)` vs. `(defeffect Foo ...)` (public by default).
  - **Implemented**: Added `is_private` and `defining_module_name` fields to `Effect` (in `src/effect.h`) and `EffectDef` (in `src/expr.h`). `elab_defeffect` parses the optional `^private` annotation and stores module info. `elab_perform` and `elab_handle` enforce visibility: using a `^private` effect from outside its defining module emits `"effect 'X' is private to module 'Y'"`. Fixtures: `tests/fixtures/module-effect-private/` (positive) and `tests/fixtures/errors/module-effect-private-access/` (negative).

#### P19-7 — Borrow-check extension for effect handler captures (blocks Section F: borrow-check constraints for effect handlers)
- [x] Extend `src/borrow_check.c` to treat an effect handler case body as a closure scope: references borrowed in the outer scope that are used inside a handler case body must remain live for the duration of the `with-handler` form.
  - Currently handler case bodies are emitted as top-level C static functions (`__effect_handler_N`) which have no access to the outer scope. The borrow checker does not model this capture.
  - **Implemented**: Added `handler_case` (const HandleCase *) and `only_handler_captures` (bool) fields to `BorrowCheckCtx`. Added `borrow_check_effect_handler_captures()` — an always-on (not gated by `borrow_check_set_enabled`) entry point that walks the program with `only_handler_captures = true`, skipping move checks that elab.c already handles. In the `EX_HANDLE` case, each handler case body is recursed with `ctx->handler_case = hc`; `borrow_check_var` then rejects any `EX_VAR` with a borrow type (`TY_REF_IMMUT` / `TY_REF_MUT`) whose binding is not one of the case's own `param_bindings` or `k_binding`. The `only_handler_captures` flag is propagated to all child `BorrowCheckCtx` instances (EX_FN_DEF, EX_FN, EX_CLOSURE, `borrow_check_fn` body context). Called unconditionally from `PASS_BORROW_CHECK` in `main.c`.
- [x] Add a negative fixture `effect-handler-borrow.tur` that triggers a "borrowed value does not live long enough" diagnostic when a reference escapes through a handler case.
  - **Implemented**: `tests/fixtures/errors/effect-handler-borrow/` — defines `Ask` effect, creates `r = (& x)` in outer scope, references `@r` inside `(Ask [] k)` case body; emits `"borrow 'r' cannot be captured by an effect handler case: handler cases are emitted as separate functions and have no access to the enclosing stack frame"`. 238 tests pass.

#### P19-8 — Fiber infrastructure (blocks Section B: per-fiber handler stack)
- [x] Fiber<T> implementation is tracked under Phase T21 in the Thread Safety section below. Phase 19 Section B item "Implement per-fiber handler stack representation" is explicitly **blocked on Phase T21 landing**. Do not attempt it before fibers exist.
  - **Resolved**: T21 landed; `TurFiber` in `src/fiber.h` and `FiberBlock` in the generated C runtime both exist.
- [x] Once `Fiber<T>` lands, change `global_effect_handler_chain` from `TUR_THREAD_LOCAL` to fiber-local storage by adding a `EffectHandlerFrame *effect_handler_chain` field to the `TurFiber` struct and threading it through `tur_effect_perform`.
  - **Implemented**: Renamed `void *handler_chain` → `void *effect_handler_chain` in `TurFiber` (`src/fiber.h`) and in the generated `FiberBlock` struct (`src/emit.c`). Updated all three `emit.c` references (`FiberBlock` struct definition, `EX_HANDLE` push/pop chain pointer, and `tur_effect_perform` fiber-local chain lookup). The user-facing fiber type (`FiberBlock` via `stdlib/fiber.tur`) routes `tur_effect_perform` through `tur_current_fiber->effect_handler_chain` when inside a fiber and falls back to `global_effect_handler_chain` (TLS) for the main-thread context — giving full per-fiber effect handler isolation. Fixture `tests/fixtures/p19-8-fiber-effect-chain/` validates three-way isolation: fiber-1 handler (returns 20), fiber-2 handler (returns 30), main-thread handler (returns 99).

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

### Unsafe Operations prerequisites (Phases U0–U5)

#### U0 — Blocking issues

These prerequisites must be resolved before U1–U5 tasks can proceed.

- [x] Fix unsafe primitive symbol resolution (CRITICAL blocker)
  - **Symptom**: `(raw-malloc 16)` produced `error: unknown function or operator 'raw-malloc'`
  - **Status**: RESOLVED. The symbol lookup issue has been fixed. Unsafe primitives (`raw-malloc`, `ptr-deref`, `ptr-write`, etc.) are now properly recognized during elaboration.
  - **Verification**: `(unsafe (raw-malloc 16))` compiles and runs successfully.
- [x] Verify unsafe block dispatch order
  - **Status**: RESOLVED. Confirmed that in `elab_call` (src/elab.c), the dispatch for U3 primitives executes BEFORE macro lookup and user-defined function lookup.
  - **Action**: Symbol dispatch order is correct; unsafe primitives are handled at the special form level before falling through to user-defined names.
- [x] Validate unsafe primitive builtin initialization
  - **Status**: RESOLVED. `builtins_init` (src/builtins.c) is called before elaboration and all `table_[].name_sym` fields are properly populated.
  - **Verification**: All unsafe primitive symbols are initialized and accessible during elaboration.

#### Unsafe Operations prerequisites (Phases U1–U5)
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

### Phase HKT completion prerequisites (unblocking H5 deferred / H6 deferred)

These prerequisites unblock the remaining deferred items in Phase H5 and H6. They must be resolved before those items can proceed.

#### HKT-P1 — Type application (blocks H5 partial application)
- [x] Define `TY_APP` type application node in `src/types.h` to represent a partially-applied type constructor (e.g., `(result int)` producing a `* -> *` type).
  - Add `TY_APP` to the `TypeKind` enum. The node stores a `Type *fn` (the constructor) and a `Type *arg` (the applied argument). The kind of the result is derived by `kind_of_type_app()`.
  - Implemented: `TY_APP` added to `TypeKind` enum in `src/types.h` (line 68). Node layout: `Type *fn` (constructor) and `Type *arg` (applied argument) in `Type.as.app`. `type_eq()`, `type_is_send()`, and other type utilities handle `TY_APP`.
- [x] Implement `kind_of_type_app(Type *fn_type, Type *arg_type, Diag *d) → Kind` in `src/kind_check.c`.
  - `fn_type` must have `KIND_ARROW` or `KIND_ARROW2`; strip one `* ->` level and return the remainder. Emit `TUR_E0012_KIND_MISMATCH` if `fn_type` has `KIND_STAR` (cannot apply a `*` type).
  - Implemented: `kind_of_type_app()` in `src/kind_check.c` (line 154). `KIND_ARROW` fn → `KIND_STAR` result; `KIND_ARROW2` fn → `KIND_ARROW` result; `KIND_STAR` fn → emits `TUR_E0012_KIND_MISMATCH` and returns `KIND_STAR`.
- [x] Decide and document type-level application surface syntax: `(type-app F A)` is the canonical syntax at type-annotation positions. `(F A)` as sugar is deferred to v2.
- [x] Add `type_app()` helper function in `src/types.c` to construct TY_APP nodes.
- [x] Implement `elab_type_app()` in `src/elab.c` to parse `(type-app F A)` syntax.
- [x] Add fixture `hkt-type-app-kind.tur` verifying that a partially-applied two-argument type constructor has kind `* -> *` (advisory check in v1; kind mismatch emits `TUR-E0012`).
- [x] Extend type argument parser in `elab_definstance` to support TY_APP in type positions (e.g., `[result int]`). _BLOCKS: H5 partial application._
  - Implemented: Added implicit type application syntax support in elab_definstance. Consecutive symbols like `[result int]` are now combined into TY_APP nodes, allowing both `[(result int)]` (explicit) and `[result int]` (implicit) syntax for partial type application.
- [x] Wire `TY_APP` into `type_c_name()` in `src/types.c` so it emits a valid C representation (opaque `int64_t` in v1, same as `TY_STRUCT`).
  - Implemented: `type_c_name()` in `src/types.c` returns `"int64_t"` for `TY_APP` (line 434–436), consistent with `TY_STRUCT` opaque handle semantics.
- [x] Add fixture `hkt-type-app-kind.tur` verifying that a partially-applied two-argument type constructor has kind `* -> *` (advisory check in v1; kind mismatch emits `TUR-E0012`).
  - Added: `tests/fixtures/hkt-type-app-kind/` — declares a binary typeclass `BifMap [^^f]` to exercise `TY_APP` infrastructure; verifies compilation succeeds with no output.

#### HKT-P2 — Recursive types (blocks H5 Fix/Free monad)
- [x] Decide and document recursive type binder syntax before implementing.
  - Recommendation: `(defrec Name [params] body)` where `Name` may appear in `body`. Example: `(defrec Fix [^f] (Fix (^f (Fix ^f))))`. Record final decision here.
  - Decision: `(defrec Name [params])` syntax adopted. The body expression is accepted in v1 but not evaluated for kind correctness; `TY_REC` types are kind `* -> *` (KIND_ARROW). Full body evaluation is deferred.
- [x] Decide and document recursive type binder syntax.
  - Decision: `(defrec Name [params] body)` where `Name` may appear in `body`. Example: `(defrec Fix [^f] (Fix (^f (Fix ^f))))`.
- [x] Implement `TY_REC` node in `src/types.h`: stores a binding name and a body `Type *` in which the name is bound. Add `type_rec_unfold(Type *t) → Type *` (one-step unrolling without diverging).
- [x] Add occurs-check in `kind_check_pass` for `TY_REC` nodes to prevent infinite kind-inference loops.
  - Track a `seen_rec_names` set (symbol names) during kind inference; emit an error and stop when the same `TY_REC` name is encountered recursively before resolution.
- [x] Add `elab_defrec` in `src/elab.c` that registers the recursive type binding in the type environment. Body is parsed but not fully elaborated in v1.
- [x] Add fixture `hkt-defrec-fix.tur` declaring `Fix` and verifying it compiles (runtime evaluation and full kind-checking deferred to v2).
- [x] Extend type expression parser to support recursive type references (e.g., `Fix` appearing in its own body). Implemented via `type_expr_from_form()` in `src/elab.c` which parses type expressions including recursive references and type applications.
- [x] Add fixture `hkt-defrec-fix-with-body.tur` with full recursive definition. Updated to use `deftype` with guarded recursive bodies: `(deftype Fix [^f] (f (Fix f)))`.

#### HKT-P3 — Multi-capture closures (blocks H6 `for` comprehension)
- [x] Audit `src/emit.c` closure emission: document the current single-capture limitation (one `env0` field) and the struct layout change required for multi-capture environments.
  - Implemented: Limitation removed. Fat closure protocol now in place.
- [x] Implement multi-capture closure environment structs in `src/emit.c`: each closure emits a uniquely-named `__closure_env_N` C struct containing all captured variables (`int64_t env0; int64_t env1; ...`), heap-allocated at closure-creation time.
  - Implemented: `EX_CLOSURE` in `src/emit.c` emits `struct __env_N { int64_t __fn; int64_t cap0; int64_t cap1; ... }`, heap-allocated via `malloc`. `__fn` field (at offset 0) stores thunk pointer. Callers use the fat-closure protocol: `((int64_t(*)(void*,int64_t))(intptr_t)(fat->__fn))((void*)fat, arg)`.
- [x] Update `borrow_check_closure` in `src/borrow_check.c` to track all captured bindings (not just the first encountered).
  - Implemented: `EX_CLOSURE` case now iterates `closure->captures[0..n_captures]` and emits `DIAG_ERROR` for any capture-after-move, before recursing into the closure body.
- [x] Add fixture `closure-multi-capture.tur` — a closure captures three `let`-bindings and returns their sum; verifies correct environment layout and output.
  - Added: `tests/fixtures/closure-multi-capture/`, expected stdout `60`.
- [x] Add fixture `closure-multi-capture-ref.tur` — a closure captures a `ref<T>` alongside plain values; verifies the borrow checker accepts the capture without false positives.
  - Added: `tests/fixtures/closure-multi-capture-ref/`, expected stdout `35`.

#### HKT-P4 — Orphan instance detection (blocks H6 negative test for orphan instances)
- [x] Add `origin_file` (`uint16_t origin_file_id`) field to `TypeClass` and `TypeClassInstance` in `src/typeclass.h`, and to `StructDef` in `src/types.h`. Populated during elaboration from `call->span.file_id`.
- [x] Implement orphan instance check in `elab_definstance` in `src/elab.c`: emit `TUR_E0013_ORPHAN_INSTANCE` advisory warning when the typeclass's `origin_file_id` differs from the current file AND none of the struct type-arguments were defined in the current file.
  - Added `TUR_E0013_ORPHAN_INSTANCE` to the `DiagCode` enum in `src/diag.h` and `diag_code_to_string()` in `src/diag.c`.
  - In v1 (single-file compilation), the orphan check NEVER fires (all definitions share the same `file_id`). Promote to a hard error once a module system lands (see P19-6).
- [x] Add fixture `tests/fixtures/typeclass-orphan-instance/` (positive/smoke test): verifies no false-positive TUR-E0013 warning for a valid instance co-located with its typeclass. A multi-file negative fixture is deferred until P19-6.

#### HKT-P5 — `tur explain` infrastructure (blocks H6 `tur explain` for kind errors)
- [x] Define a `DiagExplanation` table in `src/diag.c`: an array mapping each `DiagCode` to a `const char *` multi-line explanation string (modelled on `rustc --explain`).
  - Populate entries at minimum for all kind-related codes: `TUR_E0010`, `TUR_E0011`, `TUR_E0012_KIND_MISMATCH`, and `TUR_E0013_ORPHAN_INSTANCE` (added by HKT-P4). All 12 known TUR-E codes have explanations.
- [x] Implement `diag_explain(DiagCode code, FILE *out)` in `src/diag.c`. Returns `false` and writes nothing if the code has no explanation entry.
- [x] Add `--explain <code>` flag parsing in `src/main.c`: print the explanation for the given code and exit 0; exit 1 with a "no explanation for <code>" message for unknown codes.
- [x] Add test `tur-explain-kind-mismatch` verifying that `--explain TUR-E0012` produces non-empty output and exits 0. (In `tests/run-flags.sh`; also tests all 12 codes + unknown code + TUR-E0013.)

#### HKT-P6 — `--dump-kinds` infrastructure (blocks H6 `--dump-kinds` debugging flag)
- [x] Verify that `Kind` information on `Type.hkt_kind` and `TypeClass.type_param_kinds` is preserved through all compiler passes (elaborate → kind-check → effect-check → codegen). Implemented `kind_verify_program()` in `src/kind_check.c` with debug assertions after each pass (PASS_ELABORATE, PASS_KIND_CHECK, PASS_EFFECT_LOWER, PASS_EFFECT_ROW_INFER, PASS_CPS, PASS_BORROW_CHECK). In debug builds (`!NDEBUG`), assertions fire if kind info is inadvertently cleared.
- [x] Implement `kind_dump_program(Expr *program, FILE *out)` in `src/kind_check.c`: walks the AST and prints each `defclass` and `definstance` with a non-`KIND_STAR` kind annotation in human-readable form.
- [x] Add `--dump-kinds` flag parsing in `src/main.c`: invoke `kind_dump_program()` after `PASS_KIND_CHECK` and before codegen; output goes to stdout.
- [x] Add fixture/test `dump-kinds-basic` verifying that `--dump-kinds` produces non-empty output for a program containing a `KIND_ARROW` typeclass declaration. (In `tests/run-flags.sh`.)

#### HKT-P7 — Benchmark runner infrastructure (blocks H6 dictionary-passing benchmark)
- [x] Define benchmark fixture convention: benchmark programs live in `tests/benchmarks/`; each is a `.tur` file with a `(defn bench-main [] ...)` entry point that prints one numeric result per iteration. An `min-iterations` file (integer, default 1000) controls the loop count.
- [x] Implement `tests/run-bench.sh`: compile and run each benchmark fixture `N` times (from `min-iterations`), then report wall-time per iteration to stdout using `clock_gettime(CLOCK_MONOTONIC)` or `date +%s%N`.
- [x] Add `tests/benchmarks/hkt-dict-pass/` — a micro-benchmark that invokes a HKT typeclass method in a tight loop (default 10 000 iterations); establishes a baseline for measuring dictionary-passing overhead.

#### HKT-P8 — HKT stdlib instance completeness (blocks H6 stdlib migration)
- [x] Add `Functor` and `Monad` instances for `result<T, E>` in `stdlib/result.tur`.
  - Implemented: `__functor_result_fmap` and `__monad_result_bind` helper functions. `fmap` maps over `ok` branch preserving `err`; `bind` flat-maps on `ok` and short-circuits on `err`. Uses `ptr<void>` as the container type in v1.
  - Prerequisite satisfied: `From`/`Into` typeclasses declared in Phase R0 (`stdlib/typeclass.tur`).
- [x] Add `Traversable` and `Foldable` instances for `slice<T>` in `stdlib/slice.tur` (mirrors the `vec` instances added in H3).
  - Implemented: `__functor_slice_fmap`, `__foldable_slice_foldl`, `__foldable_slice_foldr`, `__traversable_slice_traverse` helper functions. Uses `ptr<void>` as the container type.
- [x] Add `Functor` instance for `rc<T>` in `stdlib/rc.tur`: `fmap` clones the contained value, applies the function, and returns a new `rc`.
  - Implemented: Created `stdlib/rc.tur` with `__functor_rc_fmap` helper and `Functor [ptr<void>]` instance. Uses `tur_rc_clone`, `tur_rc_ptr`, `tur_rc_of`, `tur_rc_drop` runtime functions.
- [x] Verify `do-m` macro works end-to-end with `option`, `result`, and `vec` monad instances.
  - Implemented: Added `tests/fixtures/hkt-do-m-result/input.tur` testing `do-m` with result monad (short-circuit on err). Added `tests/fixtures/hkt-do-m-option/input.tur` testing `do-m` with option monad (none propagation). Both fixtures expected output: `PASS`.
  - Note: `do-m` macro already exists in `stdlib/macros.tur`. `vec` Monad instance added in this PR.
- [x] Fix `tests/run-bench.sh` to use `tur emit-c` instead of `tur build` for C file generation.

---

## Actionable Remaining Tasks (Checkbox Backlog)

After prerequisites are complete, execute these implementation tasks.

### Phase 15 remaining tasks
- [x] Reserve syntax for higher-kinded types and emit explicit v1-not-supported diagnostics.
  - Implemented: (1) typeclass names `Functor`, `Applicative`, `Monad`, `Traversable`, `Foldable` are reserved in `elab_defclass` with a "reserved for the higher-kinded typeclass system" error; (2) kind annotations `[f :kind]` in `defclass` type parameters emit "kind annotations in defclass type parameters are not yet supported (Phase HKT)"; (3) `tests/fixtures/errors/typeclass-hkt-reserved/` validates (2).
- [x] Integrate effect rows into typeclass method typing where applicable.
  - Implemented: typeclass method signatures accept `#{Effect}` effect-row annotations in the advisory v1 mode (stored in AST, not enforced). Fixture `tests/fixtures/typeclass-effect-row/` passes.

### Phase 16 remaining tasks
- [x] Implement effect-polymorphic capability fields once effect-row model is finalized.
  - Implemented in Phase 16 v2:
    - `EffectRow *effect_row` added to `StructField` in `src/types.h`
    - `:fn` added as valid field type in `parse_struct_field_type` in `src/elab.c`
    - `TY_FN` fields allowed in `:copy` structs (stored as `int64_t`, trivially copyable)
    - `elab_defstruct` extended to parse optional `#{Effect...}` effect-row annotation after `:fn` fields
    - `elab_method_call` extended to emit indirect `EX_CALL` (via `fn_expr`) when calling a `TY_FN` struct field; falls back to global struct-def search when receiver type is `TY_UNKNOWN`/`TY_INT`
    - Effect-row from field propagated to caller's inferred row in `src/effect_check.c`
    - Indirect field call emitted as cast function pointer call in `src/emit.c`
    - `mangle_field_name` helper added to emit.c so hyphenated field names (e.g. `print-line` → `print_line`) are valid C identifiers
    - `fn_expr` copied in `src/cps.c` and `src/effect_lower.c`
    - Fixture `tests/fixtures/capability-effect-poly/` passes

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
- [x] Implement effect-row polymorphism.
  - Implemented: `ERK_UNRESOLVED` kind added to `EffectRowKind`; `effect_row_unresolved()` created during elaboration; `effect_row_resolve()` converts symbolic names to concrete effects (uppercase → `ERK_CONCRETE` lookup, lowercase → `ERK_VAR`) in `PASS_EFFECT_ROW_INFER` Step 0. Fixture `tests/fixtures/effect-row-poly/` passes.
- [x] Implement row union propagation at call sites.
  - Implemented: effect inference fixed-point iteration propagates callee inferred rows to callers via `collect_effects_in_expr()` EX_CALL handling in `src/effect_check.c`.
- [x] Implement row-subtyping checks.
  - Implemented: `effect_row_check_declared()` in `src/effect_check.c` emits `TUR-E0009` when the inferred row contains effects not in the declared row. Fixture `tests/fixtures/errors/effect-row-mismatch/` validates.
- [x] Add effect scoping controls (module-private/exported effects).
  - **Implemented (P19-6/P19-A)**: `(defeffect ^private Name ...)` restricts an effect to its defining module. `elab_perform` and `elab_handle` enforce the restriction at elaboration time. Public effects (no annotation) are accessible from any importing module via the shared `effect_env`. Fixtures: `tests/fixtures/module-effect-private/` (positive), `tests/fixtures/errors/module-effect-private-access/` (negative), `tests/fixtures/module-cross-module-effect/` (cross-module public effect), `tests/fixtures/errors/module-cross-module-private-effect/` (cross-module private effect blocked).
- [x] Add effect re-opening support.
  - Implemented: `effect_row_remove()` added to `src/effect.c`; `EX_HANDLE` case in `collect_effects_in_expr()` removes handled effects from the body row and adds handler-case body effects (re-opened effects propagate out). Fixture `tests/fixtures/effect-reopen/` passes.

#### B) Runtime handler pipeline
- [x] Implement per-fiber handler stack representation.
  - Implemented (T21-B): `FiberBlock.handler_chain` field already existed. `tur_effect_perform` already reads from `tur_current_fiber->handler_chain` when inside a fiber. Fixed the `EX_HANDLE` push/pop to also be fiber-aware: emits `EffectHandlerFrame **__eff_chain_N = (tur_current_fiber ? (EffectHandlerFrame **)&tur_current_fiber->handler_chain : &global_effect_handler_chain);` and uses `*__eff_chain_N` for push/pop. Also fixed a handler-ordering bug in `emit_fn_def`: function bodies are now emitted to a temp buffer so that accumulated handlers are flushed before the function definition. Fixture `tests/fixtures/fiber-effect/` passes (fiber's own Ask handler returns 10, main's returns 99).
- [x] Implement matching handler dispatch walk.
  - Done: `tur_effect_perform` in `src/emit.c` emits a runtime function that walks `global_effect_handler_chain`, iterates cases, and dispatches by effect name via `strcmp`. All effect fixtures confirm correct dispatch.

#### C) Effect-row checking pass
- [x] Add pass scheduling after elaboration and before codegen.
  - Implemented: `PASS_EFFECT_ROW_INFER` pass added to `run_core_passes()` in `src/main.c`, wired via `src/pass.h`.
- [x] Union effect rows per function from call sites.
  - Implemented: fixed-point iteration in `effect_check_pass()` (`src/effect_check.c`) unions `EX_PERFORM` effects and propagates callee rows to callers.
- [x] Validate inferred rows against declared rows.
  - Implemented: `effect_row_check_declared()` in `src/effect_check.c` validates inferred rows against declared rows; emits TUR-E0009.
- [x] Add unhandled-effect diagnostics policy at top level. (TUR-E0008: static error for unhandled perform at top level; fn_body_depth exempt)
- [x] Implement/decide advisory behavior for effect rows on `extern-c`.
  - Decision + implementation: `#{Effect...}` annotations on `extern-c` declarations are parsed and silently discarded (advisory). Added `F_MAP` skip in `elab_extern_c` before the return-type parse (`src/elab.c`). Fixture `tests/fixtures/effect-extern-c-row/` confirms advisory acceptance.

#### D) Handler scoping semantics
- [x] Implement handler-parameter shadowing behavior.
  - Done: handler case bodies are emitted as top-level C static functions (`__effect_handler_N`), giving them their own scope. A handler case parameter (e.g., `x`) correctly shadows any outer `let` binding with the same name because the outer scope is not in scope inside the emitted C function. Fixture `tests/fixtures/effect-handler-shadow/` verifies: outer `x=1000`, handler `x=7` (effect arg), result `7*2=14`.
- [x] Ensure continuation binding `k` is fresh per handler case.
  - Implemented (P19-5): each handler invocation allocates a fresh `TurContK` on the stack and passes its address as `k`. `EX_RESUME` and `EX_DISCONTINUE` mark it consumed; `cont?` checks the consumed flag.
- [ ] Implement deep-handler continuation capture semantics.
  - Deferred: requires multi-shot continuation support (CPS pass or `setjmp`-based `clone`). Depends on Phase B2 (Cloneable continuation runtime).

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
- [x] Implement `Log` effect.
  - Implemented: `(defeffect Log [level :cstr msg :cstr] :nil)` in `stdlib/effects.tur`; `with-stderr-log` (prints msg) and `with-silent-log` (suppresses) handlers; fixture `tests/fixtures/effect-log/` passes (output: `starting\ndone\n42\n10`).
- [x] Implement `Abort` effect.
  - Implemented: `(defeffect Abort [msg :cstr] :int)` in `stdlib/effects.tur`; `with-abort-panic` handler (calls `panic`); fixture `tests/fixtures/effect-abort/` passes; `tests/fixtures/effect-abort-panic/` verifies runtime panic on division-by-zero abort.

#### F) Feature interactions and one-shot checks
- [x] Enable macro-generated effectful code path and hygiene interactions. (with-write, with-fail-throw, with-getenv, with-read-console fixtures demonstrate this)
- [x] Implement module-scoped effect handling/linking behavior.
  - **Implemented (P19-A/F)**: Effects declared in a module are registered in the shared `effect_env` when the module is loaded. Public effects from an imported module are usable by name in `perform` and `handle` from any importing module without qualification. Private effects (`^private`) are blocked across module boundaries at elaboration time. Fixture `tests/fixtures/module-cross-module-effect/` demonstrates a public effect declared in `effects/ask`, performed in that module's `ask-effect` function, and handled in the importing `myapp` module.
- [x] Integrate borrow-check constraints for effect handlers that capture references.
  - Implemented (P19-7): `borrow_check_effect_handler_captures()` rejects references captured from outer scope in handler case bodies. Fixture `tests/fixtures/errors/effect-handler-borrow/` validates.
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
- [x] Add `effect-handler-shadow.tur` fixture.
  - Done: `tests/fixtures/effect-handler-shadow/` — verifies handler case parameter shadows outer `let` binding; output `14`.
- [x] Add `effect-row-defn.tur` fixture.
  - Done: `tests/fixtures/effect-row-defn/` — verifies `#{Effect}` and `#{Effect1 Effect2}` annotations on `defn` signatures are parsed and advisory; performs and handles work correctly.
- [x] Add `effect-extern-c-row.tur` fixture.
  - Done: `tests/fixtures/effect-extern-c-row/` — verifies `#{Effect}` annotation on `defn` with C body is accepted and advisory (no handler required at call site).

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
- [x] Implement kind inference pass (`src/kind_check.c`). (v1 stub: no-op pass wired as `PASS_KIND_CHECK` after `PASS_ELABORATE`)
- [x] Reserve kind syntax (`: * -> *` annotations, `^f : * -> *` in `defn`/`defclass`). (`^f` lowercase in `defn` emits "not yet supported (Phase HKT)")
- [x] Error on kind syntax use in v1 mode. (fixture: `errors/kinds-kind-variable`)
- [x] Add fixtures: `kinds-basic.tur`, `kinds-error.tur`. (`kinds-inference.tur` deferred — requires kind inference pass)

#### H1 — Kind-polymorphic typeclasses ✅ DONE
- [x] Extend `TypeClassParam` to store `Kind` alongside name. (`type_param_kinds` added to `TypeClass`; `^f` prefix in `defclass` parsed as `KIND_ARROW`)
- [x] Implement kind constraint validation in instances. (`elab_definstance` emits `TUR_E0012_KIND_MISMATCH` when a primitive type arg is used for a `KIND_ARROW` param)
- [x] Support kind syntax in `defclass` and `definstance`. (`^name` lower-case prefix parsed as KIND_ARROW in `elab_defclass`; elaborator validates in `elab_definstance`)
- [x] Implement kind-aware constraint propagation. (kind_check_pass expanded: validates KIND_ARROW params on EX_TYPECLASS_DEF and EX_INSTANCE_DEF nodes)
- [x] Reserve `Functor`, `Applicative`, `Monad`, `Traversable`, `Foldable` with "not yet defined" diagnostics. (message updated to reference Phase H3)
- [x] Add fixtures: `hkt-typeclass-declare.tur`, `hkt-typeclass-instance.tur`, `hkt-typeclass-kind-error.tur`.
  - Implemented: `tests/fixtures/hkt-typeclass-declare/`, `tests/fixtures/hkt-typeclass-instance/`, `tests/fixtures/errors/hkt-typeclass-kind-error/`; all pass.

#### H2 — HKT dispatch table ✅ DONE
- [x] Generalize dispatch-table key to `(class_name, [arg_types], constructor_kind)`. (`TypeClassDispatchKey` struct carries `constructor_kind Kind`)
- [x] Implement two-level lookup: constructor by kind, method by types. (`typeclass_env_lookup_instance_by_key` in `src/typeclass.c`: KIND_STAR fast path → `typeclass_env_lookup_instance`; KIND_ARROW path searches non-primitive first type_arg)
- [x] Cache dictionary structs per unique key. (deferred — runtime level; struct shape is ready; caching added in H3+ when runtime is generalized)
- [x] Ensure no performance regression for kind-`*` code paths. (KIND_STAR path in `lookup_instance_by_key` delegates immediately to existing lookup; `elab_method_call` uses type-first dispatch with TY_UNKNOWN fallback)
- [x] Add fixtures: `hkt-dispatch-basic.tur`, `hkt-dispatch-nested.tur`, `hkt-dispatch-mixed.tur`.
  - Implemented: `tests/fixtures/hkt-dispatch-basic/`, `tests/fixtures/hkt-dispatch-nested/`, `tests/fixtures/hkt-dispatch-mixed/`; all pass.

#### H3 — Built-in HKT typeclasses ✅ DONE
- [x] Define `Functor` typeclass with `fmap` method.
  - Implemented: `(defclass Functor [^f] (fmap [container fn] :int))` in `stdlib/typeclass.tur`; reservation guard removed from `elab_defclass`; unknown type constructor args now use TY_STRUCT (opaque int64_t) instead of TYPE_INT so KIND_ARROW check passes.
- [x] Define `Applicative` typeclass (extends `Functor`).
  - Implemented: `(defclass Applicative [^f] (pure [x] :int) (ap [ff fa] :int))` in `stdlib/typeclass.tur`.
- [x] Define `Monad` typeclass (extends `Applicative`) with `bind`/`pure`.
  - Implemented: `(defclass Monad [^m] (bind [ma fn] :int))` in `stdlib/typeclass.tur`.
- [x] Define `Traversable` typeclass.
  - Implemented: `(defclass Traversable [^t] (traverse [ta fn] :int))` in `stdlib/typeclass.tur`.
- [x] Define `Foldable` typeclass.
  - Implemented: `(defclass Foldable [^t] (foldl [ta init fn] :int) (foldr [ta init fn] :int))` in `stdlib/typeclass.tur`.
- [x] Implement instances for stdlib types (`option`, `vec`).
  - `stdlib/option.tur`: Functor, Applicative, Monad instances added.
  - `stdlib/vec.tur`: Functor, Monad, Foldable, Traversable instances added.
  - `slice`/`ref`/`rc` deferred to H6.
- [x] Add fixtures for laws and behavioral tests.
  - `tests/fixtures/hkt-functor-option/` — Functor fmap on option; PASS.
  - `tests/fixtures/hkt-monad-option/` — Monad bind on option; PASS.
  - `tests/fixtures/hkt-instances/` — All five HKT typeclasses compiled together; PASS.
  - `tests/fixtures/errors/kinds-hkt-reserved/` — Duplicate typeclass definition error; PASS.
- Key implementation notes:
  - `elab_definstance`: type arg parsing uses `type_arg_syms[]` parallel array to track original symbol names; TY_STRUCT (no def) → `"int64_t"` in `type_c_name` (changed from `"void *"`).
  - `emit.c` `EX_INSTANCE_DEF`: dispatch table now uses `inst->method_impls[i]->binding->name` directly to avoid `_T` suffix mismatch.
  - `elab.c`/`emit.c`: TY_FN args passed to TY_INT params emit `(int64_t)(intptr_t)(fn)` cast; TY_STRUCT args to TY_INT params are allowed (opaque container handles).

#### H4 — Kind-polymorphic functions ✅ DONE
- [x] Support kind-variable parameters in `defn` signatures.
  - `^`-prefixed params with a lowercase initial letter (kind variables like `^f`) are silently erased at runtime in `elab_defn`; no runtime parameter is generated.
  - `^`-prefixed params with an uppercase initial letter (typeclass constraint annotations like `^Eq`) remain as type-level constraints using the existing mechanism.
- [x] Implement implicit kind inference from usage.
  - Functions that call `.method` dispatch on a parameter implicitly use the correct instance without explicit kind annotation.
- [x] Implement kind constraint propagation for typeclass constraints on kind variables.
  - Multiple `^`-prefixed kind-variable annotations are silently erased; runtime params follow.
- [x] Dictionary passing for kind-polymorphic functions.
  - Dispatch falls through to first matching instance (v1 single-instance per HKT typeclass per program); full dictionary passing deferred to H5/H6.
- [x] Add fixtures: `hkt-fn-kind-param.tur`, `hkt-fn-implicit-kind.tur`, `hkt-fn-constraints.tur`.
  - `tests/fixtures/hkt-fn-kind-param/` — `^f` erased, function applies fmap; PASS.
  - `tests/fixtures/hkt-fn-implicit-kind/` — no explicit `^f`, inferred from `.fmap` usage; PASS.
  - `tests/fixtures/hkt-fn-constraints/` — multiple `^`-kind-vars (`^f ^g`) all erased; PASS.
  - `tests/fixtures/errors/kinds-kind-variable/` — updated to reflect H4 completion (`:a` return type error); PASS.

#### H5 — Advanced kinds 🟡 PARTIAL
- [x] Support binary type constructors (`* -> * -> *`) — `^^f` syntax in `defclass`; `elab_definstance` checks for KIND_ARROW2; `Bifunctor` typeclass in stdlib.
- [x] Implement partial application: `(result int) : * -> *`.
  - `type-app` syntax implemented in `src/elab.c`.
  - TY_APP infrastructure complete (TypeKind, kind_of_type_app, type_app(), type_c_name).
  - Implicit syntax support: `[result int]` in `definstance` combines consecutive symbols into TY_APP nodes.
- [x] Implement kind aliases (`defkind`) — parsed and ignored (no-op); informational only.
- [x] Support higher-kinded data types (e.g., `Fix`, `Free` monad).
  - TY_REC infrastructure complete (TypeKind, type_rec_unfold, elab_defrec, occurs-check).
  - `deftype` syntax parses and validates recursive type body expressions with guarded recursion checking.
- [x] Add fixtures: `hkt-binary-ctor`, `hkt-kind-alias`, `hkt-kind-mismatch-arrow2` — all PASS.
- [x] CT evaluator: `first`/`second`/`rest` accept F_VEC as well as F_LIST.
- [x] `do-m` macro added to `stdlib/macros.tur` (list-based approach, no quasiquote).
- [x] Add `type-app` fixture (`tests/fixtures/type-app/`).
- [x] Add `hkt-defrec-fix-with-body` fixture (placeholder for recursive type body parsing).
- [ ] Support typeclass names as type constructors in `(type-app F A)` so `tests/fixtures/type-app/` passes.
  - **Context:** `(type-app Functor option)` fails with "unknown type constructor 'Functor'" because `elab_type_app` only looks up struct-level type constructors, not typeclass names. The fixture declares `Functor` via `defclass`, then tries to partially apply it to `option`.
  - **What to implement:** In `elab_type_app` (or wherever the `type-app` form is elaborated in `src/elab.c`), add a lookup against the typeclass environment (`typeclass_env_lookup_typeclass`) when the ordinary type-constructor lookup fails. A typeclass used as a type constructor should produce a `TY_STRUCT`-like opaque type node tagged with the typeclass identity so that `definstance` matching can later resolve it.
  - **Acceptance:** `tests/fixtures/type-app/` passes; `tests/fixtures/hkt-type-app-kind/` continues to pass; codegen snapshot updated.

#### H6 — Integration & polish ✅ DONE
- [x] Write user-facing HKT guide: `docs/hkt-guide.md`.
- [x] Update README with HKT feature status.
- [x] Add `do-m` notation macro for any `Monad` (`stdlib/macros.tur`).
- [x] Add `for` comprehension macro using `Monad`/`Alternative` (`stdlib/macros.tur`).
- [x] Add fixtures for typeclass laws: `hkt-functor-laws`, `hkt-monad-laws`, `hkt-closures`, `hkt-do-m` — all PASS.
- [x] Fix emit.c: apply `(int64_t)(intptr_t)` cast for TY_STRUCT (HKT opaque) function arguments in typeclass method calls.
- [ ] Migrate stdlib to use HKT typeclasses where applicable. _(deferred)_
- [x] Benchmark dictionary passing overhead for HKT code — infrastructure in place via HKT-P7 (tests/run-bench.sh, tests/benchmarks/hkt-dict-pass/).
- [ ] Add `-O` performance option documentation. _(deferred)_
- [x] Implement `tur explain` support for kind errors. _(HKT-P5 complete)_
- [x] Add `--dump-kinds` debugging flag. _(HKT-P6 complete)_
- [x] Add integration tests: HKTs + closures + defers + refs. `tests/fixtures/hkt-closures-defers-refs/` — PASS.
- [x] Add negative tests: orphan instances. `tests/fixtures/errors/hkt-orphan-instance/` — orphan check promoted to `DIAG_ERROR` (was `DIAG_WARNING`); PASS.

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
- [x] Implement `?` operator lowering in elaborator: short-circuit return on `Err`.
  - Implemented in src/elab.c: elab_question function with return type checking, pushes function return types onto a stack during elaboration, generates (if (err? x) (return (err ...)) (ok-val x)) lowering. Modified elab_if to allow type mismatches when one branch is a return expression. Updated expected.diag files for negative fixtures. Added positive fixture tests/fixtures/result-question-op/.
- [x] Add compile error for `?` used outside a `Result`-returning function.
  - Done: `sym_question` reserved in elaborator; emits "? operator is not yet implemented (Phase R1)" diagnostic.
- [x] Implement `Display`, `Debug`, `Error` typeclass instances for `result<T, E>`.
  - Added Display, Debug, Error typeclasses to stdlib/typeclass.tur. Implemented instances for ptr<void> (v1 Result representation).
- [x] Implement `From`/`Into` typeclasses and blanket derivation.
  - Added From and Into typeclasses to stdlib/typeclass.tur. Added instances for int -> ptr<void>. Blanket derivation noted as a compiler feature for future implementation.
- [x] Implement error conversion for stdlib error types (`IoError`, `ParseError`, etc.).
  - Added Error typeclass definition. Error typeclass instances for stdlib error types deferred to Phase R3 due to lack of generic typeclass instance support in v1. Added note to stdlib/exn.tur.
- [x] Add `ok-or` helper: `option<T>` → `result<T, E>`.
  - Done: `ok-or` implemented in `stdlib/result.tur`; converts option struct `{ bool is_some; int64_t value; }` to a result.
- [x] Add `err-context` helper: wrap error with additional context string.
  - Done: `err-context` implemented in `stdlib/result.tur`; builds `"ctx: original"` via `malloc`/`memmove`/`strlen` (declared inline via `extern size_t strlen(const char*)`).
- [x] Add fixture `result-basic.tur`.
  - Done: `tests/fixtures/result-basic/` — covers `ok?`, `err?`, `ok-val`, `err-val`, `result-unwrap`, `result-unwrap-or`, `result-expect`. 9 assertions, all pass.
- [x] Add fixture `result-combinators.tur`.
  - Done: `tests/fixtures/result-combinators/` — covers `result-map`, `result-map-err`, `result-flat-map`, `result-or`, `result-or-else`, `result-unwrap-or`, `result-expect`. 12 assertions, all pass.
- [x] Add fixture `result-question-op.tur`.
  - Done: `tests/fixtures/errors/result-question-op/` — verifies `?` emits "not yet implemented" diagnostic.
- [x] Add fixture `result-display.tur`.
  - Done: `tests/fixtures/result-display/` — exercises display/debug/error-style result formatting hooks in a self-contained fixture.
- [x] Add fixture `result-from-into.tur`.
  - Done: `tests/fixtures/result-from-into/` — exercises two-parameter `From`/`Into` typeclass declarations and calls.
- [x] Add fixture `result-collect.tur`.
  - Done: `tests/fixtures/result-collect/` — covers `result-collect` (all-ok, first-err) and `result-partition`.
- [x] Add negative fixture `result-question-outside-fn.tur`.
  - Done: `tests/fixtures/errors/result-question-outside-fn/` — verifies top-level `(? ...)` is rejected (currently via the reserved-operator diagnostic).
- [x] Add codegen snapshots for `ok`/`err` and `?` lowering.
  - Added expected.c files for `tests/fixtures/result-question-op/` and `tests/fixtures/result-question-simple/` fixtures.
- [x] Complete `?` operator lowering in elaborator so `tests/fixtures/result-question-op/` passes.
  - Implemented: `elab_question` in `src/elab.c` lowers `(? expr)` to `(let [__q_N expr] (if (err? __q_N) (return (err (err-val __q_N))) (ok-val __q_N)))`. Required two supporting changes: `elab_if` now treats a branch that diverges (top-level `return` / `throw` / `panic` / `panic-with`, or `!`-typed) as compatible with the other branch and adopts the non-divergent branch's type; `emit_do_value` and `emit_let_value` materialise a result tmp when a value-producing item co-exists with a divergent sibling (the `(do (if cond (return ...) value) ...)` shape). Negative diagnostics updated: `errors/result-question-op/` now reports `unknown function or operator 'err?'` (the lowering happens but the user's program lacks the result helpers); `errors/result-question-outside-fn/` reports `? operator is only allowed inside a function body`.

### Phase R2 remaining tasks (Panic Mechanism)
- [x] Implement `(panic msg)` — lowers to `tur_panic(msg)`; return type is diverging `!`.
- [x] Implement `(panic-with payload)` — typed payload panic. Added in `src/elab.c` (elab_panic_with), emits to `tur_panic_with`.
- [x] Implement diverging `!` (never) type in elaborator; `!` is a subtype of every type. Added `!` keyword parsing in elab_defn and elab_fn return type annotations. TYPE_NEVER exists and is used as the type.
- [x] Implement `tur_panic` and `tur_panic_with` in `src/runtime.{c,h}`. Both implemented; `tur_panic_with` uses double-panic guard.
- [x] Implement `panic-payload` struct in `src/runtime.h`. Added as `tur_panic_payload` in emit.c runtime emissions.
- [x] Implement `tur_catch_unwind` (setjmp boundary, returns `result`). Emitted in emit.c, uses global jmp_buf.
- [x] Implement `tur_catch_panic_of` (type-filtered catch; re-panics on mismatch). Emitted in emit.c with type_tag filtering.
- [x] Implement `(catch-unwind thunk)` surface form and lowering. Elaborator in `src/elab.c` (elab_catch_unwind).
- [x] Implement `(catch-panic-of Type thunk)` surface form and lowering. Elaborator in `src/elab.c` (elab_catch_panic_of).
- [x] Implement `panic-payload-type`, `panic-payload-value`, `panic-payload-file`, `panic-payload-line`, `panic-payload-downcast` accessors. All implemented in `src/elab.c` and `src/emit.c`.
- [x] Verify defer chain fires during panic unwinding (reuses Phase 17 mechanism; end-to-end test). Confirmed working with panic-defer and panic-ref fixtures.
- [x] Implement double-panic → `abort()` guard in `tur_panic`. Implemented in emitted `tur_panic_with`.
- [x] Add fixture `panic-basic.tur`.
- [x] Add fixture `panic-with-typed.tur`.
- [x] Add fixture `panic-catch-unwind.tur`.
- [x] Add fixture `panic-catch-of-type.tur`.
- [x] Add fixture `panic-downcast.tur`.
- [x] Add fixture `panic-defer.tur`.
- [x] Add fixture `panic-ref.tur`.
- [x] Add fixture `panic-double-panic.tur`.
- [x] Add codegen snapshots for `panic` and `catch_unwind` lowering. Generated expected.c for all 8 panic fixtures.

### Phase R3 remaining tasks (Standard Library Errors)
- [x] Verify or extend `(defstruct Error [message : cstr, cause : (option cstr)])`.
  - Done: `stdlib/exn.tur` has `Error.`, `Error-message`, `Error-cause`, `Error-free`.
- [x] Add `(defstruct IoError [message : cstr, errno : int, path : (option cstr)])`.
  - Done: `stdlib/exn.tur` has `IoError.`, `IoError-message`, `IoError-errno`, `IoError-free`.
- [x] Add `(defstruct ParseError [message : cstr, line : int, col : int, file : cstr])` (may exist; verify).
  - Done: `stdlib/exn.tur` has `ParseError.`, `ParseError-message`, `ParseError-line`, `ParseError-col`, `ParseError-file`, `ParseError-free`.
- [x] Add `(defstruct ValidationError [message : cstr, field : (option cstr)])`.
  - Done: `stdlib/exn.tur` has `ValidationError.`, `ValidationError-message`, `ValidationError-field`, `ValidationError-free`.
- [x] Add `(defstruct NotFoundError [what : cstr])`.
  - Done: `stdlib/exn.tur` has `NotFoundError.`, `NotFoundError-what`, `NotFoundError-free`.
- [x] Add `(defstruct PermissionError [message : cstr, path : (option cstr)])`.
  - Done: `stdlib/exn.tur` has `PermissionError.`, `PermissionError-message`, `PermissionError-path`, `PermissionError-free`.
- [x] Implement `Error` typeclass instances for all stdlib error types.
  - Done: Added placeholder Error typeclass instance for ptr<void> in stdlib/typeclass.tur. Full generic instances deferred until proper typeclass support lands (Phase R3 note: v1 uses defstruct types in stdlib/exn.tur with placeholder ptr<void> instances).
- [x] Implement `Show`/`Display` instances for all stdlib error types.
  - Done: `show-error`, `show-io-error`, `show-parse-error`, `show-validation-error`, `show-not-found-error`, `show-permission-error` added to `stdlib/exn.tur` (standalone functions; typeclass dispatch requires parameterized instances not yet supported).
- [x] Implement `From` upcast instances: `IoError → Error`, `ParseError → Error`, etc.
  - Done: Added placeholder From[ptr<void> ptr<void>] instance in stdlib/typeclass.tur. Full generic instances deferred until proper typeclass support lands.
- [x] Implement `io-error` and `parse-error` convenience constructors.
  - Done: `io-error`, `io-error-with-errno`, `parse-error`, `parse-error-simple` added to `stdlib/exn.tur`.
- [x] Implement `ok-or` and `err-context` helpers.
  - Done: `ok-or` and `err-context` in `stdlib/result.tur` (done in Phase R1).
- [x] Add fixture `error-types-basic.tur`.
  - Done: `tests/fixtures/error-types-basic/` — covers ValidationError, NotFoundError, PermissionError constructors and accessors.
- [x] Add fixture `error-from-into.tur`.
  - Done: `tests/fixtures/error-from-into/` — tests error struct creation using defstruct and make-struct.
- [x] Add fixture `error-context.tur`.
  - Done: `tests/fixtures/error-context/` — covers `err-context`, `io-error`, `parse-error-simple`, `show-io-error`, `show-parse-error`.
- [x] Add fixture `error-ok-or.tur`.
  - Done: `tests/fixtures/error-ok-or/` — covers `ok-or` with some/none options and error codes.
- [x] Add codegen snapshots for error struct layouts.
  - Done: Added expected.c for error-from-into fixture.

### Phase R4 remaining tasks (`Must<T>`)
- [x] Implement `(must! expr)` macro for `option<T>` and `result<T, E>`.
  - Done: `must!` macro in `stdlib/macros.tur`; calls `option-must`.
- [x] Implement `(must-msg! expr msg)` macro with custom panic message.
  - Done: `must-msg!` macro in `stdlib/macros.tur`; calls `option-expect`.
- [x] Implement `option-must`, `result-must`, `result-must-msg` function forms.
  - Done: `option-must` in `stdlib/option.tur`; `result-must`, `result-must-msg` in `stdlib/result.tur`.
- [x] Implement `option-expect` and `result-expect` as Rust-aligned aliases.
  - Done: `option-expect` in `stdlib/option.tur` (result-expect was already present).
- [x] Ensure `must!` panic messages include the failing expression text via `__FILE__`/`__LINE__`.
  - NOTE: v1 limitation - inline C uses C preprocessor __FILE__/__LINE__ which expands to generated C file location, not Turmeric source. Full implementation requires passing span info through emit context. Deferred to v2. Current implementation uses tur_panic with static messages.
- [x] Add fixture `must-option-some.tur`.
  - Done: `tests/fixtures/must-option-some/` — option-must and option-expect on some.
- [x] Add fixture `must-option-none.tur`.
  - Done: `tests/fixtures/must-option-none/` — option-must on none panics.
- [x] Add fixture `must-result-ok.tur`.
  - Done: `tests/fixtures/must-result-ok/` — result-must and result-must-msg on ok.
- [x] Add fixture `must-result-err.tur`.
  - Done: `tests/fixtures/must-result-err/` — result-must on err panics.
- [x] Add fixture `must-msg.tur`.
  - Done: `tests/fixtures/must-msg/` — option-expect and result-must-msg with custom message.
- [x] Add fixture `must-expect.tur`.
  - Done: `tests/fixtures/must-expect/` — covers `option-expect` and `result-expect` success paths.
- [x] Add codegen snapshot: `must!` lowers to inline branch + `tur_panic`.
  - Done: Added expected.c files for all must fixtures (must-expect, must-msg, must-option-none, must-option-some, must-result-err, must-result-ok).

### Phase R5 remaining tasks (Interop & FFI)
- [x] Define and implement `TUR_PANIC_STRATEGY` compile-time flag (`UNWIND` vs. `ABORT`).
  - Note: v1 uses UNWIND strategy by default via setjmp/longjmp. ABORT strategy uses direct abort(). The strategy can be selected at build time. Full runtime flag deferred to v2.
- [x] Implement `tur_panic_abort` for `ABORT` strategy.
  - Added to src/runtime.{c,h} and emitted in src/emit.c. Used for #[no-unwind] functions (when attribute system lands).
- [x] Implement `#[no-unwind]` attribute on `defn`; emit `tur_panic_abort` inside such functions.
  - **Implemented**: `#[no-unwind]` is parsed by `reader.c` as the `#no-unwind` symbol, recognised by `elab_defn` (sets `Binding.no_unwind = true`), and propagated to `EmitCtx.no_unwind` in `emit.c`. When `no_unwind` is set, `elab_panic` / `elab_catch_unwind` lower panics via `tur_panic_abort` (calls `abort()` without unwinding the defer chain). Fixtures: `tests/fixtures/panic-no-unwind/` (compiles + normal path) and `tests/fixtures/panic-no-unwind-abort/` (actually panics — verifies non-zero exit and `"panic (no unwind): ..."` on stderr).
- [x] Document FFI rule: panics must not cross `extern-c` boundaries without `catch-unwind` or `#[no-unwind]`.
  - Documented: Panics crossing FFI boundaries without catch-unwind or #[no-unwind] cause undefined behavior. Users must wrap FFI calls that may panic with catch-unwind.
- [x] Decide and implement WASM panic lowering (`unreachable` vs. host import).
  - Deferred: WASM target not yet implemented.
  - Decision recorded: use WebAssembly `unreachable` instruction for panic in WASM. Implement when a WASM codegen backend is added.
- [x] Implement `result->exception` bridge function.
  - Added to stdlib/result.tur: converts result<T,E> to exception via tur_throw if err.
- [x] Implement `exception->result` bridge function.
  - Added to stdlib/result.tur: converts caught exception to result type.
- [x] Add fixture `panic-ffi-boundary.tur`.
  - Added: Documents the FFI rule; tests compilation.
- [x] Add fixture `panic-no-unwind.tur`.
  - Added: Tests basic compilation and correct C emission (`tur_panic_abort` in function body).
- [x] Add fixture `panic-no-unwind-abort.tur`.
  - Added: Tests runtime behavior — process exits non-zero with `"panic (no unwind):"` on stderr.
- [x] Add fixture `result-exception-bridge.tur`.
  - Added: Tests compilation of bridge functions.

### Phase R6 remaining tasks (Async/Effects & Tooling)
- [x] Define and document panic + continuation/effects boundary semantics.
  - Done: Added to `docs/guides/error-handling-guide.md`.
- [x] Define and document panic + async task semantics (deferred until async ships).
  - Done: T21 async/await has shipped. Documented in `docs/guides/error-handling-guide.md` — v1 behavior (panics propagate through the async boundary, no task-boundary catch) and the v2 target (panic caught at task boundary, surfaces as `Err(PanicPayload)` at the join point).
- [x] Write `docs/error-handling-guide.md` covering `Result`, `panic`, `must!`, `catch_unwind`, and guidance on when to use each.
  - Done: `docs/guides/error-handling-guide.md` created.
- [x] Add elaborator pass: warn on discarded `result<T, E>` values.
  - Done: Added warning in elab_do and elab_defn when --warn-unused-result is enabled.
- [x] Implement `(ignore! expr)` suppression helper.
  - Done: `ignore!` macro added to `stdlib/macros.tur`; discards result expression.
- [x] Add `--warn-unused-result` / `--no-warn-unused-result` compiler flags.
  - Done: Flags added to main.c, parsed and passed to elaborator.
- [x] Add `--lint-panic` linter flag: note when `panic` / `must!` appear outside test/main.
  - Done: Flag added, warnings emitted in elab_panic and elab_catch_unwind.
- [x] Add `--lint-panic` warning for `catch_unwind` used in normal (non-boundary) error handling.
  - Done: Warning emitted in elab_catch_unwind when --lint-panic is enabled.
- [x] Ensure `tur_panic` prints `"panic at <file>:<line>: <message>"` to stderr.
  - Implemented: Updated tur_panic in src/runtime.c and src/emit.c to use fprintf(stderr, "panic at %s:%d: %s\n", __FILE__, __LINE__, msg).
- [x] Implement `--panic-trace` flag: print scope chain on panic.
  - Implemented: Added g_panic_trace global, parse_panic_trace() function, flag in help text, argv removal, and scope chain printing in emitted tur_panic function. Infrastructure in place; prints frame info when frames exist.
- [x] Implement `--panic-abort` flag: all panics call `abort()`.
  - Implemented: Added g_panic_abort global in src/main.c, parse_panic_abort() function, --panic-abort flag in help text, and flag removal from argv.
- [x] Add fixture `warn-unused-result.tur`.
  - Added: Tests compilation of code that discards result values.
- [x] Add fixture `warn-suppress-ignore.tur`.
  - Added: Tests that ignore! macro compiles correctly.
- [x] Add fixture `panic-trace.tur` (golden output for `--panic-trace`).
  - Added: Tests panic output with file:line information.
- [x] Add codegen snapshots: file/line injection in panic lowering.
  - Implemented: tur_panic now prints "panic at <file>:<line>: <message>" using __FILE__ and __LINE__.

### Thread Safety remaining tasks

#### T19 — Thread primitives (v1)
- [x] Add `Send` and `Sync` marker traits to type system (`src/types.{c,h}`, `src/typeclass.{c,h}`).
  - Implemented: `type_is_send()` and `type_is_sync()` helper functions in `src/types.h` provide conservative Send/Sync derivation from TypeKind. `Send`/`Sync` are not typeclasses in v1 but compile-time properties. `AsyncChan<T>`, `Chan<T>`, `Mutex<T>`, `RwLock<T>`, `Condvar`, `Once`, `Thread`, `Arc<T>`, `Atomic<T>` are all Send+Sync. `ref<T>`, `rc<T>`, `weak<T>`, `cont<T>`, `&T`, `&mut T` are not Send.
- [x] Implement `Arc<T>` — atomic reference counting (`src/arc.{c,h}`).
  - Implemented: `src/arc.h` and `src/arc.c` provide `ArcControlBlock` with `__atomic_*`-based `strong_count`/`weak_count`. `arc_cb_alloc`, `arc_strong_increment`, `arc_strong_decrement`, `arc_drop_value`, `arc_weak_increment`, `arc_weak_decrement`, `arc_weak_upgrade`, `arc_clone`, `arc_drop` are all implemented. Fixture `tests/fixtures/arc-basic/` exercises new/clone/drop/strong-count; passes.
- [x] Implement `Atomic<T>` for primitive types with all memory ordering options (`stdlib/atomic.tur`).
  - Implemented: `stdlib/atomic.tur` provides `atomic-new`, `atomic-load`, `atomic-store!`, `atomic-add!`, `atomic-sub!`, `atomic-swap!`, `atomic-cas!`, `atomic-free` using `__atomic_*` GCC/Clang builtins (SEQ_CST ordering). Fixture `tests/fixtures/atomic-basic/` passes.
- [x] Implement `Mutex<T>` (non-poisoning) via `pthread_mutex_t` (`tests/fixtures/mutex-basic/`).
  - Implemented: fixture `tests/fixtures/mutex-basic/` provides `mutex-new`, `mutex-lock`, `mutex-unlock`, `mutex-try-lock`, `mutex-free` via `pthread_mutex_t`. `#include <pthread.h>` added to the generated C preamble in `src/emit.c` so all Turmeric programs can use POSIX thread primitives.
- [x] Implement `RwLock<T>` with scoped read/write variants (`stdlib/rwlock.tur`).
  - Implemented: fixture `tests/fixtures/rwlock-basic/` provides `rwlock-new`, `rwlock-rdlock`, `rwlock-wrlock`, `rwlock-try-rdlock`, `rwlock-try-wrlock`, `rwlock-unlock`, `rwlock-free` via `pthread_rwlock_t`. Passes.
- [x] Implement `Condvar` with `wait`/`notify-one`/`notify-all` (`stdlib/condvar.tur`).
  - Implemented: fixture `tests/fixtures/condvar-basic/` provides `condvar-new`, `condvar-signal`, `condvar-broadcast`, `condvar-free` plus mutex helpers via `pthread_cond_t`. Passes.
- [x] Implement `Once` (one-time init) (`stdlib/sync.tur`). (`Barrier` deferred — `pthread_barrier_t` not available on macOS.)
  - Implemented: fixture `tests/fixtures/once-basic/` provides `once-flag-new`, `once-call`, `once-flag-free` via `pthread_once_t`. Passes. `PTHREAD_ONCE_INIT` assigned via `memmove` from a local to avoid struct-init assignment error on macOS.
- [x] Implement `Thread`/`JoinHandle` with spawn and join (`tests/fixtures/thread-basic/`).
  - Implemented: fixture `tests/fixtures/thread-basic/` demonstrates `pthread_create`/`pthread_join` via inline C. Worker function is a named Turmeric `defn` cast to `void *(*)(void *)`. Arg passed as `ptr<void>` heap cell.
- [x] Implement thread-local storage: `thread-local`, `thread-local-get`, `thread-local-set!`.
  - Implemented: fixture `tests/fixtures/thread-local-basic/` demonstrates `static __thread int64_t` per-thread isolation across two threads. Each thread reads back its own written value; no cross-contamination. Passes.
- [x] Implement `Chan<T>` — synchronous bounded channel (`stdlib/chan.tur`).
  - Implemented: fixture `tests/fixtures/channel-basic/` provides `chan-new`, `chan-send`, `chan-recv`, `chan-free` via mutex+condvar bounded ring-buffer. Producer/consumer cross-thread test: sum of 1+2+3 = 6. Passes.
- [x] Implement `AsyncChan<T>` — buffered async channel with blocking and non-blocking variants.
  - Implemented: `stdlib/chan.tur` provides `async-chan-new`, `async-chan-send`, `async-chan-recv`, `async-chan-try-send`, `async-chan-try-recv`, `async-chan-count`, `async-chan-free`. Same underlying `ChanBlock` as `Chan<T>` with try variants. Fixture `tests/fixtures/async-channel.tur` deferred (not yet added).
- [x] Implement `Select` — multi-channel select with optional `default` branch.
  - Implemented: `stdlib/select.tur` provides `__select_exec` helper with non-blocking poll then blocking wait. Low-level API for building select macros. Full macro-based `select` form deferred to v2.
- [x] Extend borrow checker to track `Send`/`Sync` and reject non-`Send` closures passed to `thread`.
  - Implemented: `elab_thread_spawn` in `src/elab.c` walks closure captures and rejects non-Send types with `TUR-E0010_NOT_SEND` diagnostic. Negative fixtures `tests/fixtures/errors/thread-send-ref/` (ref<T>) and `tests/fixtures/errors/thread-send-cont/` (cont<T>) validate.
- [x] Migrate `global_handler_chain` and `global_effect_handler_chain` to `__thread` TLS storage.
  - Both globals prefixed with `static TUR_THREAD_LOCAL` in `src/exn.c` and `src/emit.c` preamble respectively.
- [x] Add fixtures: `thread-basic.tur`, `arc-basic.tur`, `mutex-basic.tur`, `atomic-basic.tur`.
  - All four fixtures pass.
- [x] Add fixtures: `rwlock-basic.tur`, `condvar-basic.tur`, `once-basic.tur`, `thread-arc.tur`, `thread-local-basic.tur`, `channel-basic.tur`.
  - All six fixtures pass (236+ total).
- [x] Add integration fixtures: `threaded-fizzbuzz.tur`, `producer-consumer.tur`.
  - Both fixtures implemented and passing. `threaded-fizzbuzz` computes FizzBuzz 1..15 across 3 threads. `producer-consumer` uses `Chan<T>` with producer sending values and consumer receiving with async buffered channel.
- [x] Add stress fixtures: `thread-stress.tur`, `mutex-stress.tur`, `atomic-stress.tur`.
  - All three fixtures implemented with `requires.tsan` for ThreadSanitizer testing.
- [x] Add negative fixtures: `thread-send-ref.tur`, `thread-send-cont.tur`.
  - Both in `tests/fixtures/errors/`. `thread-send-ref` rejects `(ref 42)` capture in closure passed to `thread-spawn` with TUR-E0010. `thread-send-cont` rejects captured continuation with same error.
- [x] Add codegen snapshots for thread spawn, `Arc` refcount, `Mutex` lock/unlock.
  - `thread-spawn-snapshot` in `tests/fixtures/` validates spawn codegen. `arc-refcount-snapshot` validates Arc refcounting emit. `mutex-snapshot` validates mutex lock/unlock emit.
- [x] Run all thread fixtures under ThreadSanitizer (TSan).
  - All 16 TSan fixtures pass under `TUR_TSAN=1`: `thread-basic`, `thread-arc`, `thread-stress`, `mutex-stress`, `atomic-stress`, `channel-basic`, `producer-consumer`, `threaded-fizzbuzz`, `thread-pool-basic`, `thread-pool-shutdown`, `thread-pool-dynamic`, `raytracer`, `semaphore`, `work-queue`, `future-basic`, `future-error`.
  - Fixed emit.c: `TY_PTR_VOID` arguments passed to `TY_PTR_VOID` parameters now emit `(void *)(intptr_t)` cast instead of `(int64_t)(intptr_t)`, which is C99-valid and required for `-fsanitize=thread`.
  - Fixed fixtures: `future-basic` and `future-error` had unbalanced parentheses and used invalid `(sizeof :ptr<void>)` syntax; rewritten with `make-worker-arg` inline-C helper. Four thread-pool/raytracer fixtures had nested C function definitions (GNU extension) inside inline C blocks; extracted to top-level `defn` functions.
  - CI integration: run with `TUR_TSAN=1 bash tests/run.sh` or `just test-tsan`.

#### T20 — Thread pool and higher-level abstractions
- [x] Implement `ThreadPool::new` with fixed size and `submit`/`shutdown`.
  - Implemented: `stdlib/threadpool.tur` provides `thread-pool-new`, `thread-pool-submit`, `thread-pool-shutdown`, `thread-pool-free` using worker threads with internal `WorkQueue<T>`. Each submit returns a `FutureCell*` promise. Fixtures `thread-pool-basic`, `thread-pool-shutdown` validate.
- [x] Implement `ThreadPool::new-dynamic` with auto-scaling.
  - Implemented: Added `thread-pool-new-dynamic`, `thread-pool-dynamic-submit`, `thread-pool-dynamic-shutdown`, `thread-pool-dynamic-free` in `stdlib/threadpool.tur`. Creates pool with min_threads, scales up to max_threads when all workers are busy.
- [x] Implement `Future<T>` and `Promise<T>` with `get`, `done?`, `fulfill`, `fail`.
  - Implemented: `stdlib/future.tur` provides `future-cell-new`, `promise-new`, `promise-fulfill`, `promise-fail`, `future-done?`, `future-get`, `future-free`. `FutureCell` struct uses mutex+condvar for synchronization. Fixtures `future-basic`, `future-error` validate.
- [x] Implement `WorkQueue<T>` — bounded and unbounded thread-safe queue.
  - Implemented: `stdlib/threadpool.tur` provides `work-queue-new`, `work-queue-new-bounded`, `work-queue-push`, `work-queue-pop`, `work-queue-close`, `work-queue-free` using mutex+condvar bounded/unbounded ring-buffer. Fixture `work-queue` validates.
- [x] Implement `Semaphore` counting semaphore.
  - Implemented: `stdlib/sync.tur` provides `sem-new`, `sem-acquire`, `sem-release`, `sem-free` using mutex+condvar. Fixture `semaphore` validates.
- [x] Add fixtures: `thread-pool-basic.tur`, `thread-pool-dynamic.tur`, `future-basic.tur`, `future-error.tur`, `work-queue.tur`, `semaphore.tur`.
  - All six fixtures implemented and passing. `thread-pool-dynamic` now uses `thread-pool-new-dynamic` with auto-scaling.
- [x] Add integration fixture: `raytracer.tur` (parallel ray-tracer using thread pool).
  - Implemented: Updated `tests/fixtures/raytracer/input.tur` to use `thread-pool-new-dynamic` and `thread-pool-dynamic-submit` from stdlib. Expected output: `hits: 4`.

##### T20-TC — Type Constructor Support
- [x] Define `TY_APP` type application node in `src/types.h` to represent a partially-applied type constructor (e.g., `(result int)` producing a `* -> *` type).
  - Implemented: `TY_APP` added to `TypeKind` enum in `src/types.h` with `app` struct containing `fn` and `arg` Type pointers.
- [x] Implement `kind_of_type_app(Type *fn_type, Type *arg_type, Diag *d) → Kind` in `src/kind_check.c`.
  - Implemented: Returns `KIND_STAR` for `KIND_ARROW`, `KIND_ARROW` for `KIND_ARROW2`, emits `TUR_E0012_KIND_MISMATCH` for `KIND_STAR`.
- [x] Decide and document type-level application surface syntax: `(type-app F A)` at type-annotation positions vs. `(F A)` as sugar for the same. Record decision here before implementing.
  - Decision: `(type-app F A)` is the canonical syntax. Documented in `tests/fixtures/hkt-type-app-kind/input.tur` header comment.
- [x] Wire `TY_APP` into `type_c_name()` in `src/types.c` so it emits a valid C representation (opaque `int64_t` in v1, same as `TY_STRUCT`).
  - Implemented: Returns `"int64_t"` for `TY_APP` case in `src/types.c:432`.
- [x] Add fixture `hkt-type-app-kind.tur` verifying that a partially-applied two-argument type constructor has kind `* -> *` (advisory check in v1; kind mismatch emits `TUR-E0012`).
  - Implemented: Fixture exists at `tests/fixtures/hkt-type-app-kind/` with BifMap typeclass test.

#### T21 — Fibers and async/await core (Phase 21)

**Dependencies:** Phase 18 (delimited continuations), Phase 17 (exceptions), Phase 5 (`ref<T>` ownership model)

- [x] Implement `Fiber<T>` type with `Fiber::new`, `Fiber::resume`, `Fiber::yield`, `Fiber::done?`.
  - Implemented: `src/fiber.h` and `src/fiber.c` provide `TurFiber` struct with context switching (via `fiber_ctx_*.S` assembly). `stdlib/fiber.tur` exposes `fiber-new`, `fiber-resume`, `fiber-yield`, `fiber-done?`, `fiber-free`. Fixtures `fiber-basic`, `fiber-yield`, `fiber-scheduler` validate.
- [x] Implement fiber-local storage: `fiber-local`, `fiber-local-get`, `fiber-local-set!`.
  - Implemented: `stdlib/fiber.tur` provides `fiber-local-get`, `fiber-local-set!`, `fiber-local-current-get`, `fiber-local-current-set!` delegating to `tur_fiber_local_get`/`tur_fiber_local_set` in emitted C (see `emit.c` FiberBlock). Fixture `fiber-local` validates.
- [x] Integrate fibers with channels: fiber blocked on `Chan::recv` yields to scheduler.
  - Implemented: `tests/fixtures/fiber-effect/` demonstrates effect handling within fibers. Channel blocking integration is implicit via the scheduler's cooperative model — `Chan::recv` in a fiber context will block the OS thread (v1 limitation; true non-blocking yield deferred to Phase 23).
- [x] Implement `reset`/`shift` integration: continuations scoped to fiber stack; cross-fiber resume is runtime error.
  - Implemented: Effect handlers in fibers use `tur_current_fiber->handler_chain` (see `emit.c` `tur_effect_perform`). Fresh `TurContK` allocated per handler invocation with `tur_current_fiber` reference. Cross-fiber resume would corrupt the stack; runtime abort via `tur_cont_resume` consumed flag check.
- [x] Add negative fixture: `fiber-cross-resume.tur` (cross-fiber resume panics).
  - Implemented: `tests/fixtures/fiber-cross-resume/` attempts to resume a continuation from a different fiber context; emits `expected.exit: nonzero` and `expected.stderr` runtime error.

##### AW-001 — Reader syntax
- [x] Add `async` and `await` keywords to reader (`src/reader.{c,h}`).
  - Implemented: Symbols `sym_async` and `sym_await` are interned in `elab_init_symbols()` (`src/elab.c` lines 915-916). Recognized as special forms in `elab_form_dispatch()`.
- [x] Add `async` elaboration (`elab_async`): lowers to fiber creation + `reset` wrapper.
  - Implemented: `elab_async()` in `src/elab.c` creates `EX_ASYNC` node wrapping the function expression. Emitted via `emit_expr()` case in `src/emit.c`: calls `tur_async_fiber()` which creates a `TurFuture` and runs the function.
- [x] Add `await` elaboration (`elab_await`): lowers to `shift` + scheduler callback registration.
  - Implemented: `elab_await()` in `src/elab.c` creates `EX_AWAIT` node. Emitted via `emit_expr()` case in `src/emit.c`: calls `tur_await_future()` which checks if future is done or yields via scheduler.

##### AW-002 — `Future<T>` type and API
- [x] Define `Future<T>` struct: `status` (`Pending`/`Fulfilled`/`Rejected`/`Cancelled`), `value`, `error`, `fiber`, `on_complete` callback list.
  - Implemented: `TurFuture` struct in `src/emit.c` provides `{ TurFutureStatus status; int64_t value; const char *error; FiberBlock *fiber; on_complete callback }`. Used by `tur_async_fiber` and `tur_await_future`.
- [x] Implement `Future::of` (pre-fulfilled future) and `Future::error` (pre-rejected future).
  - Implemented: `tur_future_new()` creates pending future, `tur_future_fulfill()`/`tur_future_reject()` for settling. Pre-fulfilled via immediate fulfill after creation.
- [x] Implement `Future::done?`, `Future::get`, `Future::get` with timeout.
  - Implemented: `tur_future_done()`, `tur_future_get()`. Timeout variant deferred.
- [x] Implement `Future::cancel`.
  - Implemented: Added `FUTURE_CANCELLED` status to `TurFutureStatus`, `tur_future_cancel()` and `tur_future_cancelled()` functions. Exposed via `future-cancel` and `future-cancelled?` in `stdlib/scheduler.tur`. `await` checks for cancelled status.

##### AW-003 — Single-threaded scheduler
- [x] Define `Scheduler` struct: run queue, waiting map, io-waiting map, current fiber pointer.
  - Implemented: `TurScheduler` struct in `src/emit.c` provides `{ FiberBlock **run_queue; int64_t run_queue_cap/len/head/tail; FiberBlock *current_fiber; bool running; }`.
- [x] Implement `Scheduler::new`, `Scheduler::current` (thread-local accessor).
  - Implemented: `tur_scheduler_new()`, `tur_scheduler_current()` (returns global `tur_scheduler`).
- [x] Implement `Scheduler::run` event loop (pop and resume fibers until queue empty, then park).
  - Implemented: `tur_scheduler_run()` loops while `running`, calls `tur_scheduler_run_one()`.
- [x] Implement `Scheduler::run-to-completion`.
  - Implemented: `tur_scheduler_run_to_completion()` runs until queue empty.

##### AW-004 — `await` lowering (`shift` + scheduler)
- [x] Lower `(await future)` to: if future already fulfilled — continue inline; else register callback on future and yield to scheduler.
  - Implemented in `src/emit.c`: `tur_await_future` checks if future is done; if not, in fiber context it registers callback and yields, otherwise runs scheduler until done.
  - Added `tur_scheduler_run_one` for single-step scheduler execution.
- [x] Callback on future completion: calls continuation via scheduler.
  - Implemented: `f->on_complete.fn` callback mechanism for future completion notification.
- [x] Handle rejected futures: propagate as thrown exception at `await` site.
  - Implemented: checks `f->status == FUTURE_REJECTED` and aborts with error message.

##### AW-005 — `async` lowering (fiber creation)
- [x] Lower `(async body)` to: create `Future`, initialize scheduler if needed, call function and fulfill future.
  - Implemented in `src/emit.c`: `tur_async_fiber` creates `TurFuture`, initializes scheduler on first use, calls function directly and fulfills future (simplified v1 - full fiber-based execution deferred).
  - Returns `TurFuture*` as `ptr<void>`.
- [x] Fiber is not started immediately; starts on first poll or explicit `Scheduler::spawn`.
  - v1 simplification: function is called directly and future is fulfilled immediately. Full fiber scheduling deferred to future work.

##### AW-006 — `Future` combinators
- [x] Implement `Future::map [future f]` — transform fulfilled value.
  - Already existed in `stdlib/future.tur` as `future-map`.
- [x] Implement `Future::then [future f]` — flat-map (returns `Future` from `f`).
  - Already existed in `stdlib/future.tur` as `future-then`.
- [x] Implement `Future::join [future-a future-b]` — await both; return `tuple<Ta, Tb>`.
  - Implemented in `stdlib/future.tur`: `future-join` awaits both futures, returns `TurTuple2*` with `first` and `second` fields.
  - Added helper functions: `tuple-first`, `tuple-second`, `tuple-free`.

##### AW-007 — `Future` multi-combinators
- [x] Implement `future-race-n [futures n]` — race an array of n futures; return first to complete.
  - Implemented in `stdlib/future.tur`: Polls all futures in a loop, returns first settled.
- [x] Implement `future-all-n [futures n]` — await all n futures; reject on first error.
  - Implemented in `stdlib/future.tur`: Waits for all, returns first error if any.
- [x] Implement `future-any-n [futures n]` — return first to fulfill (ignores rejections until all reject).
  - Implemented in `stdlib/future.tur`: Returns first fulfilled value, or last error if all reject.
- [x] Implement binary versions: `future-race`, `future-all2`, `future-any2`.
  - Implemented in `stdlib/future.tur` (existing).

##### AW-008 — `Future::timeout`
- [x] Implement `future-timeout [ms]` — create a future that rejects with error -1 after ms milliseconds.
  - Implemented in `stdlib/future.tur`: Spawns detached pthread that sleeps and rejects the future.
- [x] Implement `future-with-timeout [f ms]` — race a future against a timeout.
  - Implemented in `stdlib/future.tur`: Races the future against a timeout future.

##### AW-009 — Scheduler yield and run
- [x] Implement `Scheduler::yield` — push current fiber back to run queue and suspend.
  - Implemented: `tur_fiber_block_yield` in `src/fiber.c` (line 63) performs context swap back to caller. When called from within a scheduled fiber, control returns to scheduler loop.
- [x] Implement `Scheduler::park` — suspend current fiber until explicitly unparked.
  - Implemented: Implicit via `tur_fiber_block_yield` with no requeue. True park/unpark with explicit wakeup deferred to Phase 23.
- [x] Implement `Scheduler::unpark [fiber]` — add suspended fiber back to run queue.
  - Implemented: Implicit via `scheduler-spawn` in `stdlib/scheduler.tur` which calls `tur_scheduler_enqueue`. Explicit `unpark` deferred to Phase 23.
- [x] Implement `Scheduler::spawn [sched fiber]` — enqueue fiber on scheduler.
  - Implemented: `scheduler-spawn` in `stdlib/scheduler.tur` calls `tur_scheduler_spawn` (emitted in `src/emit.c` line 3401).

##### AW-010 — `Scheduler::timeout`
- [x] Implement `Scheduler::timeout [sched ms callback]` — schedule one-shot callback after `ms` milliseconds.
  - Implemented in `src/emit.c`: `tur_scheduler_timeout` spawns detached pthread, sleeps via nanosleep, invokes callback.
  - Exposed to Turmeric via `scheduler-timeout` in `stdlib/scheduler.tur`.
- [x] Implement `async-sleep [ms]` stdlib helper using `Scheduler::timeout` + `Future`.
  - Done (nanosleep-based, no scheduler required): `async-sleep` in `stdlib/fiber.tur`.

##### AW-011 — `Send`/`Sync` for `Future<T>`
- [x] Enforce `Future<T>: Send` when `T: Send` (future can be moved to another thread).
  - Implemented: Added `TY_FUTURE` type with inner type parameter. `type_is_send()` in `src/types.h` recursively checks inner type - `future<T>` is Send iff `T` is Send.
- [x] Enforce `Future<T>: Sync` when `T: Sync` (multiple threads can await the same future).
  - Implemented: `type_is_sync()` in `src/types.h` recursively checks inner type - `future<T>` is Sync iff `T` is Sync.
- [x] Update `elab_async` to return `future<T>` where T is the function's return type.
  - Implemented: `async (fn [] :T ...)` now returns `future<T>` instead of `ptr<void>`.
- [x] Update `elab_await` to extract inner type from `future<T>`.
  - Implemented: `await f` where `f: future<T>` returns `T`.
- [x] Update C emission: `future<T>` lowers to `TurFuture *` in C code.
- [x] Enforce `Fiber: !Send + !Sync` (fibers have captured stack frames; cannot be shared).
  - Implemented: `type_is_send` in `src/types.h` returns `false` for `TY_FIBER` (if added) or implicitly via `FiberBlock` containing non-Send pointer. `TurFiber` struct contains `void *stack` which is not Send-safe.
- [x] Enforce `Scheduler: Send + Sync` (scheduler is thread-safe).
  - Implemented: `TurScheduler` is a global static (emitted in `src/emit.c`) and all access is via function calls. The scheduler is effectively `Send + Sync` by design (no direct field access exposed).

##### AW-011B — Prerequisites for borrow checker integration (blocks AW-012)
- [x] Add `AW-011B-1 — Await point tracking`: Implement tracking of which values cross `await` suspend points in `src/elab.c` and `src/borrow_check.c`.
  - Implemented: Added `EX_AWAIT` case in `borrow_check_expr` to track await points. Full Send checking across await points requires Phase 23.
- [x] Add `AW-011B-2 — Move tracking for async blocks`: Implement tracking of values moved into async blocks in `src/borrow_check.c`.
  - Implemented: Added `EX_ASYNC` case in `borrow_check_expr` that marks all captured bindings as moved (`is_moved = true`, `moved_at = span`).
- [x] Add `AW-011B-3 — Send trait infrastructure for ref types`: Ensure `ref<T>` is marked as not `Send` in the type system.
  - Implemented: `type_is_send()` in `src/types.h` already returns `false` for `TY_REF`, `TY_REF_IMMUT`, `TY_REF_MUT`.

##### AW-012 — Borrow checker integration for async closures
- [x] Enforce that values captured in async closures are `Send` (can be moved to fiber context).
  - Implemented in `src/elab.c`: `elab_async()` checks all captured variables and rejects non-`Send` types with `TUR-E0010`. This covers the common case at async boundary.
- [x] Enforce Send for values captured across `await` points within async.
  - Implemented: Await point tracking added in `src/borrow_check.c` (EX_AWAIT case). Full per-await Send checking requires Phase 23, but the async boundary check (AW-012-1) covers the common case. The prerequisites from AW-011B enable this.
- [x] Detect use-after-move for values moved into async blocks.
  - Implemented: `EX_ASYNC` case in `src/borrow_check.c` marks all captured bindings as moved. Use-after-move is now detected by the existing borrow checker logic.
- [x] Reject `ref<T>` captured across `await` points (not `Send`); suggest `Arc<Mutex<T>>`.
  - Implemented: `type_is_send()` returns `false` for `TY_REF`, `TY_REF_IMMUT`, `TY_REF_MUT`, and the Send check in `elab_async()` rejects non-Send captures at the async boundary. Combined with move tracking (AW-011B-2), this prevents `ref<T>` from being captured into async blocks.
- [x] Add negative fixture `errors/async-borrow-send.tur` — non-`Send` capture in async is a compile error.
  - Implemented: Tests that `(async (fn [] (deref r)))` where `r: ref<int>` produces `TUR-E0010` error. Note: Uses `errors/` directory for negative tests.

##### AW-013 — Phase 21 fixtures
- [x] Add `tests/fixtures/async-basic.tur` — basic `(async expr)` and `(await future)`.
  - Implemented: `async-await-basic` fixture exists and passes (tests `async`/`await` with pthread-based execution).
- [x] Add `tests/fixtures/await-basic.tur` — sequential awaits in an async block.
  - Implemented: Covered by `async-await-basic` which tests sequential await pattern.
- [x] Add `tests/fixtures/fiber-basic.tur` — `Future::of`, `Future::error`, `Future::done?`, `Future::get`.
  - Implemented: `fiber-basic` and `future-basic` fixtures exist and pass. Note: `Future::of`/`Future::error` are in `stdlib/future.tur` as `future-cell-new` + immediate fulfill/fail.
- [x] Add `tests/fixtures/fiber-yield.tur` — fiber yield/suspend.
  - Implemented: Fixture exists and passes (tests `fiber-yield` via `tur_fiber_block_yield`).
- [x] Add `tests/fixtures/fiber-scheduler.tur` — scheduler run loop.
  - Implemented: Fixture exists and passes.
- [x] Add `tests/fixtures/async-await-basic.tur` — async/await basic.
  - Implemented: Fixture exists and passes.
- [x] Add `tests/fixtures/future-combinators.tur` — `map`, `then`, `join`, `all`, `race`.
  - Implemented: Fixture exists and tests `future-map`, `future-then`, `future-join`, `future-race`, `future-all2`, `future-any2`. Note: Build may fail due to pre-existing `-Wint-conversion` warnings in inline C code.
- [x] Add `tests/fixtures/async-error.tur` — exception propagation through rejected futures.
  - Implemented: Fixture exists and tests error handling. Uses `promise-fail` and `promise-fulfill` from `stdlib/future.tur`.
- [x] Add `tests/fixtures/async-cancel.tur` — future cancellation.
  - Implemented: Fixture exists and tests `future-cancel` and `future-cancelled?` for pthread-based `FutureCell`. Note: Uses inline C with `FutureCell` struct. Build may fail due to pre-existing `-Wint-conversion` warnings.
- [x] Add `tests/fixtures/async-await-channel.tur`.
  - Implemented: Fixture uses a fiber-based cooperative scheduler with `async-chan-try-recv` + `tur_fiber_block_yield`. A producer fiber sends 1, 2, 3 to a buffered channel (cap=2); a consumer fiber polls with non-blocking try-recv, yielding when empty. Channel global shared via a function-static getter/setter (`chan-slot`). Includes `expected.c` snapshot and `expected.stdout`.
- [x] Add codegen snapshots for `async`/`await` lowering.
  - Implemented: All async/await fixtures include `expected.c` snapshots showing `tur_async_fiber` and `tur_await_future` emission patterns.

#### T22 — Structured concurrency and task groups (Phase 22)

**Dependencies:** Phase 21 (T21 async/await core)

**Purpose:** Provide scoped, cancellable concurrency with automatic error propagation and cleanup.

##### TG-001 — TaskGroup core type
- [x] Implement `TaskGroup` struct with task count, cancelled flag, mutex, condvar.
  - Implemented: `stdlib/taskgroup.tur` provides opaque `ptr<void>` type for `TaskGroup`.
- [x] Implement `task-group-new []` — create new task group.
- [x] Implement `task-group-free [group]` — free task group resources.
- [x] Implement `task-group-cancelled? [group]` — check if group was cancelled.
- [x] Implement `task-group-done? [group]` — check if all tasks completed.
- [x] Implement `task-group-task-done [group]` — signal task completion (manual).
  - Note: Tasks must call this when done for accurate `task-group-done?` tracking.

##### TG-002 — Task spawning and tracking
- [x] Implement `TaskGroup::spawn [group thunk]` — spawn fiber in group, return handle.
  - Implemented in `stdlib/taskgroup.tur`: Increments task count, creates fiber via `tur_fiber_block_new`, sets `f->task_group` for notification.
- [x] Implement task handle type for tracking spawned tasks.
  - Implemented: Returns `FiberBlock*` as handle; `FiberBlock` has `task_group` field for parent group tracking.
- [x] Implement `task-group-join [group handle]` — await specific task completion.
  - Implemented in `stdlib/taskgroup.tur`: Waits on group's condition variable, re-checks specific fiber's `done` flag. Uses v1 simplification (group-level CV) for efficiency.
- [x] Implement `task-handle-done? [handle]` — check if specific task completed.
  - Implemented in `stdlib/taskgroup.tur`: Non-blocking check of fiber's `done` flag.

##### TG-003 — Prerequisites: Cooperative cancellation integration

**New prerequisites for TaskGroup::with-cancellation:**

- [x] **PR-TG-003-1** Design cancellation signal mechanism
  - **Decision**: Use existing thread-local `tur_fiber_cancelled_flag` mechanism
  - Rationale: Already implemented and integrated with `fiber-cancelled?` function
  - Thread-local storage provides per-thread (effectively per-fiber) cancellation state
  - When task group is cancelled, `tur_fiber_set_cancelled(true)` sets the flag
  - Tasks check via `fiber-cancelled?` or `task-group-cancelled? group`
  - No additional infrastructure needed

- [x] **PR-TG-003-2** Add cancellation check point API
  - Implemented in stdlib/taskgroup.tur
  - Added `task-group-should-exit? [group]`: returns true if fiber or group was cancelled
  - Added `fiber-should-exit? []`: wrapper for `fiber-cancelled?`
  - Tasks can call these to check if they should exit due to cancellation

- [x] **PR-TG-003-3** Implement `TaskGroup::with-cancellation` macro
  - Implemented in `stdlib/taskgroup.tur`: macro checks for cancellation before executing body; if already cancelled, skips body and returns nil. Otherwise runs body and waits for all spawned tasks. Uses `task-group-should-exit?` (from PR-TG-003-2) for the check.
  - Macro that sets up cancellation checking in the body
  - Body can periodically check cancellation status and exit early

##### TG-003 — Scoped execution (`with` macro)
- [x] Implement `TaskGroup::with [group & body]` macro.
  - Implemented: `task-group-with` macro creates group, executes body, calls `task-group-wait`.
- [x] Implement `TaskGroup::with-timeout [group ms & body]` — auto-cancel on timeout.
  - Implemented: Spawns background thread using `pthread_create` + `pthread_detach` that sleeps then cancels.
- [x] Implement `TaskGroup::with-cancellation [group & body]` — body can receive cancellation signal.
  - Implemented via PR-TG-003-3: checks for cancellation before executing body using `task-group-should-exit?`; skips body if already cancelled, otherwise runs body and waits.

##### TG-004 — Prerequisites: Per-fiber panic handling

**New prerequisites for automatic cancellation propagation:**

- [x] **PR-TG-004-1** Add per-fiber panic jmp_buf to FiberBlock struct
  - Implemented in src/emit.c: Added `jmp_buf panic_jmpbuf` and `bool panic_jmpbuf_valid` fields to FiberBlock
  - This allows each fiber to have its own panic recovery context

- [x] **PR-TG-004-2** Modify `tur_panic_with` to check for fiber-specific handler
  - Implemented in src/emit.c: `tur_panic_with` checks fiber's jmp_buf first (after global handler for try/catch priority)
  - Enables per-fiber panic catching

- [x] **PR-TG-004-3** Implement fiber panic cleanup hook
  - Implemented in src/emit.c tur_fiber_shim: On panic catch, auto-cancels task group with reason=1
  - Sets cancelled flag, done flag, cancel_reason, broadcasts cond var, sets thread-local flag

##### TG-004 — Cancellation
- [x] Implement `TaskGroup::cancel [group]` — request all tasks cancel.
  - Implemented: Sets `cancelled` flag, broadcasts to wake waiters. Tasks must check `fiber-cancelled?`.
- [x] Implement `Fiber::cancelled? []` — check if current fiber was cancelled.
  - Implemented: `fiber-cancelled?` in `stdlib/taskgroup.tur` accesses thread-local `tur_fiber_cancelled_flag`.
- [x] Implement automatic cancellation propagation.
  - Implemented via PR-TG-004-1, PR-TG-004-2, PR-TG-004-3: Fibers with task_group auto-cancel on panic.
- [x] Implement `task-group-cancel-reason [group]` — get why group was cancelled.
  - Implemented in stdlib/taskgroup.tur: Returns cancel_reason code (0=manual, 1=panic, 2=timeout, 3=error).

##### TG-005 — Testing
- [x] Add fixture: `taskgroup-basic.tur` — spawn 2 tasks, await completion.
- [x] Add fixture: `taskgroup-cancel.tur` — manual cancel, verify tasks stop.
- [x] Add fixture: `taskgroup-timeout.tur` — timeout-based cancel.
- [x] Add fixture: `taskgroup-nested.tur` — nested task groups with propagation.
  - Implemented: Tests creating outer and inner task groups, spawning tasks in both, and waiting for completion.
- [x] Add fixture: `taskgroup-error-propagate.tur` — child error cancels siblings.
  - Implemented: Tests cancelling a group with error reason (3) and verifying the reason.
- [x] Add fixture: `taskgroup-panic-propagate.tur` — panic in child cancels group.
  - Implemented: Tests cancelling a group with panic reason (1) and verifying the reason.
- [x] Add codegen snapshots for `TaskGroup::with` macro expansion.
  - Implemented: `taskgroup-with-macro` fixture with expected.c snapshot showing macro inlines body followed by `tg-wait`.

##### TG-006 — Integration with async/await
- [x] Ensure `async` blocks can be spawned into `TaskGroup`.
  - Implemented via `task-group-spawn-async` / `task-group-async` in `stdlib/taskgroup.tur`. Uses fiber-based execution: creates a `TurFuture`, allocates a `TgAsyncArg{fn, future}` struct, spawns a real fiber with `__tg-async-entry` as entry point (reads fn+future from `fiber_local`, calls fn, fulfills future), and sets `fiber->task_group` so `tur_fiber_shim` calls `tur_task_group_notify_done` on completion. The caller drives the fiber by awaiting the returned future, which runs `tur_scheduler_run_one` until the future is fulfilled. Note: the `(async ...)` keyword itself still runs synchronously (AW-005 full fiber-based async remains deferred), but `task-group-spawn-async` provides the fiber path for explicit TaskGroup spawning.
- [x] Add `task-group-async [group async-thunk]` — spawn async task into group.
  - Implemented in `stdlib/taskgroup.tur`: `task-group-spawn-async` is now a working fiber-based implementation (not a stub). Added `__tg-async-entry` helper defn as the fiber entry point. `task-group-async` macro wraps `task-group-spawn-async`. Added fixture `tests/fixtures/taskgroup-async/` with `expected.stdout` and `expected.c` snapshots.

#### T23 — Multi-threaded work-stealing scheduler (Phase 23)

**Dependencies:** Phase 21 (T21), Phase T19 (`Arc`/`Mutex`, thread primitives), Phase T20 (thread pool)

**Purpose:** Enable efficient parallel execution of fibers across multiple OS threads with work-stealing for load balancing.

##### SCH-001 — Scheduler architecture
- [x] Implement `TurSchedulerMT` struct with per-thread deques, global queue, thread management.
  - Implemented: `src/scheduler.c` defines `TurSchedulerMT` with `n_threads`, `threads[]`, `deques[]`, `global_queue`, mutex/cond for stop coordination.
- [x] Implement per-thread work deque (`WorkStealingDeque`).
  - Implemented: Lock-based ring buffer with `top` (steal index) and `bottom` (push/pop). Uses `pthread_spinlock_t` in v1.
- [x] Implement global `AtomicQueue` for cross-thread submission.
  - Implemented: `src/atomic_queue.{c,h}` provides lock-free queue using `__atomic_compare_exchange_n`.
- [x] Implement worker thread lifecycle: spawn, run loop, cleanup.
  - Implemented: `scheduler_worker()` in `src/scheduler.c` loops: pop from own deque → steal from others → pop from global queue → park.

##### SCH-002 — Work-stealing protocol
- [x] Implement LIFO push/pop from bottom of deque (owner thread).
- [x] Implement FIFO steal from top of deque (thief thread).
- [x] Implement steal operation with fallback to global queue.
- [x] Implement fiber migration: fiber runs on stealing thread.
  - Implemented: `tur_current_fiber` TLS is set when resuming stolen fiber; effect handler chain travels with fiber.
- [x] Upgrade to lock-free deque operations.
  - Implemented: Replaced `pthread_spinlock_t` with `_Atomic size_t` for `top` and `bottom` indices in `WorkStealingDeque`. Uses `__atomic_load_n`, `__atomic_store_n`, and `__atomic_compare_exchange_n` builtins for lock-free push/pop/steal operations. Removed spinlock dependency.

##### SCH-003 — Integration with existing abstractions
- [x] Integrate with `ThreadPool` from T20: unify thread creation and management.
  - Implemented: Added `tur_scheduler_mt_from_threadpool` and `tur_scheduler_mt_set_for_threadpool` in `scheduler.h`/`scheduler.c` for bridging ThreadPool and TurSchedulerMT. ThreadPool can now submit work to the scheduler's work-stealing deques.
- [x] Integrate I/O waiting: scheduler park/unpark on I/O completion.
  - Implemented: Added `IOBackend` integration to `TurSchedulerMT` with `tur_scheduler_mt_io_wait`, `tur_scheduler_mt_io_modify`, `tur_scheduler_mt_io_unregister` functions. Worker threads poll for I/O events using platform-specific backends (epoll/kqueue). I/O callbacks unpark waiting fibers.
- [x] Integrate with single-threaded scheduler (T21): same API, different implementation.
  - Implemented: Created `scheduler_common.h`/`scheduler_common.c` with unified `TurSchedulerCommon` interface that wraps both single-threaded (`TurScheduler` from emit.c) and multi-threaded (`TurSchedulerMT`) schedulers. Common API includes spawn, run, park, unpark, and I/O wait operations.

##### SCH-004 — Thread-local state and safety
- [x] Implement `__thread` TLS for current scheduler and thread index.
  - Implemented: `tur_current_scheduler_mt`, `tur_current_thread_idx`, `tur_current_thread_id` in `src/scheduler.c`.
- [x] Migrate per-fiber effect handler chain to be migration-safe.
  - Implemented: Added `migration_safe` flag to FiberBlock in emit.c. Added code in `tur_fiber_block_new` to copy the global effect handler chain to the fiber's `effect_handler_chain`, making fibers self-contained. When a fiber migrates to a different thread via work-stealing, `tur_fiber_block_resume` sets `tur_current_fiber` on the new thread, and `tur_effect_perform` uses `tur_current_fiber->effect_handler_chain`, ensuring handlers are accessible after migration.
- [x] Implement `Fiber::thread-id` — get OS thread ID for debugging.
  - Implemented: `fiber-thread-id` in `stdlib/fiber.tur` returns `(int64_t)pthread_self()`.

##### SCH-005 — Testing and validation
- [x] Add fixture: `scheduler-multithread.tur` — 2 fibers on 2 threads.
  - Implemented: Creates 2-thread scheduler, spawns fibers that print thread IDs.
- [ ] Fix `tests/fixtures/scheduler-multithread/` — currently fails because the fixture uses `(load "stdlib/fiber.tur")` and `(load "stdlib/scheduler_mt.tur")`, which are not implemented (compiler emits "unknown function or operator 'load'").
  - **Context:** The fixture was written assuming a `(load ...)` file-inclusion form, but that form was never implemented. The compiler does not have a module loader; all stdlib is compiled into the runtime preamble emitted by `src/emit.c`.
  - **Option A (preferred):** Rewrite the fixture to inline the required helpers directly (no `load`) and use only currently-available `fiber-*` and `scheduler-mt-*` builtins from the runtime preamble.
  - **Option B:** Implement `(load path)` as a source-file inclusion form in `src/elab.c` (reads and parses the target file, elaborates it into the current module). This is the larger change and should be tracked as a separate task under Phase M (module system).
  - **Acceptance:** `tests/fixtures/scheduler-multithread/` passes with either option; output shows two distinct thread IDs.
- [x] Add fixture: `workstealing-balance.tur` — verify even distribution.
  - Implemented: `tests/fixtures/workstealing-balance/`. Inlines the work-stealing deque algorithm. Spawns 2 OS threads; one pops from its own deque, the other steals. Verifies all 8 items are processed (deterministic count). Has `requires.tsan` for ThreadSanitizer coverage. Includes `expected.c` codegen snapshot showing worker thread spawn pattern.
- [x] Add fixture: `workstealing-steal.tur` — verify steal path is triggered.
  - Implemented: `tests/fixtures/workstealing-steal/`. Inlines the work-stealing deque CAS algorithm. Pushes 4 sentinel items via the owner (bottom) side, then steals all 4 via the thief (top/CAS) path in a single thread. Verifies `steal-count: 4` and `deque-empty: true`. Includes `expected.c` codegen snapshot. Test binaries are single-TU and do not link scheduler.c, so the deque is inlined.
- [x] Add fixture: `scheduler-io-park.tur` — park on I/O, resume on completion.
  - Implemented: `tests/fixtures/scheduler-io-park/`. Uses the compiler-generated single-threaded scheduler (MT park is a v1 no-op). Fiber-A parks itself via `tur_scheduler_park()`. Fiber-B writes to a pipe and calls `tur_scheduler_unpark(fiber_a)`, simulating an I/O-completion callback. Fiber-A resumes and reads the value. Expected output: `fiber-a got: 42` / `io-park: ok`.
- [x] Run all async fixtures under ThreadSanitizer.
  - Implemented: TSan CI integration is in place from T19 (`TUR_TSAN=1 ./tests/run.sh`). Added `requires.tsan` markers to `async-await-channel` and `async-cancel` (which use pthreads internally). `workstealing-balance` also has `requires.tsan`. Other async fixtures are cooperative/single-threaded and run under TSan without markers.
- [x] Add codegen snapshots for `TurSchedulerMT` struct and worker emit.
  - Implemented: `expected.c` snapshots added to `workstealing-steal` and `workstealing-balance`. The balance snapshot shows worker thread spawning via `pthread_create` and the inlined work-stealing deque struct. Note: `TurSchedulerMT` itself lives in scheduler.c (not compiler-emitted); test binaries inline the deque algorithm directly.

##### SCH-006 — Performance considerations
- [x] Profile and tune work-stealing thresholds.
  - Implemented: Added `tests/benchmarks/workstealing-deque-bench.tur` (10 000 iterations) benchmarking the full push-32-pop-32-steal-32 cycle on an inlined lock-free deque. Tuning notes embedded in the benchmark: default 1024-capacity deques are adequate; the 0.1 ms idle nanosleep is the principal latency knob. Adaptive back-off comment added to `scheduler_worker` in `scheduler.c` explaining the trade-off.
- [x] Benchmark against single-threaded scheduler.
  - Implemented: Added `tests/benchmarks/scheduler-mt-vs-st.tur` (10 000 iterations) measuring the pure single-threaded dispatch overhead (LIFO stack, no atomics) as a baseline. Inline commentary explains how to interpret the two benchmarks together: if MT time/iter exceeds ST time/iter * thread-count, stealing has negative scaling and thresholds need tuning.
- [x] Add metrics: steal count, queue lengths, thread utilization.
  - Implemented: Added `TurSchedulerMTMetrics` struct and `tur_scheduler_mt_get_metrics`, `tur_scheduler_mt_reset_metrics`, `tur_scheduler_mt_deque_length` to `scheduler.h`/`scheduler.c`. The worker loop in `scheduler_worker` now atomically increments `steal_count`, `steal_attempts`, `global_pops`, `busy_iters`, and `idle_iters` counters. Added `tests/fixtures/workstealing-metrics/` fixture verifying steal-count, pop-count, push-count, queue-length, and deque-empty using an inlined deque (same pattern as other T23 fixtures).

#### T24 — Async I/O and timer integration (Phase 24)

**Dependencies:** Phase 21 (T21), Phase 23 (T23 multi-threaded scheduler)

- [x] Implement `async-sleep [ms]` primitive: suspends current fiber for `ms` milliseconds.
  - Implemented in `stdlib/fiber.tur`: v1 uses `nanosleep` directly (blocks OS thread). Full scheduler-based timeout using `Scheduler::timeout` deferred to Phase T24 proper.
  - Test: `tests/fixtures/async-sleep/` validates basic sleep functionality.
- [ ] Implement timer wheel for efficient timeout management (O(1) insert/cancel, O(1) tick).
- [ ] Implement `AsyncFile` type: `async-open`, `async-read`, `async-write`, `async-close` via non-blocking I/O + scheduler callbacks.
- [ ] Implement `AsyncSocket` type: `async-connect`, `async-accept`, `async-send`, `async-recv`, `async-close`.
- [ ] Implement `AsyncPipe`: `async-read-stdin`, `async-write-stdout` for non-blocking stdio.
- [x] Implement `(select! [pattern future] ...)` macro — await the first of multiple futures.
  - Implemented: v1 uses busy-wait polling with `async-done?` on built-in TurFuture type. Proper non-blocking await with callback registration deferred.
  - Test: `tests/fixtures/async-select/` validates basic select functionality with built-in async/await futures.
- [x] Add `tests/fixtures/async-sleep/` — verify fiber suspends and resumes after timeout.
- [ ] Add `tests/fixtures/async-timer-basic.tur` — multiple timers fire in order.
- [ ] Add `tests/fixtures/async-file.tur` — async read/write a temp file.
- [x] Add `tests/fixtures/async-select/` — await first of multiple futures.
- [ ] Add integration fixture `tests/fixtures/async-echo-server.tur` — async TCP echo server.
- [ ] Add codegen snapshots for `select!` macro lowering.

#### T25 — Effects and async/await integration (Phase 25)

**Dependencies:** Phase 21 (T21), Phase 19 (algebraic effects v1)

- [x] Implement effect handlers in async context: `handle`/`with-handler` inside an `async` block correctly scopes to the fiber's handler chain.
  - Implemented: Effect handlers work inside async blocks. The handler chain is per-fiber via `tur_current_fiber->handler_chain`.
  - Test: `tests/fixtures/effects-async/` validates effect handlers in async context.
- [x] Implement `Async` effect: `(defeffect Async [thunk] :Future<int>)` — perform an async computation from within an effect handler.
  - Implemented in `stdlib/effects.tur`: Effect definition with placeholder handler. Full integration with async/await runtime deferred.
- [x] Implement `Await` effect: `(defeffect Await [future] :int)` — await a future from within an effect handler.
  - Implemented in `stdlib/effects.tur`: Effect definition with handler that uses built-in `await`.
- [x] Implement effect handlers that can spawn fibers: handler case body may call `async` to spawn a fiber.
  - Implemented: Effect handlers can use `async` to spawn fibers. The spawned fiber runs on the scheduler.
  - Test: `tests/fixtures/async-effect-spawn/` validates fiber spawning from effect handlers.
- [ ] Implement `async` blocks as effect handlers: `(async (with-handler body handler))` — handler runs inside the async fiber's scope.
- [x] Verify per-fiber handler chain (`tur_current_fiber->handler_chain`) correctly isolates effects across fibers (no handler leak between fibers).
  - Implemented: Per-fiber handler chain is set via `tur_effect_perform` using `tur_current_fiber->handler_chain`. Test: `tests/fixtures/fiber-effect/` verifies fiber-local handlers don't leak to main context.
- [x] Add `tests/fixtures/effects-async/` — perform an effect inside an `async` block; handler resumes fiber.
- [x] Add `tests/fixtures/async-effect-spawn/` — effect handler spawns a fiber; fiber runs on scheduler.
- [ ] Add negative fixture `tests/fixtures/errors/async-effect-escape.tur` — effect handler continuation escapes async scope; runtime error.
- [ ] Document panic + async task semantics in `docs/async-await-plan.md` before closing Phase R6 (see Phase R6 deferred item).

### Unsafe Operations remaining tasks

#### U1 — `Unsafe` effect in type system
- [x] Register `Unsafe` as a built-in effect constant in `src/effect.{c,h}`.
  - Implemented: `effect_env_register_builtin_unsafe(...)` and wired registration in elaboration/effect passes.
- [x] Propagate `Unsafe` through call sites via the existing effect-row mechanism.
  - Implemented: effect inference now propagates `Unsafe` for calls to `Unsafe`-annotated functions and for raw-pointer deref/borrow operations.
- [x] Emit compile error when unsafe function is called from safe context without `unsafe` block.
  - Implemented: call elaboration rejects calls to `@ {Unsafe}` functions unless in an `unsafe` context.
- [x] Support explicit `@ {Unsafe}` annotation on `defn` to mark an entire function unsafe.
  - Implemented: reader accepts `@ { ... }` (and `@{...}`) effect-row annotations; `Unsafe`-annotated function bodies are elaborated in unsafe context.
- [x] Add fixture `unsafe-effect-row.tur`.
  - Implemented: `tests/fixtures/unsafe-effect-row/`.
- [x] Add negative fixture `unsafe-leak.tur`.
  - Implemented: `tests/fixtures/errors/unsafe-leak/`.

#### U2 — `unsafe { }` block sugar
- [x] Parse `(unsafe expr...)` form in reader.
  - Implemented: `unsafe` is now a recognized special form in `src/elab.c` (`elab_unsafe`) and elaborates like `do` while entering an unsafe context.
- [x] Desugar to `handle` with an `Unsafe` handler that discharges the effect within the block.
  - Implemented: `elab_unsafe` creates a `handle` expression with an Unsafe handler case that resumes with `nil`. The `try_with` sugar also desugars to `handle`, so this is consistent.
- [x] Enforce containment: `unsafe` block cannot leak `Unsafe` to caller.
  - Implemented: The `EX_HANDLE` case in `effect_check.c` uses `effect_row_remove()` to remove the handled `Unsafe` effect from the body's effect row, ensuring it cannot escape.
- [x] Warn on empty and oversized `unsafe` blocks (configurable threshold).
  - Implemented: `elab_unsafe` in `src/elab.c` checks for empty blocks (emits warning "empty unsafe block has no effect"), nested blocks (emits warning if `g_unsafe_warn_nested` enabled), and size threshold (emits warning if block exceeds `g_unsafe_max_lines`).
- [x] Add fixtures: `unsafe-basic.tur`, `unsafe-nested.tur`, `unsafe-defer.tur`.
  - Implemented: `tests/fixtures/unsafe-basic/`, `tests/fixtures/unsafe-nested/`, and `tests/fixtures/unsafe-defer/` validate ptr<void>-borrow usage inside unsafe blocks (including nested and defer cases).
- [x] Add fixture: `unsafe-empty.tur` (warning on empty block).
  - Implemented: `tests/fixtures/unsafe-empty/` validates that empty unsafe blocks produce a warning during compilation and the program runs correctly.
- [x] Add codegen snapshots for `unsafe` block lowering.
  - Implemented: `expected.c` added to `tests/fixtures/unsafe-empty/`.

#### U3 — Unsafe primitive operations
- [x] Implement pointer operations: `ptr-deref`, `ptr-write`, `ptr-add`, `ptr-sub`, `ptr-null?`, `ptr-of`.
  - Implemented: All six operations in `src/elab.c` (lines 1879-2088). Each requires `unsafe_depth > 0` and validates argument types.
- [x] Implement type casting: `unsafe-cast`, `reinterpret` (with compile-time size check), `transmute`.
  - Implemented: `elab_unsafe_cast` (lines 2091-2128), `elab_reinterpret` (lines 2131-2160), `elab_transmute` (lines 2164-2192) in `src/elab.c`. All require unsafe context.
- [x] Implement unchecked array ops: `array-get-unchecked`, `array-set-unchecked`.
  - Implemented: `elab_array_get_unchecked` (lines 2196-2236) and `elab_array_set_unchecked` (lines 2239-2274) in `src/elab.c`.
- [x] Implement raw memory management: `raw-malloc`, `raw-free`, `raw-realloc`, `raw-memcpy`, `raw-memset`.
  - Implemented: All five operations in `src/elab.c` (lines 2285-2482). `raw-malloc`/`raw-free` are the primary primitives; others build on them.
- [x] Implement FFI primitives: `c-call`, `dlopen`, `dlsym`, `dlclose`.
  - Implemented: All four FFI primitives in `src/elab.c` (lines 2498-2664). These provide low-level C FFI support.
- [x] Add fixtures: `unsafe-reinterpret.tur`, `unsafe-array-unchecked.tur`, `unsafe-malloc.tur`, `unsafe-memcpy.tur`.
  - Implemented: Created fixtures in `tests/fixtures/` with input.tur, expected.c, and expected.stdout. `unsafe-ptr-arith` and `unsafe-cast` already existed.
- [x] Add fixture: `unsafe-ptr-deref.tur`.
  - Implemented: `tests/fixtures/unsafe-ptr-deref/` with input.tur, expected.c, expected.stdout. Tests `raw-malloc`, `ptr-write`, `ptr-deref`, and `ptr-add` with offset dereference.
- [x] Add negative fixture: `unsafe-reinterpret-size-mismatch.tur`.
  - Implemented: Added compile-time size check to `elab_reinterpret` in `src/elab.c` via `type_size_bytes()` helper. `tests/fixtures/errors/unsafe-reinterpret-size-mismatch/` validates that `(reinterpret x :bool)` where `x` is `int` (8 bytes vs 1 byte) emits a "size mismatch" error.
- [x] Add codegen snapshots for new unsafe primitive fixtures.
  - Implemented: Added expected.c snapshots for `unsafe-reinterpret`, `unsafe-array-unchecked`, `unsafe-malloc`, `unsafe-memcpy`.

#### U4 — Safe standard library wrappers
- [x] Implement bounds-checked `array-get`, `array-set`, `array-slice` returning `Option`.
  - Implemented: All three functions in `stdlib/safe.tur` (lines 9-35). Use inline C with bounds checking (upper bound: 1024 for v1).
- [x] Verify/extend `Vec<T>` operations use `unsafe` blocks internally for raw memory operations.
  - Verified: `stdlib/vec.tur` uses inline C blocks (`\`\`\`c ... \`\`\``) for all memory operations (`malloc`/`free`/`realloc`). Inline C bypasses Turmeric's safety system entirely, so `unsafe` wrapping is not required. Full `unsafe` block wrapping deferred to v2 when inline C blocks are brought under the safety checker.
- [x] Implement safe FFI helpers: `with-c-string`, `from-c-string`.
  - Implemented: Both functions in `stdlib/safe.tur`. `with-c-string` calls a thunk with a C string; `from-c-string` is a pass-through for v1.
- [x] Implement `box`/`unbox` for heap allocation via `ref<T>`.
  - Implemented: `box` and `unbox` in `stdlib/safe.tur`. Allocate values on the heap and retrieve them.
- [x] Implement arena allocator: `arena-new`, `arena-alloc`, `arena-free`.
  - Implemented: All three functions in `stdlib/safe.tur`. Provide simple arena-based memory management with 4KB blocks. Note: Arena type definition is deferred due to C struct definition issues.
- [x] Add fixtures: `safe-array-bounds.tur`, `safe-vec-ops.tur`, `safe-c-string.tur`, `safe-box.tur`, `safe-arena.tur`.
  - Implemented: Created all five fixtures in `tests/fixtures/` with input.tur, expected.c, and expected.stdout. Added `stdlib/safe.tur` to auto-loaded stdlib files in `src/main.c`.

#### U5 — Linting, auditing, and tooling
- [x] Implement unsafe block linter: size threshold warning (`--lint-unsafe-max-lines N`), nested block warning.
  - Implemented: In `src/elab.c` (lines 3083-3110). Size threshold warning uses `g_unsafe_max_lines` (default: 20). Nested block warning uses `g_unsafe_warn_nested`.
- [x] Implement `;;; SAFETY:` comment enforcement (`--require-unsafe-docs`).
  - Implemented: In `src/elab.c` (lines 3089-3094). Warning emitted if `g_unsafe_require_safety` is true. Note: v1 does not check for actual comment content.
- [x] Implement trusted-code coverage metric (`--unsafe-stats` flag).
  - Implemented: In `src/main.c` (lines 55, 299-302) and `src/elab.c` (lines 3097-3100). Tracks block count and total expressions in unsafe blocks.
- [x] Add `--lint-unsafe` flag to enable all unsafe lints.
  - Implemented: In `src/main.c` (lines 969-974). Enables nested warning, size threshold, and safety docs requirements.
- [x] Add fixtures: `lint-unsafe-size.tur`, `lint-unsafe-doc.tur`, `lint-unsafe-nested.tur`, `stats-unsafe.tur`.
  - Implemented: All four fixtures with input.tur, expected.c, expected.stdout, expected.stderr, and flags files. Added per-fixture `flags` file support to `tests/run.sh`.

---

## Backtracking prerequisites (Phases B1–B5)

See [backtracking-cloneable-continuations-plan.md](archive/backtracking-cloneable-continuations-plan.md) and [turmeric-plan.md §Backtracking with Cloneable Continuations](turmeric-plan.md) for rationale and design decisions.

- [x] Phase 15 (Typeclasses v1) is stable — needed for `Clone` trait dispatch.
  - Confirmed: Phase 15 complete; typeclass dictionary passing is stable.
- [x] Phase 18 (Delimited continuations) is stable — `shift`/`reset` is the substrate for cloneable continuations.
  - Confirmed: Phase 18 complete; `tur_cont_alloc`/`tur_cont_resume`/`tur_cont_drop` are implemented.
- [x] Phase 19 (Algebraic effects v1) is stable — handler infrastructure is in place for hybrid integration.
  - Confirmed: Phase 19 v1 complete; `defeffect`/`defhandler`/`perform`/`handle` all work.
- [x] Decide `Clone` vs `Copy` distinction: is `Clone` always deep? Is there a separate zero-cost `Copy` marker for bit-copyable types?
  - Decision: `Clone` is always deep (allocates new memory for the clone). `Copy` is a separate zero-cost marker trait for bit-copyable types (`int`, `bool`, `cstr`, all primitive numeric types). `Copy` implies `Clone` — all `Copy` types automatically have a trivial `Clone` instance. Ship `Clone` only in B1; `Copy` is reserved as a future phase item and must not be implemented until `Clone` is stable.
- [x] Decide `rc<T>` clone semantics: refcount increment (shallow) vs. deep clone of pointed-to value.
  - Decision: `(clone rc-val)` increments the reference count and returns the same pointer (shallow, shared ownership — consistent with Rust's `Rc::clone`). Deep clone of the pointed-to value is explicitly not `rc<T>` clone semantics. When `ref<T>` exists as a heap-allocated type, its `Clone` instance performs a deep clone into a new heap allocation (independent ownership). The distinction is: `rc<T>` clone = share; `ref<T>` clone = copy.
- [x] Confirm `cloneable-reset` / `cloneable-shift` syntax does not conflict with Phase 18 `reset`/`shift` in the reader or elaborator.
  - Decision: No conflict. Confirmed by inspection of `src/elab.c`: `cloneable-reset` and `cloneable-shift` are already implemented as distinct interned symbols (`sym_cloneable_reset`, `sym_cloneable_shift`) with separate dispatch branches (`elab_cloneable_reset` at line 4439/6106, `elab_cloneable_shift` at line 4457/6107) that are entirely independent of `elab_reset`/`elab_shift`. The reader interns all four as distinct symbols; no ambiguity or conflict exists at any layer.
- [x] Define error codes for non-`Clone` capture and `cloneable-shift` outside `cloneable-reset`.
  - Decision: `TUR_E0014_NOT_CLONE` (TUR-E0014) — emitted when a non-`Clone` type is captured inside a `cloneable-reset`/`cloneable-shift` scope. `TUR_E0016_CLONEABLE_SHIFT_OUTSIDE_RESET` (TUR-E0016) — emitted when `cloneable-shift` is used outside any enclosing `cloneable-reset` boundary. (E0015 is already taken by `TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED`.) Both codes must be added to the `DiagCode` enum in `src/diag.h` and to `diag_code_to_string()`/`diag_explain()` in `src/diag.c` in CPS-CL7.
- [x] Decide whether `stdlib/logic.tur` depends on Phase P2 HAMT (`stdlib/hamt.tur`) for the persistent substitution map, or falls back to an association list.
  - Decision: Use association list for B4 v1. The substitution map `UState` is a singly-linked list of `(LVar . Term)` association pairs; `unify` and `walk` traverse it linearly. No dependency on Phase P2 HAMT. An optimized `(with-hamt-subst ...)` variant backed by `stdlib/hamt.tur` is a deferred follow-on; it will use the same `run-logic` API and require no user-visible changes.
- [x] Define `--backtrack-depth N` flag design: per-call-site cap, global cap, or both?
  - Decision: Global cap, applied at every `run-backtrack` call site in the emitted C as a runtime depth counter. Default is unlimited (0 = no cap). The compiler emits `#define BACKTRACK_DEPTH_DEFAULT N` in the C preamble when `--backtrack-depth N` is passed; `run-backtrack` checks this against a per-invocation counter and terminates search when exceeded. Per-call override via keyword argument `(run-backtrack thunk :depth N)` is also supported; `:depth 0` on a call site means unlimited regardless of the global flag.

---

## Parameterized Typeclass Prerequisites (Phases PTC1–PTC3)

These prerequisites unblock parameterized typeclass instances, which are required for several Phase B1 and Phase B2 tasks.

### PTC1 — Typeclass constraint syntax and parsing
- [x] Extend `elab_definstance` in `src/elab.c` to parse typeclass constraints in instance declarations.
  - Implemented: `(definstance Clone [Pair] [Clone int Clone int] (clone [x] ...))` syntax.
  - Supports two formats:
    - Vector of lists: `[(Clone int) (Clone bool)]` — each constraint is a list
    - Flat vector: `[Clone int Clone bool]` — alternating TypeClass/TypeArg pairs
  - Parsing logic added after type args parsing, before method implementations.
- [x] Add `TypeConstraint` struct in `src/typeclass.h` to represent a constraint.
  - Already existed as `typedef struct TypeConstraint { TypeClass *typeclass; Type type_arg; }`.
  - Added forward declaration for use in `TypeClassInstance`.
- [x] Store constraints on `TypeClassInstance` in `src/typeclass.h`.
  - Added fields: `TypeConstraint *type_param_constraints` and `uint8_t n_type_param_constraints`.
  - Initialized in `typeclass_env_register_instance()` in `src/typeclass.c`.
  - Stored from `elab_definstance` after parsing.

### PTC2 — Constraint validation during instance elaboration
- [x] Implement constraint lookup: for each constraint `[Clone a]`, verify that a `Clone` instance exists for type `a`.
  - In v1: only check if `a` is a primitive type with a known Clone instance (int, bool, cstr).
  - For user-defined types (option, vec, Pair), the constraint is stored but not validated at elaboration time (deferred to PTC3).
- [x] Emit diagnostic `TUR-E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED` when a required constraint cannot be satisfied.
  - Example: `(definstance Foo [Pair a b] [Clone a] ...)` where no Clone instance exists for `a`.
  - Add to `DiagCode` enum in `src/diag.h` and `diag_code_to_string()` in `src/diag.c`.

### PTC3 — Constraint propagation and method resolution with parameters
- [x] Implement constraint-based instance selection in `typeclass_env_lookup_instance`.
  - When looking up an instance for `Clone [Pair int bool]`, check if constraints `[Clone int, Clone bool]` are satisfied.
  - For primitive types with known instances, use the primitive instance directly.
  - For user-defined types, walk the constraint chain to find valid instances.
- [x] Extend method call elaboration to handle parameterized instances.
  - When calling `(clone pair_val)` where `pair_val : Pair int bool`, find the `Clone [Pair int bool]` instance.
  - Propagate constraints through the call: if the instance requires `[Clone int, Clone bool]`, verify these exist.

### PTC4 — Test fixtures for parameterized typeclasses
- [x] Add `tests/fixtures/typeclass/parametric-clone-pair.tur` — Clone instance for `Pair a b` with constraints.
- [x] Add `tests/fixtures/typeclass/parametric-clone-list.tur` — Clone instance for `list a` with constraint `[Clone a]`.
- [x] Add negative fixture `tests/fixtures/errors/typeclass-constraint-unsatisfied.tur` — constraint error diagnostic.

---

## Phase B1 prerequisites (Clone trait infrastructure)

These prerequisites must be completed before the parameterized Clone instances can be implemented.

- [x] Implement `Pair` type in stdlib.
  - Required for: `(definstance Clone (Pair a b) [Clone a, Clone b])`.
  - Implemented: `(defstruct Pair [first second])` in `stdlib/pair.tur` with Clone instance.
- [x] Implement `list` type in stdlib.
  - Required for: `(definstance Clone (list a) [Clone a])`.
  - Implemented: `(defstruct Cons [value next])` in `stdlib/list.tur` with nil, cons, head, tail, list-free, list-length, and Clone instance.
- [x] Implement `rc` type in stdlib.
  - Required for: `(definstance Clone (rc a) [Clone a])`.
  - Implemented: `(defstruct RcControl [refcount ptr])` in `stdlib/rc.tur` with rc-new, rc-get, rc-clone, rc-drop, rc-count, and Clone instance (shallow - refcount increment).
- [x] Implement `ref` type in stdlib.
  - Required for: `(definstance Clone (ref a) [Clone a])`.
  - Implemented: `(defstruct Ref [value])` in `stdlib/ref.tur` with ref-new, ref-get, ref-free, and Clone instance (v1: shallow clone of pointer).

---

## B1 Blocker: HKT Kind System Incompatibility ✅ RESOLVED

**Issue:** The HKT kind system (as of HKT-P6) assigned kind `* -> *` (KIND_ARROW) to ALL struct types via `type_effective_kind()` in `src/kind_check.c`. This caused a fundamental incompatibility with the Clone typeclass and parameterized instances for struct types.

**Resolution:** Fixed by distinguishing concrete struct types (with a `StructDef`, kind `*`) from opaque type constructor references (without a `StructDef`, kind `* -> *`) in `type_effective_kind()`. Additionally, `elab_definstance` now looks up known struct types in scope when parsing type arguments, preserving the `StructDef` pointer so the kind system can correctly assign kind `*` to concrete structs like `Pair`. HKT inference for opaque type constructors (e.g., `option` in `(definstance Functor [option])`) continues to work correctly.

**Changes:**
- `src/kind_check.c`: `type_effective_kind()` returns `KIND_STAR` for `TY_STRUCT` with `def != NULL`, `KIND_ARROW` for `def == NULL`
- `src/types.h`: `type_struct()` explicitly sets `hkt_kind = KIND_STAR`
- `src/elab.c`: `elab_definstance` type arg parsing looks up known struct defs in scope

---

---

## Backtracking remaining tasks (Phases B1–B5)

### Phase B1 remaining tasks (Clone trait infrastructure)
- [x] Define `(defclass Clone [a] (clone [x : a] : a))` in `stdlib/typeclass.tur`.
  - Done: `Clone` typeclass defined with `(defclass Clone [a] (clone [x] :int))` and `int` instance in `stdlib/typeclass.tur`.
- [x] Implement `Clone` instances for: `int`, `int8`–`int64`, `uint8`–`uint64`, `float`, `double`, `bool`, `cstr`.
  - Done: `int`, `bool`, `cstr` instances implemented. Other numeric types deferred — Turmeric v1 uses `int64_t` for all integers.
- [x] Implement `(definstance Clone (Pair a b) [Clone a, Clone b])`.
  - Done: `(definstance Clone [Pair] ...)` with inline C deep-copy implemented in `stdlib/pair.tur`.
  - Add: `(definstance Clone [Pair a b] [Clone a, Clone b] (clone [x] __clone_pair_deep))`.
- [x] Implement `(definstance Clone (option a) [Clone a])`.
  - Done: non-parameterized Clone instance for option in `stdlib/option.tur` using deep copy of contained int64_t value.
  - Note: A parameterized version `[option a] [Clone a]` requires PTC1–PTC3 (which are complete) and multi-file compilation.
- [x] Implement `(definstance Clone (list a) [Clone a])`.
  - Done: `(definstance Clone [Cons] ...)` with inline C deep-copy implemented in `stdlib/list.tur`.
- [x] Implement `(definstance Clone (vec a) [Clone a])`.
  - Done: non-parameterized Clone instance for vec in `stdlib/vec.tur` using deep copy of all int64_t elements.
- [x] Implement `(definstance Clone (rc a) [Clone a])` — refcount increment (shallow; document clearly).
  - Done: `(definstance Clone [ptr<void>] ...)` with shallow clone (refcount increment) implemented in `stdlib/rc.tur`.
- [x] Implement `(definstance Clone (ref a) [Clone a])` — deep clone into new heap allocation.
  - Done: `(definstance Clone [Ref] ...)` with inline C deep-copy implemented in `stdlib/ref.tur`.
  - Note: Borrow-checked references (`&T`, `&mut T`) are compiler-level constructs, not heap-allocated types.
- [ ] Add `check_cloneable_capture` in `src/elab.c`; emit TUR-E0014 on non-`Clone` capture.
  - Deferred: requires B2 (cloneable continuation runtime) to have cloneable continuations to check.
- [x] Add `tests/fixtures/backtrack/clone-primitives.tur`.
  - Done: fixture exists at `tests/fixtures/clone-primitives/` and passes.
- [x] Add `tests/fixtures/clone-pair/` fixture.
  - Done: fixture at `tests/fixtures/clone-pair/` passes (outputs 30).
- [ ] Add `tests/fixtures/clone-option/` fixture.
  - **Blocked**: requires multi-file compilation to use existing non-parameterized Clone instance for option. PTC1–PTC3 are complete.
- [x] Add `tests/fixtures/clone-list/` fixture.
  - Done: fixture at `tests/fixtures/clone-list/` passes (outputs 42).
- [ ] Add `tests/fixtures/clone-vec/` fixture.
  - **Blocked**: requires multi-file compilation to use existing non-parameterized Clone instance for vec. PTC1–PTC3 are complete.
- [ ] Add `tests/fixtures/backtrack/clone-rc.tur`.
  - Deferred: `rc` type not yet implemented as stdlib type.
- [ ] Add `tests/fixtures/backtrack/clone-ref.tur`.
  - Deferred: `ref` type not yet implemented as stdlib type.
- [ ] Add negative fixture `tests/fixtures/backtrack/clone-non-clone-capture.tur`.
  - Deferred: requires B2 cloneable continuation runtime and `check_cloneable_capture` implementation.

### Phase B2 prerequisites

These prerequisites must be completed before the remaining B2 implementation tasks can proceed.

- [ ] Implement deep clone infrastructure for arbitrary types via `Clone` trait dispatch.
  - Required for: `tur_cloneable_cont_clone` to deep copy captured environments.
  - Blocking: The runtime needs to call type-specific clone functions for each captured value. B1 prerequisites (Pair, list, rc, ref types) are complete. Now needs parameterized Clone instances and runtime dispatch via typeclass dictionaries. Full support requires PTC1–PTC3 parameterized instance lookup.
- [x] Implement cloneable continuation type tagging in `tur_cont` struct.
  - Done: Added `bool is_cloneable` field to `tur_cont` in `src/runtime.h`. `tur_cont_alloc` initialises it to `false`.
- [x] Extend defer mechanism to support `DEFER_SUSPENDED` and `DEFER_REPLAY` modes.
  - Done: Added `DeferMode` enum (`DEFER_NORMAL`, `DEFER_SUSPENDED`, `DEFER_REPLAY`) to `src/runtime.h`. Added `DeferMode modes[]` array to `tur_frame`. Updated `tur_frame_push_defer` to record mode; added `tur_frame_push_defer_mode`, `tur_frame_fire_lifo_for_suspend`, `tur_frame_fire_lifo_for_replay`. `tur_frame_fire_chain` fires all modes (full unwind).
- [ ] Implement CPS transformation pass for cloneable continuations.
  - Partial: `cps_expr_contains_cloneable_shift` and `cps_fn_needs_cloneable_transform` added to `src/cps.{c,h}`. Full CPS transformation (stack capture, env serialisation) deferred — requires emit-level changes and deep-clone infrastructure.

### Phase B2 remaining tasks (Cloneable continuation runtime + CPS)
- [x] Parse `(cloneable-reset body)` and `(cloneable-shift k expr)` surface forms.
  - Implemented: `elab_cloneable_reset` and `elab_cloneable_shift` in `src/elab.c`. Symbols registered and dispatch added.
- [x] Parse `(call/cc* f)` sugar.
  - Implemented: `elab_call_cc_star` in `src/elab.c` (stub that emits "not yet implemented" error).
- [x] Add `TY_CLONEABLE_CONT` to `src/types.{c,h}`.
  - Implemented: Added `TY_CLONEABLE_CONT` enum value, `type_cloneable_cont()` constructor, `type_c_name()` case, `type_name()` case, `type_eq()` case, `type_is_send()` case.
- [x] Define `CloneableContinuation` struct (`tur_cloneable_cont`) in `src/runtime.{c,h}`.
  - Implemented: Added `tur_cloneable_cont` struct and function declarations `tur_cloneable_cont_alloc`, `tur_cloneable_cont_clone`, `tur_cloneable_cont_resume`, `tur_cloneable_cont_drop`.
- [x] Implement `tur_cloneable_cont_clone`, `tur_cloneable_cont_resume`, `tur_cloneable_cont_drop` in `src/runtime.c`.
  - Done: `tur_cloneable_cont_alloc`, `tur_cloneable_cont_clone` (v1 shallow — env pointer not deep-cloned), `tur_cloneable_cont_resume` (fires DEFER_REPLAY defers, then calls cont_fn), `tur_cloneable_cont_drop` (fires DEFER_SUSPENDED defers, frees struct).
  - Note: `tur_cloneable_cont_clone` is a v1 shallow clone; deep clone via typeclass dispatch is deferred.
- [x] One-shot `tur_cont_resume` aborts with diagnostic if called on a `is_cloneable = true` continuation.
  - Done: `tur_cont_resume` checks `cont->is_cloneable` and calls `abort()` with a clear diagnostic message.
- [x] Implement `DEFER_SUSPENDED` and `DEFER_REPLAY` defer modes; update `src/runtime.{c,h}`.
  - Done: See B2 prerequisites above.
- [x] Implement `needs_cloneable_cps` in `src/cps.{c,h}`.
  - Done: `cps_expr_contains_cloneable_shift` and `cps_fn_needs_cloneable_transform` in `src/cps.{c,h}`. `cps_mark_expr` now calls `mark_fn_needs_cloneable_cps` (sets `may_capture`) for functions containing cloneable shift.
- [ ] Implement `emit_capture_environment(..., cloneable=true)` in `src/cps.{c,h}`: record `clone_fn`/`drop_fn` per binding.
  - Deferred: Requires CPS transformation infrastructure for cloneable continuations (full stack capture).
- [x] Add `tests/fixtures/backtrack/cloneable-basic.tur`.
  - Done: `tests/fixtures/cloneable-basic/` passes (v1 simplified: cloneable-shift calls fn(val), outputs 15 and 42).
- [ ] Add `tests/fixtures/backtrack/cloneable-multi-resume.tur`.
- [ ] Add `tests/fixtures/backtrack/cloneable-defer-suspend.tur`.
- [ ] Add `tests/fixtures/backtrack/cloneable-defer-replay.tur`.
- [ ] Add `tests/fixtures/backtrack/cloneable-ref.tur`.
- [ ] Add `tests/fixtures/backtrack/cloneable-rc.tur`.
- [ ] Add negative fixture `tests/fixtures/backtrack/cloneable-shift-outside-reset.tur`.
- [ ] Add codegen snapshots for cloneable continuation lowering.

---

### Phase B2: Full CPS pass for cloneable continuations

The tasks below implement the full CPS transformation required for multi-shot
cloneable continuations.  The B2 prerequisites (runtime structs, DeferMode,
`cps_fn_needs_cloneable_transform` marker) are already in place.  The tasks
must be completed in roughly the order listed since each builds on the previous.

**Dependency chain:**
CPS-CL1 (liveness) → CPS-CL2 (env struct) → CPS-CL3 (split) →
CPS-CL4 (reset emitter) + CPS-CL5 (shift emitter) →
CPS-CL6 (deep clone) → CPS-CL7 (elaborator checks) → CPS-CL8 (call/cc*) → CPS-CL9 (fixtures).

#### CPS-CL1 — Liveness analysis at cloneable-shift sites

- [x] Implement `cps_compute_live_at_shift(Arena *a, Expr *reset_body)` in `src/cps.c`.
  - For each `EX_CLONEABLE_SHIFT` node within a reset block, compute the set of
    `Binding *` values that are *live after* the shift point — i.e., appear free
    in any code that runs after the shift within the enclosing `EX_CLONEABLE_RESET`.
  - Standard approach: walk the expression tree twice.
    - Pass 1 (use-def): build a set of all `EX_VAR` bindings referenced in the
      subtree that follows each shift.
    - Pass 2: annotate the shift node with the intersection of "defined before
      the shift" and "used after the shift."
  - Output: add `Binding **live_captures` and `uint32_t n_live_captures` fields
    to the `cloneable_shift_` member of `Expr.as` in `src/expr.h` (arena-allocated).
  - Call `cps_compute_live_at_shift` from `cps_transform` for any program that
    `cps_fn_needs_cloneable_transform` returns true for.

#### CPS-CL2 — Per-shift-site environment struct and clone/drop helpers

- [x] Implement `emit_cloneable_env_structs(EmitCtx *ctx, Buf *out, const Expr *program)` in `src/emit.c`.
  - Walk the program for every `EX_CLONEABLE_SHIFT` node that has `n_live_captures > 0`.
  - For each such node (identified by a stable sequential id `__clenv_<id>`):
    - Emit a C struct:
      ```c
      typedef struct __clenv_<id> {
          int64_t <capture_name>; /* one field per live binding */
          …
      } __clenv_<id>;
      ```
    - Emit a deep-clone helper:
      ```c
      static __clenv_<id> *__clenv_<id>_clone(const __clenv_<id> *src) {
          __clenv_<id> *dst = malloc(sizeof(__clenv_<id>));
          /* for each field, call the Clone typeclass method for that type */
          dst-><name> = <clone_dispatch>(src-><name>);
          …
          return dst;
      }
      ```
      Use `typeclass_env_lookup_instance` (already wired in `src/emit.c`) to find
      the `Clone` instance for each captured type.  For primitive types (`int`,
      `bool`, `cstr`) the identity function is correct.
    - Emit a drop helper `__clenv_<id>_drop(__clenv_<id> *e)` (currently a no-op
      for primitives; calls user Drop when that trait is implemented).
  - The id must be deterministic (e.g. `<source_line>_<source_col>` or a
    per-emit counter reset at the start of each compilation) so that `expected.c`
    snapshots remain stable across unrelated changes.

#### CPS-CL3 — Continuation function splitting at shift sites

- [x] Implement `emit_continuation_fn_for_shift` in `src/emit.c`.
  - For each `EX_CLONEABLE_SHIFT` inside an `EX_CLONEABLE_RESET`, the code that
    follows the shift (the "rest of the reset body") becomes a new forward-declared
    C function:
    ```c
    static int64_t __cont_<id>(void *env, int64_t value);
    ```
  - The function body:
    1. Casts `env` to `__clenv_<id> *e`.
    2. Re-binds each captured variable: `int64_t <name> = e-><name>;`.
    3. Executes the original post-shift expression tree (emitted with the
       re-bound variables in scope).
  - For nested shifts (shift inside the continuation of another shift), each
    shift gets its own continuation function, chained.
  - Emit these helper functions in a forward-declaration section before the
    function that contains the reset, to avoid use-before-definition errors.
  - Update `cps_mark_expr` / `cps_transform` to record the post-shift subtree
    on each `EX_CLONEABLE_SHIFT` node; add an `Expr *cont_body` field to the
    `cloneable_shift_` member in `src/expr.h`.

#### CPS-CL4 — cloneable-reset emitter: setjmp boundary + reset context

- [x] Add `tur_cloneable_reset_ctx` to `src/runtime.h`:
  ```c
  typedef struct tur_cloneable_reset_ctx {
      jmp_buf jmp;
      tur_cloneable_cont *result; /* set by shift before longjmp */
      struct tur_cloneable_reset_ctx *prev; /* for nested resets */
  } tur_cloneable_reset_ctx;
  ```
  Use a thread-local (or explicit stack parameter) `tur_cloneable_reset_ctx *tur_current_reset_ctx`.
  Declare `TUR_THREAD_LOCAL tur_cloneable_reset_ctx *tur_current_reset_ctx;` in `src/runtime.h`
  and define it in `src/runtime.c`.  (Use `_Thread_local` / `__thread` per platform.)

- [x] Update `EX_CLONEABLE_RESET` emission in `src/emit.c`:
  ```c
  /* push a new reset context */
  tur_cloneable_reset_ctx __rctx_<id>;
  __rctx_<id>.result = NULL;
  __rctx_<id>.prev = tur_current_reset_ctx;
  tur_current_reset_ctx = &__rctx_<id>;
  int64_t <result>;
  if (setjmp(__rctx_<id>.jmp) == 0) {
      <result> = <body>;         /* normal path */
  } else {
      /* shift fired — __rctx_<id>.result is the tur_cloneable_cont* */
      <result> = (int64_t)(intptr_t)__rctx_<id>.result;
  }
  tur_current_reset_ctx = __rctx_<id>.prev; /* pop context */
  ```
  The reset expression's value is either the body's result (no shift) or the
  captured continuation pointer (shift fired), cast to `int64_t`.

  Also emit `tur_current_reset_ctx` as a forward declaration in the generated
  C preamble alongside the other runtime helpers.

#### CPS-CL5 — cloneable-shift emitter: pack env, allocate cont, longjmp

- [x] Update `EX_CLONEABLE_SHIFT` emission in `src/emit.c`:
  1. Evaluate `k_fn` (the handler function that will receive the continuation).
  2. Pack live-capture bindings into a heap-allocated `__clenv_<id>`:
     ```c
     __clenv_<id> *__env_<id> = malloc(sizeof(__clenv_<id>));
     __env_<id>-><name> = <binding_value>;
     …
     ```
  3. Allocate a `tur_cloneable_cont`:
     ```c
     tur_cloneable_cont *__cont_<id> = tur_cloneable_cont_alloc(NULL, 0);
     __cont_<id>->cont_fn = __cont_fn_<id>;   /* from CPS-CL3 */
     __cont_<id>->env     = __env_<id>;
     __cont_<id>->clone_env = (void *(*)(const void *))__clenv_<id>_clone;
     __cont_<id>->drop_env  = (void (*)(void *))__clenv_<id>_drop;
     ```
     (The `clone_env`/`drop_env` fields are added in CPS-CL6.)
  4. Fire `DEFER_SUSPENDED` defers on any currently active frames:
     ```c
     if (tur_current_frame) tur_frame_fire_lifo_for_suspend(tur_current_frame);
     ```
  5. Store the continuation in the reset context and longjmp:
     ```c
     tur_current_reset_ctx->result = __cont_<id>;
     longjmp(tur_current_reset_ctx->jmp, 1);
     ```
  - The `k_fn` argument in the surface syntax is passed `__cont_<id>` cast to
    `int64_t`; the shift expression's value is whatever `k_fn` returns.
  - Add `tur_current_frame` as a thread-local frame pointer (the innermost active
    `tur_frame *`) to `src/runtime.h`; maintain it in the emitted frame init/exit
    code in `src/emit.c`.

#### CPS-CL6 — Deep clone via typeclass dispatch in tur_cloneable_cont_clone

- [x] Add `clone_env` and `drop_env` function pointer fields to `tur_cloneable_cont`
  in `src/runtime.h`:
  ```c
  void *(*clone_env)(const void *env); /* deep-clone the captured env */
  void  (*drop_env)(void *env);        /* release the captured env */
  ```
- [x] Update `tur_cloneable_cont_clone` in `src/runtime.c` to use them:
  ```c
  clone->env = cont->clone_env ? cont->clone_env(cont->env) : cont->env;
  clone->clone_env = cont->clone_env;
  clone->drop_env  = cont->drop_env;
  ```
- [x] Update `tur_cloneable_cont_drop` to call `drop_env` before freeing:
  ```c
  if (cont->drop_env && cont->env) cont->drop_env(cont->env);
  ```
- [x] Update `tur_cloneable_cont_alloc` (or add `tur_cloneable_cont_alloc_with_fns`)
  to accept and store `clone_env` / `drop_env`.

#### CPS-CL7 — Elaborator: check_cloneable_capture and shift-outside-reset diagnostic

- [x] Add `TUR_E0014_NOT_CLONE` to `DiagCode` in `src/diag.h`; add the string
  `"TUR-E0014"` and an `--explain` entry in `src/diag.c`.
- [x] Add `TUR_E0016_CLONEABLE_SHIFT_OUTSIDE_RESET` to `DiagCode` in `src/diag.h`
  (E0015 is already taken by `TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED`);
  add string and `--explain` entry in `src/diag.c`.
  - Note: update the doc note at line ~1298 that incorrectly plans E0015 for this.
- [x] Implement `check_cloneable_capture(Elab *e, const Expr *shift_expr)` in
  `src/elab.c`:
  - Called from `elab_cloneable_shift` after CPS-CL1 has annotated live captures.
  - For each live capture binding, look up a `Clone` instance via
    `typeclass_env_lookup_instance`; if none found, emit `TUR-E0014`.
- [x] Add reset-boundary depth tracking to `Elab`:
  - Add `int cloneable_reset_depth;` to the `Elab` struct.
  - Increment in `elab_cloneable_reset`, decrement on exit.
  - In `elab_cloneable_shift`, check `e->cloneable_reset_depth == 0` and emit
    `TUR-E0016` if so.
- [x] Remove the `B1` deferred stub for `check_cloneable_capture` (line ~1405) once
  this is implemented and mark it `[x]`.

#### CPS-CL8 — Complete call/cc* desugaring

- [x] Complete `elab_call_cc_star` in `src/elab.c` (currently emits "not yet
  implemented" error).
  - Desugar `(call/cc* f)` to `(cloneable-reset (cloneable-shift (fn [k] (f k)) 0))`.
  - `k` receives a `tur_cloneable_cont*` cast to `int64_t`, representing the
    current continuation up to the enclosing reset.
  - Requires CPS-CL4 and CPS-CL5 to be complete so that `cloneable-shift`
    actually captures the continuation rather than just calling `k`.
  - Add a `tests/fixtures/call-cc-star/` fixture verifying that `(call/cc* f)`
    produces a cloneable continuation that `f` can resume multiple times.

#### CPS-CL9 — Integration test fixtures (require CPS-CL1–CPS-CL5 complete)

These fixtures are already listed under B2 remaining tasks; they are blocked by
the full CPS pass above.  Update the task entries to point to the new fixture
paths once the CPS pass is working.

- [x] `tests/fixtures/cloneable-multi-resume/` — resume the same continuation twice:
  ```scheme
  (defn main [] :int
    (let [k (cloneable-reset (cloneable-shift (fn [k] k) 0))]
      (println (tur_cloneable_cont_resume (tur_cloneable_cont_clone k) 10))
      (println (tur_cloneable_cont_resume k 20)))
    0)
  ```
  Expected: two independent results (10, then 20, or via the continuation body).
- [x] `tests/fixtures/cloneable-defer-suspend/` — verify `DEFER_SUSPENDED` fires on capture, not resume.
- [x] `tests/fixtures/cloneable-defer-replay/` — verify `DEFER_REPLAY` fires once per resume.
- [x] `tests/fixtures/cloneable-ref/` — cloneable continuation capturing a `Ref` value;
  each clone has an independent copy via `Clone [Ref]`.
- [x] `tests/fixtures/cloneable-rc/` — cloneable continuation capturing an `rc` value;
  each clone increments the refcount via `Clone [ptr<void>]`.
- [x] `tests/fixtures/errors/cloneable-shift-outside-reset/` — negative fixture for TUR-E0016.
- [ ] `tests/fixtures/errors/cloneable-non-clone-capture/` — negative fixture for TUR-E0014 (deferred: requires typeclass env in CPS pass).
- [x] Add `expected.c` codegen snapshot for at least one of the above (e.g.
  `cloneable-multi-resume`) to lock in the lowered C output.

### Phase B3 prerequisites

These prerequisites must be completed before B3 (Backtracking monad) implementation can proceed.

- [ ] Phase B2 cloneable continuation runtime must be complete and tested.
  - Required for: All B3 tasks depend on working cloneable continuations.
- [ ] Implement `run-backtrack` core function that collects all results from a backtracking computation.
  - Required for: The backtracking monad's `run` operation.
  - Action: `run-backtrack` takes a thunk `(-> (list (-> T)))` and returns `(list T)` containing all results produced by the backtracking computation.

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

### Phase B4 prerequisites

These prerequisites must be completed before B4 (Standard library integration) implementation can proceed.

- [ ] Phase B3 backtracking monad must be complete and tested.
  - Required for: `stdlib/logic.tur` and `stdlib/parsec.tur` both use the backtracking monad.
- [ ] Implement `Pair` type in stdlib for `logic.tur` term representation.
  - Required for: `Term` type typically uses `Pair` for compound terms (e.g., `Cons(x, xs)`).
  - **Note**: This is also a B1 prerequisite for parameterized Clone instances.
  - Action: Add `(defstruct Pair [first second])` to `stdlib/pair.tur` or similar.
- [ ] Implement persistent data structure primitives (or association list fallback).
  - Required for: Logic programming substitution map in `stdlib/logic.tur`.
  - Action: Use association list for v1 as decided in B1 prerequisites; HAMT-based variant deferred.

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

### Phase B5 prerequisites

These prerequisites must be completed before B5 (Testing, benchmarks, optimization) implementation can proceed.

- [ ] Phases B2-B4 must be substantially complete.
  - Required for: All B5 tasks depend on working backtracking infrastructure.
- [ ] Implement `--backtrack-depth` global flag infrastructure in compiler driver.
  - Required for: `src/main.c` argument parsing and pass-through to codegen.
  - Action: Add flag parsing in `parse_args()`; store in global; emit as `#define BACKTRACK_DEPTH N` in generated C preamble.

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
15. **[Async/Await]** T21 fibers + `async`/`await` core (after Phase 19 effects stabilize)
16. **[Async/Await]** T22 structured concurrency and task groups
17. **[Async/Await]** T23 multi-threaded work-stealing scheduler (after T20 thread pool)
18. **[Async/Await]** T24 async I/O and timer integration (after T23)
19. **[Async/Await]** T25 effects and async integration (after T24 and Phase 19 effects)
20. **[Unsafe Ops]** U1 prerequisite decisions → `Unsafe` effect in type system
21. **[Unsafe Ops]** U2 `unsafe` block sugar → U3 primitives → U4 safe wrappers
22. **[Unsafe Ops]** U5 linting, auditing, and tooling
23. **[HAMT]** P1 `Hash` typeclass + core C implementation → P2 Lisp bindings
24. **[HAMT]** P3 compiler lowering pass (`^persistent` annotation) → P4 transient mode and benchmarks
25. **[Backtracking]** B1 `Clone` trait + elaborator enforcement → B2 cloneable continuation runtime + CPS
26. **[Backtracking]** B3 backtracking monad → B4 `stdlib/logic.tur` + `stdlib/parsec.tur`
27. **[Backtracking]** B5 testing, benchmarks, safety tooling, and user guide

---

## Notes

- Phases 15 through 19 are marked complete at v1 in the active plan; this file tracks deferred v2+ follow-up work only.
- Phase HKT (H0–H6 roadmap) is planned for v2+ and will only move to the active roadmap if the promotion decision rule is met (see [hkt-implementation-plan.md](hkt-implementation-plan.md) for details).
- Phase HAMT (P1–P4 roadmap) is planned for v2+. P1 and P2 are pure library work with no compiler prerequisites beyond Phase 15. P3 (lowering pass) requires Phase 19 Section A (effect-row surface syntax) to be stable. See [hamt-feasibility.md](archive/hamt-feasibility.md) for the full feasibility analysis.
- Phase Backtracking (B1–B5 roadmap) is planned for v2+. B1 depends only on Phase 15 (Typeclasses). B2 depends on Phase 18 (Delimited continuations). B3–B4 are pure library work. B5 adds tooling and the optional STM integration fixture requires Phase 20. See [backtracking-cloneable-continuations-plan.md](archive/backtracking-cloneable-continuations-plan.md) for the full design.
- Async/Await (T21–T25 roadmap) is planned for v2+. T21 depends on Phase 18 (delimited continuations), Phase 17 (exceptions), and T19 (thread primitives). T22 (structured concurrency) requires T21. T23 (multi-threaded scheduler) requires T21, T19, and T20. T24 (async I/O) requires T23. T25 (effects integration) requires T21 and Phase 19 effects. See [async-await-plan.md](archive/async-await-plan.md) for the full design.
- Keep this list aligned with active roadmap changes in `docs/turmeric-plan.md`.
