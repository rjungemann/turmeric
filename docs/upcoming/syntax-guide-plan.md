---
title: Syntax Guide Plan
category: Planning
description: Plan for a unified syntax guide covering Turmeric s-expressions and sweet-expression mode
---

# Syntax Guide -- Plan

## Goal

Produce a single user-facing reference that teaches a newcomer to *read* and
*write* Turmeric source in both its surface syntaxes:

1. The default S-expression dialect (`.tur` files).
2. The sweet-expression dialect activated by `#lang sweet-exp` or a
   `.tursweet` extension (indentation + neoteric + `$` + curly-infix).

Today this information is scattered across `CLAUDE.md` (sweet-exp style
section), `docs/guides/reader-forms-guide.md` (reader-level forms only),
`docs/guides/formatter-guide.md` (formatting rules), `docs/guides/README.md`
(paired-example authoring), and the `data-literals-guide.md` (literal
syntax). There is no single place a new reader can land on to learn the
shape of the language.

## Scope -- what the guide must cover

### Part 1 -- Turmeric S-expression syntax

- Lexical conventions: identifiers, keywords (`:foo`), numbers (int, float,
  hex/bin/oct), strings, character escapes, ASCII-only requirement.
- Comments: line (`;`), block (`#| ... |#`), docstring (`;;;`) -- cross-link
  to the docstring standard in `CLAUDE.md`.
- The form tree: lists `(...)`, vectors `[...]`, maps `#map{...}`, sets
  `#set{...}`, and when each is legal (expression position vs. binding
  position).
- Function application: prefix `(f x y)`, zero-arg `(f)`.
- Special forms a reader will see first: `defn`, `def`, `let`, `if`, `when`,
  `cond`, `do`, `fn`, `for`, `while`, `import`, `export`, `defstruct`,
  `defmacro`. Pointers into the relevant deep-dive guides rather than full
  semantics.
- Type-annotation syntax in declarations (`:int`, `:Vec<:int>`, `& rest
  :T`, return-type marker after the parameter list).
- Reader macros and dispatch forms: cross-link to
  `reader-forms-guide.md` instead of duplicating its catalogue.
- Indentation conventions: the Clojure-style rules currently in
  `CLAUDE.md` (args under first arg; 2-space body for special forms;
  binding pairs on one line each).
- Inline-C blocks: the ` ```c ... ```) ` fence rule, why the closing
  triple-backtick must share a line with `)`.

### Part 2 -- Sweet-expression syntax

- How to opt in: `#lang sweet-exp` directive, `.tursweet` extension; what
  happens if both are present; behaviour when the directive is missing.
- The three tools and when each one wins:
  - Indentation (t-expr) -- replaces outer `(...)` for top-level forms and
    multi-line bodies.
  - Neoteric `f(x y)` -- inline calls and operator calls (`+(x y)`).
  - Rest-of-line `$ expr` -- when a line ends in a single nested call.
- Curly-infix `{a + b}` for arithmetic and operator precedence.
- Data literals composed inside sweet-exp (`#map{...}`, `#set{...}`,
  `[...]`), pointing at the data-literals guide for full semantics.
- What still uses traditional parens: `import`/`export`, cons lists,
  inline-C blocks, trivially short expressions.
- Mixing styles within one file (legal? recommended?) -- document the
  guidance currently in `CLAUDE.md` plus what the formatter will/won't
  rewrite.
- A complete side-by-side example: one non-trivial function shown in both
  syntaxes, ideally lifted from an existing test fixture so it stays
  honest.

### Part 3 -- Reference appendix

- Operator/keyword cheat sheet (one table, one row per form, columns for
  S-expr form, sweet-exp form, and a one-line gloss).
- Cross-reference table mapping each topic to its in-depth guide.
- Pointer to `tur format` for canonical output and `tur format --check`
  for CI.

## Non-goals

- Semantics of individual special forms beyond what is needed to recognise
  the syntax (the binding-forms, structs, effects, etc. guides own that).
- The reader-form catalogue itself -- `reader-forms-guide.md` stays the
  source of truth; the new guide links to it.
- Macro authoring (`defmacro` internals) -- separate concern.

## Deliverables

1. `docs/guides/syntax-guide.md` with frontmatter:
   ```
   ---
   title: Syntax Guide
   category: Getting Started
   description: How to read and write Turmeric -- s-expression and sweet-expression syntax
   ---
   ```
2. Add an entry under **Getting Started** in `docs/guides/README.md`,
   placed before `quickstart.md` so a first-time reader can ground
   themselves syntactically before diving into the prose tutorial.
3. Every code example given in both `turmeric` and `sweet-exp` blocks
   following the README's paired-example convention, so `genguides.py`
   renders the toggle widget.
4. ASCII-only content; `--` instead of em dashes (per fixture rule).
5. Generated HTML refreshed by `just docs`.

## Sources to consolidate (and de-duplicate)

| Source | What to lift | What to leave in place |
|---|---|---|
| `CLAUDE.md` sweet-exp section | Tool descriptions, decision guide, complete example | The section can shrink to a one-line pointer at the new guide once the guide ships. |
| `CLAUDE.md` indentation section | Clojure-style rules, examples | Same -- replace with pointer. |
| `CLAUDE.md` inline-C block rule | Fence-closure rule and rationale | Replace with pointer. |
| `reader-forms-guide.md` | Link target only | Untouched -- still the catalogue. |
| `data-literals-guide.md` | Link target only | Untouched. |
| `formatter-guide.md` | Cross-link for canonical output | Untouched. |

The guide is the *front door* to all of these; it should not become a
second copy of any of them.

## Open questions

- Should the guide also cover `tur fmt` output examples in-line, or punt
  to the formatter guide entirely? Default: punt, but include one
  before/after snippet so readers see what canonical formatting looks
  like.
- Does the guide need a "common mistakes" subsection (e.g. forgetting the
  closing `)` shares a line with ` ``` `, splitting binding pairs across
  lines, mixing `#lang sweet-exp` with neoteric-only files)? Lean yes,
  short -- four to six pitfalls.
- Where to host the side-by-side example: inline in the guide, or as a
  fixture file linked from the guide? Lean inline to keep the guide
  self-contained.

## Acceptance checklist

- [ ] `docs/guides/syntax-guide.md` exists with the structure above.
- [ ] `docs/guides/README.md` lists it under Getting Started.
- [ ] All examples are paired (`turmeric` + `sweet-exp`) where applicable
      and pass `just check-guides`.
- [ ] No new content duplicates the reader, data-literals, or formatter
      guides -- only cross-links.
- [ ] `CLAUDE.md` sweet-exp/indentation/inline-C sections trimmed to
      pointers (optional follow-up PR, not required for guide landing).
- [ ] `just docs` regenerates the HTML successfully.
