# LSP Client Gaps Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-07-27
> **Type:** Tooling / `tur lsp`

---

## Overview

Every item here was found by writing a real LSP client against `tur lsp` —
[Trowel](https://github.com/rjungemann/trowel), the native Turmeric editor —
and hitting something that had to be worked around in the client rather than
fixed in the server. That is the value of the list: these are not speculative
gaps, they are places where a shipping consumer already carries compensating
code, with the workaround pinned to a file and line.

Paths marked **(trowel)** are in the Trowel repository; unmarked paths are in
this one.

A first tranche has already landed on the `lsp-phase2` branch (commit
`36572dda8`). Trowel does not benefit from it yet: it bundles a pinned
prebuilt toolchain, so every fix here needs a Turmeric release plus a bump of
`TROWEL_TURMERIC_VERSION` and its three per-arch SHA-256s before a user sees
it.

---

## 1. Done — landed on `lsp-phase2`, awaiting a release

| Gap | Client workaround it retires |
|---|---|
| Full recompile ran inline on every `didChange` | 250ms debounce in `lsp_manager.cpp:21` **(trowel)** |
| `character` was a byte offset but nothing said so | `kPositionEncoding = Utf8` in `lsp_position.h:16` **(trowel)** |
| `\uXXXX` was passed through with the backslash dropped | *none possible* — see below |
| A request with missing `params` got no response at all | *none possible* — the client simply hung |

The `\uXXXX` bug deserves a note because it had no client-side mitigation and
was invisible until measured. `unescape_json` turned `é` into the five
literal characters `u00e9`, which both corrupted the text and shifted every
byte offset after it. For `(def a "é") (def later 2)`, `later` was reported at
character 21 instead of 18 — and every diagnostic, hover, and definition later
in the file drifted by the same amount. Any file containing a single non-ASCII
character in a string literal had silently wrong positions from that point on.

---

## 2. Completion is the weakest surface

Ordered by how much they hurt in practice.

### 2.1 Completion requires a *successful* compile — the big one

`on_completion` serves symbols gathered by `run_doc_analysis`, which collects
them from a full compile. A buffer that does not parse yields no symbols, so
completion returns nothing.

The problem is that *not parsing is the normal state while typing*. The moment
a user types `(`, or `(foo`, the buffer is unbalanced and completion goes
silent — precisely when it is wanted. Verified directly: appending `\n(smoke`
to an otherwise valid file drops completion from 200 items to zero.

No client can work around this. Proposals, cheapest first:

1. **Retain the last good symbol set** per document and serve completions from
   it when the current parse fails. A few stale entries beat an empty list, and
   the symbol table changes far more slowly than the text does.
2. **Error-tolerant parsing** for the symbol-collection pass — recover at the
   next top-level form rather than abandoning the file.

(1) is a small change with most of the benefit and should land first.

### 2.2 Returns nothing at offset 0

Completion at `{"line": 0, "character": 0}` returns an empty array while the
same document at the end of the buffer returns 200 items. A client that
requests completions immediately after opening a file — the obvious thing to
do — gets nothing and cannot tell that from "no matches".

### 2.3 Returns a bare `CompletionItem[]`, not a `CompletionList`

Legal per the specification, but the asymmetry means every client needs the
both-shapes branch that Trowel carries at `lsp_manager.cpp:347` **(trowel)**.
Worth emitting a `CompletionList` for uniformity.

### 2.4 Advertises a space trigger character that is too expensive to honor

`completionProvider.triggerCharacters` is `["(", " "]`. In a lisp, space fires
on nearly every keystroke, and each trigger forced a `didChange` plus a full
compile. Trowel honors `(` only and binds the rest to an explicit key
(`editor_view.cpp:57` **(trowel)**). Now that analysis is debounced this is
less costly, but the advertised trigger set should still match what is actually
affordable.

### 2.5 Capped at 200 items, in document order

Clients that present a sorted list must sort and dedupe themselves
(`editor_view.cpp:368` **(trowel)**). Prefix-filtering server-side would be
more useful than a raw cap, which currently truncates the buffer's *own*
symbols behind 200 stdlib entries.

---

## 3. Missing protocol surface

### 3.1 No `$/cancelRequest`

The server is single-threaded and synchronous, so a slow analysis blocks every
request behind it with no way to abandon one. Trowel compensates with 1.5s
timeouts, a single-in-flight cap, and a per-document generation counter that
drops replies which arrive after the buffer moved on (`lsp_client.h:57`
**(trowel)**). Debouncing reduced the exposure but did not remove it.

### 3.2 No `textDocument/formatting`

`tur format` exists and works; it is simply not reachable over LSP. Both known
clients therefore shell out to it:

- VS Code, via `execSync` in `vscode-syntax-ext/extension.js:11-34`.
- Trowel, via a **blocking** `QProcess` in `main_window.cpp:936-945`
  **(trowel)** that can freeze the UI for up to ten seconds.

Wiring the existing formatter to a `textDocument/formatting` handler removes a
real UI stall and deletes duplicated client code.

### 3.3 No `textDocument/signatureHelp`

No argument hints while typing a call. The calltip logic already exists,
unwired, in `src/cli/lsp_lite.c` (`calltip` method) — this is mostly a matter
of exposing what is already written.

### 3.4 No semantic tokens

Every editor must hand-write and maintain a Turmeric lexer. Trowel's is 447
lines (`scanner_turmeric.cpp` **(trowel)**) and will drift from the language as
it evolves.

### 3.5 `workspace/symbol` covers open documents only

`on_workspace_symbol` iterates the open-document store, so there is no
project-wide symbol search and no cross-file go-to-definition for a file the
user has not already opened. This also shapes client architecture: Trowel runs
one shared server for the whole application rather than one per window,
because a per-window `rootUri` currently buys nothing.

---

## 4. Smaller papercuts

- **Analysis is path-based.** `run_doc_analysis` writes the buffer to a temp
  file and remaps diagnostics back, so a document needs a real filesystem path.
  Trowel therefore gives unsaved buffers no language support at all
  (`lsp_manager.cpp:76` **(trowel)**) — a visible gap for a new file.
- **Zero-width diagnostic ranges.** Some diagnostics report `start == end`,
  which paints nothing; clients must widen them to stay visible.
- **Hover returns fenced markdown** even when the client advertises only
  `plaintext` in `hover.contentFormat`. Trowel strips the fences
  (`editor_view.cpp:382` **(trowel)**).
- **Full-document sync only** (`textDocumentSync: 1`). `on_did_change` reads
  only `contentChanges[0].text` and would silently mishandle ranges if a client
  sent them. Lower priority now that analysis is debounced.
- **Analysis writes to hardcoded `/tmp`.** Fine on Unix, wrong on Windows, and
  it means one temp file per analysis.

---

## 5. Not LSP, but same origin

Found the same way — a real client hitting real behavior.

- **OSC 133;A prompt markers** are absent from older `tur` and when not on a
  tty, leaving a client unable to tell busy from idle. Trowel latches "busy"
  forever as the safe default (`repl_session.h:63` **(trowel)**).
- **The REPL reports `getcwd()`**, which resolves symlinks (`/private/var/…` on
  macOS) while the client's own path does not, so naive comparison never
  matches. Trowel compares canonically (`repl_session.cpp:52` **(trowel)**).
- **No way to evaluate a region or a string.** To run a selection, Trowel
  writes a scratch file and sends `(load "…")` (`run_buffer.cpp:45`
  **(trowel)**). A `:load-string` style meta-command would remove the
  round-trip through disk.
- **`TUR_STDLIB_DIR` leaks from the ambient environment** and can pair a new
  `tur` with an old stdlib. Both of Trowel's subprocess launchers defensively
  pin it to the `stdlib/` sitting next to the resolved binary.

---

## 6. Suggested order

1. **Retain last-good symbols** (§2.1) — small, and fixes the most damaging
   day-to-day behavior.
2. **`textDocument/formatting`** (§3.2) — the formatter exists; this removes a
   UI freeze and duplicated client code.
3. **Completion at offset 0 and prefix filtering** (§2.2, §2.5).
4. **`signatureHelp`** (§3.3) — logic already written in `lsp_lite.c`.
5. **`$/cancelRequest`** (§3.1) — needs the request loop restructured; largest
   change here.
6. Everything in §4 as opportunistic cleanup.

Semantic tokens (§3.4) and workspace-wide symbols (§3.5) are larger features
rather than gap-filling, and are deliberately left unscheduled.
