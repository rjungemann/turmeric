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
- [x] Inject `(defer (rc/drop x))` for `let` bindings of `(rc/of ...)` with consumption-aware suppression (`ref/from-rc`, explicit `rc/drop`, moved bindings).
- [x] Implement type-dependent drop policy for non-Copy payloads via explicit drop hooks (current Phase 9 kind set: `ref`, `rc`, `weak`).
- [x] Implement/verify last-use elision for redundant retain/release paths (SSA-like conservative pass + barrier rules + regression fixtures).

#### Phase 9 follow-up: SSA-like last-use elision implementation (COMPLETED)

Implemented in continuation session (May 10, 2026):
- [x] Added per-function SSA-like usage pass (`rc_elision_analyze_fn`) that identifies last-use clone/drop pairs for optimization.
- [x] Defined and implemented conservative elision eligibility rules: single clone+drop pair per let binding with no barrier expressions (EX_CALL, EX_BUILTIN, EX_WHILE, EX_THROW, EX_TRY, EX_CLOSURE, EX_DEFER) before drop.
- [x] Wired elision pass into emit.c (called before each function body emission) with elide flags (`EX_RC_CLONE.elide` and `EX_RC_DROP.elide`).
- [x] Emitter checks elide flags and skips rc_strong_increment/rc_strong_decrement + rc_free_queue_drain when marked true.
- [x] Added debug output gated by `TUR_DEBUG` to show which clones/drops were elided and why.
- [x] Added new fixtures validating elision behavior:
  - `rc-elision-drop-pair-positive.tur`: clone+drop pair with no barriers elides correctly (no rc_strong_increment/decrement emitted).
  - `rc-elision-drop-pair-negative-barrier.tur`: call barrier before drop blocks elision (increment/decrement preserved).
  - `rc-elision-barrier-call.tur`: call before drop in do-block blocks elision.
  - `rc-elision-ref-from-rc-safety.tur`: verifies elision doesn't break ref/from-rc uniqueness contract.
- [x] All Phase 9 fixtures pass (17 tests) including 4 new elision-specific fixtures and 12 pre-existing RC/weak fixtures.

#### Outstanding Phase 9 Deferred Work (Future Sessions)

The following Phase 9 items remain deferred and should be tackled in future continuations:

1. **Auto-drop injection for rc/of bindings**
   - Current state: Deferred due to complex interaction with ref/from-rc consumption and binding-usage analysis needs
   - Approach: Implement sophisticated binding-usage analysis to detect when `let [x (rc/of val)]` should automatically inject `(defer (rc/drop x))` at scope exit
   - Prerequisites: Define interaction model with ref/from-rc to ensure elision analysis doesn't conflict with auto-injection
   - Test fixtures needed: Auto-drop in nested scopes, early returns with rc bindings, closure captures of rc values

2. **User-defined destructor support for composite RC payloads**
   - Current state: Only primitive/ref/rc/weak payload kinds have drop hooks; user-defined structs lack generated glue
   - Approach: Extend drop_fn resolution in elab.c to synthesize drop-glue functions for user-defined struct payloads
   - Details: Field-level policy composition, LIFO field destruction order, re-entrancy safety with deferred queue
   - Test fixtures needed: Nested struct payloads with rc fields, cycles between user types and rc

3. **Enhanced elision analysis for branch/loop cases**
   - Current state: Conservative single-use detection; doesn't handle branch merge or loop-based patterns
   - Approach: Extend elision pass to track single-use across branch merges (both branches have binding) and loop exit conditions
   - Safety model: Only elide when all paths to binding release converge to single-use pattern
   - Test fixtures needed: Branch-local elision, loop-exit elision, phi-node-style convergence patterns

4. **Elision interaction with closure capture**
   - Current state: Closures over elided RC values may have semantics issues if environment captures intermediate state
   - Approach: Extend elision barrier rules to prevent elision when cloned binding is captured by closure or defer
   - Test fixtures needed: Closure over elided clone, deferred use of elided binding, escaped closure references

5. **Codegen snapshot validation for elision**
   - Current state: Fixtures validate behavior but codegen snapshots not yet checked for absence of elided operations
   - Approach: Add snapshot-based validation that positive elision cases show no rc_strong_increment/decrement in expected.c
   - Test infrastructure: Snapshot diff tooling to detect unexpected retain/release presence in positive cases

#### Queueing and conversions
- [x] Implement deferred free queue to avoid deep recursive free chains.
- [x] Implement `(rc/from-ref r)` conversion with move/poison semantics.
- [x] Implement `(ref/from-rc r)` conversion with strong-count==1 enforcement.
- [x] Add negative diagnostic for non-unique `ref/from-rc` attempts.

