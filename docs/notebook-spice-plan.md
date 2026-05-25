# Spice Plan: tur-notebook

> **Status:** Draft Plan
> **Last Updated:** 2026-05-24
> **Type:** Spice Design / Tooling

---

## Overview

One new spice for the `turmeric-spices` monorepo:

| Spice | Tag | Depends on | Purpose |
|-------|-----|------------|---------|
| `tur-notebook` | `notebook-v0.1.0` | (none, pure Turmeric + libturi) | Literate-programming format + interactive REPL |

`tur-notebook` is a pair of complementary tools that share one file format:

1. **The format.** A strict superset of CommonMark. A `.turmd` file is plain
   markdown; fenced code blocks tagged `turmeric` or `tursweet` are
   *executable cells*. Everything else is prose.
2. **The renderer.** `tur nb render foo.turmd` evaluates every cell in order
   in a fresh session and emits either:
   - `foo.md` -- the source with each cell followed by an `output` fenced
     block (Quarto / R Markdown pattern), or
   - `foo.html` -- a standalone HTML page (reusing `tools/gendocs.py`'s
     existing CSS so it matches the stdlib docs look).
3. **The interactive TUI.** `tur nb tui foo.turmd` opens the file in a
   terminal UI with **Jupyter-style modal navigation** -- a *command mode*
   for moving between cells and re-running them, and an *edit mode* that
   shells out to `$EDITOR` for the focused cell. State persists across
   re-runs of individual cells (one long-lived interpreter session per TUI
   instance).

