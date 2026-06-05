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
- **[multishot-continuations-plan.md](../upcoming/multishot-continuations-plan.md)** -- Multi-shot continuations (MS0-MS4); deferred until linear-continuations stable
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

### Language Ergonomics (Planned)

- **[internal-define-plan.md](history/internal-define-plan.md)** *(history)* -- body-level `define` (let\* semantics); implemented; see [binding-forms-guide.md](../guides/binding-forms-guide.md)
- **[letrec-and-named-let-plan.md](history/letrec-and-named-let-plan.md)** *(history)* -- `letrec` + named let; implemented; see [binding-forms-guide.md](../guides/binding-forms-guide.md)

### Compiler and Codegen (Active)

- **[arbitrary-arity-kinds-plan.md](arbitrary-arity-kinds-plan.md)** -- Lift the `Tuple5` / `KIND_ARROW5` cap; replace closed `Kind` enum with an integer-backed representation; not started
- **[codegen-clang-int-pointer-cleanup-plan.md](codegen-clang-int-pointer-cleanup-plan.md)** -- Clean `int<->pointer` casts in generated C to fix modern Clang warnings; not started
- **[cross-module-specialization-cache-plan.md](cross-module-specialization-cache-plan.md)** -- Cross-module ABI specialization cache (Phase J of unboxing plan); prerequisites A--I landed; not started
- **[unboxing-and-monomorphization-plan.md](unboxing-and-monomorphization-plan.md)** -- Unboxed structs, monomorphization, and sized primitives; partial (sized types landed); remaining work in phases B/C

### Tooling (Active)

- **[tur-run-plan.md](tur-run-plan.md)** -- `tur run` Justfile task runner + `tur new` scaffold; basic `tur run` shipped (see [tur-run-guide.md](../guides/tur-run-guide.md)); `tur new`, extended phases (RN6--RN9) outstanding

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
| [binding-forms-guide.md](../guides/binding-forms-guide.md) | `internal-define-plan.md` (history); `letrec-and-named-let-plan.md` (history) |
| [tur-watch-guide.md](../guides/tur-watch-guide.md) | `tur-watch-spice-plan.md` (history; v0.2.0); `tur-watch-tree-per-file-naming-plan.md` (history) |
| [web-stack-guide.md](../guides/web-stack-guide.md) | `tur-httpd-plan.md` (history); `tur-template-plan.md` (history); `tur-tourist-plan.md` (history) |
| [httpd-guide.md](../guides/httpd-guide.md) | `tur-httpd-stdlib-plan.md` (history; H1-H7 shipped) |
| [data-literals-guide.md](../guides/data-literals-guide.md) | `data-literals-plan.md` (history; DL0-DL6 complete) |
| [reactor-guide.md](../guides/reactor-guide.md) | `tur-reactor-plan.md` (history; R1-R8 shipped); `reactor-run-fibers-plan.md` (history; F1-F8 shipped) |
| [httpd-tls-guide.md](../guides/httpd-tls-guide.md) | `tur-tls-plan.md` (history; T1-T6 complete) |
| [developing-spices-guide.md](../guides/developing-spices-guide.md) | augmented by `tur-fetch-system-first-plan.md` (history; SF0-SF4 complete) |
| [httpd-middleware-guide.md](../guides/httpd-middleware-guide.md) | `httpd-middleware-async-plan.md` (history; Track M, M0-M8) |
| [httpd-async-guide.md](../guides/httpd-async-guide.md) | `httpd-middleware-async-plan.md` (history; Track A, A0-A4) |
| [syntax-guide.md](../guides/syntax-guide.md) | `syntax-guide-plan.md` (history); `parse-check-subcommand-plan.md` (history); `sweet-exp-followups-plan.md` (history) |
| [parser-combinators-tutorial.md](../guides/parser-combinators-tutorial.md) | `parser-combinators-tutorial-plan.md` (history) |
| [tourist-routing-guide.md](../guides/tourist-routing-guide.md) | `tourist-routing-composition-plan.md` (history; TR0-TR4 complete) |
| [lsp-guide.md](../guides/lsp-guide.md) | augmented by `ai-assistant-lsp-integration-plan.md` (history; LSP gap-fill + `tur mcp`) |
| [ai-assistant-integration-guide.md](../guides/ai-assistant-integration-guide.md) | `ai-assistant-lsp-integration-plan.md` (history; MCP surface) |

