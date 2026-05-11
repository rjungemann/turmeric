# Deferred Tasks Backlog — Phases 7 through 12

Purpose: track deferred items from Phases 7, 8, 9, 10, 11, and 12, why they were deferred, and what to investigate next.

## Why These Tasks Were Initially Deferred

These tasks were deferred for three recurring reasons:

1. Dependency ordering
Phase 7 and Phase 11 were designed to unblock core language/compiler progress first. Items depending on richer runtime, test harness, or deeper type/struct semantics were postponed.

2. Scope control for phase exit
The archived roadmap prioritized shipping minimal viable behavior with green fixtures. Advanced polish (full runtime stdlib behavior, full test UX, strict struct-copy validation, deep edge-case checks) was held back to avoid phase creep.

3. Missing infrastructure
Several items depend on infrastructure that was intentionally incomplete at the time:
- richer stdlib runtime contracts
- robust test registration/execution and CLI test command flow
- deeper struct type/field validation pipeline
- broader snapshot/golden coverage depth

---

## Actionable Prerequisite Tasks (Checkbox Backlog)

Use this section first. These items unblock the deferred work below.

### Cross-cutting prerequisites
- [x] Define ownership and acceptance criteria for this backlog (what counts as done for each task).
- [x] Confirm fixture naming convention and expected output format for all new tests (stdout, stderr, expected.diag, snapshots).
- [x] Decide snapshot strategy: keep codegen snapshots in existing fixture folders or create a dedicated snapshot layout.
- [x] Add a short contributor note documenting how to run only these follow-up fixtures locally.

### Phase 7 prerequisites
- [x] Write a test runner contract doc (discovery model, registration model, pass/fail semantics, exit codes, stdout/stderr shape).
- [x] Decide whether `cond` remains a special form or should migrate to macro form (record decision and rationale).
- [x] Confirm macro system readiness for `case`/`deftest` expansion (required list/data helpers available or not).
- [x] Define the stdlib runtime validation matrix per module (`vec`, `slice`, `str`, `option`, `result`).
- [x] Define negative-test expectations for bounds checks (`slice-get`, `vec-get`) including exact diagnostics.
- [x] Define callback-callability contract for function-valued parameters (argument typing, callability checks, arity rules).
- [x] Add a minimal fixture proving function values can be passed and called as test callbacks.

### Phase 11 prerequisites
- [x] Decide whether `TY_COPY_TRAIT` remains a placeholder or is replaced by typeclass-only modeling.
- [x] Specify `return` move-transfer semantics precisely (including suppression rules and diagnostics expectations).
- [x] Define `defstruct :copy` validation contract (field scan rules, rejected types, diagnostic wording).
- [x] Specify closure/defer behavior when moved bindings are referenced (including span expectations and note chaining).
- [x] Define representative codegen snapshot set for copy/move flows (let/set/call/return, copy vs move types).

---

## Actionable Remaining Tasks (Checkbox Backlog)

After prerequisites are complete, execute these implementation tasks.

### Phase 7 remaining tasks

#### A) Stdlib macro parity
- [ ] Implement `cond` as macro.
- [ ] Implement `case` macro with fixture coverage for branch selection and fallthrough behavior.
- [ ] Implement `deftest` macro integration with the selected test registration model.
- [ ] Add/update fixtures validating macro expansion and runtime behavior for `cond`/`case`/`deftest`.

#### B) Full stdlib runtime behavior validation
- [x] Add functional fixtures for `vec` (new, push, pop, len, get, free).
- [x] Add functional fixtures for `slice` (new, len, get, free, interaction with backing storage).
- [x] Add functional fixtures for `str` (from-cstr, len, eq, free).
- [x] Add functional fixtures for `option` (some/none/some?/unwrap/unwrap failure behavior as defined).
- [x] Add functional fixtures for `result` (ok/err/ok?/unwrap/unwrap-or behavior).
- [x] Add negative bounds-check fixtures for `slice-get` and `vec-get` with expected diagnostics.
- [x] Add codegen snapshots for stdlib type usage paths chosen in the snapshot strategy.