The interactive piece is the most consequential design choice in this plan;
see [Interactive design](#interactive-design) below for the rationale and the
alternatives I considered.

`tur-notebook` has no cmake C dependency of its own. It links against
**libturi** for the embedded interpreter (already exposed via the WASM
glue layer), and uses raw ANSI escape sequences for the TUI rather than
pulling in ncurses.

---

## File format

A `.turmd` file is CommonMark with two recognized fence languages:

- ```` ```turmeric ```` -- a Turmeric cell (s-expression syntax).
- ```` ```tursweet ```` -- a sweet-expression cell (`#lang sweet-exp`
  implicit).

Optional cell **attributes** on the fence line follow Quarto's syntax:

````markdown
```turmeric {id=load-data eval=true echo=true output=true}
(import frame/csv :refer [read-csv default-csv-opts])
(def iris (read-csv "iris.csv" (default-csv-opts)))
```
````

Recognized attributes (all optional):

| Attribute | Default | Meaning |
|-----------|---------|---------|
| `id` | auto (`cell-1`, `cell-2`, ...) | Stable cell handle for TUI / errors / re-runs |
| `eval` | `true` | If `false`, the cell is rendered but not executed |
| `echo` | `true` | If `false`, the source is hidden in rendered output (cell still runs) |
| `output` | `true` | If `false`, the output block is suppressed in rendered output |
| `error` | `halt` | `halt` stops on error; `continue` records the error in output and proceeds |
| `cache` | `false` | If `true`, output is cached by source-hash on disk under `.turnb-cache/` |
| `depends` | (none) | Comma-separated list of `id`s this cell needs evaluated first |
| `image` | `inline` | `inline` embeds graphics as base64 PNG in the rendered file; `file` writes a sibling `.png` and links to it |

Unknown attributes are ignored with a warning so future versions can extend
the set without breaking older readers.

### Output blocks

Renderers attach a fenced block tagged `output` immediately after each
executed cell. The block's first line is a metadata comment (`;; cell-id=...
status=ok elapsed=12ms`); the body is captured stdout followed by
the cell's last-expression value as `;; => value`.

````markdown
```output
;; cell-id=load-data status=ok elapsed=42ms
;; => <frame 150x5>
```
````

Errors render with `status=error` and the error message body. Graphics
outputs (when a cell returns a plutovg surface or writes a PNG) render as
either an inline base64 image tag (`![](data:image/png;base64,...)`) or a
sibling-file link, depending on `image=`.

### Why a superset of CommonMark and not a bespoke format

- Existing markdown viewers (GitHub, VS Code preview, Obsidian, pandoc) show
  the file as ordinary markdown with no plugin. Reviewers do not need any
  tooling to read it.
- We do not need to write a custom parser; we feed the file through any
  CommonMark library (a tiny one is vendored as part of NB1) and walk the
  AST for fence nodes whose info string starts with `turmeric` or `tursweet`.
- Round-trip safe: render emits markdown that is itself valid `.turmd`, so
  rendered + re-edited + re-rendered files do not drift.

---

## Conventions

Standard spice layout:

```
spices/notebook/
  build.tur
  src/notebook/
    cmark.h          -- vendored single-file CommonMark parser (md4c)
    format.tur       -- "notebook/format"  parse + serialize .turmd
    cell.tur         -- "notebook/cell"    cell struct, attribute parsing
    session.tur      -- "notebook/session" embedded interpreter session
    eval.tur         -- "notebook/eval"    cell evaluation + output capture
    cache.tur        -- "notebook/cache"   source-hash cache for cache=true cells
    render_md.tur    -- "notebook/rmd"     evaluate + emit .md
    render_html.tur  -- "notebook/rhtml"   evaluate + emit .html (reuses gendocs CSS)
    image.tur        -- "notebook/image"   PNG capture + base64 / sibling-file
    tui.tur          -- "notebook/tui"     interactive TUI driver
    keys.tur         -- "notebook/keys"    key parsing, default bindings
    ansi.tur         -- "notebook/ansi"    cursor / color / clear helpers
    cli.tur          -- "notebook/cli"     argv dispatch for `tur nb ...`
  tests/notebook/
    format_test.tur
    cell_test.tur
    eval_test.tur
    render_md_test.tur
    render_html_test.tur
    cache_test.tur
    tui_test.tur          -- TUI tested via scripted key sequences against a fake terminal
  examples/
    quickstart.turmd
    stats-walkthrough.turmd
    plot-gallery.turmd
```

---

## Architecture

```
                 .turmd file
                      |
                      v
            notebook/format       (md4c parse, fence walk)
                      |
                      v
            notebook/cell list    (id, lang, source, attrs, span)
                      |
        +-------------+--------------+
        |                            |
        v                            v
  notebook/eval                notebook/tui
   (sequential)              (interactive REPL)
        |                            |
        +----> notebook/session <----+
        |     (libturi interpreter)  |
        |                            |
        v                            v
  notebook/render-md         redraw / dispatch
  notebook/render-html
        |
        v
   .md or .html
```

The renderer and TUI both go through `notebook/eval`, which goes through
`notebook/session`, which wraps a single libturi interpreter handle. The
two callers differ only in: (a) which cells to evaluate, (b) how to display
output. State (definitions, imports, refs) is owned by the session and
persists across cells -- exactly like a Jupyter kernel.

---

## CLI

A single binary, `tur-nb`, with subcommands:

```sh
tur nb render foo.turmd                         # writes foo.md beside the source
tur nb render foo.turmd --to html               # writes foo.html
tur nb render foo.turmd --to html --out site/   # write into site/foo.html
tur nb render foo.turmd --cache                 # use .turnb-cache/ for cache=true cells
tur nb render foo.turmd --watch                 # re-render on save (uses inotify/kqueue)

tur nb tui foo.turmd                            # interactive
tur nb tui foo.turmd --no-color                 # disable ANSI colors
tur nb tui foo.turmd --keybindings ~/.turnb-keys # override default keybindings

tur nb exec foo.turmd --cell load-data          # one-shot: run named cell, print output, exit
tur nb exec foo.turmd --all                     # like render, but to stdout, no .md emission

tur nb new foo.turmd                            # scaffold a starter file
```

`tur-nb` is registered as a `bin` in `build.tur` so `tur install tur-notebook`
puts it on `$PATH`.

---

## Interactive design

This is the design decision the user explicitly flagged as open. The
recommendation here is **option B (modal TUI)**, with the static renderer
underneath. Section explains the alternatives and why this is the pick.

### Recommendation: modal TUI over the same renderer

The TUI is a thin layer over `notebook/eval`. It loads the file, renders
the cell list to the terminal, and listens for keystrokes in one of two
modes -- exactly like Jupyter Notebook's classic UI (or `vim`, or `less +F`).

**Command mode** (cell-level navigation; outer loop):

| Key | Action |
|-----|--------|
| `j` / `k` or arrow down/up | Move focus to next / previous cell |
| `gg` / `G` | Jump to first / last cell |
| `Enter` | Re-run the focused cell |
| `Shift-Enter` | Run focused cell, then move focus to next |
| `Ctrl-Enter` | Run focused cell, do not move focus |
| `R` | Restart session and re-run all cells in order |
| `r` | Re-run all cells from focus onward |
| `e` | Edit the focused cell (drops into `$EDITOR`; see below) |
| `a` | Insert a new cell above |
| `b` | Insert a new cell below |
| `dd` | Delete the focused cell (yank to register) |
| `p` | Paste yanked cell below |
| `o` | Toggle output visibility for the focused cell |
| `s` | Save the file (re-serialize with current outputs) |
| `/` | Search across cell sources |
| `n` / `N` | Next / previous search match |
| `?` | Help overlay |
| `q` | Quit (prompts to save if dirty) |

**Edit mode** is intentionally *minimal*. When the user hits `e` in command
mode, the TUI:

1. Writes the focused cell's source to a temp file.
2. Spawns `$EDITOR +<line>` (vim, helix, nano, emacs -- whatever the user
   already has configured).
3. On editor exit, reads the temp file back, replaces the cell source, and
   re-renders.

This avoids building an in-process editor (a substantial body of work with
plenty of edge cases around UTF-8, undo, paste, etc.) and respects the user's
existing keybindings, theme, and plugins. It also keeps the TUI surface small
and easy to test (no text-editing state machine to verify).

**Why modal navigation:**

- Jupyter's command/edit split is the muscle memory most data-science users
  already have; mirroring it lowers the learning curve.
- `vim`-style movement in command mode is the lingua franca of terminal
  tools; everything that scrolls cells (`less`, `man`, `tig`, `lazygit`)
  works this way.
- Modes let us bind single keys to high-frequency actions (`Enter`, `e`, `R`)
  without conflicts -- which is what makes a notebook UI feel snappy.

### Alternatives considered

**A. Editor extensions (VS Code / Neovim / Emacs).**

- *Pros:* WYSIWYG inline rendering, no separate tool to launch, integrates
  with existing dev environment.
- *Cons:* N implementations to maintain (one per editor); each extension has
  its own lifecycle, packaging, and review process; we lose users on editors
  we do not target. The TUI gives us a single binary that works with any
  editor via `$EDITOR`, and a VS Code extension can later be added on top of
  it if demand warrants -- it would just shell out to `tur-nb` for execution.

**B. Pure static renderer (no interactivity at all).**

- *Pros:* Simplest possible scope; the renderer is the entire product.
- *Cons:* The feedback loop (edit -> save -> re-render -> view) is
  intolerable for exploratory work, which is the main use case the user
  described. We do ship the static renderer, but only as a foundation
  beneath the TUI.

**C. Stateless renderer + watch mode.**

- *Pros:* No TUI to maintain; works in any terminal with any editor; clean
  reproducibility story.
- *Cons:* Every change re-runs the *entire* file. For workloads where a
  single early cell is expensive (loading data, fitting a model), this is
  slow. Cache (`cache=true`) helps but does not eliminate the cold-start
  cost. The TUI's per-cell re-run with persistent kernel state is the
  Jupyter UX users expect.

**D. Hybrid (the recommendation): TUI + watch-mode renderer.**

- The TUI is for active exploration (kernel stays warm, you re-run one
  cell at a time, edit via `$EDITOR`).
- `tur nb render --watch` is for "I'm writing prose, just keep the output
  fresh" workflows.
- They share the same parser, evaluator, and session machinery; the
  divergence is purely at the output layer.

### Cell execution semantics in the TUI

- The session is a single Turmeric interpreter that lives for the lifetime
  of the TUI process.
- Re-running a cell does **not** roll back state; it re-evaluates the
  source against the current session, exactly like Jupyter. Users who want
  a clean slate hit `R` (restart and run all).
- Each cell's last-expression value is captured and rendered below the cell
  in a styled output region. Stdout / stderr are captured per-cell.
- If a cell raises, the error is captured into the output region;
  subsequent cell runs continue to work (the session is not torn down).

### File saving

- The file is dirty whenever a cell's source or output differs from disk.
- `s` re-serializes the cell list back to `.turmd`. Cells without outputs
  in memory are written without `output` blocks; cells with outputs are
  written with current outputs (so saving after a run gives you a
  "rendered" file). Users who prefer source-only files can save before
  running, or set `output=false` on cells they want to keep clean.

### Why not a browser-based UI

A browser tab serving cells via WebSockets is the obvious "modern" answer
(it is what Jupyter itself, Marimo, Pluto.jl, and Observable do). It is also
substantially more code: HTTP server, WebSocket protocol, JavaScript front
end, asset pipeline. v0.1.0 ships a terminal tool that fits the rest of the
Turmeric ecosystem (the existing REPL is a CLI). A browser front end is a
natural follow-up spice (`tur-notebook-web`) that reuses `notebook/session`
and `notebook/eval` unchanged.

---

## Style and option types

```turmeric
;;; cell -- one executable code block in a .turmd file.
(defstruct cell
  id          :cstr   ;; user-supplied or auto-generated
  lang        :int    ;; 0 = turmeric, 1 = tursweet
  source      :cstr   ;; cell body (without fence lines)
  attrs       :int    ;; alist of (name . value) cons pairs
  start-line  :int    ;; 1-based line in source file (for error messages)
  end-line    :int)

;;; cell-output -- the result of evaluating one cell.
(defstruct cell-output
  cell-id     :cstr
  status      :int    ;; 0 = ok, 1 = error, 2 = skipped (eval=false)
  stdout      :cstr
  value-repr  :cstr   ;; printed form of last-expression value (or "")
  error-msg   :cstr   ;; error text if status = 1
  elapsed-ms  :int
  image-paths :int)   ;; cons list of :cstr (PNG paths produced during evaluation)

;;; render-opts -- options for notebook/render-md and notebook/render-html.
(defstruct render-opts
  output-dir     :cstr  ;; 0 (nil ptr) = same dir as source
  use-cache      :int   ;; 0 = off, 1 = on
  image-mode     :int   ;; 0 = inline (base64), 1 = sibling file
  include-source :int   ;; 0 = hide all cell sources, 1 = honor each cell's echo attr
  fail-fast      :int)  ;; 0 = continue on error (record in output), 1 = stop and exit nonzero

(default-render-opts)   ;; => render-opts with sensible defaults

;;; tui-opts -- options for notebook/tui.
(defstruct tui-opts
  color           :int  ;; 0 = monochrome, 1 = 256-color (default)
  cell-width      :int  ;; 0 = auto from terminal, else fixed
  scrollback      :int  ;; lines of output to keep per cell (default 1000)
  keybindings-file :cstr) ;; 0 = built-in defaults

(default-tui-opts)
```

---

## Modules and exports

### notebook/format

```turmeric
;; Parse a .turmd file into a list of nodes: text spans and cell structs
;; preserve original document order.
(parse-file path)                          ;; => result<list<node>>
(parse-string s)                           ;; => result<list<node>>

;; node-tag => 0 (prose chunk, :cstr) or 1 (cell struct)
(node-tag n)                               ;; => :int
(node-prose n)                             ;; => :cstr   (when tag = 0)
(node-cell n)                              ;; => cell    (when tag = 1)

;; Serialize back to .turmd source.
(nodes->str nodes)                         ;; => :cstr

;; Update a single cell's source in place inside a node list (returns a new
;; list; node lists are immutable).
(nodes-update-cell nodes cell-id new-source)
                                           ;; => result<list<node>>
