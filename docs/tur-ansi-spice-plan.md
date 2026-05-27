# Spice Plan: tur-ansi

> **Status:** Draft Plan
> **Last Updated:** 2026-05-25
> **Type:** Spice Design / Tooling
> **Depends on:** [notebook-spice-plan.md](notebook-spice-plan.md) (phases NB8--NB11 must be complete before extraction)

---

## Overview

One new spice for the `turmeric-spices` monorepo:

| Spice | Tag | Depends on | Purpose |
|-------|-----|------------|---------|
| `tur-ansi` | `ansi-v0.1.0` | (none, pure Turmeric + inline-C) | Lightweight ncurses alternative: ANSI terminal control, raw-mode key input, color, style |

`tur-ansi` is a standalone spice extracted from the terminal-control code
written for `tur-notebook`. The notebook plan notes that it uses "raw ANSI
escape sequences for the TUI rather than pulling in ncurses"; this plan
promotes those modules from notebook-private helpers into a first-class,
reusable terminal library that any spice or application can depend on.

The extraction work is gated on `tur-notebook` reaching at least phase NB11
(where `notebook/ansi`, `notebook/keys`, and the TUI image protocol code are
fully implemented and battle-tested). Extracting from working code -- rather
than specifying in the abstract -- ensures the API is shaped by real usage.

After `tur-ansi` is tagged, `tur-notebook` is refactored to import it as a
declared `:spices` dependency and delete its own copies of the modules that
moved.

**No C library dependency.** All terminal control is done through ANSI/VT100
escape sequences written to stdout / read from stdin. Raw-mode setup
(`tcgetattr` / `tcsetattr`) uses a small inline-C shim -- no ncurses link.

---

## Why a separate spice

`tur-notebook` needs terminal control. So does any future TUI written in
Turmeric (a `tur-tui` framework, a `tur-http` progress display, a
`tur-sqlite` interactive query shell, etc.). Keeping these utilities inside
`notebook/` would force every future TUI to re-implement them or copy-paste
from notebook. Extracting them into `tur-ansi` gives:

- **One implementation to test.** Edge cases in key parsing (multibyte
  sequences, modifier-key prefixes, mouse events) are subtle. One test suite
  is better than N copies.
- **Stable, versioned API.** Spices can pin to `ansi-v0.1.0` and upgrade
  when they choose.
- **Dependency inversion for tur-notebook.** The notebook's `notebook/tui`
  keeps its application-level logic (cell navigation, session dispatch,
  dirty-flag management); the raw terminal machinery lives in `tur-ansi`.

---

## Scope

`tur-ansi` covers everything needed to write a cursor-addressable, full-color
terminal UI in pure Turmeric:

| Area | Included | Deferred |
|------|----------|---------|
| Cursor movement (absolute + relative) | yes | -- |
| Cursor visibility (show/hide) | yes | -- |
| Alternate screen buffer | yes | -- |
| Terminal size query + SIGWINCH subscription | yes | -- |
| 4-bit colors (classic 16) | yes | -- |
| 8-bit colors (256-color palette) | yes | -- |
| 24-bit true-color | yes | -- |
| Named color themes | v0.2 | -- |
| Text styles (bold, italic, underline, blink, reverse, dim) | yes | -- |
| Screen / line clear | yes | -- |
| Save / restore cursor position | yes | -- |
| Raw mode + terminal setup/teardown | yes | -- |
| Key reading (blocking) | yes | -- |
| Key name normalization | yes | -- |
| Mouse event parsing | v0.2 | -- |
| Bracketed paste | v0.2 | -- |
| Inline images (Kitty / iTerm2 / sixel) | extracted from NB11 | -- |
| Box-drawing characters | yes | -- |
| ANSI capability detection (no-color env, `$TERM` query) | yes | -- |

---

## Conventions

Standard spice layout:

```
spices/ansi/
  build.tur
  src/ansi/
    cursor.tur     -- "ansi/cursor"  move-to, relative moves, show/hide
    color.tur      -- "ansi/color"   4-bit, 8-bit, 24-bit fg/bg; reset
    style.tur      -- "ansi/style"   bold, italic, underline, blink, dim, reverse
    screen.tur     -- "ansi/screen"  clear screen/line, alternate buffer, save/restore cursor
    term.tur       -- "ansi/term"    raw mode, terminal-size query, capability detection
    keys.tur       -- "ansi/keys"    blocking key read, multibyte sequence parsing, key-name strings
    image.tur      -- "ansi/image"   Kitty / iTerm2 / sixel inline image protocol; fallback string
    box.tur        -- "ansi/box"     box-drawing character constants and border-drawing helpers
    ansi.tur       -- "ansi/ansi"    re-exports the full public API (single import for callers)
  tests/ansi/
    cursor_test.tur
    color_test.tur
    style_test.tur
    screen_test.tur
    term_test.tur
    keys_test.tur
    image_test.tur
    box_test.tur
  examples/
    hello-color.tur     -- 256-color gradient demo
    keys-dump.tur       -- print normalized key names as you type (exit: Ctrl-C)
    box-demo.tur        -- nested bordered panels
    image-demo.tur      -- display a PNG inline using best available protocol
```

---

## Architecture

```
  tur-ansi public surface ("ansi/ansi" re-export)
        |
        +------ ansi/cursor  (ANSI CSI sequences -> stdout via write())
        +------ ansi/color   (SGR fg/bg color sequences)
        +------ ansi/style   (SGR attribute sequences)
        +------ ansi/screen  (clear, alt-screen, save/restore cursor)
        +------ ansi/term    (tcgetattr/tcsetattr via inline-C; TIOCGWINSZ)
        +------ ansi/keys    (read() loop; multibyte escape-sequence parser)
        +------ ansi/image   (Kitty/iTerm2/sixel inline image encoding)
        +------ ansi/box     (Unicode box-drawing constants; border helpers)
```

All eight modules are independent at the source level (no intra-module
imports). `ansi/ansi.tur` provides a single `(import ansi/ansi :refer [...])`
entry point for callers who want the whole library.

---

## Module API

### ansi/cursor

```turmeric
;;; cursor-move-to -- move the terminal cursor to an absolute position (1-based).
(cursor-move-to row col)               ;; => :void

;;; cursor-up / cursor-down / cursor-left / cursor-right -- relative moves.
(cursor-up n)                          ;; => :void
(cursor-down n)                        ;; => :void
(cursor-left n)                        ;; => :void
(cursor-right n)                       ;; => :void

;;; cursor-show / cursor-hide -- toggle cursor visibility.
(cursor-show)                          ;; => :void
(cursor-hide)                          ;; => :void

;;; cursor-save / cursor-restore -- DEC SC/RC.
(cursor-save)                          ;; => :void
(cursor-restore)                       ;; => :void

;;; cursor-col -- move to column, preserving row (CSI <n> G).
(cursor-col col)                       ;; => :void
```

---

### ansi/color

```turmeric
;;; fg4 / bg4 -- 4-bit (classic 16) foreground / background colors.
;;; color-idx: 0-7 standard, 8-15 bright (or 60-67 on some terminals).
(fg4 color-idx)                        ;; => :void
(bg4 color-idx)                        ;; => :void

;;; Named 4-bit constants.
(color-black)   (color-red)     (color-green)  (color-yellow)
(color-blue)    (color-magenta) (color-cyan)   (color-white)
(color-bright-black) ...                       ;; => :int  (0-15)

;;; fg8 / bg8 -- 8-bit 256-color palette.
(fg8 idx)                              ;; => :void   idx: 0-255
(bg8 idx)                              ;; => :void

;;; fg24 / bg24 -- 24-bit true-color (r g b each 0-255).
(fg24 r g b)                           ;; => :void
(bg24 r g b)                           ;; => :void

;;; color-reset -- restore default fg + bg.
(color-reset)                          ;; => :void
```

---

### ansi/style

