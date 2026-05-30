# Turmeric 2 -- Claude Code Guide

## Fixture Snapshots -- STRICT RULE

`tests/fixtures/*/expected.c` are codegen snapshots that **must match** before
any PR is opened. A "codegen mismatch" failure is a real failure -- never dismiss
it or open a PR with failing fixtures.

**When the codegen changes** (e.g. `main` signature, new preamble, new boilerplate):

1. Regenerate all snapshots:
   ```sh
   TUR=./build/tur
   for dir in tests/fixtures/*/; do
     if [ -f "$dir/expected.c" ]; then
       input="$dir/input.tur"
       [ -f "$input" ] || input="$dir/$(basename $dir).tur"
       [ -f "$input" ] && "$TUR" emit-c "$input" > "$dir/expected.c" 2>/dev/null
     fi
   done
   ```
2. Verify the test suite passes: `bash tests/run.sh 2>&1 | grep "^FAIL"` (must be empty)
3. Commit the updated snapshots alongside the codegen change -- never in a separate PR

**Before opening any PR**, run `bash tests/run.sh` and confirm zero `FAIL` lines.

### Leak detection (ASan/LSan) policy

The Debug build compiles `tur` with `-fsanitize=address,undefined`; on Linux
ASan ships LeakSanitizer enabled. The compiler/codegen path is leak-clean, so
`bash tests/run.sh` runs **with leak detection ON** -- a genuine leak in the
`tur build`/`emit-c` path will fail the suite (this is intended; do not
suppress it). The tree-walking turi/eval **interpreter** intentionally never
frees its closures/registered natives (process-lifetime), so the harnesses
that exercise it (`run-turi.sh`, `run-flags.sh`) and their ctest targets
default to `ASAN_OPTIONS=detect_leaks=0`. Override with
`ASAN_OPTIONS=detect_leaks=1 bash tests/<harness>.sh` to opt back in. See
[docs/asan-debug-leaks-plan.md](docs/asan-debug-leaks-plan.md).

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

The project uses Justfile recipes via `tur run` (or upstream `just` if
installed). The recipe names are the same; only the invocation form differs.

```sh
tur run build    # debug build   (also: just build)
tur run test     # build + run tests
tur run release  # release build
just docs        # generate API documentation  (tur fmt not yet installed)
just wasm        # build WebAssembly module (runs docs first)
just web-dev     # run web dev server
```