#### C) Test runner completeness
- [x] Implement `assert-true` and `assert-false`.
- [x] Implement `assert-nil`.
- [x] Implement `assert-error` behavior per agreed contract.
- [x] Implement `run-test` with pass/fail reporting and integration hooks.
- [x] Enable callback-based test execution (`passing function values as test callbacks is not supported in the current path`).
- [x] Upgrade `run-tests!` from stub to full discovery/registration execution flow.
- [x] Implement output format (`.` for pass, `F` for fail, summary line, failure details).
- [x] Implement and wire `tur test` subcommand behavior for directory-level test execution.
- [x] Add CLI fixtures for `tur test` success, failure, and mixed cases.

### Phase 11 remaining tasks

#### A) Copy trait placeholder and annotations
- [x] Finalize handling of `TY_COPY_TRAIT` decision (retain, refine, or remove placeholder path).
- [x] Implement explicit `:move` annotation handling depth consistent with `:copy` behavior.
- [x] Add tests for annotation parsing/diagnostics around `:copy` and `:move`.

#### B) Return-transfer semantics and suppression nuances
- [x] Implement move tracking on `return` edges according to agreed ownership semantics.
- [x] Implement move suppression behavior where ownership transfers to caller.
- [x] Add fixtures for nested returns, early returns, and mixed copy/move return expressions.
- [x] Add diagnostics fixtures for invalid post-return uses of moved bindings.

#### C) defstruct copy validation completeness
- [x] Implement strict field-level `:copy` validation using copy-kind checks.
- [x] Reject `:copy` structs containing non-copy fields with targeted diagnostic text.
- [x] Add positive fixture for valid `:copy` struct.
- [x] Add negative fixture for invalid `:copy` struct (for example `ref<T>` field).

#### D) Closure/defer moved-binding edge checks
- [x] Add closure capture analysis check for moved bindings.
- [x] Add defer body analysis check for moved bindings.
- [x] Add fixtures for capture/use before and after move points.
- [x] Ensure diagnostics include both move site and invalid reference site where applicable.

#### E) Snapshot depth for copy/move behavior
- [x] Add codegen snapshots for `let` move/copy paths.
- [x] Add codegen snapshots for `set!` move/copy paths.
- [x] Add codegen snapshots for function/builtin call argument move/copy paths.
- [x] Add codegen snapshots for return transfer paths.
- [x] Verify snapshots demonstrate no runtime bookkeeping overhead for static move/copy analysis.

### Closeout tasks
- [x] Re-run targeted fixtures for all new Phase 7 follow-up work.
- [x] Re-run targeted fixtures for all new Phase 11 follow-up work.
- [ ] Update docs/turmeric-plan.md or successor active roadmap with any promoted follow-up items.
- [x] Add a short completion note to this file indicating which deferred clusters were fully resolved.

### Phase 11 Completion Note

Resolved deferred clusters in this follow-up pass:
- A) Copy trait placeholder and annotations
- B) Return-transfer semantics and suppression nuances
- C) defstruct copy validation completeness
- D) Closure/defer moved-binding edge checks
- E) Snapshot depth for copy/move behavior

Outstanding cross-phase closeout still tracked separately:
- roadmap promotion/update task above

---

## Legacy Narrative Sections (Condensed)

The detailed narrative sections previously used for Phase 7 and Phase 11 deferred follow-ups were intentionally condensed to avoid duplication.

- For actionable prerequisites, use: `Actionable Prerequisite Tasks (Checkbox Backlog)`.
- For actionable implementation work, use: `Actionable Remaining Tasks (Checkbox Backlog)`.

---

## Actionable Prerequisite Tasks — Phases 8, 9, 10 (Checkbox Backlog)

### Phase 8 prerequisites (Diagnostics polish)
- [x] Decide whether operator-overload diagnostics should be fully generic now or delayed until broader overload surface exists.
- [x] Define docs-link policy for diagnostics (`DiagSuggestion.doc_url` source of truth, fallback behavior when docs are local/offline).
- [x] Define golden strategy for multi-line diagnostics (`expected.diag` normalization rules for snippets/colors/line numbers).
- [x] Decide CI policy for unknown spans in snapshots (`SPAN_UNKNOWN` guard scope and failure thresholds).

