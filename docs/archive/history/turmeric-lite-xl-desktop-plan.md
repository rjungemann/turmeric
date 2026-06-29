# Trowel Desktop Editor -- Plan (Archived)

## Goal

Ship a Processing-style desktop editor / REPL for Turmeric: a real native
app on the user's dock, a buffer, a Run button, an output pane, syntax
highlighting, and a path to autocomplete + REPL + sketch canvas later.

We base it on **Lite XL** (customized and renamed as **trowel**): a small
(~3 MB, ~4.4 MB DMG), MIT-licensed, SDL3-based cross-platform editor designed
to be extended by Lua plugins. The 2025-06 release ships native Apple Silicon,
Intel, and Universal `.dmg`s, native Windows installers/portable zips,
and Linux AppImages/tarballs -- all from a single upstream CI matrix.

This plan replaces the earlier SciTE direction
(`docs/upcoming/turmeric-scite-desktop-plan.md`, now superseded). The
SciTE approach hit a hard wall on macOS, where no free open-source SciTE
Cocoa shell exists -- only the paid Mac App Store build -- and any v1
that covered all three OSes ballooned into a 5-7 week project. Trowel
sidesteps that entirely: one binary, three OSes, native everywhere, no
shell to write.

### Why this is not just "wrap Try Turmeric"

A Tauri-wrap of the Try Turmeric PWA was the obvious shortcut and gets
ruled out for the same reason it was ruled out for SciTE: it inherits
browser sandboxing, which closes off the future features we actually
want (live graphics output, real FS access, debugger UI, native FFI
demos). Trowel is a native process with full subprocess and file-system
access from the plugin layer; nothing on the roadmap is structurally
blocked.

### Scope: turi first, tur later

V1 binds Run to **`tur --interpret <file>`** (the tree-walking
interpreter) once that subcommand is repaired -- it currently crashes
on a null `ReaderMacroRegistry`; see
[`docs/reported/tur-interpret-null-reader-macro-registry.md`](../reported/tur-interpret-null-reader-macro-registry.md).
Until then the plugin falls back to `tur run` (AOT) so that Run works
end-to-end today. The plugin treats the subcommand as configuration, so
the cutover is a one-line change in `tools/trowel/turmeric.lua`.

Compiler-mode `tur build` and a debugger pane are explicitly deferred.

## Why Trowel (built on Lite XL)

- **Native everywhere, no XQuartz.** The macOS `.app` links only system
  frameworks (AppKit, Foundation, Metal, CoreGraphics, etc.); no GTK,
  no Qt, no X11. Verified by `otool -L` on the upstream 2.1.8 arm64
  binary. SDL3 is statically linked.
- **One distribution per OS, already cut by upstream CI.** Apple Silicon
  / Intel / Universal `.dmg`, Windows `.exe` + portable `.zip`, Linux
  AppImage + tarball. We do not own a CI matrix for the editor itself
  in v1.
- **Tiny footprint.** 3.5 MB binary, 4.4 MB compressed DMG. Comparable
  to SciTE; orders of magnitude smaller than Qt/Electron/Tauri.
- **MIT-licensed and built to be forked.** Phase 5 can rebrand and
  bundle the plugin into a "trowel" distro with no license
  friction.
- **Plugin model is one Lua file.** Syntax, commands, keybindings,
  subprocess management, output streaming -- all from a small core API
  that is stable across the 2.x series (`mod-version:3`).
- **Real subprocess API.** `process.start(cmd, {stdin, stdout, stderr,
  cwd})` with async `read_stdout` / `read_stderr` / `wait` /
  `terminate` / `kill` / `returncode`. Everything we need to host `tur`
  as a child process is there; we do not have to invent it.
- **LogView is a built-in output pane** with `core.log` / `core.warn` /
  `core.error` entry points that already render `path:line:col` lines
  clickably. Phase 1 wires straight into it; Phase 4 swaps in a
  bespoke REPL view if we outgrow LogView.

## What we are explicitly NOT building (yet)

- A sketch canvas / graphics output window. Run output is text-only in
  v1; live graphics is a later, separate window (Trowel can spawn one
  via `process.start` of a `tur` sketch host, or via a future raylib
  companion).
- A debugger UI. Deferred behind the `tur` debugger
  (`docs/upcoming/debugger-plan.md`); the plugin already routes
  diagnostics so the protocol expansion is additive.
