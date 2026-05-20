# Developer Ergonomics Plan

This document audits Turmeric's "day one" developer experience against the
baseline expectations a typical user brings from languages like Rust, Python,
Node.js, or Go. Items are grouped by category, assessed against the current
state, and assigned priority tiers.

---

## 1. Baseline Checklist

| Feature | Status | Notes |
|---------|--------|-------|
| `--help` / `-h` flag | **Missing** | Running `tur` with no args prints usage, but `-h`/`--help` are not wired |
| `--version` / `-v` flag | **Missing** | Version lives in `VERSION` file; REPL banner uses it; no CLI flag |
| Run a file: `tur run <file>` | Done | Compiles + executes; supports `-- <args>` passthrough |
| Inline eval: `tur eval '<expr>'` | **Missing** | Only way is to pipe to REPL |
| Read from stdin | Partial | `tur format` reads stdin; `tur run` does not |
| REPL | Done | `tur repl`; editline history; multiline; color output |
| REPL multi-line input | Done | Detects unbalanced parens; `..` continuation prompt |
| REPL multi-line sweet-exp input | **Missing** | Indentation-based continuation for sweet-expressions |
| REPL history | Done | Persisted to `~/.tur_history` |
| REPL `:doc <sym>` | Done | Looks up builtins and user-defined symbols |
| CLI `tur doc <sym>` subcommand | **Missing** | Doc lookup requires entering REPL |
| Error file + line + column | Done | `Span` struct; context lines shown |
| Colored diagnostics | Done | ANSI; auto-detects TTY; `--no-color` to disable |
| `--explain <TUR-E####>` | Done | Prints long-form explanation for error code |
| `--explain <snippet>` | Done | Compiles inline snippet and explains errors |
| Type-check only: `tur check` | Done | No codegen; fast validation |
| Formatter: `tur format` | Done | `--check` mode for CI |
| Test runner: `tur test` | Done | Runs `.tur` files in a directory |
| Project scaffolding: `tur init` | Done | `--bin`/`--lib` variants (Phase PKG-1) |
| Tutorial system | Done | `:tutorial` in REPL; navigable steps |
| Non-interactive REPL scripting | Partial | `printf ':reload f.tur\n:quit\n' \| tur repl`; not a first-class mode |
| Exit code on compile/type error | Needs verification | Should be nonzero; worth an explicit test |
| Structured output: `--json-diagnostics` | Done | Phase 8 |
| Pipe-friendly output | Partial | Color auto-disabled on non-TTY; `tur format` uses stdin |

---

## 2. Priority Tiers

### Tier 1 — Immediate / High Visibility

These are the first things a new user tries. Absence creates a bad first
impression or blocks basic workflows.

#### 1a. `--help` / `-h` flag

**Current state:** `tur` with no arguments prints usage. But `tur --help`,
`tur -h`, `tur build --help`, etc. all fall through to "unknown command" or
parse errors.

**Expected behavior (parity with `cargo`, `go`, `python3 -m`):**

```sh
tur --help          # global usage
tur build --help    # subcommand-specific help
tur repl --help     # REPL flags
```

**Implementation notes:**
- Parse `--help` / `-h` before dispatching subcommands in `main()` at
  [src/main.c](../src/main.c)
- Each `cmd_*` function should also check for `--help` in its own argv
- Subcommand help should list all flags for that subcommand (not the full global
  usage dump)

---

#### 1b. `--version` / `-v` flag

**Current state:** Version is in [VERSION](../VERSION) (currently `0.6.0`).
The REPL banner prints it. Nothing else does.

**Expected behavior:**
```sh
tur --version   # → "turmeric 0.6.0"
tur -V          # same
```

**Implementation notes:**
- Read `VERSION` at compile time via a `#define TUR_VERSION` injected by CMake
  (`configure_file` or `target_compile_definitions`)
- Print and exit before any other processing

---

#### 1c. Inline eval: `tur eval '<expr>'`

**Current state:** No dedicated mode. Users must pipe to REPL:
```sh
echo "(+ 1 2)" | tur repl
```
This is non-obvious, pollutes output with the REPL banner, and fails if
editline is not available.

**Expected behavior:**
```sh
tur eval "(+ 1 2)"     # → 3
tur eval --file f.tur  # evaluate all top-level forms in file, print results
```

