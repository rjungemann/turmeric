# Changelog

All notable changes to Turmeric are documented here.

## [0.14.3] -- 2026-05-28

### Fixed

- **`tur install` global SDK path resolution** -- prefix-installed builds
  (e.g. Homebrew) now resolve absolute `-I`/`-L` paths to the Turmeric SDK
  when compiling spices that use `-lturi`; previously, the relative paths in
  `__tur_autolink__` failed when the working directory was not the source tree.
  Resolution order: `$TUR_SDK_ROOT` override, then walking up from the
  executable to locate `share/turmeric`.

### Changed

- **VSCode syntax extension** -- corrected the Markdown injection grammar so
  Turmeric code fences in Markdown files highlight correctly.

## [0.14.2] -- 2026-05-28

### Fixed

- **`tur install` spice dependency resolution** -- `tur install` now fetches each
  `:spices` dep declared in `build.tur` into the global spice cache and adds their
  `src/` directories as `-I` paths when building binaries; previously, spices with
  transitive deps (e.g. `tur-notebook` depending on `ansi` and `png`) failed to
  build with "module not found" errors.

## [0.14.1] -- 2026-05-28

### Fixed

- **`tur --help` missing package commands** -- `tur install`, `tur uninstall`,
  `tur list`, and `tur upgrade` were dispatched correctly but absent from the
  help output; they now appear under the "package management" section.

### Docs

- **`tur/hash` API reference** -- docstrings for `hash-int` and all
  sized-integer variants (`hash-int8`/16/32/64, `hash-uint8`/16/32/64)
  added to the stdlib API reference and web REPL doc lookup.

## [0.14.0] -- 2026-05-28

### Added

- **ABI specialization (Phases C--I)** -- the compiler now performs typed ABI
  specialization for typeclass methods, concrete structs, and inline-C bodies.
  Integer hashing, bitwise ops, and pass-by-ptr structs use unboxed calling
  conventions (C/D); function-pointer fields in concrete structs are unboxed (E);
  inline-C bodies opt in via `__TUR_TY_<NAME>__` macros (G); typeclass dispatch
  routes directly to the instance implementation rather than going through the
  generic slot (H). New `--emit-abi-trace` flag reports applied specializations (I).

### Changed

- **Web playground promoted to public beta.**

### Fixed

- **Typeclass and stdlib fixes (KB-021--034)** -- GADT HKT constraint unification
  for `equal-cong` (KB-022); typeclass dispatch ABI mismatch for struct-typed
  instances (KB-021); `rc.tur` HKT instances now dispatch on `rc<T>` rather than
  `ptr<void>` (KB-027); orphan checker credits built-in primitive types to their
  home module (KB-030); `session.tur` return-type annotation corrected (KB-029);
  `gvzip-with` in `gadt-vec.tur` now takes a typed function parameter (KB-034).

- **HKT/Clone fixture conflicts** with auto-loaded stdlib typeclasses resolved.

- **macOS Clang compatibility** -- `run.sh` adds `-Wno-error` gates for
  `int-conversion` and `incompatible-function-pointer-types` when building with
  Clang, fixing CI failures on macOS Xcode 15+.

## [0.13.0] -- 2026-05-27

### Added

- **Spice-aware REPL (RP0--RP8)** -- `tur build --shared` emits a dlopen-able `.so`;
  `tur repl` auto-discovers and dlopens the enclosing spice, binds all exports as
  callables at the prompt, and refreshes them via `(reload)` or `--watch`. Error paths
  surface actionable hints for the three most common failure modes.

- **`#rx` regex literals and `#name"body"` reader macros** -- `#rx"pattern"` compiles
  to a regex literal at read time; `#name"body"` is a general reader-macro hook. `re`
  union helpers round out the regex API.

- **Variadic rest parameters** (`& rest :type`) -- functions now accept an unknown number
  of same-type trailing arguments; rest is a cons-list of the declared type and is `nil`
  when absent.

- **Currying** (`curry` macro, effects, rank-2) -- `curry` macro, algebraic effects in
  curried bodies, and rank-2 support (CY3+CY4).

