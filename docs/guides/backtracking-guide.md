---
title: Backtracking Guide
category: Advanced Control Flow
description: Nondeterministic backtracking using the list monad in Turmeric's stdlib/backtrack.tur
---

# Turmeric Backtracking Guide

## Overview

Turmeric's backtracking support is implemented as a **list monad** in
`stdlib/backtrack.tur`. Computations that may yield multiple results (or no
result) are represented as linked lists of `int64_t` values. This gives you
classic nondeterministic/backtracking semantics with a familiar monadic API.

---

## Core Operations

| Function | Signature | Description |
|---|---|---|
| `mzero` | `() → :int` | The empty list -- zero alternatives (failure) |
| `mreturn` | `(x) → :int` | Wrap a single value -- one alternative (success) |
| `mplus` | `(xs ys) → :int` | Concatenate two alternative lists |
| `mbind` | `(ma f) → :int` | Flatmap: apply `f` to each alternative |
| `guard` | `(pred :bool) → :int` | Keep current branch if `pred` is true, else fail |
| `fresh` | `(lo hi) → :int` | Return all integers in `[lo, hi)` as alternatives |
| `once` | `(xs) → :int` | Truncate to first alternative |
| `interleave` | `(xs ys) → :int` | Fair interleaving of two streams |
| `run-backtrack` | `(xs) → :int` | Identity -- return all results |
| `run-backtrack-depth` | `(xs n) → :int` | Return first N results only |

---

## Basic Usage

```turmeric
;; Return all even numbers from 1..10
(let [evens (mbind (fresh 1 11)
                   (fn [x]
                     (if (= (mod x 2) 0)
                       (mreturn x)
                       (mzero))))]
  (bt-print (run-backtrack evens)))

; Outputs:
;
; 2
; 4
; 6
; 8
; 10
```

```sweet-exp
;; Return all even numbers from 1..10
let [evens mbind(fresh(1 11)
                 (fn [x]
                   (if (= (mod x 2) 0)
                     (mreturn x)
                     (mzero))))]
  bt-print(run-backtrack(evens))

; Outputs:
;
; 2
; 4
; 6
; 8
; 10
```

---

## `backtrack-do` Macro

The `backtrack-do` macro provides Haskell-`do`-notation-style sequencing for
the backtracking monad:

```turmeric
;; Pythagorean triples with a+b+c = 24
(backtrack-do
  a (fresh 1 24)
  b (fresh a 24)
  c (mreturn (- 24 (+ a b)))
  _ (guard (= (* a a) (+ (* b b) (* c c))))
  (mreturn (list a b c)))
```

```sweet-exp
;; Pythagorean triples with a+b+c = 24
backtrack-do
  a
  fresh(1 24)
  b
  fresh(a 24)
  c
  mreturn({24 - {a + b}})
  _
  guard((= (* a a) (+ (* b b) (* c c))))
  mreturn(list(a b c))
```

Each `var expr` line binds `var` to each alternative produced by `expr`. The
final expression is the body for each combination. `_` discards the value when
you only care about side-effects (e.g., `guard`).

---

## Depth Limiting

Use `run-backtrack-depth` when you only need the first N results:

```turmeric
;; Take only the first 5 solutions
(bt-print (run-backtrack-depth all-solutions 5))
```

```sweet-exp
;; Take only the first 5 solutions
bt-print(run-backtrack-depth(all-solutions 5))
```

You can also pass `--backtrack-depth N` to the compiler as a flag, which emits
`#define BACKTRACK_DEPTH_DEFAULT N` in the generated C preamble -- useful for
runtime dispatch when the stdlib is extended to check this constant.

---

## N-Queens Example

```turmeric
;; Count solutions to N-Queens for N=6 using backtrack-do
(defn safe? [col row packed n] : bool
  ;; check if placing at col,row is safe given previous placements in packed
  ...)

(defn queens [n] : int
  (let [result (mreturn 0)]  ;; start with empty board (encoded as 0)
    (let [board (mbind result (fn [packed]
      ;; for each row 0..n-1, expand the board
      ...))]
      board)))
```

