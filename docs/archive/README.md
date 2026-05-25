# Archive

This folder contains planning and design documents for Turmeric features under
active development or consideration. For user-facing guides and tutorials, see
[../guides/](../guides/).

## Active Planning Documents

### Phase 19+ (Active Development)

- **[thread-safety-and-primitives-plan.md](thread-safety-and-primitives-plan.md)** -- Thread API design; see [../guides/threading-guide.md](../guides/threading-guide.md)
- **[fiber-asm-ctx-plan.md](fiber-asm-ctx-plan.md)** -- Fiber context-switching fallback strategy for macOS
- **[thread-cancellation-plan.md](thread-cancellation-plan.md)** -- Cooperative thread cancellation (TC0--TC2); prerequisite for WT3 (WASM threads)
- **[wasm-threads-plan.md](wasm-threads-plan.md)** -- WASM pthread support; WT0--WT2 and WT4 complete; WT3 deferred pending TC0--TC2
- **[release-tagging-plan.md](release-tagging-plan.md)** -- Semi-automatic version bumping and GitHub release workflow (not started)

### Ongoing Implementation

- **[contracts-plan.md](contracts-plan.md)** -- Runtime contracts; C0-C1 complete (see [../guides/error-handling-guide.md](../guides/error-handling-guide.md)); C2+ planned
- **[effect-rows-plan.md](effect-rows-plan.md)** -- Effect row enforcement; ER0-ER5 complete, ER6 core done (`try-with`, `--dump-effects`, `--lint-effects`); ER6 advanced items planned (see [../guides/effects-system-guide.md](../guides/effects-system-guide.md))
- **[gadts-plan.md](gadts-plan.md)** -- GADTs; G0-G4 substantially complete (see [../guides/gadts-guide.md](../guides/gadts-guide.md)); `equal-cong` deferred pending HKT
- **[gadts-followup-tasks.md](gadts-followup-tasks.md)** -- Open polish items from the GADT and intersection/union phases
- **[recursive-types-free-monad-plan.md](recursive-types-free-monad-plan.md)** -- Recursive types and Free monad; RF0-RF4 planned
- **[package-management-plan.md](package-management-plan.md)** -- Spice package manager v2; see [../guides/package-management-guide.md](../guides/package-management-guide.md)
- **[cmake-cpm-integration-plan.md](cmake-cpm-integration-plan.md)** -- CMake/CPM integration for C dependencies (v2.x target)
- **[build-and-test-ux-plan.md](build-and-test-ux-plan.md)** -- Build and test UX improvements (dev loop quality-of-life)
- **[vscode-c-inlining-plan.md](vscode-c-inlining-plan.md)** -- C-inlining syntax highlighting for VS Code extension
- **[hkt-deferred-tasks.md](hkt-deferred-tasks.md)** -- Higher-kinded types implementation tracking
- **[hkt-opaque-dispatch-plan.md](hkt-opaque-dispatch-plan.md)** -- HKT opaque-container dispatch; D0 planned (see [../guides/hkt-guide.md](../guides/hkt-guide.md) SS Known Limitations)
- **[interpreter-features-plan.md](interpreter-features-plan.md)** -- Gaps in `src/turi/eval.c`; pattern matching, Phase S4, async/S7 items
- **[effects-continuations-tasks.md](effects-continuations-tasks.md)** -- Consolidated task list for effect rows, linear-continuations, multishot-continuations

### CLI and Developer Experience

- **[developer-ergonomics-plan.md](developer-ergonomics-plan.md)** -- Day-one UX baseline checklist (help flags, eval command, etc.); several items outstanding
- **[autodoc-plan.md](autodoc-plan.md)** -- Docstring standard and doc generator (see also CLAUDE.md); doctest integration pending
- **[doctest-plan.md](doctest-plan.md)** -- Executable doctests from `;;;` Example blocks; not yet started
- **[guide-syntax-toggle-plan.md](guide-syntax-toggle-plan.md)** -- Toggle widget for S-expression/sweet-exp paired examples in guides
- **[lsp-hover-definition-completion-plan.md](lsp-hover-definition-completion-plan.md)** -- LD0-LD4: hover, go-to-definition, completion for the LSP server; not started
- **[datum-comment-plan.md](datum-comment-plan.md)** -- `#;` datum comments (DC0-DC3); no prerequisites; not yet started

### Try Turmeric / Web REPL

- **[try-turmeric-and-tutorial-plan.md](try-turmeric-and-tutorial-plan.md)** -- Web REPL and tutorial site planning
- **[try-turmeric-smoke-tests-plan.md](try-turmeric-smoke-tests-plan.md)** -- Smoke test strategy for the web REPL
- **[try-turmeric-wasm-effects-plan.md](try-turmeric-wasm-effects-plan.md)** -- WASM effects integration for Try Turmeric

### Performance and Testing

