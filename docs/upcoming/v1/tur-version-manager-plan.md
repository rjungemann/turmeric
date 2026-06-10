# Turmeric Version Manager (`tvm`) Plan

> **Status:** Proposed -- not started.
> **Last Updated:** 2026-06-06
> **Type:** tooling / distribution
> **Modeled after:** `nvm` (Node Version Manager), with cues from `rustup` and `pyenv`.

---

## Overview

Turmeric ships as a single `tur` binary, but the project moves fast: a
working tree pinned to v0.18 will fail to build a spice that targets
v0.19 because of typing-migration churn, and conversely, a user
debugging a regression often wants to bisect between two tags without
re-bootstrapping CMake by hand. Today the only options are:

1. Build from source per checkout (slow; requires CMake + a recent
   toolchain).
2. `brew install turmeric` (always one version, stale within a week).
3. Drop a tagged tarball into `$PATH` manually (no isolation, no easy
   switch back).

`tvm` (working name: "Turmeric Version Manager") fills the gap. It is a
shell-installable, per-user tool that downloads, caches, and switches
between Turmeric releases the same way `nvm` does for Node:

```sh
tvm install 0.19.1            # download + cache the v0.19.1 prebuilt binary
tvm use 0.19.1                # symlink-resolve `tur` to that version for this shell
tvm use system                # fall back to whatever's on PATH
tvm alias default 0.19.1      # persist a default for new shells
tvm install --build 0.20.0    # no prebuilt? build from source against the tag
tvm install nightly           # latest main-branch artifact
tvm ls                        # list installed
tvm ls-remote                 # list available
tvm current                   # print active version
tvm uninstall 0.18.3
tvm which 0.19.1              # absolute path to that version's `tur`
tvm run 0.19.1 build .        # one-shot: invoke a specific version
tvm exec 0.19.1 -- sh -c '...'
```

The contract is intentionally close to `nvm`: a developer who knows
`nvm install --lts` can guess `tvm install --latest` correctly the
first time.

---

## Why now

- **Spice migrations.** `docs/upcoming/spices-v0.18-typing-migration-plan.md`
  is one of several plans that hinge on "this spice supports
  Turmeric &ge; X." A consumer who can `tvm install` the matching
  compiler in one command is much more likely to file a clean bug
  report than one who must bootstrap from source.
- **CI matrix.** GitHub Actions already build per-platform binaries
  (see `feedback_check_workflow_artifacts.md`). `tvm` reuses those
  artifacts directly, so the publishing side is mostly already done.
- **Bisecting.** Once `tur-signal` ships, regressions across releases
  will be the dominant bug class. `tvm install <tag>` + `tur run` on a
  repro is the fastest possible bisect loop.
- **Multi-project.** A user with one repo on v0.18 and another on
  v0.20 currently cannot have both work in the same shell session.

---

## Non-goals

- **Replacing CMake.** `tvm` does not build from source by default. The
  `--build` flag exists for tags older than the prebuild matrix or for
  platforms with no published artifact; otherwise downloads win.
- **Managing spices.** `tur fetch` / `tur add` already handle the spice
  side. `tvm` manages **compilers**, not libraries.
- **System-wide install.** `tvm` is per-user (`$HOME/.tvm`). The
  existing `just system-install` recipe handles system-wide.
- **Windows-native.** First targets are macOS (arm64 + x86_64) and
  Linux (x86_64 + arm64). Windows support tracks the upstream Release
  workflow; if it ships a Windows artifact, `tvm` consumes it.

---

## Architecture

### Directory layout (`$TVM_DIR`, default `$HOME/.tvm`)

```
~/.tvm/
  tvm.sh                     # shell entry point (sourced by ~/.zshrc etc.)
  versions/
    0.18.3/
      bin/tur                # the actual extracted binary
      bin/turi
      lib/                   # any shared bits the release ships
      share/turmeric/...     # stdlib bundled with that release
    0.19.1/
      ...
    nightly-2026-06-05/
      ...
  aliases/
    default                  # plain file: "0.19.1"
    lts                      # symlink or text -> e.g. "0.19.1"
  cache/
    downloads/               # raw tarballs, content-addressed
  current                    # symlink -> versions/<active>/bin
```

The shell entry point prepends `$TVM_DIR/current` to `$PATH`. `tvm use
<v>` re-points the `current` symlink (cheap, atomic). New shells pick
up whatever `aliases/default` resolves to.

### Per-shell vs. persistent

- `tvm use <v>` -- changes `current` for **this shell**, by writing the
  symlink under a per-shell scratch dir (`$TVM_DIR/shells/$PPID/current`)
  that `tvm.sh` puts ahead of the global `current` in `PATH`. Same model
  as `nvm`'s per-shell isolation.
- `tvm alias default <v>` -- changes the global `current` so new shells
  start there.
