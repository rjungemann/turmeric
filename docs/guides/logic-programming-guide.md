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

A `Backtrack` computation is a **thunk that yields a cons-list of result
thunks**: forcing the outer thunk runs the search one level, and each element
is itself a suspended result.

```turmeric
;; mzero -- the empty computation (no results)
(defn mzero [] : (fn [] int)
  (fn [] (tnil)))

;; mplus -- choice: all of fs's results, then all of gs's
(defn mplus [^fat fs : (fn [] int) ^fat gs : (fn [] int)] : (fn [] int)
  (fn [] (list-concat (fs) (gs))))

;; pure -- a computation with exactly one result
(defn pure [x : int] : (fn [] int)
  (fn [] (cons x (tnil))))

;; flat-map over an int-carried cons list; f returns a list per element
(defn list-flat-map [xs : int ^fat f : (fn [int] int)] : int
  (if (tnil? xs)
    (tnil)
    (list-concat (f (list-head xs))
                 (list-flat-map (list-tail xs) f))))

;; bind -- sequence: run xs, then f on each result, concatenating
(defn bind [^fat xs : (fn [] int) ^fat f : (fn [int] int)] : (fn [] int)
  (fn [] (list-flat-map (xs) f)))
```

```sweet-exp
;; mzero -- the empty computation (no results)
defn mzero [] : (fn [] int)
  fn [] tnil()

;; mplus -- choice: all of fs's results, then all of gs's
defn mplus [^fat fs : (fn [] int) ^fat gs : (fn [] int)] : (fn [] int)
  fn []
    list-concat(fs() gs())

;; pure -- a computation with exactly one result
defn pure [x : int] : (fn [] int)
  fn []
    cons(x tnil())

;; flat-map over an int-carried cons list; f returns a list per element
defn list-flat-map [xs : int ^fat f : (fn [int] int)] : int
  if tnil?(xs)
    tnil()
    list-concat
      f(list-head(xs))
      list-flat-map(list-tail(xs) f)

;; bind -- sequence: run xs, then f on each result, concatenating
defn bind [^fat xs : (fn [] int) ^fat f : (fn [int] int)] : (fn [] int)
  fn []
    list-flat-map(xs() f)
```

Driving it -- `mplus` offers both branches, `bind` maps over every result:

```turmeric
;; Force every suspended result and print it
(defn print-all [xs : int] : void
  (if (tnil? xs)
    nil
    (do (println (list-head xs))
        (print-all (list-tail xs)))))

(print-all ((mplus (pure 1) (pure 2))))
;; => 1
;; => 2

(print-all ((bind (mplus (pure 1) (pure 2))
                  (fn [x] (cons (* x 10) (tnil))))))
;; => 10
;; => 20
```

> **Two constraints shape the code above, both of them current-compiler facts.**
>
> - **No composite type alias.** The natural `(deftype Backtrack [a] ...)` binds
>   a `TY_REC`, not an alias, so *applying* a `Backtrack` value fails with
>   `'fs' is not a function or continuation`. The signatures below therefore
>   spell the underlying `(fn [] int)` directly. See
>   [docs/reported/composite-type-alias-gap.md](../reported/composite-type-alias-gap.md).
> - **Cons cells are int-carried**, so the element thunks ride as `int` handles
>   and the closures are `^fat`. A parametric version over `(Cons A)` is blocked
>   by two separate defects -- a generic closure return type erases its type
>   application, and a struct constructor used inside a closure in a generic
>   function is never emitted (link error). See
>   [docs/reported/generic-closure-return-type-app.md](../reported/generic-closure-return-type-app.md).

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
