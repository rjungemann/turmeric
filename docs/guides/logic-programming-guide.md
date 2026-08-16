---
title: Logic Programming Guide
category: Advanced Control Flow
description: Backtracking, logic programming, constraint solving with cloneable continuations
---

# Logic Programming with Backtracking

Implementing miniKanren-style logic programming and constraint solving using cloneable continuations.

## Overview

Turmeric supports **multi-shot (cloneable) continuations**, enabling backtracking computation. This guide covers the design, use cases, and integration with Turmeric's ownership model.

## Motivation

Backtracking enables a declarative programming style where the system **automatically explores alternatives**:

| Use Case | Example | Benefit |
|----------|---------|---------|
| **Parsing** | Parser combinators with choice | Clean, composable grammar definitions |
| **Logic Programming** | miniKanren-style queries | Bidirectional computation |
| **Constraint Solving** | Sudoku, SAT solvers | Automatic search with pruning |
| **Probabilistic Programming** | Bayesian inference | Explore multiple execution paths |
| **Game AI** | Move exploration | Try strategies, backtrack on failure |

### One-shot vs. cloneable

Plain delimited continuations are **one-shot**: calling `continue k v` consumes `k`, matching Turmeric's ownership model but preventing backtracking. **Cloneable continuations** enable multi-shot by requiring all captured values to implement a `Clone` trait.

## Core Abstraction: Cloneable Continuations

A cloneable continuation can be resumed multiple times:

```turmeric
;; Surface syntax (sugar)
(cloneable-reset
  (fn []
    body))

(cloneable-shift [k]
  body)
```

```sweet-exp
;; Surface syntax (sugar)
cloneable-reset
  fn []
    body

cloneable-shift [k]
  body
```

### The Clone Trait

Every type captured by a cloneable continuation must implement `Clone`:

```turmeric
;; Clone primitives (copy semantics)
(instance Clone int64 (clone [x] x))
(instance Clone bool (clone [x] x))

;; Clone derived types (deep copy)
(instance Clone (Pair a b) [Clone a, Clone b]
  (clone [x] (Pair (clone x.first) (clone x.second))))

(instance Clone (Vector a) [Clone a]
  (clone [x] (map clone x)))
```

```sweet-exp
;; Clone primitives (copy semantics)
instance Clone int64
  clone [x] x
instance Clone bool
  clone [x] x

;; Clone derived types (deep copy)
instance Clone (Pair a b) [Clone a, Clone b]
  clone [x]
    Pair(clone(x.first) clone(x.second))

instance Clone (Vector a) [Clone a]
  clone [x]
    map(clone x)
```

**Key invariant:** Before a cloneable continuation is resumed, all values in its environment must be cloned. This means:

- Expensive for large environments, cheap for small ones.
- Limits mutation: captured `ref<T>` values should be immutable or used carefully.
- Encourages functional style: use immutable data structures.

## The Backtrack Monad

Multi-shot continuations are modeled as the **backtracking monad**:

A `Backtrack` computation over element type `A` is a **thunk that yields a
typed cons-list of results**: forcing the thunk runs the search one level, and
each `(Cons A)` element is one answer. The monad is parametric -- the same
`pure`/`mplus`/`bind` serve `int` searches and `float` searches alike.

The recurring shape `(fn [] (Cons A))` is spelled inline below because
`defalias` does not take type parameters yet -- a *monomorphic* alias like
`(defalias Backtrack (fn [] int))` works fine if you fix the element type, and
is transparent (the alias and `(fn [] int)` are the same type at every use
site, so `(fs)` applies exactly as it would without it). `deftype` is the
wrong tool for naming either shape -- it binds a recursive `TY_REC`, a
distinct nominal type that makes `(fs)` fail with `'fs' is not a function or
continuation`.

```turmeric
;; mzero -- the empty computation (no results)
(defn mzero [A] [] : (fn [] (Cons A))
  (fn [] (:: (tnil) (Cons A))))

;; pure -- a computation with exactly one result
(defn pure [A] [x : A] : (fn [] (Cons A))
  (fn [] (tcons x (tnil))))

;; mplus -- choice: all of fs's results, then all of gs's
(defn mplus [A] [^fat fs : (fn [] (Cons A)) ^fat gs : (fn [] (Cons A))] : (fn [] (Cons A))
  (fn [] (:: (list-concat (fs) (gs)) (Cons A))))

;; flat-map over a typed cons list; f returns a list per element
(defn list-flat-map [A] [xs : (Cons A) ^fat f : (fn [A] (Cons A))] : (Cons A)
  (if (tnil? xs)
    (:: (tnil) (Cons A))
    (:: (list-concat (f (thead xs))
                     (list-flat-map (:: (ttail xs) (Cons A)) f))
        (Cons A))))

;; bind -- sequence: run xs, then f on each result, concatenating
(defn bind [A] [^fat xs : (fn [] (Cons A)) ^fat f : (fn [A] (Cons A))] : (fn [] (Cons A))
  (fn [] (list-flat-map (xs) f)))
```

