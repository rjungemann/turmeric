---
title: Guides Index
category: Getting Started
description: Index and overview of Turmeric user-facing documentation, guides, and tutorials
---

# Turmeric Guides

User-facing documentation for Turmeric features, tutorials, and best practices.

Code examples are available in both standard S-expression syntax (`turmeric`) and
sweet-expression syntax (`sweet-exp`). Use the toggle above each paired example to
switch between them.

## Authoring paired examples

Write two consecutive fenced blocks -- a `turmeric` block immediately followed (no
prose between) by a `sweet-exp` block. `genguides.py` detects the pair and renders
a toggle widget automatically. A lone `turmeric` block with no `sweet-exp` sibling
renders as a plain code block; all existing blocks remain valid.

The second (sweet-exp) variant always opens with `#lang sweet-exp` when the snippet
is a complete runnable program; inline snippets omit the directive. Do not use the
invalid `#lang turmeric/sweet-exp` or mix `#lang turmeric/neoteric` into sweet-exp
labelled examples -- the correct directive is `#lang sweet-exp` in every case.

Example:

````markdown
```turmeric
(defn use-ask [] :int
  (+ 1 (perform (Ask))))
```
```sweet-exp
defn use-ask [] :int
  {1 + perform(Ask())}
```
````

Note: all guide content must be ASCII-only. Use `--` (double hyphen), never em dashes.

## Coverage

Run `just check-guides` to verify all pairs and see current coverage. As of the last
update, paired guides include: `quickstart.md`, `repl-tutorial.md`,
`effects-system-guide.md`. Remaining guides are being converted incrementally --
see the task plan for progress.

## Getting Started

- **[quickstart.md](quickstart.md)** -- Prose introduction: expressions, functions, control flow, Option, Result, collections, closures, structs, and algebraic effects
- **[repl-tutorial.md](repl-tutorial.md)** -- 22-step interactive tutorial to follow at `tur repl` or the web REPL

## Concurrency and Async

- **[threading-guide.md](threading-guide.md)** -- OS threads, `Arc<T>`, `Mutex<T>`, `Atomic<T>`, channels
- **[async-await-guide.md](async-await-guide.md)** -- Async/await with fibers and delimited continuations
- **[stm-guide.md](stm-guide.md)** -- Software transactional memory -- API reference and mechanics
- **[stm-tutorial.md](stm-tutorial.md)** -- STM tutorial: concepts, patterns, and worked examples
- **[session-types-guide.md](session-types-guide.md)** -- Session types: type-safe binary and multi-party protocols (`-Xsessions`)
- **[dynamic-vars-guide.md](dynamic-vars-guide.md)** -- Dynamic vars: thread-local, dynamically-scoped mutable cells (`-Xdynamic-vars`)

## Advanced Control Flow

- **[effects-system-guide.md](effects-system-guide.md)** -- Algebraic effects, dependency injection, custom control flow
- **[logic-programming-guide.md](logic-programming-guide.md)** -- Backtracking, logic programming, constraint solving with cloneable continuations
- **[checkpointing-guide.md](checkpointing-guide.md)** -- Cloneable continuations for persistent workflows and checkpointing
- **[serializable-continuations-guide.md](serializable-continuations-guide.md)** -- Serializable continuations for persistent workflows and cross-process computation
- **[web-continuations-guide.md](web-continuations-guide.md)** -- Compact reference: `send-form-and-wait`, continuation store, routing model

## Data Structures

- **[hamt-guide.md](hamt-guide.md)** -- Persistent hash maps with structural sharing (HAMT)

## Language Features

### Core Language

- **[structs-guide.md](structs-guide.md)** -- Struct types: `defstruct`, field access, ownership kinds, typeclasses, RC, linear fields
- **[module-system-guide.md](module-system-guide.md)** -- Module system, namespacing, exports
- **[c-integration-guide.md](c-integration-guide.md)** -- Foreign function interface (FFI) and C interop
- **[cli-args-guide.md](cli-args-guide.md)** -- CLI argument passing via `*args*` and structured parsing with `stdlib/args.tur`

### Type System

