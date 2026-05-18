# Changelog

All notable changes to Turmeric are documented here.

## [0.4.0] — 2026-05-17

### Added
- Algebraic effects with delimited continuations and handler syntax (#25)
- WASM threads planning and infrastructure

## [0.3.2] — 2026-05-17

### Changed
- Syntax highlighter improvements
- Documentation cleanup and reorganisation

## [0.3.1] — 2026-05-17

### Changed
- Homepage layout and copy updates
- Documentation improvements

## [0.3.0] — 2026-05-17

### Added
- GADTs — generalised algebraic data types with full elaboration and codegen support (#24)
- Linear types — linearity constraints enforced by the type system

### Fixed
- Homepage horizontal scroll on mobile (#23)

## [0.2.0] — 2026-05-16

### Added
- Package manager — CPM-based dependency management (#22)
- Effect rows — row-polymorphic effect types
- Substructural and uniqueness types
- Linear types (initial support)
- "Solve this" button in the web REPL

## [0.1.0] — 2026-05-14

### Added
- Higher-ranked types — rank-N polymorphism (#18)
- GADTs — initial implementation (#21)
- Arrows — generalised computation abstractions
- Structural equality — deep equality for all types (#17)
- Serialisable continuations — capture and restore delimited continuations (#16)
- Contracts — `require!`, `ensure!`, `invariant!`, and `assert!` macros
- Set literals — `#s(...)` reader syntax
- HAMTs — persistent hash-array-mapped tries
- STM — software transactional memory
- Comonads — comonad typeclass and standard instances
- Numeric types — fixed-width integers and floats with explicit cast operators
- Auto-formatter — source code pretty-printer
- Async/await — fibre-based structured concurrency (#10)
- Unsafe effects — escape-hatch unsafe block form (#11)
- HKT — higher-kinded types, kind inference, and type application syntax
- Docstrings — `;;;` doc-comment standard and `doc` macro
- Markdown Turmeric block syntax highlighting in the web REPL

## [0.0.4] — 2026-05-09

### Added
- Algebraic effects infrastructure (v1) with delimited continuations
- Capability-passing effects (v1 effect system)
- Exceptions — `try`/`throw` via `setjmp`/`longjmp`
- Defer expressions — unified runtime-list-on-frame model
- Multi-file support and mutual recursion across files
- Async/await foundation — fibre context switching (x64/arm64 asm)
- Compile-time macro evaluation via procedural elaboration
- `cond` as a variadic `defmacro` (removed built-in `elab_cond`) (#4)
- miniKanren-style logic programming example and guide
- Snake game example project
- Phases 0–19 of the core compiler (parsing, elaboration, CPS lowering, codegen)