```sweet-exp
;; mzero -- the empty computation (no results)
defn mzero [A] [] : (fn [] (Cons A))
  fn []
    :: tnil() (Cons A)

;; pure -- a computation with exactly one result
defn pure [A] [x : A] : (fn [] (Cons A))
  fn []
    tcons(x tnil())

;; mplus -- choice: all of fs's results, then all of gs's
defn mplus [A] [^fat fs : (fn [] (Cons A)) ^fat gs : (fn [] (Cons A))] : (fn [] (Cons A))
  fn []
    :: list-concat(fs() gs()) (Cons A)

;; flat-map over a typed cons list; f returns a list per element
defn list-flat-map [A] [xs : (Cons A) ^fat f : (fn [A] (Cons A))] : (Cons A)
  if tnil?(xs)
    :: tnil() (Cons A)
    ::
      list-concat
        f(thead(xs))
        list-flat-map (:: ttail(xs) (Cons A)) f
      (Cons A)

;; bind -- sequence: run xs, then f on each result, concatenating
defn bind [A] [^fat xs : (fn [] (Cons A)) ^fat f : (fn [A] (Cons A))] : (fn [] (Cons A))
  fn []
    list-flat-map(xs() f)
```

Driving it -- `mplus` offers both branches, `bind` maps over every result, and
the same monad instantiates at `int` and at `float` with no casts at the use
site:

```turmeric
;; Force every result and print it with p
(defn print-all [A] [xs : (Cons A) ^fat p : (fn [A] void)] : void
  (if (tnil? xs)
    nil
    (do (p (thead xs))
        (print-all (:: (ttail xs) (Cons A)) p))))

(print-all ((mplus (pure 1) (pure 2)))
           (fn [x : int] (println x)))
;; => 1
;; => 2

(print-all ((bind (mplus (pure 1) (pure 2))
                  (fn [x : int] (tcons (* x 10) (tnil)))))
           (fn [x : int] (println x)))
;; => 10
;; => 20

(print-all ((mplus (pure 7.1) (pure 2.5)))
           (fn [x : float] (println x)))
;; => 7.1
;; => 2.5
```

> **Two representation facts shape the code above.**
>
> - The closures are `^fat` -- they capture, so they ride as fat
>   `{thunk, env}` values, and the parameter annotations say so.
> - List **tails** are int-carried: `ttail` returns `:int` and `list-concat`
>   is int-typed, which is why the `(:: ... (Cons A))` ascriptions appear on
>   tail recursions and around each `list-concat` result. Passing a
>   `(Cons A)` *into* an int-typed slot needs no cast (the carrier direction
>   is admitted); only the way back up is spelled out.

## Example: Parsing with Backtracking

A backtracking parser tries multiple production rules:

```turmeric
;; Token parser
(defn parse-token [t input]
  (if (= (car input) t)
    (return (cdr input))
    mzero))

;; Choice: try parseA, then parseB if parseA fails
(defn <|> [parseA parseB input]
  (mplus (parseA input) (parseB input)))

;; Sequence: parseA then parseB
(defn >> [parseA parseB input]
  (bind (fn [rest] (parseB rest))
        (parseA input)))

;; Grammar:
;; expr := term ('+' term)*
(defn parse-expr [input]
  (<|>
    (do-backtrack
      (def rest (parse-term input))
      (def rest (parse-many (parse-plus rest)))
      (return rest))
    (parse-term input)))
```

```sweet-exp
;; Token parser
defn parse-token [t input]
  if {car(input) = t}
    return(cdr(input))
    mzero

;; Choice: try parseA, then parseB if parseA fails
defn <|> [parseA parseB input]
  mplus(parseA(input) parseB(input))

;; Sequence: parseA then parseB
defn >> [parseA parseB input]
  bind
    fn [rest] parseB(rest)
    parseA(input)

;; Grammar:
;; expr := term ('+' term)*
defn parse-expr [input]
  <|>
    do-backtrack
      def rest parse-term(input)
      def rest parse-many(parse-plus(rest))
      return(rest)
    parse-term(input)
```