```

---

### notebook/cell

```turmeric
;; Parse the attribute block from a fence info string like "turmeric {id=foo
;; eval=false}".  Returns an alist of (name . value).
(parse-attrs s)                            ;; => alist<:cstr :cstr>

(attr-get attrs name default)              ;; => :cstr
(attr-get-bool attrs name default)         ;; => :int  0 or 1
(attr-get-int attrs name default)          ;; => :int

;; Build a cell from its parts (used by format parser and TUI insert).
(cell-make id lang source attrs start-line end-line)
                                           ;; => cell

;; Auto-assign ids to cells whose id attribute is missing.
(cells-assign-ids cells)                   ;; => list<cell>
```

---

### notebook/session

```turmeric
;; Open a fresh interpreter session.  Returns an opaque handle.
(session-open)                             ;; => session :int
(session-close s)                          ;; => :void

;; Evaluate a string in the session.  Captures stdout, stderr, last-expr value.
;; Returns a cell-output (with cell-id = "") suitable for display.
(session-eval s source lang)               ;; => cell-output
;; lang = 0 (turmeric) or 1 (tursweet)

;; Reset the session (drops all definitions; equivalent to close + open).
(session-reset s)                          ;; => :void
```

`session-open` initializes a libturi interpreter and installs an output
capture hook (a small inline-C bridge that redirects `printf` and friends
into a per-cell `tur-frame`-style growable buffer; flushed when the cell
finishes).

---

### notebook/eval

```turmeric
;; Evaluate one cell against the session.  Honors cell attrs (eval, error,
;; cache, depends).  Returns the cell-output, ready to render.
(eval-cell session cell cache)             ;; => cell-output

