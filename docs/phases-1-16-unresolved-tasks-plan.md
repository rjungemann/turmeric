# Plan for Resolving Unresolved Tasks in Phases 1-16

> **Status:** Draft plan for completing deferred work across Phases 1-16
> **Last Updated:** 2026-05-10
> **Owner:** Compilation team

## Overview

This document outlines a prioritized plan for resolving all unresolved/deferred tasks from Phases 1 through 16 of the Turmeric compiler implementation. Despite these phases being marked as "complete" at a high level, numerous individual tasks were deferred during implementation. This plan organizes them by priority and dependency, providing a roadmap for full completion.

**Note:** Phases 0-14 are documented in `turmeric-plan.archive.md`. Phases 15-16 are in `turmeric-plan.md`.

---

## Priority Legend

- **P0 (Critical):** Blocks other work, correctness issues, or missing core functionality
- **P1 (High):** Important features needed for practical use
- **P2 (Medium):** Nice-to-have improvements and completeness
- **P3 (Low):** Future-proofing, documentation, or stretch goals

---

## Phase-by-Phase Breakdown

### Phase 0 — C Skeleton + hello.tur Round-Trip

| Task | Description | Priority | Dependencies | Effort | Notes |
|------|-------------|----------|--------------|--------|-------|
| CI-001 | CI workflow: `make debug && make test` on push | P1 | None | 1 day | Currently local-only; needs remote setup |
| AR-001 | Arena self-test: 1M allocations under ASan | P2 | None | 0.5 day | Fixture suite indirectly tests; dedicated bench deferred |

**Summary:** Phase 0 is essentially complete. CI automation and performance validation are the only gaps.

---

### Phase 1 — Full Reader, Core Forms, Arithmetic → Fizzbuzz

| Task | Description | Priority | Dependencies | Effort | Notes |
|------|-------------|----------|--------------|--------|-------|
| RD-001 | Float literals (`1.0`, `1.5e3`) + float type in dispatch table | P1 | None | 1 day | Ship together with float fixtures |
| RD-002 | Maps `#{…}` literal syntax | P2 | Phase 7 (stdlib) | 0.5 day | No phase 1 fixture needs them; reserve `{…}` for SRFI-105 |
| RD-003 | Block comments `#\| ... \|#` | P2 | None | 0.5 day | Trivial reader extension |
| RD-004 | Reader macros: `'` (quote), `` ` `` (quasiquote), `~`, `~@` | P2 | Phase 6 (defmacro) | 1 day | No consumer in phase 1; needed for macro system |
| RD-005 | `#lang` directive parsing | P2 | Sweet-expressions (Phase S1+) | 0.5 day | Ship with sweet-expressions |
| RD-006 | Type annotation parsing (`^int`, `: T`) | P1 | Phase 2 (defn) | 0.5 day | Deferred to phase 2; already needed |
| OP-001 | Division-by-zero runtime check | P1 | None | 0.5 day | Add when fixture demands clean failure |

**Summary:** Phase 1 core is complete. Reader extensions and edge cases remain. Float literals are highest priority.

---

### Phase 2 — Top-Level Functions, extern-c, inline-C, libc

| Task | Description | Priority | Dependencies | Effort | Notes |
|------|-------------|----------|--------------|--------|-------|
| FN-001 | `fn` capture rejection diagnostic | P2 | Phase 3 (closures) | 0.25 day | Already gates to phase 3; clean up message |

**Summary:** Phase 2 is effectively complete. All major features shipped.

---

### Phase 3 — Closures

| Task | Description | Priority | Dependencies | Effort | Notes |
|------|-------------|----------|--------------|--------|-------|
| CL-001 | Early `return` with defer firing | P0 | None | 1 day | Currently `return` bypasses defers; must walk frame chain |
| CL-002 | `defer-early-return.tur` fixture | P1 | CL-001 | 0.25 day | Test that defers fire on early return |

**Summary:** Core closure functionality works. Integration with control flow (early returns) needs completion.

---