### Phase 9 prerequisites (`rc<T>` + `weak<T>`)
- [x] Define destructor/drop ABI for user-defined types that need custom `drop_fn` wiring.
- [x] Decide compiler strategy for rc auto-drop injection (`let`-bound `rc/of` only vs broader ownership-flow analysis).
- [x] Specify rules for `rc/from-ref` and `ref/from-rc` (poisoning semantics, unique-ownership check behavior, diagnostics).
- [x] Define deferred-free queue requirements (queue type, flush points, recursion-guard behavior).
- [x] Clarify scope for weak dangling behavior fixture (`upgrade -> NULL` vs hard runtime error path).
- [x] Define concrete compiler pipeline hook for `drop_fn` selection at allocation sites (`rc/of`, conversions), including where resolved drop symbols are stored in IR.
- [x] Define auto-generated drop-glue contract for user composite types (field traversal order, nested RC handling, cycle-safe behavior, and re-entrancy rules).
- [x] Define non-Copy payload drop policy matrix by type category (primitive, ref, rc, weak, struct, tuple, function) and whether policy is static, dynamic, or hybrid.
- [x] Define destructor ordering guarantees between user drop glue, runtime free-queue draining, and control-block release.
- [x] Define diagnostics contract for invalid drop-policy configurations (missing drop glue, unsupported type forms, conflicting ownership annotations).
- [x] Define last-use elision safety model (alias assumptions, side-effect barriers, call/loop boundaries, volatile/extern interactions).
- [x] Define verification strategy for elision correctness (IR assertions, codegen invariants, and fixture classes for positive and negative elision cases).

#### Phase 9 prerequisite decisions (2026-05-10)

1. `drop_fn` pipeline hook and IR storage
- Resolve `drop_fn` during elaboration after type finalization, before lowering/codegen.
- Store resolved drop identity on RC-producing expressions as a nullable IR field (for example on `EX_RC_OF` / conversion nodes), not by re-deriving in emitter.
- Keep runtime ABI as function pointer `void (*drop_fn)(void *)`; emit `NULL` only for unreachable/internal placeholder paths, never for valid non-Copy payloads.
- For `rc/from-ref`, carry over the same resolved payload drop contract as if created by `rc/of` for that payload type.

2. Auto-generated drop glue for composite types
- Generate one monomorphic drop-glue function per concrete payload type that requires destruction.
- Field traversal order is reverse declaration order (LIFO-like unwind semantics).
- Nested RC/weak fields: use RC operations (`rc_strong_decrement` / weak decrement path), never raw `free`.
- Non-owning fields (`weak`, borrowed pointers, function pointers) do not own payload memory and are not recursively freed.
- Drop glue must be re-entrancy-safe: no global mutable recursion counters; rely on RC counts + deferred free queue behavior.

3. Non-Copy payload policy matrix
- Primitive scalars: static no-op policy (`drop_fn` may be default free-only of payload storage).
- `ref<T>` payloads: explicit owned-free policy (free referenced storage according to ref ownership rules).
- `rc<T>` payloads: decrement policy on inner RC handle(s), not deep free.
- `weak<T>` payloads: weak-release policy only.
- Struct/tuple payloads: synthesized glue by field policy composition.
- Function values/closures: treat captured environment ownership according to closure representation; if owning env exists, provide glue; otherwise no-op.
- Policy selection is static by type kind in this phase; no runtime type switching.

4. Destructor ordering guarantees
- Logical order on last strong release: user drop glue runs before control-block free.
- Any RC releases triggered by drop glue may enqueue additional frees; queue drain occurs at defined flush points (existing explicit drains and scope-end/runtime shutdown points as implemented).
- Control block memory is released only after payload destructor path completes for that block.
- Weak cleanup remains count-driven and cannot observe partially dropped payload state as live.

5. Diagnostics contract
- Missing drop glue for a required non-Copy payload: hard compile-time diagnostic at allocation/conversion site.
- Unsupported type forms in RC payload (if any remain): hard compile-time diagnostic naming the offending type shape.
- Conflicting ownership annotations affecting drop policy inference: emit primary span at declaration plus note at conflicting site.
- Diagnostic text should name operation (`rc/of`, `rc/from-ref`, etc.) and required drop contract.

