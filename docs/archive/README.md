# Archive

This folder contains planning and design documents for Turmeric features under
active development or consideration. For user-facing guides and tutorials, see
[../guides/](../guides/).

## Active Planning Documents

### Phase 19+ (Active Development)

- **[thread-safety-and-primitives-plan.md](thread-safety-and-primitives-plan.md)** -- Thread API design; see [../guides/threading-guide.md](../guides/threading-guide.md)
- **[fiber-asm-ctx-plan.md](fiber-asm-ctx-plan.md)** -- Fiber context-switching fallback strategy for macOS

### Ongoing Implementation

- **[contracts-plan.md](contracts-plan.md)** -- Runtime contracts; C0-C1 complete (see [../guides/error-handling-guide.md](../guides/error-handling-guide.md)); C2+ planned
- **[effect-rows-plan.md](effect-rows-plan.md)** -- Effect row enforcement; ER0-ER5 complete, ER6 core done (`try-with`, `--dump-effects`, `--lint-effects`); ER6 advanced items planned (see [../guides/effects-system-guide.md](../guides/effects-system-guide.md))
- **[gadts-plan.md](gadts-plan.md)** -- GADTs; G0-G4 substantially complete (see [../guides/gadts-guide.md](../guides/gadts-guide.md)); `equal-cong` deferred pending HKT
- **[gadts-followup-tasks.md](gadts-followup-tasks.md)** -- Open polish items from the GADT and intersection/union phases
- **[intersection-union-types-plan.md](intersection-union-types-plan.md)** -- IT0-IT4 complete (IT4 partial: boxing/cast deferred)
- **[linear-types-plan.md](linear-types-plan.md)** -- LT0-LT4 complete (gated behind `-Xlinear`; see [../guides/substructural-types-guide.md](../guides/substructural-types-guide.md))
- **[recursive-types-free-monad-plan.md](recursive-types-free-monad-plan.md)** -- Recursive types and Free monad; RF0-RF4 planned
- **[package-management-plan.md](package-management-plan.md)** -- Spice package manager v2; see [../guides/package-management-guide.md](../guides/package-management-guide.md)
- **[cmake-cpm-integration-plan.md](cmake-cpm-integration-plan.md)** -- CMake/CPM integration for C dependencies (v2.x target)
- **[build-and-test-ux-plan.md](build-and-test-ux-plan.md)** -- Build and test UX improvements (dev loop quality-of-life)
- **[vscode-c-inlining-plan.md](vscode-c-inlining-plan.md)** -- C-inlining syntax highlighting for VS Code extension
- **[hkt-deferred-tasks.md](hkt-deferred-tasks.md)** -- Higher-kinded types implementation tracking
- **[hkt-opaque-dispatch-plan.md](hkt-opaque-dispatch-plan.md)** -- HKT opaque-container dispatch; D0 planned (see [../guides/hkt-guide.md](../guides/hkt-guide.md) §Known Limitations)

### Try Turmeric / Web REPL

- **[try-turmeric-and-tutorial-plan.md](try-turmeric-and-tutorial-plan.md)** -- Web REPL and tutorial site planning
- **[try-turmeric-smoke-tests-plan.md](try-turmeric-smoke-tests-plan.md)** -- Smoke test strategy for the web REPL
- **[try-turmeric-wasm-effects-plan.md](try-turmeric-wasm-effects-plan.md)** -- WASM effects integration for Try Turmeric

### Design Explorations (Not Yet Scoped)