- **[performance-improvement-plan.md](performance-improvement-plan.md)** -- Compiler optimization roadmap
- **[performance-comparison-plan.md](performance-comparison-plan.md)** -- Five-language performance comparison framework
- **[perf-comparison-improvements-plan.md](perf-comparison-improvements-plan.md)** -- Idiomatic Turmeric refactor + turi/Rust targets
- **[test-suite-cleanup-plan.md](test-suite-cleanup-plan.md)** -- Documents root causes of intentionally-failing fixtures (956 pass, 5 tracked)
- **[skipped-spices-cleanup-plan.md](skipped-spices-cleanup-plan.md)** -- Phase 1 complete (Category A fixed); six spices still have `requires.typecheck-skip`; companion to `spice-aware-check-plan.md` (history)
- **[spice-test-runner-fix-plan.md](spice-test-runner-fix-plan.md)** -- Five-bug investigation blocking `tur test` / `tur fetch` in all spices; root cause: `{...}` parsed as `F_CONTRACT_TYPE`
- **[interpret-perf-tests-plan.md](interpret-perf-tests-plan.md)** -- Benchmarks runnable via `tur --interpret`

### Design Explorations (Not Yet Scoped)

- **[remove-exceptions-plan.md](remove-exceptions-plan.md)** -- Plan to remove remaining exception machinery
- **[set-literal-plan.md](set-literal-plan.md)** -- `#s(...)` set literal syntax
- **[scscm-tidal-spices-plan.md](scscm-tidal-spices-plan.md)** -- SuperCollider/Tidal live coding spice pair (Phase 20+ target)
- **[effect-types-row-polymorphism-plan.md](effect-types-row-polymorphism-plan.md)** -- Full first-class effect polymorphism (ET0-ET4); not started; v3 target
- **[multishot-continuations-plan.md](multishot-continuations-plan.md)** -- Multi-shot continuations (MS0-MS4); deferred until linear-continuations stable
- **[linear-continuations-plan.md](linear-continuations-plan.md)** -- Linear continuations; deferred to v5+
- **[self-hosted-interpreter-plan.md](self-hosted-interpreter-plan.md)** -- `libturi` importable eval API + self-hosted REPL (speculative; future project)
- **[stubs-and-workarounds.md](stubs-and-workarounds.md)** -- Catalog of known placeholders and test workarounds
- **[c-dsl-plan.md](c-dsl-plan.md)** -- Lisp-to-C99 DSL embedded in Turmeric; type-safe FFI code generation; speculative
- **[glsl-dsl-plan.md](glsl-dsl-plan.md)** -- GLSL shader DSL compiling to GLSL source; pairs with `c-dsl-plan.md`; speculative
- **[opengl-spice-plan.md](opengl-spice-plan.md)** -- `tur-opengl` wrapping OpenGL 3.3 + GLFW + GLAD; draft spice design
- **[new-spices-plan.md](new-spices-plan.md)** -- Seven new Tier-2 spices: postgres, valkey, osc, rtaudio, rtmidi, wav, png; none started
- **[design-mf4-struct-gadt-namespaces.md](design-mf4-struct-gadt-namespaces.md)** -- Separate struct/GADT namespaces to resolve `Vec` name collision (MF4)
- **[reader-macros-plan.md](reader-macros-plan.md)** -- User-defined `#foo[...]` reader macros (RM0-RM4); not started
- **[reader-macros-transitive-plan.md](reader-macros-transitive-plan.md)** -- Thread reader-macro registry through module loading; depends on RM0-RM4
- **[per-spice-docs-plan.md](per-spice-docs-plan.md)** -- Per-spice HTML pages from `README.md` + `;;;` docstrings; tooling plan
- **[web-cleanup-plan.md](web-cleanup-plan.md)** -- CSS consolidation, Prism.js syntax highlighting, web component unification

### Current Plans (Recently Added)

