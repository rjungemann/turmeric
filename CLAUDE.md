# Turmeric 2 -- Claude Code Guide

## CLI Argument Parsing -- STRICT RULE

Reading CLI arguments via any mechanism other than `*args*` or `stdlib/args.tur` is
**strictly forbidden** in this codebase. This means:

- **Allowed**: `*args*` (the pre-declared global cons list), `head`/`tail`/`cstr->parse-int`
  to walk it, or `args/parse` from `stdlib/args.tur` for structured parsing.
- **Forbidden**: `parse-first-arg`, `parse-arg`, or any inline-C that directly reads
  `g_tur_args` via a raw `struct __tur_cons` cast. These patterns have been purged from
  the codebase. Do not reintroduce them.

If you need to read argument N in a self-contained benchmark file that does not import
stdlib, define local `head`/`tail`/`cstr->parse-int` stubs as inline-C at the top of
the file -- the interpreter will override them with stdlib natives automatically.

## Build System

The project uses `just` (not `make`). Common targets:

```sh
just build       # debug build
just test        # build + run tests
just release     # release build
just docs        # generate API documentation
just wasm        # build WebAssembly module (runs docs first)
just web-dev     # run web dev server
```

## Docstring Standard (`;;;`)

Use `;;;` (triple-semicolon) as the doc-comment marker. A docstring block
immediately precedes a `defn`, `defmacro`, `defstruct`, or `definstance`.

### Format

```turmeric
;;; cons -- prepend a value to a list.
;;;
;;; Parameters:
;;;   value -- the element to prepend
;;;   next  -- the existing list (or nil-value for empty)
;;;
;;; Returns:
;;;   A new Cons cell pointing to next.
;;;
;;; Example:
;;;   (cons 1 (cons 2 (nil-value)))  ; => (1 2)
;;;
;;; Since: Phase B1
(defn cons [value next] :int
  ...)
```

### Required Fields

| Field | Required | Notes |
|-------|----------|-------|
| One-line summary (first `;;;` line) | Yes | `;;; name -- brief description` |
| `Parameters:` block | If non-zero arity | One `;;;   name -- desc` line per param |
| `Returns:` | Yes, unless `:void` | Describe the return value |
| `Example:` | Yes | At least one usage example |
| `Since:` | When known | Phase tag, e.g. `Phase B1` |

### Conventions

- First line: `;;; name -- brief summary` (name repeated for greppability)
- Blank `;;;` lines separate sections
- Examples use `; => result` to show expected output
- Internal helpers (e.g. `tur-contract-check`, `__functor_*`) get a shorter
  one-liner only -- no Parameters/Returns/Example blocks needed
- **ASCII only** -- use `--` (double hyphen), never em dashes (`--`)
- A non-`;;;` line resets the docstring buffer; the `;;;` block must be
  immediately above the definition it documents

### Docstring Levels

- **Exported / public API**: full docstring (summary + params + returns + example + since)
- **Internal helpers**: single-line `;;; name -- what it does`
- **Typeclass instances** (`definstance`): single-line summary

## Generated Docs

Run `just docs` (or `python3 tools/gendocs.py stdlib/ --out docs/api/`) to
regenerate the HTML API reference from `;;;` docstrings. Also pass
`--emit-tur stdlib/docstrings.tur` to rebuild the runtime lookup table.

The generated files live in `docs/api/` -- do not edit them by hand.

## Fixture Files

Test fixture files (`tests/fixtures/**/*.tur`) must be ASCII-only. The Turmeric
parser hangs on non-ASCII bytes (e.g. UTF-8 em dashes). Always use `--` instead
of `--`.

## Sweet-Expression Style

Turmeric files can opt into sweet-expression syntax with a `#lang` directive or
a `.tursweet` extension. Prefer the full sweet-exp style over plain s-expressions
when writing new `.tursweet` files.

```
#lang sweet-exp
```

Sweet-exp gives three tools. Use all three, choosing whichever reduces noise for
a given expression:

| Tool | Syntax | Use when |
|------|--------|----------|
| Indentation (t-expr) | leading whitespace replaces outer `(...)` | top-level forms and multi-line bodies |
| Neoteric | `f(x y)` replaces `(f x y)` | inline calls, especially single-result expressions |
| Rest-of-line | `$ expr` replaces the surrounding outer `(...)` | a single nested call that would otherwise need wrapping parens |

### Indentation -- no outer parens for forms and bodies

The primary rule: drop the outer `(...)` of any form whose body can be expressed
as an indented block.

```turmeric
#lang sweet-exp

defn square [x :float] :float
  *(x x)

defn classify [x :float] :cstr
  if >(x 0.0)
    "positive"
    if <(x 0.0)
      "negative"
      "zero"

let [x compute-x()
     y compute-y()]
  +(x y)

while not(window-should-close?(w))
  do
    clear()
    render-frame()
    swap-buffers(w)
    poll-events()
```

### Neoteric -- `f(x)` for inline function calls

Use `f(args...)` wherever a function call is embedded inside another expression
and the neoteric form is more readable than the equivalent s-expression.