### Phase 4 — defer + Scope Unwind

| Task | Description | Priority | Dependencies | Effort | Notes |
|------|-------------|----------|--------------|--------|-------|
| DF-001 | Early `return X`: rewrite to fire defers then return | P0 | CL-001 | 1 day | Walks frames via parent pointer; small loop in emitted code |
| DF-002 | `defer-early-return.tur` fixture | P1 | DF-001 | 0.25 day | Verify defer ordering with early returns |

**Summary:** v0 lowering shipped. v1 unified runtime-list-on-frame model is in place. Early return integration with defers is the key remaining gap.

---

### Phase 5 — ref<T>

| Task | Description | Priority | Dependencies | Effort | Notes |
|------|-------------|----------|--------------|--------|-------|
| RF-001 | `(ref? x)` predicate | P2 | None | 0.5 day | Runtime type check for FFI |
| RF-002 | Ownership transfer on assignment (poison source) | P1 | None | 1 day | Move semantics: accessing moved ref errors |
| RF-003 | Swap pattern with refs | P2 | RF-002 | 0.5 day | `(let [tmp a] (set! a b) (set! b tmp))` must work |
| RF-004 | Return ref from function (ownership transfer) | P1 | RF-002 | 0.5 day | Caller owns returned ref; no implicit clone |
| RF-005 | `ref<T>` where T is `ptr<U>` | P2 | None | 0.5 day | Allowed but unusual; document |
| RF-006 | `ref<T>` where T is struct with destructor | P2 | Phase 11 | 1 day | Reserve `drop_fn` slot in Type |
| RF-007 | Top-level `ref` binding error | P1 | None | 0.25 day | Refs must be scope-local |
| RF-008 | `ref-move.tur` fixture | P1 | RF-002 | 0.25 day | Test ownership transfer |
| RF-009 | `ref-return.tur` fixture | P1 | RF-004 | 0.25 day | Test return ownership transfer |
| RF-010 | `ref-in-closure.tur` fixture | P1 | Phase 3 | 0.25 day | Closure captures ref, defers fire correctly |
| RF-011 | `ref-top-level.tur` negative fixture | P1 | RF-007 | 0.25 day | Error on top-level ref binding |
| RF-012 | `ref-use-after-move.tur` negative fixture | P1 | RF-002 | 0.25 day | Diagnostic on use of poisoned binding |
| RF-013 | Codegen snapshots for ref struct layout | P2 | None | 0.5 day | Verify drop injection |

**Summary:** Basic ref functionality works. Full move semantics and ownership tracking are incomplete. This is critical for memory safety.

---

### Phase 6 — defmacro + quasiquote

