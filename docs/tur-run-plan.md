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

The formatter that the template's `fmt` / `check` / `ci` recipes call --
`tur fmt` -- is scoped in its own document:
[`docs/tur-fmt-plan.md`](tur-fmt-plan.md). The two subcommands ship in the
same release; see Risk #4 below.

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
    cli.c                         -- subcommand dispatch; register `run`
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
   `fmt` recipe is `tur fmt src/ tests/` with no `|| true` shield. The
   scope, phases, and acceptance test that back this guarantee live in
   [`docs/tur-fmt-plan.md`](tur-fmt-plan.md). If the two subcommands
   ever ship out of order, the template-update step (RN6) waits on
   `tur fmt` so we never publish a template that calls a missing
   subcommand.

5. **`tur new` ships in the same release as `tur run`.** Earlier drafts of
   this plan treated `tur new` as a separate, possibly-later deliverable
   with `tur run --init` as the interim path. It is now an in-scope
   companion subcommand -- see the
   [Companion subcommand: `tur new`](#companion-subcommand-tur-new)
   section below for scope, phases, and the acceptance test. RN6
   continues to own the "wire the template into `tur new`" step so
   the dependency direction is explicit (RN6 waits on the `tur new`
   phases, not the other way around).

---

## Companion subcommand: `tur new`

`tur new` scaffolds a new spice on disk: directory layout, `build.tur`
manifest, a starter source file, a `README.md`, a `.gitignore`, the
standard `Justfile` from RN6, and (optionally) a GitHub Actions workflow.
It is planned as a parallel deliverable to `tur run` and `tur fmt`; the
three ship in the same release so that `tur new my-spice && cd my-spice
&& tur run` works on day one (the headline promise at the top of this
plan).

### Overview

The goal is a one-command path from "I want to write a new spice" to a
buildable, testable, formattable, CI-ready directory. Every file the
scaffolder writes is one the spice author would have written by hand;
the scaffolder removes the friction of remembering the exact layout and
the standard recipe names.

`tur new` is **not** a project generator with templates and prompts. It
is a single deterministic scaffold with a small set of flags. If we
later want multiple flavors (binary spice vs. library spice vs.
notebook spice), they land as `--kind <name>` flags, not as an
interactive picker.

### CLI

```sh
tur new my-spice                       # scaffold ./my-spice/
tur new path/to/my-spice               # scaffold at an explicit path
tur new my-spice --kind lib            # default; library spice
tur new my-spice --kind bin            # binary spice (adds a main entry point)
tur new my-spice --author "Name <e@x>" # override the git-config-derived author
tur new my-spice --license MIT         # write LICENSE (MIT|Apache-2.0|BSD-3-Clause|none)
tur new my-spice --no-git              # skip `git init` and the initial commit
tur new my-spice --no-ci               # skip the GitHub Actions workflow
tur new my-spice --no-justfile         # skip the Justfile (`tur run --init` adds it later)
tur new my-spice --dry-run             # print the file list that would be written; do not write
tur new --here                         # scaffold into the current directory (must be empty)
```

Exit codes:

- `0` -- spice scaffolded successfully.
- `1` -- target directory exists and is non-empty (no `--force`); or an
  I/O error occurred.
- `2` -- CLI / parse error (invalid spice name, unknown `--kind`,
  conflicting flags).

Spice-name validation: lowercase ASCII, digits, and `-`; must start with
a letter; 2-64 characters. The validator emits the same error message
the `build.tur` parser uses for invalid `:name` values, so a name that
passes here will pass the manifest parser.

### Generated layout

```
my-spice/
  build.tur                  -- manifest: :name, :version, :description, :spices []
  Justfile                   -- the RN6 template (unless --no-justfile)
  README.md                  -- one-paragraph description + `tur run --list` hint
  .gitignore                 -- build/, .tur-cache/, .turnb-cache/, *.o, etc.
  LICENSE                    -- only if --license != none
  src/
    my_spice.tur             -- module skeleton with `;;;` module docstring
  tests/
    my_spice_test.tur        -- one passing test that imports the module
  .github/workflows/
    ci.yml                   -- runs `tur run ci` on ubuntu-latest + macos-latest
                                (omitted with --no-ci)
```

For `--kind bin`, `src/my_spice.tur` is replaced with a `defn main []
:int` entry point and `build.tur` gains a `:bin "my-spice"` field.

### Modules

```
src/tur/new/
  scaffold.tur               -- "tur/new/scaffold"  file-tree definition + writer
  templates.tur              -- "tur/new/templates" embedded file contents as cstrs
  validate.tur               -- "tur/new/validate"  name + path validation
  git.tur                    -- "tur/new/git"      `git init` + initial commit (optional)
  cli.tur                    -- "tur/new/cli"      argv dispatch and orchestration
tests/tur/new/
  validate_test.tur
  scaffold_test.tur          -- writes into a tmp dir, asserts the file set
  e2e_test.tur               -- scaffolds + runs `tur run ci` inside the result
```

### Implementation phases

- [ ] **NW0** -- `tur/new/validate`: spice-name and target-path rules.
  Reused by NW1 and by `build.tur`'s `:name` field parser to keep the
  two in lockstep. Unit tests for the boundary cases (leading digit,
  uppercase, length, reserved names like `tur`/`build`/`test`).

- [ ] **NW1** -- `tur/new/templates`: embed every scaffold file as a
  string constant (mirrors `tur run --init`'s `template.just` approach,
  RN6). Each template carries a small set of substitutions
  (`{{ spice_name }}`, `{{ author }}`, `{{ year }}`, `{{ license }}`)
  resolved at scaffold time, **before** the file is written.

- [ ] **NW2** -- `tur/new/scaffold`: pure function that takes a
  resolved-options record and returns a `Vec[FileEntry]`
  (`{path, contents, mode}`); a separate writer function consumes the
  vec and creates directories + files. `--dry-run` prints the vec; the
  writer is only invoked without `--dry-run`. Refuses to write into a
  non-empty target unless `--here` was given over an empty dir.

- [ ] **NW3** -- `tur/new/git`: when present and `--no-git` was not
  passed, run `git init`, write a sensible `.gitignore`, and create an
  initial commit ("Initial scaffold from `tur new`"). Git failure is
  non-fatal -- the spice is still on disk; we print a one-line warning
  and continue. Honors `GIT_AUTHOR_*` env vars.

- [ ] **NW4** -- `tur/new/cli`: argv parsing (`--kind`, `--author`,
  `--license`, `--no-git`, `--no-ci`, `--no-justfile`, `--dry-run`,
  `--here`); resolution of defaults (author from `git config user.name`
  + `user.email`; license = `none`; kind = `lib`); error messages for
  invalid combinations.

- [ ] **NW5** -- Wire into `tur` dispatch (`src/tur/cli.c`). The `new`
  subcommand registers alongside `run`, `fmt`, etc. `tur new --help`
  documents every flag and the generated layout.

- [ ] **NW6** -- **Bootstrap test**: in CI, scaffold a temp spice
  (`tur new tmp-spice`), `cd` in, run `tur run ci` (which itself runs
  `tur fmt --check`, `tur check`, `tur test`, `tur docs`), and assert
  exit 0. This is the acceptance gate: until a freshly scaffolded
  spice passes its own CI recipe end to end, `tur new` is not done.

- [ ] **NW7** -- Documentation: `docs/guides/tur-new-guide.md` (what
  gets scaffolded, what each file is for, how to evolve a spice past
  the template); README section in the main repo; an entry on the
  turmeric-spices contributor guide pointing at `tur new` for new
  spices and `tur run --init` for existing ones.

### Design notes

#### Why bake author / license / CI in instead of leaving them to the user

A spice without a `.gitignore` accumulates build artifacts on first
commit; a spice without a CI workflow has no enforcement of the
template's contract; a spice without a license is legally ambiguous to
contribute to. These are the "didn't think to add it on day one"
papercuts that the scaffolder is best positioned to prevent. The flags
to opt out (`--no-ci`, `--license none`) exist for the cases where the
author has a strong reason.

#### Why no interactive prompts

Interactive prompts make `tur new` un-scriptable, un-testable end to
end (NW6 would need a pty harness), and slower for the common case.
Defaults derived from `git config` and one flag per decision cover
every realistic scenario.

#### Why scaffold the test file with one passing test

A scaffold that includes a *failing* placeholder test trains authors to
expect red CI on day one and dilutes the signal of a real failure. A
scaffold with *no* tests means `tur run test` has nothing to do, and
new authors may not learn the test-file convention. One passing test
that imports the module and asserts the module name strikes the
balance: it proves the build/test path works and demonstrates the
convention without inviting the author to leave it broken.

#### Why `tur new` and not `tur run --init`

`tur run --init` exists for **existing** spices that want to adopt the
standard `Justfile`. `tur new` exists for **new** spices that want the
whole layout. Keeping them as two subcommands keeps each one's purpose
crisp; `tur new` calls into `tur/run/init`'s templating code under the
hood so there is one source of truth for the `Justfile` template.

### Risks

1. **Git config absent on fresh systems.** `git config user.name` may
   be unset in containers and CI. NW4 falls back to environment
   variables (`GIT_AUTHOR_NAME` / `GIT_AUTHOR_EMAIL`), then to a
   placeholder string (`"Anonymous <unknown@local>"`) with a stderr
   warning. The scaffolded files always parse and build regardless.

2. **Cross-platform path handling.** `tur new path/to/my-spice` must
   work on Windows (`\` separators, drive letters). NW2 routes every
   path through the same `tur/path` helpers `tur run` uses; the test
   suite exercises Windows path shapes on the macOS/Linux side via
   fixture strings, with a CI job on Windows for end-to-end coverage.

3. **Template drift between `tur new` and `tur run --init`.** Both
   embed the same `template.just`. Mitigation: `tur/new/templates`
   imports the `Justfile` body from `tur/run/init`'s constant rather
   than copying it. A unit test asserts byte-equality of the two
   sources to catch accidental forks.

### Integration with `tur run` and `tur fmt`

- `tur new` writes the RN6 `Justfile` template verbatim. The template's
  `fmt`, `check`, and `ci` recipes assume `tur fmt` exists -- which it
  does, because all three subcommands ship together (Risks #4 and #5).
  See [`docs/tur-fmt-plan.md`](tur-fmt-plan.md) for the formatter's
  scope and phases.
- NW6's bootstrap test (scaffold + `tur run ci`) is the joint
  acceptance test for the three-subcommand release: if `tur new`, `tur
  run`, or `tur fmt` is broken, NW6 fails.

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
