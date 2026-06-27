# Turmeric mode for Lite XL

A single-file Lite XL plugin that turns the editor into a Processing-style
"open file, press Run, see output" experience for Turmeric. Phase 1 of
[`docs/upcoming/turmeric-lite-xl-desktop-plan.md`](../../docs/upcoming/turmeric-lite-xl-desktop-plan.md).

## What you need

- Lite XL 2.1.x (mod-version 3) from <https://lite-xl.com> -- native
  `.dmg` for Apple Silicon / Intel / Universal, `.exe` / portable `.zip`
  for Windows, AppImage / tarball for Linux. ~3-5 MB.
- `tur` on PATH **or** the plugin pointed at the `tur` binary via
  `config.plugins.turmeric.tur` (see below). On macOS this matters: apps
  launched from Finder do not inherit your shell's PATH.

## Quick start (in-tree)

From the Turmeric repo:

```sh
tur run lite-xl examples/cellular-automata.tur
```

The `lite-xl` task symlinks `tools/lite-xl/turmeric.lua` into
`~/.config/lite-xl/plugins/` and writes a `~/.config/lite-xl/init.lua`
that pins the compiler at `./build/tur`. Idempotent; safe to re-run.

## Install (no Justfile)

1. Drop `turmeric.lua` into the user plugins dir:
   - **macOS / Linux:** `~/.config/lite-xl/plugins/turmeric.lua`
   - **Windows:** `%USERPROFILE%\.config\lite-xl\plugins\turmeric.lua`
2. If `tur` is not on the PATH Lite XL inherits, add this to
   `~/.config/lite-xl/init.lua`:
   ```lua
   local config = require "core.config"
   config.plugins.turmeric = config.plugins.turmeric or {}
   config.plugins.turmeric.tur = "/abs/path/to/tur"
   ```
3. Open any `.tur` or `.tur.sweet` file.

## Bindings

| Key (mac) | Key (others) | Command |
| --- | --- | --- |
| cmd+r | ctrl+r | `turmeric:run-file` -- saves the buffer, runs `tur run <file>` |
| cmd+shift+r | ctrl+shift+r | `turmeric:check-file` -- runs `tur check <file>` |
| f1 / cmd+i | f1 | `turmeric:doc-at-cursor` -- prints the docstring for the symbol at the cursor into the log pane |

Surfaced through the command palette (cmd+shift+p / ctrl+shift+p) without
a default keybinding:

- `turmeric:new-project` -- prompt for a name, scaffold a binary project
  via `tur init <name>`, open the resulting `build.tur` + `src/main.tur`
- `turmeric:new-project-sweet` -- same, with `tur init --sweet`
- `turmeric:new-library` -- same, with `tur init --lib`

New projects are created under the current Lite XL project directory.
Override the command surface via `config.plugins.turmeric.init_cmd` and
`init_subcommand` if you want to point at a wrapper script.

Output streams into Lite XL's log pane (View > Toggle Log View if it is
not visible). Lines that look like `path:line:col:` come through
`core.error` so the log pane highlights them.

## Themes

Two color themes ported 1:1 from the Try Turmeric palette
(`web/main.js`):

- `turmeric-dark` -- the "Spice Market" dark theme. Set as the default
  when launching via `tur run lite-xl`.
- `turmeric-light` -- the light variant.

Switch via Lite XL's command palette: `Core: Change Color Theme`. The
sync between the Monaco palette and the Lua port is gated by
`python3 tests/lite-xl/check-palette-sync.py`.

## Plugin-only install (`install.sh`)

If you already have stock Lite XL and just want the Turmeric plugin:

```sh
bash tools/lite-xl/install.sh             # symlink (tracks repo updates)
bash tools/lite-xl/install.sh --copy      # standalone copy
LITE_XL_CONFIG=/custom/path bash tools/lite-xl/install.sh
```

## Turmeric Studio distribution (Phase 5)

Scripts to produce a rebranded `Turmeric Studio.app` (macOS DMG) and
`TurmericStudio.AppImage` (Linux) live in
[`dist/`](dist/README.md). The bundle does not ship the `tur`
compiler; it expects `tur` on PATH. Signing + notarization are opt-in
via `TURMERIC_SIGN_IDENTITY` / `TURMERIC_NOTARY_PROFILE`. Windows
distribution is tracked in a separate plan.

