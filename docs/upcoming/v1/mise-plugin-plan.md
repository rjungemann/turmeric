# `mise` integration via a backend-passthrough plugin

## Goal

Let users on [`mise`](https://mise.jdx.dev) manage Turmeric compiler versions
with the same ergonomics as any other tool (`mise install turmeric@0.23.1`,
`.mise.toml`, `.tool-versions`, shims, project-local pins), without forking
the install/build logic that already lives in `tvm`.

The plugin is **the lightest viable surface**: ~4 short shell scripts that
shell out to `tvm`. `mise` owns shim generation, config parsing, and
PATH/runtime activation; `tvm` remains the single source of truth for
*which* releases exist, *how* they download, *how* they build from source,
and *where* on disk they live.

## Why a passthrough, not a from-scratch port

- `tvm` already handles: GitHub release listing (`ls-remote`), per-target
  tarball selection (`__tvm_target`), SHA-256 verification, atomic extract
  into `$TVM_DIR/versions/<v>/`, source-build fallback (`--build`), and
  `TUR_STDLIB_DIR` wiring.
- Reimplementing any of that inside a mise plugin would mean two copies of
  release-asset logic, drifting whenever the release workflow changes.
- A passthrough keeps `tvm` as the **only** thing that talks to GitHub
  releases. The mise plugin is just an adapter.

## How `mise` plugins work (the minimum we need)

`mise` natively supports asdf-style plugins. A plugin is a git repo with a
`bin/` directory containing standardized scripts. `mise` calls them with
predictable env vars (`ASDF_INSTALL_VERSION`, `ASDF_INSTALL_PATH`, ...) and
expects each to read stdin / write stdout in a documented format.

Required scripts for a minimal install-only plugin:

| Script | Contract |
|---|---|
| `bin/list-all` | Print all installable versions, newline-separated, oldest→newest. |
| `bin/install` | Install `$ASDF_INSTALL_VERSION` into `$ASDF_INSTALL_PATH`. Exit 0 on success. |
| `bin/list-bin-paths` | Print space- or newline-separated subpaths (relative to `$ASDF_INSTALL_PATH`) that should land on `PATH` -- for us, just `bin`. |
| `bin/latest-stable` | Print the single newest non-pre-release tag. |

Optional but cheap wins: `bin/exec-env` (export `TUR_STDLIB_DIR`),
`bin/help.overview`, `bin/help.deps`.

## Repo layout

A separate repo, **`asdf-turmeric`** (name follows the asdf-plugin convention
that `mise` recognizes by default):

```
asdf-turmeric/
  bin/
    list-all
    install
    list-bin-paths
    latest-stable
    exec-env          # optional: exports TUR_STDLIB_DIR
  README.md
  LICENSE
  .github/workflows/ci.yml   # smoke-install a known tag on linux+macos
```

Hosting under `rjungemann/asdf-turmeric` keeps it out of the main
`turmeric` repo so plugin iterations don't churn the compiler tree, and so
the mise registry can point at a stable URL.

## Script designs

Each script is a thin shim. They all assume `tvm.sh` has been sourced; the
plugin sources it from a known location (env override, then default).

### `bin/list-all`

```sh
#!/usr/bin/env bash
set -euo pipefail
. "${TVM_SH:-$HOME/.tvm/tvm.sh}"
# tvm prints "v0.23.1" etc.; mise wants bare versions, oldest first.
__tvm_cmd_ls_remote | sed 's/^v//' | awk 'NF' | tac 2>/dev/null || \
  __tvm_cmd_ls_remote | sed 's/^v//' | awk 'NF' | tail -r
```

Edge case: `tac` isn't on BSD/macOS by default; fall back to `tail -r`.

### `bin/install`

```sh
#!/usr/bin/env bash
set -euo pipefail
: "${ASDF_INSTALL_TYPE:?}"   # "version" or "ref"
: "${ASDF_INSTALL_VERSION:?}"
: "${ASDF_INSTALL_PATH:?}"
. "${TVM_SH:-$HOME/.tvm/tvm.sh}"

v="$ASDF_INSTALL_VERSION"

# Install via tvm into its own store, then symlink/copy into the path mise
# expects. We do NOT redirect tvm's install root; tvm's layout is its own
# contract and we want shared cache benefits when users run tvm directly.
if ! __tvm_installed "$v"; then
  __tvm_cmd_install "$v" || __tvm_cmd_install --build "$v"
fi

mkdir -p "$ASDF_INSTALL_PATH"
# Mirror layout: <install_path>/bin/tur, <install_path>/stdlib, etc.
ln -sfn "$TVM_DIR/versions/$v/bin"     "$ASDF_INSTALL_PATH/bin"
ln -sfn "$TVM_DIR/versions/$v/stdlib"  "$ASDF_INSTALL_PATH/stdlib"
[ -d "$TVM_DIR/versions/$v/include" ] && \
  ln -sfn "$TVM_DIR/versions/$v/include" "$ASDF_INSTALL_PATH/include"
[ -d "$TVM_DIR/versions/$v/lib" ] && \
  ln -sfn "$TVM_DIR/versions/$v/lib" "$ASDF_INSTALL_PATH/lib"
```

Symlinking (rather than copying) means `tvm uninstall <v>` and
`mise uninstall turmeric@<v>` both work, and the disk cost is paid once.
We accept the small breakage if a user `tvm uninstall`s a version mise
still thinks it owns -- documented in the README.

Source-build fallback: `__tvm_cmd_install --build` is invoked when the
prebuilt asset doesn't exist for the host target (e.g. niche arch, or a
pre-prebuilt-era tag). `tvm` already has the workflow.

