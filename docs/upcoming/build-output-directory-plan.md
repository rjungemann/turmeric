# Build Output Directory Plan

> **Partial landing (2026-06-09):** the default-output-name half landed
> at `src/main.c`'s `cmd_build_multi_files` site -- when an enclosing
> `build.tur` is in play, its `:name` is used for the default
> `-o lib<name>.so` / `-o <name>` output instead of the cwd basename.
> Eliminates the `lib..so` empty-basename regression and gives
> workspace-member builds-from-`.` a sane default. The rest of the plan
> (artifact relocation under `<root>/build/{obj,bin,lib}/`, `:build-dir`
> manifest field, `TUR_BUILD_DIR` env var, `--build-dir` flag) is still
> outstanding.

## Problem

`tur build <dir>` and the shared-library spice build drop generated artifacts
into the source tree, alongside `.tur` sources. Per-module `<spice>__<mod>.c`
and `<spice>__<mod>.h` files (plus the final `lib<base>.so`) land in the
working directory the build was invoked from. In `../turmeric-spices/`
this means every spice dir accumulates dozens of generated files and a stray
`lib..so` after a workspace build:

```
spices/stats/stats__sample.c
spices/stats/stats__summary.c
spices/stats/lib..so
spices/tidal/tidal__event.c
spices/tidal/lib..so
...
```

The repo's `.gitignore` masks `*.c` / `*.h` outside curated trees, but the
files still exist on disk, churn editor file-watchers, slow `rg`/`fzf`, and
confuse contributors who can't tell generated from hand-written sources.
`lib..so` (an empty-basename artifact from `default_output_name(".", ...)`)
also looks like a bug.

Today's escape hatches don't cover the common case:

- `tur emit-c --output-dir <dir>` already exists, but only for the
  one-shot multi-input `emit-c` path -- not `tur build` or `tur build <dir>`
  or `tur build --shared <dir>`.
- `-o <path>` only redirects the *final* executable/shared object; the
  intermediate `.c`/`.h` files still spill next to the sources.
- The `/tmp/tur-build/` cache is used by single-file `tur build` only.

## Goals

1. Route every intermediate `.c`/`.h`/`.o` from a project/spice build into
   a single configurable directory, defaulting to `<project-root>/build/`.
2. Same for the linked artifact (`lib<base>.so` / `<base>` exe) -- the
   default landing site moves from cwd to `<build-dir>/`.
3. Make the directory overridable in three layers, with predictable
   precedence: CLI flag > env var > `build.tur` manifest field > default.
4. Keep `tests/run.sh` and the fixture harnesses byte-for-byte compatible
   (they already write to per-fixture scratch dirs; nothing should move).
5. Fix the `lib..so` empty-basename bug as a side effect: when the project
   has a manifest, the package name from `build.tur` becomes the default
   basename instead of `basename(".")`.

## Non-goals

- Changing where `/tmp/tur-build/<sanitized>.c` lives for the single-file
  `tur build <file.tur>` path. That cache is already off-tree and is
  invisible to users.
- Reworking `.tur-abi-cache/` -- it should *move into* the build dir
  (`<build-dir>/.tur-abi-cache/`) but its layout is unchanged.
- Incremental rebuild / dependency tracking. Out of scope; this plan only
  relocates artifacts.
- Changing fixture harness layout. `tests/fixtures/**/expected.c` snapshots
  stay in place.

## Design

### CLI

Add a uniform flag accepted by `tur build` (single-file, directory, and
`--shared` forms) and `tur emit-c`:

```
tur build --build-dir <dir> ...
tur build -B <dir> ...                   # short alias
tur build --shared --build-dir <dir> ...
tur emit-c --build-dir <dir> ...         # alias for existing --output-dir
```

`--output-dir` stays as a deprecated alias on `emit-c` for one release.

### Environment

`TUR_BUILD_DIR=<path>` -- consulted only when no CLI flag is given. Useful
for CI matrices that want all platforms to share a recipe.

### Manifest

Add an optional top-level keyword to `build.tur`:

```turmeric
(defpackage stats
  :name      "stats"
  :version   "0.1.0"
  :build-dir "build"          ; relative to manifest dir; default if omitted
  :exports   [stats/sample stats/summary ...])
```

Workspace manifests (`:members [...]`) accept the same field; each member's
own `build.tur` may override it. Resolution is relative to the manifest's
own directory, so a workspace at `../turmeric-spices/` with `:build-dir
"build"` produces `../turmeric-spices/build/<spice>/...`, not one
`build/` per spice.

### Precedence

1. `--build-dir` / `-B` on the command line.
2. `TUR_BUILD_DIR` env.
3. `:build-dir` in the nearest `build.tur` (walking up from the input,
   then the workspace root if the spice is a workspace member).
4. Default: `<project-root>/build/` where project-root is the dir holding
   the nearest `build.tur`, or cwd if there is none.

The resolved path is created with `mkdir -p` semantics (the existing
`emit-c --output-dir` helper already does this; lift it into a shared
`ensure_dir(path)`).

### Layout inside `<build-dir>/`

For a project/spice build:

```
<build-dir>/
  obj/                        # per-module .c / .h / .o
    <mod>__<sub>.c
    <mod>__<sub>.h
  bin/
    <name>                    # exe build
  lib/
    lib<name>.so              # --shared build (or .dylib on macOS)
  .tur-abi-cache/             # moved out of source tree
  exports.manifest            # --shared only
```

Sub-dirs (`obj/`, `bin/`, `lib/`) keep the top level scannable and let a
single `rm -rf build/` reset state without clobbering anything the user
might have placed there.

For workspace builds, each member gets a namespaced subtree so artifacts
don't collide:

```
<workspace>/build/
  stats/{obj,lib}/...
  tidal/{obj,lib}/...
```

### Default-name fix

`default_output_name(dir, ...)` currently produces an empty string when
`dir == "."`, yielding `lib..so`. Two-part fix:

1. When a `build.tur` is in play, prefer its `:name` (or `:package`) over
   the directory basename.
2. When no manifest exists and the basename would be empty, fall back to
   the absolute cwd's basename via `realpath()`.

## Implementation steps

1. **Shared helper** -- introduce `resolve_build_dir(input, cli_flag, env,
   manifest)` in `src/main.c` that returns a heap path and ensures the
   directory exists. Returns `NULL` on failure with a diagnostic.
2. **CLI plumbing** -- thread `build_dir` through `cmd_build`,
   `cmd_build_multi_files`, `cmd_emit_c_to_dir`, and the `--shared`
   entrypoint. All `snprintf(..., "%s.c", mangled)` / `"%s.h"` /
   `"lib%s.so"` sites become `snprintf(..., "%s/obj/%s.c", build_dir, ...)`
   etc. Touch points already located at `src/main.c:3214`, `:3215`, `:3176`,
   `:1222`, `:1223`.
3. **Manifest field** -- extend the manifest reader (currently parses
   `:name`, `:version`, `:members`, `:exports`, `:spices`,
   `:reader-macros`) to recognise `:build-dir`. Store on the `PkgManifest`
   struct (or its successor). Propagate to workspace member resolution.
4. **Env var** -- read `TUR_BUILD_DIR` in `resolve_build_dir`.
5. **Default-name fix** -- patch `default_output_name` per the rule above
   and route the manifest `:name` in as an override.
6. **Move `.tur-abi-cache/`** -- the index writer at `src/main.c:1318`
   currently writes `<out_dir>/.tur-abi-cache`; once `<out_dir>` is the
   build dir, no further change is needed. Audit the reader path for
   hard-coded `./.tur-abi-cache` lookups and rebase them onto build dir.
7. **Auto-`.gitignore`** -- when the build dir is first created, write
   a `.gitignore` containing `*` inside it (so contents are never
   accidentally committed even if the build dir itself is tracked).
8. **CLAUDE.md note** -- add a one-paragraph entry under "Build System"
   explaining the new default and the override layers.
9. **Doc** -- update `docs/guides/developing-spices-guide.md` and
   `docs/guides/tur-run-guide.md` with the new flag and manifest field.

## Migration

- Default flips immediately to `<root>/build/`. Anyone with scripts that
  expect `lib<name>.so` in cwd will break; the release notes call this out
  and recommend `--build-dir .` (explicit opt-in to legacy layout) as the
  short-term escape hatch.
- `emit-c --output-dir` keeps working; emits a one-line deprecation note
  on stderr suggesting `--build-dir`. Removed two releases later.
- `tests/run.sh` is unaffected (uses per-fixture scratch dirs already).
  Smoke-check by running the suite under `TUR_BUILD_DIR=/tmp/tur-bd-test`
  and confirming the source tree is untouched after.

## Validation

- `bash tests/run.sh` -- zero `FAIL` lines, no new `.c`/`.h`/`.so` files
  in fixture dirs (`find tests/fixtures -name '*.c' -newer /tmp/marker`
  should be empty post-run).
- `cd ../turmeric-spices && tur run build` -- artifacts land under
  `../turmeric-spices/build/`; `git status` shows nothing new in the
  spice trees.
- `tur build --build-dir /tmp/xx examples/hello.tur` -- emits
  `/tmp/xx/{obj,bin}/...`.
- `TUR_BUILD_DIR=/tmp/xx tur build examples/hello.tur` -- same.
- `tur build --build-dir .` -- legacy layout, regression test for users
  who pin to old behavior.
- Add a new fixture `tests/fixtures/build-dir-relocates-artifacts/` that
  asserts the generated `.c` file lands under `build/obj/`, not cwd.

## Open questions

- Should `tur run` (project mode) honor `:build-dir` for the
  REPL `.tur-repl-cache/`, or keep that cache separate? Leaning: keep
  separate -- the REPL cache is hot-reload state, not build output.
- Naming bikeshed: `:build-dir` vs `:target-dir` (Cargo) vs `:out` vs
  `:build`. Picking `:build-dir` for least surprise; revisit before
  shipping if `tur` adopts more Cargo-isms elsewhere.
- For workspace builds, is `build/<member>/` or `<member>/build/` the
  right default? Plan picks the former so a single `rm -rf build`
  cleans everything; flag in PR review.
