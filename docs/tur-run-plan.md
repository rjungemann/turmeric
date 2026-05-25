# Plan: `tur run` -- Justfile-compatible task runner

> **Status:** Draft Plan
> **Last Updated:** 2026-05-24
> **Type:** Tooling / CLI

---

## Overview

A new subcommand of the `tur` CLI for executing build / test / dev tasks
defined in a `Justfile`. The goal is that any spice or Turmeric project ships
a `Justfile` and any consumer -- whether they have `just` installed or not --
can run the recipes by typing `tur run <recipe>`.

This plan covers two pieces:

1. **`tur run`** -- a new subcommand that parses and executes a Justfile-compatible
   subset directly in Turmeric (no dependency on the upstream `just` binary
   at runtime). When the Justfile uses a feature outside the supported
   subset, `tur run` exits with a clear message that points at the
   upstream `just` binary as the fallback.
2. **A `Justfile` template for new spices**, written out automatically by
   `tur new` and offered as a one-shot scaffold via `tur run --init`. The
   template covers the build / test / docs / release tasks every spice
   needs.

Together these mean: `tur new my-spice && cd my-spice && tur run` works on
day one, and every spice's contributor experience is the same set of
recipes -- `tur run build`, `tur run test`, `tur run docs`, `tur run tag 0.2.0`.

---

## Naming: `tur run` vs `tur pls`

**Recommendation: `tur run`.** The `<tool> run <recipe>` pattern is what
`npm`, `pnpm`, `bun`, `yarn`, `deno task`, and `cargo make` all expose.
Users coming from any of those ecosystems will guess it on the first try.

`tur pls` is friendlier but cryptic on first encounter. We can register
`pls` as an aliased subcommand (zero implementation cost; `cli.c` maps both
names to the same entry point), so power users who want it get it without
paying for it in discoverability.

`tur run` does *not* collide with a future "run a Turmeric program"
subcommand. The natural shape for that is either bare `tur path/to/file.tur`
(which the launcher already supports), or `tur exec` to match the rest of
the verbs we already use (`tur build`, `tur test`, `tur install`). Both
leave `run` free for the task-runner semantics.

If we keep both, `tur run` is the documented form and `tur pls` is an
undocumented alias mentioned only in a one-line easter egg in `tur run --help`.

---

## Why a subset implementation and not a wrapper

Three options were on the table:

| Option | Verdict |
|--------|---------|
| Shell out to upstream `just` if installed; error if not | Rejected -- introduces a hard external dep for "the common case" |
| Vendor the `just` Rust source as a build dep | Rejected -- pulls in Rust toolchain, defeats the point |
| Native Turmeric implementation of a Justfile subset | **Recommended** -- self-contained, ~500-1000 lines, covers what spice Justfiles actually use |

The trade is real: a subset cannot run arbitrary Justfiles from the wild.
The mitigation is that when `tur run` encounters an unsupported feature it
prints exactly which feature was unsupported and points at upstream `just`:

```
tur run: unsupported Justfile feature on line 14: recipe attribute [unix]
        Install `just` (https://just.systems) to run this recipe, or
        remove the [unix] attribute if the recipe is portable.
```

This means: most spices "just work" with bundled `tur run`, and the
sophisticated few that need full `just` get a clean fallback path -- they
just declare `just` as a tool dep.

---

## Supported Justfile feature subset (v0.1.0)

### Included

- **Recipe definitions** with shell-command bodies (one shell line per
  body line), tab- or 4-space-indented.
- **Recipe dependencies**: `test: build`, including chained dependencies
  and dependencies-with-arguments (`test: (build "release")`).
- **Recipe parameters** with optional defaults and a final variadic
  parameter: `tag VERSION:`, `build mode='debug':`, `forward +ARGS:`.
- **Variable assignment**: `name := "value"` and `export name := "value"`.
- **Interpolation**: `{{ var }}` (and `{{var}}`) in recipe bodies and in
  the dependency-call arguments. Whitespace inside the braces is
  tolerated.
- **Line prefixes**: `@` (silent -- do not echo the command) and `-`
  (continue on non-zero exit). `@-` and `-@` both accepted.
- **Comments**: `#` line comments. A run of `#` comments immediately
  above a recipe header is treated as the recipe's **doc comment** and
  shown in `tur run --list`.
- **Settings**: `set shell := ["sh", "-c"]`,
  `set dotenv-load := true`, `set positional-arguments := true`,
  `set windows-shell := ["cmd.exe", "/c"]`.