### `bin/list-bin-paths`

```sh
#!/usr/bin/env bash
echo "bin"
```

### `bin/latest-stable`

```sh
#!/usr/bin/env bash
set -euo pipefail
. "${TVM_SH:-$HOME/.tvm/tvm.sh}"
__tvm_cmd_ls_remote | sed 's/^v//' | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' | tail -n1
```

Filters out any pre-release tags (`-rc.1`, `-beta`) by requiring
strict `MAJOR.MINOR.PATCH`.

### `bin/exec-env` (optional)

```sh
#!/usr/bin/env bash
export TUR_STDLIB_DIR="$ASDF_INSTALL_PATH/stdlib"
```

So `tur` invoked through a mise shim still finds its bundled stdlib even
if the user has no `TUR_STDLIB_DIR` set.

## What `tvm` needs to expose

These are the only `tvm` surfaces the plugin depends on. None require code
changes today, but they become **stable contracts** once the plugin ships:

1. Sourcing `tvm.sh` defines `__tvm_cmd_install`, `__tvm_cmd_ls_remote`,
   `__tvm_installed`, and `TVM_DIR`.
2. `__tvm_cmd_install <v>` installs from prebuilt; `__tvm_cmd_install --build <v>`
   builds from source. Both leave the result at
   `$TVM_DIR/versions/<v>/{bin/tur,stdlib,...}`.
3. `__tvm_cmd_ls_remote` prints one `v<MAJOR>.<MINOR>.<PATCH>` tag per line.

If we'd rather not depend on the `__tvm_*` internal names, **mini task for
`tvm`**: add a small public-facing CLI surface (`tvm list-remote --plain`,
`tvm install --quiet --no-activate`) and switch the plugin to those. Worth
doing before the plugin ships -- internal underscored names are not a stable
API. See "Open questions" below.

## Plan of work

1. **(tvm)** Stabilize the surface the plugin will call.
   - Add `tvm list-remote --plain` (bare versions, one per line). _Easy._
   - Confirm `tvm install <v>` is idempotent and exits 0 if already installed
     (it already is; add a test).
   - Document these as supported in `tvm/README.md`.
2. **(asdf-turmeric)** Spin up the new repo with the 4-5 scripts above.
3. **CI for the plugin**: GitHub Actions matrix (`ubuntu-latest`,
   `macos-latest`) that installs `mise`, installs `tvm`, installs the
   plugin from the checkout (`mise plugin install turmeric .`), then
   `mise install turmeric@<known-good-tag>` and `mise exec -- tur --version`.
4. **Docs**: a short "Using Turmeric with mise" section in
   `docs/guides/` linking to the plugin repo; one paragraph in
   `tvm/README.md` ("If you already use mise, see
   `asdf-turmeric`").
5. **Optional, later**: submit the plugin to the
   [mise registry](https://mise.jdx.dev/registry.html) so users can run
   `mise install turmeric@latest` without the explicit `plugin install`
   step.

## Out of scope

- A native Rust mise plugin (the `mise plugin new` Rust target). The shell
  passthrough is enough, and Rust would re-introduce the duplication we are
  explicitly avoiding.
- Replacing `tvm`. `tvm` stays the recommended path for users not on mise;
  the plugin is purely additive.
- Auto-publishing the plugin from the main Turmeric release workflow.
  Plugin churn is decoupled from compiler releases.

## Open questions

- **Public `tvm` CLI vs. sourced internals.** Cleaner long-term to call
  `tvm list-remote --plain` than to source `tvm.sh` and reach for
  `__tvm_cmd_ls_remote`. Recommend doing the CLI addition in `tvm` first
  (one PR, ~30 lines) so the plugin never depends on underscored names.
- **Shared store vs. mise-owned store.** Symlinking from
  `$TVM_DIR/versions/<v>/` into `$ASDF_INSTALL_PATH` shares disk and cache
  but couples uninstall semantics. Alternative: install fully into
  `$ASDF_INSTALL_PATH` and accept the duplication. Recommend symlinks +
  a documented note; users who care can `unset TVM_DIR` and let mise own
  it entirely.
- **`.tur-version` vs. `.tool-versions`.** Two different files mean two
  sources of truth in the same repo. Document that mise users should pick
  one (`.mise.toml` recommended); do not try to make `tvm` read
  `.tool-versions` or vice versa.

## Effort estimate

- `tvm` CLI stabilization: ~0.5 day (one CLI flag + tests + docs).
- Plugin scripts + README: ~0.5 day.
- Plugin CI: ~0.5 day.

Total: ~1.5 days of focused work, off the v1 critical path. The plugin
repo can be iterated on independently after the initial cut.