## Example: Logic Programming with miniKanren

Relational queries that work in multiple directions:

```turmeric no-check
;; Unification: make two values equal
(defn unify [x y subst]
  (cond
    [(== x y) (return subst)]
    [(lvar? x) (ext-s x y subst)]
    [(lvar? y) (ext-s y x subst)]
    [(and (pair? x) (pair? y))
     (bind (fn [s] (unify (cdr x) (cdr y) s))
           (unify (car x) (car y) subst))]
    [:else mzero]))

;; Relation: append(x, y, z) :- z = x ++ y
(defn appendo [x y z]
  (<|>
    ;; Base case: x = [], z = y
    (bind (fn [s] (unify y z s)) (unify x [] {}))
    ;; Recursive: x = [h|t], z = [h|r], append(t, y, r)
    (bind (fn [s]
            (let [h (lvar 'h)
                  t (lvar 't)
                  r (lvar 'r)]
              (appendo t y r)))
          (unify x (cons (lvar 'h) (lvar 't)) {}))))

;; Query: (appendo [1 2] [3 4] X) => X = [1 2 3 4]
(run 1 [x]
  (appendo [1 2] [3 4] x))
```

```sweet-exp
;; Unification: make two values equal
defn unify [x y subst]
  cond
    ==(x y)  return(subst)
    lvar?(x)  ext-s(x y subst)
    lvar?(y)  ext-s(y x subst)
    and(pair?(x) pair?(y))
      bind(fn [s] unify(cdr(x) cdr(y) s)
           unify(car(x) car(y) subst))
    :else  mzero

;; Relation: append(x, y, z) :- z = x ++ y
defn appendo [x y z]
  <|>
    ;; Base case: x = [], z = y
    bind(fn [s] unify(y z s)  unify(x [] {}))
    ;; Recursive: x = [h|t], z = [h|r], append(t, y, r)
    bind
      fn [s]
        let [h lvar('h)
             t lvar('t)
             r lvar('r)]
          appendo(t y r)
      unify(x cons(lvar('h) lvar('t)) {})

;; Query: (appendo [1 2] [3 4] X) => X = [1 2 3 4]
run 1 [x]
  appendo([1 2] [3 4] x)
```

## Example: Constraint Solving (Sudoku)

```turmeric
(defn sudoku [grid]
  ;; grid is a 9x9 array with some cells filled (1-9), others unbound (lvar)
  
  ;; For each cell, either it's already bound or we bind it to 1-9
  (defn choose-domain [cell]
    (if (lvar? cell)
      (choice-point [1 2 3 4 5 6 7 8 9])
      (return cell)))
  
  (defn all-different [xs]
    ;; Constraint: all values in xs must be distinct
    (bind (fn [vs]
            (if (distinct? vs)
              (return vs)
              mzero))
          (map-backtrack choose-domain xs)))
  
  ;; Run constraints: rows, columns, 3x3 boxes all distinct
  (do-backtrack
    (def filled (map choose-domain grid))
    (map all-different (rows filled))
    (map all-different (cols filled))
    (map all-different (boxes filled))
    (return filled)))

;; Solve and get first 10 solutions
(run 10 [solution]
  (sudoku initial-grid))
```

```sweet-exp
defn sudoku [grid]
  ;; grid is a 9x9 array with some cells filled (1-9), others unbound (lvar)

  ;; For each cell, either it's already bound or we bind it to 1-9
  defn choose-domain [cell]
    if lvar?(cell)
      choice-point([1 2 3 4 5 6 7 8 9])
      return(cell)

  defn all-different [xs]
    ;; Constraint: all values in xs must be distinct
    bind
      fn [vs]
        if distinct?(vs)
          return(vs)
          mzero
      map-backtrack(choose-domain xs)

  ;; Run constraints: rows, columns, 3x3 boxes all distinct
  do-backtrack
    def filled map(choose-domain grid)
    map(all-different rows(filled))
    map(all-different cols(filled))
    map(all-different boxes(filled))
    return(filled)

;; Solve and get first 10 solutions
run 10 [solution]
  sudoku(initial-grid)
```

## Integration with Turmeric's Ownership Model

### Challenge: Capturing Mutable State

