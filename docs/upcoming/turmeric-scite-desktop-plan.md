# Turmeric Desktop Editor (SciTE-embedded) -- Plan

## Goal

Ship a small, native, double-clickable desktop editor/REPL for Turmeric in
the spirit of Try Turmeric, but as a real app on the user's dock --
**Turmeric SciTE**: SciTE wrapping a Scintilla buffer with Turmeric syntax
highlighting, auto-complete, an integrated REPL/console, a Run button, and
the Try Turmeric color scheme.

This is the "compact and boring and works tomorrow" route. It is explicitly
**not** Tauri-wrap-the-PWA, and **not** an ImGui native shell. Those remain
viable parallel tracks; this plan covers only the SciTE direction.

### Scope: turi first, tur later

The v1 backend is **turi** -- the tree-walking interpreter -- not the
compiler. Run, REPL, autocomplete, and diagnostics all go through `turi`
(or a new `tur turi`/`tur lsp-lite` subcommand layered on the same
front-end). Concretely:

- Run button shells out to `turi <file>` (or the equivalent `tur run`
  interpreter path), not `tur build`.
- The REPL pane is the existing turi REPL, not the AOT-compiled
  `tur repl` shared-lib path.
- Diagnostics are whatever turi prints to stderr, parsed by the same
  `TUR-E####` regex.

This shrinks the moving parts (no codegen, no `.tur-repl-cache/`, no
`build.tur` walk-up dependency, no leak-checked compile path) and gets
us the Processing-style edit-run-console loop fastest.

Compiler-mode (`tur build` / `tur run` AOT path) and a debugger pane
are **explicitly deferred, not precluded**. The properties files, the
`lsp-lite` RPC shape, and the diagnostic regex are all designed so that
a future `mode = compiler` or `mode = debugger` toggle slots in without
re-architecting the editor. See "Forward-compatibility hooks" below.

## Why SciTE

- Scintilla is the editor component behind Notepad++, RStudio's old editor,
  and dozens of small tools; it is rock-solid, C++/portable, and gives us
  styled text, folding, gutters, autocompletion popups, calltips, and
  margin markers for free.
- SciTE is the reference single-window shell on top of Scintilla. Its
  behavior -- including build/run commands, output panes, keymaps, lexer
  configuration, and theming -- is driven by `.properties` files. A
  surprising amount of "make this look and feel like a Turmeric IDE" is
  configuration, not C++.
- Footprint: single-digit MB binary, no webview, no Node, no JIT.
- All three desktop OSes are covered (Linux/Windows native; macOS via the
  Cocoa SciTE port).

## What we are explicitly NOT building (yet)

- A sketch canvas / graphics output window. The Run target for v1 is the
  SciTE output pane (textual console). Live graphics is left to the future
  ImGui/raylib direction.
- A debugger UI. Future hookup to the in-flight Turmeric debugger
  (`docs/upcoming/debugger-plan.md`) is mentioned in followups but not in
  the v1 scope. Not precluded -- see "Forward-compatibility hooks".
- Compiler-mode Run (`tur build` / AOT `tur run`). The v1 Run button
  drives **turi**. Compiler-mode is a Phase-6 followup, not a v1 gate.
- A project manager / multi-file workspace UI beyond what SciTE already
  ships (open file, recent files, session). `build.tur` discovery is reused
  from the CLI -- no new project model.

## Architecture

```
+------------------------------------------------------------+
|  SciTE window                                              |
|  +------------------+   +-------------------------------+  |
|  | Scintilla buffer |   | Output pane (Scintilla too)   |  |
|  | -- Turmeric lexer|   |  Run button output, REPL I/O, |  |
|  | -- TM color sch. |   |  compiler diagnostics         |  |
|  +------------------+   +-------------------------------+  |
+------------------------------------------------------------+
            |                          ^
            v                          |
        Build/Run command  ----------- |
        (shells out to `tur`)          |
                                       |
        Autocomplete / calltip  -------+
        backend (stdin RPC to a
        long-lived `tur lsp-lite`
        helper -- new subcommand)
```

Three integration surfaces:

1. **Lexer**: a Turmeric lexer registered with Scintilla, configured via
   SciTE `.properties`.
2. **Build/Run**: SciTE's `command.build.*` / `command.go.*` properties
   invoke `tur` against the current buffer (or its enclosing `build.tur`).
3. **Autocomplete + REPL**: a small JSON-RPC helper subcommand of `tur`
   (working name: `tur lsp-lite`) that SciTE talks to over a pipe -- one
   process for the session, fed by Scintilla notifications and Run
   invocations.

