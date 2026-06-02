---
title: Sweet-Exp T-Expression Follow-Ups
category: Planning
description: Remaining polish for the sweet-exp (SRFI-110) preprocessor — `\\` group operator and source-map-based diagnostics
---

# Sweet-Exp T-Expression Follow-Ups -- Plan

## Background

The indent-sensitive sweet-expression reader (`#lang sweet-exp` / files
with the `.tur.sweet` extension) is implemented as a textual
preprocessor in `src/compiler/reader.c`.  It walks the source into
logical lines, recursively groups deeper-indented lines under their
parents, and emits transformed s-expression text that is then fed
through the regular reader (with curly-infix and neoteric enabled).
Inline forms like `f(x)` and `{a + b}` keep working unchanged.

### What is already implemented

- Indentation-based implicit `(...)` grouping (the t-expression core).
- `$` rest-of-line operator (`f $ g x` → `f (g x)`).
- Multi-line bracket continuation: `(...)`, `[...]`, `{...}` may cross
  newlines without triggering indent rules.
- `\` line continuation: a trailing `\` (followed only by whitespace
  and `\n`) joins the next physical line into the same logical line.
  Works at any bracket depth; respects strings and block comments;
  composes with `$` rest-of-line and with neoteric.
- Racket-style shebang support: a `#!` at byte 0 (followed by `/`,
  whitespace, or EOL) is treated as a line comment.  Works in plain
  `.tur`, in `.tur.sweet`, and in files where `#lang sweet-exp` is on
  line 2 after the shebang.
- String literals (`"..."`), line comments (`; ...`), and block
  comments (`#| ... |#`) respected by the line scanner, element
  counter, and emitter.

### Fixtures

- `tests/fixtures/t-expression-sweet-exp/` — indented `defn` bodies,
  nested `if`, multi-line `let`, `$` rest-of-line, neoteric inside the
  body.
- `tests/fixtures/sweet-exp-continuation/` — `\` continuation at top
  level, inside `$`, and inside `[]` brackets.
- `tests/fixtures/shebang-tur/` — shebang on a plain `.tur` file.
- `tests/fixtures/shebang-sweet-lang/` — shebang on line 1, `#lang
  sweet-exp` on line 2.
- Pre-existing `data-literal-sweet-exp/` and `fn-type-neoteric/`
  fixtures that use traditional parens — preserved unchanged; the
  preprocessor correctly leaves single-element bracket-continued lines
  alone.

### Explicitly out of scope

These were considered and *rejected* by design:

- **`#suite` directive** (per-file or per-name "wrap body in `(do
  ...)`" declaration).  Users will add explicit `do` levels when a
  macro's single-body slot needs multiple statements.
- **`:` as a Python-style block opener.**  SRFI-110 explicitly
  rejected it; Racket's sweet package doesn't have it; the only
  popular Lisp indent syntax that does (`#lang something`) is not a
  SRFI-110 superset.  Avoiding it also sidesteps the collision with
  Turmeric's `:int` keyword tokens.
- **`;` as expression separator.**  Conflicts with Turmeric's line
  comments and would force every existing file with end-of-line
  comments to be rewritten.  Users can still write multiple
  expressions on one line using explicit `(form1) (form2)`.

Two follow-ups remain.

---

## Follow-up 1 -- `\\` group operator

SRFI-110 reserves a leading `\\` token on a line to mean "**group**":
it suppresses the implicit list-wrap that the line would otherwise
get, making the rest of the line behave as if it were several siblings
at the same indent level.  This is the standard escape hatch for
expressing a sequence of forms whose head is *not* a function call.

### Examples

```
defn body
  \\
  (do-a)
  (do-b)
  (do-c)
```

Without `\\`, the three child lines would each become elements of
`(do-a)` (the first becomes head, the rest become arguments).  With
`\\`, the children are siblings under `defn body`:

```
(defn body (do-a) (do-b) (do-c))
```

`\\` may also appear inline at column 0 of a line to merge that line's
tokens with the previous line's group instead of opening a new one
(the "split-line" use).

### Scope

- Detect a leading `\\` token after the indent on a logical line.
- When present, the line is parsed for its remaining tokens but the
  enclosing wrap decision treats those tokens as if each were a
  separate sibling at the parent indent — equivalent to flattening
  them into the parent's child list.

### Files

- `src/compiler/reader.c` -- extend `SweetLine` with an `is_group`
  flag, set it in `sweet_collect_lines` when content begins with
  `\\`, and consume the `\\` in both `sweet_count_elements` and
  `sweet_emit_content` so it does not appear in the output.
  `sweet_analyze_node` skips it for wrap accounting and splices the
  line's tokens directly into the parent.

### Fixtures

- `tests/fixtures/sweet-exp-group/` -- exercise leading-`\\` as in
  the body example above, plus split-line `\\` (continuation of a
  previous group on a new line).

### Risks

- `\\` is also Turmeric's escape sequence inside string literals; the
  bracketed scanner already gates on `in_str`, so this composes
  cleanly.
- Diagnostics that point into a `\\` line will show the literal `\\`
  marker; that is acceptable for v1.

---

## Follow-up 2 -- Source-map-preserving diagnostics

Today the preprocessor inserts `(` / `)` directly into the source text
that the reader and diagnostics see.  Newlines are preserved, so error
line numbers stay correct, but the **error snippets** show the
transformed text (with implicit parens visible) and **columns** shift
by the count of inserted parens to the left of the token.

Example: `defn foo []` at column 0 in the user's file is shown as
`(defn foo []` in error context, with `defn` at column 2 instead of
column 1.  It is accurate (matches the offsets in the recorded
spans), but visually differs from what the user wrote.

### Goal

Diagnostics should show the **original** source verbatim — no
inserted parens, columns matching the user's file — while internal
span offsets continue to use the transformed text.

### Approach

1. Keep the transformed source as the reader's working buffer (no
   change to recording spans).
2. Build a parallel offset map: `transformed_offset →
   original_offset`.  The map is sparse — populate it only at
   character boundaries that match between the two streams (i.e.
   everywhere except across an inserted `(` or `)`).  A run-length
   representation suffices: each span between insertions is a single
   `{xform_start, orig_start, length}` triple.
3. Carry the map on the shadow `SourceFile` (extend the struct, or
   stash it in a side table keyed by `file_id`).
4. In `src/compiler/diag.c`, when rendering a snippet, look up the
   shadow file's map and translate `(line, col)` from the transformed
   coordinate system to the original; read the snippet text from the
   *original* `SourceFile->src`.

### Files

- `src/compiler/diag.h` / `diag.c` -- snippet rendering and span
  translation.
- `src/compiler/reader.c` -- emit the offset map alongside the
  transformed text and attach it to the shadow `SourceFile`.

### Risks

- Span translation must be self-consistent: every column the
  diagnostic reports must be a real column in the original.
  Inserted `(` / `)` have no original counterpart — if an error
  points *exactly* at one of them (e.g. an unmatched paren the
  preprocessor inserted), the translator should fall back to the
  nearest preserved column and add a note ("(inserted by sweet-exp
  grouping)").
- A bug in the offset map shows up as off-by-one diagnostics — easy
  to notice in fixtures.  Add a fixture whose `;; error:`
  expectation pins the column.

---

## Order of work

1. **`\\` group operator** -- moderate scope; clean signal (token at
   start of line) and well-understood semantics.
2. **Source-map diagnostics** -- larger scope; nice-to-have polish
   that does not affect what compiles.

Each follow-up can ship independently and each adds at least one
fixture under `tests/fixtures/`.
