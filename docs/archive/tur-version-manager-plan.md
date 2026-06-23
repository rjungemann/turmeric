# Turmeric Version Manager (`tvm`) Plan

> **Status:** In progress -- Phases 1-3 landed in `tvm/`, plus the Phase 5
> ergonomics (`--activate`, completion, `doctor`). Phase 4 (nightly channel
> CI plumbing) remains. See `tvm/README.md` and `tvm/tests/run.sh`.
> **Last Updated:** 2026-06-23
> **Type:** tooling / distribution
> **Modeled after:** `nvm` (Node Version Manager), with cues from `rustup` and `pyenv`.

---

## Overview

Turmeric ships as a single `tur` binary, but the project moves fast
(current release line is v0.23.x as of 2026-06-22, with the typing
migration that broke spice builds across the v0.17 -> v0.19 window
still well within bisect range): a working tree pinned to one minor
release will fail to build a spice that targets a newer one, and
conversely, a user debugging a regression often wants to bisect
between two tags without re-bootstrapping CMake by hand. Today the
only options are:

1. Build from source per checkout (slow; requires CMake + a recent
   toolchain).
2. `brew install turmeric` (always one version, stale within a week).
3. Drop a tagged tarball into `$PATH` manually (no isolation, no easy
   switch back).

`tvm` (working name: "Turmeric Version Manager") fills the gap. It is a
shell-installable, per-user tool that downloads, caches, and switches
between Turmeric releases the same way `nvm` does for Node:

```sh
tvm install 0.23.1            # download + cache the v0.23.1 prebuilt binary
tvm use 0.23.1                # symlink-resolve `tur` to that version for this shell
tvm use system                # fall back to whatever's on PATH
tvm alias default 0.23.1      # persist a default for new shells
tvm install --build 0.17.0    # no prebuilt? build from source against the tag
tvm install nightly           # latest main-branch artifact
tvm ls                        # list installed
tvm ls-remote                 # list available
tvm current                   # print active version
tvm uninstall 0.22.0
tvm which 0.23.1              # absolute path to that version's `tur`
tvm run 0.23.1 build .        # one-shot: invoke a specific version
tvm exec 0.23.1 -- sh -c '...'
```

The contract is intentionally close to `nvm`: a developer who knows
`nvm install --lts` can guess `tvm install --latest` correctly the
first time.

---

## Why now

- **Spice migrations.** The v0.18 typing migration (now archived as
  `docs/archive/history/spices-v0.18-typing-migration-plan.md`) is one
  of several past releases that broke "this spice supports Turmeric
  &ge; X" assumptions. A consumer who can `tvm install` the matching
  compiler in one command is much more likely to file a clean bug
  report than one who must bootstrap from source.
- **CI matrix.** The Release workflow (`.github/workflows/release.yml`)
  already builds per-platform binaries -- currently `linux-x86_64`,
  `linux-aarch64`, and `macos-arm64`. `tvm` reuses those artifacts
  directly, so the publishing side is mostly already done.
- **Bisecting.** `tur-signal` (now shipped; rebuild plan archived at
  `docs/archive/history/tur-signal-rebuild-plan.md`) makes
  regressions across releases the dominant bug class. `tvm install
  <tag>` + `tur run` on a repro is the fastest possible bisect loop.
- **Multi-project.** A user with one repo on an older minor and another
  on the latest minor currently cannot have both work in the same shell
  session.

---

## Non-goals

- **Replacing CMake.** `tvm` does not build from source by default. The
  `--build` flag exists for tags older than the prebuild matrix or for
  platforms with no published artifact; otherwise downloads win.
- **Managing spices.** `tur fetch` / `tur add` already handle the spice
  side. `tvm` manages **compilers**, not libraries.
- **System-wide install.** `tvm` is per-user (`$HOME/.tvm`).
  System-wide install remains a manual `cp` / packager job; if a
  `just system-install` recipe lands later, `tvm` does not replace it.
- **Windows-native.** First targets match what the Release workflow
  publishes today: `macos-arm64`, `linux-x86_64`, `linux-aarch64`.
  macOS x86_64 and Windows are not built upstream yet; if/when the
  Release workflow adds either target, `tvm` consumes it.

---

## Architecture

### Directory layout (`$TVM_DIR`, default `$HOME/.tvm`)

```
~/.tvm/
  tvm.sh                     # shell entry point (sourced by ~/.zshrc etc.)
  versions/
    0.22.0/
      bin/tur                # the extracted compiler binary
      lib/libturi.a          # static interpreter lib (shipped in the tarball)
      include/turi/*.h       # headers needed to link against libturi
      stdlib/                # stdlib snapshot bundled with that release
    0.23.1/
      ...
    nightly-2026-06-20/
      ...
  aliases/
    default                  # plain file: "0.23.1"
    lts                      # symlink or text -> e.g. "0.23.1"
  cache/
    downloads/               # raw tarballs, content-addressed
  current                    # symlink -> versions/<active>/bin
```

The layout above reflects what the Release workflow actually packages
today (see "Asset format" below): a `tur` binary, `libturi.a`,
`include/turi/*.h`, and a `stdlib/` tree -- no separate `turi` binary,
no `share/turmeric/` directory. `tvm` extracts the tarball as-is and
points `current` at `versions/<v>/bin/`.

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
`rjungemann/turmeric`, parses asset filenames, and presents the
matrix. Asset filenames follow the convention from
`.github/workflows/release.yml`:

```
turmeric-<tag>-<target>.tar.gz
```

where `<tag>` is the git tag (`v0.23.1`) and `<target>` is one of
`linux-x86_64`, `linux-aarch64`, `macos-arm64` today:

```
0.23.1   macos-arm64  linux-x86_64  linux-aarch64
0.23.0   macos-arm64  linux-x86_64  linux-aarch64
nightly  macos-arm64  linux-x86_64
```

A version is installable on the current host iff a matching asset
exists. Otherwise `tvm install` either errors with a clear message or,
if `--build` is set, falls through to the `git clone && cmake` path.

### Asset format

A release artifact is a single tarball produced by the `Package` step
of `.github/workflows/release.yml`:

```
turmeric-v0.23.1-macos-arm64.tar.gz
  tur                     # the compiler binary
  libturi.a               # static interpreter library
  include/turi/eval.h
  include/turi/env.h
  include/turi/value.h
  include/turi/fiber.h
  stdlib/                 # stdlib snapshot for that release
```

There is no separate `turi` binary, no top-level `VERSION` file, and
no `share/turmeric/` -- those are spec from earlier drafts of this
plan; the actual workflow ships the layout above. `tvm install`
extracts to `versions/<v>/`, verifies the SHA-256 against the
checksums file generated by the `Generate checksums` step, and
atomically renames into place.

---

## Phases

### Phase 1 -- MVP: install + use + ls  *(landed in `tvm/tvm.sh`)*

- `tvm.sh` shell entry point (zsh + bash; fish via a tiny wrapper).
- `tvm install <v>` -- download from GitHub Releases, verify SHA,
  extract into `versions/<v>/`.
- `tvm use <v>` -- per-shell symlink swap.
- `tvm ls` -- list installed.
- `tvm ls-remote` -- list available (cached for 60s).
- `tvm current` -- print active version.
- `tvm uninstall <v>` -- remove `versions/<v>/`.
- Detect when nothing is selected and exit with a useful message.

Acceptance: a fresh-installed `tvm` can `install 0.23.1`, `use 0.23.1`,
and `tur --version` reports `0.23.1`, on macOS (arm64) and Linux
(x86_64).

### Phase 2 -- ergonomics  *(landed: alias, .tur-version auto, which, run, exec)*

- `tvm alias default <v>` and `tvm alias lts <v>`.
- `.tur-version` file detection with `tvm auto on/off`.
- `tvm which <v>` -- absolute path to the binary.
- `tvm run <v> <args...>` -- one-shot invocation without changing the
  active version. Useful for CI and bisect scripts.
- `tvm exec <v> -- <cmd...>` -- run an arbitrary command with that
  version on `PATH`.

### Phase 3 -- build-from-source fallback  *(landed: `tvm install --build`)*

- `tvm install --build <ref>` -- when no prebuilt asset exists, clone
  `turmeric-lang/turmeric` at the given tag/branch/SHA, run the
  documented CMake bootstrap (Release config), and stage the
  resulting `build-release/tur` into `versions/<v>-src-<sha>/`.
- Cache the source tree in `cache/sources/` so repeated builds at the
  same SHA do not re-clone.
- Be explicit in `tvm ls` that a version is source-built (`*` suffix
  or similar).

### Phase 4 -- nightlies and channels  *(pending: needs net-new CI workflow)*

- A scheduled CI job in `rjungemann/turmeric` uploads `nightly`
  artifacts to a known release tag (e.g. `nightly-latest`). The
  existing `release.yml` only fires on tag push, so this is net-new
  workflow plumbing, not just a reuse of what ships today.
- `tvm install nightly` downloads the asset whose timestamp is newest
  and stores it as `nightly-YYYY-MM-DD`.
- `tvm install nightly@2026-06-20` pins a specific dated nightly.

### Phase 5 -- ecosystem integration  *(partial: `--activate`, completion, `doctor` landed)*

- `tvm install --activate` -- combines install + `use` + `alias
  default`; mirrors `rustup install --default`.
- Shell completion (`tvm completion zsh > _tvm`).
- A `tvm doctor` command that checks: `tvm.sh` sourced, `PATH` order,
  symlink integrity, network reachability of the releases API.
- Publishing-side companion: the existing `bump-patch`/`bump-minor`/
  `bump-major` Justfile recipes already drive a tag push that triggers
  `release.yml`, and that workflow already emits a checksums file in
  the `Generate checksums` step. The remaining publishing-side gap is
  documenting (and surfacing in release notes) the canonical checksums
  filename so `tvm install` can verify without a heuristic.

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
2. **stdlib bundling.** Each release tarball already carries a
   `stdlib/` snapshot at the top level (see "Asset format"). Open
   question is whether `tvm use <v>` should also export a
   `TUR_STDLIB_DIR` (or equivalent) so that `tur` picks up that
   version's stdlib unambiguously, or whether the binary's
   resource-path lookup is already sufficient.
3. **Spice cache cross-version.** A `tur.lock` produced under a newer
   minor may not work under an older one. Out of scope for `tvm`, but
   worth a note in the spice-consumer guide once `tvm` ships.
4. **Self-update.** `tvm self-update` is convenient but introduces an
   "uninstall surface." Prefer documenting `git pull` against
   `$TVM_DIR` for the MVP.

---

## Sibling work

- There is no `system-install` / `system-uninstall` Just recipe today
  (an earlier draft of this plan referenced one as if it had landed; it
  has not). System-wide install is still manual `cp` or packager-driven;
  `tvm` is the per-user complement and does not block that work.
- `tur fetch` and the spice manifest already cover the library side;
  `tvm` is intentionally scoped to compilers only.
- The Release workflow (`.github/workflows/release.yml`) is the
  upstream the installer consumes. Since checksums are already
  emitted by the workflow, the remaining publishing-side change is
  surfacing the checksums filename in release metadata so `tvm`
  doesn't have to guess.