- **[type-annotations-guide.md](type-annotations-guide.md)** -- Compound type annotation syntax: `(-> a b)`, `(vec T)`, `forall`, and more
- **[hkt-guide.md](hkt-guide.md)** -- Higher-kinded types (functor, monad, applicative abstractions, performance/dispatch model)
- **[hrt-guide.md](hrt-guide.md)** -- Higher-ranked types: rank-2/3 polymorphic function parameters
- **[existential-types-guide.md](existential-types-guide.md)** -- Existential types: `pack`/`open`, typeclass-constrained boxing, the scope-escape check, and the `stdlib/existential.tur` helpers
- **[gadts-guide.md](gadts-guide.md)** -- GADTs: `defgadt`, type refinement, equality witnesses, union types, gradual typing
- **[gadts-cookbook.md](gadts-cookbook.md)** -- GADTs cookbook: practical patterns and recipes
- **[union-intersection-types-guide.md](union-intersection-types-guide.md)** -- Union (`A | B`) and intersection (`A & B`) types, `any`, gradual typing
- **[sized-types-guide.md](sized-types-guide.md)** -- Sized types: compile-time size tracking, stack allocation, `SizedBuf`, `SizedMatrix`, `SizedBitVec` (`-Xsized-types`)

### Functional Abstractions

- **[arrows-guide.md](arrows-guide.md)** -- Arrows: `arr`/`>>>`/`first`/`second`, composition combinators, and DSP signal graphs with `stdlib/signal/`
- **[generators-guide.md](generators-guide.md)** -- Zero-overhead generators (`gen`/`yield`), lazy `Seq` combinators, and `Range` types

### Design Rationale

- **[effects-vs-monads.md](effects-vs-monads.md)** -- Design rationale: why effects instead of Haskell-style monads
- **[advanced-type-system-rationale.md](advanced-type-system-rationale.md)** -- Design rationale: why Turmeric chose the advanced type system features it did, and why dependent and refinement types were deferred

## Type Safety

- **[substructural-types-guide.md](substructural-types-guide.md)** -- `^linear`, `^affine`, `^relevant` type disciplines
- **[uniqueness-types-guide.md](uniqueness-types-guide.md)** -- `^unique`: at-most-one-reference ownership

## Error Handling

- **[error-handling-guide.md](error-handling-guide.md)** -- `Result`, `Option`, `panic`, contract macros (`assert!`, `require!`, `ensure!`)
- **[contract-types-guide.md](contract-types-guide.md)** -- Contract types: `{ x : T | p }`, `:pre`/`:post` annotations, FFI contracts (planned v4)
- **[../design/error-handling-rationale.md](../design/error-handling-rationale.md)** -- Design rationale: exceptions vs. panic

## Tutorials and Examples

- **[minikanren-tutorial.md](minikanren-tutorial.md)** -- Logic programming with miniKanren
- **[cellular-automata-comonad-tutorial.md](cellular-automata-comonad-tutorial.md)** -- Cellular automata with comonads
- **[custom-effects-tutorial.md](custom-effects-tutorial.md)** -- Writing custom effects
- **[snake-game-tutorial.md](snake-game-tutorial.md)** -- Building the snake game example
- **[web-continuations-tutorial.md](web-continuations-tutorial.md)** -- Multi-page web forms using serializable continuations (guestbook example)
- **[web-emscripten-tutorial.md](web-emscripten-tutorial.md)** -- Compile Turmeric to WebAssembly with Emscripten and run it in a browser

### EAVT Database (multi-chapter)

- **[datalog/01-concepts.md](datalog/01-concepts.md)** -- EAVT model, immutability, comparison with SQL
- **[datalog/02-minimal-impl.md](datalog/02-minimal-impl.md)** -- Walk through `minimal.tur` line by line
- **[datalog/03-query-api.md](datalog/03-query-api.md)** -- Walk through `query.tur` additions
- **[datalog/04-indexing.md](datalog/04-indexing.md)** -- Why indexing matters and how `indexed.tur` works

## Package Management