- **Tuple2--Tuple5 built-in structs** -- `Tuple2` through `Tuple5` are now pre-defined
  in the compiler; pointer-type slots are handled correctly.

- **Sized-primitives mixed-width arithmetic (TUR-E0042)** -- mixed-width expressions over
  `i8`/`i16`/`i32`/`i64`/`u8`/`u16`/`u32`/`u64` now elaborate without manual casts.

- **`vec-of` macro** -- construct a `Vec` from a literal element list.

### Changed

- **Contract macros removed; `--no-auto-stdlib` added** -- the old `assert!`/`require!`/
  `ensure!` contract macros are no longer loaded by default; `--no-auto-stdlib` suppresses
  automatic stdlib injection entirely.

- **macOS release binary is now arm64-only** -- the release matrix is
  `linux-x86_64`, `linux-aarch64`, and `macos-arm64`.

### Fixed

- **Aggregate Carrier ABI (ACB Phase 1 + 3)** -- ACB Phase 1 audited call sites for
  struct-carrying aggregates; Phase 3 completes the carrier-to-struct bridge in
  `EX_ASCRIBE` so aggregate returns through ascription work correctly.

- **GADT `Vec` rename, `Clone` typeclass, `Functor` collision** -- `Vec` GADT renamed to
  avoid shadowing the stdlib type; `Clone` typeclass added; `Functor` name collision
  between stdlib and user definitions resolved.

- **Spaced compound type annotations** -- `f : (-> int int)` and compound annotations
  with internal spaces now elaborate correctly through the full pipeline.

- **`?` operator** -- missing `__tur-q-is-err?` and `__tur-q-ok-val` stdlib helpers
  added so the `?` early-return operator functions correctly.

## [0.12.0] -- 2026-05-25

### Changed

- **Typed stdlib modules now use canonical names**
  - The old `t*` module names were dropped in favor of unprefixed modules such as `vec.tur`, `map.tur`, `option.tur`, `result.tur`, `pair.tur`, `list.tur`, `grid.tur`, `zipper.tur`, `set.tur`, and `mutmap.tur`
  - Compiler preloads and synthesized structural-equality helpers were updated to use the new module and helper names
  - Tests, benchmarks, and generated docs were refreshed to use the unprefixed APIs throughout

- **Docs layout cleanup**
  - `drop-typed-prefix-plan.md` moved into `docs/archive/`
  - Several roadmap docs were promoted out of `docs/upcoming/` into the main `docs/` tree

## [0.11.0] -- 2026-05-25

### Added

- **`defalias` for primitive type aliases**
  - New `(defalias Name :primitive-type)` form for defining named aliases of primitive types such as `:int` and `:float`
  - Function parameter and return annotations now resolve these aliases during elaboration
  - Added coverage for basic aliases, float aliases, and invalid alias targets

- **New guides**
  - `frame-guide.md` -- using the `tur-frame` spice for in-memory columnar dataframes and Arrow interop
  - `tur-logic-guide.md` -- miniKanren-style relational programming with `tur/logic`

### Changed

- Documentation archive reorganized under `docs/archive/` and `docs/archive/history/`, with the archive index refreshed
- The miniKanren tutorial was renamed to `minikanren-1-relations-and-queries.md` to make room for a multi-part guide series

## [0.10.0] -- 2026-05-25

### Fixed

- Higher-order function return types now preserve their full `TY_FN` payload through elaboration and calls, so let-bound functions returned from other functions remain callable instead of degrading to a non-callable shell type.
- The signal spice no longer needs `requires.typecheck-skip`; `signal/core.tur`, `signal/dsp.tur`, `signal/envelope.tur`, and `signal/synth.tur` all typecheck cleanly against the current compiler and signal API surface.

## [0.9.0] -- 2026-05-22

### Added

