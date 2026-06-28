# Turmeric mode for Trowel

A single-file Trowel plugin that turns the editor into a Processing-style
"open file, press Run, see output" experience for Turmeric. Phase 1 of
[`docs/upcoming/trowel-renaming-plan.md`](../../docs/upcoming/trowel-renaming-plan.md).

## What you need

- Trowel 2.1.x (mod-version 3) or Lite XL -- native
  `.dmg` for Apple Silicon / Intel / Universal, `.exe` / portable `.zip`
  for Windows, AppImage / tarball for Linux. ~3-5 MB.
- `tur` on PATH **or** the plugin pointed at the `tur` binary via
  `config.plugins.turmeric.tur` (see below). On macOS this matters: apps
  launched from Finder do not inherit your shell's PATH.

## Quick start (in-tree)

From the Turmeric repo:

```sh
just trowel examples/cellular-automata.tur
```

The `trowel` task symlinks `tools/trowel/turmeric.lua` into
`~/.config/trowel/plugins/` and writes a `~/.config/trowel/init.lua`
that pins the compiler at `./build/tur`. Idempotent; safe to re-run.

## Install (no Justfile)

1. Drop `turmeric.lua` into the user plugins dir:
   - **macOS / Linux:** `~/.config/trowel/plugins/turmeric.lua`
   - **Windows:** `%USERPROFILE%\.config\trowel\plugins\turmeric.lua`
2. If `tur` is not on the PATH Trowel inherits, add this to
   `~/.config/trowel/init.lua`:
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

New projects are created under the current Trowel project directory.
Override the command surface via `config.plugins.turmeric.init_cmd` and
`init_subcommand` if you want to point at a wrapper script.

Output streams into Trowel's log pane (View > Toggle Log View if it is
not visible). Lines that look like `path:line:col:` come through
`core.error` so the log pane highlights them.

## Themes

Two color themes ported 1:1 from the Try Turmeric palette
(`web/main.js`):

- `turmeric-dark` -- the "Spice Market" dark theme. Set as the default
  when launching via `just trowel`.
- `turmeric-light` -- the light variant.

Switch via Trowel's command palette: `Core: Change Color Theme`. The
sync between the Monaco palette and the Lua port is gated by
`python3 tests/trowel/check-palette-sync.py`.

## Plugin-only install (`install.sh`)

If you already have stock Trowel/Lite XL and just want the Turmeric plugin:

```sh
bash tools/trowel/install.sh             # symlink (tracks repo updates)
bash tools/trowel/install.sh --copy      # standalone copy
TROWEL_CONFIG=/custom/path bash tools/trowel/install.sh
```

## Trowel distribution (Phase 3)

Scripts to produce a rebranded `Trowel.app` (macOS DMG) and
`Trowel.AppImage` (Linux) live in
[`dist/`](dist/README.md). The bundle does not ship the `tur`
compiler; it expects `tur` on PATH. Windows distribution is tracked in a separate plan.

## Branded macOS bundle (temporary)

On the first `just trowel` macOS launch, `launch.sh` builds a sibling
`tools/trowel/Trowel.app/` that wears the Turmeric logo from
`web/public/logo-icon.svg`:

- `Resources/trowel.icns` is rasterized at 16/32/64/128/256/512/1024 px
  (ImageMagick + `iconutil`) and centered on a square transparent canvas.
- The `trowel` binary is **copied** from `/Applications/Trowel.app` or `/Applications/Lite XL.app`
  (~3 MB) rather than symlinked -- macOS resolves binary symlinks and
  would otherwise report the upstream bundle to LaunchServices, defeating
  the icon swap.
- Everything else under `Resources/` is symlinked into the upstream
  bundle, so plugin/data updates from a Trowel/Lite XL upgrade are picked up
  automatically.
- The bundle is gitignored as a build artifact.

Rebuild manually with `bash tools/trowel/build-app-icon.sh`. Skip the
auto-build with `TUR_SKIP_BRAND=1 just trowel`. Requires
`brew install imagemagick`.

The branded bundle is a temporary "look, our icon in the dock!"
affordance ahead of the packaged distribution, which will publish a real
`Trowel.app` from upstream sources.

## Configuration

Override any of these in `~/.config/trowel/init.lua`:

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
  surfaces stdlib + buffer-local names as you type. Enable Trowel's
  autocomplete plugin if it is not on by default.

Override the helper subcommand via
`config.plugins.turmeric.lsp_cmd = "lsp-lite"` (the default).

## REPL (Phase 4)

`turmeric:start-repl` opens a real `ReplView` pane (a custom `core.view`
subclass) in a horizontal split below the active editor. The pane is
backed by a long-lived `tur repl` subprocess; stdout/stderr stream in
asynchronously and the bottom prompt captures keystrokes directly.

| Key (mac) | Key (others) | Command |
| --- | --- | --- |
| cmd+shift+l | ctrl+shift+l | `turmeric:repl-reload-buffer` -- save the active buffer and pipe its full contents into the REPL pane |

Keys active **only when the ReplView has focus** (so they never shadow
editor keys):

| Key | Action |
| --- | --- |
| return | submit current input |
| backspace | delete last char |
| ctrl+u | clear current input |
| up / down | walk input history |

Also surfaced through the command palette:

- `turmeric:start-repl` -- open the ReplView pane (or focus the existing one)
- `turmeric:stop-repl`  -- send `:quit` and terminate the subprocess

Configuration:

```lua
config.plugins.turmeric.repl_subcommand = "repl"   -- or "--interpret", etc.
config.plugins.turmeric.repl_max_lines  = 5000     -- ring-buffer cap
```

## Why Trowel (built on Lite XL)

Trowel is a lightweight, customizable editor built on top of Lite XL, a lightweight, fast text editor written in C and Lua. It gives us a cross-platform, hackable text editor with a single cohesive Lua file integration for the Turmeric language.