- **[signal-processing-arrows-plan.md](signal-processing-arrows-plan.md)** -- Signal processing with arrows and HKTs (tutorial plan; depends on Phase 19 + HKT)
- **[performance-improvement-plan.md](performance-improvement-plan.md)** -- Compiler optimization roadmap
- **[test-perf-plan.md](test-perf-plan.md)** -- Test suite and performance testing strategy
- **[panic-system-vs-exception-system-plan.md](panic-system-vs-exception-system-plan.md)** -- Error handling design rationale
- **[remove-exceptions-plan.md](remove-exceptions-plan.md)** -- Plan to remove exception machinery
- **[set-literal-plan.md](set-literal-plan.md)** -- `#s(...)` set literal syntax
- **[autodoc-plan.md](autodoc-plan.md)** -- Docstring standard and doc generator (see also CLAUDE.md)
- **[auto-formatter-plan.md](auto-formatter-plan.md)** -- Formatter implementation details; see [../guides/formatter-guide.md](../guides/formatter-guide.md)
- **[vscode-syntax-highlighting-plan.md](vscode-syntax-highlighting-plan.md)** -- VS Code extension implementation; see [../guides/vscode-guide.md](../guides/vscode-guide.md)
- **[scscm-hcsynth-livecoding-plan.md](scscm-hcsynth-livecoding-plan.md)** -- SuperCollider/Haskell live coding integration (Phase 20+ target)
- **[effect-types-row-polymorphism-plan.md](effect-types-row-polymorphism-plan.md)** -- Full first-class effect polymorphism (ET0-ET4); not started; v3 target
- **[multishot-continuations-plan.md](multishot-continuations-plan.md)** -- Multi-shot continuations (MS0-MS4); deferred until linear-continuations stable
- **[linear-continuations-plan.md](linear-continuations-plan.md)** -- Linear continuations; deferred to v5+

## Extracted Guides

The following planning documents have guide counterparts in [../guides/](../guides/):

| Guide | Origin |
|---|---|
| [async-await-guide.md](../guides/async-await-guide.md) | `async-await-plan.md` (history) |
| [backtracking-guide.md](../guides/backtracking-guide.md) | `backtracking-cloneable-continuations-plan.md` (history) |
| [effects-system-guide.md](../guides/effects-system-guide.md) | `effects-plan.md` (history); `effect-rows-plan.md` (ER0-ER6 core) |
| [effects-vs-monads.md](../guides/effects-vs-monads.md) | `effects-vs-monads.md` (archive copy deleted; guides version is canonical) |
| [error-handling-guide.md](../guides/error-handling-guide.md) | `contracts-plan.md` (C0-C1 section) |
| [gadts-guide.md](../guides/gadts-guide.md) | `gadts-plan.md` (G0-G4) |
| [gadts-cookbook.md](../guides/gadts-cookbook.md) | `gadts-plan.md` (examples) |
| [formatter-guide.md](../guides/formatter-guide.md) | `auto-formatter-plan.md` |
| [hamt-guide.md](../guides/hamt-guide.md) | `hamt-plan.md` (history) |
| [hkt-guide.md](../guides/hkt-guide.md) | `closure-typeclass-dispatch-plan.md` (CCL complete; closure-capture section added) |
| [hrt-guide.md](../guides/hrt-guide.md) | `higher-ranked-types-plan.md` (history) |
| [minikanren-tutorial.md](../guides/minikanren-tutorial.md) | `minikanren-plan.md` (history) |
| [module-system-guide.md](../guides/module-system-guide.md) | `module-system-plan.md` (history) |
| [package-management-guide.md](../guides/package-management-guide.md) | `package-management-plan.md`, `tur-cli-plan.md` |
| [serializable-continuations-guide.md](../guides/serializable-continuations-guide.md) | `serializable-continuations-plan.md` (history) |
| [stm-guide.md](../guides/stm-guide.md) | `stm-plan-2.md` (history) |
| [substructural-types-guide.md](../guides/substructural-types-guide.md) | `substructural-types-plan.md` (complete) |
| [tidal-guide.md](../guides/tidal-guide.md) | `tidalcycles-dsl-plan.md` (history) |
| [threading-guide.md](../guides/threading-guide.md) | `threading-tasks.md` (complete); `select-fair-blocking-plan.md` (SEL0-SEL2 complete) |
| [type-annotations-guide.md](../guides/type-annotations-guide.md) | `compound-type-annotations-plan.md` (complete) |
| [uniqueness-types-guide.md](../guides/uniqueness-types-guide.md) | `uniqueness-types-plan.md` (complete) |
| [vscode-guide.md](../guides/vscode-guide.md) | `vscode-syntax-highlighting-plan.md` |
| [web-continuations-tutorial.md](../guides/web-continuations-tutorial.md) | `web-continuations-tutorial-plan.md` (complete) |

## Historical Documents

Completed implementation plans and superseded design explorations are in
[history/](history/). The `history/` folder contains documents from phases 7-11.
