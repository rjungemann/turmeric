---
title: tur/logic — Logic Programming Guide
category: Advanced Control Flow
description: How to use and extend tur/logic for miniKanren-style relational programming in Turmeric
---

# `tur/logic` — Logic Programming Guide

`tur/logic` provides miniKanren-style logic programming: unification, logic
variables, goals, and a backtracking search engine.  The module lives in
`stdlib/logic.tur` and has no dependencies outside the standard C allocator.

---

## Background: what is miniKanren?

miniKanren is a small, embeddable logic programming language pioneered by
William Byrd, Daniel Friedman, and collaborators.  Its central idea is that
programs are written as *relations* rather than functions, so the same
relation can be queried in multiple directions:

- given `x`, find all `y` such that `parento(x, y)` holds;
- given `y`, find all `x` such that `parento(x, y)` holds;
- enumerate all `(x, y)` pairs for which the relation holds.

The key primitives are:

| Concept | miniKanren name | `tur/logic` name |
|---------|-----------------|-----------------|
| Succeed unconditionally | `succeed` | `(succeed)` |
| Fail unconditionally | `fail` | `(fail)` |
| Unify two terms | `==` | `(lequal t1 t2)` |
| Logical AND | `fresh`/`conj` | `(conjoined g1 g2)` |
| Logical OR | `conde`/`disj` | `(disjoined g1 g2)` |
| Introduce a variable | `fresh` | `(fresh (fn [x] ...))` |
| Run a goal | `run` | `(run-logic n goal)` |

---

## Quick start

```turmeric
(import tur/logic :refer [term-int term-var term-nil term-pair
                           lvar-next subs-empty
                           lequal succeed fail conjoined disjoined fresh
                           run-logic first-state reify-walk
                           term-int-val term-tag logic-walk])

;; Q: is 5 equal to 5?
(let [results (run-logic 1 (lequal (term-int 5) (term-int 5)))]
  (println (bt-length results)))   ; => 1

;; Q: what value does x have if x == 7?
(let [x      (term-var (lvar-next))
      goal   (lequal x (term-int 7))
      result (run-logic 1 goal)
      walked (reify-walk x result)]
  (println (term-int-val walked)))  ; => 7
```
```sweet-exp
import tur/logic :refer [term-int term-var term-nil term-pair
                           lvar-next subs-empty
                           lequal succeed fail conjoined disjoined fresh
                           run-logic first-state reify-walk
                           term-int-val term-tag logic-walk]
;; Q: is 5 equal to 5?
let [results (run-logic 1 (lequal (term-int 5) (term-int 5)))]
  println(bt-length(results))
; => 1
;; Q: what value does x have if x == 7?
let [x      (term-var (lvar-next))
      goal   (lequal x (term-int 7))
      result (run-logic 1 goal)
      walked (reify-walk x result)]
  println(term-int-val(walked))
; => 7
```

---

## Core concepts

### Terms

A *term* is a tagged heap struct with three 64-bit fields: `tag`, `data1`,
`data2`.

| Tag | Kind | Constructor | Accessor(s) |
|-----|------|-------------|-------------|
| 0 | integer constant | `(term-int n)` | `(term-int-val t)` |
| 1 | logic variable | `(term-var id)` | `(term-var-id t)` |
| 2 | pair | `(term-pair a b)` | `(term-pair-fst t)`, `(term-pair-snd t)` |
| 3 | nil | `(term-nil)` | — |

The tag of any term can be inspected with `(term-tag t)`.

Pairs nest to form lists in the usual cons-cell style:

```turmeric
;; build the list [1, 2, nil]
(let [lst (term-pair (term-int 1)
                     (term-pair (term-int 2) (term-nil)))]
  ...)
```
```sweet-exp
;; build the list [1, 2, nil]
let [lst (term-pair (term-int 1)
                     (term-pair (term-int 2) (term-nil)))]
  ...
```

### Logic variables

Every variable is identified by a unique integer id, obtained from the
global counter `(lvar-next)`:

```turmeric
(let [x (term-var (lvar-next))
      y (term-var (lvar-next))]
  ...)
```
```sweet-exp
let [x (term-var (lvar-next))
      y (term-var (lvar-next))]
  ...
```

Prefer `(fresh ...)` (below) over manual `lvar-next` calls — it keeps
variable scope explicit.

### Substitutions

A *substitution* maps variable ids to terms.  It is represented as an alist
(a linked list of `{ var_id, term_ptr, next }` structs) whose empty value is
`(subs-empty)` (i.e., 0).

You rarely manipulate substitutions directly; `run-logic` starts from the
empty one and `reify-walk` lets you look up results.

### Goals

A *goal* is a fat closure of type `UState → list UState` — it accepts a
substitution and returns zero or more extended substitutions (the solution
stream).

The built-in goal constructors:

```turmeric
;; Primitives
(succeed)                         ; always succeeds
(fail)                            ; always fails
(lequal t1 t2)                    ; unify t1 and t2

;; Combination
(conjoined g1 g2)                 ; g1 AND g2 (sequential)
(disjoined g1 g2)                 ; g1 OR  g2 (parallel streams)

;; Variable introduction
(fresh (fn [x] body-goal))        ; create one fresh variable x
```
```sweet-exp
;; Primitives
succeed()
; always succeeds
fail()
; always fails
lequal(t1 t2)
; unify t1 and t2
;; Combination
conjoined(g1 g2)
; g1 AND g2 (sequential)
disjoined(g1 g2)
; g1 OR  g2 (parallel streams)
;; Variable introduction
fresh(fn([x] body-goal))
; create one fresh variable x
```

### Running a goal

```turmeric
(run-logic n goal)
```
```sweet-exp
run-logic(n goal)
```

Returns a solution list of at most `n` substitutions.  Use helper functions
to extract results:

```turmeric
(first-state results)             ; first substitution, or 0
(reify-walk term results)         ; walk a term through the first substitution
(bt-length results)               ; number of solutions (from tur/logic, re-exported)
```
```sweet-exp
first-state(results)
; first substitution, or 0
reify-walk(term results)
; walk a term through the first substitution
bt-length(results)
; number of solutions (from tur/logic, re-exported)
```

---

## Examples

### Unification failure

```turmeric
(let [results (run-logic 1 (lequal (term-int 1) (term-int 2)))]
  (println (bt-length results)))   ; => 0 (no solutions)
```
```sweet-exp
let [results (run-logic 1 (lequal (term-int 1) (term-int 2)))]
  println(bt-length(results))
; => 0 (no solutions)
```

### Disjunction — multiple answers

```turmeric
(let [x   (term-var (lvar-next))
      g   (disjoined (lequal x (term-int 1))
                     (disjoined (lequal x (term-int 2))
                                (lequal x (term-int 3))))
      res (run-logic 5 g)]
  (println (bt-length res)))       ; => 3
```
```sweet-exp
let [x   (term-var (lvar-next))
      g   (disjoined (lequal x (term-int 1))
                     (disjoined (lequal x (term-int 2))
                                (lequal x (term-int 3))))
      res (run-logic 5 g)]
  println(bt-length(res))
; => 3
```

### Fresh variables

`fresh` is the idiomatic way to introduce a scoped logic variable:

```turmeric
(let [res (run-logic 1
            (fresh (fn [x]
              (conjoined
                (lequal x (term-int 42))
                (succeed)))))]
  (println (term-int-val (reify-walk x res))))  ; => 42
```
```sweet-exp
let [res (run-logic 1
            (fresh (fn [x]
              (conjoined
                (lequal x (term-int 42))
                (succeed)))))]
  println(term-int-val(reify-walk(x res)))
; => 42
```

### Pair unification