- **[package-management-guide.md](package-management-guide.md)** -- Creating projects, adding spices, `build.tur`, `tur.lock`, CLI reference
- **[consuming-spices-guide.md](consuming-spices-guide.md)** -- Adding, fetching, and importing spices; cmake-deps; security and lock file rules
- **[developing-spices-guide.md](developing-spices-guide.md)** -- Creating, testing, versioning, and publishing a spice; wrapping C libs with cmake-deps; CMake/CPM export
- **[using-turmeric-from-cmake.md](using-turmeric-from-cmake.md)** -- Publishing a Turmeric library for C/C++ consumers via `tur emit-cmake`
- **[turmeric-spices](https://github.com/rjungemann/turmeric-spices)** -- Official first-party spice library: `tur-test`, `tur-math`, `tur-sqlite`, `tur-raylib`, `tur-json`, `tur-http`, `tur-regex`

## Tools and IDE

- **[devcontainer-guide.md](devcontainer-guide.md)** -- Dev container: open the project in a ready-to-build environment from VS Code or the CLI
- **[formatter-guide.md](formatter-guide.md)** -- `tur format` CLI and web REPL Format button
- **[lsp-guide.md](lsp-guide.md)** -- Language server (`tur lsp`): editor setup for diagnostics
- **[vscode-guide.md](vscode-guide.md)** -- VS Code extension installation and configuration
- **[vim-guide.md](vim-guide.md)** -- Vim / Neovim syntax highlighting installation and configuration

## Reference

- **[compiler-flags-guide.md](compiler-flags-guide.md)** -- All `-X` feature flags and diagnostic flags: status, what each enables, dependency graph, and common combinations
- **[test-runner-contract.md](test-runner-contract.md)** -- Test framework API and contract
- **[sandboxing-guide.md](sandboxing-guide.md)** -- Running untrusted code safely: `turi_env_new_sandboxed`, capability flags, step-fuel limits, and native function exposure

---

## Finding Guides

**By topic:**
- Concurrency → [threading-guide.md](threading-guide.md), [async-await-guide.md](async-await-guide.md), [stm-guide.md](stm-guide.md), [stm-tutorial.md](stm-tutorial.md), [session-types-guide.md](session-types-guide.md), [dynamic-vars-guide.md](dynamic-vars-guide.md)
- Data structures → [hamt-guide.md](hamt-guide.md)
- Control flow → [effects-system-guide.md](effects-system-guide.md), [logic-programming-guide.md](logic-programming-guide.md), [serializable-continuations-guide.md](serializable-continuations-guide.md), [web-continuations-guide.md](web-continuations-guide.md)
- Generators and sequences → [generators-guide.md](generators-guide.md)
- CLI arguments → [cli-args-guide.md](cli-args-guide.md)
- Arrows and signal processing → [arrows-guide.md](arrows-guide.md)
- Type system → [structs-guide.md](structs-guide.md), [hkt-guide.md](hkt-guide.md), [hrt-guide.md](hrt-guide.md), [module-system-guide.md](module-system-guide.md), [type-annotations-guide.md](type-annotations-guide.md), [union-intersection-types-guide.md](union-intersection-types-guide.md), [sized-types-guide.md](sized-types-guide.md)
- Type safety → [substructural-types-guide.md](substructural-types-guide.md), [uniqueness-types-guide.md](uniqueness-types-guide.md)
- Effects design → [effects-vs-monads.md](effects-vs-monads.md)
- Package management → [package-management-guide.md](package-management-guide.md), [consuming-spices-guide.md](consuming-spices-guide.md), [developing-spices-guide.md](developing-spices-guide.md), [using-turmeric-from-cmake.md](using-turmeric-from-cmake.md), [turmeric-spices](https://github.com/rjungemann/turmeric-spices)
- Tools → [devcontainer-guide.md](devcontainer-guide.md), [formatter-guide.md](formatter-guide.md), [lsp-guide.md](lsp-guide.md), [vscode-guide.md](vscode-guide.md), [vim-guide.md](vim-guide.md)
- Error handling → [error-handling-guide.md](error-handling-guide.md), [contract-types-guide.md](contract-types-guide.md)
- Embedding and sandboxing → [eval-api.md](eval-api.md), [sandboxing-guide.md](sandboxing-guide.md), [c-integration-guide.md](c-integration-guide.md)

**By level:**
- Beginner → [quickstart.md](quickstart.md), [repl-tutorial.md](repl-tutorial.md), then core feature guides
- Intermediate → Advanced control flow, type system
- Advanced → Effects, logic programming, serializable continuations, checkpointing

---

## Planning and Design

For design documents, architecture, and phase planning, see:
- **[../](../README.md)** -- Main docs folder
- **[../archive/](../archive/README.md)** -- Active planning documents
- **[../archive/history/](../archive/history/README.md)** -- Historical completed work