- **A small set of built-in functions** in interpolation expressions:
  `env_var("NAME")`, `env_var_or_default("NAME", "fallback")`,
  `os()`, `arch()`, `justfile_directory()`, `invocation_directory()`,
  `uppercase(s)`, `lowercase(s)`, `trim(s)`.
- **Listing**: `tur run` with no recipe lists recipes (doc comment +
  parameters); `tur run --list` is the explicit form.
- **The `default` recipe**: if a recipe named `default` exists, `tur run`
  with no arguments runs it instead of listing.

### Deferred (parser flags an unsupported-feature error)

- Recipe attributes (`[private]`, `[unix]`, `[windows]`, `[no-cd]`,
  `[no-exit-message]`, `[confirm]`)
- Conditional expressions (`if ... { ... } else { ... }`)
- String functions beyond the small set above (`replace`, `replace_regex`,
  `sha256`, `path_exists`, ...)
- Modules and imports (`mod foo`, `import 'subfile'`)
- Recipe groups (`[group: 'lint']`)
- Backtick command substitution in assignments
- Heredoc / multi-line strings in recipe bodies
- Aliases (`alias b := build`) -- though we may add this in NB1, see below
- Recipe arguments after dependency calls (`build: clean (test "fast")`)
  beyond the simple case above
- Cross-recipe variable scoping (`set export := true`)

Each of these emits a specific error message naming the feature and line
number; we never silently misinterpret.

### Compatibility target

For any spice's Justfile that uses only the included subset, running
upstream `just <recipe>` and running `tur run <recipe>` must produce the
**same observable behavior**: same commands executed, same environment,
same exit status, same stdout/stderr ordering (modulo the buffering
caveats below).

A CI job in this repo runs the spice-template Justfile under both `just`
and `tur run` and diffs the outputs as a regression test.

---

## CLI

```sh
tur run                          # if `default` recipe exists, run it; else list recipes
tur run --list                   # always list; one line per recipe with doc + params
tur run --list --json            # machine-readable form (for editor integrations)

tur run build                    # run the `build` recipe
tur run tag 0.2.0                # run `tag` with arg "0.2.0"
tur run --                       # explicit end-of-options marker
tur run build -- --release       # pass `--release` through to the recipe as a positional
tur run --dry-run build          # print the resolved commands but do not execute
tur run --verbose build          # echo recipe execution metadata (start, end, elapsed)
tur run --justfile path/to/Justfile build
                                 # use an explicit Justfile (else search upward from cwd)
tur run --chdir DIR build        # run the recipe from DIR (else from Justfile's dir)

tur run --init                   # write a starter Justfile into cwd (see template below)
tur run --init --force           # overwrite an existing Justfile

tur pls ...                      # undocumented alias for `tur run`
```

Exit codes:

- `0` -- recipe succeeded.
- `1` -- recipe ran but exited non-zero (propagate exit code, capped at 255).
- `2` -- CLI / parse error (unsupported feature, recipe not found, missing arg).
- `127` -- could not find or read the Justfile.

---

## Spice Justfile template

`tur run --init` and `tur new <spice>` both write the following starter
Justfile. It depends only on the `tur` binary -- no external tools -- so
`tur run` can execute every recipe.

```just
# Justfile for {{ spice_name }}
#
# Run `tur run --list` for the full set of recipes.
# Add your own recipes below; the ones above are the contract that the
# spice template, CI, and `tur publish` rely on.

# Default to listing recipes; override for your most common task if you like.
default:
    @tur run --list

# Build the spice (debug profile).
build:
    tur build

# Build with optimizations.
release:
    tur build --release

# Run the spice's test suite; depends on a fresh debug build.
test: build
    tur test

# Re-build on source changes; useful while iterating.
watch:
    tur build --watch

# Remove build artifacts and the local cache.
clean:
    rm -rf build/ .tur-cache/

# Generate the per-spice API docs from `;;;` docstrings.
docs:
    tur docs

# Format sources in place.
fmt:
    tur fmt src/ tests/

# Type-check / lint without producing an artifact, and verify style.
check:
    tur check
    tur fmt --check src/ tests/

# Tag a release: `tur run tag 0.2.0` produces `{{ spice_name }}-v0.2.0`.
tag VERSION:
    git tag -a "{{ spice_name }}-v{{ VERSION }}" -m "{{ spice_name }} v{{ VERSION }}"
    @echo "Tagged {{ spice_name }}-v{{ VERSION }}. Push with: git push --tags"

# Install this spice into the local registry for downstream testing.
install:
    tur install .

# CI entry point: clean + check + test + docs. Used by the default GitHub Actions
# workflow that `tur new` also scaffolds.
ci: clean check test docs
```

