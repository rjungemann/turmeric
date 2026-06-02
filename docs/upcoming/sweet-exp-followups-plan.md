---
title: Sweet-Exp T-Expression Follow-Ups
category: Planning
description: Polish work for the sweet-exp (SRFI-110) preprocessor — `\\` group operator, `\` line continuation, and source-map-based diagnostics
---

# Sweet-Exp T-Expression Follow-Ups -- Plan

## Background

The indent-sensitive sweet-expression reader (`#lang sweet-exp` / files
with the `.tur.sweet` extension) landed as a textual preprocessor in
`src/compiler/reader.c`.  At a high level it walks the source into
logical lines, recursively groups deeper-indented lines under their
parents, and emits transformed s-expression text that is then handed to
the regular reader (with curly-infix and neoteric enabled).  The
preprocessor implements:

- Indentation-based implicit `(...)` grouping (the t-expression core).
- `$` rest-of-line operator (`f $ g x` → `f (g x)`).
- Multi-line bracket continuation (`(...)`, `[...]`, `{...}` may cross
  newlines without triggering indent rules).
- String literals (`"..."`), line comments (`; ...`), and block comments
  (`#| ... |#`) respected by the line scanner and element counter.

The unit covered by tests includes
`tests/fixtures/t-expression-sweet-exp/` (indented `defn` bodies, nested
`if`, multi-line `let`, `$`, neoteric inside the body) and the
pre-existing `data-literal-sweet-exp` / `fn-type-neoteric` fixtures that
use traditional parens — the preprocessor leaves single-element
bracket-continued lines alone.

Three follow-ups remain.

---

## Follow-up 1 -- `\\` group operator

SRFI-110 reserves a leading `\\` token on a line to mean "**group**":
it suppresses the implicit list-wrap that the line would otherwise get,
making the rest of the line behave as if it were several siblings at
the same indent level.  This is the standard escape hatch for
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
  separate sibling at the parent indent — equivalent to flattening them
  into the parent's child list.

### Files

- `src/compiler/reader.c` -- extend `SweetLine` with an `is_group` flag,
  set it in `sweet_collect_lines` when content begins with `\\`, and
  consume the `\\` in both `sweet_count_elements` and
  `sweet_emit_content` so it does not appear in the output.
  `sweet_analyze_node` skips it for wrap accounting and splices the
  line's tokens directly into the parent.

### Fixtures

- `tests/fixtures/sweet-exp-group/` -- exercise both leading-`\\` (as
  in the body example above) and split-line `\\` (continuation of a
  previous group on a new line).

### Risks

- The `\\` token also collides with Turmeric's escape sequence inside
  string literals — but those are gated by `in_str`, so the bracketed
  scanner already ignores them.
- Diagnostics that point into a `\\` line will show the literal `\\`
  marker; that is acceptable for v1.

---

## Follow-up 2 -- `\` line continuation

A line ending in a bare `\` should be joined with the next physical
line into one logical line, regardless of indent.  This complements
bracket-continuation: bracket continuation is implicit (any unbalanced
`(`, `[`, `{` carries to the next line), whereas `\` continuation is
explicit for lines that *would* otherwise close cleanly.

### Example

```
defn long-call [a :int b :int c :int d :int] :int
  some-very-long-function a \
                          b \
                          c \
                          d
```

Without `\`, only `some-very-long-function a` (a 2-token line) would
become the head, with `b`, `c`, `d` as four-space-indented children —
producing `(some-very-long-function a b c d)` *by accident* in this
specific case, but generally any indentation change would split the
call.  With `\`, the four physical lines become one logical line whose
content is `some-very-long-function a b c d`.

### Scope

- During `sweet_collect_lines`, when the last non-whitespace,
  non-comment character of a physical line is `\`, drop the `\` and
  splice the next physical line into the current logical line.
- Track that the join happened so the emitter can preserve newlines
  for diagnostic accuracy (we still want column reports to land on the
  right physical line).

### Files

- `src/compiler/reader.c` -- modify `sweet_collect_lines` to handle the
  trailing-`\` case; the rest of the pipeline (count/analyze/emit) does
  not change.

### Fixtures

- `tests/fixtures/sweet-exp-continuation/` -- a multi-arg call broken
  across lines with `\`, plus a control fixture confirming a bare `\`
  inside a string is *not* a continuation.

### Risks

- `\` is also Turmeric's char-escape inside string literals.  The
  scanner already tracks `in_str`; only continuation outside of
  `in_str` / `in_bc` / `bd > 0` should trigger.

---

## Follow-up 3 -- Source-map-preserving diagnostics

Today the preprocessor inserts `(` / `)` directly into the source text
that the reader and diagnostics see.  Newlines are preserved, so error
line numbers stay correct, but the **error snippets** show the
transformed text (with implicit parens visible) and **columns** shift
by the count of inserted parens to the left of the token.

Example: `defn foo []` at column 0 in the user's file is shown as
`(defn foo []` in error context, with `defn` at column 2 instead of
column 1.  It is accurate (matches the offsets in the recorded spans),
but visually differs from what the user wrote.

### Goal

Diagnostics should show the **original** source verbatim — no inserted
parens, columns matching the user's file — while internal span offsets
continue to use the transformed text.

### Approach

1. Keep the transformed source as the reader's working buffer (no
   change to recording spans).
2. Build a parallel offset map: `transformed_offset → original_offset`.
   The map is sparse — populate it only at character boundaries that
   match between the two streams (i.e. everywhere except across an
   inserted `(` or `)`).  A run-length representation suffices: each
   span between insertions is a single `{xform_start, orig_start,
   length}` triple.
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
  diagnostic reports must be a real column in the original.  Inserted
  `(` / `)` have no original counterpart — if an error points
  *exactly* at one of them (e.g. an unmatched paren the preprocessor
  inserted), the translator should fall back to the nearest preserved
  column and add a note ("(inserted by sweet-exp grouping)").
- A bug in the offset map shows up as off-by-one diagnostics — easy to
  notice in fixtures.  Add a fixture whose `;; error:` expectation
  pins the column.

---

## Order of work

1. **`\` line continuation** -- smallest change, isolated to
   `sweet_collect_lines`, unblocks the multi-arg call idiom.
2. **`\\` group operator** -- moderate scope; touches the analyzer and
   emitter but has a clean signal (token at start of line).
3. **Source-map diagnostics** -- largest scope; nice-to-have polish
   that does not affect what compiles.

Each follow-up can ship independently and each adds at least one
fixture under `tests/fixtures/`.

## Out of scope

- Re-pretty-printing sweet-exp via `tur fmt`.  The formatter currently
  treats `--lang sweet-exp` as "format using sweet-exp reader, emit
  s-expression output"; round-tripping back to indented form is a
  separate project.
- `#!sweet-exp` shebang support.
- Indent-sensitive macros (e.g. user-defined block forms that want
  Python-style suite syntax).