```turmeric
(style-bold)          ;; => :void
(style-dim)           ;; => :void
(style-italic)        ;; => :void
(style-underline)     ;; => :void
(style-blink)         ;; => :void
(style-blink-fast)    ;; => :void
(style-reverse)       ;; => :void
(style-strikethrough) ;; => :void
(style-reset)         ;; => :void   -- clear all attributes (SGR 0)
```

---

### ansi/screen

```turmeric
;;; screen-clear -- erase the entire screen and move cursor to (1,1).
(screen-clear)                         ;; => :void

;;; screen-clear-to-eol -- erase from cursor to end of current line.
(screen-clear-to-eol)                  ;; => :void

;;; screen-clear-line -- erase the entire current line.
(screen-clear-line)                    ;; => :void

;;; alt-screen-enter / alt-screen-leave -- switch to / from the alternate
;;; screen buffer (the "smcup" / "rmcup" terminfo entries).
(alt-screen-enter)                     ;; => :void
(alt-screen-leave)                     ;; => :void

;;; scroll-up / scroll-down -- scroll viewport by n lines.
(scroll-up n)                          ;; => :void
(scroll-down n)                        ;; => :void
```

---

### ansi/term

```turmeric
;;; term-size -- query the terminal dimensions via TIOCGWINSZ.
;;; Returns (cons rows cols).  Falls back to (cons 24 80) if unavailable.
(term-size)                            ;; => (cons :int :int)

;;; term-enable-raw / term-disable-raw -- toggle raw mode via tcsetattr.
;;; term-enable-raw saves the original termios; term-disable-raw restores it.
(term-enable-raw)                      ;; => :void
(term-disable-raw)                     ;; => :void

;;; term-color-support -- detect color capability from $COLORTERM / $TERM.
;;; Returns 0 = no color, 1 = 4-bit, 2 = 8-bit, 3 = 24-bit true-color.
(term-color-support)                   ;; => :int

;;; term-no-color? -- true when $NO_COLOR is set (https://no-color.org/).
(term-no-color?)                       ;; => :int

;;; term-image-protocol -- detect inline image support.
;;; Returns 0 = none, 1 = kitty, 2 = iterm2, 3 = sixel.
(term-image-protocol)                  ;; => :int

;;; term-on-resize -- register a callback invoked when the terminal is resized.
;;; The callback receives no arguments; call (term-size) inside it to get the
;;; new dimensions.  Pass 0 to remove the current callback.
;;;
;;; Internally, term-enable-raw installs a SIGWINCH handler that writes one
;;; byte to a self-pipe; the term-read-key select() loop also monitors the
;;; pipe read-end and delivers a "<Resize>" pseudo-event, after which the
;;; callback (if any) is called before returning.
(term-on-resize callback)              ;; => :void
```

The `tcgetattr`/`tcsetattr`/`TIOCGWINSZ`, `sigaction(SIGWINCH)`, and
`pipe()`/`select()` calls are all bridged via a small inline-C block in
`term.tur`. No external C library is needed.

---

### ansi/keys

```turmeric
;;; term-read-key -- block until one keypress, return a normalized key string.
;;;
;;; Examples: "a", "A", " ", "<Enter>", "<Escape>", "<Up>", "<C-a>",
;;;           "<S-Tab>", "<C-S-Enter>", "<F1>", "<Backspace>", "<Delete>".
(term-read-key)                        ;; => :cstr

;;; term-read-key-timeout -- like term-read-key but returns "" after ms millis.
(term-read-key-timeout ms)             ;; => :cstr

;;; key-name->bytes -- inverse of the normalizer; produces the raw escape
;;; sequence for a given key string (useful for scripted terminal tests).
(key-name->bytes key-name)             ;; => :cstr

;;; key=? -- compare two normalized key strings.
(key=? a b)                            ;; => :int

;;; key-ctrl? / key-shift? / key-alt? -- test modifier flags in a key string.
(key-ctrl? k)                          ;; => :int
(key-shift? k)                         ;; => :int
(key-alt? k)                           ;; => :int
```

