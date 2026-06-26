---
title: Syntax Guide
category: Getting Started
description: How to read and write Turmeric -- s-expression and sweet-expression syntax
---

# Turmeric Syntax Guide

This guide is the front door to Turmeric's surface syntax. It teaches you to
*read* and *write* Turmeric source in both dialects:

1. The default **S-expression** dialect used by `.tur` files. SRFI-105
   **curly-infix** is enabled here too -- `{a + b}` reads as `(+ a b)` in
   every dialect, no `#lang` directive required.
2. The **sweet-expression** dialect activated by `#lang sweet-exp` or a
   `.tur.sweet` extension (indentation + neoteric + `$` + curly-infix).

It does not re-explain the semantics of every special form -- the deep-dive
guides own that. Instead it shows you the *shape* of the language and points
you at the right reference for the details.

Throughout, code is shown in paired blocks: a `turmeric` block followed by its
`sweet-exp` equivalent. Use the toggle above each pair to switch between them.

---

## Part 1 -- S-expression syntax

The default dialect is a Lisp. Every program is a tree of **forms**: atoms
(numbers, strings, symbols, keywords) and lists that group them.

### Lexical conventions

**Identifiers** are symbols like `square`, `vec-push!`, `option-some?`, or
`make-adder`. Conventionally, a trailing `!` marks mutation (`vec-push!`) and a
trailing `?` marks a predicate (`option-none?`). Operators like `+`, `-`, `*`,
`<=`, and `=` are ordinary identifiers used in prefix position.

**Keywords** start with a colon: `:int`, `:else`, `:name`. They are
self-evaluating and most often appear as type annotations, `cond` fallbacks,
and map keys.

**Numbers** include integers (`42`, `-5`), floats (`3.14`, `0.1`), and the
radix forms documented in the reader reference -- hex (`0xFF`), binary
(`0b1010`), and octal (`0o755`).

**Strings** are double-quoted with C-style escapes (`"line\n"`, `"tab\tend"`,
`"quote\"inside"`).

**ASCII only.** Source must be ASCII -- the reader hangs on non-ASCII bytes
such as UTF-8 em dashes. Always write `--` (double hyphen), never an em dash.

