---
title: Parser Combinators Tutorial
category: Tutorials
description: Build a small parser in pure Turmeric using algebraic data types, GADTs, and pattern matching
---

# Parser Combinators Tutorial

This tutorial builds a working parser in pure Turmeric. By the end you
will:

- Model a parse result as a parametric `defdata` sum type and walk it
  with exhaustive `match`.
- Recover the shapes of the classic combinators -- `pfail`, alternation,
  sequencing, `many` -- by hand-inlining them into a recursive-descent
  grammar. Higher-order versions land cleanly once a couple of
  compiler gaps close; see the closing section.
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
function from parser(s) to a new parser. The result is that the parser
reads like the grammar.

Turmeric's ADTs and GADTs let us build the same story with the
type-checker on our side. A `PRes A` value can only mean "failure" or
"success carrying an A"; a valid `Expr` node can only be one of the
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

So in this tutorial the input is a **list of ASCII codes** -- a plain
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
  (POK a :int))
```

- `(PFail)` -- no parse.
- `(POK v rest)` -- parsed `v`, leftover input list is `rest` (again a
  `:int` list carrier -- `0` for end-of-input).

> **Syntax note.** The field type on `(POK a :int)` must be
> keyword-prefixed. Writing `(POK a int)` errors out with
> "defdata: constructor field type must be a keyword like `:int`".
> `defgadt` (below) accepts both spellings. Tracked in
> [docs/reported/defdata-parametric-inference-and-elab-match-segv.md](../reported/defdata-parametric-inference-and-elab-match-segv.md).

Every parser in the tutorial has the shape

```
Parser<A> = (fn [xs : int] : (PRes A))
```

where `xs` is the current input list.

---

## The combinators, hand-inlined

The classic library exposes `pure`, `pfail`, `<|>` (alternation),
`>>=` (sequencing), and `many`. In this tutorial we don't write them as
higher-order functions -- see the closing section for why -- but every
one shows up as a *pattern* inside the grammar. Once you see the
patterns you can lift them into standalone combinators the moment the
reported gaps close.

### `pfail` -- "no parse here"

```turmeric
(:: (PFail) (PRes Expr))
```

That's it. The `(:: e T)` ascription is required because bare
`(PFail)` can't infer its parameter `a` in every context (see the
tracked bug).

### Sequencing (the `>>=` pattern)

"Parse a `p`, then use its value to build the next parser" reads as a
`match` on the previous parser's result:

```turmeric
(match (number xs)
  (PFail)             (:: (PFail) (PRes Expr))
  (POK n rest)        (POK (ENum n) rest))
```

The `(POK n rest)` arm gets both the value and the leftover input,
which is exactly what `>>=` would have handed us. The failure branch
short-circuits.

### Alternation (the `<|>` pattern)

"Try `p`; if it fails, try `q`" is a conditional on the first byte
followed by a `match` on the result:

```turmeric
(if (= c 40)                       ;; '(' -> try parenthesised expr
  (match (expr-parse (list-tail xs))
    (PFail)             (:: (PFail) (PRes Expr))
    (POK inner rest)    ...)
  (match (number xs)                ;; else -> fall through to number
    ...))
```

### Repetition (`many`)

Greedy zero-or-more is a tail-recursive loop that accumulates matches
and stops when a match fails. `digits->int` inlines both the loop and
the "fold into an int" step:

```turmeric
(defn digits->int-loop [xs : int acc : int] : (PRes int)
  (if (at-end? xs)
    (POK acc xs)
    (let [c (list-head xs)]
      (if (is-digit? c)
        (digits->int-loop (list-tail xs) (+ (* acc 10) (- c 48)))
        (POK acc xs)))))
```

The recursion is self-tail-call, so the compiler turns it into
iteration -- no stack growth, no O(n) intermediate allocations.

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

### `number`

`many1 digit` semantics: require at least one digit, then greedily
accumulate.

```turmeric
(defn number [xs : int] : (PRes int)
  (if (at-end? xs)
    (:: (PFail) (PRes int))
    (let [c (list-head xs)]
      (if (is-digit? c)
        (digits->int-loop (list-tail xs) (- c 48))
        (:: (PFail) (PRes int))))))