Turmeric's `ref<T>` assumes linear consumption (move semantics). Cloneable continuations re-execute the captured code, trying to consume the same `ref<T>` multiple times:

```turmeric
;; ERROR: re-executing code will consume the ref twice
(cloneable-reset
  (fn []
    (let [r (ref 42)]
      (choice-point [1 2]))))  ; captures r; next backtrack tries to use r again
```

```sweet-exp
;; ERROR: re-executing code will consume the ref twice
cloneable-reset
  fn []
    let [r ref(42)]
      choice-point([1 2])  ; captures r; next backtrack tries to use r again
```

**Solution:** Capture immutable data or use `rc<T>` for shared ownership:

```turmeric
;; OK: captured value is immutable
(cloneable-reset
  (fn []
    (let [x 42]  ; immutable
      (choice-point [1 2]))))

;; OK: shared ownership doesn't consume on re-entry
(cloneable-reset
  (fn []
    (let [r (rc 42)]
      (choice-point [1 2]))))
```

```sweet-exp
;; OK: captured value is immutable
cloneable-reset
  fn []
    let [x 42]  ; immutable
      choice-point([1 2])

;; OK: shared ownership doesn't consume on re-entry
cloneable-reset
  fn []
    let [r rc(42)]
      choice-point([1 2])
```

### Defer and Cloneable Continuations

When a cloneable continuation captures state across a `defer` boundary, each clone must re-run the deferred cleanup on entry:

```turmeric
;; When backtracking re-enters this block,
;; the file is re-opened in the cloned continuation
(cloneable-reset
  (fn []
    (defer-with-close (open-file "data.txt")
      (fn [f]
        (let [data (read f)]
          (choice-point (parse data)))))))
```

```sweet-exp
;; When backtracking re-enters this block,
;; the file is re-opened in the cloned continuation
cloneable-reset
  fn []
    defer-with-close open-file("data.txt")
      fn [f]
        let [data read(f)]
          choice-point(parse(data))
```

This is safe but can be expensive. Prefer immutable snapshots where possible.

## API Summary

### Core Operators

```turmeric
;; Delimit a cloneable computation
(cloneable-reset body)

;; Capture continuation for backtracking
(cloneable-shift [k] body)

;; Choice point: try multiple values
(choice-point [v1 v2 ...])

;; Run a backtracking computation, collect N solutions
(run n [vars] body)

;; Run all solutions
(run* [vars] body)

;; Constraint: succeed if predicate holds
(constraint pred)
```

```sweet-exp
;; Delimit a cloneable computation
cloneable-reset body

;; Capture continuation for backtracking
cloneable-shift [k] body

;; Choice point: try multiple values
choice-point([v1 v2 ...])

;; Run a backtracking computation, collect N solutions
run n [vars] body

;; Run all solutions
run* [vars] body

;; Constraint: succeed if predicate holds
constraint pred
```

### Monadic Interface

```turmeric
(return x)           ; succeed with one value
(mzero)              ; fail (zero solutions)
(mplus fs gs)        ; choice between fs and gs
(bind f xs)          ; flatMap: sequence computations

;; Syntactic sugar
(do-backtrack
  (def x (goal1))
  (def y (goal2 x))
  (return (list x y)))
```

```sweet-exp
return(x)           ; succeed with one value
mzero()             ; fail (zero solutions)
mplus(fs gs)        ; choice between fs and gs
bind(f xs)          ; flatMap: sequence computations

;; Syntactic sugar
do-backtrack
  def x goal1()
  def y goal2(x)
  return(list(x y))
```

## Performance Considerations

### Clone Overhead

Cloning captured environments is expensive for large states. Strategies:

1. **Minimize capture:** Use local variables; avoid capturing large structures.
2. **Functional style:** Immutable data structures (tree, trie) amortize clone cost.
3. **Lazy evaluation:** Defer constraint checking until necessary.
4. **Cut:** Add `!` operator to commit to first solution, preventing unnecessary backtracking.

### Stack vs. Heap

Unlike Prolog's stack-based choice points, Turmeric's cloneable continuations allocate on the heap. This is acceptable for bounded search spaces but may not scale to million-node search trees.

## See Also

- [Effects System Guide](effects-system-guide.md) -- Algebraic effects foundation
- [Async/Await Guide](async-await-guide.md) -- Single-shot continuations for I/O
- [Checkpointing with Serializable Continuations](checkpointing-guide.md) -- Persisting computations
