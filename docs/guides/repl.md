---
title: tur repl -- REPL Reference
category: Getting Started
description: Reference guide for the `tur repl` interactive read-eval-print loop, covering startup, expression evaluation, meta-commands, and configuration
---

# tur repl -- REPL Reference

`tur repl` launches an interactive Turmeric read-eval-print loop.

```sh
tur repl              # interactive Turmeric prompt
tur repl --watch      # also auto-reload spice exports when source changes
                      # (see "Working with spices in the REPL" below)
```

---

## Starting and stopping

```
$ tur repl
Turmeric v0.x.0  (type :help for help, :quit to exit)
> 
```

Exit with `:quit`, `:q`, or `Ctrl-D`.

---

## Evaluating expressions

Type any Turmeric expression and press Enter.  The result is printed on the
next line.

```
> (+ 1 2)
3
> (* 6 7)
42
> "hello"
"hello"
```

Top-level definitions persist across expressions:

```
> (defn square [x :int] :int (* x x))
> (square 9)
81
```

---

## Multi-line input

If the parentheses in the current input are not balanced, the REPL keeps
reading additional lines (shown with a `  ` indent prompt) until the
expression is complete:

```
> (let [x 10
        y 20]
    (+ x y))
30
```

To abandon an incomplete expression, enter a blank line.

---

## Meta-commands

Meta-commands begin with `:` and are processed before evaluation.

### `:help`

Prints a summary of all meta-commands.

```
> :help
Meta-commands:
  :help               show this help
  :quit  :q           exit the REPL
  :type <expr>        print inferred type without evaluating
  :doc  <sym>         print documentation for a symbol or builtin
  :reload <file>      evaluate a .tur file into the current session
```

### `:quit` / `:q`

Exits the REPL.  Equivalent to `Ctrl-D`.

### `:type <expr>`

Elaborates `<expr>` and prints the inferred type without evaluating it.

```
> :type (+ 1 2)
:int
> :type (fn [x :int] :int x)
(:int -> :int)
```

### `:doc <sym>`

Prints brief documentation for a known builtin or user-defined symbol.

```
> :doc println
println -- print a value followed by a newline
> :doc +
+ -- integer or float addition
```

For user-defined functions the signature is printed:

```
> (defn square [x :int] :int (* x x))
> :doc square
square : (Int -> Int)
```

### `:reload <file>`

Evaluates the contents of `<file>` into the current session.  Useful for
loading a script into the REPL without restarting.

```
> :reload src/utils.tur
reloaded src/utils.tur
```

If the file cannot be opened or contains an error, a diagnostic is printed and
the session continues.

