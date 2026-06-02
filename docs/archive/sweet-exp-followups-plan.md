---
title: Sweet-Exp T-Expression Follow-Ups
category: Planning
description: Status doc for the sweet-exp (SRFI-110) preprocessor — all planned follow-ups landed
---

# Sweet-Exp T-Expression Follow-Ups -- Plan

> **Status:** All planned follow-ups have landed.  This document is
> retained as the design record; once it has been reviewed it can be
> moved to `docs/archive/history/`.

## Background

The indent-sensitive sweet-expression reader (`#lang sweet-exp` / files
with the `.tur.sweet` extension) is implemented as a textual
preprocessor in `src/compiler/reader.c`.  It walks the source into
logical lines, recursively groups deeper-indented lines under their
parents, and emits transformed s-expression text that is then fed
through the regular reader (with curly-infix and neoteric enabled).
Inline forms like `f(x)` and `{a + b}` keep working unchanged.

### What is implemented

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
- **`\\` GROUP operator** (SRFI-110): a leading double-backslash on a
  line suppresses the implicit list-wrap, flattening that line's
  tokens and its descendants into the parent's element list.  Three
  call-site patterns are supported and covered by fixtures:
  - `\\` alone with siblings at the same indent (parent gets siblings
    directly).
  - `\\` alone with children at a deeper indent (children flatten into
    the parent).
  - `\\` followed by inline tokens on the same line (tokens flatten
    into the parent).
- **Source-map-preserving diagnostics**: every error snippet renders
  the user's original `.tur.sweet` source verbatim (no inserted
  parens, no `\\` markers stripped on display, columns matching the
  user's file).  Internal span offsets still reference the
  transformed text for correctness, and a sparse `xform_offset →
  orig_offset` run-length map (`SweetMap`) carried on the shadow
  `SourceFile` translates spans at snippet-render time.
- String literals (`"..."`), line comments (`; ...`), and block
  comments (`#| ... |#`) respected by the line scanner, element
  counter, and emitter.

### Fixtures

- `tests/fixtures/t-expression-sweet-exp/` — indented `defn` bodies,
  nested `if`, multi-line `let`, `$` rest-of-line, neoteric inside the
  body.
- `tests/fixtures/sweet-exp-continuation/` — `\` continuation at top
  level, inside `$`, and inside `[]` brackets.
- `tests/fixtures/sweet-exp-group/` — leading-`\\` siblings,
  leading-`\\` with deeper children, leading-`\\` with inline tokens.
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
  ...)`" declaration).  Users add explicit `do` levels when a
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

### `\\` SPLIT variant — not implemented

SRFI-110 also defines `\\` as a "SPLIT" marker when it appears *after*
some tokens on a line, indicating that the previous group continues
across the newline.  Only the **GROUP** form (leading `\\` on a line)
is implemented today; SPLIT would only matter when authoring a
sequence of items each starting with the same group, which is rare in
Turmeric usage.  If a real need surfaces, add it as a small extension
to `sweet_collect_lines` and `sweet_analyze_node` — the line-content
range and `is_group` flag are already in place.

## Implementation notes

### Source-map design

`SweetMap` (defined in `src/compiler/diag.h`) is a run-length array of
`{xform_offset, orig_offset, length}` triples kept sorted by
`xform_offset`.  Each "run" represents a contiguous byte range copied
verbatim from the original to the transformed source.  Bytes in the
gaps between runs are reader-inserted (`(`, `)`, the synthesized `\n`
for `\` continuation, etc.) and have no original counterpart.

Building: the preprocessor's emit path threads a `SweetEmit { Buf *out;
SweetMap *map; }` struct through `sweet_emit_content` and the per-line
loop in `sweet_preprocess`.  Two helpers — `emit_copy_` (records a
run) and `emit_insert_char_` (does not record) — make the intent
explicit at each emit site.  Adjacent copies that are contiguous in
both spaces are merged into a single run.

Lookup: `sweet_map_translate_offset` does a binary search for the
rightmost run with `xform_offset <= input`.  Offsets that fall in
inserted gaps are mapped to the end of the preceding run (the closest
real position).

Rendering: `render_snippet_ex` in `src/compiler/diag.c` checks for
`f->xform_map != NULL` at function entry; when present, it shadows `f`
with a stack-local `SourceFile` pointing at `f->orig_src`/`orig_len`
and rewrites `span` to original coordinates (line, col, off_start,
off_end) before the body of the function runs.  No other code in
`diag.c` needs to know about the transformation.