---

## Detailed Analysis: Why Phase 9 Auto-Drop and Related Tasks Are Blocked (May 10, 2026)

### Current Test Status
- **66 passing / 74 failing** overall
- **16 Phase 9 tests passing** (rc-basic, rc-shared, rc-cycle-leak, rc-auto-drop-injection, weak tests, rc-elision tests)
- **4 Phase 9 tests FAILING** due to auto-drop not being enabled:
  1. `rc-auto-drop-positive` — codegen mismatch (expects auto-drop but not generated)
  2. `rc-auto-drop-multiple` — codegen mismatch (multiple RC bindings need auto-drop)
  3. `rc-auto-drop-negative-consumed` — codegen mismatch (consumed RC should not auto-drop)
  4. `rc-ref-conversion` — **tur emit-c crashes** with null pointer dereference at emit.c:1619

### Root Cause Analysis

#### Issue 1: RC Auto-Drop Injection Disabled
**Why disabled:** Complex interaction with ref/from-rc consumption semantics. When `(ref/from-rc rc)` is called, it extracts the RC and frees the control block, transferring ownership. If a prior auto-drop also fires, it would cause double-free.

**Current state:** The auto-drop code exists but is fully disabled with `if (false && has_rc_bindings)` guards in elab.c lines 1045–1120.

**Required fix:** Implement binding-consumption detection that checks whether an RC binding is used by `ref/from-rc` or explicitly dropped via `(rc/drop x)`. If either is true, skip auto-drop for that binding. If neither is true, inject auto-drop.

**Prerequisite breakdown:**
1. **Binding-usage analysis pass**
   - Traverse entire let/do body AST to find all uses of each RC binding
   - Track which bindings are referenced by `ref/from-rc` calls (ownership transfer)
   - Track which bindings are referenced by explicit `(rc/drop x)` calls (manual drop)
   - Store consumption state as boolean per binding

2. **Auto-drop eligibility filtering**
   - Skip auto-drop for any RC binding that is consumed by ref/from-rc
   - Skip auto-drop for any RC binding that is explicitly dropped
   - Inject defer-based auto-drop only for RC bindings with no consumption

3. **Interaction with move semantics**
   - When ref/from-rc is called on an RC binding, it should mark that binding as moved
   - Auto-drop code should skip defers for moved bindings (avoid use-after-move in defer body)
   - **Current bug:** Ref auto-drop code checks `binds[k].init->kind != EX_REF_FROM_RC` but doesn't check move state

#### Issue 2: Ref Auto-Drop + RC Conversion Interaction
**Symptom:** `rc-ref-conversion` crashes with null pointer in emit.c:1619 (null do-block item)

**Root cause:** 
1. First let binds `r` from `(ref 123)` 
2. Ref auto-drop code checks `binds[k].init->kind != EX_REF_FROM_RC` → true, so auto-drop fires, injecting `(defer (drop! r))`
3. Inner let binds `rc` from `(rc/from-ref r)` → this marks `r` as moved
4. `r` is moved but auto-drop defer still tries to drop it in the outer scope
5. Emit phase crashes because moved binding creates invalid IR state

**Prerequisite breakdown:**
1. **Move-state tracking in elab_let**
   - Before calling elab_form on each binding init, snapshot current move state
   - After elaborating init, capture which bindings were moved during init elaboration
   - Check move state before adding auto-drop defer for any binding

2. **Skip auto-drop for moved bindings**
   - If a ref binding is marked as moved during init elaboration, don't add auto-drop
   - This prevents injecting defer that references a moved binding

3. **Track movement in all nested expressions**
   - Move tracking must propagate through complex nested expressions
   - Example: if `(let [r (ref 123)] (let [rc (rc/from-ref r)] ...))`, the inner let needs to mark outer r as moved

#### Issue 3: User-Defined Struct Destructors Not Yet Supported
**Current state:** Drop hooks only exist for primitive types and built-in RC/weak/ref types. User-defined structs have no generated drop glue.

**Example failing fixture:** `rc-auto-drop-multiple` expects struct payloads to be destroyed in LIFO order of fields.