> **Not to be confused with `(reload)`.**
> `(reload)` (a Turmeric form, no leading colon) rebuilds the *enclosing
> spice* and refreshes its FFI bindings -- a different mechanism from
> `:reload <file>` for loading a one-off `.tur` script.
> See [Working with spices in the REPL](#working-with-spices-in-the-repl).

---

## Output format

| Value type | Example output |
|-----------|----------------|
| Integer | `42` |
| Float | `3.14` |
| Boolean | `true` / `false` |
| String (`:cstr`) | `"hello"` |
| Nil | (nothing printed -- nil results are silent) |
| Closure | `#<fn square>` |
| Struct | `#<struct Point>` |
| Future | `#<future pending>` / `#<future resolved>` |
| Error | `error: <message>` (to stderr) |

---

## Colour output

ANSI colour is enabled automatically when both stdout and stderr are
connected to a terminal.  Diagnostics (type errors, parse errors) are
highlighted in colour when enabled.

To force-disable colour, redirect output through a pipe:

```
tur repl 2>/dev/null | cat
```

---

## Editing and history

If the binary was built with `editline` support (the default on macOS and
most Linux distributions), the REPL provides:

- **Line editing** -- arrow keys, `Ctrl-A`/`Ctrl-E`, `Ctrl-K`, etc.
- **History** -- `Up`/`Down` arrows cycle through previous entries.
- **Completion** -- `Tab` completes known symbol names (when implemented).

Without editline, raw `fgets` input is used with no history or editing.

---

## Working with spices in the REPL

When you launch `tur repl` from inside a spice project (any directory
whose ancestor contains a `build.tur`), the REPL auto-discovers and
compiles the project into a shared library, then makes every exported
defn callable from the prompt:

```
$ cd ~/projects/my-spice
$ tur repl
Loaded spice from /home/me/projects/my-spice (5 exports)
turmeric> (add42 100)
=> 142
turmeric> (sh/mul 6 7)
=> 42
```

Each export is bound under **two** names so you can call it either way:

- **Bare** -- `(add42 100)`
- **Qualified** -- `(<module>/<defn> ...)`, e.g. `(sh/add42 100)`

The qualified form avoids collisions when two modules in the same
project export the same name.

### Cache layout

The compiled library and its symbol manifest live under
`.tur-repl-cache/` next to your `build.tur`:

```
my-spice/
├── build.tur
├── src/
│   └── lib.tur
└── .tur-repl-cache/        <- auto-generated, gitignored
    ├── lib-0.so            <- shared library (one per process generation)
    └── exports.manifest    <- module/defn -> mangled C symbol :: signature
```

The first time the cache directory is created, `.tur-repl-cache/` is
appended to your project's existing `.gitignore` (idempotently; no
`.gitignore` is created if one didn't already exist).

The `lib-<N>.so` filenames are generation-tagged so re-loads always
see a fresh `dlopen` handle. The cache is fully reproducible from
source -- deleting it just costs one rebuild on the next REPL start.

### Skipping the auto-build

If sources haven't changed since the last REPL invocation, the loader
sees the cached `.so` is newer than every `.tur` under `src/` and
skips the rebuild entirely:

```
$ tur repl                    # rebuild + load (~1s)
$ tur repl                    # nothing changed -- instant load
```

### (reload) -- pick up edits without restarting

The REPL exposes a `(reload)` form that re-runs the build for the
current spice, swaps in the fresh library, and refreshes the symbol
bindings against the new function pointers:

```
turmeric> (add42 0)
=> 42
[...you edit src/lib.tur to change the body of add42...]
turmeric> (reload)
(reload) rebuilt 1 export
=> nil
turmeric> (add42 0)
=> 100
```

`(reload)` is well-behaved in every scenario:

| Situation | Output |
|---|---|
| No spice loaded, no project here | `(reload) no spice project here; nothing to reload` |
| No spice loaded, build.tur found | `(reload) loaded N exports from /path` (self-heal) |
| Spice loaded, no source changes | `(reload) no changes` |
| Spice loaded, source changed | `(reload) rebuilt N exports` |
| Build failed (e.g. compile error) | `(reload) failed; previous spice image left in place` |

The self-heal case matters most when your startup build failed (e.g.
a compile error in your spice): instead of restarting the REPL after
fixing the source, just type `(reload)`.

### --watch -- auto-reload on edit

`tur repl --watch` checks source freshness between every prompt and
fires `(reload)` automatically when any `.tur` file's mtime advances:

```
$ tur repl --watch
turmeric> (add42 0)
=> 42
[...you edit src/lib.tur...]
turmeric> (add42 0)
(reload) rebuilt 1 export
=> 100
```

The check is synchronous and runs right before each eval (one `stat`
call per `.tur` file in the build dir). There's no background thread
and no platform-specific filesystem watcher; polling at the prompt
cadence is sufficient because the user has to type *something* to
advance the loop anyway.

### Environment knobs

| Variable | Effect |
|---|---|
| `TUR_NO_AUTO_SPICE=1` | Skip discovery entirely. Useful for a pure-Turmeric REPL inside a project directory. |
| `TUR_BIN=<path>` | Override the executable used for the rebuild subprocess. Defaults to `tur` (PATH lookup). Helpful when running an in-tree dev build. |

### Calling spice defns: type marshaling

Arguments are marshaled per the defn's signature recorded in
`exports.manifest`. Each parameter falls into one of two **classes**:

- **`:int` class** -- `:int`, `:bool`, `:cstr`, `:ptr`, sized integer
  types (`:int8`, `:uint32`, ...). All passed in a 64-bit integer
  register.
- **`:float` class** -- `:float`, `:float32`, `:float64`. Passed in a
  vector register.

The marshaler accepts compatible Turmeric values:

```
turmeric> (sh/add42 100)         ; :int -> :int            ✓
=> 142
turmeric> (sh/scale 2.5 4.0)     ; :float :float -> :float ✓
=> 10
turmeric> (sh/add42 1.5)         ; :float into :int slot   ✗ rejected
error: ffi: 'sh/add42' arg 0: expected :int-class, got float
```

Auto-widening from `:int` to `:float` is allowed; the reverse is not
(it would lose precision). Arity mismatches surface as:

```
turmeric> (sh/add42)
error: ffi: 'sh/add42' expects 1 arg, got 0
```

### Current limits (v1)

- **Variadic exports** (`& rest :type`) are recognised but not
  callable from the REPL. The error message is explicit; the marshaling
  code for cons-list rest args lives in a later phase.
- **Struct / ADT returns** can't yet be reconstructed on the
  interpreter side. The error suggests sticking to primitive
  (`:int` / `:float` / `:cstr` / etc.) returns for now.
- **(import M :refer [...])** at REPL top level still hits the
  elaborator's "import is only allowed inside defmodule" restriction.
  Since spice exports are pre-bound at load time, you don't need to
  import them -- call them directly.

### Troubleshooting

**`stale exports.manifest`** -- the loader found a symbol in
`exports.manifest` that isn't in `lib-N.so`. Either type `(reload)`
to rebuild against the current source, or delete the cache and
restart:

```sh
rm -rf .tur-repl-cache
tur repl
```

**`spice rebuild failed`** -- the underlying `tur build --shared`
subprocess reported a compile error. The full output is replayed.
Fix the source and type `(reload)` (no need to restart the REPL).

**`no dispatcher for shape`** -- a defn's arity exceeds the FFI
dispatcher table's coverage (default: arity 0..6). Regenerate with a
larger bound:

```sh
python3 tools/gen_ffi_dispatch.py --max-arity 8
# rebuild the tur binary
```

---

## Non-interactive use

`:reload` can be piped in to run a script and then drop to the REPL, or to
run a script non-interactively:

```sh
# Run a file and exit
printf ':reload myfile.tur\n:quit\n' | tur repl

# Capture output only (discard banner)
printf ':reload myfile.tur\n:quit\n' | tur repl 2>/dev/null \
    | sed '1d'   # strip the banner line
```

---

## See also

- [eval-api.md](eval-api.md) -- C embedding API for programmatic use of libturi.
- `man tur-repl` -- man page with a concise option reference.
