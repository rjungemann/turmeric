# tur run feature audit: missing just features worth implementing

> **Status:** draft (2026-08-20). **Track:** tooling. **Type:** CLI / justfile
> compatibility.
>
> **Builds on:** the original tur-run-plan.md (RN0-RN9) and the current
> `src/compiler/justrun.c` implementation.  Referenced just version: 1.54.0
> (from casey/just).

## 0. Summary

`tur run` is Turmeric's built-in Justfile-compatible task runner that allows
spice and project authors to ship a `Justfile` without requiring the upstream
`just` binary to be installed.  It currently implements a subset of the full
`just` feature set.  This document audits that subset against upstream just
1.54.0, identifies the gaps, and ranks them by implementation value.

The headline numbers:

| Category | just v1.54.0 | tur run (current) | Gap |
|----------|--------------|-------------------|-----|
| Recipe attributes | ~25 types | 1 (`[private]`) | **24 missing** |
| Parameters | Full (named, flags, validation) | Basic (positional, variadic) | **Large** |
| Modules / imports | Yes | No | **All missing** |
| Built-in functions | ~50 | 9 | **41 missing** |
| Settings | ~25 | 4 | 21 missing |
| CLI flags | ~20 | ~10 | ~10 missing |
| Shebang recipes | Yes | No | **All missing** |

The largest and most impactful gaps are in **recipe attributes**, **parameter
features**, **modules/imports**, and **built-in functions**.

## 1. What tur run supports today

Verified against `src/compiler/justrun.c` lines 7-23 and the implementation:

- Recipe definitions with shell-command bodies (tab or 2+ space indent; the
  original 4-space-only bug was fixed)
- Recipe dependencies (chained, deps-with-arguments)
- Recipe parameters with optional defaults and variadic `+PARAM` / `*PARAM`
- Variable assignment (`name := "value"`, `export name := "value"`)
- Interpolation (`{{ var }}` in recipe bodies and dep args)
- Line prefixes (`@` silent, `-` continue on failure, `@-`, `-@`)
- Settings: `set shell`, `set dotenv-load`, `set positional-arguments`,
  `set windows-shell`
- Built-in functions: `env_var`, `env_var_or_default`, `os`, `arch`,
  `justfile_directory`, `invocation_directory`, `uppercase`, `lowercase`,
  `trim`, `quote`
- Listing: `tur run` / `tur run --list` `[--json]` `[--all]`
- Aliases: `alias NAME := TARGET` (parsed and resolved)
- The default recipe
- Dotenv loading from the Justfile's directory
- Private recipes: `[private]` attribute and `_`-prefixed names hidden from
  `--list`

## 2. Missing features ranked by implementation value

### 2.1 Tier 1: high value, relatively low effort

These are the "quick wins" — features that provide significant user benefit
for modest implementation cost.

| Feature | just | tur run | User value | Implementation effort |
|---------|------|---------|------------|----------------------|
| `--set VAR VALUE` | Yes | No | High | Low |
| `[group('name')]` | Yes | No | High | Low-Medium |
| `[unix]` / `[windows]` | Yes | No | High | Medium |

#### 2.1.1 `--set VAR VALUE` CLI flag

Allows overriding justfile variables from the command line without editing the
file.

```sh
# Instead of editing the Justfile, users can do:
tur run --set CFLAGS="-O3" build
```

Implementation: add to argument parsing in `cmd_justrun`, pass through to
variable evaluation.  Low effort, high user demand.

#### 2.1.2 `[group('name')]` attribute

Enables organizing recipes into logical groups for `--list` output.  With
60+ recipes in the main turmeric Justfile, the flat list is overwhelming.

```just
[group('build')]
debug:
  ...

[group('build')]
release:
  ...

[group('test')]
test: build
  ...
```

Implementation: parse attribute, store on recipe, modify `--list` output to
print group headers.  Low-medium effort, high UX improvement.

#### 2.1.3 `[unix]` and `[windows]` platform attributes

Enable cross-platform Justfiles that adapt to the current OS.

