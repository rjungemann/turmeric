---
title: Parser Combinators Tutorial
category: Parsing
description: Build parser combinators from scratch on top of the backtracking (list) monad
---

# Parser Combinators Tutorial

Build a working parser-combinator library from first principles. By the end
of this tutorial you will:

- Understand the shape `Parser<a> = Input -> List<(a, Input)>` and why a
  list of results falls naturally out of nondeterministic parsing.
- Implement the three primitive parsers (`pfail`, `item`, `satisfy`) and the
  two core combinators (alternative `<|>` and sequencing `>>=`).
- Derive the standard library of derived combinators -- `many`, `many1`,
  `optional`, `between`, `sepBy`, and `choice`.
- Parse a tiny arithmetic-expression grammar into a small AST and evaluate
  it so that `1+2*(3+4)` round-trips to `15`.

The tutorial is implementation-focused. A separate guide will cover *using*
the production `stdlib/parsec` module without rebuilding it.

The runnable end-to-end version of every snippet here lives in
[`tests/fixtures/parsec-tutorial/input.tur`](https://github.com/rjungemann/turmeric/tree/main/tests/fixtures/parsec-tutorial/input.tur).

---

## 1. Why combinators?

A parser is a function that takes input and produces zero or more parses.
"Zero" is failure, "one" is the usual happy path, and "more than one" is the
heart of nondeterminism: when a grammar has alternatives, a parse can
succeed in several ways simultaneously, and *each* way carries the leftover
input it would resume from.

Hand-written recursive-descent parsers mutate a shared cursor, which makes
them awkward to compose: you cannot try a branch, fail, and back up without
explicitly saving and restoring state. Combinators replace that ceremony
with values. Each combinator is a small function from parser(s) to a new
parser, and the whole library is a few pages of code.

By the end of this tutorial, the arithmetic parser reads almost like the
grammar it implements:

```turmeric
;; expr   := term (('+' | '-') term)*
;; term   := factor (('*' | '/') factor)*
;; factor := number | '(' expr ')'
;; number := digit+

(defn factor [] : ptr<void>
  (or-parser
    (between (pchar 40) (pchar 41) (expr-ref))
    (number)))
```

```sweet-exp
#lang sweet-exp

defn factor [] :ptr<void>
  or-parser
    between pchar(40) pchar(41) expr-ref()
    number()
```

That target is forty lines of grammar away.

---

## 2. The `Input` type

The first abstraction is an explicit cursor over the source string. We avoid
slicing -- that would allocate a new C string at every step. Instead, an
`Input` is a `(string, position)` pair, and `input-advance` returns a fresh
struct with `position + 1`.

```turmeric
(defn input-new [s : cstr pos] : int
  ```c
  struct { int64_t str; int64_t pos; } *inp = malloc(sizeof(*inp));
  inp->str = (int64_t)(intptr_t)s; inp->pos = pos;
  return (int64_t)(intptr_t)inp;
  ```)

(defn input-at-end [inp] : bool
  ```c
  struct { int64_t str; int64_t pos; } *i = (void*)(intptr_t)inp;
  return ((const char*)(intptr_t)i->str)[i->pos] == '\0';
  ```)

(defn input-current-char [inp] : int
  ```c
  struct { int64_t str; int64_t pos; } *i = (void*)(intptr_t)inp;
  return (int64_t)(unsigned char)((const char*)(intptr_t)i->str)[i->pos];
  ```)

(defn input-advance [inp] : int
  ```c
  struct { int64_t str; int64_t pos; } *i = (void*)(intptr_t)inp;
  struct { int64_t str; int64_t pos; } *n = malloc(sizeof(*n));
  n->str = i->str; n->pos = i->pos + 1;
  return (int64_t)(intptr_t)n;
  ```)
```

Every operation on `Input` is O(1) and (apart from `input-advance`) does no
allocation. Storing the string as `int64_t` rather than `:cstr` makes it
trivial to capture inside fat closures.

---

## 3. The shape of `Parser<a>`

A parser of values of type `a` takes an `Input` and returns a *list* of
`(value, leftover-input)` pairs:

```
Parser<a> = Input -> List<(a, Input)>
```

Why a list? Because alternation is built in: if a grammar can match two
ways, we keep both successes side-by-side rather than picking one and
losing the option to back up. Failure is just the empty list. Determinism
is "the list happens to have one element".

We reuse the *backtracking monad* (see
[backtracking-guide.md](backtracking-guide.md)) for the result list. Its
three constants are:

- `mzero` -- the empty result list (parse failure).
- `mreturn x` -- the singleton result list (one success).
- `mplus xs ys` -- concatenate two result lists (combine alternatives).

A parser is represented as a fat closure that takes one `int64_t` argument
(the `Input` pointer) and returns one `int64_t` (the head of a `Cell` list
of `(value, Input)` pairs).

Each result `Cell` looks like this in C:

```c
struct Cell { int64_t value; int64_t next; };
struct Pair { int64_t first; int64_t second; };
```

The cells form the backtracking list; each cell's `value` is a `Pair`
holding the parsed value and the leftover `Input`.

We need one helper to actually *call* a parser:

```turmeric
(defn apply-parser [p inp] : int
  ```c
  int64_t *fat = (int64_t*)(intptr_t)p;
  return TUR_APPLY1(fat, inp);
  ```)
```

```sweet-exp
defn apply-parser [p inp] :int
  ```c
  int64_t *fat = (int64_t*)(intptr_t)p;
  return TUR_APPLY1(fat, inp);
  ```
```

Everything else in the tutorial is built on top of `apply-parser` and the
three monad operations.

---

## 4. Three primitive parsers

### `pfail` -- always fail

The simplest parser ignores its input and returns no results.

```turmeric
(defn pfail-impl [inp] : int (mzero))

(defn pfail [] ^fat :ptr<void>
  (fn [inp] (pfail-impl inp)))
```

```sweet-exp
defn pfail-impl [inp] :int
  mzero()

defn pfail [] ^fat :ptr<void>
  fn [inp]
    pfail-impl(inp)
```

The `^fat` marker on the return type is not cosmetic: the inner `fn` captures
nothing, so the compiler would otherwise emit it as a bare C function pointer,
which is *not* callable through `apply-fat`. `^fat` makes the compiler box the
bare pointer into a fat closure at the tail, so consumers can fat-call it. We
will see this marker repeatedly. (Section 8 explains why.)

### `item` -- consume one character

If the input is at end, fail. Otherwise return the current character paired
with the advanced input.

```turmeric
(defn item-impl [inp] : int
  (if (input-at-end inp)
    (mzero)
    (mreturn (pair-new (input-current-char inp) (input-advance inp)))))

(defn item [] ^fat :ptr<void>
  (fn [inp] (item-impl inp)))
```

```sweet-exp
defn item-impl [inp] :int
  if input-at-end(inp)
    mzero()
    mreturn $ pair-new input-current-char(inp) input-advance(inp)

defn item [] ^fat :ptr<void>
  fn [inp]
    item-impl(inp)
```

### `satisfy` -- one character matching a predicate

`satisfy` is `item` filtered by a predicate. The predicate is a fat closure
of type `int -> int` (we use `0` for false, `1` for true).

```turmeric
(defn apply-fat [f arg] : int
  ```c
  int64_t *fat = (int64_t*)(intptr_t)f;
  return TUR_APPLY1(fat, arg);
  ```)

(defn satisfy-impl [_pred inp] : int
  (if (input-at-end inp)
    (mzero)
    (let [c (input-current-char inp)]
      (if (= (apply-fat _pred c) 0)
        (mzero)
        (mreturn (pair-new c (input-advance inp)))))))

(defn satisfy [pred] : ptr<void>
  (let [_pred pred] (fn [inp] (satisfy-impl _pred inp))))
```

```sweet-exp
defn apply-fat [f arg] :int
  ```c
  int64_t *fat = (int64_t*)(intptr_t)f;
  return TUR_APPLY1(fat, arg);
  ```

defn satisfy-impl [_pred inp] :int
  if input-at-end(inp)
    mzero()
    let [c input-current-char(inp)]
      if {apply-fat(_pred c) = 0}
        mzero()
        mreturn $ pair-new c input-advance(inp)

defn satisfy [pred] :ptr<void>
  let [_pred pred]
    fn [inp]
      satisfy-impl(_pred inp)
```

From `satisfy` we can derive specific parsers without writing any more
closure plumbing. `pchar` recognises a single literal character; `digit`
recognises any ASCII digit:

```turmeric
(defn pchar-impl [_c inp] : int
  (if (input-at-end inp)
    (mzero)
    (if (= (input-current-char inp) _c)
      (mreturn (pair-new _c (input-advance inp)))
      (mzero))))

(defn pchar [c] : ptr<void>
  (let [_c c] (fn [inp] (pchar-impl _c inp))))

(defn is-digit [c] : int
  ```c return (c >= '0' && c <= '9') ? 1 : 0; ```)

(defn digit-pred [] : ptr<void>
  (let [dummy 0] (fn [c] (let [_ dummy] (is-digit c)))))

(defn digit [] : ptr<void>
  (satisfy (digit-pred)))
```

`pchar` could just as well be written as `(satisfy (eq c))`; we keep an
explicit `pchar-impl` because `pchar` is the most common primitive and it
avoids one closure allocation per character.

---

## 5. The two core combinators

Two combinators take us from primitives to a real library. They are exactly
the list monad's `mplus` and `mbind`, specialised to parser closures.

### Alternative -- `or-parser` (`<|>`)

"Try `p`; if it fails, try `q`" is just *both* parsers run on the same
input, with their result lists concatenated. Backtracking is free:
unsuccessful alternatives become empty lists that `mplus` swallows.

```turmeric
(defn or-parser-impl [lp lq inp] : int
  (mplus (apply-parser lp inp) (apply-parser lq inp)))

(defn or-parser [p q] : ptr<void>
  (let [lp p
        lq q]
    (fn [inp] (or-parser-impl lp lq inp))))
```

```sweet-exp
defn or-parser-impl [lp lq inp] :int
  mplus apply-parser(lp inp) apply-parser(lq inp)

defn or-parser [p q] :ptr<void>
  let [lp p
       lq q]
    fn [inp]
      or-parser-impl(lp lq inp)
```

### Sequencing -- `bind-parser` (`>>=`)

"Run `p`; for each success `(value, leftover)`, pass `value` into `f`, run
the parser `f` returns on `leftover`." Concretely it is `mbind` on result
lists, except the per-result computation has to *call* the parser that `f`
returns instead of just returning a list.

```turmeric
(defn bind-parser-inner [lf pair] : int
  (let [lf2 lf]
    (apply-parser (apply-fat lf2 (pair-first pair)) (pair-second pair))))

(defn bind-parser-impl [lp lf inp] : int
  (let [lf2 lf]
    (mbind (apply-parser lp inp)
      (fn [pair] (let [_ lf2] (bind-parser-inner lf2 pair))))))

(defn bind-parser [p ^fat f] : ptr<void>
  (let [lp p
        lf f]
    (fn [inp] (bind-parser-impl lp lf inp))))
```

```sweet-exp
defn bind-parser-inner [lf pair] :int
  let [lf2 lf]
    apply-parser apply-fat(lf2 pair-first(pair)) pair-second(pair)

defn bind-parser-impl [lp lf inp] :int
  let [lf2 lf]
    mbind apply-parser(lp inp)
      fn [pair]
        let [_ lf2]
          bind-parser-inner(lf2 pair)

defn bind-parser [p ^fat f] :ptr<void>
  let [lp p
       lf f]
    fn [inp]
      bind-parser-impl(lp lf inp)
```

The `^fat` marker on `f` tells the compiler that `bind-parser` calls its
continuation through the fat-closure ABI (`apply-fat`). When a caller passes
a captureless `(fn ...)`, the compiler boxes it into a one-cell fat closure
at the call site, so the continuation dispatches correctly with no manual
workaround. (Section 8 explains the underlying ABI.)

`or-parser` and `bind-parser` are everything. Every other combinator is a
short, mechanical definition on top of them, and `mzero`/`mreturn` give us
the unit and zero of the parser monoid.

---

## 6. Derived combinators

### `then-parser` (`>>`) -- discard the first result

Useful for delimiters where the punctuation is consumed but not kept.

```turmeric
(defn then-parser-impl [lp lq inp] : int
  (let [lq2 lq]
    (mbind (apply-parser lp inp)
      (fn [pair] (let [_ lq2] (apply-parser lq2 (pair-second pair)))))))

(defn then-parser [p q] : ptr<void>
  (let [lp p
        lq q]
    (fn [inp] (then-parser-impl lp lq inp))))
```

```sweet-exp
defn then-parser-impl [lp lq inp] :int
  let [lq2 lq]
    mbind apply-parser(lp inp)
      fn [pair]
        let [_ lq2]
          apply-parser(lq2 pair-second(pair))

defn then-parser [p q] :ptr<void>
  let [lp p
       lq q]
    fn [inp]
      then-parser-impl(lp lq inp)
```

### `pure` -- succeed with a fixed value, consuming nothing

```turmeric
(defn pure-impl [_v inp] : int (mreturn (pair-new _v inp)))

(defn pure [v] : ptr<void>
  (let [_v v] (fn [inp] (pure-impl _v inp))))
```

```sweet-exp
defn pure-impl [_v inp] :int
  mreturn $ pair-new _v inp

defn pure [v] :ptr<void>
  let [_v v]
    fn [inp]
      pure-impl(_v inp)
```

### `many` -- greedy zero-or-more

`many` is the only combinator where we drop down into inline-C, because
naive recursion through `bind-parser` allocates a fresh result list per
iteration and we want one tight loop. Conceptually it runs `p` until it
fails, collecting matches into a `Cell` list and returning the final
leftover input.

```turmeric
(defn many-c-impl [p_raw inp] : int
  ```c
  typedef struct { int64_t value; int64_t next; } Cell;
  typedef struct { int64_t first; int64_t second; } Pair;
  typedef struct { int64_t str; int64_t pos; } Input;
  int64_t *fat_p = (int64_t*)(intptr_t)p_raw;
  int64_t current_inp = inp;
  Cell *rev_list = NULL;
  for (;;) {
      int64_t matches = TUR_APPLY1(fat_p, current_inp);
      if (!matches) break;
      Cell *match_cell = (Cell*)(intptr_t)matches;
      Pair *pr = (Pair*)(intptr_t)match_cell->value;
      Input *old_i = (Input*)(intptr_t)current_inp;
      Input *new_i = (Input*)(intptr_t)pr->second;
      if (new_i->pos <= old_i->pos) break;
      Cell *nc = malloc(sizeof(Cell));
      nc->value = pr->first; nc->next = (int64_t)(intptr_t)rev_list; rev_list = nc;
      current_inp = pr->second;
  }
  /* reverse and wrap as a singleton parse result */
  Cell *fwd = NULL;
  while (rev_list) {
      Cell *tmp = (Cell*)(intptr_t)rev_list->next;
      rev_list->next = (int64_t)(intptr_t)fwd; fwd = rev_list; rev_list = tmp;
  }
  Pair *result_pair = malloc(sizeof(Pair));
  result_pair->first = (int64_t)(intptr_t)fwd; result_pair->second = current_inp;
  Cell *result_cell = malloc(sizeof(Cell));
  result_cell->value = (int64_t)(intptr_t)result_pair; result_cell->next = 0;
  return (int64_t)(intptr_t)result_cell;
  ```)

(defn many [p] : ptr<void>
  (let [_p p] (fn [inp] (many-c-impl _p inp))))
```

### `many1`, `optional`, `between`

The remaining combinators are pure derivations:

```turmeric
(defn many1-impl [_p inp] : int
  (if (input-at-end inp)
    (mzero)
    (let [first-results (apply-parser _p inp)]
      (if (= first-results 0) (mzero) (many-c-impl _p inp)))))

(defn many1 [p] : ptr<void>
  (let [_p p] (fn [inp] (many1-impl _p inp))))

(defn optional-impl [_p inp] : int
  (mplus (apply-parser _p inp) (mreturn (pair-new 0 inp))))

(defn optional [p] : ptr<void>
  (let [_p p] (fn [inp] (optional-impl _p inp))))

(defn between [open close p] : ptr<void>
  (let [_open  open
        _close close
        _p     p]
    (then-parser _open
      (bind-parser _p
        (fn [x]
          (let [_ _close]
            (then-parser _close (pure x))))))))
```

```sweet-exp
defn many1-impl [_p inp] :int
  if input-at-end(inp)
    mzero()
    let [first-results apply-parser(_p inp)]
      if {first-results = 0}
        mzero()
        many-c-impl(_p inp)

defn many1 [p] :ptr<void>
  let [_p p]
    fn [inp]
      many1-impl(_p inp)

defn optional-impl [_p inp] :int
  mplus(apply-parser(_p inp) mreturn(pair-new(0 inp)))

defn optional [p] :ptr<void>
  let [_p p]
    fn [inp]
      optional-impl(_p inp)

defn between [open close p] :ptr<void>
  let [_open  open
       _close close
       _p     p]
    then-parser _open
      bind-parser _p
        fn [x]
          let [_ _close]
            then-parser _close pure(x)
```

`between` is the first combinator that actually uses `bind-parser` to build
something interesting. Its inner lambda captures `_close` from the outer
scope -- which, as the next subsection shows, is required, not optional.

`sepBy1` and `choice` follow the same pattern. `sepBy1 p sep` reads a `p`,
then `many (sep >> p)`, and conses them into a list; `choice` folds
`or-parser` over a list of alternatives. The fixture ships them and they
are short, mechanical exercises -- try writing them out before peeking.

---

## 7. Worked example: arithmetic

We will parse the grammar

```
expr   := term (('+' | '-') term)*
term   := factor (('*' | '/') factor)*
factor := number | '(' expr ')'
number := digit+
```

into a tiny AST:

```turmeric
;; Expr variants:
;;   ENum int       -- tag = 0
;;   EBin op l r    -- tag = 1, op is ASCII '+', '-', '*', '/'

(defn mk-enum [n] : int
  ```c
  struct { int64_t tag; int64_t a; int64_t b; int64_t c; } *e = malloc(sizeof(*e));
  e->tag = 0; e->a = n; e->b = 0; e->c = 0;
  return (int64_t)(intptr_t)e;
  ```)

(defn mk-ebin [op l r] : int
  ```c
  struct { int64_t tag; int64_t a; int64_t b; int64_t c; } *e = malloc(sizeof(*e));
  e->tag = 1; e->a = op; e->b = l; e->c = r;
  return (int64_t)(intptr_t)e;
  ```)
```

### `number`

`many1 digit` returns a list of ASCII digits. We fold it into an integer
and wrap it as `ENum`.

```turmeric
(defn number [] : ptr<void>
  (bind-parser (many1 (digit))
    (fn [digs]
      (pure (mk-enum (digits->int digs))))))
```

```sweet-exp
defn number [] :ptr<void>
  bind-parser many1(digit())
    fn [digs]
      pure $ mk-enum digits->int(digs)
```

The continuation `(fn [digs] ...)` captures nothing, but we no longer need a
dead capture to coerce it into a fat closure: `bind-parser` declares its
continuation `^fat` (section 5), so the compiler boxes this captureless
lambda at the call site automatically. See section 8.

### `factor`, `term`, `expr`

`expr` and `factor` are mutually recursive, so we go through a *thunk* --
a closure that, when invoked, calls `(expr)` and runs the result.

```turmeric
(defn expr-thunk-impl [inp] : int
  (apply-parser (expr) inp))

(defn expr-ref [] ^fat :ptr<void>
  (fn [inp] (expr-thunk-impl inp)))

(defn factor [] : ptr<void>
  (or-parser
    (between (pchar 40) (pchar 41) (expr-ref))
    (number)))
```

For `term` and `expr` we build the standard left-associative chain. Each
"tail" is a `(operator, operand)` pair; we collect all tails with `many`
and then `fold-bin-tail` walks them left-to-right, growing an `EBin` AST.

```turmeric
(defn term-tail-pair [] : ptr<void>
  (bind-parser (term-op)
    (fn [op]
      (bind-parser (factor)
        (fn [rhs] (pure (pair-new op rhs)))))))

(defn term [] : ptr<void>
  (bind-parser (factor)
    (fn [lhs]
      (bind-parser (many (term-tail-pair))
        (fn [tails] (pure (fold-bin-tail lhs tails)))))))
```

`expr` is the same shape with `expr-op` and `term` instead of `term-op`
and `factor`. The continuations capture `op`/`lhs` (or nothing at all);
either way `bind-parser`'s `^fat` continuation parameter boxes them
correctly, and inner lambdas can reference an enclosing `fn`'s parameter
directly without rebinding it to a local first.

### Evaluation

The AST evaluator is a five-line recursive walk:

```turmeric
(defn eval-expr [e] : int
  (if (= (expr-tag e) 0)
    (expr-a e)
    (let [op (expr-a e)
          lv (eval-expr (expr-b e))
          rv (eval-expr (expr-c e))]
      (if (= op 43) (+ lv rv)
        (if (= op 45) (- lv rv)
          (if (= op 42) (* lv rv)
            (if (= op 47) (/ lv rv) 0)))))))
```

### The round-trip

```turmeric
(defn main []
  (let [results (run-parser-full (expr) "1+2*(3+4)")
        ast     (first-value results)
        answer  (eval-expr ast)]
    (println answer)))  ; => 15
```

```sweet-exp
defn main []
  let [results run-parser-full(expr() "1+2*(3+4)")
       ast     first-value(results)
       answer  eval-expr(ast)]
    println(answer)  ; => 15
```

Running the fixture prints `15`. The same parsers would happily produce an
AST for arbitrary expressions; swapping `eval-expr` for a pretty-printer
would give a normaliser without touching the parser code.

---

## 8. Comparing to `stdlib/parsec`

The tutorial code and [`stdlib/parsec.tur`](../../stdlib/parsec.tur) share a
common shape, but the production library is tighter in three ways.

**Inline-C in `mbind`.** `stdlib/parsec` implements `mbind` directly in
inline C, walking the result list once and stitching new cells in place.
Our tutorial calls `mbind` through `bind-parser-inner`, which adds two
function calls per result. For a JSON-sized parse this matters; for a
forty-character arithmetic expression it does not.

**Captureless-lambda ABI.** A `fn` body that captures no free variables is
optimised to a bare C function pointer (`int64_t (*)(int64_t)`), which is
*not* callable through `apply-fat` (which expects a fat closure laid out as
`int64_t (*)(void*, int64_t)` in slot 0). Feed a bare pointer to `apply-fat`
and it reads the first instruction byte as a thunk address and segfaults.

The compiler tracks this representation in the type system and boxes a
captureless lambda into a one-cell fat closure wherever a *fat-expecting
sink* is annotated `^fat` -- either a parameter (`[p ^fat f]`, as on
`bind-parser`) or a constructor's return type (`^fat :ptr<void>`, as on
`pfail`/`item`). Earlier versions of this tutorial (and `stdlib/parsec`)
forced the fat ABI by hand with a dead `(let [sentinel 0] ...)` capture;
that workaround is obsolete now that the sinks carry `^fat`. Inner lambdas
may also reference an enclosing `fn`'s parameter directly -- the old "bind
parameters to locals first" rule no longer applies. See
[c-integration-guide.md](c-integration-guide.md#closures-and-fat-pointers)
for the underlying calling convention.

**Pointer-to-int casting.** Both the tutorial and the production library
pass parser pointers around as `:int` (raw `int64_t`) and reinterpret them
in inline C with `(int64_t*)(intptr_t)p`. This is the standard
turmeric-side convention for handing closures across function boundaries;
the `:ptr<void>` return type on the parser constructors is purely a hint
to the type checker.

---

## 9. Where to go next

- **Exercise:** write a JSON parser. The hard cases (strings with escapes,
  numbers with exponents) need only the combinators in this tutorial. The
  `parsec-json-subset` fixture in `tests/fixtures/` is a starting point.
- **Production library:** [`stdlib/parsec.tur`](../../stdlib/parsec.tur)
  has the inline-C-optimised versions of every combinator here plus a few
  more (`pstring`, `parse-value`, `print-char-list`, ...).
- **Advanced topics for a future guide:** error positions and recovery,
  packrat memoisation, and left-recursion elimination. None of those
  fit cleanly on top of the tutorial library; they are interesting in
  their own right.

## See Also

- [stdlib/parsec.tur](../../stdlib/parsec.tur) -- production parser-combinator library
- [backtracking-guide.md](backtracking-guide.md) -- list monad / nondeterminism
- [stm-tutorial.md](stm-tutorial.md) -- style template (sweet-exp pairing)
- [c-integration-guide.md](c-integration-guide.md) -- inline-C, fat closures, `TUR_APPLY1`
- [`tests/fixtures/parsec-tutorial/`](https://github.com/rjungemann/turmeric/tree/main/tests/fixtures/parsec-tutorial/) -- the runnable fixture for this tutorial