## Historical Documents

Completed implementation plans and superseded design explorations are in
[history/](history/). Recent additions (post-v0.12.0 sweep):

- **[notebook-spice-plan.md](history/notebook-spice-plan.md)** -- `tur-notebook` spice; NB0-NB12 complete; see [notebook-guide.md](../guides/notebook-guide.md)
- **[stats-spice-plan.md](history/stats-spice-plan.md)** -- `tur-stats` spice shipped; see [stats-guide.md](../guides/stats-guide.md)
- **[plot-spice-plan.md](history/plot-spice-plan.md)** -- `tur-plot` spice shipped; follow-ups in [plot-spice-followups-plan.md](plot-spice-followups-plan.md)
- **[adt-type-params-plan.md](history/adt-type-params-plan.md)** -- Typed type parameters for `defdata` / `defgadt`; complete
- **[csv-optional-delimiter-plan.md](history/csv-optional-delimiter-plan.md)** -- Optional delimiter in `stdlib/csv.tur`; complete
- **[test-recovery-plan.md](history/test-recovery-plan.md)** -- `turi_fixture_tests` / `tur_spice_resolver_tests` fully green
- **[tuple-type-plan.md](history/tuple-type-plan.md)** -- N-ary tuple types (`Tuple2`..`Tuple5`, TP1) shipped
- **[direct-anonymous-lambda-application-plan.md](history/direct-anonymous-lambda-application-plan.md)** -- `((fn [...] ...) args)` direct application; landed in `7660a5b8`
- **[param-type-annotation-plan.md](history/param-type-annotation-plan.md)** -- Spaced `: T` parameter type annotations; landed in `fccb1621`
- **[typed-slots-generic-substrate-plan.md](history/typed-slots-generic-substrate-plan.md)** -- GS1-GS5 substrate; subsumed by completed TS1-TS6
- **[typed-slots-gs5-compiler-support-plan.md](history/typed-slots-gs5-compiler-support-plan.md)** -- GS5 compiler support; subsumed by completed TS1-TS6

Post-v0.14.6 sweep:

- **[variadic-typing-plan.md](history/variadic-typing-plan.md)** -- Typed variadic rest parameters (V0--V7); complete; documented in CLAUDE.md
- **[control-flow-completeness-plan.md](history/control-flow-completeness-plan.md)** -- Pre-v1.0 control-flow gap closure (CF0--CF7); complete; see [effects-system-guide.md](../guides/effects-system-guide.md) and [backtracking-guide.md](../guides/backtracking-guide.md)
- **[control-flow-completeness-audit.md](history/control-flow-completeness-audit.md)** -- Pre-/post-v1.0 control-flow audit; companion to above
- **[typing-gap-plan.md](history/typing-gap-plan.md)** -- Pre-v1.0 advanced-typing gap closure (TY0--TY6); complete
- **[typing-gap-audit.md](history/typing-gap-audit.md)** -- Pre-/post-v1.0 advanced-typing audit; companion to above
- **[first-class-handlers-plan.md](history/first-class-handlers-plan.md)** -- First-class effect handler values (FH0--FH7); complete; `effects-system-guide.md` updated
- **[first-class-handlers-semantics.md](history/first-class-handlers-semantics.md)** -- FH0 operational semantics spec; companion to above
- **[handler-typecheck-and-typename-followups-plan.md](history/handler-typecheck-and-typename-followups-plan.md)** -- Handler arg-checking + `type_name` ownership follow-ups (PH0--PH3); complete
- **[lifetime-syntax-plan.md](history/lifetime-syntax-plan.md)** -- `'a` lifetime-annotation surface syntax (LS0--LS5); complete; `substructural-types-guide.md` updated
- **[sized-types-completion-plan.md](history/sized-types-completion-plan.md)** -- Sized types SZ4--SZ9 (real `-Xsized-types` flag, type-level index, static checking, inference); complete; see [sized-types-guide.md](../guides/sized-types-guide.md)
- **[sized-types-index-spec.md](history/sized-types-index-spec.md)** -- SZ6 type-level size-index spec; companion to above
- **[manifest-driven-build-descent-plan.md](history/manifest-driven-build-descent-plan.md)** -- Manifest-aware `tur build <dir>` + recursive src/ collection + T1--T3 follow-ups; complete; documented in CLAUDE.md
- **[local-spice-dev-workflow-plan.md](history/local-spice-dev-workflow-plan.md)** -- `:path`/`:members` local cross-spice dependency resolution (LS1--LS8); complete; documented in CLAUDE.md
- **[scscm-spice-import-refactor-plan.md](history/scscm-spice-import-refactor-plan.md)** -- scscm spice import refactor + `scscm-compile` fixture repair; complete
- **[repl-spice-watch-flake-plan.md](history/repl-spice-watch-flake-plan.md)** -- De-flake `tur_repl_spice_watch` CI test; complete
- **[reader-float-parsing-plan.md](history/reader-float-parsing-plan.md)** -- Float-literal exponent + precision fixes (Option B); complete
- **[asan-debug-leaks-plan.md](history/asan-debug-leaks-plan.md)** -- ASan/LSan leak policy + ABI-spec arena fix (Phase 1+2); complete; documented in CLAUDE.md
- **[known-bugs-followups-plan.md](history/known-bugs-followups-plan.md)** -- Remaining open bugs from `known-bugs.md`; all resolved
- **[internal-define-plan.md](history/internal-define-plan.md)** -- Body-level `define` (let\* semantics); complete; see [binding-forms-guide.md](../guides/binding-forms-guide.md)
- **[letrec-and-named-let-plan.md](history/letrec-and-named-let-plan.md)** -- `letrec` + named let; complete; see [binding-forms-guide.md](../guides/binding-forms-guide.md)
- **[raygui-spice-plan.md](history/raygui-spice-plan.md)** -- `tur-raygui` immediate-mode GUI spice; complete (v0.1.0 in turmeric-spices)
- **[tur-watch-spice-plan.md](history/tur-watch-spice-plan.md)** -- `tur-watch` cross-platform watcher spice; complete (v0.2.0); see [tur-watch-guide.md](../guides/tur-watch-guide.md)
- **[tur-watch-tree-per-file-naming-plan.md](history/tur-watch-tree-per-file-naming-plan.md)** -- `tur-watch` v0.2.0 per-file naming in tree mode; complete
- **[tur-httpd-plan.md](history/tur-httpd-plan.md)** -- `tur-httpd` threaded HTTP/1.1 server spice; complete (v0.1.0); see [web-stack-guide.md](../guides/web-stack-guide.md)
- **[tur-template-plan.md](history/tur-template-plan.md)** -- `tur-template` ERB/EJS-style templating engine spice; complete (v0.1.0); see [web-stack-guide.md](../guides/web-stack-guide.md)
- **[tur-tourist-plan.md](history/tur-tourist-plan.md)** -- `tur-tourist` Scotty-style micro-framework spice; complete (v0.1.0); see [web-stack-guide.md](../guides/web-stack-guide.md)
- **[tur-fmt-plan.md](history/tur-fmt-plan.md)** -- `tur fmt` in-place formatter with directory walking; complete (see `tur fmt --help`); see also [formatter-guide.md](../guides/formatter-guide.md)
- **[solid-modeling-sdf-raylib-plan.md](history/solid-modeling-sdf-raylib-plan.md)** -- `tur-sdf-raylib` SDF + raylib solid modeling spice; Phase 1 complete (v0.1.0 in turmeric-spices)

Post-v0.16.0 sweep:

- **[tur-reactor-plan.md](history/tur-reactor-plan.md)** -- `tur/reactor` lightweight evented reactor; R1-R8 shipped; see [reactor-guide.md](../guides/reactor-guide.md)
- **[reactor-run-fibers-plan.md](history/reactor-run-fibers-plan.md)** -- Local fiber driver on top of `tur/reactor`; F1-F8 shipped; F9 (global-scheduler rewrite) split out to [scheduler-on-local-fiber-group-plan.md](scheduler-on-local-fiber-group-plan.md)
- **[tur-httpd-stdlib-plan.md](history/tur-httpd-stdlib-plan.md)** -- `stdlib/httpd` lightweight HTTP/1.1 server (was `tur-httpd-plan.md`); H1-H7 shipped; see [httpd-guide.md](../guides/httpd-guide.md)
- **[tur-tls-plan.md](history/tur-tls-plan.md)** -- `tur-tls` spice + httpd H5 integration; T1-T6 complete; see [httpd-tls-guide.md](../guides/httpd-tls-guide.md)
- **[data-literals-plan.md](history/data-literals-plan.md)** -- `#map{...}` / `#set{...}` / `[...]` data literals (`-Xdata-literals`); DL0-DL6 complete; see [data-literals-guide.md](../guides/data-literals-guide.md)
- **[tur-fetch-system-first-plan.md](history/tur-fetch-system-first-plan.md)** -- `:prefer-system` resolution for `tur fetch :cmake-deps`; SF0-SF4 complete; documented in [developing-spices-guide.md](../guides/developing-spices-guide.md)
- **[list-quasiquote-plan.md](history/list-quasiquote-plan.md)** -- Runtime list quasiquote (`` #` ``); rejected (Option D / "not now")
- **[runtime-symbols-plan.md](history/runtime-symbols-plan.md)** -- First-class `:Sym` type (`-Xsymbols`); SYM0--SYM6 complete; see [symbols-guide.md](../guides/symbols-guide.md)
- **[schema-plan.md](history/schema-plan.md)** -- `tur/schema` runtime validation; SC0--SC4/SC6 shipped here; SC5/SC7 completed in `return-type-dispatch-and-schema-sc5-sc7-plan.md`; see [schema-guide.md](../guides/schema-guide.md)
- **[return-type-dispatch-and-schema-sc5-sc7-plan.md](history/return-type-dispatch-and-schema-sc5-sc7-plan.md)** -- Return-type-directed dispatch + `HasSchema` typeclass (SC5) + `Functor`/`Applicative`/`Alternative` (SC7); complete; see [schema-guide.md](../guides/schema-guide.md)
- **[sc7-carrier-duality-plan.md](history/sc7-carrier-duality-plan.md)** -- SC7 final blocker: transparent int-newtype approach for `(Schema a)` wrapper; resolved
- **[json-reader-macro-plan.md](history/json-reader-macro-plan.md)** -- `#json(...)` reader macro; JR0--JR5 complete; see [json-guide.md](../guides/json-guide.md)
- **[typed-collection-elements-plan.md](history/typed-collection-elements-plan.md)** -- Polymorphic `Vec[A]`/`Map[K V]` element carrier (TCE0--TCE6); complete; see [data-literals-guide.md](../guides/data-literals-guide.md)
- **[typed-map-surface-plan.md](history/typed-map-surface-plan.md)** -- Typed `Map[K V]` API surface (TMS0--TMS5); `smap-*` retired; see [data-literals-guide.md](../guides/data-literals-guide.md)
- **[generic-map-key-dispatch-plan.md](history/generic-map-key-dispatch-plan.md)** -- Uniform `#map{...}`/`hamt-of` content-keyed dispatch for typed keys (GMK0--GMK4); complete via Approach B; see [data-literals-guide.md](../guides/data-literals-guide.md)
- **[generic-hash-eq-dispatch-plan.md](history/generic-hash-eq-dispatch-plan.md)** -- `Hash`/`Eq` typeclass dispatch for typed map keys (GHE0--GHE5); effectively complete; generic-dict path extracted to GDE plan
- **[generic-dict-eq-map-dispatch-plan.md](history/generic-dict-eq-map-dispatch-plan.md)** -- Content equality through polymorphic `^Eq A` constraint (GDE0--GDE5); complete
- **[diagnose-unbound-call-heads-plan.md](history/diagnose-unbound-call-heads-plan.md)** -- `tur check` diagnoses unbound call heads (UCH0--UCH2); complete
- **[codegen-parentheses-warnings-plan.md](history/codegen-parentheses-warnings-plan.md)** -- BIN_INFIX/VARIADIC_FOLD paren trimming to silence `-Wparentheses-equality` (PW0--PW3); complete
- **[codegen-cross-module-private-defn-collision-plan.md](history/codegen-cross-module-private-defn-collision-plan.md)** -- Fix for private same-named defns across modules collapsing to one C symbol (CC0--CC2); complete
- **[fat-closure-return-position-plan.md](history/fat-closure-return-position-plan.md)** -- `^fat` return-type marker auto-shimming non-capturing lambdas at return sites; complete; documented in test-suite-idioms-plan.md

Post-v0.18.0 sweep:

- **[httpd-middleware-async-plan.md](history/httpd-middleware-async-plan.md)** -- `tur/httpd` standard middleware library and async server; M0-M8 + A0-A4 shipped; see [httpd-middleware-guide.md](../guides/httpd-middleware-guide.md) and [httpd-async-guide.md](../guides/httpd-async-guide.md). Remaining items tracked in [httpd-middleware-plan.md](httpd-middleware-plan.md)
- **[error-handling-deferred-plan.md](history/error-handling-deferred-plan.md)** -- `?` query operator, contracts, panic lints, `catch-unwind` (R1, C2, R6a-d, R2 + R6c); complete; see [error-handling-guide.md](../guides/error-handling-guide.md)
- **[cps-transform-plan.md](history/cps-transform-plan.md)** -- Whole-program CPS transform (CPS0-CPS11); complete; backs `call/cc`, `escape`, and the delimited-control substrate; see [delimited-control-operators-guide.md](../guides/delimited-control-operators-guide.md)
- **[call-cc-completion-plan.md](history/call-cc-completion-plan.md)** -- Undelimited `call/cc` / `escape` on the CPS substrate (CC0-CC6); complete; enabled by default; see [delimited-control-operators-guide.md](../guides/delimited-control-operators-guide.md)
- **[ai-assistant-lsp-integration-plan.md](history/ai-assistant-lsp-integration-plan.md)** -- LSP capability gap-fill + MCP server (`tur mcp`); shipped in #173; see [lsp-guide.md](../guides/lsp-guide.md) and [ai-assistant-integration-guide.md](../guides/ai-assistant-integration-guide.md)
- **[parse-check-subcommand-plan.md](history/parse-check-subcommand-plan.md)** -- `tur parse-check` subcommand backing the guide toggle-pair AST checker; shipped in #182; see [syntax-guide.md](../guides/syntax-guide.md)
- **[syntax-guide-plan.md](history/syntax-guide-plan.md)** -- Unified syntax guide covering s-expressions and sweet-expression mode; shipped in #182; see [syntax-guide.md](../guides/syntax-guide.md)
- **[sweet-exp-followups-plan.md](history/sweet-exp-followups-plan.md)** -- SRFI-110 preprocessor follow-ups (curly-infix operator detection, neoteric bracket chaining); all landed (#188); see [syntax-guide.md](../guides/syntax-guide.md)
- **[parser-combinators-tutorial-plan.md](history/parser-combinators-tutorial-plan.md)** -- Parser-combinators tutorial outline; shipped as [parser-combinators-tutorial.md](../guides/parser-combinators-tutorial.md)
- **[tourist-routing-composition-plan.md](history/tourist-routing-composition-plan.md)** -- `tourist/routing` (`url-map!`, `cascade!`, `cascade-with!`, `req-full-path`) in `turmeric-spices/spices/tourist`; TR0-TR4 complete; see [tourist-routing-guide.md](../guides/tourist-routing-guide.md)
- **[tursweet-extension-rename-plan.md](history/tursweet-extension-rename-plan.md)** -- Global rename of `.tursweet` -> `.tur.sweet` and `--lang tursweet` -> `--lang sweet-exp`; complete
- **[drop-just-dependency-plan.md](history/drop-just-dependency-plan.md)** -- Replace `just` invocations in docs/bootstrap with plain CMake + `tur run`; complete; see [tur-run-guide.md](../guides/tur-run-guide.md)
- **[defstruct-inline-c-byvalue-callsite-plan.md](history/defstruct-inline-c-byvalue-callsite-plan.md)** -- Compiler: sync call-site to inline-C by-value struct params (DS0-DS2); complete
- **[variadic-rest-closure-cast-plan.md](history/variadic-rest-closure-cast-plan.md)** -- Compiler: cast variadic-rest function-pointer args at call sites (V0-V2); complete
- **[curried-call-cast-rough-edges-plan.md](history/curried-call-cast-rough-edges-plan.md)** -- Compiler: partial-application + closure-call `(intptr_t)` cast fixes surfaced by the httpd middleware work; complete
- **[bare-fat-lambda-param-plan.md](history/bare-fat-lambda-param-plan.md)** -- Allow bare `^fat g` param on `(fn ...)` lambdas; implemented
- **[noncapturing-closure-inline-c-dispatch-plan.md](history/noncapturing-closure-inline-c-dispatch-plan.md)** -- Auto-shim non-capturing closures at fat-dispatched inline-C sinks; implemented (2026-06-03)
- **[httpd-conn-struct-consolidation-plan.md](history/httpd-conn-struct-consolidation-plan.md)** -- Consolidate `HttpdConn` struct definitions in `stdlib/httpd.tur`; implemented (2026-06-03)
- **[io-real-filesystem-and-list-dir-uncompilable.md](history/io-real-filesystem-and-list-dir-uncompilable.md)** -- `stdlib/io.tur` nested-`static` + missing `<dirent.h>` defects; resolved
- **[log-capability-vtable-uncompilable.md](history/log-capability-vtable-uncompilable.md)** -- `stdlib/log.tur` and `test/capability.tur` vtable defects (same root cause as io); resolved
- **[sourcefile-uninit-xform-map-fix.md](history/sourcefile-uninit-xform-map-fix.md)** -- `SourceFile` uninitialised `xform_map` crash; done
- **[stale-fn-gensym-snapshots-on-main.md](history/stale-fn-gensym-snapshots-on-main.md)** -- Off-by-one `__fn_*` gensym in two committed snapshots; regenerated
- **[stdlib-future-linearity-aliasing.md](history/stdlib-future-linearity-aliasing.md)** -- Refcount the shared `FutureCell` so `Future` can be `:affine`; resolved
- **[fn-typed-return-lowered-to-result-type.md](history/fn-typed-return-lowered-to-result-type.md)** -- `defn` returning `(fn [...] T)` mis-lowered to `T`; producer side resolved
- **[nested-closure-transitive-capture.md](history/nested-closure-transitive-capture.md)** -- Two-level nested closures failing to thread grandparent captures; resolved
- **[borrow-param-forwarding-drop.md](history/borrow-param-forwarding-drop.md)** -- LT1 linear-drop check spuriously firing on `^borrow` param forwarding; resolved
- **[defgadt-malformed-pattern-segfault.md](history/defgadt-malformed-pattern-segfault.md)** -- NULL-deref SEGV on malformed `defgadt` constructor; regression fixture in place
- **[bare-fat-sink-poly-box-slot0-int64-mismatch.md](history/bare-fat-sink-poly-box-slot0-int64-mismatch.md)** -- Poly box / bare `^fat` sink slot-0 int64 shim vs `double` invoke cast; fixed (2026-06-04)
- **[poly-wrapper-forces-int64-args-non-int-fat-sink.md](history/poly-wrapper-forces-int64-args-non-int-fat-sink.md)** -- `make_poly_wrapper` forcing int64 arg params on float-class methods; fixed (2026-06-04)
- **[poly-to-fat-drops-args-beyond-first-multiarg-method.md](history/poly-to-fat-drops-args-beyond-first-multiarg-method.md)** -- N-ary `__tur_poly_to_fat*` carriers / shims for multi-arg typeclass methods; fixed
- **[polymorphic-return-type-instantiation-collapses-to-first-tyvar.md](history/polymorphic-return-type-instantiation-collapses-to-first-tyvar.md)** -- Polymorphic accessor return type wrongly collapsed to first tyvar; fixed (2026-06-04)
- **[tuplen-struct-param-passed-by-pointer-codegen-mismatch.md](history/tuplen-struct-param-passed-by-pointer-codegen-mismatch.md)** -- TupleN (N>=3) struct param mis-passed by pointer to by-value callee; fixed (2026-06-04)
- **[typeclass-method-int-carrier-return-truncates-non-int.md](history/typeclass-method-int-carrier-return-truncates-non-int.md)** -- int64-carrier truncating non-int instance results (explicit annotations); fixed (2026-06-04)
- **[closure-first-class-type-plan.md](history/closure-first-class-type-plan.md)** -- First-class closure type (Closure Repr Unification Phase 3, Option B); B-0..B-4 shipped
- **[closure-returning-instance-method-codegen-plan.md](history/closure-returning-instance-method-codegen-plan.md)** -- `definstance` dict-field codegen for closure-returning methods; T1-T6 complete
- **[poly-to-fat-typed-shim-plan.md](history/poly-to-fat-typed-shim-plan.md)** -- Typed `__tur_poly_to_fat1` shim generalisation; capturing-closure path implemented
- **[ptr-generic-parameterised-type-plan.md](history/ptr-generic-parameterised-type-plan.md)** -- Parameterised `:ptr<T>` first-class type; P1-P4 core implemented
- **[sum-types-either-plan.md](history/sum-types-either-plan.md)** -- `Either L R` sum type end-to-end; landed in `d993ba3d`
- **[stdlib-effect-rows-plan.md](history/stdlib-effect-rows-plan.md)** -- Capability effect-row tags on I/O-touching stdlib modules; completed in `cdcf646f`; see [effects-system-guide.md](../guides/effects-system-guide.md)
- **[positional-nominal-type-identity-fix-plan.md](history/positional-nominal-type-identity-fix-plan.md)** -- Type-checker fix to enforce nominal identity on positional struct/opaque/ADT params; landed alongside the partial-application fix
- **[positional-nominal-type-identity-not-checked.md](history/positional-nominal-type-identity-not-checked.md)** -- Bug report behind the above plan; resolved
- **[partial-application-skips-captured-arg-type-check.md](history/partial-application-skips-captured-arg-type-check.md)** -- `elab_partial_apply` capture loop now checks provided arg types; both nominal and kind slices fixed
- **[curried-fn-typed-param-second-application-not-callable.md](history/curried-fn-typed-param-second-application-not-callable.md)** -- Second application of higher-order-returning param now type-checks; fixed (2026-06-04)
- **[arrow-thin-call-segfaults-capturing-closures.md](history/arrow-thin-call-segfaults-capturing-closures.md)** -- `__arrow_call*` thin-call helpers crashing on capturing closures; RESOLVED 2026-06-03 in closure-representation-unification
- **[eq-synthesis-dispatcher-passes-bare-comparator-to-fat-sink.md](history/eq-synthesis-dispatcher-passes-bare-comparator-to-fat-sink.md)** -- Constrained-Eq dispatcher passing bare comparator into `^fat` sink; RESOLVED 2026-06-03
- **[fat-fn-param-capturing-closure-gap.md](history/fat-fn-param-capturing-closure-gap.md)** -- Capturing closure unable to reach an `fn`-typed `^fat` parameter; RESOLVED 2026-06-03
- **[ptr-void-direct-call-representation-split.md](history/ptr-void-direct-call-representation-split.md)** -- Direct call of a `:ptr<void>` callback crashed half the time; RESOLVED 2026-06-03
- **[intra-instance-method-dispatch-unsupported.md](history/intra-instance-method-dispatch-unsupported.md)** -- `(.other self ...)` intra-instance dispatch; FIXED 2026-06-04
- **[poly-defn-shares-inner-closure-body-across-monomorphizations.md](history/poly-defn-shares-inner-closure-body-across-monomorphizations.md)** -- Polymorphic defn shared one inner closure body across specialisations; RESOLVED 2026-06-04 (diagnose direction)
- **[instance-method-closure-return-lowered-to-result-type.md](history/instance-method-closure-return-lowered-to-result-type.md)** -- `definstance` closure-returning method mis-lowered to its result type; RESOLVED 2026-06-04
- **[stdlib-linear-handle-borrows.md](history/stdlib-linear-handle-borrows.md)** -- `^borrow` parameter attribute for non-consuming accessors on linear handles; RESOLVED 2026-06-04
- **[defgadt-copy-and-shared-bounds.md](history/defgadt-copy-and-shared-bounds.md)** -- `defgadt :copy` opt-out for shared GADT values; resolved 2026-06-04

Earlier additions:

- **[frame-spice-plan.md](history/frame-spice-plan.md)** -- `tur-frame` dataframe spice; FR0-FR10 complete; see [frame-guide.md](../guides/frame-guide.md)
- **[module-docstrings-plan.md](history/module-docstrings-plan.md)** -- Module-level `;;;` docstring rendering; implemented and documented in CLAUDE.md
- **[signal-processing-arrows-plan.md](history/signal-processing-arrows-plan.md)** -- Arrow-based DSP tutorial; extracted to `tur-signal` v0.1.0; see [arrows-guide.md](../guides/arrows-guide.md)
- **[spice-aware-check-plan.md](history/spice-aware-check-plan.md)** -- Per-file `tur check` auto-discovers `build.tur`; implemented and documented in CLAUDE.md
- **[test-perf-plan.md](history/test-perf-plan.md)** -- Stamp caching + ccache; T1-A/B/C/D and T2-A/C complete

## Design rationale docs

Design-decision rationale and FAQs live in [../design/](../design/):

- **[typed-slots-gs5-representation-rationale.md](../design/typed-slots-gs5-representation-rationale.md)** -- Why GS5 does not require tagged unions