- Compiler-mode Run as default. V1 leans on the interpreter; flipping
  the default to `tur build` / AOT `tur repl` is a Phase-6 follow-up.
- A project manager / multi-file workspace UI beyond Trowel's existing
  TreeView + buffer-list. A `Tools > New Turmeric Project...` entry
  that shells out to `tur init` (Phase 1.5) is in scope precisely
  because it adds no project model of its own.

## Architecture

```
+------------------------------------------------------------+
|  Trowel window (single native app per OS)                  |
|  +------------------+  +--------------------------------+  |
|  | DocView          |  | TreeView (project files)       |  |
|  |  -- .tur lexer   |  +--------------------------------+  |
|  |  -- TM palette   |  | LogView (output / diagnostics) |  |
|  +------------------+  +--------------------------------+  |
+------------------------------------------------------------+
            |                          ^
            v                          |
        Run / Check command  --------- |
        (cmd+r / cmd+shift+r)          |
        process.start(tur ...) ------- |
        with stdout/stderr pipes       |
                                       |
        Autocomplete / calltip  -------+
        backend (stdin RPC to a
        long-lived `tur lsp-lite`
        helper -- new subcommand)
```

Three integration surfaces, in landing order:

1. **Lexer + commands**: `tools/trowel/turmeric.lua` declares the
   syntax and registers `turmeric:run-file` / `turmeric:check-file` with
   keybindings. This is the entire Phase 1 deliverable.
2. **Color theme**: a Trowel `colors/turmeric.lua` ported 1:1 from the
   Try Turmeric CSS palette.
3. **Autocomplete + REPL**: `tur lsp-lite` subcommand of `tur`. The
   plugin talks to it over stdio (Trowel's `process.start` makes this
   straightforward) and feeds completions into Trowel's built-in
   `autocomplete` plugin.

## Phase breakdown

### Phase 1 -- "It runs Turmeric files" (DONE in spike)

Goal: open a `.tur` file in Trowel, press cmd+r, see the program
output in the log pane; press cmd+shift+r, see diagnostics in the log
pane with `path:line:col` lines clickable.

Deliverables, already landed in tree:

- `tools/trowel/turmeric.lua` -- one-file plugin: syntax mode (pattern
  list + symbol table), `turmeric:run-file` + `turmeric:check-file`
  commands, cmd+r / cmd+shift+r keymap. Streams stdout/stderr from
  `tur` into the log pane and routes `path:line:col:` diagnostics
  through `core.error` for highlighting.
- `tools/trowel/README.md` (TODO) -- per-OS install instructions:
  drop `turmeric.lua` into `~/.config/trowel/plugins/`, set
  `config.plugins.turmeric.tur` to the compiler path if `tur` is not on
  the PATH inherited by the Finder-launched app.
- A Justfile `trowel` task that launches the system Trowel with the
  in-tree plugin symlinked in and the in-tree compiler pre-configured.

Acceptance: end-to-end demo runs without touching the SciTE tree.

### Phase 1.5 -- "New Turmeric Project..." (0.5 day)

Goal: scaffold a project from inside the editor.

- Add a `turmeric:new-project` command (no keymap by default; surfaces
  through the command palette) that prompts for a destination directory
  + project name via Trowel's `CommandView` and shells out to
  `tur init <name>` (or `tur init --sweet`, `--lib` via flag-prefixed
  variants).
- After scaffolding succeeds, open `<dir>/build.tur` (or
  `build.tur.sweet`) and `src/main.tur` in new buffers.
- Config knob `config.plugins.turmeric.init_cmd` defaults to `tur init`
  for the same swap-readiness as the Run path.

Acceptance: from a fresh Trowel window, run the command, end up with a
generated project directory and `build.tur` open.

### Phase 2 -- Try Turmeric color theme (1 day)

Goal: Turmeric source looks like Turmeric source, with the same palette
as Try Turmeric.

- `tools/trowel/colors/turmeric.lua` -- a Trowel color theme using
  the Try Turmeric palette (read from `web/main.js` token colors).
  Both `mod-version:3` light and dark variants.
- A Python test in `tests/trowel/check-palette-sync.py` that diffs
  `web/main.js` token colors against `tools/trowel/colors/turmeric.lua`
  and fails if they drift -- mirroring the parity check from the
  earlier SciTE plan.
- Refine the Phase-1 pattern list with `;;;` docstring blocks, sweet-exp
  indentation markers, curly-infix braces, `#map{` / `#set{` / `#row{`
  data literals, inline-C fences -- all using Lua patterns.