```just
[unix]
configure:
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

[windows]
configure:
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

Implementation: parse attributes, check `os()` at runtime, skip or error on
non-matching recipes.  Medium effort, high value for cross-platform projects.

### 2.2 Tier 2: high value, medium effort

| Feature | just | tur run | User value | Implementation effort |
|---------|------|---------|------------|----------------------|
| Named / flag parameters | Yes | No | High | Medium-High |
| Modules / imports | Yes | No | High | High |
| Shebang recipes | Yes | No | Medium | Medium |

#### 2.2.1 Named and flag parameters

Essential for Justfiles that need CLI-like interfaces.

```just
# Current (supported):
tag VERSION:
  git tag -a v{{VERSION}}

# Desired (missing):
[arg('version', short='v', long='version')]
[arg('force', long='force', value='true')]
tag version force?:
  git tag -a v{{version}} {{force}}
```

Support for `--name value`, `--name=value`, `-n value`, and `--flag` syntax.
Implementation requires parsing attribute syntax, storing parameter metadata,
and handling option-style argument passing.  Medium-high effort but unlocks
CLI-like Justfile interfaces.

#### 2.2.2 Modules and imports

Critical for organizing large Justfiles.  The main turmeric repo's Justfile
is 577 lines spanning build, docs, web, perf, release, and Windows concerns.

```just
# Root justfile
mod build
mod docs
mod web
```

With each module in its own file (`build.just`, `docs.just`, etc.).

Implementation: add module/import parsing, file loading, scoping rules,
dependency management between modules.  High effort but essential for
large-scale adoption.

#### 2.2.3 Shebang recipes

Support recipes written in arbitrary languages (Python, Node, etc.).

```just
polyglot: python js perl

python:
  #!/usr/bin/env python3
  print("Hello from Python")
```

Implementation: detect `#!` line, save recipe body to temp file, mark
.executable, run.  On Windows, split shebang and invoke directly.  Medium
effort, useful for non-shell scripting.

### 2.3 Tier 3: medium value, low-medium effort

| Feature | just | tur run | User value | Implementation effort |
|---------|------|---------|------------|----------------------|
| `[no-cd]` | Yes | No | Medium | Low |
| `[confirm]` | Yes | No | Medium | Low |
| `path_exists()` etc. | Yes | No | Medium | Low-Medium |
| Backtick substitution | Yes | No | Medium | Medium |

#### 2.3.1 `[no-cd]` attribute

Prevent `tur run` from changing directory to the Justfile's location before
executing the recipe.

```just
[no-cd]
run-here:
  echo "Running from {{invocation_directory()}}"
```

Implementation: simple flag check before chdir.  Low effort.

#### 2.3.2 `[confirm]` attribute

Require user confirmation before executing destructive operations.

```just
[confirm("This will delete all build artifacts. Continue?")]
clean:
  rm -rf build/
```

Implementation: prompt user, read input, abort if not confirmed.  Low
effort, high safety value.

#### 2.3.3 Additional built-in functions

Most commonly requested: `path_exists()`, `replace()`, `join()`, `error()`.

Implementation: per-function additions to `eval_builtin`.  Low-medium effort,
medium user value.

#### 2.3.4 Backtick command substitution

Allow shell command evaluation in variable assignments and interpolation.

```just
GIT_REV := `git rev-parse HEAD`

build:
  echo "Building at {{ `git rev-parse --short HEAD` }}"
```

Implementation: shell evaluation at parse time adds complexity (security,
quoting, caching).  Medium effort, medium value.  Can be worked around with
explicit recipes.

## 3. Features NOT worth implementing (yet)

### 3.1 Unstable upstream features

- `[cache]` attribute — still unstable in just 1.54.0
- `set lists` — still unstable in just 1.54.0

Wait for upstream stabilization before implementing.

### 3.2 Niche or situational features

- Most additional settings (`allow-duplicate-recipes`, `fallback`,
  `windows-powershell`, etc.) — too niche, not enough demand
- Most additional CLI flags (`--choose`, `--completions`, `--dump`,
  `--edit`, `--man`) — not critical for core use cases
