---
title: Legible character literals (`#\a`) -- Plan
category: Planning
description: A v1 legibility slice. Turmeric currently spells every byte-comparison and byte-list in prose as a raw ASCII integer (`43` for `+`, `48` for `0`, `(list 49 43 50 42 ...)` for `"1+2*..."`). The magic numbers hurt every parser, lexer, and byte-oriented example -- most visibly the parser-combinators tutorial. This plan adds a reader-level char literal `#\a` that desugars to `:int` at read time. No new type, no ABI change, no runtime work; the entire feature lives in the reader plus a small named-char table.
---

# Legible character literals -- Plan

## Why this exists

Every byte-oriented example in the codebase and docs -- the parser
tutorial, the JSON-subset fixture, `effects-vs-monads.md`, any place
that pattern-matches a byte against an ASCII code -- currently reads
like this:

```turmeric
(if (= c 43) (EAdd lhs rhs)
  (if (= c 45) (ESub lhs rhs)
    ...))

(let [input (list 49 43 50 42 40 51 43 52 41)]   ;; "1+2*(3+4)"
  ...)
```

The `43`s and `48`s carry no meaning at the reading level. Every reader
either memorizes the ASCII table or scrolls up to the comment. That is
a legibility tax on exactly the code that most benefits from being
readable -- parsers, lexers, tutorial fixtures.

The target reading:

```turmeric
(if (= c #\+) (EAdd lhs rhs)
  (if (= c #\-) (ESub lhs rhs)
    ...))

(let [input (list #\1 #\+ #\2 #\* #\( #\3 #\+ #\4 #\))]
  ...)
```

No new semantics -- `#\+` is just `43` -- but the operator jumps off
the line the way it does in every other language.

## Non-goals

- **A distinct `:char` type.** Not this slice. A `defopaque :char : int`
  can layer on top later if we decide the tag-safety is worth the API
  churn; today the byte-oriented code is already comparing with `=`
  and doing arithmetic on `- c 48`, and lifting all of that into a
  newtype is a separate, larger change.
- **Unicode grapheme support.** `#\a` is a single-byte ASCII / Latin-1
  codepoint. Multi-byte codepoints get the `#A` escape below;
  full Unicode segmentation belongs with the wider `stdlib/str` story.
- **String literals of chars.** `"foo"` stays a `:cstr`. Nothing in
  this plan changes existing string reading.

## Syntax

### Base form: `#\<char>`

`#\` followed by exactly one character. The reader emits an `:int`
whose value is the character's byte code:

| Source     | Reads as | Notes                          |
|------------|----------|--------------------------------|
| `#\a`      | `97`     | Any printable ASCII letter     |
| `#\A`      | `65`     | Case-sensitive                 |
| `#\0`      | `48`     | Digits                         |
| `#\+`      | `43`     | Operators, punctuation         |
| `#\-`      | `45`     |                                |
| `#\*`      | `42`     |                                |
| `#\/`      | `47`     |                                |
| `#\(`      | `40`     | Reserved paren -- see below    |
| `#\)`      | `41`     |                                |
| `#\{`      | `123`    | Reserved brace                 |
| `#\}`      | `125`    |                                |
| `#\[`      | `91`     | Reserved bracket               |
| `#\]`      | `93`     |                                |
| `#\;`      | `59`     | Reserved comment start         |
| `#\"`      | `34`     | Reserved string quote          |
| `#\\`      | `92`     | Backslash -- see escape below  |
| `#\ `      | `32`     | Space -- see named form below  |
| `#\|`      | `124`    |                                |

The reader stops at exactly one character after `#\`. Whitespace *is*
a valid target character (`#\ ` is space, `#\<tab>` is tab), but the
named form (below) reads better and should be preferred.

### Named form: `#\<name>` for non-printables and clarity

For characters that don't render well as bare glyphs, `#\` accepts a
lowercase alphabetic name terminated by the usual reader delimiters
(whitespace, `)`, `]`, `}`, `;`):

| Named form   | Reads as | Description        |
|--------------|----------|--------------------|
| `#\space`    | `32`     | ` `                |
| `#\newline`  | `10`     | `\n`               |
| `#\tab`      | `9`      | `\t`               |
| `#\return`   | `13`     | `\r`               |
| `#\null`     | `0`      | NUL byte           |
| `#\backspace`| `8`      | `\b`               |
| `#\delete`   | `127`    | DEL                |
| `#\escape`   | `27`     | ESC                |

Named forms are canonical: prefer `#\space` over `#\ ` in written
code. The reader accepts both; the formatter (once we have one) can
normalize.

Disambiguation rule: after `#\`, if the next byte is an ASCII letter
**and** the byte after that is *also* an ASCII letter, the reader
enters name mode and consumes until a delimiter. Otherwise it reads
exactly one character. Concretely:

- `#\a )` -- one letter, next byte is `)`: reads as `#\a` = `97`.
- `#\space` -- two letters, name mode: reads as `32`.
- `#\a2` is a **hard error** at read time (ambiguous: single char `a`
  followed by `2`, or start of a name?). Users should write `#\a` and
  `#\2` separately, or use an escape.

This rule was chosen because it matches Racket's behavior and is
easier to read than "any letter is name mode" (which would force
`#\space` for every letter and lose the appeal of `#\a`).

### Escape form: `#\u<hex>` for arbitrary codepoints