Acceptance: opening any stdlib `.tur` file shows keywords, types,
docstrings, data literals, and inline-C fences in the Try Turmeric
palette. Light + dark both ship.

### Phase 3 -- Autocomplete and calltips (3 days)

Goal: typing `(co` pops up `cons`, `cond`, `cond->`; typing `(cons `
shows a calltip with the argument list.

- Extend `tur` with a new subcommand `tur lsp-lite` (working name)
  exposing `tokenize` / `complete` / `calltip` / `signature` / `doc`
  over stdio JSON-RPC. Reuses the existing docstring registry built by
  `tools/gendocs.py --emit-tur stdlib/docstrings.tur`; adds a
  symbol-to-signature map from the compiler's defn-arity table.
- Plugin spawns one long-lived `tur lsp-lite` helper on first edit and
  talks to it from a Trowel coroutine. Feeds completion entries into
  Trowel's built-in `autocomplete` plugin via its public API.
- F1 / cmd+i on a symbol opens `docs/api/<module>.html` in the system
  browser via `process.start({"open" or "xdg-open" or "start", url})`.

Acceptance: stdlib names autocomplete with calltips; in-buffer names
appear in the same popup.

### Phase 4 -- REPL pane (2 days)

Goal: a Turmeric REPL inside the editor.

- Add a `turmeric:start-repl` command that spawns `tur --interpret` (or
  `tur run --repl` once that lands) and wires stdin/stdout to a custom
  `ReplView` derived from Trowel's `View` class. (Trowel's CommandView
  + LogView together give us most of the building blocks.)
- A reload key (default cmd+shift+L) sends the current buffer to the
  REPL.
- The REPL uses the Phase-3 `lsp-lite` helper for completion at the
  prompt -- one tokenizer, two consumers.

Acceptance: start REPL, type `(my-fn 1 2)`, get a value; edit `my-fn`,
press reload, call again, see new behavior.

### Phase 5 -- Distribution: "Trowel" branded bundle for macOS + Linux

The scaffolding (install.sh, vendor.sh, bake-bundle.sh, make-dmg.sh,
make-appimage.sh, Info.plist, .desktop, Homebrew cask template) shipped
in commit `eb5c1af66`. The remaining shipping work -- actually vendoring
Trowel, the first end-to-end build on each OS, Apple Developer ID
signing + notarization, Universal DMG, release-workflow integration, and
Homebrew tap publication -- is broken out into its own plan:
[`trowel-renaming-plan.md`](trowel-renaming-plan.md) and
[`trowel-distribution-plan.md`](../../upcoming/trowel-distribution-plan.md).
Windows distribution stays in
[`turmeric-lite-xl-windows-plan.md`](turmeric-lite-xl-windows-plan.md).

## Files and locations

- `tools/trowel/` -- everything new lives here. Holds `turmeric.lua`,
  `colors/turmeric.lua`, install scripts, README, an example
  `init.lua`, and the palette-sync test.
- `src/cli/lsp_lite.c` (new, Phase 3) -- the `tur lsp-lite` subcommand.
  Reuses the existing tokenizer + docstring registry; adds no new
  dependencies.
- `web/try/` -- color palette source of truth; Phase 2 reads token
  colors from `web/main.js` and mirrors them into the Trowel theme. A
  Python test asserts sync.
- `vendor/trowel/` (Phase 5) -- pinned upstream vendor drop for the
  bundled "Trowel" distribution.
- `docs/guides/desktop-editor-guide.md` (added with Phase 1) -- user
  documentation: installing, customizing, troubleshooting.

## Non-goals / explicit deferrals

- **No project window beyond Trowel's existing TreeView + buffer
  list.** The `Tools > New Turmeric Project...` entry shells out to
  `tur init` -- it adds no project model of its own.
- **No embedded graphics output.** The log pane is text-only. A future
  plan can add a raylib companion that the editor launches as a child
  process for sketches that draw.
- **No telemetry, update checker, or sign-in.** Desktop tool, stays that
  way.
- **No bundling of `tur` itself.** Editor and compiler stay separable;
  bleeding-edge compiler builds don't get a stale compiler glued to a
  stable editor.

## Open questions

1. **Distribution: bundled "Trowel" vs. plugin-only.** The
   plugin-only path (Phase 1-4) is enough for early adopters: install
   stock Trowel/Lite XL, drop the plugin in. The bundled distro (Phase 5)
   adds polish but also adds a vendor drop, a build matrix, and a
   signing/notarization burden. We can ship Phase 1-4 first and treat
   Phase 5 as a measured-by-demand follow-up.
