# Archive

This folder contains planning and design documents for Turmeric features under
active development or consideration. For user-facing guides and tutorials, see
[../guides/](../guides/).

## Active Planning Documents

### Phase 19+ (Active Development)

- **[thread-safety-and-primitives-plan.md](thread-safety-and-primitives-plan.md)** -- Thread API design; see [../guides/threading-guide.md](../guides/threading-guide.md)
- **[fiber-asm-ctx-plan.md](fiber-asm-ctx-plan.md)** -- Fiber context-switching fallback strategy for macOS

### Try Turmeric / Web REPL

- **[try-turmeric-and-tutorial-plan.md](try-turmeric-and-tutorial-plan.md)** -- Web REPL and tutorial site planning
- **[try-turmeric-smoke-tests-plan.md](try-turmeric-smoke-tests-plan.md)** -- Smoke test strategy for the web REPL
- **[try-turmeric-wasm-effects-plan.md](try-turmeric-wasm-effects-plan.md)** -- WASM effects integration for Try Turmeric

### Design Explorations (Not Yet Scoped)

- **[gadts-plan.md](gadts-plan.md)** -- Generalized algebraic data types
- **[signal-processing-arrows-plan.md](signal-processing-arrows-plan.md)** -- Signal processing with arrows and HKTs
- **[hkt-deferred-tasks.md](hkt-deferred-tasks.md)** -- Higher-kinded types implementation tracking
- **[performance-improvement-plan.md](performance-improvement-plan.md)** -- Compiler optimization roadmap
- **[test-perf-plan.md](test-perf-plan.md)** -- Test suite and performance testing strategy
- **[panic-system-vs-exception-system-plan.md](panic-system-vs-exception-system-plan.md)** -- Error handling design decision
- **[remove-exceptions-plan.md](remove-exceptions-plan.md)** -- Plan to remove exception machinery
- **[set-literal-plan.md](set-literal-plan.md)** -- `#s(...)` set literal syntax
- **[autodoc-plan.md](autodoc-plan.md)** -- Docstring standard and doc generator (see also CLAUDE.md)
- **[auto-formatter-plan.md](auto-formatter-plan.md)** -- Formatter implementation details; see [../guides/formatter-guide.md](../guides/formatter-guide.md)
- **[vscode-syntax-highlighting-plan.md](vscode-syntax-highlighting-plan.md)** -- VS Code extension implementation; see [../guides/vscode-guide.md](../guides/vscode-guide.md)
- **[effects-vs-monads.md](effects-vs-monads.md)** -- Effects vs. monads analysis; see [../guides/effects-vs-monads.md](../guides/effects-vs-monads.md)
- **[scscm-hcsynth-livecoding-plan.md](scscm-hcsynth-livecoding-plan.md)** -- SuperCollider/Haskell live coding integration

## Extracted Guides

The following planning documents have guide counterparts in [../guides/](../guides/):

| Guide | Plan (in docs/history/) |
|---|---|
| [async-await-guide.md](../guides/async-await-guide.md) | `async-await-plan.md` |
| [backtracking-guide.md](../guides/backtracking-guide.md) | `backtracking-cloneable-continuations-plan.md` |
| [effects-system-guide.md](../guides/effects-system-guide.md) | `effects-plan.md` |
| [effects-vs-monads.md](../guides/effects-vs-monads.md) | `effects-vs-monads.md` |
| [formatter-guide.md](../guides/formatter-guide.md) | `auto-formatter-plan.md` |
| [hamt-guide.md](../guides/hamt-guide.md) | `hamt-plan.md` |
| [hrt-guide.md](../guides/hrt-guide.md) | `higher-ranked-types-plan.md` |
| [minikanren-tutorial.md](../guides/minikanren-tutorial.md) | `minikanren-plan.md` |
| [module-system-guide.md](../guides/module-system-guide.md) | `module-system-plan.md` |
| [serializable-continuations-guide.md](../guides/serializable-continuations-guide.md) | `serializable-continuations-plan.md` |
| [stm-guide.md](../guides/stm-guide.md) | `stm-plan-2.md` |
| [tidal-guide.md](../guides/tidal-guide.md) | `tidalcycles-dsl-plan.md` |
| [vscode-guide.md](../guides/vscode-guide.md) | `vscode-syntax-highlighting-plan.md` |

## Historical Documents

Completed implementation plans and superseded design explorations are in
[../history/](../history/). The original `docs/archive/history/` folder
contains earlier historical documents from phases 7-11.