The three surfaces are independent and ship in that order. Each is useful
on its own; we never need all three to land in one PR.

## Phase breakdown

### Phase 1 -- "It runs Turmeric files" via turi (1-2 days)

Goal: install SciTE, point it at `turi`, and get Run / Check working from
a stock SciTE binary with **zero** custom C++.

Deliverables in this repo:

- `tools/scite/turmeric.properties` -- SciTE properties file that:
  - Registers `*.tur` and `*.tur.sweet`.
  - Sets `command.go.$(file.patterns.tur)` to `turi $(FileNameExt)`
    (the v1 Run target -- interpreter).
  - Sets `command.compile.$(file.patterns.tur)` to `tur check $(FileNameExt)`
    (static check still uses the compiler front-end; safe and fast).
  - Reserves `command.build.$(file.patterns.tur)` as a commented-out
    `tur build $(FileNameExt)` line for the Phase-6 compiler-mode
    followup -- the slot exists, it's just disabled in v1.
  - Sets `command.go.subsystem` so console output streams into the SciTE
    output pane.
  - Defines an error-message regex (`error.list.regex`) that parses
    Turmeric `TUR-E####` diagnostics into clickable file:line jumps.
- `tools/scite/README.md` -- one-page "drop these properties into your
  SciTE user config" instructions for Linux/Windows/macOS.
- A regex calibration fixture: feed a handful of canonical `TUR-E####`
  outputs at the harness and confirm SciTE's "next error" jumps land on
  the right line.

Acceptance: open a `.tur` file in stock SciTE, press F7 / Ctrl+F7,
compiler output appears in the bottom pane, clicking an error jumps to
the source line.

### Phase 2 -- Syntax highlighting + Try Turmeric color scheme (2-3 days)

Goal: Turmeric source looks like Turmeric source, with the same palette as
Try Turmeric.

Two paths; we choose path A unless it proves unworkable.

**Path A: container lexer driven by `tur lsp-lite tokenize`.**
- Scintilla supports "container lexing": the host application supplies
  styles for byte ranges in response to a notification. We add a small
  `--keep-properties` shim (still no custom shell -- this lives as a
  patch in `tools/scite/turmeric-lexer-patch.diff` against SciTE's
  `LexLPeg` slot OR is provided as a Lua script if we use SciTE's Lua
  build).
- The real tokenizer lives in `tur` itself, exposed as a new subcommand
  `tur lsp-lite tokenize`. It reads source on stdin and emits a stream of
  `(start, length, style)` triples on stdout. This guarantees the editor
  agrees with the compiler about what is a keyword, special form,
  docstring (`;;;`), inline-C block, sweet-exp indentation marker, curly-
  infix expression, data literal (`#map{ ... }`, `#set{ ... }`, `#row{...}`),
  etc.

**Path B (fallback): native Scintilla lexer in C++.** Hand-written
`LexTurmeric.cxx`, registered statically into a tiny custom SciTE build.
More effort; only taken if container lexing turns out to be unavailable
on the macOS Cocoa SciTE port we care about.

Style palette: ported 1:1 from Try Turmeric's CSS (`web/main.js` token
colors). Captured in `tools/scite/turmeric-colors.properties` and split
into a light / dark variant so SciTE's `Options > Use Monospaced Font` /
theme switch picks it up.

Acceptance: opening any stdlib `.tur` file shows: keywords, types,
defstruct/defn/defmacro markers, `;;;` docstring blocks, `(import ...)`
forms, sweet-exp indentation markers, curly-infix, inline-C fences, and
string/number literals, all in the Try Turmeric palette. Dark and light
variants both ship.

### Phase 3 -- Autocomplete and calltips (3-4 days)

Goal: typing `(co` pops up `cons`, `cond`, `cond->`, etc.; typing `(cons `
shows the calltip with the argument list pulled from the live docstring
table.

- Extend `tur lsp-lite` with two more requests: `complete` and `calltip`.
- Backend reuses the existing docstring registry built by
  `tools/gendocs.py --emit-tur stdlib/docstrings.tur`. We already have a
  symbol -> doc map; we add a symbol -> signature map by reusing the
  compiler's defn-arity table.
- SciTE side wires Scintilla's `AutoCShow` / `CallTipShow` to the
  RPC responses. With the Lua-enabled SciTE this is ~50 lines of Lua; for
  non-Lua SciTE it is the same as the container-lexer hook.
- Locally-defined names: the helper additionally scans the current buffer
  on `SCN_MODIFIED` and emits a list of in-scope `defn` / `def` /
  `defstruct` names so completion includes them.

