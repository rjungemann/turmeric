# Programming Turmeric -- Book Outline Plan

NOTE: This plan is up-to-date with turmeric v0.16.0. If the version is later than this, then revisit the features and adjust the outline accordingly.

Inspired by *Programming Ruby* (the Pickaxe), adapted for Turmeric's character:
functional-first, Lisp syntax, typeclasses, linear types, effects, inline C,
and a WASM-ready runtime. The arc is the same -- tour, ecosystem, deep-dive,
reference -- but the chapters diverge wherever Turmeric's design demands it.

---

## Preface

- Why Turmeric?
  - Functional, embeddable, WASM-ready
  - Escape hatch to C when you need it
  - Capability-based I/O model
- Notation conventions
  - S-expression syntax
  - `;;; docstring` format
  - `; => result` for expected output in examples
- Road Map
- Resources

---

## Part I -- Facets of Turmeric *(tutorial)*

### Chapter 1 -- Getting Started

- Building from source (`just build`)
- Running a script: `turi myscript.tur`
- The interactive REPL (command line)
- The web REPL (WASM, Monaco editor)
- Hello, World

### Chapter 2 -- Turmeric.new

A fast lap around the language to give readers enough footing for the rest of
the book.

- Everything is an expression; the value of a form is always defined
- Atoms: integers, booleans, `nil-value`, `cstr`, `ptr`
- Calling functions: `(f a b)` prefix syntax
- Defining functions: `defn`, return types, arity
- `let` for local bindings, `do` for sequential evaluation
- Printing: `println`, `print`
- Commenting: `;` line comments and `;;;` doc-comments
- Reading command-line arguments via `*args*`

### Chapter 3 -- Structs, Types, and Values

- Primitive types: `:int`, `:bool`, `:cstr`, `:ptr`, `:void`
- Defining structs with `defstruct`
- Field access and construction
- Type annotations on function parameters and return types
- Value vs. pointer semantics
- Introducing linear structs (`:linear`) -- full treatment in Part III

### Chapter 4 -- Lists, Options, and Results

- Singly-linked lists: `cons`, `nil-value`, `head`, `tail`
- `option`: `some` / `none` for nullable values
- `result`: `ok` / `err` for fallible operations
- `vec`: growable arrays
- `map` / `hamt`: persistent hash maps
- Choosing the right container

### Chapter 5 -- Functions, Macros, and Pattern Matching

- First-class functions and lambdas (`fn`)
- Higher-order functions: `map`, `filter`, `fold`
- `defmacro` and quasiquoting: writing code that writes code
- Core macros from `stdlib/macros.tur`: `cond`, `when`, `for`, `do-m`, `doc`
- Pattern matching with `match` and destructuring
- Guards in match arms

### Chapter 6 -- Typeclasses and Polymorphism

- What a typeclass is and why it is not a class
- `definstance` and dispatch
- Built-in typeclasses: `Equal`, `Comparable`, `Functor`, `Monad`
- Writing your own typeclass
- How this differs from Ruby mixins and Java interfaces

### Chapter 7 -- Error Handling and Contracts

- Chaining `result` values without nesting
- When to use `option` vs. `result`
- `contract.tur`: `assert!`, `require!`, `ensure!`
- Runtime contracts vs. type-level guarantees
- Propagation patterns: early-exit with `do-m`

### Chapter 8 -- I/O and Side Effects

- Printing and reading stdin
- File I/O with `io.tur`: open, read, write, close
- The capability pattern: passing I/O handles rather than using globals
- Async I/O overview: `async_file.tur`, `async_pipe.tur`

### Chapter 9 -- Concurrency

One of Turmeric's strongest suits; gets a full tutorial chapter.

- Fibers and cooperative multitasking (`fiber.tur`)
- OS threads (`thread.tur`, `threadpool.tur`)
- Channels and CSP-style message passing (`chan.tur`)
- Software Transactional Memory (`stm.tur`)
- Async sockets (`async_socket.tur`)
- When to use each model

### Chapter 10 -- Testing

- `test.tur`: `assert`, `assert-true`, `assert-false`, `assert-eq`
- Writing fixture files (ASCII-only rule; the reason why)
- Running tests with `just test`
- Test organization: files, suites, naming

---

## Part II -- Turmeric in Its Setting

### Chapter 11 -- From the Command Line

- Invoking `turi`: options and flags
- `*args*` and the `stdlib/args.tur` structured parser
  - Flags (`--verbose`), options (`--output file`), positional args, subcommands
- Environment variables with `env.tur`
- Writing shebang scripts