- Style functions (`style()`) — very situational
- Hash/UUID functions (`sha256()`, `uuid()`) — rarely needed in build scripts
- Datetime functions — niche use cases
- Semver matching — situational
- User directory functions — can use `env_var("HOME")` etc.

### 3.3 Can be worked around

- Conditional expressions — can use separate recipes
- Heredoc/multi-line strings — can use multiple recipe lines
- Recipe groups with `--group` flag — depends on attribute support

## 4. Recommended implementation order

### Phase 1: Quick wins (1-2 days each)

1. `--set VAR VALUE` CLI flag
2. `[group('name')]` attribute
3. `[unix]` / `[windows]` attributes

These provide immediate user-visible improvements with minimal risk.

### Phase 2: Core gaps (3-7 days each)

4. Named and flag parameters
5. Modules and imports
6. Shebang recipes

These address the largest functional gaps and are essential for serious
Justfile usage.

### Phase 3: Nice-to-have (1-3 days each)

7. `[no-cd]` attribute
8. `[confirm]` attribute
9. Additional built-in functions (`path_exists`, `replace`, `join`, `error`)
10. Backtick command substitution

These round out the feature set for power users.

## 5. What full parity would require

To achieve full `just` v1.54.0 compatibility, `tur run` would need to add:

- ~24 additional recipe attributes
- Full parameter system (named, flags, validation, constraints)
- Complete module and import system
- ~41 additional built-in functions
- ~21 additional settings
- ~10 additional CLI flags
- Shebang recipe support
- Conditional expressions
- Backtick command substitution

Estimated effort: **6-8 weeks of focused work** for a single developer,
or **2-3 sprints** for a small team.  The value proposition is questionable
for Turmeric's use case — most spices need only the core subset, and users who
need advanced just features can install `just` itself (the documented fallback
path).

## 6. Decision framework

When considering whether to implement a missing feature, ask:

1. **Is it on the critical path for a real spice?**  If yes, prioritize.
2. **Does it have a simple workaround?**  If yes, deprioritize.
3. **Does it enable a common pattern we see in the wild?**  If yes, prioritize.
4. **Is the implementation effort < 2 days?**  If yes, probably worth doing.
5. **Is the feature stable upstream?**  If no, wait.

Using this framework:

| Feature | Critical? | Workaround? | Common? | Effort | Stable? | Decision |
|---------|-----------|-------------|---------|--------|---------|----------|
| `--set` | No | No | Yes | Low | Yes | **Do it** |
| `[group]` | No | No | Yes | Low | Yes | **Do it** |
| `[unix]/[windows]` | Yes | Bash conditionals | Yes | Medium | Yes | **Do it** |
| Named params | No | Separate recipes | Yes | Medium | Yes | **Do it** |
| Modules | Yes | Manual splitting | Yes | High | Yes | **Do it** |
| Shebang | No | Explicit shell | Yes | Medium | Yes | **Do it** |
| `[cache]` | No | Manual caching | No | Medium | **No** | Wait |
| Style functions | No | Shell echo | No | Low | Yes | Skip |

## 7. Related work

- Original tur-run-plan.md: the v0.1.0 scope and phase breakdown (RN0-RN9)
- `src/compiler/justrun.c`: the current implementation
- `tools/just-vs-tur-run.sh`: CI parity test between `just` and `tur run`
- `tests/run-tur-run-alias.sh`: alias resolution regression test
- `tests/run-tur-run-rhs-eval.sh`: RHS evaluation tests

## 8. Open questions

1. Should we add a `--strict` or `--pedantic` mode that errors on any
   unsupported feature, for CI use?  Currently errors are only raised when
   trying to execute an unsupported feature.

2. Should we track "partial support" vs "no support" separately?  For example,
   we support basic aliases but not module aliases (`alias f := frontend`).

3. Should we maintain a formal compatibility table in the docs, showing
   which just features are supported in which `tur run` version?

4. Is there value in adding a `--compatibility` flag that warns on any
   constructs not in our supported subset, even if they happen to work?

---

*Generated by Mistral Vibe.*
*Co-Authored-By: Mistral Vibe <vibe@mistral.ai>*