For the main turmeric repo's own build you still need CMake (via `just
configure`). `tur run` is for spice development. See
[docs/guides/tur-run-guide.md](docs/guides/tur-run-guide.md).

## Spice Repository Layout

Spice implementations live in the sibling repository `../turmeric-spices`, not
under this repository.

- Do not create or scaffold a local `./spices/` tree in this repo.
- When work targets a spice, edit it in `../turmeric-spices`.
- In this repo, only touch spice-related fixtures, docs, integration glue, or
  references that are intentionally kept here.

## Per-file Commands Inside a Spice

`tur check`, `tur emit-c`, `tur emit-h`, and `tur run <file>` walk up
from the input file looking for an enclosing `build.tur`. When they
find one, the spice's `src/` is added to the module-resolution search
path, and every `:spices` dep declared in the manifest contributes its
`src/` too -- including `:path`-based local deps and workspace siblings
resolved via the parent `:members` list. The result is that intra-spice
imports (like `(import frame/schema)`) resolve without per-spice `-I`
configuration in your editor, LSP, or format-on-save hook.

`tur fetch` is only needed for `:url`-backed deps. Local-source deps
(`:path` entries and workspace siblings) resolve with no fetch step and
produce no `tur.lock` entries.

- Explicit `-I <dir>` flags still work and win on name collisions.
- `--no-auto-spice` (global flag, before the subcommand) opts out.
- `tur build <file>` (single-file build) does **not** auto-discover --
  use `tur run <file>` for the same convenience, or pass `-I` explicitly.
- `tur build <dir>` and `tur run` (project mode) configure themselves
  from `build.tur`; auto-discovery is a no-op there. When `<dir>` holds a
  `build.tur`, the build descends into `src/` (recursively, including
  nested `src/<pkg>/`), skips the manifest itself, resolves the include
  path from the project's own `src/` plus each `:spices` dep's `src/`, and
  validates that every declared `:exports` module has a backing source
  file. See [docs/manifest-driven-build-descent-plan.md](docs/manifest-driven-build-descent-plan.md).

See [docs/guides/developing-spices-guide.md](docs/guides/developing-spices-guide.md#per-file-commands-inside-a-spice)
for the full rules.

## REPL Auto-Discovery (spice-repl-plan)

`tur repl` also walks up from cwd looking for `build.tur`. When it
finds one, the spice tree is AOT-compiled into a shared library
under `<root>/.tur-repl-cache/`, dlopened, and every exported defn is
bound as a callable native at the prompt (both bare `add42` and
qualified `mod/add42`). Source edits propagate via the `(reload)`
form, or automatically with `tur repl --watch`.

- `TUR_NO_AUTO_SPICE=1` opts out (the REPL behaves as before).
- `TUR_BIN=<path>` overrides the subprocess executable; tests set
  this to point at the in-tree build.
- `.tur-repl-cache/` is appended to `.gitignore` on first creation
  (if a `.gitignore` already exists).

See [docs/guides/repl.md](docs/guides/repl.md#working-with-spices-in-the-repl)
for the full workflow + cache layout + troubleshooting guide.

## Docstring Standard (`;;;`)

Use `;;;` (triple-semicolon) as the doc-comment marker. A docstring block
immediately precedes a `defn`, `defmacro`, `defstruct`, or `definstance`.

### Module docstrings

A contiguous `;;;` block that appears **before the first real definition**
(`defn`, `defmacro`, `defstruct`, `definstance`, `defopaque`) in a file
becomes the *module docstring*. Place it at the very top of the file,
followed by a `;;` comment line (which acts as the separator):

```turmeric
;;; tur/list -- untyped singly-linked Cons/nil list.
;;;
;;; Legacy list implementation; prefer tur/list for new code.
;;;
;;; Since: Phase B1
;; List type for Turmeric        <- ;; line terminates the module block
(defstruct Cons ...)
```

The `tools/gendocs.py` parser captures this block as `module['docstring']`
and renders it as a description paragraph on the per-module HTML page.
It also registers the module name as a key in the doc-lookup table so
`(doc 'tur/list)` returns the summary.

The separator can also be any non-comment, non-blank, non-definition form
(e.g. `(defmodule ...)`, `(export ...)`, `(extern-c ...)`).

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

### `requires.*` skip markers

A fixture directory can carry a marker file that causes `tests/run.sh` to
PASS-skip it under certain conditions:

| Marker | Skips when ... |
| --- | --- |
| `requires.tsan` | `TUR_TSAN` is not `1` |
| `requires.interp` | (override) forces the interpreter path even under non-TSan |
| `requires.dedicated-runner` | always under `run.sh`; the fixture is owned by its own ctest target (e.g. `tur_eval_import`) |
| `requires.spices` | the sibling `../turmeric-spices/` checkout is absent |

## Optional dependencies

Some fixtures depend on the sibling repo `../turmeric-spices/`. When present,
fixtures tagged `requires.spices` run as normal; when absent they auto-skip.
To enable them, clone the repo next to this one:

```sh
git clone <turmeric-spices-url> ../turmeric-spices
```

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

defn classify [x : float] :cstr
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

## Function Arity Style Guide

### Hard parameter limit

`MAX_FN_ARITY` is **16**. Functions with more than ~5 positional parameters
are a code smell; 16 is an emergency escape hatch, not a target.

### More than 5 params -- reach for `defstruct`

When a function needs many named, independent inputs, pack them into a
struct and pass a single options value:

```turmeric
(defstruct CsvOpts
  [delim       :int   ;; field separator (e.g. 44 = ',')
   quote       :int   ;; quote char (e.g. 34 = '"')
   has-header  :int   ;; 1 = first row is header
   infer-rows  :int   ;; rows to sample for type inference
   null-str    :cstr  ;; string that represents NULL (e.g. "")
  ])

(defn read-csv [src :cstr opts :CsvOpts] :int
  ...)
```

**Default values via partial application** (Haskell-style idiom):

```turmeric
(def default-csv-opts (CsvOpts 44 34 1 100 ""))

;; read-csv-fast already has opts baked in; call it with just the filename.
(def read-csv-fast (read-csv default-csv-opts))

(read-csv-fast "data.csv")
```

This composes cleanly with currying: `(read-csv default-csv-opts)` returns a
closure `(fn [src :cstr] :int ...)` that already has the defaults locked in.

### Genuine variadic interfaces -- use `& rest :type`

When a function takes an *unknown number of values of the same type*
(e.g., `println`, `format`, aggregation column lists), use a variadic rest
parameter:

```turmeric
(defn println-all [first :cstr & rest :cstr] :void
  (println first)
  ;; rest is a cons-list of :cstr; walk it with head/tail helpers
  ...)

(println-all "hello")              ;; rest = nil
(println-all "a" "b" "c")         ;; rest = cons("b", cons("c", 0))
```

The rest type is **fully type-checked** -- not just primitives. User-defined
types (`defopaque` newtypes, structs, ADTs, type applications) are resolved to
their full type and each rest argument is checked by identity at the call site:

```turmeric
(defopaque Route :int)
(defopaque Middleware :int)

(defn launch [& routes :Route] :int ...)

(launch (route!) (route!))     ;; OK -- all Route
(launch (route!) (make-mw))    ;; ERROR: rest arg 1 (expected Route, got Middleware)
```

Because of this, the old workaround "declare the rest as `:int` and cast the
opaque handles back inside the body" is **no longer needed** -- write the real
type. A bare `:int` rest now also rejects opaque/struct/ADT values; pass the
declared type instead. For a mix of distinct handle types, prefer two explicit
`:list<T>` parameters over a single untyped rest.

Rules for `& rest`:

- **One `&` per parameter list** -- the rest parameter must be last.
- **Type annotation required** -- `& rest :int`, `& rest :Route`, etc. An
  unknown type name is a hard error (it is never silently demoted to `:int`).
- **Typed by the declared element type** -- primitive rest uses a fast
  TypeKind compare; a user-defined rest type compares full type identity.
  A declared type parameter (`(defn f [A] [& xs :A] ...)`) is a polymorphic
  rest that accepts any argument type.
- **Nil when absent** -- calling with zero rest args passes `rest = 0`.
- **No inline-C in variadic bodies** -- inline-C blocks declare fixed C
  signatures; wrap the inline-C in a fixed-arity helper and call it from
  the variadic body.
- **Not auto-curried** -- variadic `defn` does not produce a curried entry
  point. You can still under-saturate up to the required positional params
  (which returns a variadic closure), but you cannot partially apply into
  the rest slot.

### Cons-list manipulation in `#{Unsafe}` code

The rest parameter is a `int64_t` holding a pointer to a linked list of
`__tur_cons_cell { int64_t head; int64_t tail; }` cells, or `0` (nil).
Inline-C helpers that walk it look like:

```turmeric
(defn cons-list-sum [lst :int] #{Unsafe} :int
  ```c
  typedef struct { int64_t head; int64_t tail; } __tur_cons_cell;
  int64_t acc = 0;
  __tur_cons_cell *p = (__tur_cons_cell *)(intptr_t)lst;
  while (p) { acc += p->head; p = (__tur_cons_cell *)(intptr_t)p->tail; }
  return acc;
  ```)
```

Or use a pure tail-recursive helper:

```turmeric
(defn list-sum-acc [lst :int acc :int] #{Unsafe} :int
  (if (= lst 0)
    acc
    (list-sum-acc (cons-tail lst) (+ acc (cons-head lst)))))
```

### Quick decision guide

| Situation | Reach for |
|---|---|
| >5 named, independent params | `defstruct` options value |
| Default values + currying | `defstruct` + `(def fast (f defaults))` |
| Unknown number of same-type values | `& rest :type` variadic |
| Recursive accumulator threading context | closure-capture for context; fixed-arity for changing args |
| Genuinely >16 params | Something is wrong -- split the function |
