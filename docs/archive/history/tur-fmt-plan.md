# Plan: `tur fmt` -- Turmeric source formatter

> **Status:** Draft Plan
> **Last Updated:** 2026-05-24
> **Type:** Tooling / CLI

---

## Overview

`tur fmt` formats `.tur` and `.tur.sweet` source files in place, applying the
indentation and style rules already documented in this repo's
`CLAUDE.md`. It is built into the `tur` binary; no external dependency.
The formatter is **idempotent** (`fmt(fmt(x)) == fmt(x)`) and **syntax-style
preserving** -- it never rewrites an s-expression file into sweet-exp or
the reverse; it formats each file in its existing style.

`tur fmt` is planned as a parallel deliverable to `tur run` (see
[`docs/tur-run-plan.md`](tur-run-plan.md)); the two ship in the same
release so the spice Justfile template's `fmt`, `check`, and `ci`
recipes are honest on day one.

---

## CLI

```sh
tur fmt                              # format every .tur and .tur.sweet under cwd
tur fmt src/ tests/                  # format only the given paths (files or dirs)
tur fmt --check                      # exit non-zero if any file would change; print the list
tur fmt --check --diff               # like --check, but also print unified diffs
tur fmt --stdout path/file.tur       # print formatted output to stdout, do not write
tur fmt --stdin                      # read from stdin, write formatted to stdout
tur fmt --stdin --lang sweet-exp      # tell the formatter which dialect stdin is

# Pass-through to the existing tur permission model (no surprises here):
tur fmt --dry-run src/               # alias for --check (matches `tur build --dry-run`)
```

Exit codes:

- `0` -- all files already formatted (or, without `--check`, all files
  successfully written).
- `1` -- with `--check`, at least one file would be reformatted; without
  `--check`, an I/O error occurred.
- `2` -- CLI / parse error (unparseable input, unknown flag).

---

## Style rules (the contract)

The formatter is a mechanical implementation of `CLAUDE.md`'s "Indentation
Style" and "Sweet-Expression Style" sections. Briefly:

- **Function calls**: arguments after the first align to the column of the
  first argument.
- **Special forms** (`defn`, `defmacro`, `defstruct`, `definstance`, `fn`,
  `let`, `loop`, `if`, `when`, `cond`, `do`, `for`, `while`, `import`,
  `export`, `defmodule`, `defpackage`): two-space body indent regardless of
  column.
- **Binding vectors** (in `let` / `loop` / similar): names align in a
  column, values align in a column.
- **Sweet-exp files** keep their indentation-based form. Neoteric
  (`f(x y)`), `$` rest-of-line, and curly-infix (`{a + b}`) are preserved
  exactly as written; the formatter does not "promote" s-exp calls to
  neoteric or vice versa.
- **Docstrings** (`;;;` blocks immediately preceding a definition) are
  preserved verbatim -- line breaks, spacing, and ASCII content unchanged.
- **Inline-C blocks** (` ```c ... ``` `) are preserved verbatim, including
  the closing ` ```) ` on the same line as the closing paren (the
  CLAUDE.md rule).
- **Comments**: `;` line comments are attached to the next form by
  position. Trailing comments stay on their line. Blank lines between
  top-level forms are preserved (with consecutive blanks collapsed to a
  single blank line).
- **ASCII only**: the formatter rejects non-ASCII input with a precise
  error (matches the existing fixture rule).
- **Trailing whitespace** stripped; files end with exactly one newline.

The intent is "what a careful human writes following CLAUDE.md." Anything
ambiguous in the style guide is decided here and back-ported to CLAUDE.md
as part of FT3.

---

## Modules

```
src/tur/fmt/
  lex.tur               -- "tur/fmt/lex"   tokenize (s-expr + sweet-exp)
  trivia.tur            -- "tur/fmt/trivia" comments + blank lines as first-class
  parse.tur             -- "tur/fmt/parse" build a concrete syntax tree with trivia
  style.tur             -- "tur/fmt/style" special-form table + binding-form table
  print_sexp.tur        -- "tur/fmt/print-sexp"   pretty print s-exp output
  print_sweet.tur       -- "tur/fmt/print-sweet"  pretty print sweet-exp output
  detect.tur            -- "tur/fmt/detect" decide dialect (extension + #lang line)
  cli.tur               -- "tur/fmt/cli"   argv dispatch, file walking, --check/--diff
tests/tur/fmt/
  lex_test.tur
  parse_test.tur
  print_sexp_test.tur
  print_sweet_test.tur
  idempotence_test.tur  -- fmt(fmt(x)) == fmt(x) on every stdlib file
  bootstrap_test.tur    -- `tur fmt --check stdlib/` returns 0
```

---

## Implementation phases

- [ ] **FT0** -- `tur/fmt/lex` tokenizes Turmeric source with trivia (every
  comment and blank line preserved as a token). `tur/fmt/trivia` defines
  the trivia-attached-to-form data model.

- [ ] **FT1** -- `tur/fmt/parse` builds a concrete syntax tree: each form
  carries its leading and trailing trivia. Round-trip test:
  `emit(parse(x)) == x` (byte-for-byte) on the stdlib.

- [ ] **FT2** -- `tur/fmt/style` special-form table (defn / defmacro /
  defstruct / definstance / fn / let / loop / if / when / cond / do / for /
  while / import / export / defmodule / defpackage); `tur/fmt/print-sexp`
  applies the table; output matches CLAUDE.md examples for every special
  form.

- [ ] **FT3** -- Binding-vector alignment (`let [a 1, bb 2, ccc 3]` ->
  names align in a column, values align in a column);
  function-call argument-alignment (args after first under col of first);
  any style points raised during implementation are written into
  CLAUDE.md alongside the code change.

- [ ] **FT4** -- Verbatim preservation: docstring blocks, inline-C blocks
  (including same-line ` ```) ` rule), string literals (no escape
  normalization), character literals, numeric literals (no canonicalization
  of integer / float representations).

