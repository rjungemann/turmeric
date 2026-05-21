# Turmeric Standard Library Expansion Plan

## What Is Already Present

| Module | Coverage |
|--------|----------|
| `io.tur` | `fopen`/`fread`/`fwrite`/`opendir`/`readdir`, `Real-FileSystem` capability |
| `net.tur` / `async_socket.tur` / `async_pipe.tur` | Socket primitives, async I/O |
| `args.tur` | CLI argument parsing (flags, options, subcommands) |
| `time.tur` | Time access |
| `math.tur` | Numeric functions |
| `log.tur` | Structured logging |
| `random.tur` | Random numbers |
| `signal/` | Signal handling (capability subdirectory) |
| `thread.tur`, `fiber.tur`, `chan.tur`, `stm.tur`, etc. | Concurrency |

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
- `path/basename p` -- last component (`"foo/bar.txt"` → `"bar.txt"`)
- `path/dirname p` -- parent dir (`"foo/bar.txt"` → `"foo"`)
- `path/extension p` -- extension including dot (`"bar.txt"` → `".txt"`), or `""`
- `path/stem p` -- base without extension (`"bar.txt"` → `"bar"`)
- `path/absolute? p` -- true if starts with `/`
- `path/normalize p` -- collapse `..` and `.` segments

Implementation: pure Turmeric string manipulation over `str.tur`.

---

### 4. Extended File System (`fs.tur` or extend `io.tur`)

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
env.tur  →  process.tur  →  path.tur  →  fs.tur  →  re.tur  →  json.tur  →  term.tur  →  digest.tur  →  csv.tur
```

- `env` and `process` together give full UNIX scripting capability.
- `path` and `fs` are needed for any real file-handling program.
- `re`, `json`, `csv` cover the common data-wrangling use cases.
- `term` and `digest` are lower priority but round out the scripting toolkit.