```turmeric
(let [x   (term-var (lvar-next))
      y   (term-var (lvar-next))
      lhs (term-pair x (term-int 2))
      rhs (term-pair (term-int 1) y)
      res (run-logic 1 (lequal lhs rhs))]
  ;; after unification: x=1, y=2
  (println (term-int-val (reify-walk x res)))  ; => 1
  (println (term-int-val (reify-walk y res)))) ; => 2
```
```sweet-exp
let [x   (term-var (lvar-next))
      y   (term-var (lvar-next))
      lhs (term-pair x (term-int 2))
      rhs (term-pair (term-int 1) y)
      res (run-logic 1 (lequal lhs rhs))]
  ;; after unification: x=1, y=2
  println(term-int-val(reify-walk(x res)))
  ; => 1
  println(term-int-val(reify-walk(y res)))
; => 2
```

### Conjunction chain

Build a chain of goals with `conjoined`:

```turmeric
;; helper: (conjoin-all gs) folds a list of goals with conjoined
(defn conjoin-all [gs] : ptr<void>
  (if (= (tail gs) 0)
    (head gs)
    (conjoined (head gs) (conjoin-all (tail gs)))))
```
```sweet-exp
;; helper: (conjoin-all gs) folds a list of goals with conjoined
defn conjoin-all [gs] :ptr<void>
  if =(tail(gs) 0)
    head(gs)
    conjoined(head(gs) conjoin-all(tail(gs)))
```

### Family-tree relations

```turmeric
;; Encode people as integers; 0=Alice, 1=Bob, 2=Carol, 3=Dave
(defn parento [parent child] : ptr<void>
  (disjoined (conjoined (lequal parent (term-int 0))
                        (lequal child  (term-int 1)))
             (disjoined (conjoined (lequal parent (term-int 0))
                                   (lequal child  (term-int 2)))
                        (conjoined (lequal parent (term-int 1))
                                   (lequal child  (term-int 3))))))

;; grandparento via fresh intermediate variable
(defn grandparento [grand child] : ptr<void>
  (fresh (fn [mid]
    (conjoined (parento grand mid)
               (parento mid child)))))

;; Query: who are the grandchildren of Alice (id=0)?
(let [child (term-var (lvar-next))
      res   (run-logic 10 (grandparento (term-int 0) child))]
  ;; walks each solution
  ...)
```
```sweet-exp
;; Encode people as integers; 0=Alice, 1=Bob, 2=Carol, 3=Dave
defn parento [parent child] :ptr<void>
  disjoined(conjoined(lequal(parent term-int(0)) lequal(child term-int(1))) disjoined(conjoined(lequal(parent term-int(0)) lequal(child term-int(2))) conjoined(lequal(parent term-int(1)) lequal(child term-int(3)))))
;; grandparento via fresh intermediate variable
defn grandparento [grand child] :ptr<void>
  fresh(fn([mid] conjoined(parento(grand mid) parento(mid child))))
;; Query: who are the grandchildren of Alice (id=0)?
let [child (term-var (lvar-next))
      res   (run-logic 10 (grandparento (term-int 0) child))]
  ;; walks each solution
  ...
```

---

## Extending `tur/logic`

### Custom constraints

`tur/logic` uses *syntactic unification* without an occurs check.  You can
add arithmetic constraints or domain restrictions by building goals that
inspect terms:

```turmeric
;; goal: t must walk to an integer in the range [lo, hi]
(defn range-goal [t lo hi] : ptr<void>
  (fn [state]
    (let [walked (logic-walk t state)
          tag    (term-tag walked)]
      (if (= tag 0)                             ; INT term
        (let [v (term-int-val walked)]
          (if (and (>= v lo) (<= v hi))
            (mreturn state)
            (mzero)))
        (mzero)))))                             ; not ground -- fail
```
```sweet-exp
;; goal: t must walk to an integer in the range [lo, hi]
defn range-goal [t lo hi] :ptr<void>
  fn [state]
    let [walked (logic-walk t state)
          tag    (term-tag walked)]
      if =(tag 0)
        ; INT term
        let [v (term-int-val walked)]
          if and(>=(v lo) <=(v hi))
            mreturn(state)
            mzero()
        mzero()
; not ground -- fail
```

### Reification helpers

`reify-walk` follows a single substitution.  To pretty-print a full term
tree, walk recursively:

```turmeric
(defn reify-term [t subs] : cstr
  (let [walked (logic-walk t subs)
        tag    (term-tag walked)]
    (cond
      (= tag 0) (int->cstr (term-int-val walked))
      (= tag 1) "_"
      (= tag 2) (str-concat "(" (str-concat (reify-term (term-pair-fst walked) subs)
                                            (str-concat " . " (str-concat (reify-term (term-pair-snd walked) subs) ")"))))
      (= tag 3) "nil"
      :else     "?")))
```
```sweet-exp
defn reify-term [t subs] :cstr
  let [walked (logic-walk t subs)
        tag    (term-tag walked)]
    cond
      =
        tag
        0
      int->cstr
        term-int-val(walked)
      =
        tag
        1
      "_"
      =
        tag
        2
      str-concat
        "("
        str-concat(reify-term(term-pair-fst(walked) subs) str-concat(" . " str-concat(reify-term(term-pair-snd(walked) subs) ")")))
      =
        tag
        3
      "nil"
      :else
      "?"
```

Each `str-concat` / `int->cstr` here returns a fresh "caller frees" `cstr`, and
the recursive nesting leaks every intermediate while handing back a heap buffer
the caller must remember to free. When the reified text is a value you *return*
and pass around, prefer an owned `String`: build it with `stdlib/string.tur`'s
`StringBuilder` (`builder/push-cstr!` the constant pieces, `builder/push-string!`
the recursive results, `builder/finish` to freeze), or wrap the final buffer in
`string/adopt-cstr`. The result owns its bytes and frees exactly once. See
[strings-guide.md](strings-guide.md).

### `conde` macro

The classic miniKanren `conde` is syntactic sugar over nested `disjoined` /
`conjoined`.  You can define it as a Turmeric macro:

```turmeric
;; (conde [g1 g2 ...] [g3 g4 ...] ...)
;; each clause is a conjunction; clauses are disjoined
(defmacro conde [& clauses]
  (if (= (tail clauses) 0)
    `(conjoin-all ~(head clauses))
    `(disjoined (conjoin-all ~(head clauses))
                (conde ~@(tail clauses)))))
```
```sweet-exp
;; (conde [g1 g2 ...] [g3 g4 ...] ...)
;; each clause is a conjunction; clauses are disjoined
defmacro conde [& clauses]
  if =(tail(clauses) 0)
    `(conjoin-all ~(head clauses))
    `(disjoined (conjoin-all ~(head clauses))
                (conde ~@(tail clauses)))
```

### Interleaving search

`disjoined` currently uses `mplus`, which appends streams left-to-right
(*depth-first* enumeration).  For fairer, *breadth-first* (interleaved)
search replace `mplus` with an interleaving version:

```turmeric
;;; mplus-i -- interleaved (BFS) concatenation of two solution streams.
(defn mplus-i [xs ys] : int
  ;; swap xs and ys for every cons cell so solutions alternate
  (if (= xs 0) ys
    (let [head-val (bt-head xs)
          rest     (bt-tail xs)]
      (bt-cons head-val (mplus-i ys rest)))))
```
```sweet-exp
;;; mplus-i -- interleaved (BFS) concatenation of two solution streams.
defn mplus-i [xs ys] :int
  ;; swap xs and ys for every cons cell so solutions alternate
  if =(xs 0)
    ys
    let [head-val (bt-head xs)
          rest     (bt-tail xs)]
      bt-cons(head-val mplus-i(ys rest))
```

Then define `disjoined-i` analogously and use it in place of `disjoined`
when you need complete enumeration of infinite search spaces.

### Tabling / memoisation

For recursive relations that diverge under depth-first search, add a memo
table keyed on `(goal-id, substitution)`:

```turmeric
;; memo-table: map from (goal-name x subs-hash) -> result-list
;; Use tur/hamt for the persistent map.
(import tur/hamt :refer [hamt-empty hamt-insert hamt-lookup])

(defn tabled [name goal-fn args subs] : int
  (let [key  (hash-key name args subs)
        memo (hamt-lookup *memo-table* key)]
    (if (option-some? memo)
      (option-unwrap memo)
      (let [result (apply-goal (goal-fn args) subs)]
        (set! *memo-table* (hamt-insert *memo-table* key result))
        result))))
```
```sweet-exp
;; memo-table: map from (goal-name x subs-hash) -> result-list
;; Use tur/hamt for the persistent map.
import tur/hamt :refer [hamt-empty hamt-insert hamt-lookup]
defn tabled [name goal-fn args subs] :int
  let [key  (hash-key name args subs)
        memo (hamt-lookup *memo-table* key)]
    if option-some?(memo)
      option-unwrap(memo)
      let [result (apply-goal (goal-fn args) subs)]
        set!(*memo-table* hamt-insert(*memo-table* key result))
        result
```

---

## Integration with `tur/backtrack`

`tur/logic` inlines the backtracking monad (`mzero`, `mreturn`, `mplus`,
`mbind`) directly for performance.  If you are building tools on top of the
same monad without the full logic layer, `stdlib/backtrack.tur` exports these
primitives separately.

---

## Performance notes

- Substitutions are persistent alists.  Deep chains of `conjoined` goals
  produce long alist walks.  For critical inner loops, cache `logic-walk`
  results in a local let binding.
- `run-logic n g` returns at most `n` solutions and short-circuits; use
  small `n` in interactive sessions.
- The N-Queens benchmark (`benchmarks/bench-backtrack-n-queens.tur`) and the
  pair-sum benchmark (`benchmarks/bench-logic-query.tur`) serve as
  representative workloads.

---

## Where to learn more

### The miniKanren language and theory

- **"The Reasoned Schemer" (2nd ed.)** — Daniel P. Friedman, William E. Byrd,
  Oleg Kiselyov, Jason Hemann (MIT Press, 2018).  The canonical introduction;
  every concept in `tur/logic` maps directly to a chapter.

- **"miniKanren, Live and Untagged"** — William Byrd et al., 2012 Workshop
  on Scheme and Functional Programming.
  <https://webyrd.net/scheme-2013/papers/HemannMuKanren2013.pdf>

- **µKanren** (micro-Kanren) — Jason Hemann & Daniel Friedman, 2013.
  The minimal core (≈ 40 lines of Scheme) that `tur/logic` is modeled after.
  <http://webyrd.net/scheme-2013/papers/HemannMuKanren2013.pdf>

- **miniKanren.org** — canonical reference implementation, papers, and talks.
  <http://minikanren.org>

### Unification

- **"An Efficient Unification Algorithm"** — Martelli & Montanari (1982),
  *ACM Transactions on Programming Languages and Systems*.
  Describes the linear-time algorithm; `tur/logic` uses the simpler quadratic
  alist walk, suitable for small substitutions.

- **"Unification: A Multidisciplinary Survey"** — Kevin Knight (1989).
  <https://dl.acm.org/doi/10.1145/62029.62030>

### Logic programming broadly

- **"The Art of Prolog"** — Sterling & Shapiro (MIT Press, 1994).
  Classical treatment of resolution, unification, and search strategies.

- **core.logic** (Clojure) — a production miniKanren embedding closest in
  spirit to `tur/logic`.  Good source of idioms and constraint extensions.
  <https://github.com/clojure/core.logic>

- **Kanren** (original, Scheme) — Byrd & Friedman.
  <https://github.com/webyrd/miniKanren>

### Related guides in this documentation

- [backtracking-guide.md](backtracking-guide.md) — The `tur/backtrack` monad
  that `tur/logic` is built on.
- [minikanren-1-relations-and-queries.md](minikanren-1-relations-and-queries.md)
  — A runnable worked example building a family-graph query.
- [datalog-01-concepts.md](datalog-01-concepts.md) — Datalog, a cousin of
  miniKanren suited to database-style queries.
- [effects-system-guide.md](effects-system-guide.md) — Algebraic effects,
  an alternative to the monad-based search strategy used here.
