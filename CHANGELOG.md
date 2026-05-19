# Changelog

All notable changes to Turmeric are documented here.

## [0.5.0] — 2026-05-19

### Added

- **Session types — binary (SS0–SS4)** (`-Xsessions`) (#38, #36, SS0a–SS3c)
  - `Session[P]` type; `make-session`, `send`, `recv`, `close` channel operations
  - `Choose`/`Branch` for internal/external choice; `choose-left`/`choose-right`/`offer`
  - `Rec` equirecursive protocols (co-inductive equality with seen-set guard)
  - Duality checking (`dual(P)`) and protocol-progress enforcement via linear-type machinery
  - Session delegation (protocol ownership transfer) and session subtyping
  - Typed timeout channels (`recv-timeout`, `TY_TIMEOUT`, `Timeout` protocol constructor)
  - C codegen: `TurChannel` struct; synchronous rendezvous via pthread condvars
  - Debug builds embed initial protocol name as `const char* dbg_proto`
  - Error codes `TUR_E0210`–`TUR_E0212`; `tur explain` entries

- **Session types — multi-party (SS5–SS8)** (`-Xsessions`) (#39, #40, #41, #42)
  - `defprotocol` global protocol declaration with role list and interaction forms
  - `(-> From To MsgType)`, `(choice From [label branch ...])`, `(loop label body)`, `(continue label)`
  - Well-formedness checks (undeclared roles, non-guarded recursion): `TUR_E0223`
  - `(project G R)` type annotation: compile-time projection of a global type onto a role
  - Honda/Yoshida/Carbone projection algorithm (`src/compiler/elab_global.c`); validated by `tools/project.py`
  - Projection failure diagnostic `TUR_E0220`; role/projection mismatch `TUR_E0221`/`TUR_E0222`
  - `make-protocol` allocates N `Role[G, R]` endpoints (one per declared role)
  - `send-to`/`recv-from` route messages through a shared N-party lock-based router
  - Stdlib multi-party templates in `stdlib/session.tur`: `three-way-handshake`, `coordinator`, `ring`
  - Tutorials: Two-Phase Commit and OAuth-Style Auth Flow in `session-types-plan.md`
  - Full user guide: [docs/guides/session-types-guide.md](docs/guides/session-types-guide.md)

- **Dynamic vars (DV0–DV4)** (`-Xdynamic-vars`) (#44)
  - `defdynamic` top-level form declares typed thread-local cells with a root value
  - `binding` form pushes per-thread override frames; cleanup via `__attribute__((cleanup))`
  - Dynamic-var `set!` mutates the current thread's top binding frame
  - `TY_DYNVAR` type kind; `DynVarEntry` struct; codegen using `pthread_key_t` + linked-frame stack
  - `spawn-conveying`: spawn a thread with a snapshot of the parent's current binding frame
  - Stdlib common vars in `stdlib/dynvar.tur`: `*log-level*`, `*locale*`, `*random-seed*`, `*current-module*`
  - Error codes `TUR_E0600`–`TUR_E0605`, `TUR_W0600`; `tur explain` entries
  - Full user guide: [docs/guides/dynamic-vars-guide.md](docs/guides/dynamic-vars-guide.md)

- Datalog database tutorial (#35)

### Fixed
- Scheduler multithread codegen snapshot (#63906e87)

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