| Task | Description | Priority | Dependencies | Effort | Notes |
|------|-------------|----------|--------------|--------|-------|
| MC-001 | `cond` as macro (not special form) | P2 | List operations | 1 day | Currently special form; macro needs first/second/slice/len |
| MC-002 | `case` macro | P2 | MC-001 | 1 day | Dispatch on value with `=` comparisons |
| MC-003 | `deftest` macro | P2 | Test runner | 1 day | Deferred until test infrastructure complete |
| MC-004 | `assert-true`, `assert-false`, `assert-nil` | P2 | MC-003 | 0.5 day | Bool type limitations |
| MC-005 | `assert-error` (checks body raises error) | P2 | Phase 17 (exceptions) | 1 day | Requires error handling infrastructure |
| MC-006 | `run-test`, `run-tests!` | P2 | MC-003 | 1 day | Test runner macros |
| MC-007 | `tur test` subcommand | P2 | MC-006 | 1 day | Builds and runs test files |
| MC-008 | Reader macros implementation | P1 | RD-004 | 1 day | `'`, `` ` ``, `~`, `~@` reader support |

**Summary:** Core macro system works. Test infrastructure and reader macros are the main gaps.

---

### Phase 7 — Stdlib Seed

| Task | Description | Priority | Dependencies | Effort | Notes |
|------|-------------|----------|--------------|--------|-------|
| ST-001 | Full runtime functionality for stdlib types | P1 | Phase 11 | 2 days | Inline C with malloc/free causes type mismatches |
| ST-002 | `assert-*` functions | P2 | MC-001 | 1 day | Depends on bool type support |
| ST-003 | Full functional tests for stdlib types | P1 | ST-001 | 1 day | Deferred until type annotations |
| ST-004 | Bounds-check failures on slice/vec access | P2 | ST-003 | 0.5 day | Negative fixtures |
| ST-005 | Codegen snapshots for stdlib types | P2 | ST-001 | 0.5 day | Verify emitted C |

**Summary:** Stdlib types defined but not fully functional due to compilation issues.

---

### Phase 8 — Diagnostics Polish

| Task | Description | Priority | Dependencies | Effort | Notes |
|------|-------------|----------|--------------|--------|-------|
| DI-001 | Linter: error if IR node has SPAN_UNKNOWN | P2 | Snapshot infrastructure | 0.5 day | CI check for span completeness |
| DI-002 | Operator lookup failure: show operator, arg types, available overloads | P2 | Phase 15 (typeclasses) | 0.5 day | Needs operator overloading |
| DI-003 | Suggestions reference documentation URLs | P3 | Docs site | 0.5 day | Deferred until docs available |
| DI-004 | Golden files for error fixtures | P2 | DI-001 | 1 day | Multi-line output support |

**Summary:** Core diagnostics work. Multi-line output and better error messages are gaps.

---

### Phase 9 — rc<T> + weak<T>

| Task | Description | Priority | Dependencies | Effort | Notes |
|------|-------------|----------|--------------|--------|-------|
| RC-001 | Drop function for rc<T> where T is struct with destructor | P2 | Phase 11 | 1 day | Phase 9 supports stdlib; user types deferred |
| RC-002 | Defer injection for rc/drop | P1 | Phase 4 | 1 day | Inject `(defer (rc/drop x))` at let binding |
| RC-003 | Last-use elision optimization | P2 | RC-002 | 2 days | Skip retain/release for single-use rc |
| RC-004 | Deferred free queue for cascade prevention | P2 | RC-002 | 1 day | Prevent stack overflow from long drop chains |
| RC-005 | `(rc/from-ref r)` — convert ref to rc | P2 | Phase 5 | 1 day | Moves value; poisons original ref |
| RC-006 | `(ref/from-rc r)` — convert rc to ref | P2 | RC-005 | 1 day | Requires strong count == 1; poisons other rc pointers |
| RC-007 | `weak-dangling.tur` fixture | P2 | None | 0.25 day | Accessing weak after all strong refs dropped |
| RC-008 | `rc-ref-conversion.tur` fixture | P2 | RC-005, RC-006 | 0.25 day | Test conversion functions |
| RC-009 | `rc-unique-violation.tur` negative fixture | P2 | RC-006 | 0.25 day | `ref/from-rc` with count > 1 errors |

**Summary:** Basic RC works but lacks drop injection and conversion functions.

---

### Phase 10 — Bacon-Rajan Cycle Collector

| Task | Description | Priority | Dependencies | Effort | Notes |
|------|-------------|----------|--------------|--------|-------|
| GC-001 | Trial deletion: scan reachable from suspect roots | P2 | Type metadata | 3 days | Deferred - needs type metadata for scanning |
| GC-002 | Suspect identification | P2 | GC-001 | 2 days | Needs type metadata |
| GC-003 | `rc_upgrade` confirm object alive | P2 | GC-001 | 1 day | Needs gc_is_alive integration |
| GC-004 | Threshold mode (auto-collect when buffer exceeds N) | P2 | GC-001 | 1 day | Needs full trial deletion |
| GC-005 | Background mode (separate thread) | P3 | GC-004, Threads | 3 days | Deferred - requires thread support |
| GC-006 | Collect only RC'd objects, not entire heap | P2 | GC-001 | 1 day | Needs type metadata |
| GC-007 | `gc-cycle-freed.tur` fixture | P2 | GC-001 | 0.5 day | Verify cyclic data collected |
| GC-008 | `gc-no-false-positives.tur` fixture | P2 | GC-001 | 0.5 day | Live cyclic data not collected |
| GC-009 | `gc-perf.tur` fixture | P2 | GC-004 | 1 day | Measure collection overhead |
| GC-010 | GC ABI documentation | P3 | GC-001 | 1 day | Control block layout, color semantics |
| GC-011 | Collector hook for custom implementations | P3 | GC-001 | 1 day | Reserved function pointer |
| GC-012 | Benchmark: Lisp interpreter with cyclic structures | P3 | GC-001 | 2 days | Deferred |

**Summary:** Cycle collector infrastructure in place but core trial deletion algorithm is incomplete. Blocked on type metadata.

---

### Phase 11 — Copy Traits

| Task | Description | Priority | Dependencies | Effort | Notes |
|------|-------------|----------|--------------|--------|-------|
| CP-001 | Move poisoning at return | P1 | Return expression support | 1 day | Mark moved at return |
| CP-002 | Move suppression on return (ownership transfer) | P1 | CP-001 | 0.5 day | Returning moves ownership to caller |
| CP-003 | Reserved `:move` annotation | P2 | None | 0.25 day | Explicit move annotation |
| CP-004 | Error on invalid `:copy` for structs with non-Copy fields | P2 | Phase 5 | 0.5 day | e.g., struct with ref<int> cannot be Copy |
| CP-005 | Closure capture of moved bindings error | P2 | CP-001 | 0.5 day | Error at capture analysis time |
| CP-006 | Defer with moved bindings error | P2 | CP-001 | 0.5 day | Defer body referencing moved binding errors |
| CP-007 | Remove phase-5-specific move tracking code | P2 | CP-001 | 0.5 day | Subsume into general copy traits |
| CP-008 | `copy-traits-basic.tur` fixture | P1 | None | 0.25 day | Basic copy/move functionality |
| CP-009 | `copy-use-after-move.tur` negative fixture | P1 | CP-001 | 0.25 day | Use after move error |
| CP-010 | `copy-use-after-move-set.tur` negative fixture | P1 | CP-001 | 0.25 day | Set after move error |
| CP-011 | `copy-traits-struct.tur` fixture | P2 | Phase 5 | 0.25 day | Struct with :copy annotation |
| CP-012 | `copy-traits-struct-noncopy.tur` negative fixture | P2 | CP-004 | 0.25 day | Non-copyable struct error |
| CP-013 | `copy-traits-return.tur` fixture | P2 | CP-002 | 0.25 day | Return ownership transfer |
| CP-014 | `copy-traits-closure.tur` negative fixture | P2 | CP-005 | 0.25 day | Closure capture of moved binding error |
| CP-015 | `copy-traits-defer.tur` negative fixture | P2 | CP-006 | 0.25 day | Defer with moved binding error |
| CP-016 | Codegen snapshots: no runtime overhead | P2 | None | 0.5 day | Static checking only |

**Summary:** Copy traits infrastructure in place but move tracking at returns and integration with closures/defer are incomplete.

---

### Phase 12 — Borrow Traits

| Task | Description | Priority | Dependencies | Effort | Notes |
|------|-------------|----------|--------------|--------|-------|
| BR-001 | Covariance: `&T` subtype of `&U` if `T` subtype of `U` | P2 | Subtyping | 1 day | Deferred |
| BR-002 | `@r` dereference syntax for `&T` and `&mut T` | P2 | None | 0.5 day | Overloaded with ref<T> deref |
| BR-003 | `(set! (@ r) value)` for `&mut T` | P1 | BR-002 | 0.5 day | Mutate through mutable borrow; error for immutable |
| BR-004 | Reader macro `&x` → `(& x)` | P2 | None | 0.25 day | Reader sugar |
| BR-005 | Borrow of ref<T> | P2 | Phase 5 | 0.5 day | Borrow lifetime tied to ref scope |
| BR-006 | Borrow struct field (`&(.field s)`) | P2 | Struct fields | 0.5 day | Field borrow |
| BR-007 | Borrow through pointer | P2 | BR-005 | 0.5 day | Pointer deref borrow |
| BR-008 | `borrow-basic.tur` fixture | P1 | None | 0.25 day | Basic borrow functionality |
| BR-009 | `borrow-conflict.tur` negative fixture | P1 | None | 0.25 day | Conflicting borrow error |
| BR-010 | `borrow-moved.tur` negative fixture | P1 | Phase 5 | 0.25 day | Borrow of moved binding error |

**Summary:** Borrow traits infrastructure exists but dereference syntax and mutation through borrows are incomplete.

---

### Phase 13 — Lifetime Annotations

| Task | Description | Priority | Dependencies | Effort | Notes |
|------|-------------|----------|--------------|--------|-------|
| LT-001 | Explicit lifetime parameters (`'a`) on functions and references | P1 | BR-001 | 2 days | Surface syntax for lifetimes |
| LT-002 | Lifetime elision rules | P1 | LT-001 | 2 days | Cover common cases automatically |
| LT-003 | `lifetime-basic.tur` fixture | P1 | LT-001 | 0.5 day | Explicit lifetime annotations |
| LT-004 | `lifetime-elision.tur` fixture | P1 | LT-002 | 0.5 day | Elided cases work correctly |

