# Archive

This folder contains planning and design documents for Turmeric features under active development or consideration.

## Active Planning Documents

These are current roadmaps for features in development or planned phases. For user-facing guides and tutorials, see [../guides/](../guides/).

### Phase 19+ (Active Development)

- **[deferred-tasks-T19-T21.md](deferred-tasks-T19-T21.md)** — Current phase tracking for T19 (threads), T20 (schedulers), T21 (async/await)
- **[thread-safety-and-primitives-plan.md](thread-safety-and-primitives-plan.md)** — Thread API design; see [../guides/threading-guide.md](../guides/threading-guide.md)
- **[fiber-asm-ctx-plan.md](fiber-asm-ctx-plan.md)** — Fiber context-switching fallback strategy for macOS

### Phase 20+ (Planned)

- **[async-await-plan.md](async-await-plan.md)** — Async/await design; see [../guides/async-await-guide.md](../guides/async-await-guide.md)
- **[effects-plan.md](effects-plan.md)** — Algebraic effects design; see [../guides/effects-system-guide.md](../guides/effects-system-guide.md)
- **[stm-plan-2.md](stm-plan-2.md)** — STM v1+v2 roadmap; see [../guides/stm-tutorial.md](../guides/stm-tutorial.md)

### Phase 21+ (Stretch Goals)

- **[hamt-plan.md](hamt-plan.md)** — Persistent hash array mapped trie for v2
- **[backtracking-cloneable-continuations-plan.md](backtracking-cloneable-continuations-plan.md)** — Cloneable continuations for logic programming; see [../guides/logic-programming-guide.md](../guides/logic-programming-guide.md)
- **[serializable-continuations-plan.md](serializable-continuations-plan.md)** — Checkpointing and persistent workflows; see [../guides/checkpointing-guide.md](../guides/checkpointing-guide.md)

### Design Explorations (Not Yet Scoped)

- **[gadts-plan.md](gadts-plan.md)** — Generalized algebraic data types
- **[numeric-types-plan.md](numeric-types-plan.md)** — Numeric type system (fixed-point, rationals, etc.)
- **[module-system-plan.md](module-system-plan.md)** — Module system design
- **[module-system-racket-alt-plan.md](module-system-racket-alt-plan.md)** — Alternative module system (Racket-style)
- **[signal-processing-arrows-plan.md](signal-processing-arrows-plan.md)** — Signal processing with arrows and HKTs
- **[unsafe-operations-plan.md](unsafe-operations-plan.md)** — Unsafe code features (`unsafe` blocks, FFI)
- **[copy-borrow-move-lifetimes.md](copy-borrow-move-lifetimes.md)** — Ownership and lifetime semantics
- **[effects-vs-monads.md](effects-vs-monads.md)** — Comparison of effects and monad-based approaches
- **[hkt-deferred-tasks.md](hkt-deferred-tasks.md)** — Higher-kinded types implementation tracking
- **[performance-improvement-plan.md](performance-improvement-plan.md)** — Compiler optimization roadmap
- **[test-perf-plan.md](test-perf-plan.md)** — Test suite and performance testing strategy

## Extracted Guides

The following documents have been extracted into user-facing guides in [../guides/](../guides/):

| Archive File | Guide File | Purpose |
|---|---|---|
| async-await-plan.md | [../guides/async-await-guide.md](../guides/async-await-guide.md) | Async/await user guide |
| thread-safety-and-primitives-plan.md | [../guides/threading-guide.md](../guides/threading-guide.md) | Threading and concurrency |
| effects-plan.md | [../guides/effects-system-guide.md](../guides/effects-system-guide.md) | Effects system reference |
| backtracking-cloneable-continuations-plan.md | [../guides/logic-programming-guide.md](../guides/logic-programming-guide.md) | Backtracking and logic programming |
| serializable-continuations-plan.md | [../guides/checkpointing-guide.md](../guides/checkpointing-guide.md) | Checkpointing workflows |
| panic-system-vs-exception-system-plan.md | [../design/error-handling-rationale.md](../design/error-handling-rationale.md) | Error handling design rationale |
| stm-plan-2.md | [../guides/stm-tutorial.md](../guides/stm-tutorial.md) | STM (software transactional memory) |

## Historical Documents

Completed work and superseded planning are archived in [history/](history/). See that folder for:

- Phase 7–11 deferred tasks (completed phases)
- CMake migration plan (completed)
- Prerequisite decisions (resolved)
- HKT feasibility analysis (shipped in Phase H3)

## Status Legend

- **Active**: Currently being worked on or next in queue
- **Planned**: Scheduled for a future phase
- **Stretch**: Desirable but not yet scheduled
- **Historical**: Completed or superseded (see [history/](history/))
