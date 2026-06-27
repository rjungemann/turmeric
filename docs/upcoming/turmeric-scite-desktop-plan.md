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
- Linux and Windows are covered by upstream SciTE -- we re-skin the
  existing shell with our properties, lexer, and theme baked in. macOS
  is **not** covered by upstream (no free Cocoa SciTE shell exists --
  see "Embedding / packaging research" below); on macOS we ship a thin
  native Cocoa host around the Scintilla widget instead. XQuartz/X11 is
  explicitly off the table.

## Embedding / packaging research

The first version of this plan assumed "ship our properties, tell users
to install upstream SciTE, done on all three OSes." Research into the
2025/2026 state of the upstream tree shows the macOS half of that
assumption does not hold. This section captures the dependency and
distribution reality and the chosen path.

### The units we actually embed

- **Scintilla** is the editor *widget* (text buffer + view). It ships
  active native backends for Win32, GTK, Cocoa, and Qt. License is the
  permissive HPND-style "Scintilla License" -- rebrand, rename, and
  redistribute are all fine as long as Neil Hodgson's copyright stays
  in the source and About box.
- **Lexilla** is the lexer library that was split out at Scintilla 5.0
  with a stable C ABI. Scintilla loads lexers from it at runtime, so
  adding a Turmeric lexer is one C++17 class linked into Lexilla -- no
  Scintilla fork needed.
- **SciTE** is the reference *shell* around Scintilla -- menus, file
  management, build/run pipeline, output pane, properties loader, Lua
  extension layer. The SciTE source tree carries three separate shells
  (`win32/`, `gtk/`, `cocoa/`-adjacent), and most of what feels like
  "customization" is properties + Lua, not C++: `command.build`,
  `command.go.*.tur=tur --interpret $(FileNameExt)`, error-regex
  patterns, key bindings, status bar format are all properties.
  Window title, About box text, and app icon are baked into resource
  files and need a ~50-line patch to rebrand.

### Per-OS upstream status (2025/2026)

- **Windows.** SciTE 5.6.3 (released 2026-06-06) ships a healthy native
  Win32 `SciTE.exe` with full Lua and the properties system. Re-skinning
  is straightforward.
- **Linux / GTK.** The GTK shell is current (GTK3, tested on Fedora 36 /
  Ubuntu 22.04), shipped as binaries and Debian apt packages. Re-skin
  works.
- **macOS -- the load-bearing finding.** *No free, open-source SciTE
  Cocoa shell exists.* The Scintilla source's `cocoa/` directory contains
  the **widget** (`ScintillaCocoa.mm`, `ScintillaView.mm`, `PlatCocoa.mm`,
  `InfoBar.mm`) plus a `ScintillaTest` sample app -- not a SciTE shell.
  The only released Cocoa SciTE is Neil Hodgson's closed-source paid
  Mac App Store build. MacPorts' `scite` port is GTK3 on macOS, which
  means XQuartz or native-Quartz GTK -- both unacceptable. The Scintilla
  Cocoa **widget** itself is actively maintained (fixes through Aug 2024;
  macOS 10.13 minimum since 5.3.8). So we have a healthy widget and no
  shell.

### Options considered

- **(A) Re-skin and re-package upstream SciTE.** Clone the SciTE source,
  drop `turmeric.properties` + the Turmeric Lexilla lexer + colors into
  the bundle, patch ~50 lines of resource files for title/icon/About,
  bake an in-bundle properties dir lookup, build, sign, ship. Works on
  Win32 and GTK Linux. **Does not exist for macOS** without first
  writing a Cocoa SciTE shell from scratch.
- **(B) Embed Scintilla in our own minimal native shell.** Cocoa NSView
  on macOS, GTK on Linux, Win32 on Windows. Linear cost: three shells,
  three accelerator tables, three runners, three output panes. Each is
  a few hundred lines of glue around a widget that already does the
  heavy lifting. Single-developer estimate for full three-OS parity:
  4-8 weeks. We lose the Lua extension surface and the body of community
  properties unless we reimplement the properties loader.
- **(C) Qt + ScintillaEdit.** One codebase, three OSes. Costs: Qt6
  binary is 30-80 MB before compression, macOS chrome looks "Qt-ish"
  rather than truly native, signing/notarization adds a step, and we
  inherit a heavyweight cross-platform dep for a one-developer project.