**Summary:** Lifetime annotations are the foundation for borrow checking. Currently not implemented.

---

### Phase 14 — Borrow Checker with Lifetimes

| Task | Description | Priority | Dependencies | Effort | Notes |
|------|-------------|----------|--------------|--------|-------|
| BC-001 | Full intra-procedural borrow checking | P1 | LT-001, Phase 12 | 3 days | Prevent dangling refs within function |
| BC-002 | Full inter-procedural borrow checking | P1 | BC-001 | 3 days | Check across function boundaries |
| BC-003 | Integration with ref<T> move tracking | P1 | BC-001, Phase 5 | 1 day | Prevent use-after-move at compile time |
| BC-004 | Integration with &T/&mut T aliasing | P1 | BC-001, Phase 12 | 1 day | Enforce aliasing rules |
| BC-005 | `(unsafe ...)` block | P1 | BC-001 | 1 day | Escape hatch for FFI/perf code |
| BC-006 | `borrow-dangling.tur` negative fixture | P1 | BC-002 | 0.25 day | Dangling reference error |
| BC-007 | `borrow-use-after-move.tur` negative fixture | P1 | BC-003 | 0.25 day | Use after move error |
| BC-008 | `borrow-conflicting.tur` negative fixture | P1 | BC-004 | 0.25 day | Conflicting borrow error |
| BC-009 | `unsafe-block.tur` fixture | P1 | BC-005 | 0.25 day | Unsafe block still compiles |

