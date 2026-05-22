# Changelog

All notable changes to Turmeric are documented here.

## [0.8.0] — 2026-05-22

### Added

- **Language Server Protocol (LSP)** (#62)
  - `tur lsp`: stdio-based LSP server with hover, go-to-definition, diagnostics, and completion
  - `tur check --json`: machine-readable diagnostic output for editor integration
  - VSCode extension updated to launch `tur lsp` and wire hover/definition providers

- **Sandboxed eval (SB0–SB4)** (#65)
  - `turi_env_new_sandboxed()`: capability-restricted interpreter environment
  - `TuriCaps` bitmask (`TURI_CAP_IO`, `TURI_CAP_FFI`, `TURI_CAP_INLINE_C`, `TURI_CAP_ASYNC`, `TURI_CAP_UNSAFE`, `TURI_CAP_IMPORT`)
  - Step-fuel limit (`turi_env_set_fuel`) and max-depth guard (`turi_env_set_max_depth`)
  - `import` and inline-C call sites blocked in sandboxed environments
  - New API: `turi_env_allow`, `turi_env_deny`, `turi_env_has_cap`

- **Lazy sequences (LZ0–LZ1)** (#63)
  - `Seq[A]` pull-based lazy sequence type with short-circuit support
  - Builders: `seq-from-list`, `seq-from-range`, `seq-repeat`, `seq-iterate`
  - Transforms: `seq-map`, `seq-filter`, `seq-take`, `seq-drop`, `seq-flat-map`, `seq-zip`
  - Consumers: `seq-to-list`, `seq-fold`, `seq-for-each`, `seq-count`, `seq-first`
  - `Range` type with constructors and set-algebra operations (`range-intersection`, `range-gap`, `range-span`, `range-encloses`, etc.)
  - Stdlib in `stdlib/seq/core.tur`, `stdlib/seq/builders.tur`, `stdlib/seq/transform.tur`, `stdlib/seq/consume.tur`, `stdlib/range.tur`

- **9 new stdlib modules** (#64)
  - `stdlib/json.tur` -- JSON parse/emit
  - `stdlib/csv.tur` -- CSV read/write
  - `stdlib/fs.tur` -- filesystem operations
  - `stdlib/path.tur` -- path manipulation
  - `stdlib/env.tur` -- environment variables
  - `stdlib/process.tur` -- subprocess spawning
  - `stdlib/re.tur` -- POSIX regular expressions
  - `stdlib/term.tur` -- terminal control (ANSI colours, cursor)
  - `stdlib/digest.tur` -- hashing (SHA-256, MD5)

- **Doctest framework (D0–D5)** (#58)
  - `tools/doctest.py`: extract and run `;;; Example:` blocks from stdlib docstrings
  - `tools/run-doctests.sh`: stamp-cached test runner with SKIP support for interpreter-incompatible modules
  - `just doctest` target; `just test` extended to include doctests
  - Fixed incorrect docstring examples in `stdlib/math.tur` and `stdlib/async_pipe.tur`

- **Emacs major mode** (#61)
  - `emacs/turmeric-mode.el`: syntax highlighting, indentation, and `M-x turmeric-run` for `.tur` files

- **Spice `subdir` support**
  - Spice manifests now accept a `subdir` key for monorepo sub-packages

- **New guides**
  - Generators guide (`docs/guides/generators-guide.md`) -- generator state machine design and usage
  - CLI args guide (`docs/guides/cli-args-guide.md`) -- structured argument parsing with `stdlib/args.tur`
  - Cloudflare deployment guide (`docs/guides/cloudflare-deployment-guide.md`) -- deploying the web REPL to Cloudflare Pages

## [0.7.0] — 2026-05-21

### Added

- **Sized types (SZ0–SZ1)** (`-Xsized-types`) (#50)
  - `StaticInt` and size arithmetic (`static-int-add`, `static-int-mul`, `static-int-eq`) for phantom size annotations
  - `SizedVec` — length-indexed vector; `sized-vec-new`, `sized-vec-push`, `sized-vec-get`, `sized-vec-set`
  - `SizedBuf` — flat byte/word buffers with heap and stack allocation dispatch; `sized-buf-alloc`, `sized-buf-stack`, `sized-buf-get`, `sized-buf-set`
  - `SizedMatrix` — sized 2-D matrix with row/column shape annotations; `sized-matrix-new`, `sized-matrix-ref`, `sized-matrix-set`
  - `SizedBitVec` — compact bit array with size annotation; `sized-bitvec-new`, `sized-bitvec-get`, `sized-bitvec-set`, `sized-bitvec-popcount`
  - Stdlib in `stdlib/sized.tur`, `stdlib/sized-buf.tur`, `stdlib/sized-matrix.tur`, `stdlib/sized-bits.tur`
  - Full user guide: [docs/guides/sized-types-guide.md](docs/guides/sized-types-guide.md)

- **Literal match patterns** (Phase S4) (#50)
  - Match arms now accept integer, boolean, float, and string literals directly (no wrapping constructor required)
  - Emits an `if`/`else-if` chain for primitive scrutinees; integrates with the existing ADT elaboration path

- **`async-race` and `with-timeout`** in the `turi` interpreter (#50)
  - `async-race`: race two futures; the first to resolve wins and the loser is cancelled
  - `with-timeout`: run a future with a deadline; cancel and throw on expiry

- **Tail call optimization (TCO)** in `turi` interpreter (#50)
  - Self-tail-recursive and mutually-tail-recursive functions no longer grow the call stack
  - Tail calls through `let` bindings and `do` blocks are optimized

- **`catch-unwind`** boundary in `turi` interpreter (#50)
  - `setjmp`/`longjmp` panic boundary exposed as `EX_CATCH_UNWIND`; `panic?` native predicate

- **Weak pointer upgrade returns Option** (#50)
  - `(weak-upgrade r)` now returns `(some value)` on success and `(none)` on dangling — previously returned a raw value or zero

- **Args parser stdlib** (`stdlib/args.tur`) (#50)
  - Builder-pattern CLI argument parsing: flags (`--verbose`), options (`--input=file` or `--input file`), positional args, and arbitrarily nested subcommands
  - `args/spec-new`, `args/spec-flag`, `args/spec-option`, `args/spec-subcmd`, `args/parse`, `args/get`, `args/positional`

- **Math stdlib** (`stdlib/math.tur`) (#50)
  - Thin wrappers around libm: `sqrt`, `fabs`, `floor`, `ceil`, `round`, `pow`, `log`, `log2`, `exp`, `sin`, `cos`, `tan`, `atan2`, `hypot`

- **Bits stdlib** (`stdlib/bits.tur`) (#50)
  - `bit-shr` (unsigned right shift), `println-float` (float with precision), and related bitwise helpers

- **Per-subcommand help strings** (#50)
  - `tur build --help`, `tur run --help`, `tur eval --help`, etc. now print usage for each subcommand

- **Performance comparison suite** (`performance-comparison/`) (#50)
  - Multi-language benchmark harness comparing C, Turmeric (compiled), `turi` (interpreted), Rust, Clojure, Racket, and Python
  - `tur --interpret file.tur` flag runs programs through the tree-walking interpreter without compiling
  - Benchmarks cover numerical computation, data structures, string processing, concurrency, I/O, and recursion

- **Cross-language validation framework** (`validation/`) (#50)
  - Python validation scripts that run the same benchmark across languages and verify identical results
  - `validate_fibonacci.py` checks correctness of Fibonacci across all five languages

- **Tier 3 worker pool** (#45)
  - Persistent interpreter processes for test fixtures; dramatically reduces per-test process-spawn overhead

- **`emit_effects.c`** — effects codegen extracted from `emit_expr.c` into a dedicated translation unit (#50)

- **EAVT / Datalog database tutorial series** (`docs/guides/datalog-*.md`, `examples/datalog/`)
  - Four-part guide: EAVT concepts, minimal implementation, query API, B-tree indexing
  - Five progressive example programs in `examples/datalog/`: `minimal.tur`, `indexed.tur`, `query.tur`, `blog.tur`, `datalog.tur`

- **New and expanded guides**
  - Performance guide (`docs/guides/performance-guide.md`) — numerical, data structures, concurrency, memory, recursion, I/O, benchmarking methodology
  - Sized types guide (`docs/guides/sized-types-guide.md`)
  - Building for the Web with Emscripten (`docs/guides/web-emscripten-tutorial.md`)
  - Structs guide (`docs/guides/structs-guide.md`)
  - Web continuations tutorial (`docs/guides/web-continuations-tutorial.md`)
  - Dual Turmeric / sweet-expression syntax toggle throughout all guides (`check-guide-pairs.py`)
  - Substantially expanded: threading, STM, session types, HKT, tidal/scscm cookbook guides

### Fixed

- ADT match regression: literal match path no longer incorrectly triggers for ADT matches on unannotated parameters
- Memory leaks and stale codegen snapshots from perf-comparison branch
- `weak-dangling` test updated to reflect Option-returning `weak-upgrade`

## [0.6.0] — 2026-05-19

### Changed

- Documentation refresh and cleanup
- Regenerated stdlib API reference

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