6. Last-use elision safety model
- Elide retain/release only when SSA-like last-use proof is available in the current function body.
- Do not elide across unknown side-effect boundaries: extern calls, opaque builtins, loops with unknown aliasing, or captured closure escapes.
- Treat address-taking/escape of RC handles as an elision barrier.
- Prefer conservative false negatives over unsound elision.

7. Elision verification strategy
- Add IR-level assertions: no negative refcount transitions and no dropped-live-use edges after elision.
- Add codegen invariants checks in snapshots for representative patterns (straight-line, branch, loop barrier, call barrier).
- Add fixture classes:
	- Positive: obvious last-use in straight-line code.
	- Negative: alias/escape/call-boundary cases where elision must not fire.
	- Regression: `ref/from-rc` + deferred queue interaction under elision-disabled boundary.

### Phase 10 prerequisites (GC v1 Bacon-Rajan infrastructure)
- [x] Define type-metadata shape needed to scan object fields for RC pointers during trial deletion.
- [x] Decide minimal v2 trial-deletion rollout (which object categories become scannable first).
- [x] Define threshold-mode trigger policy (buffer size, hysteresis, collection frequency caps).
- [x] Decide `gc_is_alive`/`rc_upgrade` contract once trial deletion is active.
- [x] Define GC ABI documentation scope and owner (control block layout, colors, suspect buffer semantics).

---

## Actionable Remaining Tasks — Phases 8, 9, 10 (Checkbox Backlog)

### Phase 8 remaining tasks
- [x] Implement operator lookup failure diagnostics showing operator, arg types, and available overload set.
- [x] Add docs URL plumbing for suggestions and include at least one fixture/assertion covering URL emission.
- [x] Add/update golden fixtures for multi-line diagnostic outputs under `tests/fixtures/errors/*.diag`.
- [x] Add CI/lint check to fail when snapshot IR contains `SPAN_UNKNOWN` where prohibited.

### Phase 9 remaining tasks

#### RC lifecycle and drop behavior
- [ ] Populate `drop_fn` for user-defined destructor-bearing RC payloads. **[PARTIAL: wired default typed hooks for `ref<T>`, `rc<T>`, and `weak<T>` payload kinds; user-defined composite payload glue still pending]**
- [ ] Inject `(defer (rc/drop x))` for `let` bindings of `(rc/of ...)` **[DEFERRED: complex interaction with ref/from-rc consumption; needs binding-usage analysis]**.
- [x] Implement type-dependent drop policy for non-Copy payloads via explicit drop hooks (current Phase 9 kind set: `ref`, `rc`, `weak`).
- [ ] Implement/verify last-use elision for redundant retain/release paths. **[DEFERRED: requires SSA-like usage analysis; fixtures created as baseline; full implementation in Phase 9 follow-up]**

#### Phase 9 follow-up: SSA-like last-use elision implementation
- [ ] Add a per-function SSA-like usage pass that computes last-use information for RC values (including linear flow + branch merge handling).
- [ ] Define and implement elision eligibility rules in the pass (single-use clones, no alias/escape, no opaque side-effect barriers, no loop uncertainty).
- [ ] Wire pass results into lowering/emission so eligible retain/release pairs are skipped without changing observable semantics.
- [ ] Add conservative bailout diagnostics/logging mode for development builds to explain why candidate elisions were rejected.
- [ ] Extend regression fixtures with positive branch/merge elision cases and negative barrier cases (extern call, closure escape, address-taken).
- [ ] Add at least one fixture covering `ref/from-rc` interaction to verify elision never breaks uniqueness/poison semantics.
- [ ] Add snapshot assertions to prove removed retain/release calls in positive cases and preserved calls in blocked cases.
- [ ] Re-run full Phase 9 fixture matrix and record pass/fail summary in this doc when the follow-up lands.

#### Queueing and conversions
- [x] Implement deferred free queue to avoid deep recursive free chains.
- [x] Implement `(rc/from-ref r)` conversion with move/poison semantics.
- [x] Implement `(ref/from-rc r)` conversion with strong-count==1 enforcement.
- [x] Add negative diagnostic for non-unique `ref/from-rc` attempts.

