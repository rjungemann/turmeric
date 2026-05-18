# Turmeric Guides

User-facing documentation for Turmeric features, tutorials, and best practices.

## Getting Started

- **[quickstart.md](quickstart.md)** — Prose introduction: expressions, functions, control flow, Option, Result, collections, closures, structs, and algebraic effects
- **[repl-tutorial.md](repl-tutorial.md)** — 22-step interactive tutorial to follow at `tur repl` or the web REPL

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
- **[web-continuations-guide.md](web-continuations-guide.md)** — Compact reference: `send-form-and-wait`, continuation store, routing model

## Data Structures

- **[hamt-guide.md](hamt-guide.md)** — Persistent hash maps with structural sharing (HAMT)

## Language Features

- **[hkt-guide.md](hkt-guide.md)** — Higher-kinded types (functor, monad, applicative abstractions, performance/dispatch model)
- **[hrt-guide.md](hrt-guide.md)** — Higher-ranked types: rank-2/3 polymorphic function parameters
- **[module-system-guide.md](module-system-guide.md)** — Module system, namespacing, exports
- **[c-integration-guide.md](c-integration-guide.md)** — Foreign function interface (FFI) and C interop
- **[effects-vs-monads.md](effects-vs-monads.md)** — Design rationale: why effects instead of Haskell-style monads
- **[type-annotations-guide.md](type-annotations-guide.md)** — Compound type annotation syntax: `(-> a b)`, `(vec T)`, `forall`, and more
- **[gadts-guide.md](gadts-guide.md)** — GADTs: `defgadt`, type refinement, equality witnesses, union types, gradual typing
- **[gadts-cookbook.md](gadts-cookbook.md)** — GADTs cookbook: practical patterns and recipes
- **[union-intersection-types-guide.md](union-intersection-types-guide.md)** — Union (`A | B`) and intersection (`A & B`) types, `any`, gradual typing

## Type Safety

- **[substructural-types-guide.md](substructural-types-guide.md)** — `^linear`, `^affine`, `^relevant` type disciplines
- **[uniqueness-types-guide.md](uniqueness-types-guide.md)** — `^unique`: at-most-one-reference ownership

## Error Handling

- **[error-handling-guide.md](error-handling-guide.md)** — `Result`, `Option`, `panic`, contract macros (`assert!`, `require!`, `ensure!`)
- **[contract-types-guide.md](contract-types-guide.md)** — Contract types: `{ x : T | p }`, `:pre`/`:post` annotations, FFI contracts (planned v4)
- **[../design/error-handling-rationale.md](../design/error-handling-rationale.md)** — Design rationale: exceptions vs. panic

## Tutorials and Examples

- **[minikanren-tutorial.md](minikanren-tutorial.md)** — Logic programming with miniKanren
- **[cellular-automata-comonad-tutorial.md](cellular-automata-comonad-tutorial.md)** — Cellular automata with comonads
- **[custom-effects-tutorial.md](custom-effects-tutorial.md)** — Writing custom effects
- **[snake-game-tutorial.md](snake-game-tutorial.md)** — Building the snake game example
- **[web-continuations-tutorial.md](web-continuations-tutorial.md)** — Multi-page web forms using serializable continuations (guestbook example)

## Package Management

- **[package-management-guide.md](package-management-guide.md)** — Creating projects, adding spices, `build.tur`, `tur.lock`, CLI reference

## Tools and IDE

- **[formatter-guide.md](formatter-guide.md)** — `tur format` CLI and web REPL Format button
- **[vscode-guide.md](vscode-guide.md)** — VS Code extension installation and configuration
- **[vim-guide.md](vim-guide.md)** — Vim / Neovim syntax highlighting installation and configuration

## Reference

- **[test-runner-contract.md](test-runner-contract.md)** — Test framework API and contract

---

## Finding Guides

**By topic:**
- Concurrency → [threading-guide.md](threading-guide.md), [async-await-guide.md](async-await-guide.md), [stm-guide.md](stm-guide.md), [stm-tutorial.md](stm-tutorial.md)
- Data structures → [hamt-guide.md](hamt-guide.md)
- Control flow → [effects-system-guide.md](effects-system-guide.md), [logic-programming-guide.md](logic-programming-guide.md), [serializable-continuations-guide.md](serializable-continuations-guide.md), [web-continuations-guide.md](web-continuations-guide.md)
- Type system → [hkt-guide.md](hkt-guide.md), [hrt-guide.md](hrt-guide.md), [module-system-guide.md](module-system-guide.md), [type-annotations-guide.md](type-annotations-guide.md), [union-intersection-types-guide.md](union-intersection-types-guide.md)
- Type safety → [substructural-types-guide.md](substructural-types-guide.md), [uniqueness-types-guide.md](uniqueness-types-guide.md)
- Effects design → [effects-vs-monads.md](effects-vs-monads.md)
- Package management → [package-management-guide.md](package-management-guide.md)
- Tools → [formatter-guide.md](formatter-guide.md), [vscode-guide.md](vscode-guide.md), [vim-guide.md](vim-guide.md)
- Error handling → [error-handling-guide.md](error-handling-guide.md), [contract-types-guide.md](contract-types-guide.md)

**By level:**
- Beginner → [quickstart.md](quickstart.md), [repl-tutorial.md](repl-tutorial.md), then core feature guides
- Intermediate → Advanced control flow, type system
- Advanced → Effects, logic programming, serializable continuations, checkpointing

---

## Planning and Design

For design documents, architecture, and phase planning, see:
- **[../](../README.md)** — Main docs folder
- **[../archive/](../archive/README.md)** — Active planning documents
- **[../archive/history/](../archive/history/README.md)** — Historical completed work