```sweet-exp
;; Count solutions to N-Queens for N=6 using backtrack-do
defn safe? [col row packed n] :bool
  ;; check if placing at col,row is safe given previous placements in packed
  ...

defn queens [n] :int
  let [result mreturn(0)]  ;; start with empty board (encoded as 0)
    let [board mbind(result
                     (fn [packed]
                       ;; for each row 0..n-1, expand the board
                       ...))]
      board
```

See `tests/fixtures/backtrack-n-queens/input.tur` for the full self-contained
implementation.

---

## Integration with Algebraic Effects

The backtracking monad can be combined with algebraic effects. Effects run
outside the monad boundary and the results are wrapped with `mreturn`:

```turmeric
(defeffect Choose [a :int b :int] :int)

(defn make-choice [] : int
  (let [x (perform (Choose 10 20))]
    (mreturn (* x 2))))

(defn main []
  (let [result (handle (make-choice)
                 (Choose [a b] k)
                 (resume k a))]   ;; always pick 'a'
    (bt-print result)))
```

```sweet-exp
defeffect Choose [a :int b :int] :int

defn make-choice [] :int
  let [x perform(Choose(10 20))]
    mreturn({x * 2})

defn main []
  let [result (handle (make-choice)
                (Choose [a b] k)
                (resume k a))]   ;; always pick 'a'
    bt-print(result)
```

See `tests/fixtures/backtrack-integration-effects/input.tur` for a complete
working example.

---

## Sudoku Solver Example

The file `tests/fixtures/backtrack-sudoku/input.tur` demonstrates a 4×4
mini-Sudoku solver using iterative backtracking within inline C code. It
produces the unique solution to the given puzzle.

---

## Memory and Performance

- Every alternative is a heap-allocated `Cell { int64_t value; int64_t next; }`.
- Use `run-backtrack-depth` to bound result count and avoid building large lists.
- For performance-sensitive use, consider using the benchmarks in
  `benchmarks/bench-backtrack-n-queens.tur` as a baseline.
- The `--dump-clone-plan` compiler flag prints a summary of every
  `cloneable-shift` site with its captured bindings and resolved `Clone`
  instance methods -- useful for debugging continuation capture overhead.

---

## Compiler Flags (Phase B5)

| Flag | Description |
|---|---|
| `--backtrack-depth N` | Emit `#define BACKTRACK_DEPTH_DEFAULT N` in the C preamble |
| `--dump-clone-plan` | After the CPS pass, print each cloneable-shift site to stderr |

---

## Residual Liveness Imprecision (1.0 limitation)

The compile-time capture check (`TUR-E0014`) is conservative: it flags every
binding in scope at a `cloneable-shift` site inside the **same function** that
lacks a Clone instance, even if that binding is not actually used in the
continuation body.  Bindings from **enclosing functions** (outer lambdas or
defns) are excluded from the check (CF7.3), but same-function bindings that
are merely in lexical scope -- not live -- may be falsely rejected.

**Workaround:** move non-Clone values out of scope before the shift:

```turmeric
;; Instead of:
(let [nc (make-struct NoClone x)]
  (cloneable-reset
    (let [k (cloneable-shift k-fn 0)]
      ...)))  ;; nc is in scope even if unused => TUR-E0014

;; Do:
(let [nc (make-struct NoClone x)
      result (extract nc)]   ;; consume nc before the reset
  (cloneable-reset
    (let [k (cloneable-shift k-fn 0)]
      result)))               ;; only `result` (Clone-able) is in scope
```

Full liveness precision is gated on the post-1.0 CPS pass (tracked in
[control-flow-completeness-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/archive/history/control-flow-completeness-plan.md) CF7.5).

---

## See Also

- [tur/backtrack API](../api/tur-backtrack.html) -- standard library implementation
- [parser-combinators-tutorial.md](parser-combinators-tutorial.md) -- worked example: parser combinators built directly on the list monad described here
- [`tests/fixtures/backtrack-*/`](https://github.com/rjungemann/turmeric/tree/main/tests/fixtures/) -- test fixtures for all backtracking features
- [`benchmarks/bench-backtrack-*.tur`](https://github.com/rjungemann/turmeric/tree/main/benchmarks/) -- performance benchmarks