Acceptance: stdlib names autocomplete with calltips; in-buffer names
appear in the same popup; pressing F1 on a symbol opens the
`docs/api/<module>.html` page in the system browser via
`xdg-open` / `open` / `start`.

### Phase 4 -- turi REPL pane (2-3 days)

Goal: a real turi REPL prompt inside the SciTE output pane.

- Add a `Tools > Start REPL` menu entry that spawns **`turi`** (no args,
  or with the current file pre-loaded) in a subprocess with stdio
  attached to the output pane. No AOT cache, no `.tur-repl-cache/`,
  no shared-lib dlopen -- just the interpreter.
- Add a reload key (default `Ctrl+Shift+L`) that re-evaluates the
  current buffer in the running turi REPL. Subsequent Run-button
  presses send the current selection / file to the running REPL
  instead of re-launching `turi` each time.
- The REPL inherits the autocomplete backend from Phase 3 by talking to
  the same `lsp-lite` helper -- there is no duplicate tokenizer.

Acceptance: open a file, start REPL, type `(my-fn 1 2)` at the prompt,
get a value; edit `my-fn`, press reload key, call it again, see the new
behavior.

(`tur repl` -- the AOT spice REPL with `.tur-repl-cache/` -- is the
Phase-6 compiler-mode upgrade, not v1.)

### Phase 5 -- Polish + packaging (2-3 days)

- Per-OS bundles: a macOS `.app` containing the Cocoa SciTE build with our
  properties pre-installed, a Windows `.zip` with `SciTE.exe` and the
  properties dropped next to it, a Linux `tar.gz` with a `.desktop`
  entry. None of these ship a `tur` binary -- they assume `tur` is on
  PATH and surface a clear error in the output pane when it is not.
- A `tools/scite/install.sh` (and `.ps1`) that drops the properties into
  the user's SciTE config directory for users who already have SciTE
  installed and just want the Turmeric mode.
- **Homebrew cask** for the macOS bundle, distributed via a tap (working
  name: `rjungemann/turmeric`). Lives at
  `tools/scite/homebrew/turmeric-scite.rb` in this repo and is mirrored
  into the tap by the release workflow. Shape:

  ```ruby
  cask "turmeric-scite" do
    version "0.1.0"
    sha256 "<filled by release workflow>"

    url "https://github.com/rjungemann/turmeric/releases/download/" \
        "v#{version}/TurmericSciTE-#{version}-macos.zip"
    name "Turmeric SciTE"
    desc "Desktop editor / REPL for the Turmeric language"
    homepage "https://turmeric.dev/"

    depends_on formula: "turmeric"   # the `tur` compiler formula
    depends_on macos: ">= :ventura"

    app "Turmeric SciTE.app"

    zap trash: [
      "~/Library/Preferences/dev.turmeric.scite.plist",
      "~/Library/Application Support/Turmeric SciTE",
    ]
  end
  ```

  Install path for users: `brew install --cask rjungemann/turmeric/turmeric-scite`.
  The cask `depends_on formula: "turmeric"` so a single command pulls
  both the editor and the compiler -- consistent with the "we don't
  bundle `tur`" rule while still making the first-run experience one
  command.
- Release-cut skill update: the macOS bundle build + cask SHA refresh
  hooks into the existing `cut-*-release` skills. The release workflow
  builds the bundle, uploads it to the GitHub release, computes the
  SHA-256, and opens (or pushes directly to, if the tap is in the same
  org) a PR against the tap repo bumping `version` and `sha256`.
- Linux: a parallel `tools/scite/aur/` PKGBUILD is a stretch goal under
  this phase; not required for v1.
- Try Turmeric homepage gets a "Get the desktop editor" callout linking
  to the release artifacts and showing the `brew install --cask ...`
  one-liner. (Coordinated with the homepage positioning note in memory.)

Acceptance: a brand-new user can download the macOS bundle, open it,
double-click an `examples/*.tur` file, press Run, and see output --
without having read any docs beyond a one-paragraph "you'll also need
`tur` on your PATH" line.

## Files and locations

- `tools/scite/` -- everything new lives here. Holds `.properties`,
  optional Lua scripts, install scripts, README, color palette files,
  and the regex fixture for diagnostic parsing.
- `src/cli/lsp_lite.c` (new) -- the `tur lsp-lite` subcommand:
  `tokenize`, `complete`, `calltip`, `signature`, `doc` over stdio
  JSON-RPC. Reuses the existing tokenizer + docstring registry; adds
  no new dependencies.