;; Evaluate every cell in document order.  Returns a parallel list of outputs.
(eval-all session cells cache opts)        ;; => list<cell-output>

;; Evaluate from a given cell id forward (used by TUI's 'r' key).
(eval-from session cells start-id cache opts)
                                           ;; => list<cell-output>
```

---

### notebook/cache

```turmeric
;; Cache key = SHA-256 of (cell source + sorted attribute list + list of
;; previous-cell hashes for cells in `depends`).  Stored under .turnb-cache/.
(cache-open dir)                           ;; => cache :int
(cache-close c)                            ;; => :void

(cache-get c key)                          ;; => option<cell-output>
(cache-put c key output)                   ;; => :void
(cache-clear c)                            ;; => :void
```

---

### notebook/render-md and notebook/render-html

```turmeric
;; Evaluate and emit foo.md beside foo.turmd (or under opts.output-dir).
(render-md path opts)                      ;; => result<:cstr>   (path written)

;; Render to HTML using the same CSS as the stdlib API docs.
(render-html path opts)                    ;; => result<:cstr>

;; In-memory variants (for tests, scripting, the TUI's preview pane).
(render-md-string nodes outputs opts)      ;; => :cstr
(render-html-string nodes outputs opts)    ;; => :cstr
```

The HTML renderer uses `tools/gendocs.py`'s existing CSS (already used for
the stdlib docs and guides) so notebook output blends into the docs site.
It also injects the syntax-toggle widget already present in guides, so
turmeric / tursweet cells of the same content can be displayed side-by-side
when both are provided.

---

### notebook/image

```turmeric
;; Hook for spices that produce images (tur-plot, tur-plutovg).  The TUI
;; and renderers install this hook on the session; when the cell calls
;; (plot-write-png ...) or similar, the path is captured for the output.
(image-hook-install session)               ;; => :void
(image-hook-take session)                  ;; => list<:cstr>  (and clears)

;; For inline images: read a PNG and produce a base64 data URL.
(png->data-url path)                       ;; => result<:cstr>
```

Spices opt in by calling `image-hook-record-path` from their save-to-disk
paths (a tiny addition documented in the integration guide). Nothing
breaks if a spice does not opt in -- it just means the user has to write
the PNG explicitly and include it via markdown image syntax.

---

### notebook/tui

```turmeric
;; Launch the interactive TUI on a .turmd file.
;; Returns the process exit code (0 = clean quit, 1 = quit with unsaved changes after warning).
(tui-run path opts)                        ;; => result<:int>

;; Lower-level entry: open with an already-parsed node list (used by tests).
(tui-run-nodes nodes opts)                 ;; => result<:int>
```

---

### notebook/keys

```turmeric
;; Default key bindings as an alist of (key-string . action-symbol-cstr).
(default-keybindings)                      ;; => alist<:cstr :cstr>

;; Load user keybindings from a file (one "key action" per line, # = comment).
(load-keybindings path)                    ;; => result<alist>

;; Action dispatcher.
(action-dispatch tui-state action)         ;; => tui-state
```

Actions are named strings (`"cell-next"`, `"cell-edit"`, `"run-from-here"`,
`"quit"`, ...) so users can rebind without source changes.

---

### notebook/ansi

A handful of helpers; this module exists so the TUI can be written without
caring about escape-code minutiae.

```turmeric
(ansi-clear-screen)                        ;; => :void
(ansi-move-to row col)                     ;; => :void
(ansi-set-fg color-idx)                    ;; => :void
(ansi-set-bg color-idx)                    ;; => :void
(ansi-reset)                               ;; => :void
(ansi-bold) (ansi-italic) (ansi-underline) ;; => :void

(term-size)                                ;; => (cons rows cols)
(term-enable-raw)                          ;; => :void
(term-disable-raw)                         ;; => :void
(term-read-key)                            ;; => :cstr  ("j", "<C-Enter>", "<Up>", ...)
```

---

### notebook/cli

```turmeric
;; Argv dispatch.  Returns the process exit code.
(cli-main args)                            ;; => :int
;; args = cons list of :cstr (program name omitted; matches *args*)
```

Subcommands: `render`, `tui`, `exec`, `new`. Unknown subcommand prints help
and exits 2.

---

## Implementation phases

- [ ] **NB0** -- `build.tur`; vendor `md4c` (~2k lines, single .c / .h, MIT)
  as `cmark.h` and a one-file companion `cmark.c`; `notebook/format` parse
  and serialize round-trip on the `examples/quickstart.turmd` fixture.

- [ ] **NB1** -- `notebook/cell` (attribute parser, defaults, id assignment);
  golden-file tests against fenced blocks with all known attributes.

- [ ] **NB2** -- `notebook/session` wrapping libturi (open, eval, close, reset);
  stdout / stderr capture via the inline-C output-redirect hook;
  `notebook/eval` (eval-cell, eval-all, eval-from) with `eval`, `echo`,
  `output`, `error` attributes honored.

- [ ] **NB3** -- `notebook/cache` (SHA-256-keyed disk cache under
  `.turnb-cache/`); `cache=true` and `depends=` attributes honored;
  invalidation test: changing an upstream cell busts every downstream cell
  that lists it in `depends`.

- [ ] **NB4** -- `notebook/render-md` writing valid round-trippable `.md`;
  `notebook/cli render` subcommand; `--watch` flag using `kqueue` on Darwin
  and `inotify` on Linux (small inline-C bridge per OS).

- [ ] **NB5** -- `notebook/render-html` using the gendocs CSS; image embedding
  (base64 inline and sibling-file modes); syntax-toggle wrapping for
  turmeric+tursweet sibling cells. Verify a rendered notebook looks
  consistent with the stdlib docs (visual diff against a fixture page).

- [ ] **NB6** -- `notebook/ansi` (raw mode, key reading, cursor moves);
  `notebook/keys` (default bindings, file loader, action dispatch);
  `notebook/tui` with command mode navigation (`j`, `k`, `gg`, `G`, `Enter`,
  `Shift-Enter`, `R`, `r`, `s`, `q`). No edit mode yet.

- [ ] **NB7** -- TUI edit mode: `e` spawns `$EDITOR` on a temp file, reads
  it back, updates the cell, re-renders. Insert / delete / paste (`a`, `b`,
  `dd`, `p`). File-dirty detection and save prompt on quit.

- [ ] **NB8** -- TUI search (`/`, `n`, `N`); help overlay (`?`); output
  toggle (`o`); search across both cell sources and output text. User
  keybindings override file (`--keybindings`).

- [ ] **NB9** -- `notebook/image` hook; integration glue documented for
  `tur-plot` and `tur-plutovg`; inline graphics show in the TUI's output
  region via the Kitty / iTerm2 image protocol when the terminal supports
  it, sixel where available, or a `[image: path]` placeholder otherwise.

- [ ] **NB10** -- `notebook/cli exec`, `notebook/cli new` (starter file
  scaffold); end-to-end tests for every subcommand; README in
  `turmeric-spices`; `docs/guides/notebook-guide.md`; `notebook-v0.1.0` tag.

---

## Design notes

### Why one binary with subcommands

The renderer and TUI share parser, evaluator, cache, and session code; the
divergence is small enough that two binaries would mostly duplicate setup.
A single `tur-nb` with subcommands matches `git`, `cargo`, `gh`, and is the
shape Turmeric users already know from the main `tur` driver.

### How the TUI captures output

Each cell evaluation installs a stdout capture hook on the session, runs
the source, and reads back the captured buffer. The hook is implemented in
inline-C as a `fflush` + `freopen` to a `tmpfile()` per cell -- robust
across `printf`, `fprintf(stdout, ...)`, and inline-C `puts`. Stderr is
captured the same way. Last-expression value comes from libturi's
existing REPL eval entry point, which already returns a printable form.

### Why no in-process editor

Building a real text editor (multi-line, UTF-8, undo, paste, syntax-aware
indent) is on the order of a thousand lines of careful code, and we would
forever be re-inventing decisions the user has already made in their
`vim`/`helix`/`emacs` config. Shelling out to `$EDITOR` is the same pattern
used by `git commit`, `crontab -e`, `gh pr edit`, and `visidata`. It cleanly
inherits the user's plugins, theme, and muscle memory.

### Why mode-based and not chord-based

Modal navigation (Jupyter, vim, less) gives us single-key bindings for the
high-frequency actions (`Enter`, `e`, `R`). A chord-based scheme (`Ctrl-X
Ctrl-E`) would be more keystrokes per action and would conflict with
`$EDITOR`'s own bindings once the user is inside it. The mode itself is
indicated by a one-character cue in the status bar (`-- COMMAND --` /
`-- EDITING (cell-id) --`) so users always know which mode they are in.

### Reproducibility and seeds

Notebooks that use randomness (any cell calling into `tur-stats`'s `rng-*`
or any future PRNG) should pass an explicit seed. The notebook tooling
does not auto-seed. Reasons: implicit auto-seeding would make notebooks
that *look* reproducible silently non-reproducible the moment they are
edited; explicit `(rng-make 42)` in user code is unambiguous. The
notebook guide spells this out.

### Why HTML render reuses gendocs CSS

The stdlib API reference, guides, and (per `per-spice-docs-plan.md`) spice
docs all share one stylesheet. Notebook output naturally belongs in the
same visual family -- a rendered analysis is documentation. Reusing the
CSS is free and avoids a parallel style system.

### Watch-mode rendering

`--watch` calls `notebook/render-md` (or `--to html`) once on launch, then
listens for write events on the source file via `kqueue` / `inotify`. On
each event it debounces 150 ms (handles editors that write atomically via
rename) and re-renders. State across renders is *not* shared -- each
re-render is a fresh session -- because watch mode is for "I'm writing
prose, just keep the rendered file fresh" and predictability matters more
than warm caches. For warm caches use the TUI.

---

## Risks and open questions

1. **Output capture and inline-C.** Capturing stdout via `freopen(tmpfile)`
   is portable but interferes with anything that holds a `FILE*` to the
   original stdout. We document that cells must not stash `stdout` across
   cells (a contrived case, but worth calling out).

2. **Terminal image protocols are not universal.** Kitty, iTerm2, WezTerm,
   Konsole, and (sometimes) Alacritty all use different escape sequences
   for inline images; many terminals support none. NB9's fallback chain
   (Kitty graphics -> sixel -> placeholder) leaves users on plain xterm
   without inline images; they see `[image: foo-cell-3.png]` and can open
   the file. The renderer (`tur nb render`) produces fully-portable HTML or
   markdown-with-base64, so the TUI's limitation does not affect shareable
   artifacts.

3. **Session restart cost.** `R` (restart and re-run) re-imports every
   spice the notebook touches; for large dependency trees this can take
   seconds. Acceptable for v0.1.0; a longer-term optimization is to fork
   from a snapshot taken at session-open, but Turmeric's interpreter
   does not currently expose that hook.

4. **md4c bundling.** md4c is small (~2k LoC, single C file, MIT) but it
   is the first vendored C file in a spice that is more than a header. We
   could instead parse CommonMark in Turmeric directly. The decision in
   this plan is to vendor; the implementation phase NB0 includes a check
   that the file size and license remain acceptable.

5. **Multi-cell undo.** v0.1.0 has no undo for cell-level edits (insert,
   delete, paste). The save-on-quit prompt protects against accidental
   data loss; full undo is tracked for v0.2.

---

## Shared work

### turmeric-spices README

Add row once v0.1.0 is tagged:

| Spice | Description | Tier | Depends on |
|-------|-------------|------|------------|
| `tur-notebook` | Literate `.turmd` notebooks: renderer (md/html) + interactive TUI | 1 -- pure Turmeric | (none) |

### Guide

Deliver `docs/guides/notebook-guide.md` alongside the `v0.1.0` tag. Sections:

1. Your first `.turmd` (cells, attributes, running it)
2. Rendering: markdown vs HTML, watch mode
3. The TUI: command mode tour, editing via `$EDITOR`, restart vs partial re-run
4. Caching expensive cells (`cache=true`, `depends=`)
5. Embedding plots and images
6. Reproducibility: seeds, frozen outputs, `tur nb exec` in CI
7. Customizing keybindings

### Integration notes

- Pair with `tur-frame` for data loading cells (`read-csv` is the prototypical
  expensive cell that benefits from `cache=true`).
- Pair with `tur-stats` for reproducible analyses (deterministic seeds make
  notebook outputs byte-stable across re-renders, so diffs in version control
  are meaningful).
- Pair with `tur-plot` for inline figures (NB9 image hook is wired through
  `plot-write-png` automatically; in the TUI on a supporting terminal,
  figures display inline below the cell).
- The `per-spice-docs-plan.md` HTML pipeline can ingest rendered `.turmd`
  files as guide pages; this is a follow-up integration once both plans
  land.

---

## Future spices

| Spice | Purpose | Why separate |
|-------|---------|--------------|
| `tur-notebook-web` | Browser-based notebook UI (WebSockets + `tur-notebook` core) | Different runtime model; large JS asset surface |
| `tur-notebook-vscode` | VS Code extension wrapping `tur-nb` | Editor-specific lifecycle, marketplace packaging |
| `tur-notebook-export` | Export to PDF (via wkhtmltopdf or weasyprint) and `.ipynb` | Pulls heavy external tools |
| `tur-notebook-slides` | RevealJS-backed slide output (`tur nb render --to slides`) | Different layout engine |
