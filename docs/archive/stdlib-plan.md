# Turmeric Standard Library Expansion Plan

## What Is Already Present

### Core Types

| Module | Description |
|--------|-------------|
| `list.tur` | Singly-linked list (`Cons`/`nil`) -- the foundational sequence type |
| `option.tur` | Optional values (`some`/`none`) |
| `result.tur` | Error handling (`ok`/`err`) with heap-allocated mixed-type storage |
| `pair.tur` | Generic two-element pair with `Clone` support |
| `str.tur` | UTF-8 string view (pointer + length) over C strings |
| `vec.tur` | Growable owning array with dynamic allocation and resizing |
| `slice.tur` | Borrowed view into a contiguous sequence (pointer + length) |

### Data Structures

| Module | Description |
|--------|-------------|
| `hamt.tur` | Persistent Hash Array Mapped Trie with C bindings |
| `map.tur` | Map operations (mutable and persistent); persistent variant lowers to HAMT |

### Language Infrastructure

| Module | Description |
|--------|-------------|
| `macros.tur` | Core macros: `cond`, `when`, `for`, `do-m`, `doc`, and more |
| `typeclass.tur` | Typeclass definitions: `Eq`, `Ord`, `Show`, `Num` |
| `contract.tur` | Runtime contracts: `assert!`, `require!`, `ensure!` |
| `capability.tur` | Capability passing via function-pointer structs (zero-cost dependency injection) |
| `safe.tur` | Bounds-checked wrappers returning `Option`/`Result` instead of panicking |
| `equal.tur` | Type-equality witnesses GADT for type refinement |

### Math and Bits

| Module | Description |
|--------|-------------|
| `math.tur` | Numeric functions: `sqrt`, `fabs`, `floor`, `ceil`, `pow`, trig, etc. |
| `bits.tur` | Bitwise operations (logical right shift and others) not expressible as arithmetic |

### Functional Programming

| Module | Description |
|--------|-------------|
| `fix.tur` | Fixed-point combinator (`Fix`) for recursive functor types; `cata`/`ana` |
| `free.tur` | Free monad over any functor; `free-pure`/`lift`/`bind`/`fmap`/`run` |
| `comonad.tur` | Comonad typeclass (`extract`/`extend`) -- the categorical dual of `Monad` |
| `backtrack.tur` | Backtracking monad (list monad) for non-deterministic computation |
| `effects.tur` | Algebraic effects for common side effects (`IO`, `State`, etc.) |

### Concurrency and Threading

| Module | Description |
|--------|-------------|
| `thread.tur` | POSIX thread `spawn`/`join`/`detach` wrapping `pthread_create` |
| `fiber.tur` | Lightweight cooperative fibers backed by `FiberBlock` C runtime |
| `chan.tur` | Synchronous and buffered channels with blocking/non-blocking send/recv |
| `stm.tur` | Software Transactional Memory for atomic multi-variable transactions |
| `mutex.tur` | POSIX mutex wrapping `pthread_mutex_t` |
| `condvar.tur` | POSIX condition variable wrapping `pthread_cond_t` |
| `rwlock.tur` | POSIX read-write lock (multiple concurrent readers, exclusive writer) |
| `atomic.tur` | Atomic integer operations (load/store/add/sub/swap/CAS) with sequentially-consistent ordering |
| `sync.tur` | `Once` (one-time initialization) and `Semaphore` (counting) primitives |
| `concurrent.tur` | Stub concurrency types for linear resource tracking (e.g. move-only `MutexGuard`) |
| `select.tur` | Multi-channel `select` -- simultaneous wait on multiple channel operations |
| `future.tur` | Promise/Future pair for async computation with fulfillment and exception handling |
| `taskgroup.tur` | Structured concurrency: `TaskGroup` for grouped task spawning and cancellation |
| `threadpool.tur` | `WorkQueue` + `ThreadPool` for thread-safe FIFO work distribution |
| `scheduler.tur` | Cooperative fiber scheduler with an explicit yield-based run queue |
| `scheduler_mt.tur` | Multi-threaded work-stealing fiber scheduler over a pool of OS threads |
| `timer.tur` | Timer API backed by a min-heap timer wheel for scheduled callbacks |

### I/O and System

| Module | Description |
|--------|-------------|
| `io.tur` | Low-level file I/O (`fopen`/`fread`/`fwrite`/`opendir`/`readdir`); `Real-FileSystem` capability |
| `args.tur` | CLI argument parsing: flags, options, subcommands via `args/parse` |
| `time.tur` | Time access capability backed by platform time functions |
| `random.tur` | Random number generation capability using platform RNG (`rand`) |
| `log.tur` | Structured logging capability (`Real-Logger`); debug/info/warn/error to stdout/stderr |

