# tur repl — REPL Reference

`tur repl` launches an interactive Turmeric read-eval-print loop.

```sh
tur repl
```text

---

## Starting and stopping

```
$ tur repl
Turmeric v0.x.0  (type :help for help, :quit to exit)
> 
```text

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
```text

Top-level definitions persist across expressions:

```
> (defn square [x :int] :int (* x x))
> (square 9)
81
```text

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
```text

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
```turmeric

### `:quit` / `:q`

Exits the REPL.  Equivalent to `Ctrl-D`.

### `:type <expr>`

Elaborates `<expr>` and prints the inferred type without evaluating it.

```
> :type (+ 1 2)
:int
> :type (fn [x :int] :int x)
(:int -> :int)
```turmeric

### `:doc <sym>`

Prints brief documentation for a known builtin or user-defined symbol.

```
> :doc println
println — print a value followed by a newline
> :doc +
+ — integer or float addition
```text

For user-defined functions the signature is printed:

```
> (defn square [x :int] :int (* x x))
> :doc square
square : (Int -> Int)
```turmeric

### `:reload <file>`

Evaluates the contents of `<file>` into the current session.  Useful for
loading a script into the REPL without restarting.

```
> :reload src/utils.tur
reloaded src/utils.tur
```sh

If the file cannot be opened or contains an error, a diagnostic is printed and
the session continues.

---

## Output format

| Value type | Example output |
|-----------|----------------|
| Integer | `42` |
| Float | `3.14` |
| Boolean | `true` / `false` |
| String (`:cstr`) | `"hello"` |
| Nil | (nothing printed — nil results are silent) |
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

```sh
tur repl 2>/dev/null | cat
```

---

## Editing and history

If the binary was built with `editline` support (the default on macOS and
most Linux distributions), the REPL provides:

- **Line editing** — arrow keys, `Ctrl-A`/`Ctrl-E`, `Ctrl-K`, etc.
- **History** — `Up`/`Down` arrows cycle through previous entries.
- **Completion** — `Tab` completes known symbol names (when implemented).

Without editline, raw `fgets` input is used with no history or editing.

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
```text

---

## See also

- `docs/eval-api.md` — C embedding API for programmatic use of libturi.
- `man tur-repl` — man page with a concise option reference.