`#\u` followed by 1-4 hex digits reads as the integer value of those
digits. `#\u41` = `65` = `#\A`. Terminated by delimiters, same as
the named form. Values above `0x7F` are accepted (up to `0xFF` for
now) but the semantics of "byte value 200" in a `:cstr` context is on
the caller; this slice is purely lexical.

### What the reader does

- Read `#\` prefix (one lookahead byte after `#`).
- Peek at the next byte(s) to decide: escape (`u<hex>`), named (two+
  letters), or single character.
- Emit a plain `:int` literal token. Downstream elaboration sees the
  same AST it would have seen from a numeric literal. No type-checker
  changes, no codegen changes, no runtime changes.

### What the reader does *not* do

- No implicit type coercion, no new `:char` type, no autoboxing.
- No format-string interpolation. `"...#\+..."` is a normal string
  containing the four characters `#`, `\`, `+`, and so on -- the char
  literal reader is not active inside string literals.
- No sweet-exp interaction. `#\` composes with sweet-exp exactly the
  way `#map{...}` does today: the reader dispatch runs below the
  sweet-exp layer and returns a plain literal.

## Phasing

### CH0 -- Reader dispatch + base form (`#\a`)

- Add `#\` to the reader's `#`-dispatch table (`src/frontend/reader.c`
  or wherever `#map{`, `#set{`, `#row{`, `#refine{`, `#fx{` are
  currently dispatched). Emit a numeric-literal token.
- Delimiter rule: name-mode kicks in on two consecutive letters. One
  letter followed by a delimiter is single-char.
- Error on: unterminated `#\` at EOF, `#\` followed by two letters
  that don't match a named form (surface the unknown-name error, don't
  silently take the first letter).

CH0 exit: `(println #\A)` prints `65`; `#\+ == 43` in every position a
numeric literal would appear.

### CH1 -- Named char table

- Ship the eight named chars from the table above in a hard-coded
  reader-side lookup. No user extension for now.
- The table lives next to the reader dispatch; no runtime dependency.

CH1 exit: `#\space`, `#\newline`, `#\tab`, `#\return`, `#\null`,
`#\backspace`, `#\delete`, `#\escape` all round-trip.

### CH2 -- Codepoint escape (`#\u<hex>`)

- 1-4 hex digits after `#\u`. Terminate on delimiter. Range check
  0..0xFF for now; wider codepoints when we do the `:char` type.

CH2 exit: `#\u41 == 65`; `#\uFF == 255`; `#\u200` is a range
error at read time.

### CH3 -- Fixture + docs sweep

- Convert the parser-combinators tutorial fixture
  (`tests/fixtures/parsec-tutorial/input.tur`) and the guide
  (`docs/guides/parser-combinators-tutorial.md`) to `#\`. The doc has
  already been rewritten against the target syntax (see the note at
  the top of the guide); this phase closes the loop by updating the
  fixture and regenerating snapshots.
- Sweep other guides that carry raw ASCII codes for legibility --
  `docs/guides/effects-vs-monads.md` where it walks `is-digit?`
  logic, any JSON-subset fixtures, any lexer examples.
- No mass code migration in `stdlib/`. Existing byte-oriented
  primitives keep their integer literals; converting them is a
  legibility cleanup, not a correctness fix, and can happen
  opportunistically.

CH3 exit: parser tutorial reads with `#\+` throughout; `bash
tests/run.sh` green after snapshot regen.

## Test coverage

Add a small fixture directory `tests/fixtures/char-literals/`:

- `(println #\A)` -> `65`
- `(println #\+)` -> `43`
- `(println #\space)` -> `32`
- `(println #A)` -> `65`
- `(= #\a 97)` -> `1`
- Bad forms as `*.expected-fail` fixtures: `#\a2`, `#\unknown-name`,
  `#\u200`, `#\` at EOF.

Extend the parser-tutorial fixture as its own regression once CH3
lands.

## Follow-ups (not in scope)

- **`:char` opaque newtype** over `:int`. Would let `psat` take a
  `(fn [char] bool)` instead of `(fn [int] bool)`, matching the STRICT
  RULE against lazy `:int` stand-ins. Bigger change; separate plan.
- **String iteration returns `:char`.** Ties in with `stdlib/cstr`'s
  `cstr-nth` -- once we have a `:char` type, that primitive returns
  `:char` instead of `:int`. Downstream code that pattern-matches
  bytes benefits.
- **Char literals in `:cstr` interpolation.** Out of scope; existing
  `str-concat` and friends already handle this.

## Rejected alternatives

- **`?a` (Emacs Lisp).** Visually collides with Turmeric's
  `?`-suffix predicate convention (`is-digit?`, `at-end?`). Even a
  clean reader disambiguation leaves human readers guessing.
- **`#?a`.** Two-byte prefix with no precedent and no advantage over
  `#\a`. Preserves the `?`-collision problem for anyone skimming.
- **`'a'` (C-style).** Conflicts with `'` as quote. Non-starter.
- **`\a` (Clojure).** Single-byte prefix is nicer than `#\` but
  Turmeric's reader dispatch is already `#`-based, and adding a
  bare-`\` dispatch would be a lonely exception outside the family.

## Related

- [parser-combinators-tutorial.md](../guides/parser-combinators-tutorial.md)
  -- the primary consumer; already written against the `#\` syntax.
- [data-literals-guide.md](../guides/data-literals-guide.md) --
  companion reader-dispatch feature (`#map{}`, `#set{}`, `#row{}`)
  whose implementation `#\` slots next to.