### Networking and Async I/O

| Module | Description |
|--------|-------------|
| `net.tur` | Socket primitives |
| `async_socket.tur` | Async socket I/O |
| `async_pipe.tur` | Async pipe I/O |
| `async_file.tur` | Async file I/O |

### Parsing

| Module | Description |
|--------|-------------|
| `parsec.tur` | Parser combinator library built on the backtracking monad |

### Logic and Constraint Solving

| Module | Description |
|--------|-------------|
| `logic.tur` | miniKanren-style logic programming with unification and relational queries |

### Memory Management

| Module | Description |
|--------|-------------|
| `rc.tur` | Reference-counted owning pointer with primitive lifetime operations |
| `ref.tur` | Heap-allocated single-value container with independent ownership |
| `dynvar.tur` | Dynamic variables and binding conveyance for implicit context passing |

### Sized / Static-Shape Types

| Module | Description |
|--------|-------------|
| `sized.tur` | Foundational support for static sizes and memory layout verification |
| `nat.tur` | Type-level natural numbers via GADT for phantom-type numeric constraints |
| `gadt-vec.tur` | Length-indexed Vec GADT using phantom types for compile-time length safety |
| `sized-bits.tur` | Packed bit vector (`SizedBitVec`) storing `n` bits in `ceil(n/8)` bytes |
| `sized-buf.tur` | Flat contiguous memory layout (`SizedBuf`) for sized types |
| `sized-matrix.tur` | Row-major 2D matrix with sized type annotations |

### Comonadic Structures / Zippers

| Module | Description |
|--------|-------------|
| `zipper.tur` | 1D list zipper comonad with cursor for efficient structural updates |
| `grid.tur` | `GridCtx` comonad: 2D grid with focused position for cellular automata |

### Serialization and Persistence

| Module | Description |
|--------|-------------|
| `serial.tur` | `Serializable` typeclass and instances for continuation checkpointing |
| `workflow.tur` | Workflow helpers for serializable continuations and persistent execution state |
| `session.tur` | Reusable session-type protocol patterns (`send`/`recv`/`choose`/`offer`) |

### Eval API

| Module | Description |
|--------|-------------|
| `turi/eval.tur` | Turmeric bindings for the `libturi` eval API |

### Domain-Specific Libraries

| Module | Description |
|--------|-------------|
| `signal/` | Signal processing: core `Signal` type and signal function arrows; DSP, envelopes, synth |
| `tidal/` | TidalCycles-inspired pattern DSL: patterns, polyrhythm, temporal transforms, live coding |
| `scscm/` | SuperCollider Synth Control (SCSCM): FFI bindings, pattern/synth/live-coding integration |
| `raylib.tur` | FFI bindings for Raylib (graphics and input) |

---

## Gaps to Fill

### 1. `env.tur` -- Environment Variables

Functions:
- `env/get name` -- return `(some cstr)` or `none`
- `env/get! name` -- return `cstr`, panic if missing
- `env/set name value` -- `setenv`
- `env/unset name` -- `unsetenv`
- `env/all` -- return a `map` of all env vars
- Convenience: `env/home`, `env/path`, `env/user`, `env/shell`

Implementation: thin `extern-c` wrappers around `getenv`/`setenv`/`unsetenv`/`environ`.

---

### 2. `process.tur` -- Process Management

Functions:
- `process/pid` -- `getpid`
- `process/ppid` -- `getppid`
- `process/exit code` -- `exit(code)`
- `process/exec path args` -- `execvp`; replaces current process
- `process/spawn path args` -- `fork` + `execvp`; returns child PID
- `process/wait pid` -- `waitpid`; returns exit status
- `process/run path args` -- spawn + wait; returns `(ok exit-code)` or `(err msg)`
- `process/capture path args` -- spawn with stdout pipe; returns `(ok output-cstr)` or `(err msg)`
- `process/cwd` -- `getcwd`
- `process/chdir path` -- `chdir`

Implementation: `extern-c` into `unistd.h` / `sys/wait.h`. `process/capture` uses `popen` or a pipe pair.

---

### 3. `path.tur` -- Path String Operations

Pure string functions, no syscalls:

- `path/join base segment ...` -- OS-correct path joining
- `path/basename p` -- last component (`"foo/bar.txt"` -> `"bar.txt"`)
- `path/dirname p` -- parent dir (`"foo/bar.txt"` -> `"foo"`)
- `path/extension p` -- extension including dot (`"bar.txt"` -> `".txt"`), or `""`
- `path/stem p` -- base without extension (`"bar.txt"` -> `"bar"`)
- `path/absolute? p` -- true if starts with `/`
- `path/normalize p` -- collapse `..` and `.` segments

