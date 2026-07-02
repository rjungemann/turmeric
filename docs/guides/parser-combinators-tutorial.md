---
title: Parser Combinators Tutorial
category: Tutorials
description: Build a small parser in pure Turmeric using algebraic data types, GADTs, higher-order combinators, and pattern matching
---

# Parser Combinators Tutorial

This tutorial builds a working parser-combinator library in pure
Turmeric. By the end you will:

- Model a parse result as a parametric `defdata` sum type and walk it
  with exhaustive `match`.
- Assemble the classic combinators -- `psat`, `pchar`, `or-*`,
  `map-*` -- and use them to compose the grammar.
- Parse `"1+2*(3+4)"` into a **typed GADT AST** and evaluate it via
  `match`, so the whole thing round-trips to `15`.

The focus is pedagogy, not micro-optimisation. Every line is idiomatic
Turmeric with `defdata`, `defgadt`, `match`, and no inline C.

The runnable end-to-end version of every snippet here lives in
[`tests/fixtures/parsec-tutorial/input.tur`](https://github.com/rjungemann/turmeric/tree/main/tests/fixtures/parsec-tutorial/input.tur).

---

## Why combinators?

A parser is a function that takes input and either succeeds -- consuming
some prefix and returning a value plus the leftover input -- or fails.

Hand-written recursive-descent parsers mutate a shared cursor, so trying
one branch, failing, and backing up requires bookkeeping. Parser
combinators replace that ceremony with values: each combinator is a
function *from parsers to a new parser*. The result reads like the
grammar.

Turmeric's ADTs and GADTs let us build the same story with the
type-checker on our side. A `(PRes A)` value can only mean "failure" or
"success carrying an `A`"; a valid `Expr` node can only be one of the
constructors we declared; `match` refuses to compile if we forget a
case. Every combinator gets stronger static guarantees than the
equivalent Haskell tutorial would have handed us.

---

## Modelling the input

For a **byte-oriented** parser we want to walk the source one character
at a time. The natural Turmeric type would be a `:cstr` plus a
`(cstr-nth s i)` primitive, but that primitive isn't autoloaded yet
(see
[docs/reported/no-cstr-byte-primitives-pure-turmeric.md](../reported/no-cstr-byte-primitives-pure-turmeric.md)).

So the input is a **list of ASCII codes** -- a plain
`(cons int (cons int ...))` built with `stdlib/list`'s `list-head` and
`list-tail`. The string `"1+2*(3+4)"` becomes:

```turmeric
(list 49 43 50 42 40 51 43 52 41)
;;    '1' '+' '2' '*' '(' '3' '+' '4' ')'
```

Two helpers make the rest of the code read cleanly. End-of-input is a
`0` list carrier (nil):

```turmeric
(defn at-end? [xs : int] : bool (= xs 0))

(defn is-digit? [c : int] : bool
  (if (< c 48) false (if (> c 57) false true)))
```

`list-head xs` returns the current byte and `list-tail xs` advances the
cursor.

---

## The result type

A parse either fails or succeeds and returns leftover input. That's a
two-armed sum, parameterised over the success payload:

```turmeric
(defdata PRes [a]
  (PFail)
  (POK a int))
```

- `(PFail)` -- no parse.
- `(POK v rest)` -- parsed `v`, leftover input list is `rest` (an
  `int` list carrier -- `0` for end-of-input).

Every parser in the tutorial has the shape

```
Parser<A> = (fn [xs : int] : (PRes A))
```

where `xs` is the current input list.

---

## The primitive: `psat`

The one *primitive* parser -- the atom every other combinator is built
on -- consumes a single character if it matches a predicate:

```turmeric
(defn psat [pred : (fn [int] bool)] : (fn [int] (PRes int))
  (fn [xs : int] : (PRes int)
    (if (at-end? xs)
      (PFail)
      (let [c (list-head xs)]
        (if (pred c) (POK c (list-tail xs)) (PFail))))))
```

`psat` returns a *closure* -- a function value that captures `pred`.
Every combinator below follows the same shape: take some parsers (and
maybe a helper), return a new parser (a `(fn [int] (PRes A))`
closure). Once you have `psat`, `pchar` and `digit` are one-liners:

```turmeric
(defn eq-char [target : int] : (fn [int] bool)
  (fn [c : int] : bool (= c target)))

(defn pchar [target : int] : (fn [int] (PRes int))
  (psat (eq-char target)))

(defn digit [xs : int] : (PRes int)
  ((psat is-digit?) xs))
```

`pchar` currying: `eq-char` builds a predicate closure and `psat`
lifts it into a parser. `digit` invokes `(psat is-digit?)` eagerly and
eta-expands the result so it can be used as a plain top-level parser.

---

## Combinators

The combinators are the recurring shapes -- alternation, mapping,
sequencing -- lifted out of ad-hoc grammar code so we can compose them
freely. The tutorial spells each one *monomorphically* (one instance
per element type: `or-int` / `or-expr`, `map-int-to-expr`) because
Turmeric's codegen currently has an open bug on polymorphic-`defn`
bodies that return an inner lambda; see
[docs/reported/poly-defn-inner-lambda-codegen.md](../reported/poly-defn-inner-lambda-codegen.md).
Both spellings would elaborate; only the monomorphic ones compile
end-to-end today.

### Alternation -- `or-*`

"Try `p`; if it fails, try `q`":

```turmeric
(defn or-int [p : (fn [int] (PRes int)) q : (fn [int] (PRes int))]
    : (fn [int] (PRes int))
  (fn [xs : int] : (PRes int)
    (match (p xs)
      (POK v rest) (POK v rest)
      (PFail)      (q xs))))

(defn or-expr [p : (fn [int] (PRes Expr)) q : (fn [int] (PRes Expr))]
    : (fn [int] (PRes Expr))
  (fn [xs : int] : (PRes Expr)
    (match (p xs)
      (POK v rest) (POK v rest)
      (PFail)      (q xs))))
```

The two are line-for-line identical except for the element type. In a
future Turmeric they'd collapse to one `(or-parser [A] ...)` `defn`.

### Mapping -- `map-*-to-*`

"Run `p`; on success, transform the payload":

```turmeric
(defn map-int-to-expr [f : (fn [int] Expr) p : (fn [int] (PRes int))]
    : (fn [int] (PRes Expr))
  (fn [xs : int] : (PRes Expr)
    (match (p xs)
      (PFail)      (PFail)
      (POK v rest) (POK (f v) rest))))
```

Note the bare `(PFail)` on the failure arm: the enclosing
`fn`'s declared return `(PRes Expr)` propagates into the arm body, so
no `(:: (PFail) (PRes Expr))` ascription is needed. Same for the
`(PFail)` inside `or-*`.

---

## The AST as a GADT

Every constructor pins the `[a]` parameter to `int`. In a richer
calculator you could add `(EEq (Expr int) (Expr int) : (Expr bool))`
and the type checker would then refuse `EAdd (EEq ...) (ENum 1)` at
compile time. That's the GADT payoff -- illegal ASTs stop being
representable.

```turmeric
(defgadt Expr [a]
  (ENum int                     : (Expr int))
  (EAdd (Expr int) (Expr int)   : (Expr int))
  (ESub (Expr int) (Expr int)   : (Expr int))
  (EMul (Expr int) (Expr int)   : (Expr int))
  (EDiv (Expr int) (Expr int)   : (Expr int)))

(defn apply-op [op : int lhs : Expr rhs : Expr] : Expr
  (if (= op 43) (EAdd lhs rhs)
    (if (= op 45) (ESub lhs rhs)
      (if (= op 42) (EMul lhs rhs)
        (if (= op 47) (EDiv lhs rhs)
          (ENum 0))))))
```

`apply-op` dispatches on the ASCII operator code. It falls through to
`(ENum 0)` on an unknown byte, which never happens if the grammar is
correct -- and if it does, `eval-expr` will still produce a defined
value.

---

## The grammar

```
expr   := term (('+' | '-') term)*
term   := factor (('*' | '/') factor)*
factor := number | '(' expr ')'
number := digit+
```

The two-level `expr` / `term` split is what buys precedence: `*` and
`/` are one level deeper than `+` and `-`, so they bind tighter.

### `number` -- one-or-more digits, folded

```turmeric
(defn digits-int-loop [xs : int acc : int] : (PRes int)
  (if (at-end? xs)
    (POK acc xs)
    (let [c (list-head xs)]
      (if (is-digit? c)
        (digits-int-loop (list-tail xs) (+ (* acc 10) (- c 48)))
        (POK acc xs)))))

(defn number [xs : int] : (PRes int)
  (match (digit xs)
    (PFail)      (PFail)
    (POK d rest) (digits-int-loop rest (- d 48))))
```

`digit` (from earlier) is the "at-least-one" gate; `digits-int-loop`
is the tail-recursive `many` that greedily accumulates the rest into
an int. The recursion is self-tail-call, so the compiler turns it into
iteration -- no stack growth, no O(n) intermediate allocations.

### `factor` -- alternation via `or-expr`

```turmeric
(defn to-enum [n : int] : Expr (ENum n))

(defn number-as-expr [xs : int] : (PRes Expr)
  ((map-int-to-expr to-enum number) xs))

(defn paren-expr [xs : int] : (PRes Expr)
  (if (at-end? xs)
    (PFail)
    (let [c (list-head xs)]
      (if (= c 40)
        (match (expr-parse (list-tail xs))
          (PFail)          (PFail)
          (POK inner rest)
          (if (at-end? rest)
            (PFail)
            (if (= (list-head rest) 41)
              (:: (POK inner (list-tail rest)) (PRes Expr))
              (PFail))))
        (PFail)))))

(defn factor [xs : int] : (PRes Expr)
  ((or-expr paren-expr number-as-expr) xs))
```

`factor` is now a one-liner over `or-expr`: try `paren-expr` first,
fall back to `number-as-expr`. `number-as-expr` shows the `map` shape
in action -- `to-enum` wraps `ENum` as a plain function value (a
GADT constructor can't yet be passed directly as a first-class value)
and `map-int-to-expr` lifts it into a `Parser<Expr>`.

`paren-expr` still needs to reach into `match` for the closing `)`
handling; not every one-off pattern collapses into a combinator.

One `(:: (POK inner (list-tail rest)) (PRes Expr))` ascription lingers
in `paren-expr`. It's load-bearing: `expr-parse` is defined *later* in
the file (mutual recursion), so when `paren-expr` is elaborated the
callee's parametric return type isn't yet visible; the match binder
falls back to a placeholder element type and the bare `(POK ...)`
needs a hint. Tracked in
[docs/reported/defdata-parametric-forward-decl-inference.md](../reported/defdata-parametric-forward-decl-inference.md).

### Left-associative chains -- `term` and `expr`

`term := factor (('*'|'/') factor)*` is a `factor` followed by an
inlined `many` over `(op, factor)` pairs, folded left-to-right:

```turmeric
(defn term-tail [xs : int lhs : Expr] : (PRes Expr)
  (if (at-end? xs)
    (POK lhs xs)
    (let [c (list-head xs)]
      (if (if (= c 42) true (= c 47))
        (match (factor (list-tail xs))
          (PFail)          (PFail)
          (POK rhs rest)   (term-tail rest (apply-op c lhs rhs)))
        (POK lhs xs)))))

(defn term [xs : int] : (PRes Expr)
  (match (factor xs)
    (PFail)          (PFail)
    (POK lhs rest)   (term-tail rest lhs)))
```

`term-tail` is where left-associativity comes from: we apply the op
*before* recursing, so `2*3*4` folds into `EMul (EMul 2 3) 4`, not
`EMul 2 (EMul 3 4)`. `expr` and `expr-tail` are the same shape with
`+`/`-` and `term` swapped in.

```turmeric
(defn expr-tail [xs : int lhs : Expr] : (PRes Expr)
  (if (at-end? xs)
    (POK lhs xs)
    (let [c (list-head xs)]
      (if (if (= c 43) true (= c 45))
        (match (term (list-tail xs))
          (PFail)          (PFail)
          (POK rhs rest)   (expr-tail rest (apply-op c lhs rhs)))
        (POK lhs xs)))))

(defn expr-parse [xs : int] : (PRes Expr)
  (match (term xs)
    (PFail)          (PFail)
    (POK lhs rest)   (expr-tail rest lhs)))
```

The chain-fold pattern -- match the head, keep folding tails, wrap
each application through `apply-op` -- is itself a candidate for a
future `chainl1` combinator once the underlying compiler bugs close.
Today it reads clearly enough as its own `defn`.

---

## Evaluation

Evaluation is a five-line walk of the GADT. No tag comparisons, no
default arm, no fall-through -- `match` proves every constructor is
handled:

```turmeric
(defn eval-expr [e : Expr] : int
  (match e
    (ENum n)   n
    (EAdd l r) (+ (eval-expr l) (eval-expr r))
    (ESub l r) (- (eval-expr l) (eval-expr r))
    (EMul l r) (* (eval-expr l) (eval-expr r))
    (EDiv l r) (/ (eval-expr l) (eval-expr r))))
```

### The round-trip

```turmeric
(defn main [] : int
  (let [input (list 49 43 50 42 40 51 43 52 41)]   ;; "1+2*(3+4)"
    (match (expr-parse input)
      (PFail)          (do (println 0) 1)
      (POK ast rest)   (do (println (eval-expr ast)) 0))))
```

Running the fixture prints `15`. Swap `eval-expr` for a pretty-printer
and you have a normaliser without touching the parser code -- that's
the separation of syntax and semantics the ADT/GADT split buys you.

---

## Where the types earned their keep

Looking back at what the type system gave us:

- **`match` on `PRes`** is exhaustiveness-checked. We physically cannot
  forget the failure branch -- the compiler would refuse the definition.
- **`(PRes Expr)`** carries the success payload's type. `number`
  returns `(PRes int)`; `factor` returns `(PRes Expr)`. A cross-wire
  is a type error, not a runtime crash.
- **The `Expr` GADT** rules out impossible AST shapes. There is no
  "unknown tag" case to guard against because there is no way to
  construct one.
- **`is-digit?`** returns `:bool`, not `:int`. The rule against `:int`
  stand-ins (`CLAUDE.md`) is directly why.

None of these are aesthetic wins. Each rules out an entire class of
bugs at compile time. That is the reason to reach for GADTs and ADTs
even for a tutorial-sized parser.

---

## Where to go next

- **Exercise:** write a JSON parser. Strings-with-escapes and
  numbers-with-exponents are the hard cases and need only the patterns
  here. See `tests/fixtures/parsec-json-subset/` for a starting
  point.
- **Production library:** [`stdlib/parsec.tur`](../../stdlib/parsec.tur)
  has performance-tuned versions of every combinator plus `pstring`,
  `parse-value`, and friends -- built on top of inline-C for the tight
  loops.
- **Real strings:** once
  [docs/reported/no-cstr-byte-primitives-pure-turmeric.md](../reported/no-cstr-byte-primitives-pure-turmeric.md)
  closes, the input list of ASCII ints goes away and the parser takes
  a `:cstr` directly.
- **Polymorphic combinators:** once
  [docs/reported/poly-defn-inner-lambda-codegen.md](../reported/poly-defn-inner-lambda-codegen.md)
  closes, the `or-int` / `or-expr` (and every other monomorphic pair
  in the tutorial) collapses to a single `(or-parser [A] p q)`.
- **Error positions and recovery:** the current library returns "no
  result" on failure. A production parser threads the furthest position
  reached, so error messages can say *where* parsing died.
- **Packrat memoisation:** caching `parser` x `input-pos` results
  turns exponential grammars into linear ones. It fits neatly on top
  of the interface here.

## See also

- [gadts-guide.md](gadts-guide.md) -- `defgadt`, `match`, type refinement
- [backtracking-guide.md](backtracking-guide.md) -- the list monad and
  nondeterminism (the direction a full-fat combinator library would go)
- [`stdlib/parsec.tur`](https://github.com/rjungemann/turmeric/tree/main/stdlib/parsec.tur)
  -- the production parser-combinator library
- [`tests/fixtures/parsec-tutorial/`](https://github.com/rjungemann/turmeric/tree/main/tests/fixtures/parsec-tutorial/)
  -- the runnable fixture for this tutorial