**Summary:** Borrow checker is critical for memory safety but completely unimplemented. This is a major gap.

---

### Phase 15 — Typeclasses

| Task | Description | Priority | Dependencies | Effort | Notes |
|------|-------------|----------|--------------|--------|-------|
| TC-001 | Reserve syntax for higher-kinded types (error in v1) | P2 | None | 0.25 day | Future-proofing |
| TC-002 | Effect rows for typeclass methods | P3 | Phase 19 | 1 day | Future effects integration |

**Summary:** Typeclasses are essentially complete. Only future-proofing tasks remain.

---

### Phase 16 — Capability Passing (v1 Effects)

| Task | Description | Priority | Dependencies | Effort | Notes |
|------|-------------|----------|--------------|--------|-------|
| CB-001 | Effect-polymorphic capability fields | P2 | Phase 19 | 2 days | Accept functions with effect rows |

**Summary:** Capability passing is essentially complete. Only forward-looking effects integration remains.

---

## Prioritized Roadmap

### Tier 1: Critical Correctness (P0)
**Goal:** Fix correctness issues that could cause undefined behavior

1. **DF-001** — Early return with defer firing (1 day) — Blocks: CL-001
2. **CL-001** — Early `return X` rewrite for defers (1 day) — Depends: DF-001
3. **DF-002** — `defer-early-return.tur` fixture (0.25 day) — Depends: DF-001