- **LSP intelligence (LD0-LD4)** (#67)
  - `textDocument/hover` -- returns a Markdown snippet with the symbol's type signature and `;;;` docstring
  - `textDocument/definition` -- returns the source location for the symbol under the cursor
  - `textDocument/completion` -- prefix-filtered `CompletionItem` list from the symbol index; also completes stdlib module paths inside `(import ...)` forms
  - New LSP helpers: `lsp_scan_docs` (re-scans `;;;` blocks without invoking the compiler), `tur_collect_symbols` (elaboration-based symbol index)

- **`Show` typeclass (SI0-SI2)** (#68)
  - `Show [bool]` -- returns `"true"` or `"false"`
  - Fixed `Display [int]` -- was `"<int>"`, now decimal via `snprintf %lld`
  - Fixed `Debug [int]` -- was `"<int>"`, now `"int(N)"` format
  - Fixed `Display/Debug [ptr<void>]` -- correct `ok(N)` / `err(N)` / `Result::ok(N)` / `Result::err(N)` formatting
  - `derive-show` macro: generates a `Show` instance for any struct from a field descriptor list
  - New compile-time built-ins powering `derive-show`: `vec?`, `symbol-name`, `dot-sym`, `str-append`

- **`name : type` annotations** -- the parser now accepts a space before the colon (`name : type`) in addition to the existing `name :type` form

- **Datum comments** (`#;`) -- `#;expr` suppresses the next form without removing it from the source, matching standard Scheme/Racket convention

- **Sweet-expression syntax** (`#lang sweet-exp`) -- opt-in indentation-sensitive syntax; `#lang sweet-exp` at the top of a file (or a `.tursweet` extension) enables full t-expression + neoteric + curly-infix mode

- **Devcontainer** -- `.devcontainer/devcontainer.json` for one-click VS Code Remote / GitHub Codespaces setup

- **Spices directory page** -- `tools/genspices.py` generates `docs/html/spices/index.html` from the `turmeric-spices` README (local `../turmeric-spices/` or GitHub fallback) with full Turmeric syntax highlighting; `just spices` runs it; `just docs` now depends on it

- **New guides**
  - `lsp-guide.md` -- setting up the LSP server with VS Code, Neovim, and Emacs
  - `advanced-type-system-rationale.md` -- design rationale for HKTs, GADTs, session types, and sized types
  - `arrows-guide.md` -- composable `Arrow` abstractions and the `>>>` / `***` / `&&&` combinators
  - `sandboxing-guide.md` -- capability-restricted interpreter environments and step-fuel limits

### Fixed

- LSP server: fixed a crash when the client sent a request before the workspace root was known
- `Display [int]` / `Debug [int]` / `Display/Debug [ptr<void>]` now produce correct output (see Show typeclass above)

### Changed

- Guide code examples reformatted throughout to follow the new Clojure-style indentation rules and sweet-exp style guide (Guides, API Docs, and Spices pages all updated)
- Documentation reorganised: several plan documents moved to `docs/archive/`

## [0.8.0] -- 2026-05-22

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

## [0.7.0] -- 2026-05-21

### Added

- **Sized types (SZ0–SZ1)** (`-Xsized-types`) (#50)
  - `StaticInt` and size arithmetic (`static-int-add`, `static-int-mul`, `static-int-eq`) for phantom size annotations
  - `SizedVec` -- length-indexed vector; `sized-vec-new`, `sized-vec-push`, `sized-vec-get`, `sized-vec-set`
  - `SizedBuf` -- flat byte/word buffers with heap and stack allocation dispatch; `sized-buf-alloc`, `sized-buf-stack`, `sized-buf-get`, `sized-buf-set`
  - `SizedMatrix` -- sized 2-D matrix with row/column shape annotations; `sized-matrix-new`, `sized-matrix-ref`, `sized-matrix-set`
  - `SizedBitVec` -- compact bit array with size annotation; `sized-bitvec-new`, `sized-bitvec-get`, `sized-bitvec-set`, `sized-bitvec-popcount`
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
  - `(weak-upgrade r)` now returns `(some value)` on success and `(none)` on dangling -- previously returned a raw value or zero

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

- **`emit_effects.c`** -- effects codegen extracted from `emit_expr.c` into a dedicated translation unit (#50)

- **EAVT / Datalog database tutorial series** (`docs/guides/datalog-*.md`, `examples/datalog/`)
  - Four-part guide: EAVT concepts, minimal implementation, query API, B-tree indexing
  - Five progressive example programs in `examples/datalog/`: `minimal.tur`, `indexed.tur`, `query.tur`, `blog.tur`, `datalog.tur`

- **New and expanded guides**
  - Performance guide (`docs/guides/performance-guide.md`) -- numerical, data structures, concurrency, memory, recursion, I/O, benchmarking methodology
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

## [0.6.0] -- 2026-05-19

### Changed

- Documentation refresh and cleanup
- Regenerated stdlib API reference

## [0.5.0] -- 2026-05-19

### Added

- **Session types -- binary (SS0–SS4)** (`-Xsessions`) (#38, #36, SS0a–SS3c)
  - `Session[P]` type; `make-session`, `send`, `recv`, `close` channel operations
  - `Choose`/`Branch` for internal/external choice; `choose-left`/`choose-right`/`offer`
  - `Rec` equirecursive protocols (co-inductive equality with seen-set guard)
  - Duality checking (`dual(P)`) and protocol-progress enforcement via linear-type machinery
  - Session delegation (protocol ownership transfer) and session subtyping
  - Typed timeout channels (`recv-timeout`, `TY_TIMEOUT`, `Timeout` protocol constructor)
  - C codegen: `TurChannel` struct; synchronous rendezvous via pthread condvars
  - Debug builds embed initial protocol name as `const char* dbg_proto`
  - Error codes `TUR_E0210`–`TUR_E0212`; `tur explain` entries

- **Session types -- multi-party (SS5–SS8)** (`-Xsessions`) (#39, #40, #41, #42)
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

## [0.4.0] -- 2026-05-17

### Added
- Algebraic effects with delimited continuations and handler syntax (#25)
- WASM threads planning and infrastructure

## [0.3.2] -- 2026-05-17

### Changed
- Syntax highlighter improvements
- Documentation cleanup and reorganisation

## [0.3.1] -- 2026-05-17

### Changed
- Homepage layout and copy updates
- Documentation improvements

## [0.3.0] -- 2026-05-17

### Added
- GADTs -- generalised algebraic data types with full elaboration and codegen support (#24)
- Linear types -- linearity constraints enforced by the type system

### Fixed
- Homepage horizontal scroll on mobile (#23)

## [0.2.0] -- 2026-05-16

### Added
- Package manager -- CPM-based dependency management (#22)
- Effect rows -- row-polymorphic effect types
- Substructural and uniqueness types
- Linear types (initial support)
- "Solve this" button in the web REPL

## [0.1.0] -- 2026-05-14

### Added
- Higher-ranked types -- rank-N polymorphism (#18)
- GADTs -- initial implementation (#21)
- Arrows -- generalised computation abstractions
- Structural equality -- deep equality for all types (#17)
- Serialisable continuations -- capture and restore delimited continuations (#16)
- Contracts -- `require!`, `ensure!`, `invariant!`, and `assert!` macros
- Set literals -- `#s(...)` reader syntax
- HAMTs -- persistent hash-array-mapped tries
- STM -- software transactional memory
- Comonads -- comonad typeclass and standard instances
- Numeric types -- fixed-width integers and floats with explicit cast operators
- Auto-formatter -- source code pretty-printer
- Async/await -- fibre-based structured concurrency (#10)
- Unsafe effects -- escape-hatch unsafe block form (#11)
- HKT -- higher-kinded types, kind inference, and type application syntax
- Docstrings -- `;;;` doc-comment standard and `doc` macro
- Markdown Turmeric block syntax highlighting in the web REPL

## [0.0.4] -- 2026-05-09

### Added
- Algebraic effects infrastructure (v1) with delimited continuations
- Capability-passing effects (v1 effect system)
- Exceptions -- `try`/`throw` via `setjmp`/`longjmp`
- Defer expressions -- unified runtime-list-on-frame model
- Multi-file support and mutual recursion across files
- Async/await foundation -- fibre context switching (x64/arm64 asm)
- Compile-time macro evaluation via procedural elaboration
- `cond` as a variadic `defmacro` (removed built-in `elab_cond`) (#4)
- miniKanren-style logic programming example and guide
- Snake game example project
- Phases 0–19 of the core compiler (parsing, elaboration, CPS lowering, codegen)