- **[sandboxed-eval-plan.md](sandboxed-eval-plan.md)** -- SB0--SB4 sandboxed eval implementation; SB1--SB4 close remaining gaps
- **[turmeric-spices-plan.md](turmeric-spices-plan.md)** -- Official first-party spice monorepo; all seven spices shipped; see [github.com/rjungemann/turmeric-spices](https://github.com/rjungemann/turmeric-spices)
- **[cross-plan-followups-plan.md](cross-plan-followups-plan.md)** -- Cross-plan follow-up tasks (F3-2..F3-7 dictionary passing, F5 MutableMap, F6 turi fixture gaps, F8 defstruct compound annotations)
- **[defstruct-field-types-plan.md](defstruct-field-types-plan.md)** -- Residual hardening after F8 landed; `exg5-exists-cycle` and `exg4-pack-into-struct` still blocked
- **[existential-gc-followup-plan.md](existential-gc-followup-plan.md)** -- GC integration for packed existentials; cycle-construction fixtures blocked on defstruct compound annotations
- **[existential-types-plan.md](existential-types-plan.md)** -- `pack`/`open` with typeclass constraints; phases partially in progress

## Extracted Guides

The following planning documents have guide counterparts in [../guides/](../guides/):

| Guide | Origin |
|---|---|
| [async-await-guide.md](../guides/async-await-guide.md) | `async-await-plan.md` (history) |
| [backtracking-guide.md](../guides/backtracking-guide.md) | `backtracking-cloneable-continuations-plan.md` (history) |
| [cli-args-guide.md](../guides/cli-args-guide.md) | `cli-arguments-plan.md` (history), `arg-parser-plan.md` (history) |
| [effects-system-guide.md](../guides/effects-system-guide.md) | `effects-plan.md` (history); `effect-rows-plan.md` (ER0-ER6 core) |
| [effects-vs-monads.md](../guides/effects-vs-monads.md) | `effects-vs-monads.md` (archive copy deleted; guides version is canonical) |
| [error-handling-guide.md](../guides/error-handling-guide.md) | `contracts-plan.md` (C0-C1 section) |
| [formatter-guide.md](../guides/formatter-guide.md) | `auto-formatter-plan.md` (history) |
| [gadts-guide.md](../guides/gadts-guide.md) | `gadts-plan.md` (G0-G4) |
| [gadts-cookbook.md](../guides/gadts-cookbook.md) | `gadts-plan.md` (examples) |
| [generators-guide.md](../guides/generators-guide.md) | `generators-and-sequences-plan.md` (history) |
| [hamt-guide.md](../guides/hamt-guide.md) | `hamt-plan.md` (history) |
| [hkt-guide.md](../guides/hkt-guide.md) | `closure-typeclass-dispatch-plan.md` (history; CCL complete) |
| [hrt-guide.md](../guides/hrt-guide.md) | `higher-ranked-types-plan.md` (history) |
| [minikanren-tutorial.md](../guides/minikanren-tutorial.md) | `minikanren-plan.md` (history) |
| [module-system-guide.md](../guides/module-system-guide.md) | `module-system-plan.md` (history) |
| [package-management-guide.md](../guides/package-management-guide.md) | `package-management-plan.md`, `tur-cli-plan.md` |
| [serializable-continuations-guide.md](../guides/serializable-continuations-guide.md) | `serializable-continuations-plan.md` (history) |
| [session-types-guide.md](../guides/session-types-guide.md) | `session-types-plan.md` (history; SS0--SS8 complete) |
| [sized-types-guide.md](../guides/sized-types-guide.md) | `sized-types-plan.md` (history; SZ3 complete) |
| [stm-guide.md](../guides/stm-guide.md) | `stm-plan-2.md` (history) |
| [substructural-types-guide.md](../guides/substructural-types-guide.md) | `substructural-types-plan.md` (complete); `linear-types-plan.md` (history) |
| [threading-guide.md](../guides/threading-guide.md) | `threading-tasks.md` (complete); `select-fair-blocking-plan.md` (history) |
| [type-annotations-guide.md](../guides/type-annotations-guide.md) | `compound-type-annotations-plan.md` (complete) |
| [union-intersection-types-guide.md](../guides/union-intersection-types-guide.md) | `intersection-union-types-plan.md` (history; IT0--IT4 complete) |
| [uniqueness-types-guide.md](../guides/uniqueness-types-guide.md) | `uniqueness-types-plan.md` (complete) |
| [vscode-guide.md](../guides/vscode-guide.md) | `vscode-syntax-highlighting-plan.md` (history) |
| [contract-types-guide.md](../guides/contract-types-guide.md) | `contract-types-plan.md` (history; CT0--CT4 substantially complete) |
| [dynamic-vars-guide.md](../guides/dynamic-vars-guide.md) | `dynamic-vars-plan.md` (history; DV0--DV4 complete) |
| [web-continuations-tutorial.md](../guides/web-continuations-tutorial.md) | `web-continuations-tutorial-plan.md` (complete) |
| [web-emscripten-tutorial.md](../guides/web-emscripten-tutorial.md) | `web-emscripten-tutorial-plan.md` (history) |
| [frame-guide.md](../guides/frame-guide.md) | `frame-spice-plan.md` (history; FR0-FR10 complete) |

## Historical Documents

Completed implementation plans and superseded design explorations are in
[history/](history/). Recent additions:

- **[frame-spice-plan.md](history/frame-spice-plan.md)** -- `tur-frame` dataframe spice; FR0-FR10 complete; see [frame-guide.md](../guides/frame-guide.md)
- **[module-docstrings-plan.md](history/module-docstrings-plan.md)** -- Module-level `;;;` docstring rendering; implemented and documented in CLAUDE.md
- **[signal-processing-arrows-plan.md](history/signal-processing-arrows-plan.md)** -- Arrow-based DSP tutorial; extracted to `tur-signal` v0.1.0; see [arrows-guide.md](../guides/arrows-guide.md)
- **[spice-aware-check-plan.md](history/spice-aware-check-plan.md)** -- Per-file `tur check` auto-discovers `build.tur`; implemented and documented in CLAUDE.md
- **[test-perf-plan.md](history/test-perf-plan.md)** -- Stamp caching + ccache; T1-A/B/C/D and T2-A/C complete
