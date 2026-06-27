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

Output streams into Lite XL's log pane (View > Toggle Log View if it is
not visible). Lines that look like `path:line:col:` come through
`core.error` so the log pane highlights them.

## Configuration

Override any of these in `~/.config/lite-xl/init.lua`:

```lua
local config = require "core.config"
config.plugins.turmeric = config.plugins.turmeric or {}
config.plugins.turmeric.tur = "/usr/local/bin/tur"   -- compiler binary
```

The plugin currently shells out to `tur run` because the documented
v1 target (`tur --interpret`) hits a segfault on a null
`ReaderMacroRegistry`; see
[`docs/reported/tur-interpret-null-reader-macro-registry.md`](../../docs/reported/tur-interpret-null-reader-macro-registry.md).
The command string is a one-line change in `turmeric.lua` once the
interpreter is fixed.

## Why Lite XL (not SciTE)

The earlier SciTE direction is [superseded](../../docs/upcoming/turmeric-scite-desktop-plan.md);
no free open-source SciTE Cocoa shell exists, which pushed three-OS v1
to 5-7 weeks. Lite XL ships native binaries for all three OSes from
one upstream CI matrix and exposes everything we need from a single
Lua file.