For the exhaustive catalogue of literal forms (radix integers, character
escapes, keyword rules), see the
[Reader Forms Reference](reader-forms-guide.md#literals).

### Comments

```turmeric no-check
(println "hi")  ; line comment -- to end of line

#|
  Block comment. These nest:
  #| inner |#
|#
(println "ok")
```

Doc comments use the triple-semicolon marker `;;;` and sit immediately above a
`defn`, `defmacro`, `defstruct`, or `definstance`. The docstring format (its
required `Parameters:`/`Returns:`/`Example:`/`Since:` sections) is specified in
the project's docstring standard. See the
[Reader Forms Reference](reader-forms-guide.md#comments) for the full comment
catalogue including the planned datum comment `#;`.

### The form tree

Turmeric has four bracketed containers, each legal in specific positions:

| Form | Syntax | Where it is legal |
|---|---|---|
| List | `(f x y)` | everywhere -- the universal form |
| Vector | `[a b c]` | binding position (`let`/`defn` params); expression position |
| Map literal | `#map{:k v}` | expression position |
| Set literal | `#set{a b}` | expression position |

In binding position `[...]` is a *binding spec* (parameter list or `let`
bindings). In expression position, `[...]` lowers to `(vec-of ...)`. See the
[Data Literals Guide](data-literals-guide.md) for the literal collection
semantics.

### Function application

A list evaluates as a call: the head is the function, the rest are arguments.
Zero-argument calls are just an empty-argument list.

```turmeric
(+ 1 2)            ; => 3
(square 9)         ; => 81
(vec-new)          ; zero-arg call
```
```sweet-exp
{1 + 2}            ; => 3
square(9)          ; => 81
vec-new()          ; zero-arg call
```

### Special forms you will see first

These are the forms a newcomer meets immediately. This guide only shows their
*shape*; follow the links for semantics.

`def` and `defn` -- bind a value or a function. Parameter and return types are
annotated with `:type`:

```turmeric
(defn square [x : int] : int (* x x))

(square 9)    ; => 81
```
```sweet-exp
defn square [x :int] :int
  {x * x}

square(9)    ; => 81
```

`let` -- local bindings scoped to its body:

```turmeric
(let [x 10 y 20] (+ x y))   ; => 30
```
```sweet-exp
let [x 10 y 20] {x + y}   ; => 30
```

`if` -- an expression; both branches required:

```turmeric
(defn abs [n : int] : int
  (if (< n 0) (- 0 n) n))
```
```sweet-exp
defn abs [n :int] :int
  if {n < 0}
    {0 - n}
    n
```

`cond` -- ordered multi-way branch with an `:else` fallback:

```turmeric
(defn sign [n : int] : int
  (cond (> n 0) 1
        (< n 0) -1
        :else   0))
```
```sweet-exp
defn sign [n :int] :int
  cond
    >(n 0)
    1
    <(n 0)
    -1
    :else
    0
```

`when` -- one-armed conditional for side effects:

```turmeric
(when (< x 0) (println "negative"))
```
```sweet-exp
when {x < 0}
  println("negative")
```

`fn` -- an anonymous function capturing its lexical scope:

```turmeric
(let [add5 (fn [x : int] : int (+ x 5))]
  (add5 10))    ; => 15
```
```sweet-exp
let [add5 fn([x :int] :int {x + 5})]
  add5(10)    ; => 15
```

`for` -- bind a counter over a half-open range:

```turmeric
(for i 0 5 (println i))
```
```sweet-exp
for i 0 5 println(i)
```

Other forms you will meet -- `do` (sequence side effects), `while` (loop while
a condition holds), `import`/`export` (module wiring), `defstruct` (record
types), and `defmacro` (syntax extension). Each has a dedicated guide:

- Binding forms (`let`, `letrec`, named let): [binding-forms-guide.md](binding-forms-guide.md)
- Structs: [structs-guide.md](structs-guide.md)
- Modules, `import`/`export`: [module-system-guide.md](module-system-guide.md)

### Type-annotation syntax

Types appear after a name with a leading colon. In a parameter list each
parameter is followed by its type; the return type follows the closing `]` of
the parameter vector:

```turmeric
(defn add [a : int b : int] : int (+ a b))
```
```sweet-exp
defn add [a :int b :int] :int
  {a + b}
```

Compound types use parenthesised constructors -- `(vec T)`, `(-> a b)`,
`forall`, and friends. A variadic tail is written `& rest :T`. The full
compound-annotation grammar is in the
[Type Annotations Guide](type-annotations-guide.md); variadic rules are in the
function-arity section of the project conventions.

### Indentation conventions

Turmeric source follows Clojure-style indentation.

**Regular calls** align arguments under the first argument:

```turmeric no-check
(some-long-fn arg1
              arg2
              arg3)
```

**Special forms and macros** (`defn`, `fn`, `let`, `if`, `when`, `do`, `cond`,
`for`, `while`) use a fixed 2-space body indent regardless of column:

```turmeric no-check
(defn greet [name :cstr] :void
  (println name))
```

**Binding vectors** align pairs under one another, one pair per line. Never
split a name from its value across lines:

```turmeric no-check
(let [x   1
      y   2
      foo (+ x y)]
  foo)
```

The canonical formatter enforces these rules automatically -- see
[Canonical formatting](#canonical-formatting) below.

### Inline-C blocks

Turmeric can embed C in a fenced ` ```c ... ``` ` block inside a `defn` body.
The closing triple-backtick **must share its line with the enclosing `)`** --
written ` ```) ` -- so that Markdown renderers do not mistake it for the end of
a surrounding documentation code fence:

```turmeric no-check
(defn file-size [f] :int
  ```c
  FILE* file = (FILE*)f;
  return (int)ftell(file);
  ```)
```

See the [C Integration Guide](c-integration-guide.md) for FFI details and the
[Reader Forms Reference](reader-forms-guide.md#inline-c-block----c--) for the
fence grammar.

---

## Part 2 -- Sweet-expression syntax

Sweet-expressions are an alternate surface syntax that removes most parentheses
without changing the underlying form tree. Every sweet-exp program reads to the
*same* AST as its s-expression equivalent -- the toggle widgets in this guide
are verified to be parse-equal.

### Opting in

Activate sweet-exp one of two ways:

- A `#lang sweet-exp` directive on the first line of a `.tur` file.
- A `.tur.sweet` file extension.

A complete runnable snippet opens with the directive:

```turmeric
(defn main [] :int 0)
```
```sweet-exp
#lang sweet-exp

defn main [] :int
  0
```

Inline fragments in prose omit the directive (as the paired examples in this
guide do). If the directive is absent and the file is not `.tur.sweet`, the
reader stays in plain s-expression mode and treats indentation as
insignificant.

### The three tools

Sweet-exp gives you three independent tools. Use whichever reduces noise for a
given expression; they compose freely.

**1. Indentation (t-expr)** -- a leading-whitespace block replaces the outer
`(...)` of a form. Use it for top-level forms and multi-line bodies:

```turmeric
(defn factorial [n : int] : int
  (if (<= n 1) 1 (* n (factorial (- n 1)))))
```
```sweet-exp
defn factorial [n :int] :int
  if {n <= 1}
    1
    {n * factorial({n - 1})}
```

**2. Neoteric `f(x y)`** -- replaces `(f x y)` for inline calls. The opening
paren must touch the function name with no space. Operators work too --
`+(x y)` is `(+ x y)`:

```turmeric
(println (vec-len v))
```
```sweet-exp
println(vec-len(v))
```

**3. Rest-of-line `$`** -- replaces the outer `(...)` when a line's only
argument is itself a single nested call. Prefer it over neoteric when the outer
call takes exactly one argument:

```turmeric
(println (vec-get squares i))
```
```sweet-exp
println(vec-get(squares i))
```

### Curly-infix `{a op b}`

Arithmetic and comparison read more naturally in infix. `{a + b}` lowers to
`(+ a b)`; nesting makes precedence visual:

```turmeric
(let [hyp (sqrt (+ (* a a) (* b b)))] hyp)
```
```sweet-exp
let [hyp sqrt({*(a a) + *(b b)})] hyp
```

### Data literals inside sweet-exp

The `#map{...}`, `#set{...}`, and `[...]` literals work transparently inside
sweet-exp -- the reader dispatch sits below the
sweet-exp layer, so neoteric and curly-infix compose inside a literal. See the
[Data Literals Guide](data-literals-guide.md) for the full semantics.

### What still uses traditional parens

A few forms stay clearer in s-expression syntax even in a sweet-exp file:

- **`import` / `export`** -- short enough that indentation adds nothing.
- **`cons` lists** -- `(cons x (cons y 0))` scans better than nested neoteric.
- **Inline-C blocks** -- the ` ```c ``` ` fence is already special; keep the
  body as-is (the enclosing `defn` may still use sweet-exp form).
- **Trivially short expressions** -- `(nil-value)`, `(ok-val r)`, and similar
  one-liners.

### Mixing styles

You can mix s-expressions and sweet-exp tools within one sweet-exp file -- the
two share a form tree, so a traditional `(...)` list is always legal inside an
indented block, and neoteric/`$`/curly-infix may appear anywhere an expression
is expected. The guidance above (when to fall back to parens) is a *style*
recommendation, not a parser restriction. The canonical formatter normalises
plain `.tur` files but does not rewrite a sweet-exp file into s-expressions or
vice versa -- the dialect is a property of the source you choose to write.

### Side-by-side: a complete function

Here is `make-adder` -- a function returning a closure -- shown in both
dialects. (Lifted from the quickstart so it stays honest.)

```turmeric
(defn make-adder [n : int] (fn [x : int] : int (+ x n)))

(let [add3 (make-adder 3)
      add7 (make-adder 7)]
  (println (add3 10))    ; 13
  (println (add7 10)))   ; 17
```
```sweet-exp
defn make-adder [n :int]
  fn [x :int] :int {x + n}

let [add3 make-adder(3)
     add7 make-adder(7)]
  println(add3(10))    ; 13
  println(add7(10))    ; 17
```

---

## Part 3 -- Reference appendix

### Form cheat sheet

One row per construct: the s-expr form, its sweet-exp form, and a one-line
gloss.

| Construct | S-expression | Sweet-expression | Gloss |
|---|---|---|---|
| Call | `(f x y)` | `f(x y)` | function application |
| Operator | `(+ x y)` | `{x + y}` or `+(x y)` | infix arithmetic / prefix op |
| One-arg call | `(f (g x))` | `f $ g(x)` | rest-of-line argument |
| Define fn | `(defn f [x :int] :int ...)` | `defn f [x :int] :int` + indent | named function |
| Anon fn | `(fn [x :int] :int ...)` | `fn([x :int] :int ...)` | closure |
| Local binding | `(let [x 1] ...)` | `let [x 1]` + indent | scoped names |
| Conditional | `(if c a b)` | `if c` + indent | two-armed expression |
| Multi-branch | `(cond p1 e1 :else e)` | `cond` + indented pairs | ordered dispatch |
| Side-effect guard | `(when c ...)` | `when c` + indent | one-armed conditional |
| Counted loop | `(for i 0 n ...)` | `for i 0 n ...` | range iteration |
| Sequence | `(do a b)` | `do` + indent | evaluate in order |
| Vector literal | `[a b c]` | `[a b c]` | growable array |
| Map literal | `#map{:k v}` | `#map{:k v}` | HAMT map |
| Set literal | `#set{a b}` | `#set{a b}` | set |

### Where to go deeper

| Topic | In-depth guide |
|---|---|
| Every reader form / literal grammar | [reader-forms-guide.md](reader-forms-guide.md) |
| Data literal semantics | [data-literals-guide.md](data-literals-guide.md) |
| Local binding forms | [binding-forms-guide.md](binding-forms-guide.md) |
| Compound type annotations | [type-annotations-guide.md](type-annotations-guide.md) |
| Structs | [structs-guide.md](structs-guide.md) |
| Modules / `import` / `export` | [module-system-guide.md](module-system-guide.md) |
| C interop / inline-C | [c-integration-guide.md](c-integration-guide.md) |
| Canonical formatting | [formatter-guide.md](formatter-guide.md) |
| End-to-end prose tour | [quickstart.md](quickstart.md) |

### Canonical formatting

`tur format` rewrites a file into canonical layout -- applying the indentation
rules above so you never have to hand-align. Use `--check` in CI to fail on
drift:

```turmeric no-check
tur format myfile.tur          # rewrite in place (via redirect)
tur format --check myfile.tur  # exit non-zero if not canonical
```

A before/after gives the flavour -- ragged input on the left, canonical output
on the right:

```turmeric no-check
;; before
(defn add [a :int b :int] :int
    (+ a
  b))

;; after `tur format`
(defn add [a :int b :int] :int
  (+ a b))
```

See the [Formatter Guide](formatter-guide.md) for the complete rule set and
editor integration.

### Common mistakes

A short list of pitfalls newcomers hit:

1. **Inline-C fence on its own line.** The closing ` ``` ` must share a line
   with `)` (written ` ```) `); a lone ` ``` ` breaks Markdown rendering.
2. **Splitting a binding pair across lines.** Keep each `name value` (or
   `name :type value`) on one line in a `let`/`loop` vector.
3. **A space before a neoteric paren.** `f (x)` is *not* neoteric application
   -- it reads as `f` followed by `(x)`. Write `f(x)`.
4. **Non-ASCII bytes.** An em dash or smart quote pasted from a doc will hang
   the reader. Use `--` and straight quotes only.
5. **Expecting `#lang sweet-exp` to enable neoteric in a plain file.** Without
   the directive (or a `.tur.sweet` extension) the reader stays in s-expression
   mode and indentation is insignificant.
6. **Mixing up `[...]` positions.** `[...]` is a value (lowers to `vec-of`)
   in expression position and a binding spec (parameter list / `let` bindings)
   in binding position -- the context determines which.