### Chosen path -- hybrid A + narrow-B

**Re-skin upstream SciTE on Windows and Linux. Ship a narrow custom
Cocoa shell around the Scintilla widget on macOS.** No Qt, no XQuartz,
no GTK-on-macOS.

What is shared across all three OSes:

- The Turmeric `.properties` file (Phase 1).
- The Lexilla Turmeric lexer (Phase 2).
- The color theme (Phase 2).
- The `tur lsp-lite` autocomplete backend (Phase 3).
- The diagnostic regex and the run-mode selector
  (`turmeric.run.mode`, Phase 1).

What is per-OS:

- **Win32 + GTK:** vendor upstream SciTE source, drop our properties /
  lexer / theme into the bundle, patch resource files for branding,
  ship `Turmeric SciTE.exe` and a `.deb`. Lua scripting comes along
  for free.
- **macOS:** vendor `scintilla/cocoa/` into `vendor/scintilla/`. Use the
  `ScintillaTest` Xcode project as the skeleton. Build a minimal
  `AppDelegate` + `NSWindowController` + split view: `ScintillaView`
  on top, read-only `NSTextView` for the output pane on the bottom,
  an `NSToolbar` with Build/Run buttons that `NSTask`-exec `tur`.
  Reimplement only the SciTE features Turmeric actually uses --
  open/save, extension-to-lexer dispatch via Lexilla, the `command.go`
  runner, error-regex line parsing for clickable output, Cmd-key
  bindings. Estimated **~1500-2500 LOC of Objective-C++** for shell
  parity with what properties + SciTE shell give us for free on the
  other two OSes. Lua is deferred on macOS for v1.

### Honest effort estimate

The original "9-13 working days" estimate assumed option A worked on all
three OSes. With the macOS path now option B, the real budget is:

- **Win32 + Linux (option A skin):** ~9-13 days, as originally scoped.
- **macOS (option B narrow Cocoa shell):** ~3-5 weeks for shell parity.
- **Total for three-OS v1:** ~5-7 weeks single-developer.

If that exceeds appetite, two honest reductions:

- **Ship Win32 + Linux v1 first**, mark macOS "in progress" with a
  pointer at the upstream paid Mac App Store SciTE + our properties as a
  bring-your-own-editor fallback for early macOS users. The Phase 5
  Homebrew cask graduates to actually shipping a real bundle once the
  Cocoa shell lands.
- **Pivot macOS to Qt (option C)** and accept the size + look-and-feel
  cost. Not recommended.

Abort is not warranted -- the Scintilla Cocoa widget is healthy and a
thin shell is a known shape of work -- but the macOS half is a
sub-project of its own, not a property-file drop.

### License + branding

The Scintilla license is HPND-style permissive. Rebranding to "Turmeric
SciTE" is fine; the source files and the About box must keep Neil
Hodgson's copyright notice. The macOS Cocoa shell we write ourselves
ships under our own license, vendoring the Scintilla widget under its.

### Cited sources

- Scintilla docs (platforms, widget): <https://www.scintilla.org/ScintillaDoc.html>
- Lexilla overview: <https://www.scintilla.org/Lexilla.html>
- SciTE downloads (Win32 + GTK, no macOS): <https://www.scintilla.org/SciTEDownload.html>
- SciTE for OS X (paid Mac App Store): <https://www.scintilla.org/SciTE-OSX.html>
- SciTE Lua extension scope: <https://www.scintilla.org/SciTELua.html>
- Scintilla license: <https://www.scintilla.org/License.txt>
- Scintilla Cocoa source (widget only, no SciTE shell): <https://github.com/mirror/scintilla/tree/master/cocoa>

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
  from the CLI -- no new project model. A `Tools > New Turmeric Project...`
  entry that shells out to `tur init` (Phase 1.5) is in scope precisely
  because it adds no project model of its own; everything past that --
  workspace trees, multi-root windows, dependency UIs -- stays out.

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

### Phase 1.5 -- "New Turmeric Project..." menu entry (0.5 day)

Goal: a first-run user who installed the editor and `tur` can scaffold a
working project from inside SciTE without dropping to a shell. This is a
plain shell-out to `tur init` -- the same model as Run/Check -- and adds
no project model of its own.

Deliverables (still in `tools/scite/turmeric.properties`):