Implementation: pure Turmeric string manipulation over `str.tur`.

---

### 4. `fs.tur` -- Extended File System

Higher-level operations beyond the low-level `io.tur` primitives:

- `fs/stat path` -- return struct `{ size mtime mode }` via `stat(2)`
- `fs/exists? path` -- true if path exists
- `fs/file? path` / `fs/dir? path` -- type checks
- `fs/mkdir path` -- `mkdir(2)`
- `fs/mkdirp path` -- create path and all parents
- `fs/rmdir path` -- `rmdir(2)`
- `fs/rm path` -- `unlink(2)`
- `fs/rename src dst` -- `rename(2)`
- `fs/copy src dst` -- read/write loop or `sendfile`
- `fs/glob pattern` -- match filenames; returns `(list cstr)`
- `fs/tmpfile` -- `mkstemp`, returns `(path fd)` pair
- `fs/read-text path` -- slurp entire file as `cstr`
- `fs/write-text path content` -- write `cstr` to file

Implementation: `extern-c` into `sys/stat.h`, `dirent.h`, `fnmatch.h`.

---

### 5. `re.tur` -- Regular Expressions

- `re/compile pattern` -- returns opaque regex handle; `(result handle err-msg)`
- `re/match re input` -- returns `(option (vec cstr))` of capture groups
- `re/match? re input` -- boolean test
- `re/find-all re input` -- returns `(list (vec cstr))`
- `re/replace re input replacement` -- first match replacement
- `re/replace-all re input replacement` -- global replacement
- `re/free re` -- release handle

Implementation: `extern-c` into POSIX `regcomp`/`regexec` (always available) or optionally PCRE2.

---

### 6. `json.tur` -- JSON Encode/Decode

- `json/encode value` -- convert a Turmeric value tree to a JSON `cstr`
- `json/decode s` -- parse a JSON `cstr` into a Turmeric value tree
- JSON value type: tagged union of `null | bool | int | float | string | array | object`
- `json/get obj key` -- safe key lookup; returns `(option value)`
- `json/get! obj key` -- panics if missing

Implementation: pure Turmeric recursive descent parser and emitter, no external deps. Uses `vec.tur` for arrays, `map.tur` for objects, `str.tur` for string building.

---

### 7. `term.tur` -- Terminal Utilities

- `term/width` / `term/height` -- `ioctl(TIOCGWINSZ)`
- `term/is-tty? fd` -- `isatty(3)`
- `term/set-raw fd` / `term/set-cooked fd` -- `tcsetattr(3)` mode switching
- ANSI helpers: `term/bold s`, `term/red s`, `term/green s`, `term/dim s`, etc. -- wrap string in ANSI escape codes; emit plain if `$NO_COLOR` is set or not a tty

Implementation: `extern-c` into `sys/ioctl.h`, `termios.h`.

---

### 8. `digest.tur` -- Checksums

- `digest/sha256 data len` -- return 32-byte digest as `(vec :int)`
- `digest/sha256-hex data len` -- return 64-char hex `cstr`
- `digest/md5 data len` -- 16-byte digest
- `digest/md5-hex data len` -- 32-char hex `cstr`

Implementation: vendor a small single-file SHA-256 and MD5 C implementation (MIT-licensed) under `src/vendor/`, expose via `extern-c`.

---

### 9. `csv.tur` -- CSV Parse/Emit

- `csv/parse-row line` -- split one CSV line into `(vec cstr)`; handles quoting
- `csv/parse s` -- parse multi-line CSV into `(list (vec cstr))`
- `csv/emit-row fields` -- serialize a `(vec cstr)` to a CSV line `cstr`
- `csv/emit rows` -- serialize `(list (vec cstr))` to full CSV `cstr`
- `csv/read-file path` -- `fs/read-text` + `csv/parse`
- `csv/write-file path rows` -- `csv/emit` + `fs/write-text`

Implementation: pure Turmeric, depends only on `str.tur`, `vec.tur`, `list.tur`, and `fs.tur`.

---

## Priority Order

Work left to right; each module unblocks the next:

```
env.tur  ->  process.tur  ->  path.tur  ->  fs.tur  ->  re.tur  ->  json.tur  ->  term.tur  ->  digest.tur  ->  csv.tur
```

- `env` and `process` together give full UNIX scripting capability.
- `path` and `fs` are needed for any real file-handling program.
- `re`, `json`, `csv` cover the common data-wrangling use cases.
- `term` and `digest` are lower priority but round out the scripting toolkit.