- `.tur-version` -- a file at the project root, one line, version tag.
  When `tvm.sh` cd's into a directory containing it, it auto-`use`s
  that version (`nvm`'s `.nvmrc` equivalent). Opt-in via
  `tvm auto on`.

### Source of truth for releases

`tvm ls-remote` queries the GitHub Releases API for
`turmeric-lang/turmeric`, parses asset filenames, and presents the
matrix:

```
0.19.1   darwin-arm64  darwin-x86_64  linux-x86_64  linux-arm64
0.19.0   darwin-arm64  darwin-x86_64  linux-x86_64
nightly  darwin-arm64  linux-x86_64
```

A version is installable on the current host iff an asset matches
`<v>-<os>-<arch>.tar.gz`. Otherwise `tvm install` either errors with a
clear message or, if `--build` is set, falls through to the
`git clone && cmake` path.

### Asset format

A release artifact is a single tarball:

```
turmeric-0.19.1-darwin-arm64.tar.gz
  tur                     # the compiler binary
  turi                    # the interpreter binary (if built)
  share/turmeric/         # stdlib snapshot for that release
  VERSION                 # plain text
```

`tvm install` extracts to `versions/<v>/`, verifies the SHA-256 against
a checksums file published alongside the release, and atomically
renames into place.

---

## Phases

### Phase 1 -- MVP: install + use + ls

- `tvm.sh` shell entry point (zsh + bash; fish via a tiny wrapper).
- `tvm install <v>` -- download from GitHub Releases, verify SHA,
  extract into `versions/<v>/`.
- `tvm use <v>` -- per-shell symlink swap.
- `tvm ls` -- list installed.
- `tvm ls-remote` -- list available (cached for 60s).
- `tvm current` -- print active version.
- `tvm uninstall <v>` -- remove `versions/<v>/`.
- Detect when nothing is selected and exit with a useful message.

Acceptance: a fresh-installed `tvm` can `install 0.19.1`, `use 0.19.1`,
and `tur --version` reports `0.19.1`, on macOS (arm64) and Linux
(x86_64).

### Phase 2 -- ergonomics

- `tvm alias default <v>` and `tvm alias lts <v>`.
- `.tur-version` file detection with `tvm auto on/off`.
- `tvm which <v>` -- absolute path to the binary.
- `tvm run <v> <args...>` -- one-shot invocation without changing the
  active version. Useful for CI and bisect scripts.
- `tvm exec <v> -- <cmd...>` -- run an arbitrary command with that
  version on `PATH`.

### Phase 3 -- build-from-source fallback

- `tvm install --build <ref>` -- when no prebuilt asset exists, clone
  `turmeric-lang/turmeric` at the given tag/branch/SHA, run the
  documented CMake bootstrap (Release config), and stage the
  resulting `build-release/tur` into `versions/<v>-src-<sha>/`.
- Cache the source tree in `cache/sources/` so repeated builds at the
  same SHA do not re-clone.
- Be explicit in `tvm ls` that a version is source-built (`*` suffix
  or similar).

### Phase 4 -- nightlies and channels

- A scheduled CI job in `turmeric-lang/turmeric` uploads `nightly`
  artifacts to a known release tag (e.g. `nightly-latest`).
- `tvm install nightly` downloads the asset whose timestamp is newest
  and stores it as `nightly-YYYY-MM-DD`.
- `tvm install nightly@2026-06-01` pins a specific dated nightly.

### Phase 5 -- ecosystem integration

- `tvm install --activate` -- combines install + `use` + `alias
  default`; mirrors `rustup install --default`.
- Shell completion (`tvm completion zsh > _tvm`).
- A `tvm doctor` command that checks: `tvm.sh` sourced, `PATH` order,
  symlink integrity, network reachability of the releases API.
- Publishing-side companion: a Justfile recipe that, after `bump-*`,
  writes a SHA-256 checksums file and uploads it as a release asset.

---

## Implementation language

Two real options:

1. **POSIX shell** (`nvm`'s choice). Maximum portability, zero
   bootstrap dependency, but the `nvm` codebase is famous for being
   gnarly to maintain.
2. **Self-hosted in Turmeric.** Once `tur` is on a user's machine they
   already have everything needed to run `tvm`. The bootstrap problem
   is real, though: you cannot run `tvm install` before you have a
   `tur` binary.

Recommendation: **POSIX shell for the entry point + the active-version
switching logic** (the parts that must work before any `tur` exists),
plus a tiny Turmeric helper invoked by the shell once a compiler is
available (for tarball verification, `ls-remote` parsing, etc.).
Equivalent to how `rustup`'s installer shell script bootstraps a Rust
binary that then takes over.

A useful test-driven motivator: the moment `tvm` exists, the project's
own CI matrix can use `tvm install <ref>` instead of bespoke setup
steps. That gives us a forcing function to keep the tool working.

---

## Open questions

1. **Release-asset format.** Do we ship one tarball per
   `(os, arch)`, or a fat universal tarball plus per-arch verifier?
   Current Releases workflow already produces per-`(os, arch)`
   artifacts; reuse that.
2. **stdlib bundling.** Should each `versions/<v>/share/turmeric/`
   carry a full stdlib snapshot, or should the binary embed its
   resource path the way Rust does? Embedding is cleaner; tarball
   carrying is more debuggable.
3. **Spice cache cross-version.** A `tur.lock` produced under v0.19
   may not work under v0.18. Out of scope for `tvm`, but worth a note
   in the spice-consumer guide once `tvm` ships.
4. **Self-update.** `tvm self-update` is convenient but introduces an
   "uninstall surface." Prefer documenting `git pull` against
   `$TVM_DIR` for the MVP.

---

## Sibling work

- `system-install` / `system-uninstall` Just recipes (added 2026-06-06)
  cover the system-wide path. `tvm` is the per-user complement.
- `tur fetch` and the spice manifest already cover the library side;
  `tvm` is intentionally scoped to compilers only.
- The Release workflow (see `feedback_check_workflow_artifacts.md`)
  is the upstream the installer consumes. Phase 5's checksums recipe
  is the only publishing-side change `tvm` requires.