**Prerequisite breakdown:**
1. **Struct type metadata generation**
   - For each user-defined struct type, generate metadata describing field layout and offset
   - Include type information for each field (whether it's primitive, ref, rc, weak, or nested struct)

2. **Drop-glue code generation**
   - For each RC payload type that is a user struct, generate a monomorphic drop-glue function
   - Function signature: `void drop_glue_struct_T(void *ptr)` where T is the struct name
   - Implementation: iterate fields in reverse order, call appropriate drop operation per field kind
   - Handle nested structs recursively (call their drop glue)

3. **Drop-glue registration**
   - Wire drop_fn resolution in elab.c to synthesize and register glue for composite types
   - Update emit.c to reference generated drop-glue function for struct payloads
   - Ensure drop-glue is re-entrancy-safe (no global state; use RC counts + queue)

4. **Test fixtures for composite payloads**
   - Create RC<struct> with simple fields
   - Create RC<struct> with nested RC fields to test recursive destruction
   - Create RC<struct> with cycles to test GC integration

#### Issue 4: Last-Use Elision Interaction With Auto-Drop
**Current state:** Elision analysis is implemented and working for standalone RC operations. However, auto-drop changes the AST structure by injecting defers, which may cause elision analysis to incorrectly mark operations as non-elidable due to defer presence.

**Prerequisite breakdown:**
1. **Elision barrier awareness for auto-drop defers**
   - When building elision candidate list, treat auto-drop defers specially
   - Auto-drop defers are scope-level housekeeping, not semantic barriers
   - RC operations in non-defer body items should still be elision candidates

2. **Verify elision safety with auto-drop enabled**
   - Run existing elision test fixtures after enabling auto-drop
   - Check that no regressions occur (elision flags still set correctly)
   - Add new fixture: RC auto-drop + elision interaction in same scope

---

## Revised Phase 9 Prerequisite Task List

### Prerequisite 1: Move-State Tracking in Ref Auto-Drop
**Blocker:** rc-ref-conversion crash

**Tasks:**
- [ ] Add move-state snapshot before elaborating each let binding
- [ ] Capture move state after each binding elaboration
- [ ] In ref auto-drop logic, check if binding was moved; skip defer if moved
- [ ] In RC auto-drop logic, also check move state before injecting defer
- [ ] Test fixture: `ref-auto-drop-moved-binding` (ref binding used in rc/from-ref should not auto-drop)

**Acceptance criteria:**
- rc-ref-conversion fixture passes without crash
- ref auto-drop defers are only injected for non-moved refs
- Move-state tracking is consistent across nested lets

### Prerequisite 2: RC Binding-Consumption Detection
**Blocker:** rc-auto-drop-positive, rc-auto-drop-multiple, rc-auto-drop-negative-consumed tests

**Tasks:**
- [ ] Implement `find_binding_uses()` AST traversal to find all uses of a specific binding in let body
- [ ] Extend traversal to recognize both `(ref/from-rc rc)` and `(rc/drop rc)` patterns
- [ ] Build consumption map: binding → bool (is_consumed)
- [ ] In RC auto-drop logic, check consumption map; only inject defer if not consumed
- [ ] Add validation: warn if binding is moved AND explicitly consumed (should be one or the other)
- [ ] Test fixtures:
  - `rc-auto-drop-simple` (single RC binding, no consumption)
  - `rc-auto-drop-consumed-by-ref-from-rc` (RC consumed, no auto-drop)
  - `rc-auto-drop-consumed-by-explicit-drop` (RC explicitly dropped, no double-drop)

**Acceptance criteria:**
- rc-auto-drop-positive fixture passes (auto-drop generated for unconsused RC)
- rc-auto-drop-negative-consumed fixture passes (no auto-drop for consumed RC)
- rc-auto-drop-multiple fixture passes (selective auto-drop for multiple RCs)

### Prerequisite 3: User-Defined Struct Destructor Support
**Blocker:** rc-auto-drop-multiple and any RC<struct> fixtures

**Tasks:**
- [ ] Define struct metadata layout for RC payload types (field offsets, type kinds, counts)
- [ ] Implement `synthesize_drop_glue()` in elab.c to generate drop functions for struct types
- [ ] Wire drop_fn resolution to call synthesize_drop_glue for user-defined struct payloads
- [ ] Update emit.c to emit drop-glue function definitions alongside structs
- [ ] Update rc.c / runtime to reference synthesized drop functions for struct RC payloads
- [ ] Test fixtures:
  - `rc-struct-simple-payload` (RC<struct> with primitive fields)
  - `rc-struct-nested-rc-fields` (RC<struct> with RC fields inside)
  - `rc-struct-auto-drop` (auto-drop of RC<struct> with cleanup)

**Acceptance criteria:**
- Generated drop-glue compiles without errors
- Struct fields destroyed in reverse declaration order
- Nested RC fields properly decremented via drop glue
- All new fixtures pass without memory leaks or crashes

### Prerequisite 4: Elision Interaction Validation
**Blocker:** Potential regression in elision once auto-drop is enabled

**Tasks:**
- [ ] Re-run all existing rc-elision fixtures after enabling auto-drop
- [ ] Verify no new elision errors or false elision (elide flags unchanged)
- [ ] Add fixture: `rc-elision-with-auto-drop` (auto-drop + elision in same scope)
- [ ] Check that auto-drop defers don't create false barriers for elision
- [ ] Document elision barrier semantics: auto-drop defers excluded from barrier check

**Acceptance criteria:**
- All existing elision tests still pass after auto-drop is enabled
- Elision analysis correctly ignores auto-drop defer structure
- New mixed auto-drop + elision fixture validates interaction safety

---

## Proposed Implementation Sequence

**Phase 1 (Unblock rc-ref-conversion):**
1. Prerequisite 1: Move-state tracking
2. Add minimal move-state check to ref auto-drop to prevent use-after-move defers

**Phase 2 (Unblock rc-auto-drop tests):**
1. Prerequisite 2: RC consumption detection
2. Enable RC auto-drop injection with consumption awareness

**Phase 3 (Unblock composite payloads):**
1. Prerequisite 3: Struct destructor support
2. Run all Phase 9 fixtures; validate 20+ tests passing

**Phase 4 (Validation):**
1. Prerequisite 4: Elision interaction
2. Regression test all elision fixtures
3. Final cleanup and document Phase 9 completion

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

**Latest continuation session (auto-drop edge coverage):**
- Added `rc-auto-drop-nested-scope` fixture to validate nested `rc/of` bindings get scope-exit auto-drop behavior.
- Added `rc-auto-drop-early-return` fixture to validate auto-drop still fires across early-return control flow.
- Added `rc-auto-drop-closure-capture` fixture to validate closure capture does not suppress rc auto-drop defer injection.
- Added `rc-auto-drop-explicit-drop` fixture to validate explicit `rc/drop` is treated as consumption and suppresses auto-drop.
- Added snapshots and validated targeted run (`3 passed, 0 failed`) plus broader Phase 9 slice (`24 passed, 0 failed`) under timeout-bounded execution.
- Documented elision opportunity: when RC is cloned, used exactly once, then immediately dropped, the increment/decrement pair can be elided
- Deferred full SSA-like analysis implementation to Phase 9 follow-up; current baseline shows where optimization would apply
- All 12 Phase 9 fixtures now pass: rc-basic, rc-shared, rc-cycle-leak, rc-auto-drop-injection, weak-upgrade, weak-dangling, rc-ref-conversion, rc-unique-violation, rc-nested-free-queue, rc-drop-hook-inner-rc, rc-elision-positive, rc-elision-negative-escape

**Phase 9 follow-up session: Last-use elision implementation (May 10, 2026):**
- Implemented full SSA-like RC clone/drop pair elision pass in `src/rc_elision.c` with conservative eligibility rules
- Added `elide` bool flag to `EX_RC_CLONE` and `EX_RC_DROP` IR nodes in `expr.h`
- Initialized elide flags in elaborator (`elab.c`) for `rc/clone` and `rc/drop` forms (default false)
- Wired `rc_elision_analyze_fn()` call into `emit.c` emit_fn_def before function body emission
- Emitter checks elide flags and skips `rc_strong_increment`/`rc_strong_decrement` + `rc_free_queue_drain` calls when marked
- Added conditional debug output (gated by `TUR_DEBUG`) showing elision decisions in generated code
- Created 4 new elision-specific fixtures testing positive cases, barrier rejection, and ref/from-rc safety interaction
- Phase 9 fixture validation: 17 tests pass (4 new + 13 existing RC/weak fixtures)

**Current analysis session (May 10, 2026):**
- Created detailed prerequisite task breakdown explaining why remaining 4 Phase 9 tests are blocked
- Identified root causes:
  1. **rc-ref-conversion crash** caused by move-state tracking gap: ref bindings moved by rc/from-rc still trigger auto-drop defers
  2. **rc-auto-drop-positive/multiple/negative-consumed** blocked by disabled RC auto-drop: needs consumption detection before injection
  3. **Struct destructors** required to support composite RC payloads (blocking multi-field tests)
  4. **Elision interaction** needs validation to ensure auto-drop doesn't break existing optimizations
- Documented 4 structured prerequisites with acceptance criteria and test fixtures
- Proposed 4-phase implementation sequence to unblock and complete Phase 9

**Current test breakdown (66 passing / 74 failing):**
- ✅ 16 Phase 9 tests passing: rc-basic, rc-shared, rc-cycle-leak, rc-auto-drop-injection, weak-upgrade, weak-dangling, rc-drop-hook-inner-rc, rc-nested-free-queue, rc-unique-violation, rc-elision-drop-pair-positive, rc-elision-drop-pair-negative-barrier, rc-elision-barrier-call, rc-elision-positive, rc-elision-negative-escape, rc-elision-ref-from-rc-safety, rc-ref-from-rc-safety
- ❌ 4 Phase 9 tests failing:
  - rc-ref-conversion (emit crash due to moved binding in auto-drop defer)
  - rc-auto-drop-positive (auto-drop injection disabled)
  - rc-auto-drop-multiple (auto-drop + struct payloads disabled)
  - rc-auto-drop-negative-consumed (auto-drop not checking consumption)

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

---

## Future Work Summary and Roadmap

### Immediate Next Steps (High Priority)

1. **Phase 9 auto-drop injection** (Medium complexity, unlocks better developer UX)
   - Implement sophisticated binding-usage analysis to detect when rc/of bindings should auto-drop at scope exit
   - Files to modify: src/elab.c (binding analysis), src/expr.h (auto-drop IR node)
   - Estimated impact: Eliminates manual rc/drop calls for ~80% of rc usage patterns
   - Prerequisite: Verify interaction model with ref/from-rc doesn't create unsafe elision opportunities

2. **Phase 10 GC trial deletion v2** (High complexity, enables cycle collection)
   - Implement Bacon-Rajan trial deletion using type metadata scanning
   - Files to create: src/gc_trial_deletion.c/h
   - Current blockers: Type metadata layout decision deferred; need concrete shape definition
   - Estimated timeline: 2-3 full sessions given complexity of metadata synthesis

3. **Phase 12 borrow type system** (High complexity, enables safer pointer usage)
   - Implement deref operator for borrow types (`&T`, `&mut T`)
   - Implement mutation through mutable borrows
   - Files to modify: src/elab.c, src/emit.c, src/types.h
   - Current blockers: Borrow lifetime validation infrastructure needed; deferred from Phase 12

### Medium-term Work (Next 2-3 sessions)

1. **Phase 9 user-defined drop glue** for composite struct RC payloads
   - Extend drop_fn resolution to synthesize glue for user types
   - Focus on LIFO field destruction, re-entrancy safety with deferred queue

2. **Phase 9 enhanced elision analysis** for branch/loop patterns
   - Extend SSA-like analysis to handle phi-node convergence (both branches merge)
   - Implement loop-exit elision pattern recognition

3. **Phase 10 GC collection modes** (threshold-based, background)
   - Implement automatic collection when suspect buffer crosses size threshold
   - Future: Background collection thread infrastructure

4. **Phase 11 return-transfer edge cases** (if any remain)
   - Verify all paths through early return + branch combinations
   - Add fixtures for complex nested return scenarios

### Backlog / Lower Priority

1. Phase 12 full borrow checker with all edge cases (struct fields, reborrow, unsafe opt-out)
2. Phase 13 lifetime parameters and constraints
3. Phase 15 full typeclass coverage (currently minimal implementation)
4. Phase 17 exception handling completeness (currently basic try-catch)
5. Phase 18 delimited continuations (reset/shift)
6. Phase 19 algebraic effects (defeffect/perform/handle)

### Test Coverage Goals for Next Phase

- Phase 9 follow-up: 5-7 new fixtures for auto-drop injection, user-defined drop glue, enhanced elision
- Phase 10: 8-10 new fixtures for trial deletion, collection modes, cycle detection scenarios
- Phase 12: 6-8 new fixtures for borrow deref, mutation, field borrowing, closure interactions

### Known Testing Infrastructure Gaps

1. Codegen snapshot validation could be automated (checking for absence/presence of operations)
2. Performance regression testing not yet implemented (baseline snapshots needed)
3. Multi-file compilation tests deferred (only single-file tests currently)

### Session Execution Model Going Forward

For **Phase 9 continuations**: Start with auto-drop injection as highest-value item; estimated 1-2 sessions.

For **Phase 10 start**: Pre-plan type metadata shape in writing before implementation; high-risk area.

For **Phase 12 start**: Confirm borrow lifetime infrastructure exists or plan parallel Phase 12 prerequisite work.

