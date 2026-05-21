# Programming Turmeric -- Book Outline Plan

NOTE: This plan is up-to-date with tumeric v0.7.0. If the version is later than this, then revisit the features and adjust the outline accordingly.

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

### Chapter 18 -- Linear Types and Resource Safety

- What "linear" means: a value must be consumed exactly once
- `lref`, move semantics, and the `TUR-E0100` diagnostic
- Linear structs in practice: `Socket`, `FileHandle`, `MutexGuard`
- Patterns for guaranteed cleanup without `try/finally`

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
- Struct field syntax and field access notation
- Linear types: `:linear`, lref, move semantics, `TUR-E0100`
- Type annotations on `defn` parameters and return types
- Typeclass constraints

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
- `str.tur` -- UTF-8 string views
- `vec.tur` -- growable arrays
- `map.tur` / `hamt.tur` -- persistent hash maps

### Chapter 28 -- Collections and Algorithms

- `slice.tur` -- array slices
- `zipper.tur` -- list zippers for cursor-style traversal
- `gadt-vec.tur` -- length-indexed vectors
- `bits.tur` / `sized-bits.tur` -- bitset operations
- `sized.tur` / `sized-buf.tur` / `sized-matrix.tur` -- size-typed containers
- `parsec.tur` -- parser combinators

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
- `signal/` -- POSIX signal handling
- `time.tur` / `timer.tur` -- wall clock and timers
- `math.tur` -- numeric functions
- `random.tur` -- random number generation
- `log.tur` -- structured logging

### Chapter 32 -- Advanced Type Machinery

- `fix.tur` -- fixed-point of a functor
- `free.tur` -- Free Monad
- `typeclass.tur` -- typeclass dispatch infrastructure
- `session.tur` -- session types
- `dynvar.tur` -- dynamic variables
- `effects.tur` -- algebraic effects
- `capability.tur` -- capability structs
- `contract.tur` -- runtime assertions

---

## Appendices

- **A: Building from Source** -- prerequisites, `just build`, release vs. debug
- **B: Inline-C Quick Reference** -- type mapping table, common patterns
- **C: `just` Target Reference** -- all targets and what they do
- **D: Docstring Format Cheat Sheet** -- `;;;` required fields, conventions, ASCII rule
- **E: Language Changes by Phase** -- B1 through present, feature timeline