Key parsing handles:
- Single ASCII bytes (printable + control)
- ANSI CSI sequences: `\e[A` (Up), `\e[1;5A` (Ctrl-Up), `\e[Z` (Shift-Tab), etc.
- SS3 sequences: `\eOP` (F1) through `\e[24~` (F12)
- xterm modifier extensions (modifyOtherKeys)
- Kitty keyboard protocol keys (when the terminal negotiates it)

Unknown / unrecognized sequences are returned as `"<raw:XX...>"` hex strings so
they are always printable without discarding input.

---

### ansi/image

```turmeric
;;; image-display -- display a PNG file inline using the best available protocol.
;;; Falls back to a bracketed "[image: path]" string if no protocol is supported.
(image-display path)                   ;; => :void

;;; image-display-base64 -- display a PNG from a base64-encoded string.
(image-display-base64 b64)             ;; => :void

;;; image-display-protocol -- display using a specific protocol (0=kitty, 1=iterm2, 2=sixel).
;;; Caller is responsible for having checked (term-image-protocol) first.
(image-display-protocol proto path)    ;; => :void

;;; image-placeholder -- write the fallback string for terminals without image support.
(image-placeholder path)               ;; => :void
```

This module is the inline-image code from `tur-notebook` NB11, extracted
verbatim and re-exported with a stable API. `tur-notebook` imports
`ansi/image` after the extraction refactor.

---

### ansi/box

```turmeric
;;; Box-drawing character constants (Unicode, rendered as :cstr).
(box-tl-single)  (box-tr-single)  (box-bl-single)  (box-br-single)
(box-h-single)   (box-v-single)
(box-tl-double)  (box-tr-double)  (box-bl-double)  (box-br-double)
(box-h-double)   (box-v-double)
(box-tl-round)   (box-tr-round)   (box-bl-round)   (box-br-round)
;; => :cstr   single UTF-8 character each

;;; box-draw -- draw a bordered rectangle at (row col) of size (height width).
;;; style: 0 = single, 1 = double, 2 = round-corners.
(box-draw row col height width style)  ;; => :void

;;; box-fill -- fill the interior of a rectangle with a character (or space).
(box-fill row col height width ch)     ;; => :void

;;; box-title -- write a title string centered on the top border of a box.
;;; Truncates with "..." if the title is wider than the box.
(box-title row col width title)        ;; => :void
```

---

### ansi/ansi (re-export facade)

```turmeric
;; One-stop import: (import ansi/ansi :refer [...]) gives access to every
;; symbol in all seven modules above without separate per-module imports.
;;
;; The module has no code of its own; it is a pure re-export list.
```

---

## Implementation phases

- [ ] **AN0** -- `build.tur`; `ansi/term` (raw mode + terminal size, inline-C
  bridge for `tcgetattr` / `tcsetattr` / `TIOCGWINSZ`; `term-color-support`
  reading `$COLORTERM` / `$TERM`; `term-no-color?` reading `$NO_COLOR`;
  SIGWINCH self-pipe: `pipe()` + `sigaction(SIGWINCH)` in `term-enable-raw`,
  `select()` on stdin + pipe read-end in `term-read-key`, `term-on-resize`
  callback registration, `"<Resize>"` pseudo-event);
  `ansi/cursor` (all cursor-move functions, show/hide, save/restore via
  ANSI escape sequences); smoke-test: `examples/hello-color.tur`.

- [ ] **AN1** -- `ansi/color` (4-bit, 8-bit, 24-bit fg/bg; named constants;
  `color-reset`); `ansi/style` (all SGR attributes; `style-reset`);
  `ansi/screen` (screen clear, line clear, alternate buffer enter/leave,
  scroll); tests asserting the correct escape byte sequences are emitted
  (redirect stdout to a buffer via the same capture hook as `tur-notebook`).