### Chapter 12 -- Processes and the OS

- Spawning subprocesses: `process/spawn`, `process/run`, `process/capture`
- Pipes and capturing stdout/stderr
- `process/exec` for replacing the current process
- Signal handling with `signal/`
- Exit codes and `process/exit`

### Chapter 13 -- File System and Paths

- Pure path operations with `path.tur`: join, basename, dirname, extension, normalize
- Directory operations: `fs/mkdir`, `fs/mkdirp`, `fs/rmdir`, `fs/glob`
- File metadata: `fs/stat`, `fs/exists?`, `fs/file?`, `fs/dir?`
- Convenience: `fs/read-text`, `fs/write-text`, `fs/tmpfile`

### Chapter 14 -- Networking

- TCP sockets with `net.tur` and `async_socket.tur`
- A simple HTTP client from scratch
- The capability model applied to network handles

### Chapter 15 -- Building and Tools

- The `just` build system: `build`, `test`, `release`, `docs`, `wasm`
- The `gendocs.py` pipeline: `;;;` → HTML → `docstrings.tur`
- `turi_doc_lookup` and the runtime doc table
- Keeping `stdlib/docstrings.tur` up to date (it is auto-generated; do not edit)

### Chapter 16 -- The Web REPL and WebAssembly

- The Monaco-based REPL in `web/`
- How `turi_doc_lookup` powers the doc panel
- Building the WASM module with `just wasm`
- The `web/public/turmeric.wasm` artifact and `turmeric.js` glue
- Embedding Turmeric in a web page

---

## Part III -- Turmeric Crystallized *(advanced)*

### Chapter 17 -- The Inline-C Escape Hatch