**Implementation notes:**
- New `cmd_eval()` in `src/main.c`
- Reuses existing parse → typecheck → eval pipeline (same as REPL's single-form
  path) without spinning up the interactive loop
- Suppress banner; exit with code 0 on success, 1 on any error

---

#### 1d. Nonzero exit codes on errors

**Expected behavior (matches `rustc`, `tsc`, `clang`):**

| Condition | Exit code |
|-----------|-----------|
| Success | 0 |
| Type / parse / semantic error | 1 |
| File not found | 2 |
| Internal compiler error (ICE) | 3 |

**Verification needed:** Confirm that `tur build`, `tur check`, and `tur run`
actually return nonzero on failure. Wire any paths that currently call `exit(0)`
or fall off `main()` with implicit 0.

---

### Tier 2 — Standard / Expected by Most Users

These are features that experienced users expect within the first hour.

#### 2a. CLI `tur doc <symbol>`

**Current state:** `:doc <sym>` works inside the REPL. There is no CLI equivalent.

**Expected behavior:**
```sh
tur doc map        # print doc for stdlib `map`
tur doc +          # builtins too
```

**Implementation notes:**
- New `cmd_doc()` in `src/main.c`
- Calls the same `turi_doc_lookup()` function used by the WASM glue
  ([src/wasm_glue.c](../src/wasm_glue.c))
- Prints to stdout (not stderr), exits 0 if found, 1 if not found

---

#### 2b. `tur run` from stdin

**Current state:** `tur format` reads stdin, but `tur run` does not.

**Expected behavior:**
```sh
echo "(println (+ 1 2))" | tur run -
cat myfile.tur | tur run -
```

**Implementation notes:**
- Treat `-` as a special filename meaning stdin in `cmd_run()`
- Write stdin content to a temp file (or memory buffer), then compile as normal

---

#### 2c. Subcommand-specific error context

**Current state:** When a subcommand gets wrong arguments, it falls through to
the global `usage()` dump — which is ~60 lines and buries the actionable hint.

**Expected behavior:** Wrong args to a subcommand should print only that
subcommand's usage, plus a "try `tur <cmd> --help`" note:
```sh
$ tur build
error: tur build: expected <file.tur> or <dir>

usage:
  tur build <file.tur> [-o <out>]
  tur build <dir>      [-o <out>]

  --release   optimized build
  --offline   skip dependency fetch

Try 'tur build --help' for full options.
```

---

#### 2d. REPL `:reset` command

**Current state:** No way to clear the session environment without restarting
the REPL process.

**Expected behavior:**
```
turmeric> :reset
;; Session cleared.
turmeric>
```

**Implementation notes:**
- Add `:reset` to the meta-command table in [src/turi/repl.c](../src/turi/repl.c)
- Re-initialize the environment to stdlib-only state

---

#### 2e. REPL multi-line sweet-expression input

**Current state:** The REPL detects incomplete input by counting unbalanced
parentheses and shows a `..` continuation prompt. This works correctly for
s-expression input, but sweet-expressions use indentation to delimit forms
rather than closing parens — so the continuation heuristic does not apply.

**Expected behavior:** When the user is entering a sweet-exp form, the REPL
should recognize that an indented continuation line is still part of the
current expression and keep accumulating input until a blank line (or fully
dedented line) signals the end of the form:

```
turmeric> define square x
..   * x x
;; => #<fn square>
turmeric> square 5
;; => 25
```

**Implementation notes:**
- In [src/turi/repl.c](../src/turi/repl.c), the continuation check currently
  inspects paren balance; add a parallel path for sweet-exp mode that instead
  tracks indentation depth relative to the first line of the form
- A blank line or a line with no leading whitespace (at the same depth as the
  opener) terminates the form
- The `..` prompt should remain unchanged; sweet-exp and s-exp multi-line entry
  share the same visual continuation cue
- This feature only activates when the REPL is in sweet-exp mode (if a mode
  flag exists) or when the first line of input does not start with `(`

---

#### 2f. REPL `_` for last result

**Current state:** Each expression is evaluated and printed; the value is not
bound to anything.

**Expected behavior (matches GHCi, Python REPL, `node`):**
```
turmeric> (+ 1 2)
3
turmeric> (* _ 10)
30
```

**Implementation notes:**
- After each successful eval, bind the result to `_` in the REPL environment
- Skip binding if the result is `()` / void

---

### Tier 3 — Quality-of-Life / Power User

These items meaningfully improve productivity but are not blocking for beginners.