- [ ] **AN2** -- `ansi/keys`: raw key reading loop; multibyte CSI/SS3 escape
  sequence state machine; modifier extension parsing; normalized key-name
  strings; `term-read-key-timeout`; `key-name->bytes` inverse;
  `examples/keys-dump.tur`. Tests cover a representative sample of the
  CommonTerminal key table (at least all arrow keys, F1-F12, Ctrl+letter,
  Shift-Tab, Enter, Backspace, Delete, Escape).

- [ ] **AN3** -- `ansi/box`: Unicode box-drawing constants; `box-draw`;
  `box-fill`; `box-title`; `examples/box-demo.tur`. `ansi/ansi` re-export
  facade assembled from AN0-AN3; all tests passing.

- [ ] **AN4** -- `ansi/image`: Kitty inline-image encoding (base64 chunked);
  iTerm2 protocol; sixel encoder for 256-color fallback; `term-image-protocol`
  detection in `ansi/term`; `examples/image-demo.tur`; `ansi-v0.1.0` tag.

- [ ] **AN5** -- **tur-notebook refactor**: replace `notebook/ansi` and the
  key-reading part of `notebook/keys` with imports from `tur-ansi`; update
  `notebook/image` to import `ansi/image`; delete the moved source files;
  all existing notebook tests pass unchanged (they test behavior, not module
  boundaries). Update `tur-notebook`'s `build.tur` to declare `tur-ansi` as
  a `:spices` dependency.

---

## Relationship to tur-notebook

The dependency arrow after AN5 is:

```
tur-notebook  -->  tur-ansi
```

Before AN5, `tur-notebook` owns its own copies of the ansi/key/image code
(as delivered by notebook phases NB8--NB11). AN5 is the extraction commit:
the notebook copies are deleted, imports are updated, and `tur-ansi` becomes
a declared dependency. No notebook API changes; only the source location of
the terminal utilities moves.

The split also means `tur-ansi` can be versioned independently: a breaking
change to the key-name format (say, adopting Kitty keyboard protocol names
across the board) ships as `ansi-v0.2.0`, and `tur-notebook` can stay on
`ansi-v0.1.0` until it is ready to update.

---

## Design notes

### Why not ncurses

ncurses is a C library. Linking it in means every spice that imports `tur-ansi`
pulls in a cmake C dependency, which:

- Upgrades the spice from Tier 1 (pure Turmeric + inline-C) to Tier 2
  (cmake-dep), breaking the "no build system config" story for users who just
  want a colored prompt.
- Adds ~500kB of library code for a use case that needs ~200 lines of ANSI
  escape sequences and one `tcsetattr` call.
- Couples library authors to ncurses's ABI (which varies between macOS/Linux
  and between ncurses / ncursesw builds).

Raw ANSI/VT100 sequences work on every terminal emulator in use today
(xterm, iTerm2, WezTerm, Kitty, GNOME Terminal, Windows Terminal). The only
genuine capability gap is the absence of the ncurses terminfo fallback chain
for antique terminals -- which no Turmeric user is running.

### Key-name normalization convention

All key names use angle-bracket notation for non-printing keys: `<Enter>`,
`<Up>`, `<F1>`, `<C-a>`, `<S-Tab>`. Plain ASCII printable characters are
returned as-is: `"a"`, `"A"`, `" "`, `"!"`. This matches the convention used
by Neovim and Helix so that users copy key names from those editors' docs
without translation.

### Inline-C footprint

The only inline-C in the spice is in `ansi/term.tur`:

- `term-enable-raw` / `term-disable-raw`: `tcgetattr` / `tcsetattr` with the
  standard "raw mode" flag sequence (disable `ECHO`, `ICANON`, `ISIG`; set
  `VMIN=1`, `VTIME=0`); also `pipe()` + `sigaction(SIGWINCH, ...)` on
  enable, and `sigaction` restore + `close()` on disable.
- `term-size`: `ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)`.
- `term-read-key` / `term-read-key-timeout`: `select(2, ...)` on both
  `STDIN_FILENO` and the pipe read-end + `read()`.

