---
title: Reader Forms Reference
category: Reference
description: Complete reference for every syntactic form the Turmeric reader recognises -- comments, literals, collections, prefix macros, and reader extensions
---

# Reader Forms Reference

The Turmeric reader converts source text into a form tree before the compiler
sees it. This guide documents every construct the reader understands, organised
by category.

---

## Comments

Comments are consumed by the reader and produce no form.

### Line comment -- `;`

Ignores everything from `;` to the end of the line.

```turmeric
(println "hello")  ; this is ignored
```

### Block comment -- `#| ... |#`

Ignores a delimited region of text. Block comments nest.

```turmeric
#|
  This whole region is ignored.
  (println "never runs")
  #| nested block comments work too |#
|#
(println "ok")
```

### Datum comment -- `#;` *(planned)*

Reads one complete datum and discards it. The surrounding code sees nothing.
Unlike `#| |#`, `#;` understands structure -- it always discards exactly one
form, no matter how many lines it spans.

```turmeric
(println #;99 "ok")              ; => ok
(println #;"ignored" "ok")       ; => ok
(println #;(this is discarded) "ok")  ; => ok
(println (+ 1 #;999 2))          ; => 3
```

Datum comments compose: `#;#;1 2 3` discards `2` (the result that inner `#;1`
would have produced) and leaves `3`.

```turmeric
(println #;#;1 2 3)   ; => 3
```

`#;` at end of file, or immediately before a closing delimiter, is an error.

---

## Literals

### Integer

Decimal, hexadecimal (`0x`/`0X`), and binary (`0b`/`0B`) are supported.
An optional type suffix pins the width.

```turmeric
42
-7
0xff
0b1010
255u8
1000i32
```

| Suffix | Type |
|--------|------|
| `i8` | signed 8-bit |
| `i16` | signed 16-bit |
| `i32` | signed 32-bit |
| `i64` | signed 64-bit |
| `u8` | unsigned 8-bit |
| `u16` | unsigned 16-bit |
| `u32` | unsigned 32-bit |
| `u64` | unsigned 64-bit |

### Float

A `.` or exponent (`e`/`E`) makes a literal a float. The suffix `f32` or `f64`
pins the width; unsuffixed floats are `f64`.

```turmeric
3.14
1.0e10
2.5f32
1f64
```

### String -- `"..."`

Double-quoted UTF-8 text. Standard C escape sequences apply (`\n`, `\t`, `\\`,
`\"`, etc.).

```turmeric
"hello, world"
"line one\nline two"
```

### Keyword -- `:`

A colon followed immediately by a name produces a keyword (an interned
compile-time constant).

```turmeric
:ok
:error
:tur
```

### Symbol / identifier

Any name that starts with a letter or one of `+ - * / = < > ! ? _ $ & . ^ |`
or a UTF-8 code-point in the symbol-start set (includes `λ`, `∀`, `∃`).

```turmeric
foo
my-var
+
->
|>
λ
```

---

## Collections

### List -- `(...)`

An ordered sequence of forms. The primary syntactic unit in Turmeric.

```turmeric
(println "hello")
(+ 1 2 3)
(defn square [x :int] :int (* x x))
```

### Vector -- `[...]`

A vector literal.

```turmeric
[1 2 3]
[:a :b :c]
```

### Map literal -- `#{...}`

A map (hash-array-mapped trie) literal. Key-value pairs are written
interleaved.

```turmeric
#{:name "Alice" :age 30}
```

### Set literal -- `#s(...)`

A set literal.

```turmeric
#s(1 2 3)
#s(:red :green :blue)
```

### Contract type -- `{ var : T | pred }`

A refinement type annotation. Introduces a binding `var` of type `T` with
predicate `pred`.

```turmeric
{ x : :int | (> x 0) }
```

---

## Prefix Reader Macros

Each of these reads one following datum and rewrites it into a longer form.

### Quote -- `'expr`

Expands to `(quote expr)`. Prevents evaluation.

```turmeric
'foo          ; => (quote foo)
'(1 2 3)      ; => (quote (1 2 3))
```

### Quasiquote -- `` `expr ``

Expands to `(quasiquote expr)`. Like quote, but `~` and `~@` splice in values.

```turmeric
`(1 2 3)             ; => (quasiquote (1 2 3))
`(1 ~x 3)            ; unquotes x
`(1 ~@xs 3)          ; splices list xs
```

### Unquote -- `~expr`

Valid only inside a quasiquote. Expands to `(unquote expr)`.

```turmeric
`(a ~b c)   ; b is evaluated and inserted
```

### Unquote-splicing -- `~@expr`

Valid only inside a quasiquote. Expands to `(unquote-splicing expr)`. The
value must be a list; its elements are spliced into the surrounding list.

```turmeric
(let [xs '(2 3)]
  `(1 ~@xs 4))   ; => (1 2 3 4)
```

### Deref -- `@expr`

Expands to `(deref expr)`.

```turmeric
@my-ref   ; => (deref my-ref)
```

### Effect-row annotation -- `@{...}`

A `@` immediately followed by `{` reads a brace-delimited effect-row map
rather than a deref.

```turmeric
@{IO}
@{IO Exn}
```

### Borrow -- `&expr`

Expands to `(& expr)`. A bare `&` followed by whitespace or a close delimiter
is left as the symbol `&` (preserving the explicit `(& x)` call form).

```turmeric
&x       ; => (& x)
(& x)    ; identical -- explicit form still works
```

### Mutable borrow -- `&mut expr`

Expands to `(&mut expr)`.

```turmeric
&mut x   ; => (&mut x)
```

---

## Hash-Dispatch Forms

Forms beginning with `#` followed by a specific character.

### Attribute -- `#[...]`

Attaches a compile-time attribute to the next definition.

```turmeric
#[no-unwind]
(defn risky [] :int ...)
```

### Reader conditional -- `#?(...)`

Selects a branch based on the current reader target. Use `:tur` for compiled
output and `:turi` for the interpreter.

```turmeric
(println #?(:tur "compiled" :turi "interpreted"))
```

---

## Inline C Block -- ` ```c ... ``` `

A triple-backtick block embeds raw C code inside a `defn` body. The closing
` ``` ` must be on the same line as its enclosing `)`.

```turmeric
(defn file-size [f] :int
  ```c
  FILE* file = (FILE*)f;
  return (int)ftell(file);
  ```)
```

---

## Sweet-Expression Extensions

The following forms are active when sweet-expression mode is enabled
(`#lang sweet-exp` or the `--sweet` flag).

### Neoteric application -- `f(...)`

A symbol (or atom) followed immediately by `(` (no space) is read as a
function call.

```turmeric
square(9)     ; => (square 9)
```

### Curly-infix -- `{a op b}`

Braces in sweet-exp mode treat the middle element as an infix operator.

```turmeric
{1 + 2}       ; => (+ 1 2)
{x > 0}       ; => (> x 0)
```