- [ ] **FT5** -- `tur/fmt/detect` (`.tur.sweet` extension, or `#lang
  sweet-exp` first-line directive); `tur/fmt/print-sweet` applies sweet-exp
  style rules (preserve neoteric / `$` / curly-infix verbatim; normalize
  indentation to the CLAUDE.md examples); output matches the sweet-exp
  example in CLAUDE.md.

- [ ] **FT6** -- `tur/fmt/cli`: argv dispatch, recursive file walking
  (default to cwd; skip `build/`, `.tur-cache/`, `.git/`, `.turnb-cache/`);
  `--check`, `--diff` (unified-diff output), `--stdout`, `--stdin`,
  `--lang`. Exit codes per the table above.

- [ ] **FT7** -- **Bootstrap test**: running `tur fmt --check stdlib/`
  returns 0. This is the acceptance gate: until the stdlib is
  self-formatted, the formatter is not done. Land any whitespace-only
  cleanups to stdlib in a single commit so the diff is reviewable.

- [ ] **FT8** -- **Idempotence test**: for every `.tur` and `.tur.sweet`
  file in stdlib and in every spice's `src/`, assert `fmt(fmt(x)) ==
  fmt(x)` byte-for-byte. Run as part of CI.

- [ ] **FT9** -- Editor integration hints in `docs/guides/tur-fmt-guide.md`:
  a one-liner each for vim (`autocmd BufWritePre *.tur silent! !tur fmt %`),
  VS Code (point at the `formatOnSave` hook), helix (language
  `formatter` block), emacs (`before-save-hook`). README section in the
  main repo; `tur fmt --help` links to the guide.

---

## Design notes

### Why not just publish a style guide and let humans follow it

Style guides degrade. CLAUDE.md is good, but reviewers still spend cycles
on whitespace nits, and every contributor's editor enforces a slightly
different interpretation. A mechanical formatter ends the debate: the
contract is "what `tur fmt` produces." `tur fmt --check` in CI catches
drift on every PR.

### Why preserve sweet-exp vs s-exp instead of canonicalizing

`.tur` and `.tur.sweet` are different languages-of-presentation, chosen by
the file author for readability reasons. Forcing one onto the other would
make the formatter a refactoring tool, not a layout tool -- a much larger
scope and a much riskier set of edits (a `.tur.sweet` file converted to
`.tur` may still parse but read worse). The formatter respects the
author's choice of dialect and only normalizes layout within it.

### Why a concrete syntax tree and not the abstract one

The parser used by the compiler discards comments, blank lines, and exact
whitespace -- it does not need them. A formatter must preserve all three.
`tur/fmt/parse` is a separate parser whose output is a concrete syntax
tree, with trivia first-class. The cost is duplicating the read-side
state machine; the win is that formatter changes never risk breaking the
compiler's parser.

### Comment placement

Comments are notoriously the place formatters break user intent.
`tur/fmt/parse` attaches each comment to the nearest following form (or
to the enclosing form's trailing position if it is the last thing inside
parens). Trailing comments on the same source line as a form stay
trailing. End-of-file trailing comments are preserved without a form
attached.

### Bootstrap as the acceptance test

The strongest test of a formatter is "self-format the entire stdlib and
review the diff." FT7 is exactly this test, and the resulting whitespace
commit is the one-time disruption that proves the contract. After
landing, `tur fmt --check stdlib/` runs in CI on every commit.

---

## Risks

1. **Sweet-exp parser fidelity.** Sweet-exp is more complex than plain
   s-exp (indentation is significant, neoteric and `$` and curly-infix
   coexist). FT5 carries the highest implementation risk; the
   `examples/` in CLAUDE.md form the test corpus, supplemented by every
   `.tur.sweet` file in the codebase.

2. **Inline-C block contents.** `tur fmt` must not touch the C inside a
   ` ```c ... ``` ` block (the formatter is not a C formatter). FT4's
   fixture set includes blocks with `;`, `(`, `)`, and unbalanced
   characters inside string literals to ensure the formatter does not
   try to "balance" them.

3. **Editor format-on-save loops.** Some editors interpret a formatter
   that rewrites the file as "the file changed on disk" and trigger a
   reload + format cycle. The standard mitigation (do nothing if the
   bytes did not change) is implemented in FT6: when output equals
   input, do not write.

4. **CLAUDE.md drift.** As contributors write new code, style decisions
   not covered by the guide will surface. The contract: the formatter
   is the source of truth, CLAUDE.md is its rendering. Any FT-phase
   PR that adds a style decision also updates CLAUDE.md in the same
   commit.

---

## Integration with `tur run`

Once `tur fmt` ships, the spice template's `fmt` recipe (defined in
[`docs/tur-run-plan.md`](tur-run-plan.md)) stays exactly as written --
`tur fmt src/ tests/` -- with no `|| true` shield needed. The template's
`check` and `ci` recipes are updated to include `tur fmt --check src/
tests/` so CI fails on style drift in any spice that follows the
template.

The joint acceptance test for the three-subcommand release (`tur run`,
`tur fmt`, `tur new`) lives in the `tur new` companion section of
[`docs/tur-run-plan.md`](tur-run-plan.md): NW6 scaffolds a temp spice,
runs `tur run ci`, and asserts exit 0. If any of the three is broken,
NW6 fails.