#### 3a. REPL tab-completion

**Current state:** editline is wired for history and navigation but completion
callback is not hooked up.

**Expected behavior:** Tab completes stdlib names, local bindings, and
meta-commands.

**Implementation notes:**
- Implement `rl_complete` / editline completion callback in
  [src/turi/repl.c](../src/turi/repl.c)
- Source completions from the current environment's symbol table

---

#### 3b. `tur format --diff`

**Current state:** `tur format --check` exits 1 if the file would change but
prints nothing useful.

**Expected behavior:**
```sh
tur format --diff file.tur   # prints a unified diff, nonzero exit if changes
```

**Implementation notes:**
- Run formatter, compute diff against original source, print to stdout
- Useful for code review and pre-commit hooks

---

#### 3c. `tur explain` as a standalone UX

**Current state:** `--explain <TUR-E####>` and `--explain <snippet>` exist but
are global flags mixed with compiler flags — awkward to discover.

**Proposed:** Promote to a first-class subcommand:
```sh
tur explain TUR-E0001       # error code explanation
tur explain "(+ 1 \"x\")"  # compile snippet and explain
```

The `--explain` flags can remain as aliases for backward compat.

---

#### 3d. Machine-readable output modes

**Current state:** `--json-diagnostics` exists. No JSON output for `tur doc`,
`tur check` summary, or `tur test` results.

**Expected behavior:** A `--json` flag that applies globally and switches all
subcommand output to structured JSON (useful for editor integration and CI
tooling):
```sh
tur check --json myfile.tur   # { "errors": [...], "warnings": [...] }
tur test --json tests/        # { "passed": 10, "failed": 2, ... }
tur doc --json map            # { "name": "map", "sig": "...", "doc": "..." }
```

---

#### 3e. Startup time / `tur repl` cold start

Track and optimize cold-start latency. A REPL that takes >300 ms to show a
prompt loses interactive feel. Establish a benchmark:
```sh
time ./build/tur repl <<< ':quit'
```

---

## 3. Summary Roadmap

| ID | Feature | Tier | Effort |
|----|---------|------|--------|
| E1 | `--help` / `-h` on all subcommands | 1 | Small |
| E2 | `--version` / `-v` | 1 | Small |
| E3 | `tur eval '<expr>'` | 1 | Medium |
| E4 | Verify nonzero exit codes | 1 | Small |
| E5 | `tur doc <symbol>` CLI subcommand | 2 | Small |
| E6 | `tur run -` (stdin) | 2 | Small |
| E7 | Per-subcommand error + mini-usage | 2 | Medium |
| E8 | REPL `:reset` | 2 | Small |
| E9 | REPL multi-line sweet-exp input | 2 | Medium |
| E10 | REPL `_` last-result binding | 2 | Small |
| E11 | REPL tab-completion | 3 | Medium |
| E12 | `tur format --diff` | 3 | Small |
| E13 | `tur explain` as subcommand | 3 | Small |
| E14 | Global `--json` output flag | 3 | Medium |
| E15 | REPL cold-start benchmark | 3 | Small |

---

## 4. Reference: What Comparable Languages Do

| Language | `--help` | `--version` | Inline eval | Stdin run | Doc CLI | REPL `_` |
|---------|----------|-------------|-------------|-----------|---------|---------|
| Rust (`cargo`/`rustc`) | ✓ per-subcommand | ✓ | `rustc --edition 2021 - <<< '...'` | ✓ (`-`) | `rustdoc` | — |
| Python | ✓ | ✓ | `python3 -c '<expr>'` | ✓ (`-`) | `pydoc <sym>` | ✓ |
| Node.js | ✓ | ✓ | `node -e '<expr>'` | ✓ | — | ✓ |
| Go | ✓ per-subcommand | ✓ | `go run -` (stdin) | ✓ | `go doc <sym>` | — |
| GHCi / Haskell | ✓ | ✓ | `ghc -e '<expr>'` | ✓ | `:doc` in REPL | ✓ (`it`) |
| Ruby | ✓ | ✓ | `ruby -e '<expr>'` | ✓ | `ri <sym>` | ✓ (`_`) |
| Turmeric (current) | ✗ | ✗ | Pipe only | ✗ | REPL only | ✗ |
| **Turmeric (target)** | **✓** | **✓** | **`tur eval`** | **`tur run -`** | **`tur doc`** | **✓** |