The template uses **only the supported subset**, so the CI regression
test (running it under both `just` and `tur run`) catches any drift.

### Placeholders

`{{ spice_name }}` in the template is replaced by `tur new` / `tur run
--init` with the actual spice name at scaffold time. The written-out
Justfile contains no double-brace placeholders -- only resolved values
plus the `{{ VERSION }}` interpolation that Justfile itself processes at
runtime.

---

## Conventions

```
src/
  tur/                            -- existing CLI driver
    cli.c                         -- subcommand dispatch; register `run` and `pls`
  tur/run/                        -- new subdirectory for the runner
    parse.tur                     -- "tur/run/parse"   tokenize + parse Justfile
    ast.tur                       -- "tur/run/ast"    AST node types + accessors
    eval.tur                      -- "tur/run/eval"   resolve deps, interpolate, exec
    settings.tur                  -- "tur/run/settings"  `set` directives
    functions.tur                 -- "tur/run/fns"    built-in interpolation functions
    list.tur                      -- "tur/run/list"   --list output formatting
    init.tur                      -- "tur/run/init"   scaffold a starter Justfile
    template.just                 -- the literal template above
    cli.tur                       -- "tur/run/cli"    argv parsing and dispatch
  tests/tur/run/
    parse_test.tur
    eval_test.tur
    list_test.tur
    init_test.tur
    fixtures/                     -- known-good and known-bad Justfiles
      ok-simple/Justfile
      ok-deps/Justfile
      ok-template/Justfile
      err-unsupported-attr/Justfile
      ...
  tools/
    just-vs-tur-run.sh            -- CI script: diffs `just <r>` vs `tur run <r>` for each fixture
```

---

## Architecture

```
argv
  |
  v
tur/run/cli            -- parse own flags; locate Justfile (walk up from cwd)
  |
  v
tur/run/parse          -- tokenize -> parse -> AST (recipes, vars, settings)
  |
  v
tur/run/ast            -- typed AST values (recipe, dep-call, body-line, expr)
  |
  v
tur/run/eval           -- topo-sort deps; bind params; interpolate; spawn shell
  |
  +-- tur/run/functions   -- env_var / os / arch / uppercase / ... evaluator
  +-- tur/run/settings    -- shell choice, dotenv, positional-args policy
  |
  v
process exec (posix_spawn / CreateProcess), captured exit status
```

Each recipe body line is a single shell invocation: the chosen shell
(`set shell := ...` or the default `["sh", "-c"]`) is spawned with the
interpolated body line as its argument. Stdout and stderr are passed
through to the user's terminal unchanged. Recipe-prefix semantics
(`@`, `-`) are enforced before/after the spawn.

---

## Implementation phases

- [ ] **RN0** -- `tur/run/parse` covering recipe headers (name, params,
  deps), bodies (one line each), variable assignments, comments,
  doc-comment association. Round-trip test: `parse(emit(parse(x))) ==
  parse(x)` on the spice template.

- [ ] **RN1** -- `tur/run/ast` types and accessors; `tur/run/eval`
  topological sort of the dependency graph; missing-dep / cyclic-dep /
  duplicate-recipe error messages. Recipes with no body and no
  interpolation execute end-to-end (no params yet).

- [ ] **RN2** -- Interpolation: `{{ var }}` substitution in recipe
  bodies. Recipe parameters with defaults; positional argument binding;
  variadic `+ARGS` and `*ARGS`. The `tag VERSION:` recipe in the spice
  template executes correctly.

- [ ] **RN3** -- Line prefixes (`@`, `-`); `set shell`, `set
  dotenv-load`, `set windows-shell`, `set positional-arguments`;
  default-recipe selection; the `default` recipe in the spice template
  works.

- [ ] **RN4** -- Built-in functions: `env_var`, `env_var_or_default`,
  `os`, `arch`, `justfile_directory`, `invocation_directory`,
  `uppercase`, `lowercase`, `trim`. Tests for each function on macOS
  and Linux CI.