- A `user.command.*` entry bound to `Tools > New Turmeric Project...` that
  prompts for a destination directory + project name, then runs
  `tur init <name>` (or `tur init --sweet <name>` / `tur init --lib <name>`
  via separate menu entries or a `turmeric.init.flags` user property) in
  the chosen parent directory.
- After scaffolding succeeds, SciTE opens the newly-created
  `<dir>/build.tur` (or `build.tur.sweet`) in a new buffer so the user
  lands in an obviously-editable file. If a `src/main.tur` was generated,
  open it too; recent-files picks up both.
- The same property knobs that govern Run-mode govern Init: a
  `turmeric.init.cmd` property defaults to `tur init` so the day a
  `turi init` (or similar) lands it is a one-line swap, mirroring the
  `turmeric.turi.cmd` pattern.
- The init invocation streams stdout/stderr into the output pane and
  reuses the existing `error.list.regex` -- any `path:line:col:` style
  failure from `tur init` is already clickable.

Acceptance: from a fresh SciTE window with no file open, choose
`Tools > New Turmeric Project...`, type a name, and end up with a
generated project directory, `build.tur` open in the editor, and the
output pane showing the scaffolding log. Pressing Run on `src/main.tur`
immediately works because Phase 1 already routes through `tur --interpret`
with `build.tur` walk-up.

Forward-compat: when run-mode flips to `compiler` later, `tur init` is
unchanged -- the manifest is the same on both paths -- so no extra
plumbing is required at the Phase-6 cutover.

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

### Phase 5 -- Polish + packaging (2-3 days for Win32/Linux; macOS separately)

Packaging splits along the option-A / option-B seam from the embedding
research:

- **Windows + Linux (option A skin):** vendor the upstream SciTE
  source, drop our `turmeric.properties`, Lexilla Turmeric lexer, and
  color theme into the bundle, patch ~50 lines of resource files for
  title/icon/About, build, sign, ship. Output: a Windows `.zip` with
  `Turmeric SciTE.exe` (and an optional installer), a Linux `tar.gz`
  with a `.desktop` entry, and a Debian `.deb`.
- **macOS (option B narrow Cocoa shell):** a separate ~3-5 week
  subproject. Vendor `scintilla/cocoa/` into `vendor/scintilla/`, fork
  the upstream `ScintillaTest` Xcode project as the skeleton, build a
  minimal Cocoa shell (AppDelegate + NSWindowController + split view +
  NSToolbar), reimplement the Turmeric-shaped subset of SciTE features
  on top of it, sign + notarize with a Developer ID, ship a `.dmg`
  containing `Turmeric SciTE.app`.

None of the bundles ship a `tur` binary -- they assume `tur` is on PATH
and surface a clear error in the output pane when it is not.
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
   no C++ changes on Win32 and GTK -- both upstream shells ship
   Lua-enabled by default in current releases. macOS is not affected by
   this question, because the macOS path is a custom Cocoa shell with no
   SciTE/Lua extension surface (Phases 2-4 there talk directly to the
   Scintilla widget's `AutoCShow`/`CallTipShow` C++ API from our shell).
   Net effect: **Lua on Win32 + Linux, native shell calls on macOS** --
   the `tur lsp-lite` RPC backend is the same on all three.
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

Revised after the embedding research; see "Embedding / packaging
research" above for the rationale.

- **Win32 + Linux end-to-end (option A skin):** ~9-13 working days for a
  single developer, the original whole-plan estimate. Phases 1 + 2
  (~5 days) are the smallest viable first cut worth publishing.
- **macOS (option B narrow Cocoa shell):** ~3-5 weeks on top, single
  developer. This is a separate, smaller subproject; it does **not**
  block the Win32/Linux v1.
- **Total three-OS v1:** ~5-7 weeks single-developer.

The Homebrew cask in Phase 5 is ~0.5 day on top once the macOS bundle
exists, assuming the `turmeric` formula already lives in a tap we
control. Until the Cocoa shell lands, the cask is shelved -- not the
Win32/Linux releases.

The smallest publishable artifact is **Phase 1 + 2 on Win32 + Linux**
(~5 days), shipped as re-skinned upstream SciTE with our properties and
lexer. macOS users get the existing "install upstream paid SciTE +
import turmeric" fallback from `tools/scite/README.md` until the Cocoa
shell catches up.