```turmeric
; Inline call as an argument
println(name)
let [v normalize(cross(a b))]

; Nested construction
let [proj mat4-perspective(0.785 {800.0 / 600.0} 0.1 100.0)]

; Operator calls -- neoteric keeps the symbol in prefix position
+(x y)
*(a b)
not(done?())
```

Prefer neoteric over traditional parens for any call that fits on one line and
has a clear, short argument list.

### `$` -- rest-of-line as argument

Use `$` to avoid one level of wrapping parens when a line has a single nested
call as its only argument.

```turmeric
; Without $:
println(str-concat("Hello, " name))

; With $:
println $ str-concat "Hello, " name

; Chained:
println $ normalize $ vec3(1.0 0.0 0.0)
```

Prefer `$` over neoteric when the outer call takes exactly one argument that is
itself a call with multiple space-separated arguments.

### Curly-infix -- `{a + b}` for arithmetic

Use `{...}` for arithmetic expressions to make operator precedence visual.

```turmeric
let [area {width * height}]
let [hyp sqrt({*(a a) + *(b b)})]
```

### What still uses traditional parens

A few forms are cleaner in traditional syntax:

- **`import` / `export`** -- short enough that indentation adds no value
- **`cons` lists** -- `(cons x (cons y 0))` reads clearly; neoteric
  `cons(x cons(y 0))` is harder to scan
- **Inline C blocks** -- the ` ```c ... ``` ` fence is already special syntax;
  the enclosing `defn` still uses the sweet-exp form but the body stays as-is
- **Single-form expressions** that fit on one line and are already minimal:
  `(nil-value)`, `(ok-val r)`, etc.

### Complete example

```turmeric
#lang sweet-exp

import opengl/window :refer [make-window destroy-window window-should-close?
                              poll-events swap-buffers set-clear-color clear]
import opengl/shaders :refer [compile-shader shader-program use-program]

defn make-program [vert-src :cstr frag-src :cstr] :int
  shader-program
    compile-shader(":vertex"   vert-src)
    compile-shader(":fragment" frag-src)

defn main [] :int
  let [w make-window(800 600 "Demo")]
    set-clear-color(0.1 0.1 0.1 1.0)
    while not(window-should-close?(w))
      clear()
      swap-buffers(w)
      poll-events()
    destroy-window(w)
    0
```

## Indentation Style

Follow Clojure-style indentation rules:

### Regular function calls -- align args under the first arg

When a call spans multiple lines, every argument after the first is indented to
the column of the first argument (one past the opening `(`).

```turmeric
(some-long-fn arg1
              arg2
              arg3)

(println (str-concat "Hello, "
                     name))
```

### Special forms and macros -- 2-space body indent

`defn`, `fn`, `let`, `if`, `when`, `do`, `cond`, `for`, `while`, and similar
forms use a fixed 2-space indent for their bodies, regardless of column position.

```turmeric
(defn greet [name :cstr] :void
  (println name))

(fn [x]
  (* x x))

(if condition
  then-branch
  else-branch)

(do
  (step-a)
  (step-b))
```

### Binding vectors -- align bindings under each other

In `let`, `loop`, etc., each binding pair is aligned so the names line up.

```turmeric
(let [x   1
      y   2
      foo (+ x y)]
  foo)
```

### Nesting -- rules compose

Each sub-expression follows its own rule at its own column.

```turmeric
(let [f (fn []
          (println "Hello"))]  ; fn body: 2 past the ( of (fn
  ...)

(foo (bar a
          b)   ; bar args: aligned with a
     c)        ; foo args: aligned with (bar ...
```

## Inline C Block Style Rule

Always place the closing ` ``` ` and its enclosing `)` on the same line
(` ```) `). Placing ` ``` ` on its own line causes Markdown renderers to
interpret it as the end of any surrounding code fence, breaking rendered
documentation. Example:

```turmeric
(defn file-size [f] :int
  ```c
  FILE* file = (FILE*)f;
  return (int)ftell(file);
  ```)
```

## Stdlib Layout

```
stdlib/
  list.tur      -- singly-linked list (Cons/nil)
  option.tur    -- optional values (some/none)
  result.tur    -- error handling (ok/err)
  pair.tur      -- generic two-element pair
  str.tur       -- UTF-8 string view
  vec.tur       -- growable array
  macros.tur    -- core macros (cond, when, for, do-m, doc, ...)
  contract.tur  -- runtime contracts (assert!, require!, ensure!, ...)
  hamt.tur      -- persistent hash-array-mapped trie
  map.tur       -- map operations (delegates to hamt)
  fix.tur       -- Fix type (fixed-point of a functor); cata/ana
  free.tur      -- Free monad; free-pure/lift/bind/fmap/run
  ...           -- concurrency, effects, typeclass, I/O, etc.
  docstrings.tur -- AUTO-GENERATED by gendocs.py --emit-tur; do not edit
```

## Web / WASM

The web REPL lives in `web/`. It uses Monaco Editor and the Emscripten WASM
build of libturi (`web/turmeric.js`).

The doc panel in the web REPL calls `turi_doc_lookup(name)` (exported from
`src/wasm_glue.c`) to retrieve doc strings without printing to the console.