2. **`tur lsp-lite` vs. a real LSP server.** Same question as the SciTE
   plan: minimal stdio RPC for Phase 3, with the option to grow into a
   real LSP server that any editor (VS Code, Helix, Emacs) can consume.
3. **Structured diagnostics.** Phase 1 parses `path:line:col:` lines via
   pattern. A future `tur --diag-json` mode would let the plugin
   underline ranges in the buffer; depends on the compiler growing
   structured diagnostics, which is a sibling effort.

## Forward-compatibility hooks (turi-first, but tur/debugger ready)

- **Run-mode selector** lives as `config.plugins.turmeric.run_cmd` --
  defaults to `{"run"}` today, flips to `{"--interpret"}` once that
  subcommand is repaired, swaps to `{"build"}` or `{"debug"}` for
  Phase 6 / Phase 7.
- **Output-pane protocol**: Phase 3's `lsp-lite` helper RPC reserves a
  `kind` field on every line (`stdout`, `stderr`, `diag`, `repl-value`,
  reserved future kinds `debug-event`, `breakpoint-hit`). The plugin
  ignores unknown kinds in v1; the debugger phase starts emitting them
  without a protocol break.
- **Margin markers**: Trowel `DocView` already supports gutter marks.
  Phase 2 reserves a `breakpoint` mark; the debugger phase populates
  it without further plumbing.

## Followups (not part of this plan)

- **Phase 6 -- compiler-mode Run.** Flip the default run command to
  `tur build` / AOT `tur repl`. ~1-2 days on top of v1.
- **Phase 7 -- debugger pane.** Wire `tur` debugger into the log/REPL
  pane using the reserved `debug-event` kinds and the breakpoint
  margin from Phase 2.
- Real LSP server building on `lsp-lite` for editor portability.
- Companion graphics window for sketches that draw.

## Effort estimate

Single developer, end-to-end:

- Phase 1 (DONE in spike): ~0.5 day to finalize, README, Justfile task.
- Phase 1.5: 0.5 day.
- Phase 2: 1 day.
- Phase 3: 3 days.
- Phase 4: 2 days.

**Total plugin-only v1: ~7 working days.**

Phase 5 macOS + Linux bundled distro adds ~5 working days (vendor drop,
branding, Homebrew cask, two-OS CI tweaks, macOS signing/notarization).
Total with macOS + Linux distribution: ~12 working days.

Windows distribution is tracked separately in
[`turmeric-lite-xl-windows-plan.md`](turmeric-lite-xl-windows-plan.md)
and adds another ~8-9 working days plus signing-cert procurement and
package-channel review wait time.

Compare to the superseded SciTE plan, which was 5-7 weeks for three-OS
parity once the embedding-research finding (no Cocoa SciTE shell) was
honest. Trowel collapses the macOS half of that estimate to "use the
upstream `.dmg`."

## Spike report

The spike (this commit) verified the load-bearing assumptions:

- `lite-xl-v2.1.8-macos-arm64.dmg` is 4.4 MB; the embedded binary is
  3.5 MB Mach-O arm64, ad-hoc signed, links AppKit + Foundation +
  Metal + CoreGraphics + Carbon + IOKit + libobjc + libSystem only --
  no XQuartz, no GTK, no Qt, no bundled dylibs.
- A `mod-version:3` plugin at `tools/trowel/turmeric.lua` registers
  the Turmeric lexer, two commands, and a keymap in <150 lines of Lua.
- `process.start(cmd, {stdout=PIPE, stderr=PIPE, cwd})` plus
  `core.add_thread(coroutine)` is the canonical async-subprocess shape;
  output streams into `core.log` / `core.error` and shows up in the log
  pane immediately.
- Launching the .app with the plugin symlinked into
  `~/.config/trowel/plugins/` and an `init.lua` pinning
  `config.plugins.turmeric.tur` to the in-tree build produces a clean
  boot (no stderr, no warnings).
- Caught a `tur --interpret` segfault as a side benefit (logged at
  `docs/reported/tur-interpret-null-reader-macro-registry.md`); the
  plugin falls back to `tur run` cleanly via the config knob.

The spike artifacts (`tools/trowel/turmeric.lua`) become the Phase 1
deliverable -- no rework needed when promoting from spike to v1.