- Syntax: the `` ```c ... ``` `` block inside a `defn`
- Declaring external functions with `extern-c`
- The closing-paren style rule: `` ```) `` on the same line (and why)
- Marshaling values across the boundary: `:int` for pointers, `:cstr`, `:ptr`
- When to reach for inline C vs. keeping things pure Turmeric

### Chapter 18 -- Linear and Substructural Types

- What "linear" means: a value must be consumed exactly once
- `lref`, move semantics, and the `TUR-E0100` diagnostic
- Linear structs in practice: `Socket`, `FileHandle`, `MutexGuard`
- Affine vs. linear: the substructural lattice
- Uniqueness types (`:unique`) and how they differ from linearity
- Patterns for guaranteed cleanup without `try/finally`
- *See also: `substructural-types-guide.md`, `uniqueness-types-guide.md`*

### Chapter 18a -- Contract and Refinement Types

- Contract types: `{ x : T | p }` syntax and the `-Xcontracts` flag
- Predicate-as-`Form` storage; runtime checks at boundaries
- Refinement types (`-Xrefinements`): the compiler proves predicates statically
- Worked example: ridding a codebase of defensive `require!` guards
- Limits: what the prover can and cannot discharge; SMT fallback
- *See also: `contract-types-guide.md`, `upcoming/refinement-types-plan.md`*

### Chapter 18b -- Union, Intersection, and Existential Types

- Union types (`(or T1 T2)`): typed sums without a wrapping ADT
- Intersection types (`(and T1 T2)`): combining capabilities
- Narrowing in `match` and conditional contexts
- Existential types (`exists T. ...`): hiding implementation type
- Existential vectors and heterogeneous collections (`vec-existential.tur`)
- Type erasure: when to forget structure on purpose
- *See also: `union-intersection-types-guide.md`, `existential-types-guide.md`, `type-erasure-guide.md`*

### Chapter 18c -- GADTs and Higher-Rank Types

- Generalized Algebraic Data Types: indexing constructors by their result type
- Worked example: a tag-safe expression evaluator
- `gadt-vec.tur`: length-indexed vectors as a GADT in stdlib
- Higher-rank types (HRT): `forall a. ...` inside a parameter position
- When HRT lets you write APIs you cannot write with prenex polymorphism
- *See also: `gadts-guide.md`, `gadts-cookbook.md`, `hrt-guide.md`*

### Chapter 18d -- Sized Types

- The motivation: tracking dimensions in the type system
- Sized primitives: `:i8`, `:i16`, `:i32`, `:i64`, `:u8`-`:u64`, `:f32`, `:f64`
- Sized containers: `sized.tur`, `sized-buf.tur`, `sized-matrix.tur`, `sized-bits.tur`
- Length-indexed APIs: catch off-by-one at compile time
- Interop with linalg, frames, and FFI buffers
- *See also: `sized-types-guide.md`, `sized-primitives-guide.md`*

### Chapter 19 -- Metaprogramming with Macros

- Deep dive into `defmacro`: quasiquote, unquote, splicing
- Hygiene and gensym
- Writing a DSL inside Turmeric: a worked example
- Limits of macro metaprogramming vs. runtime reflection

### Chapter 20 -- Fixed Points, Free Monads, and Higher-Kinded Types

- The `fix.tur` fixed-point type: recursive types without recursion in the type
- Catamorphisms (`cata`) and anamorphisms (`ana`)
- The Free Monad (`free.tur`): `free-pure`, `lift`, `bind`, `fmap`, `run`
- Higher-kinded types via `typeclass.tur`
- When these abstractions pay off

### Chapter 21 -- Session Types

- Protocol-safe communication with `session.tur`
- Dual endpoints: `send!` on one side requires `recv!` on the other
- Encoding a client/server handshake as a type
- Static guarantees vs. runtime overhead

### Chapter 22 -- Dynamic Variables

- `dynvar.tur`: thread-local dynamic binding without global state
- `dynvar/bind`, `dynvar/get`, `dynvar/set`
- Practical patterns: logging context, request-scoped config, capability threading

### Chapter 23 -- Effects and Capabilities

- The effects model in `effects.tur`
- Capability-passing style: explicit I/O capabilities vs. global side effects
- Writing effect handlers
- Composing effects

---

## Part III.5 -- Data, Plots, and Notebooks *(applied)*

The book pivots here from language internals to the spice ecosystem that makes
Turmeric usable for day-to-day data work. Each chapter focuses on one spice
from `../turmeric-spices/` and how it composes with the others.

### Chapter 23a -- Dataframes with `tur-frame`

- Columnar in-memory dataframes modeled on R's `data.frame` and pandas
- Backed by Apache Arrow's C Data Interface for zero-copy interop with
  Python, R, DuckDB, and Polars
- Building frames: `frame-from-cols`, `frame-from-rows`
- Columns and types: `column-int64`, `column-float64`, `column-utf8`,
  `type-int64`, `type-float64`, `type-utf8`
- Selecting and projecting: `select-cols`, `drop-cols`, `rename`,
  `with-col`, `map-col`, `mutate`
- Filtering, sorting, sampling: `filter`, `drop-nulls`, `distinct`,
  `sample`, `arrange`
- Grouping and aggregation: `group-by`, `agg-sum`, `agg-mean`, `agg-count`,
  `agg-min`, `agg-max`, `summarize`
- Joins: `inner-join`, `left-join`, `join`
- Reshape: `melt`
- *See also: `frame-guide.md`*

### Chapter 23b -- Statistics and Formulas

- `tur-stats`: descriptive statistics, OLS, t-tests, on `tur-frame` inputs
- Wilkinson-style formula DSL (`y ~ x1 * x2 + I(x3^2)`) via `tur-stats-formula`
- Pairing formulas with `frame-from-cols` outputs
- *See also: `stats-guide.md`, `upcoming/stats-formula-plan.md`*

### Chapter 23c -- Plotting with `tur-plot`

- Tier-1 spice for 2D data visualization, rendered via `tur-plutovg`
- Function plots, scatter points, intervals, histograms, contours
- Legends, axes, titles
- Output paths: PNG file or a plutovg surface for further composition
- Pairing plots with frames: `plot-frame-col`, sampled curves from typed `defn`s
- Composing several plots into one image with `plot-into-canvas`
- *See also: `tur-plot` spice README*

### Chapter 23d -- Generating Images

The image-generation stack is a small, composable set of spices: draw with
plutovg, read/write pixels with png, and (optionally) generate procedural
shapes with SDFs.

- **`tur-plutovg`** -- 2D vector graphics engine (the same that backs LunaSVG
  and PlutoSVG): canvases, paths, fills, strokes, gradients, patterns, fonts,
  PNG export. Good for SVG-style rendering, icons, plotting backends, and
  print-quality output.
- **`tur-png`** -- libpng wrapper for `png-read` / `png-write`, plus per-pixel
  RGBA accessors. The pixel-level I/O layer that pairs naturally with
  `tur-plutovg` for generative art.
- **`tur-sdf-raylib`** -- signed-distance-field primitives plus marching-cubes
  meshing, rendered through raylib. Phase 1 ships CPU SDF + MC; Phase 2 adds
  colored SDFs.
- **`tur-raylib`** / **`tur-raygui`** -- real-time windowed graphics and an
  immediate-mode GUI on top.
- **`tur-opengl`** / **`tur-glsl`** -- low-level GPU access for shader-driven
  generation.
- Worked example: rendering a parameterized poster with plutovg, exporting
  it via png, and re-using the same renderer headlessly in a notebook.

### Chapter 23e -- Notebooks with `tur-notebook`

- Literate `.tur.md` notebooks -- Markdown prose with `turmeric` code fences
- Session-backed evaluation: each cell runs in a persistent evaluator
- The TUI (`tur nb tui`): cell editing, insert/delete/paste, dirty-save
  quit handling, search (`/`, `n`, `N`), help overlay (`?`),
  focused-output toggling (`o`), user keybinding overrides
- Standalone HTML export with the vendored docs-site stylesheet
- The `exec` subcommand for scripted, headless notebook execution
- The image-hook display system: returning a plutovg/png artifact from a
  cell renders it inline
- Pairing notebooks with frames + plot + plutovg for end-to-end data
  notebooks
- *See also: `notebook-guide.md`*

---

## Part IV -- Language Reference

### Chapter 24 -- Syntax Reference

- Source layout and the reader
- Atom types and their literals
- Special forms, A-Z:
  - `defn`, `defmacro`, `defstruct`, `definstance`
  - `let`, `do`, `if`, `match`, `fn`
  - `extern-c`, `import`
  - Inline-C block syntax
- The `;;;` docstring format in full

### Chapter 25 -- Type System Reference

- Primitive types: `:int`, `:bool`, `:cstr`, `:ptr`, `:void`
- Sized primitives: `:i8`/`:i16`/`:i32`/`:i64`, `:u8`-`:u64`, `:f32`, `:f64`
- Struct field syntax and field access notation
- Substructural qualifiers: `:linear`, `:unique`, lref, move semantics, `TUR-E0100`
- Contract types `{ x : T | p }` (`-Xcontracts`)
- Refinement types and static predicate proof (`-Xrefinements`)
- Union (`(or T1 T2)`) and intersection (`(and T1 T2)`) types
- Existential types and type erasure
- GADT constructors and HRT (`forall`) parameter positions
- Sized type constructors and length indexing
- Type annotations on `defn` parameters and return types
- Typeclass constraints and higher-kinded type variables

### Chapter 26 -- Macro Reference

- All macros in `stdlib/macros.tur`, with signatures and examples
- `cond`, `when`, `unless`, `for`, `do-m`, `doc`, and others

---

## Part V -- Standard Library Reference

### Chapter 27 -- Core Data Types

- `list.tur` -- singly-linked lists
- `option.tur` -- optional values
- `result.tur` -- error handling
- `pair.tur` -- generic two-element pairs
- `tuple.tur` -- fixed-arity tuples
- `str.tur` -- UTF-8 string views
- `vec.tur` -- growable arrays
- `set.tur` -- persistent sets
- `map.tur` / `hamt.tur` -- persistent hash maps
- `mutmap.tur` -- mutable hash map
- `ref.tur` / `rc.tur` -- references and reference-counted boxes
- `hash.tur` -- general-purpose hashing
- `equal.tur` -- structural equality
- `range.tur` / `float-range.tur` -- integer and float ranges

### Chapter 28 -- Collections and Algorithms

- `slice.tur` -- array slices
- `zipper.tur` -- list zippers for cursor-style traversal
- `gadt-vec.tur` -- length-indexed vectors
- `vec-existential.tur` -- existentially-typed heterogeneous vectors
- `bits.tur` / `sized-bits.tur` -- bitset operations
- `sized.tur` / `sized-buf.tur` / `sized-matrix.tur` -- size-typed containers
- `grid.tur` -- 2D grids
- `parsec.tur` -- parser combinators
- `re.tur` -- regular expressions
- `backtrack.tur` -- backtracking search
- `logic.tur` -- miniKanren-style relational programming
- `gen.tur` -- generators
- `csv.tur` -- CSV reading and writing
- `json.tur` -- JSON parsing and serialization
- `schema.tur` -- structural data validation
- `digest.tur` -- cryptographic digests

### Chapter 29 -- I/O, Files, and Networking

- `io.tur` -- low-level file I/O and directory listing
- `async_file.tur` -- non-blocking file operations
- `async_pipe.tur` -- non-blocking pipes
- `async_socket.tur` -- non-blocking sockets
- `net.tur` -- Socket linear type and helpers
- `env.tur` -- environment variables *(proposed)*
- `process.tur` -- process management *(proposed)*
- `path.tur` -- path string operations *(proposed)*
- `fs.tur` -- high-level file system operations *(proposed)*

### Chapter 30 -- Concurrency and Synchronization

- `thread.tur`, `threadpool.tur` -- OS threads and pools
- `fiber.tur` -- cooperative coroutines
- `chan.tur` -- typed channels
- `stm.tur` -- software transactional memory
- `mutex.tur`, `condvar.tur`, `rwlock.tur` -- classical synchronization
- `atomic.tur` -- atomic integers
- `sync.tur` -- barrier and once primitives
- `future.tur` -- deferred values
- `scheduler.tur`, `scheduler_mt.tur` -- work-stealing scheduler
- `taskgroup.tur` -- structured concurrency groups
- `select.tur` -- multiplexed channel select

### Chapter 31 -- System and Utilities

- `args.tur` -- CLI argument parser
- `env.tur` -- environment variables
- `process.tur` -- process management
- `path.tur` / `fs.tur` -- paths and high-level filesystem
- `signal/` -- POSIX signal handling
- `time.tur` / `timer.tur` -- wall clock and timers
- `math.tur` -- numeric functions
- `random.tur` -- random number generation
- `log.tur` -- structured logging
- `term.tur` -- terminal control and TUI primitives
- `serial.tur` -- serializable continuations
- `workflow.tur` -- durable workflows
- `reactor.tur` -- event-loop reactor
- `httpd.tur` -- embedded HTTP server

### Chapter 32 -- Advanced Type Machinery

- `fix.tur` -- fixed-point of a functor
- `free.tur` -- Free Monad
- `arrow.tur` -- Arrow abstraction
- `comonad.tur` -- Comonad abstraction
- `nat.tur` -- type-level naturals
- `typeclass.tur` -- typeclass dispatch infrastructure
- `typeclass-eq.tur` / `typeclass-clone.tur` / `typeclass-functor.tur` -- canonical instances
- `existential.tur` -- existential type machinery
- `safe.tur` -- safe-cast helpers for substructural types
- `session.tur` -- session types
- `dynvar.tur` -- dynamic variables
- `effects.tur` -- algebraic effects
- `capability.tur` -- capability structs
- `contract.tur` -- runtime assertions

---

## Part VI -- Spice Ecosystem Reference

A short reference index of the first-party spices in `../turmeric-spices/`,
cross-referenced from the applied chapters in Part III.5.

### Chapter 33 -- Data and Analytics Spices

- `tur-frame` -- columnar dataframes, Arrow C Data Interface
- `tur-stats` -- statistics on frames
- `tur-stats-formula` -- Wilkinson-style formula DSL *(planned)*
- `tur-linalg` -- linear algebra
- `tur-math` -- extended math routines

### Chapter 34 -- Graphics and Image Spices

- `tur-plutovg` -- 2D vector graphics (paths, fills, gradients, text, PNG)
- `tur-png` -- libpng read/write with per-pixel access
- `tur-plot` -- 2D plotting on top of plutovg
- `tur-sdf-raylib` -- signed-distance fields with raylib rendering
- `tur-raylib` / `tur-raygui` -- real-time windowed graphics and immediate-mode GUI
- `tur-opengl` / `tur-glsl` -- low-level GPU and shader access

### Chapter 35 -- Notebooks and Tooling Spices

- `tur-notebook` -- `.tur.md` literate notebooks (TUI, HTML export, exec mode)
- `tur-template` -- text templating
- `tur-watch` -- file-watcher driven workflows
- `tur-test` -- extended test runner

### Chapter 36 -- I/O and Integration Spices

- `tur-httpd` / `tur-http` / `tur-tls` -- HTTP server, client, TLS
- `tur-postgres` / `tur-sqlite` / `tur-valkey` -- databases
- `tur-json` / `tur-regex` / `tur-ansi` -- text and protocol helpers
- `tur-osc` / `tur-rtmidi` / `tur-rtaudio` / `tur-wav` / `tur-tidal` / `tur-signal` -- audio and music
- `tur-c-dsl` -- declarative inline-C DSL
- `tur-tourist` -- guided-tour packaging for spices

---

## Appendices

- **A: Building from Source** -- prerequisites, `just build`, release vs. debug
- **B: Inline-C Quick Reference** -- type mapping table, common patterns
- **C: `just` Target Reference** -- all targets and what they do
- **D: Docstring Format Cheat Sheet** -- `;;;` required fields, conventions, ASCII rule
- **E: Language Changes by Phase** -- B1 through present, feature timeline