**Total: ~2.25 days**

### Tier 2: Memory Safety Foundation (P1)
**Goal:** Complete move semantics and basic borrow checking

1. **RF-002** — Ownership transfer on assignment/poisoning (1 day)
2. **RF-004** — Return ref from function (0.5 day)
3. **RF-007** — Top-level ref binding error (0.25 day)
4. **CP-001** — Move poisoning at return (1 day)
5. **CP-002** — Move suppression on return (0.5 day)
6. **BR-003** — `(set! (@ r) value)` for `&mut T` (0.5 day)
7. **LT-001** — Lifetime parameters syntax (2 days)
8. **LT-002** — Lifetime elision rules (2 days)
9. **BC-001** — Intra-procedural borrow checking (3 days)
10. **BC-002** — Inter-procedural borrow checking (3 days)
11. **BC-003** — Integration with ref move tracking (1 day)
12. **BC-004** — Integration with &T/&mut T (1 day)
13. **BC-005** — `(unsafe ...)` block (1 day)

**Total: ~17.25 days**

### Tier 3: Core Language Completeness (P1)
**Goal:** Complete essential language features

1. **RD-001** — Float literals + float type (1 day)
2. **OP-001** — Division-by-zero runtime check (0.5 day)
3. **RD-006** — Type annotation parsing (0.5 day)
4. **RC-002** — Defer injection for rc/drop (1 day)
5. **MC-008** — Reader macros implementation (1 day)
6. **ST-001** — Full runtime for stdlib types (2 days)
7. **ST-003** — Full functional tests for stdlib (1 day)

**Total: ~7.25 days**

### Tier 4: Improvements and Polish (P2)
**Goal:** Quality of life improvements and completeness

1. **RD-002** — Maps `#{…}` syntax (0.5 day)
2. **RD-003** — Block comments (0.5 day)
3. **RD-004** — Reader macros (already in Tier 3)
4. **RD-005** — `#lang` directive (0.5 day)
5. **RF-001** — `(ref? x)` predicate (0.5 day)
6. **RF-003** — Swap pattern (0.5 day)
7. **RF-005** — `ref<ptr<U>>` (0.5 day)
8. **RF-006** — `ref` with struct destructor (1 day)
9. **RF-008-RF-013** — ref fixtures (1.5 days)
10. **RC-001, RC-003-RC-009** — RC improvements (5 days)
11. **CP-003-CP-016** — Copy traits improvements (5 days)
12. **BR-001-BR-007** — Borrow traits improvements (3 days)
13. **LT-003-LT-004** — Lifetime fixtures (1 day)
14. **BC-006-BC-009** — Borrow checker fixtures (1 day)
15. **MC-001-MC-007** — Macro improvements (4 days)
16. **ST-002, ST-004-ST-005** — Stdlib improvements (2 days)
17. **DI-001-DI-004** — Diagnostics improvements (3 days)

**Total: ~34.5 days**

### Tier 5: Future-Proofing (P2/P3)
**Goal:** Prepare for future phases

1. **CI-001** — CI workflow (1 day)
2. **AR-001** — Arena self-test (0.5 day)
3. **TC-001-TC-002** — Typeclass future-proofing (1.25 days)
4. **CB-001** — Effect-polymorphic capabilities (2 days)
5. **GC-001-GC-012** — Cycle collector (17 days)

**Total: ~21.75 days**

---

## Recommended Execution Order

### Sprint 1: Critical Correctness (Week 1)
- DF-001, CL-001, DF-002
- **Deliverable:** Early returns correctly fire defers

### Sprint 2: Type Annotations + Reader (Week 2)
- RD-001, RD-006, OP-001, MC-008
- **Deliverable:** Float support, type annotations, reader macros