- `web/try/` -- color palette is the source of truth; Phase 2 reads
  `web/main.js` token styles and mirrors them into
  `tools/scite/turmeric-colors.properties`. A test asserts they stay in
  sync (a small Python diff in `tests/scite/`).
- `docs/guides/scite-desktop-guide.md` (added with Phase 1) -- user
  documentation: installing, customizing, troubleshooting, where to put
  user properties.

## Non-goals / explicit deferrals

- **No project window beyond SciTE's session model.** Multi-file editing
  works via SciTE's existing tab/buffer support; we don't add a tree.
- **No embedded graphics output.** The Run pane is text-only. A future
  plan can add a Tauri-or-raylib companion that the editor launches as
  a child process for sketches that draw.
- **No telemetry, update checker, or sign-in.** This is a desktop tool
  and stays that way.
- **No bundling of `tur` itself.** Keeping the editor and compiler
  separable means users on bleeding-edge compiler builds (which is
  ~everyone right now) don't get a stale compiler glued to the editor.

## Open questions

1. **Lua-SciTE or vanilla SciTE?** Lua scripting unlocks Phases 2-4 with
   no C++ changes; the Cocoa SciTE port has historically lagged on the
   Lua extension. We choose Lua-SciTE if and only if all three OS
   targets have a current Lua-enabled build; otherwise we fall back to
   the C++ container-lexer hook (still small).
2. **`tur lsp-lite` vs. a real LSP.** Going full LSP would let VS Code,
   Helix, Emacs, etc. share the same backend. For this plan we ship the
   minimal stdio RPC; a real LSP is a sibling effort that should reuse
   the same `tokenize/complete/calltip` core. Flagged in followups.
3. **Diagnostic streaming.** Phase 1 parses diagnostics out of stdout
   via regex. Phase 3+ may want a structured `--diag-json` flag on the
   compiler so SciTE can underline ranges. This depends on whether the
   compiler already has a JSON diagnostic mode (it does not, today);
   if we add one, both SciTE and a future LSP benefit.

## Forward-compatibility hooks (turi-first, but tur/debugger ready)

The v1 surface is turi, but the seams below are designed so the
compiler and debugger drop in as additive phases, not rewrites:

- **Run-mode selector.** The properties file defines a single
  `turmeric.run.mode` user property (`turi` in v1; future values:
  `compiler`, `debug`). All Run/REPL command bindings read this
  property, so flipping to `compiler` swaps `turi $(FileNameExt)` for
  `tur run $(FileNameExt)` and the AOT `tur repl` without touching
  the lexer, palette, or autocomplete wiring.
- **Subcommand naming.** The helper is `tur lsp-lite`, not `turi
  lsp-lite`. It lives in the compiler binary from day one and shells
  out to whichever evaluator the Run-mode says -- so adding compiler
  Run later is a backend change, not an editor change.
- **Diagnostic regex.** The error-list regex matches the shared
  `TUR-E####` family, which both turi and tur emit. Same regex
  survives the compiler-mode switch.
- **Output-pane protocol.** Run output is plain text in v1, but the
  helper RPC reserves a `kind` field on every line (`stdout`,
  `stderr`, `diag`, `repl-value`, and the reserved future kinds
  `debug-event`, `breakpoint-hit`). The editor ignores unknown kinds
  in v1; the debugger phase starts emitting them without a protocol
  break.
- **Margin markers.** Phase 2 reserves the breakpoint margin (Scintilla
  margin 1) and leaves it empty in v1. The debugger phase populates
  it without re-laying-out the editor.

These are cheap to add now and impossible to retrofit cleanly later.

## Followups (not part of this plan)

- **Phase 6 -- compiler-mode Run.** Flip `turmeric.run.mode = compiler`,
  enable `command.build.*`, swap `turi` for `tur run` and the AOT
  `tur repl`. ~1-2 days on top of v1.
- **Phase 7 -- debugger pane.** Wire `tur` debugger
  (`docs/upcoming/debugger-plan.md`) into the output pane using the
  reserved `debug-event` kinds and the breakpoint margin from Phase 2.
- Real LSP server building on `lsp-lite` for editor portability.
- Companion graphics window for sketches that draw.
- Tauri-wrap Try Turmeric as a parallel, web-stack desktop route (the
  other branch we considered).

## Effort estimate

Roughly **9-13 working days** end-to-end for a single developer, with
Phases 1 + 2 (~5 days) being the smallest viable first cut that is worth
publishing on its own. The Homebrew cask in Phase 5 is ~0.5 day on top
once the macOS bundle exists, assuming the `turmeric` formula already
lives in a tap we control.