- [ ] **RN5** -- `tur/run/list` formatting (plain and `--json`);
  `--justfile`, `--chdir`, `--dry-run`, `--verbose` flags;
  recipe-not-found and missing-argument error messages with suggestions
  (Levenshtein for recipe-name typos, recipe-signature reminder for
  argument mistakes).

- [ ] **RN6** -- `tur/run/init`: embed `template.just` as a string
  constant; substitute `{{ spice_name }}`; write `Justfile` into cwd
  (refuse if existing unless `--force`). Wire into `tur new` so newly
  scaffolded spices ship with the template.

- [ ] **RN7** -- Unsupported-feature detection: every deferred Justfile
  construct emits a specific error with line + column + a one-line
  upgrade hint. The `err-*` fixtures verify each one. `tur run --help`
  links to the supported-subset section of the docs.

- [ ] **RN8** -- CI: `tools/just-vs-tur-run.sh` runs every `ok-*`
  fixture's recipes under both `just` and `tur run` and asserts equal
  exit codes and identical stdout/stderr (the diff job is allowed to
  skip on hosts without `just` installed, with a soft warning). Add a
  GitHub Actions job to the main repo and to the spice-template
  workflow.

- [ ] **RN9** -- Docs: `docs/guides/tur-run-guide.md` (writing
  Justfiles, what the subset covers, when to install upstream `just`);
  README section in the main turmeric repo; an entry on the
  turmeric-spices contributor guide pointing at `tur run --init` for
  existing spices that want to adopt the template.

---

## Design notes

### Why Justfile and not `package.json scripts` / `Makefile` / custom

| Candidate | Why not |
|-----------|---------|
| `Makefile` | Tab/space pitfalls, no params, no `@`/`-` semantics, Windows hostile |
| `package.json scripts` | Requires JSON; awkward for multi-line bodies; npm ecosystem coupling |
| `pyproject.toml [tool.tur.run]` | Niche; no escape hatch to upstream tool when stuck |
| Custom Turmeric DSL | We'd be inventing what `just` already specifies; no reuse of users' muscle memory |
| **Justfile** | Established, documented, has a fallback tool, syntax users already know |

### Shell choice and Windows

The default shell is `["sh", "-c", "{}"]`. On Windows we honor `set
windows-shell := ["cmd.exe", "/c", "{}"]` (or `["powershell.exe", "-c",
"{}"]`) when present, otherwise fall back to `sh` if `sh.exe` is on PATH
(Git Bash, MSYS), else error with a clear "this Justfile expects a POSIX
shell; install Git Bash or set windows-shell in the Justfile" message.

This matches upstream `just`'s default and is the convention every spice
template uses.

### Output buffering parity with `just`

`tur run` does not capture stdout/stderr -- the shell child inherits the
parent's fds directly. This is what upstream `just` does and is necessary
for interactive recipes (`tur run watch`, `tur run shell`). Side effect:
when `tur run --verbose` interleaves its own logging, it goes to stderr
and may interleave with the recipe's stderr in ways that differ from
`just`'s formatting. The CI parity test ignores `--verbose` output for
this reason.

### Dotenv loading

`set dotenv-load := true` reads a `.env` file from the Justfile's
directory and sets variables in the recipe's environment, matching
upstream semantics (variables already in the environment win). We use a
~50-line dotenv parser (handles `KEY=value`, `KEY="value with spaces"`,
`#` comments, `export KEY=value`). No interpolation inside the `.env`
file in v0.1.0.

### Why scaffold the Justfile and not document the recipe names

Documentation rots; scaffolding compels. Every spice generated by `tur
new` ships with the same Justfile, so consumers know that `tur run test`
works in any spice -- they do not need to read each project's README to
discover the recipe name. Adding to the template means *every* new
spice gets the recipe for free; deprecating a recipe means *every* new
spice stops including it.

### `tur new` integration

When `tur new <name>` is run, the scaffolder writes the standard layout
(see existing scaffolds for `tur new-web` etc.) and the templated
`Justfile`. The template is the single source of truth for "standard
spice tasks." If we later add a new common task (say `bench` for
benchmarking), it lands in `template.just` and everyone running `tur
new` gets it on the next release.

For existing spices, `tur run --init` writes the same template. If an
existing `Justfile` is present, the command refuses to overwrite and
suggests `tur run --init --force` or manually merging the missing
recipes.

---

## Risks and open questions

