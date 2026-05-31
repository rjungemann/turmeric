---
title: Binding Forms Guide
category: Language Basics
description: Internal define, letrec, and named-let -- the three local binding idioms that complement let and defn
---

# Binding Forms Guide

Turmeric provides three complementary local-binding primitives beyond `let`:
`define` for sequential body-level bindings, `letrec` for mutually-recursive
helpers, and *named let* for tail-recursive loops. All three desugar through
the same elaborator primitive so their typing, continuation-discipline, and
codegen are consistent with `let`.

---

## `define` -- Body-level sequential binding

`define` introduces a name inside a *body* -- the body of a `defn`, `fn`,
`let`, or `do`. It splices a `let` wrapping all subsequent forms:

```turmeric
(do
  (define x 1)
  (define y (+ x 1))
  (println y))         ;; prints 2
```

This is exactly equivalent to:

```turmeric
(do
  (let [x 1]
    (let [y (+ x 1)]
      (println y))))
```

Sweet-exp:

```sweet-exp
do
  define x 1
  define y +(x 1)
  println(y)
```

### Supported body positions

`define` works anywhere a *body sequence* is valid:

| Form | Example |
|------|---------|
| `defn` body | `(defn f [] (define x 1) x)` |
| `fn` body | `(fn [] (define x 1) x)` |
| `let` body | `(let [y 2] (define x y) x)` |
| `do` body | `(do (define x 1) x)` |

`define` is **not** valid at top level (use `def` there), in `if` branches,
or anywhere that is not a body sequence.

### Annotations

All annotations that `let` accepts work on `define`:

```turmeric
(defn counter [] :int
  (define ^mut n 0)
  (set! n (+ n 1))
  n)
```

Type annotations are written after the name:

```turmeric
(define total :int (+ a b))
```

### Semantics: let\* (sequential)

Each `define` sees all earlier bindings but **not** later ones -- the same
rule as `let`. Self-recursion inside a `define` init does not work:

```turmeric
;; Error: f is not in scope inside its own init
(define f (fn [n] (f n)))

;; Fix: use letrec (see below) or lift f to top-level defn
```

For mutually-recursive helpers, use `letrec`.

---

## `letrec` -- Mutually-recursive local bindings

`letrec` is like `let` but pre-registers every name in the binding group
before any init is elaborated, so each init can reference any name in the
group:

```turmeric
(defn run [] :int
  (letrec [even? (fn [n :int] :bool (if (= n 0) true  (odd?  (- n 1))))
           odd?  (fn [n :int] :bool (if (= n 0) false (even? (- n 1))))]
    (println (if (even? 10) "even" "odd"))
    0))
```

Sweet-exp:

```sweet-exp
defn run [] :int
  letrec [even? fn([n :int] :bool if(=(n 0) true  odd?(-(n 1))))
          odd?  fn([n :int] :bool if(=(n 0) false even?(-(n 1))))]
    println $ if(even?(10) "even" "odd")
    0
```

### Self-recursion

A single self-recursive function is the common case:

```turmeric
(defn main [] :int
  (letrec [fact (fn [n :int] :int
                  (if (= n 0) 1 (* n (fact (- n 1)))))]
    (println (fact 5))   ;; 120
    0))
```

### Type annotation requirement

For mutual recursion, annotate the return type so the elaborator can
build placeholder types before the bodies are checked:

```turmeric
(letrec [a (fn [n :int] :int (b (- n 1)))
         b (fn [n :int] :int (if (= n 0) 0 (a n)))]
  (a 3))
```

Omitting `:int` on mutually-referencing functions causes an "unknown type"
error; add `:ret` annotations to resolve it.

### Non-function bindings

`letrec` allows non-function bindings as long as they do not self-reference:

```turmeric
;; Fine: x is a plain value, no self-reference
(letrec [x 42]
  x)

;; Error: x cannot reference itself during initialization
(letrec [x (+ x 1)]
  x)
```

---

## Named `let` -- Tail-recursive loops

Named let is the standard Scheme/Racket idiom for tail-recursive iteration.
Write `(let loop [bindings...] body...)` to both introduce a local function
`loop` and call it with the initial argument values:

```turmeric
;; Sum 1..n
(defn sum [n :int] :int
  (let loop [i n acc 0]
    (if (= i 0)
      acc
      (loop (- i 1) (+ acc i)))))
```

Sweet-exp:

```sweet-exp
defn sum [n :int] :int
  let loop [i n acc 0]
    if =(i 0)
      acc
      loop(-(i 1) +(acc i))
```

The named let desugars to:

```turmeric
(letrec [loop (fn [i :int acc :int] :int
                (if (= i 0) acc (loop (- i 1) (+ acc i))))]
  (loop n 0))
```

### Type annotations in named let

Annotate binding names the same way as `let`:

```turmeric
(let loop [xs :int n :int 0]
  (if (= xs 0)
    n
    (loop (cons-tail xs) (+ n 1))))
```

---

## Choosing the right form

| Situation | Reach for |
|---|---|
| Simple sequential local name | `define` or `let` |
| Self-recursive local function | `letrec [f (fn ...)]` |
| Mutually recursive local helpers | `letrec [f (fn ...) g (fn ...)]` |
| Tail-recursive loop with initial values | Named `let` |
| Top-level constant or function | `def` / `defn` |

`define` and `let` share sequential semantics: pick whichever reads more
naturally in context. `define` saves one level of indentation when the body
is long; `let` keeps the binding and its use visually co-located.

Named `let` is idiomatic for loops; `letrec` is idiomatic for mutual helpers
that are too specific to lift to top level.

---

## See also

- [Style Guide](style-guide.md) -- indentation rules for `let` / `do` bodies
- [tur/macros API](../api/tur-macros.html) -- `for`, `cond`, `when`, and other loop/control macros
