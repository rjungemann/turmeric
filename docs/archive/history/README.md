# Historical Planning Documents

This folder contains planning and design documents for completed phases or superseded decisions. Kept for reference and historical context.

## Completed Phases (Phase 1--11)

- **deferred-tasks-phase7-phase11.md** -- Tracking for phases 7--11 (now complete); includes deferred work items that were rolled into later phases
- **deferred-tasks-phase15-phase19.md** -- Tracking for phases 15--19 (now complete); includes deferred items rolled into later phases

## Completed Infrastructure

- **cmake-migration-plan.md** -- CMake build system migration (Phase 16); completed and now standard practice
- **source-layout-plan.md** -- C/H source reorganisation; completed
- **bench-monte-carlo-pi-fix-plan.md** -- Monte Carlo Pi benchmark output-format fix; resolved

## Completed Feature Plans (with guides)

Plans whose features have shipped and are documented in [../../guides/](../../guides/).

| Plan | Guide |
|---|---|
| [auto-formatter-plan.md](auto-formatter-plan.md) | [formatter-guide.md](../../guides/formatter-guide.md) |
| [closure-typeclass-dispatch-plan.md](closure-typeclass-dispatch-plan.md) | [hkt-guide.md](../../guides/hkt-guide.md) (CCL section) |
| [contract-types-plan.md](contract-types-plan.md) | [contract-types-guide.md](../../guides/contract-types-guide.md) |
| [dynamic-vars-plan.md](dynamic-vars-plan.md) | [dynamic-vars-guide.md](../../guides/dynamic-vars-guide.md) |
| [generators-and-sequences-plan.md](generators-and-sequences-plan.md) | [generators-guide.md](../../guides/generators-guide.md) |
| [intersection-union-types-plan.md](intersection-union-types-plan.md) | [union-intersection-types-guide.md](../../guides/union-intersection-types-guide.md) |
| [linear-types-plan.md](linear-types-plan.md) | [substructural-types-guide.md](../../guides/substructural-types-guide.md) |
| [lsp-plan.md](lsp-plan.md) | [vscode-guide.md](../../guides/vscode-guide.md) (LSP section) |
| [select-fair-blocking-plan.md](select-fair-blocking-plan.md) | [threading-guide.md](../../guides/threading-guide.md) (select section) |
| [session-types-plan.md](session-types-plan.md) | [session-types-guide.md](../../guides/session-types-guide.md) |
| [sized-types-plan.md](sized-types-plan.md) | [sized-types-guide.md](../../guides/sized-types-guide.md) |
| [stdlib-plan.md](stdlib-plan.md) | stdlib shipped in full; see API docs |
| [vscode-syntax-highlighting-plan.md](vscode-syntax-highlighting-plan.md) | [vscode-guide.md](../../guides/vscode-guide.md) |

## Completed Tutorials (plans superseded by written guides)

- **datalog-database-tutorial-plan.md** -- EAVT database tutorial plan; tutorials written as [datalog/01--04](../../guides/)
- **web-emscripten-tutorial-plan.md** -- WASM/Emscripten tutorial plan; tutorial written as [web-emscripten-tutorial.md](../../guides/web-emscripten-tutorial.md)

## Completed Compiler Work (no standalone guide needed)

- **emit-effects-extraction-plan.md** -- EE0--EE3 effect/continuation source extraction; complete; see compiler-internals.md
- **test-process-reduction-plan.md** -- macOS Gatekeeper mitigation; all tiers complete

## Resolved Prerequisite Decisions

- **deferred-prerequisite-decisions.md** -- Design decisions that were locked pending later phases; now resolved

## Feasibility Studies

- **hamt-feasibility.md** -- Feasibility analysis for persistent hash array mapped tries (HAMTs); decision made and implemented

## Resolved Design Decisions

- **panic-system-vs-exception-system-plan.md** -- Pre-implementation comparison of panic vs. exception error handling (2024); decision made and implemented: hybrid Result + limited panic (see [../../guides/error-handling-guide.md](../../guides/error-handling-guide.md))

## Design References (Kept for Architecture Context)

- **higher-kinded-types-plan.md** -- HKT architecture and design (Phase H3); mostly implemented; kept as reference for understanding the type system design

---

These documents are archived for:
- **Audit trail**: Understanding historical decisions and trade-offs
- **Pattern reference**: Prior analysis patterns for future work
- **Closure**: Marking completed work
- **Legal/documentation**: Record of design process

Most users should refer to the active planning docs in [../](../) and user guides in [../../guides/](../../guides/).
