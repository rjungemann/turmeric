# Turmeric Guides

User-facing documentation for Turmeric features, tutorials, and best practices.

## Concurrency and Async

- **[threading-guide.md](threading-guide.md)** — OS threads, `Arc<T>`, `Mutex<T>`, `Atomic<T>`, channels
- **[async-await-guide.md](async-await-guide.md)** — Async/await with fibers and delimited continuations
- **[stm-guide.md](stm-guide.md)** — Software transactional memory — API reference and mechanics
- **[stm-tutorial.md](stm-tutorial.md)** — STM tutorial: concepts, patterns, and worked examples

## Advanced Control Flow

- **[effects-system-guide.md](effects-system-guide.md)** — Algebraic effects, dependency injection, custom control flow
- **[logic-programming-guide.md](logic-programming-guide.md)** — Backtracking, logic programming, constraint solving with cloneable continuations
- **[checkpointing-guide.md](checkpointing-guide.md)** — Cloneable continuations for persistent workflows and checkpointing
- **[serializable-continuations-guide.md](serializable-continuations-guide.md)** — Serializable continuations for persistent workflows and cross-process computation

## Data Structures

- **[hamt-guide.md](hamt-guide.md)** — Persistent hash maps with structural sharing (HAMT)

## Language Features

- **[hkt-guide.md](hkt-guide.md)** — Higher-kinded types (functor, monad, applicative abstractions, performance/dispatch model)
- **[module-system-guide.md](module-system-guide.md)** — Module system, namespacing, exports
- **[c-integration-guide.md](c-integration-guide.md)** — Foreign function interface (FFI) and C interop

## Error Handling

- **[error-handling-guide.md](error-handling-guide.md)** — Try/catch, exception types
- **[../design/error-handling-rationale.md](../design/error-handling-rationale.md)** — Design rationale: exceptions vs. panic

## Tutorials and Examples

- **[minikanren-tutorial.md](minikanren-tutorial.md)** — Logic programming with miniKanren
- **[cellular-automata-comonad-tutorial.md](cellular-automata-comonad-tutorial.md)** — Cellular automata with comonads
- **[custom-effects-tutorial.md](custom-effects-tutorial.md)** — Writing custom effects
- **[snake-game-tutorial.md](snake-game-tutorial.md)** — Building the snake game example

## Reference

- **[test-runner-contract.md](test-runner-contract.md)** — Test framework API and contract

---

## Finding Guides

**By topic:**
- Concurrency → [threading-guide.md](threading-guide.md), [async-await-guide.md](async-await-guide.md), [stm-guide.md](stm-guide.md), [stm-tutorial.md](stm-tutorial.md)
- Data structures → [hamt-guide.md](hamt-guide.md)
- Control flow → [effects-system-guide.md](effects-system-guide.md), [logic-programming-guide.md](logic-programming-guide.md), [serializable-continuations-guide.md](serializable-continuations-guide.md)
- Type system → [hkt-guide.md](hkt-guide.md), [module-system-guide.md](module-system-guide.md)
- Error handling → [error-handling-guide.md](error-handling-guide.md)

**By level:**
- Beginner → Tutorials, guides on core features
- Intermediate → Advanced control flow, type system
- Advanced → Effects, logic programming, serializable continuations, checkpointing

---

## Planning and Design

For design documents, architecture, and phase planning, see:
- **[../](../README.md)** — Main docs folder
- **[../archive/](../archive/README.md)** — Active planning documents
- **[../archive/history/](../archive/history/README.md)** — Historical completed work