```

### `factor` -- alternation

The alternation `number | '(' expr ')'` is a conditional on the first
byte. Recovery on `')'` mismatch produces a hard `PFail` -- packing
what a real library would spell as `between (pchar 40) (pchar 41) expr`:

```turmeric
(defn factor [xs : int] : (PRes Expr)
  (if (at-end? xs)
    (:: (PFail) (PRes Expr))
    (let [c (list-head xs)]
      (if (= c 40)
        (match (expr-parse (list-tail xs))
          (PFail)          (:: (PFail) (PRes Expr))
          (POK inner rest)
          (if (at-end? rest)
            (:: (PFail) (PRes Expr))
            (if (= (list-head rest) 41)
              (:: (POK inner (list-tail rest)) (PRes Expr))
              (:: (PFail) (PRes Expr)))))
        (match (number xs)
          (PFail)          (:: (PFail) (PRes Expr))
          (POK n rest)     (POK (ENum n) rest))))))
```

### Left-associative chains (`term` and `expr`)

`term := factor (('*'|'/') factor)*` is a `factor` followed by an
inlined `many` over `(op, factor)` pairs, folded left-to-right:

```turmeric
(defn term-tail [xs : int lhs : Expr] : (PRes Expr)
  (if (at-end? xs)
    (POK lhs xs)
    (let [c (list-head xs)]
      (if (if (= c 42) true (= c 47))
        (match (factor (list-tail xs))
          (PFail)          (:: (PFail) (PRes Expr))
          (POK rhs rest)   (term-tail rest (apply-op c lhs rhs)))
        (POK lhs xs)))))

(defn term [xs : int] : (PRes Expr)
  (match (factor xs)
    (PFail)              (:: (PFail) (PRes Expr))
    (POK lhs rest)       (term-tail rest lhs)))
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
          (PFail)          (:: (PFail) (PRes Expr))
          (POK rhs rest)   (expr-tail rest (apply-op c lhs rhs)))
        (POK lhs xs)))))

(defn expr-parse [xs : int] : (PRes Expr)
  (match (term xs)
    (PFail)              (:: (PFail) (PRes Expr))
    (POK lhs rest)       (expr-tail rest lhs)))
```

`factor` calls `expr-parse` (mutual recursion): the grammar closes on
itself through the parenthesised alternative.

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

## From direct-style to higher-order combinators

The natural next step is to lift the patterns above into standalone
combinators:

```turmeric
(defn or-parser [p : Parser<A> q : Parser<A>] : Parser<A>
  (fn [xs] (match (p xs)
             (POK v rest)  (POK v rest)
             (PFail)       (q xs))))

(defn bind-parser [p : Parser<A> f : (fn [A] Parser<B>)] : Parser<B>
  (fn [xs] (match (p xs)
             (PFail)       (PFail)
             (POK v rest)  ((f v) rest))))
```

At the time of writing these don't compile as cleanly as they should
because of two elaborator gaps tracked in
[docs/reported/defdata-parametric-inference-and-elab-match-segv.md](../reported/defdata-parametric-inference-and-elab-match-segv.md):

1. Parametric `defdata` return-type inference is weak, so every
   bare `(PFail)` needs a `(:: (PFail) (PRes T))` ascription for the
   surrounding match to type-check.
2. Some unascribed nested matches on parametric datatypes SEGV in
   `elab_match` instead of producing a diagnostic.

Once those close, the arithmetic parser will collapse to something
like

```turmeric
(defn factor [] : Parser<Expr>
  (or-parser
    (between (pchar 40) (pchar 41) (expr-ref))
    (map-parser ENum (many1 (digit)))))
```

That is what the finished tutorial should look like. Everything above
is the direct-style version we can compile *today*.

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