#### Phase 9 fixtures
- [x] Add `weak-dangling.tur` fixture for post-drop weak behavior.
- [x] Add `rc-ref-conversion.tur` fixture.
- [x] Add `rc-unique-violation.tur` negative fixture.
- [x] Add `rc-nested-free-queue.tur` fixture for deferred queue stress test.
- [x] Add `rc-elision-positive.tur` fixture for obvious last-use elision cases.
- [x] Add `rc-elision-negative-escape.tur` fixture for cases where elision is blocked.

#### Phase 9 Completion Note (Follow-up Session)

Completed in this session:
- Extended `collect_free_vars` traversal to handle all new rc/weak expression kinds (rc/of, rc/clone, rc/drop, rc/from-ref, ref/from-rc, weak, upgrade, strong-count, weak?, ref?)
- Implemented proper free-variable capture for defer bodies containing rc/weak operations
- Refined auto-drop semantics: refs extracted via ref/from-rc are no longer auto-dropped (they don't own the data)
- Validated conversion fixtures all pass: `weak-dangling`, `rc-ref-conversion`, `rc-unique-violation`
- Documented that rc auto-drop injection is deferred pending more sophisticated binding-usage analysis

**Additional completion in continuation session:**
- Implemented deferred free queue infrastructure to prevent stack overflow on deeply nested RC value drops
- Added rc_free_queue.h/c with queue management functions (push, drain)
- Modified rc.c rc_strong_decrement to use queue instead of immediate freeing
- Updated emit.c to generate rc_free_queue_drain calls after rc_strong_decrement operations
- Added type-aware default drop hooks for non-Copy payload kinds (`ref`, `rc`, `weak`) and wired selection in runtime + emitted runtime (`rc_cb_alloc`, `tur_rc_from_ref`)
- Added fixture `rc-drop-hook-inner-rc` validating nested `rc<rc<T>>` payload release behavior (`2 -> 1` strong-count transition)
- Validated all Phase 9 fixtures pass: rc-basic, rc-shared, rc-cycle-leak, rc-auto-drop-injection, weak-upgrade, weak-dangling, rc-ref-conversion, rc-drop-hook-inner-rc

**Latest continuation session (elision baseline):**
- Created `rc-elision-positive.tur` fixture demonstrating obvious last-use elision candidate (single clone-use-drop sequence)
- Created `rc-elision-negative-escape.tur` fixture showing cases where elision must not fire (multiple uses, extern call barriers)
- Documented elision opportunity: when RC is cloned, used exactly once, then immediately dropped, the increment/decrement pair can be elided
- Deferred full SSA-like analysis implementation to Phase 9 follow-up; current baseline shows where optimization would apply
- All 12 Phase 9 fixtures now pass: rc-basic, rc-shared, rc-cycle-leak, rc-auto-drop-injection, weak-upgrade, weak-dangling, rc-ref-conversion, rc-unique-violation, rc-nested-free-queue, rc-drop-hook-inner-rc, rc-elision-positive, rc-elision-negative-escape

### Phase 10 remaining tasks

#### Collector core
- [ ] Implement trial deletion over suspect roots using type metadata field scanning.
- [ ] Complete suspect-identification integration in `gc_collect()`.
- [ ] Integrate `rc_upgrade` with finalized liveness check contract.
- [ ] Add optional `may_contain_cycles=false` fast-path in allocation/collection flow.

#### Collector modes and behavior
- [ ] Implement threshold mode (auto collection when suspect buffer crosses N).
- [ ] Define and, if in scope, implement background mode scaffolding (or explicitly defer with API stubs).

#### Phase 10 fixtures and validation
- [ ] Add `gc-dag.tur` fixture.
- [ ] Add `gc-mixed.tur` fixture.
- [ ] Add `gc-stress.tur` fixture.
- [ ] Add `gc-deterministic.tur` fixture.
- [ ] Add `gc-cycle-freed.tur` fixture.
- [ ] Add `gc-no-false-positives.tur` fixture.
- [ ] Add `gc-perf.tur` fixture and perf baseline notes.
- [ ] Add codegen snapshots for GC control block layout and collector integration points.

#### Documentation and future-proofing
- [ ] Document GC ABI and collector state machine in a dedicated doc section.
- [ ] Add/decide `collector_hook` extension point for custom collector implementations.

---

## Actionable Prerequisite Tasks — Phase 12 (Checkbox Backlog)

### Type-system and semantics prerequisites
- [x] Decide explicit variance model for borrow types (`&T`, `&mut T`) and document whether variance is needed now vs deferred.
- [x] Define overloaded deref contract for borrow refs vs owning refs (`@r` behavior and error precedence rules).
- [x] Define semantics for mutating through borrowed references (`set! (@ r) ...`) including immutable-borrow diagnostics.
- [x] Specify parser/readability stance for `&x` sugar and `&mut x` tokenization (keep deferred or implement now).

### Borrow-checker scope prerequisites
- [x] Define minimum interop level for borrowing from `ref<T>` and raw `ptr<T>` in Phase 12 follow-up.
- [x] Define closure/defer borrow-lifetime rules and expected diagnostics (including primary/secondary spans).
- [x] Decide whether `unsafe` opt-out is in scope now or remains deferred, and document boundary behavior.

### Fixture/snapshot prerequisites
- [x] Define fixture matrix for borrow edge cases (struct fields, reborrow, ptr/ref interop, closure/defer interactions).
- [x] Define codegen snapshot criteria proving borrow lowering remains runtime-zero-cost.

---

## Actionable Remaining Tasks — Phase 12 (Checkbox Backlog)

### Core language behavior
- [ ] Implement or formally defer covariance behavior for `&T` / `&mut T` with tests matching chosen policy.
- [ ] Implement overloaded dereference for borrow types (`@r` on `&T` and `&mut T`).
- [ ] Implement mutation through mutable borrows: `(set! (@ r) value)` with immutable-borrow rejection diagnostics.
- [ ] Implement/decide reader sugar for `&x` and normalize `&mut x` handling per parser policy.

### Borrow source and reborrow support
- [ ] Implement borrow-from-`ref<T>` semantics with lifetime/validity checks.
- [ ] Implement borrow-from-`ptr<T>` behavior with explicit unsafe/untracked diagnostics policy.
- [ ] Implement reborrowing for immutable borrows (`&` of an existing borrow) with lifetime propagation.
- [ ] Implement reborrowing for mutable borrows (`&mut` of an existing mutable borrow) with exclusivity enforcement.

### Struct/field and expression borrow cases
- [ ] Implement immutable struct-field borrowing (`(& (.field s))`).
- [ ] Implement mutable struct-field borrowing (`(&mut (.field s))`).
- [ ] Implement borrow-through-deref cases for immutable and mutable paths.

### Feature interaction checks
- [ ] Implement closure capture lifetime validation for borrowed values.
- [ ] Implement defer-body borrow-validity checks.
- [ ] Implement/decide `(unsafe ...)` borrow-check opt-out path and diagnostics.

### Phase 12 fixtures and snapshots
- [ ] Add `borrow-struct-field` fixture.
- [ ] Add `borrow-reborrow` fixture.
- [ ] Add `borrow-closure` fixture.
- [ ] Add `borrow-ref` fixture.
- [ ] Add `borrow-defer` fixture.
- [ ] Add `borrow-unsafe` fixture.
- [ ] Add `borrow-ptr` fixture.
- [ ] Add codegen snapshots for borrow lowering to pointer-level operations.

---

## Suggested Execution Order (Later)

1. Cross-cutting prerequisites (acceptance criteria, fixture conventions, snapshot strategy)
2. Phase 8 diagnostic infrastructure follow-through (goldens, URL policy, SPAN_UNKNOWN CI guard)
3. Phase 7 test runner contract + implementation (assert suite + run-tests semantics + tur test behavior)
4. Phase 7 stdlib functional/negative/runtime fixtures and snapshots
5. Phase 9 RC lifecycle upgrades (auto-drop injection, conversion APIs, deferred-free queue)
6. Phase 10 GC v2 foundations (trial deletion metadata + threshold mode path)
7. Phase 11 semantic edge completion (`:copy` validation, return-transfer, closure/defer move checks)
8. Phase 12 borrow edge completion (deref/mutation/reborrow/closure-defer interop + fixtures)
9. Snapshot/golden coverage expansion and final closeout sweep across all phases

---

## Notes

- Phases 7 through 12 are closed in the archive plan; this file tracks deferred follow-up work only.
- Keep this list aligned with any future active-plan items moved into docs/turmeric-plan.md.

## Executed Decisions (Current)

The following prerequisites were executed and documented:

- Test runner contract doc: [docs/test-runner-contract.md](docs/test-runner-contract.md)
- Contributor runbook + fixture conventions + local workflow: [docs/deferred-followup-contributor-note.md](docs/deferred-followup-contributor-note.md)
- Snapshot strategy decision: colocated snapshots per fixture folder (documented in contributor note)
- Diagnostics docs-link policy: keep `doc_url` optional; when docs are local/offline, omit URL and keep actionable suggestion text
- Multi-line diagnostics golden policy: `expected.diag` lines are matched as required substrings; avoid brittle full-snippet exact matching
- `assert-true`/`assert-false` implemented in `stdlib/test.tur` using current `1=true` / `0=false` convention (temporary until bool-typed function argument signatures are fully supported)
- Prerequisite decision log for remaining phases: [docs/deferred-prerequisite-decisions.md](docs/deferred-prerequisite-decisions.md)
- Callback callability contract details (v1): [docs/deferred-prerequisite-decisions.md](docs/deferred-prerequisite-decisions.md)
- Test-runner contract updated with callback callability section: [docs/test-runner-contract.md](docs/test-runner-contract.md)
- Runtime-negative fixture support in harness (`expected.exit`, `expected.stderr`) and new bounds fixtures:
	- [tests/fixtures/stdlib-vec-bounds-negative/input.tur](tests/fixtures/stdlib-vec-bounds-negative/input.tur)
	- [tests/fixtures/stdlib-slice-bounds-negative/input.tur](tests/fixtures/stdlib-slice-bounds-negative/input.tur)
- Codegen snapshots added for runtime-negative bounds fixtures:
	- [tests/fixtures/stdlib-vec-bounds-negative/expected.c](tests/fixtures/stdlib-vec-bounds-negative/expected.c)
	- [tests/fixtures/stdlib-slice-bounds-negative/expected.c](tests/fixtures/stdlib-slice-bounds-negative/expected.c)
- `tur test` subcommand implemented in driver and validated with CLI fixtures:
	- [src/main.c](src/main.c)
	- [tests/run-cli.sh](tests/run-cli.sh)
	- [tests/cli/tur-test-success](tests/cli/tur-test-success)
	- [tests/cli/tur-test-failure](tests/cli/tur-test-failure)
	- [tests/cli/tur-test-mixed](tests/cli/tur-test-mixed)
- `run-tests!` upgraded from stub to registry-backed execution with explicit registration helpers:
	- [stdlib/test.tur](stdlib/test.tur)
	- [tests/fixtures/stdlib-test-runner-registry/input.tur](tests/fixtures/stdlib-test-runner-registry/input.tur)
- `assert-error` implemented via typed `try`/`catch` callback assertion flow:
	- [stdlib/test.tur](stdlib/test.tur)
	- [tests/fixtures/stdlib-test-assert-error/input.tur](tests/fixtures/stdlib-test-assert-error/input.tur)
- Phase 11 defstruct follow-up: explicit `:move` annotation + strict `:copy` field validation and fixtures:
	- [src/elab.c](src/elab.c)
	- [tests/fixtures/defstruct-copy-valid/input.tur](tests/fixtures/defstruct-copy-valid/input.tur)
	- [tests/fixtures/defstruct-move-annotation/input.tur](tests/fixtures/defstruct-move-annotation/input.tur)
	- [tests/fixtures/errors/defstruct-copy-noncopy-field/input.tur](tests/fixtures/errors/defstruct-copy-noncopy-field/input.tur)
- Phase 11 copy-trait finalization: remove placeholder-path dependence and make copy/move defaults deterministic by type kind:
	- [src/types.h](src/types.h)
	- [src/types.c](src/types.c)
	- [src/elab.c](src/elab.c)
- Phase 11 move-suppression nuance: branch-local moves in `if` no longer poison outer path unless moved in both branches:
	- [src/elab.c](src/elab.c)
	- [tests/fixtures/ref-if-branch-move-suppression/input.tur](tests/fixtures/ref-if-branch-move-suppression/input.tur)
- Phase 11 return-transfer semantics completed: return-edge moves + suppression nuances + diagnostics fixtures:
	- [src/elab.c](src/elab.c)
	- [tests/fixtures/ref-return-early-branch/input.tur](tests/fixtures/ref-return-early-branch/input.tur)
	- [tests/fixtures/ref-return-nested-transfer/input.tur](tests/fixtures/ref-return-nested-transfer/input.tur)
	- [tests/fixtures/errors/ref-return-use-after-move/input.tur](tests/fixtures/errors/ref-return-use-after-move/input.tur)
- Phase 11 closure/defer moved-binding checks completed with two-span diagnostics (invalid use + move site):
	- [src/expr.h](src/expr.h)
	- [src/elab.c](src/elab.c)
	- [tests/fixtures/errors/ref-closure-capture-after-move/input.tur](tests/fixtures/errors/ref-closure-capture-after-move/input.tur)
	- [tests/fixtures/errors/ref-defer-use-after-move/input.tur](tests/fixtures/errors/ref-defer-use-after-move/input.tur)
- Phase 11 snapshot-depth coverage added for copy/move paths:
	- [tests/fixtures/phase11-snapshot-let-copy-move/input.tur](tests/fixtures/phase11-snapshot-let-copy-move/input.tur)
	- [tests/fixtures/phase11-snapshot-set-copy-move/input.tur](tests/fixtures/phase11-snapshot-set-copy-move/input.tur)
	- [tests/fixtures/phase11-snapshot-call-arg-copy-move/input.tur](tests/fixtures/phase11-snapshot-call-arg-copy-move/input.tur)
	- [tests/fixtures/phase11-snapshot-return-transfer/input.tur](tests/fixtures/phase11-snapshot-return-transfer/input.tur)
	- generated snapshots: `expected.c` in each fixture directory
- Static-overhead check: grep over new snapshots found no runtime move bookkeeping markers (`is_moved`, `tur_move`, `copy_kind`, `move_`).
- Phase 8 diagnostics follow-up completed:
	- overload lookup now reports available overload set (operator, arity, arg/result types): [src/elab.c](src/elab.c), [src/builtins.c](src/builtins.c), [src/builtins.h](src/builtins.h)
	- docs URL plumbing exercised via unbound-symbol suggestion path: [src/elab.c](src/elab.c)
	- new diagnostics fixtures: [tests/fixtures/errors/operator-overload-set/input.tur](tests/fixtures/errors/operator-overload-set/input.tur), [tests/fixtures/errors/unbound-symbol-doc-url/input.tur](tests/fixtures/errors/unbound-symbol-doc-url/input.tur)
	- SPAN_UNKNOWN snapshot guard in test flow: [tests/check-span-unknown.sh](tests/check-span-unknown.sh), [Makefile](Makefile)
- Phase 9 fixture follow-up:
	- added weak dangling behavior fixture for post-drop `upgrade` path: [tests/fixtures/weak-dangling/input.tur](tests/fixtures/weak-dangling/input.tur)
	- implemented `(rc/from-ref r)` and `(ref/from-rc rc)` expression kinds through elaboration and emission, with `ref/from-rc` uniqueness runtime enforcement:
		- [src/expr.h](src/expr.h), [src/expr.c](src/expr.c), [src/elab.c](src/elab.c), [src/emit.c](src/emit.c)
	- updated `rc/of` emission to allocate payload storage separately from control block to support safe ownership transfer in `ref/from-rc`:
		- [src/emit.c](src/emit.c)
	- added/validated conversion fixtures:
		- positive conversion: [tests/fixtures/rc-ref-conversion/input.tur](tests/fixtures/rc-ref-conversion/input.tur)
		- uniqueness violation negative: [tests/fixtures/rc-unique-violation/input.tur](tests/fixtures/rc-unique-violation/input.tur)
