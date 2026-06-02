# Parser Combinators Tutorial Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-02
> **Type:** Documentation / Tutorial

---

## Overview

Turmeric ships `stdlib/parsec` -- a parser-combinator library built on the
backtracking (list) monad. The library is fully usable today, but there is no
narrative tutorial that walks a reader through *why* combinators are shaped
the way they are and *how* they are built up from a handful of primitive
ideas.

This plan covers a new guide,
`docs/guides/parser-combinators-tutorial.md`, that teaches a Turmeric reader
to design and implement parser combinators from scratch. The endpoint of the
tutorial mirrors the public-facing slice of `stdlib/parsec`, so a motivated
reader finishes the tutorial able to read (and contribute to) the real
implementation.

The tutorial is *implementation-focused*, not *usage-focused*: a separate
"using `stdlib/parsec`" guide can come later. Here we build the abstraction.

---

## Goals / Non-Goals

### Goals

- A single self-contained tutorial that a reader can follow start-to-finish
  in roughly 60-90 minutes.
- Every code snippet is runnable -- pasteable into `tur repl` or saveable as
  a standalone `.tur` / `.tur.sweet` file.
- Both s-expression and sweet-exp variants appear for the major building
  blocks, matching the dual-form convention used in `stm-tutorial.md` and
  `cellular-automata-comonad-tutorial.md`.
- The reader builds, in order: an `Input` type, a `Parser<a>` shape, primitive
  parsers (`item`, `pchar`, `satisfy`), the two combinator families
  (alternative `<|>` and sequencing `>>=`/`>>`), and the standard derived
  combinators (`many`, `many1`, `optional`, `between`, `sepBy`, `choice`).
- A worked example: parsing a small arithmetic expression grammar
  (`expr := term ('+'|'-' term)*`) end-to-end.
- A "comparison" closing section that diffs the tutorial code against
  `stdlib/parsec`, highlighting where the real library trades clarity for
  closure-capture / inline-C performance.

### Non-Goals

- No new code in `stdlib/`. This is documentation only; the tutorial code
  lives inline in the guide and (optionally) as a fixture under
  `tests/fixtures/parsec-tutorial/` to keep it from rotting.
- No coverage of left-recursion elimination, packrat memoization, or
  error-recovery strategies -- those are advanced topics that warrant their
  own follow-up guide.
- No reimplementation of `stdlib/parsec` itself. The tutorial points to the
  real module rather than competing with it.
- No discussion of typed parser combinators (`Parser<a>` as a real
  parametric type). Turmeric's HKT story is covered elsewhere; here we use
  `:int`-of-pointer for clarity.

---

## Audience and Prerequisites

Target reader: someone comfortable with `defn`, `let`, `fn`, and closures in
Turmeric, who has seen the REPL and run a fixture. They do not need to know
what a monad is; the tutorial introduces backtracking results concretely
before naming the pattern.

Prereq guides referenced from the intro:

- [quickstart-tutorial-plan.md](../guides/quickstart-tutorial-plan.md) -- for `defn`/`let`/`fn`.
- [repl-tutorial.md](../guides/repl-tutorial.md) -- so readers can paste snippets.
- [backtracking-guide.md](../guides/backtracking-guide.md) -- optional
  companion; the tutorial reproduces enough of the backtracking monad to
  stand alone, but readers who want the deeper treatment are pointed there.

---

## Outline

### 1. Motivation (~400 words)

- The "two parsers in a trench coat" framing: a parser takes input, produces
  *zero or more* (value, remaining-input) pairs.
- Why combinators beat hand-written recursive-descent: composability,
  testability per piece, no global mutable cursor.
- One screenshot-style snippet of the *finished* arithmetic parser so the
  reader sees the destination before the journey.

### 2. The `Input` type (~200 words)

- A two-field struct: source string + position.
- `input-new`, `input-at-end`, `input-current-char`, `input-advance`.
- Why position-as-int beats slicing the string each step (allocation).

### 3. The shape of a `Parser<a>` (~400 words)

- `Parser<a> = Input -> List<(a, Input)>`.
- Why a *list* of results, not a single result: alternatives and backtracking
  fall out for free.
- Concrete representation: a fat closure (`:ptr<void>`) over `Input :int`
  returning a `Cell*` chain. The tutorial uses a tiny `Cell { value, next }`
  with `bt-nil` / `bt-cons` helpers, mirroring `stdlib/parsec` but with
  s-expression bodies instead of inline-C where possible.

### 4. Three primitive parsers (~500 words)

- `pfail` -- always returns nil.
- `item` -- consumes one character (any).
- `satisfy pred` -- consumes one character matching a predicate.
- Show how `pchar c` and `digit?` are derived from `satisfy`.

### 5. The two core combinators (~700 words)

- **Alternative** (`<|>` / `or-parser`): "try p; if it fails, try q".
  Implementation = `mplus` on result lists.
- **Sequencing** (`>>=` / `bind-parser`): "run p, feed each result-pair's
  value into f, run the parser f returns on the leftover input".
  Implementation = `mbind` on result lists.
- A short aside: these are the list monad's `mzero` / `mplus` / `mbind`. Name
  the pattern, link to `backtracking-guide.md`, keep moving.

### 6. Derived combinators (~600 words)

Build, in order:

- `then-parser` (`>>`): sequence and discard the first result.
- `pure v`: succeed with `v`, consuming nothing.
- `many p`: zero-or-more, returns a list of values.
- `many1 p`: one-or-more, defined as `p` then `many p`.
- `optional p`: `p <|> pure nil`.
- `between open close p`: `open >> p >>= \x -> close >> pure x`.
- `sepBy p sep` and `choice [p, q, r, ...]`.

Each one is ~10-20 lines and is given in both s-expression and sweet-exp
form.

### 7. Worked example: arithmetic expressions (~800 words)

Grammar:

```
expr   := term (('+' | '-') term)*
term   := factor (('*' | '/') factor)*
factor := number | '(' expr ')'
number := digit+
```

- Build `number` from `many1 digit`.
- Build `factor` using `between (pchar '(') (pchar ')') expr` plus the
  number alternative via `<|>`.
- Show the left-fold-over-operators trick for `term` and `expr` so the
  result is the *evaluated* integer, not an AST -- keeps the tutorial small
  and runnable.
- Final snippet: `(run-parser-full expr "1+2*(3+4)")` -> `15`.

### 8. Comparing to `stdlib/parsec` (~300 words)

Side-by-side diff: the tutorial's `bind-parser` versus
[`stdlib/parsec.tur`](../../stdlib/parsec.tur)'s. Highlight:

- Why the real library uses inline-C for the inner trampoline (allocation
  pressure on the `Cell` list).
- The "params must be bound to locals before inner lambda capture" rule
  from the parsec.tur header comment, and why the tutorial sidesteps it.
- Pointer-to-`int` casting conventions; link to `c-integration-guide.md`.

### 9. Where to go next (~150 words)

- Try writing a JSON parser as an exercise.
- Read `stdlib/parsec.tur` for the production version.
- Follow-up topics: error positions, packrat memoization, left recursion.
  Note these as candidates for a future "Advanced Parser Combinators" guide.

---

## Code-Snippet Conventions

- Every snippet that defines a parser is given **once in s-expression form**
  and **once in sweet-exp form**, in that order, matching `stm-tutorial.md`.
- Short one-liners (e.g. `pchar`, `digit?`) only need one form -- pick the
  one that reads more clearly.
- All identifiers track the names already used in `stdlib/parsec.tur` so a
  reader bouncing between the tutorial and the real source is not paying a
  rename tax.
- Inline-C is avoided in the tutorial body. Where the real `stdlib/parsec`
  uses inline-C, the tutorial uses a slower-but-clearer pure-Turmeric
  equivalent and notes the tradeoff in section 8.

---

## Test / Validation Strategy

To keep the tutorial from rotting, add a fixture:

- `tests/fixtures/parsec-tutorial/input.tur` -- a single file containing
  every numbered snippet concatenated, ending with the arithmetic example
  and an `assert!` that `1+2*(3+4)` evaluates to `15`.
- `tests/fixtures/parsec-tutorial/expected.c` -- generated by `tur emit-c`.

This fixture is what catches "the tutorial code no longer compiles" when
core language or stdlib changes happen. If the snippets and the fixture
ever drift, the fixture is the source of truth; the guide must be updated
to match.

---

## Phases

### Phase PCT0 -- Draft outline & first three sections

- Write the Markdown skeleton with section headers and word-count budgets.
- Fill in sections 1-3 (motivation, Input, Parser shape).
- Stub the remaining sections with TODO markers.

### Phase PCT1 -- Combinator core (sections 4-6)

- Write sections 4, 5, 6 with both s-expression and sweet-exp snippets.
- Create the `tests/fixtures/parsec-tutorial/` fixture and snapshot it.

### Phase PCT2 -- Worked example (section 7)

- Write the arithmetic walkthrough.
- Extend the fixture to include the full grammar and assertion.

### Phase PCT3 -- Polish (sections 8-9 + cross-links)

- Write the comparison and "where to go next" sections.
- Add cross-links from `backtracking-guide.md`, `effects-vs-monads.md`,
  and the stdlib README to the new tutorial.
- Regenerate HTML via the docs pipeline (`tur run docs` if applicable, or
  the guide-rendering step the rest of `docs/guides/` uses).

---

## Open Questions

1. **Sweet-exp pairing depth.** `stm-tutorial.md` shows every snippet in
   both forms; `cellular-automata-comonad-tutorial.md` is less aggressive
   about pairing. The plan currently assumes the stm-style "show both"
   approach. Confirm before drafting.
2. **AST vs. evaluator in section 7.** The plan picks an evaluator (return
   `:int`) for compactness. An AST version would be more honest about how
   real parsers are used, at the cost of needing a small `Expr` ADT defined
   inside the tutorial. Decide before PCT2.
3. **Fixture location.** `tests/fixtures/parsec-tutorial/` is the natural
   spot, but if it grows >300 lines we may prefer a dedicated
   `tests/parsec-tutorial.sh` harness similar to `run-turi.sh`. Defer until
   PCT1 surfaces the actual size.

---

## See Also

- [stdlib/parsec.tur](../../stdlib/parsec.tur) -- the production parser-combinator library
- [docs/guides/backtracking-guide.md](../guides/backtracking-guide.md) -- backtracking / list monad
- [docs/guides/stm-tutorial.md](../guides/stm-tutorial.md) -- tutorial style template
- [docs/guides/cellular-automata-comonad-tutorial.md](../guides/cellular-automata-comonad-tutorial.md) -- tutorial style template
- [docs/guides/c-integration-guide.md](../guides/c-integration-guide.md) -- inline-C conventions referenced in section 8