## Branded macOS bundle (temporary)

On the first `tur run lite-xl` macOS launch, `launch.sh` builds a sibling
`tools/lite-xl/Turmeric.app/` that wears the Turmeric logo from
`web/public/logo-icon.svg`:

- `Resources/turmeric.icns` is rasterized at 16/32/64/128/256/512/1024 px
  (ImageMagick + `iconutil`) and centered on a square transparent canvas.
- The `lite-xl` binary is **copied** from `/Applications/Lite XL.app`
  (~3 MB) rather than symlinked -- macOS resolves binary symlinks and
  would otherwise report the upstream bundle to LaunchServices, defeating
  the icon swap.
- Everything else under `Resources/` is symlinked into the upstream
  bundle, so plugin/data updates from a Lite XL upgrade are picked up
  automatically.
- The bundle is gitignored as a build artifact.

Rebuild manually with `bash tools/lite-xl/build-app-icon.sh`. Skip the
auto-build with `TUR_SKIP_BRAND=1 tur run lite-xl`. Requires
`brew install imagemagick`.

The branded bundle is a temporary "look, our icon in the dock!"
affordance ahead of the Phase 5 distribution, which will publish a real
`Turmeric Studio.app` from upstream Lite XL sources with proper signing
and notarization.

## Configuration

Override any of these in `~/.config/lite-xl/init.lua`:

```lua
local config = require "core.config"
config.plugins.turmeric = config.plugins.turmeric or {}
config.plugins.turmeric.tur = "/usr/local/bin/tur"   -- compiler binary
```

The Run command shells out to `tur --interpret <file>` (the tree-walking
interpreter); the prior null `ReaderMacroRegistry` segfault has been
fixed and the archived report lives at
[`docs/archive/tur-interpret-null-reader-macro-registry.md`](../../docs/archive/tur-interpret-null-reader-macro-registry.md).

## Autocomplete and docstrings (Phase 3)

The plugin spawns one long-lived `tur lsp-lite` helper on first symbol
lookup and talks to it with newline-delimited JSON over stdin/stdout.
The helper indexes `stdlib/docstrings.tur` on startup plus any `;;;`
blocks in the active buffer (refreshed on save).

- F1 (or cmd+i on macOS) -- print the docstring for the symbol at the
  cursor into the log pane.
- The built-in `autocomplete` plugin gets a `turmeric` source that
  surfaces stdlib + buffer-local names as you type. Enable Lite XL's
  autocomplete plugin if it is not on by default.

Override the helper subcommand via
`config.plugins.turmeric.lsp_cmd = "lsp-lite"` (the default).

## REPL (Phase 4)

Spawns a long-lived `tur repl` subprocess and bridges it through the
LogView (output) and CommandView (input). The implementation reuses
the two built-in views rather than rolling a bespoke `ReplView` --
honest about what's there.

| Key (mac) | Key (others) | Command |
| --- | --- | --- |
| cmd+e | ctrl+e | `turmeric:repl-eval` -- prompt for an expression, send it to the running REPL |
| cmd+shift+l | ctrl+shift+l | `turmeric:repl-reload-buffer` -- save the active buffer and pipe its full contents into the REPL |

Also surfaced through the command palette:

- `turmeric:start-repl` -- spawn the REPL subprocess if not already running
- `turmeric:stop-repl`  -- send `:quit` and terminate

Output appears in the log pane prefixed with `repl> `; the input that
triggered each block is logged with `repl< ` so the back-and-forth is
easy to follow. Configure the subcommand via
`config.plugins.turmeric.repl_subcommand` (default `"repl"`).

## Why Lite XL (not SciTE)

The earlier SciTE direction is [superseded](../../docs/upcoming/turmeric-scite-desktop-plan.md);
no free open-source SciTE Cocoa shell exists, which pushed three-OS v1
to 5-7 weeks. Lite XL ships native binaries for all three OSes from
one upstream CI matrix and exposes everything we need from a single
Lua file.
