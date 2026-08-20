---
title: CLI Arguments Guide
category: Language Basics
description: Passing arguments to scripts with `*args*` and parsing them with `stdlib/args.tur`
---

# CLI Arguments Guide

Turmeric scripts can receive command-line arguments via the built-in `*args*`
cons list. For structured parsing -- flags, named options, subcommands, and
type coercion -- the standard library provides `stdlib/args.tur` with a
builder-pattern API.

## Passing Arguments to a Script

Use `--` to separate `tur run` flags from your script's arguments:

```sh
tur run script.tur -- input.txt output.txt --verbose
```

Everything after `--` is forwarded to the script as a cons list bound to
`*args*`.

## Raw Access via `*args*`

`*args*` is a pre-declared cons list of `:cstr` values. Each element is one
argument string, in order.

```turmeric
;; script.tur
(defn main [] : int
  (println "arg 0:" (head *args*))
  (println "arg 1:" (head (tail *args*)))
  0)
```
```sweet-exp
;; script.tur
defn main [] :int
  println("arg 0:" head(*args*))
  println("arg 1:" head(tail(*args*)))
  0
```

```turmeric
;; Walk all arguments
(defn print-args [args : int] : void
  (when (not (= args 0))
    (println (head args))
    (print-args (tail args))))

(print-args *args*)
```
```sweet-exp
;; Walk all arguments
defn print-args [args :int] :void
  when not(=(args 0))
    println(head(args))
    print-args(tail(args))
print-args(*args*)
```

For scripts that do not import stdlib, `head` and `tail` are available as
stdlib natives automatically:

```turmeric
(defn main [] : int
  (let [n (cstr->parse-int (head *args*))]
    (println "count:" n)
    0))
```
```sweet-exp
defn main [] :int
  let [n (cstr->parse-int (head *args*))]
    println("count:" n)
    0
```

> **Note:** `cstr->parse-int` parses a C string into `:int`. It is available
> without any import.

## Structured Parsing with `stdlib/args.tur`

For anything more complex than a positional argument or two, use
`stdlib/args.tur`. It provides:

- **Flags** -- boolean `--verbose`, `--help`
- **Options** -- `--input=file.txt` or `--input file.txt` with type coercion
- **Defaults** -- option values used when the flag is absent
- **Positional args** -- ordered non-flag arguments
- **Subcommands** -- arbitrarily nested `build`/`test`/`push` dispatch
- **Help generation** -- automatic `--help` output from the spec

### Import

```turmeric
(load "stdlib/args.tur")
```
```sweet-exp
load("stdlib/args.tur")
```

### Building a Spec

Build a spec with the `args/spec-*` functions, then call `args/parse`.
Two conventions to know:

- Registration names **include** the dashes (`"--verbose"`); result accessors
  (`args/has?`, `args/get-*`) take the name **without** dashes (`"verbose"`).
- An option's default is passed as a `cstr` smuggled through the `:int`
  `dflt` slot -- define a tiny reinterpret helper (as below) -- or `0` to
  make the option required.

```turmeric
(defn cstr->int [s : cstr] : int
  ```c
  return (int64_t)(intptr_t)s;
  ```)

(defn main [] : int
  (let [spec (-> (args/spec-new)
                 (args/spec-prog "mytool")
                 (args/spec-flag "--verbose")
                 (args/spec-option "--input"  "string" 0)                 ; required
                 (args/spec-option "--count"  "int"    (cstr->int "1"))   ; default 1
                 (args/spec-option "--output" "string" (cstr->int "out.txt")))
        result (args/parse spec *args*)]
    (if (args/error? result)
      (do
        (println "error:" (args/error-msg result))
        1)
      (do
        (when (args/has? result "verbose")
          (println "verbose mode on"))
        (println "input: " (args/get-str result "input"))
        (println "count: " (args/get-int result "count"))
        (println "output:" (args/get-str result "output"))
        0))))
```
```sweet-exp
defn cstr->int [s :cstr] :int
  ```c
  return (int64_t)(intptr_t)s;
  ```

defn main [] :int
  let [spec (-> (args/spec-new)
                 (args/spec-prog "mytool")
                 (args/spec-flag "--verbose")
                 (args/spec-option "--input"  "string" 0)                 ; required
                 (args/spec-option "--count"  "int"    (cstr->int "1"))   ; default 1
                 (args/spec-option "--output" "string" (cstr->int "out.txt")))
        result (args/parse spec *args*)]
    if args/error?(result)
      do
        println("error:" args/error-msg(result))
        1
      do
        when args/has?(result "verbose")
          println("verbose mode on")
        println("input: " args/get-str(result "input"))
        println("count: " args/get-int(result "count"))
        println("output:" args/get-str(result "output"))
        0
```

```sh
tur run mytool.tur -- --input=data.csv --count=5
```

### Option Types

| Type string | Accessor | Notes |
|---|---|---|
| `"string"` | `args/get-str` | Returned as `:cstr` |
| `"int"` | `args/get-int` | Parsed to `:int` |
| `"float"` | `args/get-int` | Parsed to `:float` |
| `"bool"` | `args/get-bool` | `"true"`/`"1"` => true |

### Flags

Flags are boolean switches -- present means true, absent means false:

```turmeric
(args/spec-flag spec "--verbose")
(args/spec-flag spec "--dry-run")

;; At runtime:
(args/has? result "verbose")  ; => true/false
```
```sweet-exp
args/spec-flag(spec "--verbose")
args/spec-flag(spec "--dry-run")
;; At runtime:
args/has?(result "verbose")
; => true/false
```

### Positional Arguments

Positional arguments are everything that is not a flag or option value.
Access them as a cons list:

```turmeric
(let [pos (args/positional result)]
  (println "first file:" (head pos))
  (println "second file:" (head (tail pos))))
```
```sweet-exp
let [pos (args/positional result)]
  println("first file:" head(pos))
  println("second file:" head(tail(pos)))
```

```sh
tur run copy.tur -- src.txt dst.txt
```

### Subcommands

Register subcommands with their own specs. Each subcommand can have its own
flags, options, and nested subcommands:

```turmeric
(defn main [] : int
  (let [build-spec (-> (args/spec-new)
                       (args/spec-flag "--release")
                       (args/spec-option "--output" "string" (cstr->int "a.out")))
        test-spec  (-> (args/spec-new)
                       (args/spec-flag "--verbose")
                       (args/spec-option "--filter" "string" 0))
        spec       (-> (args/spec-new)
                       (args/spec-prog "myapp")
                       (args/spec-subcommand "build" build-spec)
                       (args/spec-subcommand "test"  test-spec))
        result     (args/parse spec *args*)]
    (if (args/error? result)
      (do
        (args/print-help spec)
        1)
      (let [sub (args/subcommand result)]
        (cond
          (cstr-eq? sub "build")
            (let [r (args/sub-result result)]
              (println "building, release:" (args/has? r "release"))
              0)
          (cstr-eq? sub "test")
            (let [r (args/sub-result result)]
              (println "testing, filter:" (args/get-str r "filter"))
              0)
          true
            (do
              (args/print-help spec)
              1))))))
```
```sweet-exp
defn main [] :int
  let [build-spec (-> (args/spec-new)
                       (args/spec-flag "--release")
                       (args/spec-option "--output" "string" (cstr->int "a.out")))
        test-spec  (-> (args/spec-new)
                       (args/spec-flag "--verbose")
                       (args/spec-option "--filter" "string" 0))
        spec       (-> (args/spec-new)
                       (args/spec-prog "myapp")
                       (args/spec-subcommand "build" build-spec)
                       (args/spec-subcommand "test"  test-spec))
        result     (args/parse spec *args*)]
    if args/error?(result)
      do
        args/print-help(spec)
        1
      let [sub (args/subcommand result)]
        cond
          cstr-eq?(sub "build")
          let
            [r (args/sub-result result)]
            println("building, release:" args/has?(r "release"))
            0
          cstr-eq?(sub "test")
          let
            [r (args/sub-result result)]
            println("testing, filter:" args/get-str(r "filter"))
            0
          true
          do
            args/print-help(spec)
            1
```

`cstr-eq?` is byte-wise content equality on `cstr`, from
`(import cstr :refer [cstr-eq?])`. It is not optional sugar: `=` has **no**
`cstr` overload at all, so `(= sub "build")` is a `TUR-E0006` operator-lookup
error rather than a comparison. `(eq? sub "build")` is the same comparison
reached through the auto-loaded `Eq[cstr]` instance, if you would rather not
import `cstr`.

```sh
tur run myapp.tur -- build --release
tur run myapp.tur -- test --filter=core --verbose
```

Nested subcommands work the same way: call `args/sub-result` on the outer
result, then `args/subcommand` on the inner to walk the chain.

### Help Output

`args/print-help` writes usage text to stdout based on the spec:

```turmeric
(when (args/has? result "help")
  (args/print-help spec)
  (exit 0))
```
```sweet-exp
when args/has?(result "help")
  args/print-help(spec)
  exit(0)
```

Or just let the parser handle missing required options and check
`args/error?`:

```turmeric
(when (args/error? result)
  (println (args/error-msg result))
  (args/print-help spec)
  (exit 1))
```
```sweet-exp
when args/error?(result)
  println(args/error-msg(result))
  args/print-help(spec)
  exit(1)
```

### Cleanup

Both spec and result are heap-allocated. Free them when done:

```turmeric
(args/spec-free   spec)
(args/result-free result)
```
```sweet-exp
args/spec-free(spec)
args/result-free(result)
```

## API Reference

| Function | Signature | Description |
|---|---|---|
| `args/spec-new` | `-> :int` | Create an empty arg spec |
| `args/spec-prog` | `spec :int name :cstr -> :int` | Set program name for help |
| `args/spec-flag` | `spec :int name :cstr -> :int` | Register a boolean flag |
| `args/spec-option` | `spec :int name :cstr type :cstr dflt :int -> :int` | Register a named option |
| `args/spec-subcommand` | `spec :int name :cstr sub :int -> :int` | Register a subcommand |
| `args/parse` | `spec :int argv :int -> :int` | Parse `*args*` against spec |
| `args/has?` | `result :int key :cstr -> :bool` | True if flag/option was supplied |
| `args/get-str` | `result :int key :cstr -> :cstr` | Get option value as string |
| `args/get-int` | `result :int key :cstr -> :int` | Get option value as int |
| `args/get-bool` | `result :int key :cstr -> :bool` | Get option value as bool |
| `args/positional` | `result :int -> :int` | Cons list of positional args |
| `args/subcommand` | `result :int -> :cstr` | Matched subcommand name |
| `args/sub-result` | `result :int -> :int` | Parse result for subcommand |
| `args/error?` | `result :int -> :bool` | True if parsing failed |
| `args/error-msg` | `result :int -> :cstr` | Error description string |
| `args/print-help` | `spec :int -> :void` | Print usage to stdout |
| `args/spec-free` | `spec :int -> :void` | Free spec memory |
| `args/result-free` | `result :int -> :void` | Free result memory |

## CLAUDE.md Rule

The `CLAUDE.md` file for this project enforces a strict rule: **CLI arguments
must only be read via `*args*` or `stdlib/args.tur`**. Reading from
`g_tur_args` via raw inline C is forbidden. See `CLAUDE.md` for details.