Everything else -- escape sequence emission, sequence parsing, image protocol
encoding -- is pure Turmeric operating on strings and integers. The inline-C
blocks are each fewer than 20 lines.

### SIGWINCH (resize events)

Resize events are delivered via the **self-pipe trick** so they integrate
naturally with the existing `select()`-based key-reading loop:

1. `term-enable-raw` opens a pipe (two fds, stored in a module-level global)
   and calls `sigaction(SIGWINCH, ...)` to install a signal handler that
   writes one byte (`'R'`) to the write-end of the pipe.
2. The `select()` call inside `term-read-key` (and `term-read-key-timeout`)
   waits on both `STDIN_FILENO` and the pipe read-end.
3. When the pipe becomes readable, the implementation drains the byte, calls
   the registered callback (if any), and returns `"<Resize>"` to the caller.
4. `term-disable-raw` restores the original `SIGWINCH` disposition via
   `sigaction` and closes both pipe fds.

Callers that care about resize handle it exactly like any other key event:

```turmeric
(let [k (term-read-key)]
  (if (key=? k "<Resize>")
    (handle-resize (term-size))
    (dispatch-key k)))
```

Callers that prefer a callback register it with `term-on-resize`; it fires
before `term-read-key` returns `"<Resize>"`, so either style works.

The self-pipe approach is fully POSIX and identical on macOS and Linux --
no `kqueue EVFILT_SIGNAL` or Linux-only `signalfd` required. The signal
handler is async-signal-safe (`write(2)` is on the async-safe list).

---

## Shared work

### turmeric-spices README

Add row once `ansi-v0.1.0` is tagged:

| Spice | Description | Tier | Depends on |
|-------|-------------|------|------------|
| `tur-ansi` | Lightweight ncurses alternative: ANSI color, cursor, raw-mode key input, inline images | 1 -- inline-C only | -- |

### Guide

Deliver `docs/guides/ansi-guide.md` alongside `ansi-v0.1.0`. Sections:

1. Your first colored output (5 lines with `ansi/color` + `ansi/style`)
2. Cursor-addressable rendering: moving the cursor, clearing regions
3. Raw mode and key input: reading keystrokes one at a time
4. Box drawing and layouts
5. Inline images: Kitty, iTerm2, sixel, and the fallback
6. Building a minimal full-screen TUI (worked example)
7. No-color and accessibility: respecting `$NO_COLOR`

---

## Risks and open questions

1. **Sixel encoder size.** A correct sixel encoder that handles the 256-color
   palette and dithering is on the order of 300--500 lines of Turmeric. If
   it grows excessively the sixel path can be deferred to v0.2 and AN4 ships
   with Kitty + iTerm2 only plus the text placeholder fallback.

2. **Windows Terminal.** VT processing is available on Windows 10+ via
   `ENABLE_VIRTUAL_TERMINAL_PROCESSING`, but `tcgetattr` / `TIOCGWINSZ` are
   not available. The inline-C shim in `ansi/term` would need a
   `#ifdef _WIN32` branch using `GetConsoleScreenBufferInfo` /
   `SetConsoleMode`. Deferred: Turmeric itself does not currently target
   Windows. If that changes, `ansi/term` is the only module that needs porting.

3. **Kitty keyboard protocol negotiation.** Full Kitty protocol support
   requires sending `\e[?u` to query, reading the response, and adjusting key
   parsing. This is useful (it gives us unambiguous modifier keys for
   Ctrl-Shift-letter etc.) but adds state to `term-enable-raw`. Deferred to
   v0.2; v0.1.0 falls back to xterm modifier extensions which cover the
   tur-notebook use case.

4. **Coordinating AN5 with tur-notebook development.** If `tur-notebook` is
   still in active development when AN3/AN4 lands, the AN5 refactor will
   require merging across active feature branches in `turmeric-spices`. Simplest
   mitigation: cut `ansi-v0.1.0` first, land AN5 as a follow-up PR against the
   notebook branch before notebook's own tag is cut, so both ship together.