1. **Subset drift.** Upstream `just` adds features; ours does not, until
   we add them. Mitigation: the CI parity test on the spice template
   guards against semantic divergence on the things we *do* support;
   the unsupported-feature error guards against silent misbehavior on
   the things we do not. We commit to publishing a `tur run`
   compatibility table tied to a specific upstream `just` version in
   the docs guide.

2. **Recipe name collisions with `tur` subcommands.** `tur run test`
   resolves to the Justfile's `test` recipe, not `tur test` -- which
   itself happens to be one of the standard recipes' bodies (`test:
   build` -> `tur test`). This is the intended behavior, but it can
   surprise: typing `tur build` runs the top-level `tur build`,
   *not* the Justfile's `build` recipe. Documented prominently in the
   guide; `tur run build` is the form to use when you want the recipe.

3. **Shell injection in interpolation.** Recipe bodies are shell
   strings; `{{ var }}` substitutes without quoting. This matches
   upstream `just`. We document the standard mitigation (use `"{{
   var }}"` in the body) and ship a `quote(s)` built-in for cases
   where the value cannot be wrapped (e.g. inside a single-quoted
   shell string).

4. **`tur fmt` ships in the same release as `tur run`.** The template's
   `fmt` recipe is `tur fmt src/ tests/` with no `|| true` shield. See
   the [Companion subcommand: `tur fmt`](#companion-subcommand-tur-fmt)
   section below for the scope, phases, and acceptance test that backs
   this guarantee. If the two subcommands ever ship out of order, the
   template-update step (RN6) waits on `tur fmt` so we never publish a
   template that calls a missing subcommand.

5. **`tur new` may not exist yet** at the time `tur run` ships. Two
   resolutions are acceptable: (a) `tur new` is a parallel deliverable
   in the same release; (b) `tur run` ships first and `tur run --init`
   is the only path to the template, with `tur new` integration as a
   follow-up. The phases above keep the `tur new` integration in RN6
   so either ordering works.

---

## Companion subcommand: `tur fmt`

The spice template's `fmt` recipe calls `tur fmt src/ tests/`. This section
scopes that subcommand so the recipe lands intact and so contributors have
one formatter every spice uses. `tur fmt` is planned as a parallel
deliverable to `tur run`; the two ship in the same release so the template
is honest on day one.

### Overview

`tur fmt` formats `.tur` and `.tursweet` source files in place, applying the
indentation and style rules already documented in this repo's
`CLAUDE.md`. It is built into the `tur` binary; no external dependency.
The formatter is **idempotent** (`fmt(fmt(x)) == fmt(x)`) and **syntax-style
preserving** -- it never rewrites an s-expression file into sweet-exp or
the reverse; it formats each file in its existing style.

### CLI

```sh
tur fmt                              # format every .tur and .tursweet under cwd
tur fmt src/ tests/                  # format only the given paths (files or dirs)
tur fmt --check                      # exit non-zero if any file would change; print the list
tur fmt --check --diff               # like --check, but also print unified diffs
tur fmt --stdout path/file.tur       # print formatted output to stdout, do not write
tur fmt --stdin                      # read from stdin, write formatted to stdout
tur fmt --stdin --lang tursweet      # tell the formatter which dialect stdin is

# Pass-through to the existing tur permission model (no surprises here):
tur fmt --dry-run src/               # alias for --check (matches `tur build --dry-run`)
```

Exit codes:

- `0` -- all files already formatted (or, without `--check`, all files
  successfully written).
- `1` -- with `--check`, at least one file would be reformatted; without
  `--check`, an I/O error occurred.
- `2` -- CLI / parse error (unparseable input, unknown flag).

### Style rules (the contract)

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

### Modules

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

### Implementation phases

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

- [ ] **FT5** -- `tur/fmt/detect` (`.tursweet` extension, or `#lang
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

- [ ] **FT8** -- **Idempotence test**: for every `.tur` and `.tursweet`
  file in stdlib and in every spice's `src/`, assert `fmt(fmt(x)) ==
  fmt(x)` byte-for-byte. Run as part of CI.

- [ ] **FT9** -- Editor integration hints in `docs/guides/tur-fmt-guide.md`:
  a one-liner each for vim (`autocmd BufWritePre *.tur silent! !tur fmt %`),
  VS Code (point at the `formatOnSave` hook), helix (language
  `formatter` block), emacs (`before-save-hook`). README section in the
  main repo; `tur fmt --help` links to the guide.

### Design notes

#### Why not just publish a style guide and let humans follow it

Style guides degrade. CLAUDE.md is good, but reviewers still spend cycles
on whitespace nits, and every contributor's editor enforces a slightly
different interpretation. A mechanical formatter ends the debate: the
contract is "what `tur fmt` produces." `tur fmt --check` in CI catches
drift on every PR.

#### Why preserve sweet-exp vs s-exp instead of canonicalizing

`.tur` and `.tursweet` are different languages-of-presentation, chosen by
the file author for readability reasons. Forcing one onto the other would
make the formatter a refactoring tool, not a layout tool -- a much larger
scope and a much riskier set of edits (a `.tursweet` file converted to
`.tur` may still parse but read worse). The formatter respects the
author's choice of dialect and only normalizes layout within it.

#### Why a concrete syntax tree and not the abstract one

The parser used by the compiler discards comments, blank lines, and exact
whitespace -- it does not need them. A formatter must preserve all three.
`tur/fmt/parse` is a separate parser whose output is a concrete syntax
tree, with trivia first-class. The cost is duplicating the read-side
state machine; the win is that formatter changes never risk breaking the
compiler's parser.

#### Comment placement

Comments are notoriously the place formatters break user intent.
`tur/fmt/parse` attaches each comment to the nearest following form (or
to the enclosing form's trailing position if it is the last thing inside
parens). Trailing comments on the same source line as a form stay
trailing. End-of-file trailing comments are preserved without a form
attached.

#### Bootstrap as the acceptance test

The strongest test of a formatter is "self-format the entire stdlib and
review the diff." FT7 is exactly this test, and the resulting whitespace
commit is the one-time disruption that proves the contract. After
landing, `tur fmt --check stdlib/` runs in CI on every commit.

### Risks

1. **Sweet-exp parser fidelity.** Sweet-exp is more complex than plain
   s-exp (indentation is significant, neoteric and `$` and curly-infix
   coexist). FT5 carries the highest implementation risk; the
   `examples/` in CLAUDE.md form the test corpus, supplemented by every
   `.tursweet` file in the codebase.

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

### Integration with `tur run`

Once `tur fmt` ships, the spice template's `fmt` recipe stays exactly as
written -- `tur fmt src/ tests/` -- with no `|| true` shield needed. The
template's `check` and `ci` recipes are updated to include `tur fmt
--check src/ tests/` so CI fails on style drift in any spice that
follows the template.

---

## Shared work

### Main turmeric repo

- New section in the top-level `README.md`: a two-paragraph "Running tasks"
  block pointing at `tur run --list` and `tur run --init`.
- `CLAUDE.md` update: replace the existing "uses `just` (not `make`)" note
  with "uses Justfile recipes via `tur run` (or upstream `just` if
  installed)." The recipe names stay the same; only the invocation form
  changes.
- New entry under `docs/guides/`: `tur-run-guide.md` (see RN9).

### turmeric-spices repo

- The contributor guide gains a "Standard recipes" section listing the
  template's recipes and what each does. A short instruction tells
  existing-spice maintainers to run `tur run --init --force` after
  reviewing the diff to bring an older spice up to template parity.
- The CI workflow gains a step that runs `tur run ci` from each spice
  directory, replacing the current ad-hoc `cd spices/foo && just test`
  pattern. The CI job no longer needs `just` installed on the runner.

### Integration with other planned spices

- `tur-notebook`: `tur nb` is a separate subcommand and unaffected. Spice
  authors who ship notebooks can add `tur run examples` to their
  Justfile (e.g. `tur nb render examples/quickstart.tur.md`).
- `tur-frame` / `tur-stats`: standard template covers them; no changes
  needed.
- `tur-plot`: same; the `examples` and `gallery` recipes some plot users
  will want can be added on top of the template, not replacing it.

---

## Future work

| Item | Why deferred |
|------|--------------|
| Recipe attributes (`[private]`, `[unix]`, `[windows]`) | Common in larger Justfiles; small parser addition; v0.2 |
| Module / import support (`mod foo`, `import 'x'`) | Larger surface; only matters for very large projects |
| Backtick command substitution in assignments | Adds shell evaluation at parse time; want to keep parse pure for v0 |
| Recipe groups + grouped listing | Cosmetic; small effort once attributes land |
| `tur run --completions <shell>` | After the subset settles; shell completion script generator |
| Watch mode integration (`tur run --watch test`) | Composable with existing `tur build --watch`; revisit if demand appears |