### Sprint 3: Move Semantics Foundation (Week 3-4)
- RF-002, RF-004, RF-007, CP-001, CP-002
- **Deliverable:** Basic move tracking with ownership transfer

### Sprint 4: Borrow Checking MVP (Week 5-7)
- LT-001, LT-002, BR-003, BC-001, BC-002, BC-003, BC-004, BC-005
- **Deliverable:** Basic borrow checking prevents dangling references

### Sprint 5: RC/Ref Integration (Week 8)
- RC-002, ST-001, ST-003
- **Deliverable:** RC drop injection, stdlib types work

### Sprint 6: Polish and Tests (Week 9-10)
- All remaining P2 tasks
- **Deliverable:** Complete fixture coverage, improved diagnostics

---

## Dependencies Graph

```
Phase 3/4 defer+return
    ├── CL-001 (Early return rewrite)
    └── DF-001 (Defer firing on return)
    
Phase 5 ref move semantics
    ├── RF-002 (Ownership transfer)
    ├── RF-004 (Return ownership)
    ├── RF-007 (Top-level error)
    └── CP-001/CP-002 (Move at return)
    
Phase 12-14 borrow checking
    ├── LT-001/002 (Lifetime syntax + elision)
    ├── BR-003 (Dereference + mutation)
    ├── BC-001/002 (Intra/inter-procedural)
    └── BC-003/004 (Integration with move/borrow)
    
Reader features
    ├── RD-001 (Float literals)
    ├── RD-006 (Type annotations)
    └── MC-008 (Reader macros)
    
RC features
    └── RC-002 (Defer injection)
    
Stdlib
    ├── ST-001 (Runtime functionality)
    └── ST-003 (Functional tests)
```

---

## Success Criteria

### Minimum Viable Completion (MVP)
- All P0 tasks complete
- All P1 tasks complete
- All existing fixtures still pass
- No regressions in ASan/UBSan

### Full Completion
- All P0, P1, P2 tasks complete
- All fixtures pass
- Full test coverage for all features

---

## Tracking

Use the following checklist format for tracking progress:

```markdown
## Progress

### P0 Critical
- [ ] DF-001: Early return with defer firing
- [ ] CL-001: Early return X rewrite
- [ ] DF-002: defer-early-return.tur fixture

### P1 Memory Safety
- [ ] RF-002: Ownership transfer on assignment
- [ ] RF-004: Return ref from function
- [ ] RF-007: Top-level ref binding error
- [ ] CP-001: Move poisoning at return
- [ ] CP-002: Move suppression on return
- [ ] BR-003: set! through &mut T
- [ ] LT-001: Lifetime parameters
- [ ] LT-002: Lifetime elision
- [ ] BC-001: Intra-procedural borrow checking
- [ ] BC-002: Inter-procedural borrow checking
- [ ] BC-003: Integration with ref move tracking
- [ ] BC-004: Integration with borrow traits
- [ ] BC-005: unsafe block

### P1 Core Completeness
- [ ] RD-001: Float literals
- [ ] OP-001: Division-by-zero check
- [ ] RD-006: Type annotation parsing
- [ ] RC-002: Defer injection for rc/drop
- [ ] MC-008: Reader macros
- [ ] ST-001: Stdlib runtime
- [ ] ST-003: Stdlib tests
```

---

## Notes

1. **Phases 15-16 are largely complete** — only future-proofing tasks remain (higher-kinded types, effect rows). These are lower priority.

2. **Phase 4's unified defer model is in place** — the v1 infrastructure from effects-plan.md §6.10 is implemented. The main gap is early return integration.

3. **Phase 10-14 (borrow checker chain) is the biggest gap** — Lifetime annotations and borrow checking are essentially unimplemented. This is critical for memory safety guarantees.

4. **Phase 5-9 (ownership/model) has partial implementation** — ref<T> works, rc<T> exists, but move semantics and integration are incomplete.

5. **Test infrastructure gaps** — Many test-related macros and fixtures are deferred. Consider prioritizing these to improve developer experience.
