# Try Turmeric: minimap, outline, and navigable definitions

> **Status:** Executed (2026-08-28) -- M0-M5 and F1 landed, and verified in CI
> on rjungemann/turmeric#787. One item cannot be closed here: the C-interpreter
> host is behind an egress policy that denies `*.turmeric-lang.com`. See
> [§10 Execution record](#10-execution-record-2026-08-28), including §10.5 on
> two spec suites that were passing by not running.
> **Type:** Web playground / Monaco / `src/lsp`
> **Related:** [`try-turmeric-lsp-plan.md`](../archive/try-turmeric-lsp-plan.md)
> (executed 2026-07-29),
> [`lsp-client-gaps-plan.md`](../archive/lsp-client-gaps-plan.md) (executed),
> [`try-turmeric-multi-tab-and-projects-plan.md`](../archive/try-turmeric-multi-tab-and-projects-plan.md)
> **Reference implementation:** `~/Projects/c2mir-playground/c2mp/vite-wasm`

---

## 0. Summary

The ask is to bring four things c2mp has to Try Turmeric: a minimap, mouse-over
LSP, go-to-definition, and an outline. Reading both codebases first changes the
shape of the work considerably, and in Try Turmeric's favor.

**Three of the four are already implemented and unreachable.** Try Turmeric runs
the real `tur lsp` server inside a second WASM instance (`web/public/lsp-worker.js`)
behind a hand-rolled Monaco adapter (`web/lsp-client.js`) that already registers
hover, definition, documentSymbol, completion, signature help, and diagnostics
providers. What is missing is not language intelligence. It is:

- a config flag (`minimap: { enabled: false }`, `web/main.js:1625`),
- a UI surface for a provider that answers correctly but has nothing to render
  into (documentSymbol),
- and one early return that gives up at the stdlib boundary
  (`web/lsp-client.js:458-463`).

**c2mp is the opposite case**, and that is why its code is worth reading but not
worth porting. c2mp has no language server -- c2mir exports no symbol table, so
`vite-wasm/src/symbols.js` is 862 lines of hand-rolled C scanning -- and no
Monaco, so `vite-wasm/src/minimap.js` is 443 lines of canvas painting over a
textarea. Every one of those lines exists to reach a floor Monaco and `tur lsp`
are already above. What c2mp is worth reading for is its *design decisions*,
which are recorded in its comments and which this plan adopts: blocks not glyphs
at minimap scale, diagnostics on an outboard ruler where they stay visible,
marks as washes rather than fills, an outline built on open rather than kept in
sync, and -- the load-bearing one, stated at `vite-wasm/src/minimap.js:342` --
that a decoration which fails must remove itself rather than degrade the editor.

**Do not port `minimap.js` or `symbols.js`.**

This plan also carries one unrelated item the same request named: a
"Try our C Interpreter" link in the footer (§7).

---

## 1. What exists (repo facts)

### 1.1 Try Turmeric, today

| Capability | State | Anchor |
|---|---|---|
| Monaco 0.45, full `editor.api` entry, dynamically imported | shipping | `web/main.js:3068` |
| LSP worker (second WASM instance, lazy on first editor focus) | shipping | `web/public/lsp-worker.js` |
| Monaco<->JSON-RPC adapter, UTF-8/UTF-16 column conversion | shipping | `web/lsp-client.js` |
| Diagnostics -> `setModelMarkers` | shipping | `web/lsp-client.js:229` |
| Completion, signature help, formatting | shipping | `web/lsp-client.js:355,411` |
| **Hover provider** | registered | `web/lsp-client.js:392` |
| **Definition provider** | registered, gives up off-tab | `web/lsp-client.js:443` |
| **DocumentSymbol provider** | registered, **no UI reads it** | `web/lsp-client.js:476` |
| **Minimap** | **disabled** | `web/main.js:1625` |
| **Overview ruler** | **disabled** (`overviewRulerLanes: 0`) | `web/main.js:1670` |
| `occurrencesHighlight: true` | set, but no server provider backs it | `web/main.js:1668` |
| Analysis status pill | shipping | `web/main.js:384` |
| Toolbar popover pattern (`.more-menu`, aria-wired) | shipping | `web/try/index.html:60-135` |

Server side, `src/lsp/lsp.c:695-719` advertises `hoverProvider`,
`definitionProvider`, `documentSymbolProvider`, `workspaceSymbolProvider`,
`documentFormattingProvider`, `signatureHelpProvider`, `completionProvider`.
It does **not** advertise `documentHighlightProvider` or semantic tokens.

### 1.2 c2mp, and what it is for here

| c2mp file | What it does | Turmeric equivalent |
|---|---|---|
| `src/minimap.js` (443) | canvas minimap over a textarea | Monaco's built-in minimap |
| `src/symbols.js` (862) | hand-rolled C symbol index | `tur_collect_symbols` via `tur lsp` |
| `src/editor.js:486-586` | hover card + placement | Monaco hover + `registerHoverProvider` |
| `src/main.js:416-467` | **Symbols dropdown (the outline)** | **nothing -- this is the port** |
| `index.html:93-104` | Symbols button markup | maps onto the existing `.more-menu` pattern |

The outline is the one place where c2mp has a real surface Try Turmeric lacks,
and it is 50 lines of DOM. Everything else is c2mp reaching a floor we are past.

---

## 2. The gaps, precisely

### 2.1 Minimap -- off by one flag, plus theme colors

`web/main.js:1625` sets `minimap: { enabled: false }`, and `web/main.js:1670`
sets `overviewRulerLanes: 0`, which is what suppresses the diagnostic ticks in
the right-hand strip -- exactly the affordance c2mp builds by hand
(`minimap.js:45`, `RULER_W`, and the comment at `:280` explaining why a
diagnostic gets an outboard tick and an occurrence does not).

Turning both on is the whole feature. What is *not* free:

- **Theme colors.** `turmeric-dark` and `turmeric-light` are defined at
  `web/main.js:1491,1522` and neither sets `minimap.background`,
  `minimapSlider.*`, `minimapGutter.*`, or `editorOverviewRuler.*`. Without
  them the strip renders with VS Code defaults against a Turmeric palette.
- **Narrow panes.** The playground is a split view and ships a mobile layout
  (`isMobileSplit()`, `web/main.js:2759`). A minimap on a 390px editor is
  taking a fifth of the code column to show three characters of shape.

### 2.2 Hover -- works, with one likely hole

`on_hover` (`src/lsp/lsp.c:821`) resolves the word under the cursor against the
document's symbol index and returns fenced ```` ```(name : type)``` ```` plus the
docstring. The adapter renders it (`web/lsp-client.js:392-409`).

The hole: `find_symbol` (`src/lsp/lsp.c:126`) scans only what
`tur_collect_symbols` put in this document's index, and `collect_binding`
(`src/lsp/lsp_collect.c:16`) keeps only non-synthesized globals. Whether a
stdlib name -- `println`, `map`, `cons` -- lands in that index depends on whether
the collector walks imported modules' globals, and the answer is not readable
from the source without running it. If it does not, `on_hover` returns
`{"contents":""}` (`src/lsp/lsp.c:855`), `hoverText` yields `''`, and the
provider returns null (`web/lsp-client.js:402`) -- meaning hover is silent on
exactly the names a first-time visitor types most.

**This is a measurement, not a claim.** Phase M0 probes it. If the hole is real
it is the highest-value item in this plan, and the fix is cheap: the page
already ships the entire stdlib docstring table (`web/public/doc-names.json`,
`web/public/docs-pack/`, driving the docs pane), so the adapter can fall back to
it when the server has nothing.

### 2.3 Go-to-definition -- dies at the stdlib boundary

`on_definition` (`src/lsp/lsp.c:898`) returns a real `Location` built from
`sym->file_path` via `lsp_path_to_uri`. Inside the WASM bundle the stdlib is
preloaded into MEMFS at `/stdlib`, so a stdlib symbol resolves to something like
`file:///stdlib/list.tur`.

The client then looks that URI up in the tab strip (`tabForUri`,
`web/lsp-client.js:141`), finds no tab, and returns null -- with a comment
saying so at `web/lsp-client.js:459-462`: *"A definition in the stdlib, which
has no tab. There is nothing to open, and inventing a read-only buffer for it
is a bigger feature than this plan."*

This plan is that plan. The blocker is narrow and specific:

- **`FS` is not exported from the WASM bundle.** `src/CMakeLists.txt:1563` sets
  `-sEXPORTED_RUNTIME_METHODS=stringToUTF8,UTF8ToString,lengthBytesUTF8`. The
  worker cannot read `/stdlib/list.tur` today. Two ways out, in §5.4.

Also missing regardless of the stdlib question: F12, and a way back. Monaco
standalone registers `editor.action.revealDefinition` but ships no navigation
history service, so "jump, then return" needs a small stack of our own.

### 2.4 Outline -- a provider with no surface

`registerDocumentSymbolProvider` is wired (`web/lsp-client.js:476`) and the
server answers (`src/lsp/lsp.c:967`). Nothing in the page ever renders the
result. Monaco standalone has no outline pane and no breadcrumbs; those are VS
Code workbench features.

Two things follow, one of them free:

- **`editor.action.quickOutline` is already registered.** `web/main.js:3068`
  does `await import('monaco-editor')`, the full `editor.api` entry, which pulls
  `editor.all.js` and with it `standaloneGotoSymbolQuickAccess.js`
  (confirmed present in `web/node_modules/monaco-editor/esm/`). Ctrl/Cmd+Shift+O
  works in the shipped playground right now. It is simply undiscoverable.
- **The kind mapping is binary.** `lsp_symbol_kind` (`src/lsp/lsp.c:960`) maps
  anything whose `type_str` does not start with `(fn` to `Variable`. A
  `defstruct`, a `defmacro`, a `definstance` and a `def` are indistinguishable
  in the list. c2mp's outline labels six kinds (`symbols.js:752`) and the labels
  are most of what makes the list scannable.

### 2.5 Occurrence highlight -- set but unbacked

`occurrencesHighlight: true` (`web/main.js:1668`) needs a
`DocumentHighlightProvider`. The server advertises none, so Monaco falls back to
its word-based selection highlight -- textual, so it matches inside strings and
comments. c2mp's `occurrencesOf` (`symbols.js:731`) deliberately filters those,
with the reasoning at `:721`: *"`total` inside `subtotal`, inside a comment, or
inside a string literal is not a use of `total`, and a regular expression over
the source cannot tell."*

This matters more once the minimap is on, because c2mp paints occurrence marks
into the strip and Monaco will too -- a wrong answer becomes a visible wrong
answer down the whole file.

### 2.6 Explicitly out of scope

- Semantic tokens. Monaco keeps the Monarch tokenizer
  (`lsp-client-gaps-plan.md` §3.4, still unscheduled).
- Rename, code actions, inlay hints -- the server has none.
- A docked outline pane. The playground is already a three-way split plus a docs
  overlay; a fourth region is a layout project, not this.
- Porting c2mp's `minimap.js` or `symbols.js`. See §0.
- Editing stdlib buffers. Read-only, always.

---

## 3. Phases

| Phase | Scope | Depends on |
|---|---|---|
| **M0** | Measurement spike (timeboxed, ~1h) | -- |
| **M1** | Minimap + overview ruler + theme colors + narrow-pane gating | -- |
| **M2** | Outline: discoverable quick-outline, then the Symbols dropdown | -- |
| **M3** | Hover fallback to the stdlib docstring table | M0 |
| **M4** | Go-to-definition into read-only stdlib tabs; F12; jump-back stack | M0 |
| **M5** | Server-side polish: symbol kinds, `documentHighlight` | M1, M2 |
| **F1** | "Try our C Interpreter" footer link (independent) | -- |

M1, M2 and F1 touch no C and need no WASM rebuild, so they ship on their own.
M4 and M5 change `src/` and therefore need a bundle regen -- see §6.

---

## 4. M0 -- measurement spike

Everything downstream branches on facts that are cheaper to measure than to
reason about. Run against the deployed playground, or a local `tur run web-dev`,
and record the answers **in this document** before starting M3 or M4.

1. **Does hover answer on a stdlib name?** Open the default buffer, hover
   `println`. Record: real content / empty string / null. (Decides whether M3 is
   a fallback or a no-op.)
2. **Does go-to-definition return a stdlib Location?** In the browser console:
   `window._turiLsp` is already exposed (`web/main.js:442`). Send a
   `textDocument/definition` on a stdlib name and record the `uri` -- a
   `/stdlib/...` path, some other path, or null. (Decides M4's whole shape: if
   the server returns null there is nothing to open and M4 shrinks to F12 plus
   the jump-back stack.)
3. **Does `documentSymbol` return anything for a multi-definition buffer?**
   Record the kinds. (Confirms M2's list has content and quantifies §2.4's
   binary-kind problem.)
4. **Minimap cost.** Flip `minimap.enabled` in devtools on a 500-line buffer and
   watch for input latency. Expected: none -- Monaco's minimap is already in the
   bundle and already optimized -- but c2mp's `minimap.js:15` is explicit that
   nothing about a minimap belongs on the typing path, and the check is a
   minute.

Exit criteria: four recorded answers. If (1) and (2) both come back rich, M3
collapses to nothing and M4 shrinks; that is a good outcome and the spike is
what reveals it.

### 4.1 Results (2026-08-28)

Measured against the **real `tur lsp`** driven over stdio from a scripted
session, not against the deployed page. That is the same server the WASM
bundle hosts -- `src/web/wasm_lsp.c` differs only in where the stdlib is
mounted, and it resolves that through `TUR_STDLIB_DIR`
(`src/web/wasm_lsp.c:71-81`), so every answer below carries over except the
literal stdlib path, noted where it matters. Buffer used:

```turmeric
(defn twice [x : int] : int (* x 2))
(defstruct Point [x : int y : int])
(def eight 8)
(def p (pair 1 2))
(println (twice 21))
```

**1. Does hover answer on a stdlib name? Split, and §2.2 had the seam in the
wrong place.**

| Hover on | Answer |
|---|---|
| `pair` (stdlib module global) | ``` ```\n(pair : (fn [tyvar tyvar] : (type-app ? ?)))\n``` ``` |
| `twice` (buffer-local) | ``` ```\n(twice : (fn [int] : int))\n``` ``` |
| `println` | `{"contents":""}` |

The collector **does** walk imported modules' globals -- everything defined in
a `.tur` file under `stdlib/` lands in the document's index and hovers
correctly. The hole is one layer down: **compiler builtins have no `Binding`
at all**, so `collect_binding` never sees them. `println` is a row in
`src/compiler/builtins.c:120-133` (one per argument type), not a `defn`.
`+`, `-`, `=`, `not`, `mod` and the rest of `builtins.c` are the same.

This matters more than the original framing, because `doc-names.json` is
generated from stdlib `;;;` docstrings and therefore **does not carry
`println` either**. The M3 fallback as designed cannot close this hole:

- M3 (client, docs table) covers documented names the index misses -- spice
  symbols, and any buffer whose analysis failed hard enough to leave the
  stdlib cache empty. Worth shipping, and it ships with no rebuild.
- The builtin hole needs the **server**, which already owns the only table
  that has the answer. Added to M5 as §5.5's third item.

**2. Does go-to-definition return a stdlib Location? Yes, a real one.**

```
definition on `pair`    -> {"uri":"file:///home/user/turmeric/stdlib/pair.tur",
                            "range":{"start":{"line":27,"character":6}, ...}}
definition on `twice`   -> {"uri":"file:///project/main.tur", ...}
definition on `println` -> null            (same builtin hole as (1))
```

Under WASM the same symbol resolves to `file:///stdlib/pair.tur`, because
`--embed-file .../stdlib@/stdlib` (`src/CMakeLists.txt:1550`) mounts the tree
at `/stdlib` and `wasm_lsp.c` defaults `TUR_STDLIB_DIR` to it.

**M4 is full-shape, not the shrunken version §9 hedged for.** There is a real
file at a real path with a real range, and the read-only tab has something to
put in it.

**3. Does `documentSymbol` return anything, and what kinds?** Four entries for
the buffer above, and §2.4's binary-kind problem is exactly as described:

| Name | Defined by | Kind returned |
|---|---|---|
| `twice` | `defn` | 12 (Function) |
| `Point` | `defstruct` | **13 (Variable)** |
| `eight` | `def` | 13 (Variable) |
| `main` | implicit top-level wrapper | 12 (Function) |

A `defstruct` and a `def` are indistinguishable in the list, which is most of
what M2's kind column is for. M5's kind field is what fixes it.

Note the fourth row: the implicit `main` the elaborator wraps top-level forms
in is a real entry in the index, with a range covering the last top-level
form. It is not synthesized in the `is_synthesized` sense, so `collect_binding`
keeps it. The outline should not hide it -- it *is* where those forms run --
but M2 sorts by position so it lands where the code is.

**4. Minimap cost.** Not measured in a browser -- this container has no
`emcc`, so `web/public/turmeric.{js,wasm}` cannot be regenerated here and the
playground cannot be driven end to end. What is checkable was checked:
`monaco-editor`'s minimap is part of the `editor.api` entry the page already
imports (§2.4), so enabling it adds no bytes, and Monaco renders it off the
typing path on its own scheduler. The M1 code adds nothing to the input path
either -- the width gate runs from `applySplit` and a `ResizeObserver`, never
from `onDidChangeModelContent`. Re-check on a real bundle before the release
cut §6 requires anyway.

**Consequences for the phase list.**

- M3 stays, with its coverage stated honestly (documented names the index
  misses), and stops being described as the fix for `println`.
- M4 keeps its full shape.
- M5 grows a third item: a compiler-builtin fallback in the server, which is
  the actual fix for the measured hole and the highest-value item here.

---

## 5. Design notes

### 5.1 M1 -- minimap

In `web/main.js:1625`:

```js
minimap: {
    enabled: true,
    // Blocks, not glyphs. c2mp reached the same conclusion the hard way
    // (minimap.js:8-13): no browser hints glyphs at 2px, so rendered
    // characters are platform-dependent mush. Blocks are legible and cheap.
    renderCharacters: false,
    showSlider: 'mouseover',
    size: 'proportional',
    maxColumn: 80,
},
```

and at `web/main.js:1670`, `overviewRulerLanes: 3`. That single change is what
puts error and warning ticks in the right-hand strip. Keep
`overviewRulerBorder: false`.

**Theme colors.** Extend both `defineTheme` calls (`web/main.js:1491,1522`) with:
`minimap.background`, `minimap.errorHighlight`, `minimap.warningHighlight`,
`minimap.selectionHighlight`, `minimapSlider.background` /
`.hoverBackground` / `.activeBackground`, `minimapGutter.addedBackground` and
siblings, and `editorOverviewRuler.errorForeground` /
`.warningForeground` / `.infoForeground`. Source the values from the existing
palette in `web/vars.css` -- c2mp resolves its mark colors out of CSS custom
properties for exactly this reason (`minimap.js:204-209`): a hardcoded hex here
silently stops matching the first time a token color moves.

**Narrow panes.** Gate on measured editor width rather than a media query, since
the split handle (`web/main.js:2779`) can make the editor narrow on a desktop:

- Below ~600px of editor width, `editor.updateOptions({ minimap: { enabled: false } })`.
- Re-evaluate from the same place the split already recomputes layout, and from
  `applySplit` (`web/main.js:2762`).

**User toggle.** Add a row to the existing More menu
(`#more-btn` / `#more-menu`, `web/try/index.html:145-152`), persisted through
`safeWrite`/`safeRead` (`web/main.js:124,134`). Precedence: an explicit user
choice wins; the width gate only applies when the user has not chosen. A visible
toggle also means someone who dislikes the strip is not stuck with it, which is
the cheapest possible answer to "should this be on by default".

**Failure contract.** Monaco's minimap is a supported component and is not
expected to throw. c2mp's rule (`minimap.js:342`) still applies as a review
criterion for anything we add *around* it -- the width gate, the toggle, the
theme extension: if any of it throws, the editor must keep working.

### 5.2 M2 -- outline

**M2a, one line.** Add a `Symbols` button to the editor toolbar
(`web/try/index.html`, alongside `#examples-btn`) that runs:

```js
editor.getAction('editor.action.quickOutline').run();
```

Add Ctrl/Cmd+Shift+O to the title attribute. This ships an outline in an hour
because the action is already registered (§2.4).

**M2b, the dropdown.** Model it on c2mp's (`vite-wasm/src/main.js:416-467`,
markup at `vite-wasm/index.html:93-104`) and build it out of the popover pattern
`web/try/index.html:60-135` already uses for Examples and Language -- same
`.more-menu` class, same `aria-haspopup` / `aria-expanded` / `aria-controls`
wiring, same outside-click close. There is nothing new to design.

- **Fill on open, not on change.** c2mp's reason (`main.js:427`) holds exactly:
  the buffer changes on every keystroke and this list is read once in a while.
  Call `provideDocumentSymbols` from the click handler.
- **Rows** carry the name plus a right-aligned kind label, from
  `SYMBOL_KIND_NAMES` (`web/lsp-client.js:29`) -- c2mp's `KIND_LABEL`
  (`symbols.js:752`) is the phrasing to copy: what you would say out loud about
  the entry (`function`, `type`, `macro`), not the LSP enum name.
- **Mark the entry containing the caret** `is-current`. Port c2mp's rule
  verbatim (`symbols.js:775-795`): the name wins when the caret is actually on
  one, otherwise the smallest containing range wins -- because someone editing
  the middle of a function is not on its name, and the point of the marker is to
  answer "where am I".
- **Click jumps** with the same two calls `onNavigate` already makes
  (`web/main.js:432-436`): `revealRangeInCenterIfOutsideViewport` then
  `setPosition`.
- **Empty state:** "Nothing defined yet" (c2mp `main.js:435`). Also needed: a
  distinct state for when the LSP is unavailable, since the playground must stay
  exactly as usable when analysis fails (`web/lsp-client.js:13-16`). Do not show
  an empty outline that implies an empty file.
- **Mobile:** the dropdown *is* the outline. No docked pane.

The provider already flushes pending edits before answering
(`flushPendingChanges`, `web/lsp-client.js:298`), so an outline opened
mid-keystroke describes the current buffer rather than one two characters ago.
Nothing to add.

### 5.3 M3 -- hover fallback

Only if M0 (1) shows the hole. The adapter gains an optional hook:

```js
createLspClient({ ..., lookupDoc: (name) => ({ signature, docstring }) | null })
```

`provideHover` (`web/lsp-client.js:392`) tries the server first and falls back to
`lookupDoc` when `hoverText(result.contents)` is empty. `main.js` supplies the
hook from the same docstring table the docs pane already loads
(`web/public/doc-names.json` / `web/public/docs-pack/`), so no new asset ships
and no new fetch happens on the hover path -- if the table is not loaded yet,
the hook returns null and hover behaves exactly as it does today.

Mark fallback content visibly as coming from the docs rather than from analysis
(a trailing `-- stdlib docs` line, or the module name). A hover that silently
mixes "the checker says this" with "the manual says this" is worse than either.

Two things c2mp's hover does that we should **not** copy:

- Its diagnostic line (`editor.js:505`). Monaco already renders marker messages
  in its own hover; adding ours would double them.
- Its async second-line inferred type (`editor.js:556`). `tur lsp` already puts
  the checked type in the first line. There is no second source to wait for.

### 5.4 M4 -- definitions into the stdlib

Contingent on M0 (2) returning a real `/stdlib/...` URI.

**Reading the file.** `FS` is not exported (`src/CMakeLists.txt:1563`). Two
options:

1. **Add `FS` to `EXPORTED_RUNTIME_METHODS`.** One-word CMake change; widens the
   bundle's runtime surface and lets any JS in the page walk the virtual
   filesystem.
2. **Export `turi_wasm_read_file(const char *path)`** returning a malloc'd
   string, matching the shape every other export already has -- the worker's
   `callStringToString` helper (`web/public/lsp-worker.js:22`) drives it with no
   new marshaling code, and the worker gains a `{type:'read-file', path}`
   message.

**Recommend (2).** It is the same amount of work, it keeps the exported surface
a list of named operations rather than a filesystem, and it matches the existing
transport exactly. It must return `null` for a path outside `/stdlib` -- an
export that reads arbitrary MEMFS paths on request from the page is a wider
capability than "show me where `map` is defined" needs.

**Read-only tabs.** `main.js` gains `openReadOnlyTab(name, text)`:

- Model created with the tab machinery already in place (`ensureModel`,
  `web/main.js:245`), editor put in `readOnly` for that tab.
- A lock affordance in the tab strip (`renderTabs`, `web/main.js:466`).
- **Excluded from `buildProjectEntries()`** (`web/main.js:2118`) so a downloaded
  project zip does not carry copies of the stdlib.
- **Excluded from `persistTabs`**, so a reload does not restore stdlib buffers
  the user never opened deliberately.
- Reused, not duplicated: opening `list.tur` twice focuses the existing tab.
- Closable like any other tab.

Then delete the early return at `web/lsp-client.js:458-463` and replace it with
an `onOpenExternal(uri, range)` callback, keeping the existing `onNavigate` path
for in-workspace targets untouched.

**Keyboard.** Bind F12 to `editor.action.revealDefinition` (Cmd/Ctrl+click
already works through the provider). Monaco standalone ships no navigation
history, so add a small stack: push `{tabId, position}` in `onNavigate` /
`onOpenExternal`, pop on Ctrl+Alt+- (Cmd+Alt+- on macOS), cap it at ~20 entries.
Say plainly in the toolbar tooltip what the binding is; an undiscoverable
jump-back is the same as none.

### 5.5 M5 -- server-side polish

Both items are in `src/` and are worth doing for Trowel as much as for the web.

- **Real symbol kinds.** `lsp_symbol_kind` (`src/lsp/lsp.c:960`) says as much
  itself: *"A `;;;` `defstruct` / `defmacro` distinction is not preserved in
  `LspSymbol` today, so this is a best-effort mapping."* Add a kind field to
  `LspSymbol` (`src/lsp/lsp_sym.h`), populate it in `collect_binding`
  (`src/lsp/lsp_collect.c`), and map it properly. Benefits `documentSymbol`,
  `workspace/symbol`, and completion item kinds at once.
- **`textDocument/documentHighlight`.** Advertise the capability
  (`src/lsp/lsp.c:704`), add the handler off the same index `on_definition`
  reads, and register a `DocumentHighlightProvider` in the adapter. This makes
  `occurrencesHighlight: true` (`web/main.js:1668`) mean what it says and makes
  the minimap's occurrence marks correct rather than textual.

  If the server change is too much for the current track, the client-side
  fallback is a provider in `lsp-client.js` that filters Monaco's own
  tokenization for comment and string tokens -- the same line c2mp draws
  (`symbols.js:737-745`), one layer up. Worse (it cannot tell a shadowed local
  from a global) but strictly better than the textual default.

- **Compiler-builtin hover** (added by M0; see §4.1). `find_symbol`
  (`src/lsp/lsp.c:126`) searches an index built from `Binding`s, and a
  compiler builtin has none -- so `println`, `+`, `=`, `not` and every other
  row of `src/compiler/builtins.c` hover to `{"contents":""}` and
  go-to-definition to `null`. These are the names a first-time visitor types
  most, and no client-side table can cover them: `doc-names.json` is generated
  from stdlib `;;;` docstrings and a builtin has no `defn` to hang one on.

  The table that does have the answer is `builtins.c`'s own. Add a
  name-keyed reader beside `builtin_first_with_name` that renders the
  overload set as one signature line per row, and have `on_hover` consult it
  after `find_symbol` misses. Signature help gets the same fallback off the
  same reader. Go-to-definition stays `null`: a builtin has no source
  location, and inventing one would be worse than answering nothing.

---

## 6. Deployment note -- read before shipping M4 or M5

M4 and M5 change `src/`, which means regenerating
`web/public/turmeric.{js,wasm}`. `sw.js`'s `CACHE_VERSION` is rewritten from
`VERSION` at **build** time (`injectSwVersion`, `apply: 'build'`), not on
artifact regen, and the precache is cache-first. Shipping new wasm bytes under
an unchanged version token serves a returning visitor the stale module -- the
trap recorded in `try-turmeric-lsp-plan.md` §7.6.

Nothing to do beyond this: cut a release. But do not deploy an M4/M5 bundle
without one.

M1, M2 and F1 are JS/HTML/CSS only and ride the normal asset hash.

---

## 7. F1 -- "Try our C Interpreter" in the footer

Independent of everything above; ships on its own.

### 7.1 There are two footers, not one

- **`SiteFooter`** (`web/site.js:135-170`), placed by `<site-footer>` on the
  marketing pages (`web/index.html:395`, tour, roadmap, trowel). Four columns;
  the relevant one is **Ecosystem** (Guides / API Docs / Spices,
  `web/site.js:154-158`).
- **`/try` has its own inline footer** (`web/try/index.html:404-408`) and does
  **not** place `<site-footer>` -- it loads `/site.js` at `:28` for the syntax
  highlighting and nav, but never uses the footer element. Its whole content is
  one prose line: *"Try Turmeric -- A web-based REPL for the Turmeric
  language"*.

The request says "the footer". Both should get the link: the marketing footer is
where a visitor browsing the site finds it, and the `/try` footer is where
someone already in a playground finds the other playground. Missing the second
is missing the point.

### 7.2 Third place, for consistency

`SiteSidebar` (`web/site.js:172-200`) mirrors the same three groups and is used
by the docs and guide pages. Its **Ecosystem** list should match the footer's or
the two disagree about what the ecosystem contains.

### 7.3 Copy

Sibling labels in the footer columns are bare noun phrases -- Tour, Trowel,
Spices, Guides. Recommend:

- **Footer/sidebar Ecosystem column:** `C Interpreter`
- **`/try` footer, which is prose:** append
  `-- also try our <a href="https://c.turmeric-lang.com">C interpreter</a>.`

The verb belongs in prose and reads as noise in a link column. The author's call
if the literal "Try our C Interpreter" is wanted in both.

### 7.4 Mechanics

Match the existing Spices link exactly (`web/site.js:157`): plain `href`, no
`target`, no `rel`. It is a same-project subdomain and there is nothing new to
justify.

### 7.5 Before shipping

- **Verify `https://c.turmeric-lang.com` resolves and serves.** A 404 in the
  footer of every page on the site is worse than no link at all.
- No footer spec exists in `web/tests/`. Add one assertion to
  `deploy-gate.spec.js` (or a small `footer.spec.js`) that the link is present
  on a marketing page and on `/try`, with the right href -- a link in a web
  component that silently stops rendering is exactly the kind of thing nothing
  else here would catch.

---

## 8. Tests

`web/tests/` runs under Playwright (`web/playwright.config.js`), with a `mobile`
project (iPhone 13, 390x664) that already carries LSP coverage.

| Phase | Test |
|---|---|
| M1 | New `web/tests/minimap.spec.js`: strip renders; a buffer with a known error puts a tick in the overview ruler; toggle persists across reload |
| M1 | `web/tests/mobile.lsp.spec.js` (or a sibling): minimap is **off** at 390px |
| M2 | `web/tests/lsp.spec.js`: Symbols menu lists the buffer's own definitions; clicking one moves the cursor; `is-current` tracks the caret; unavailable-LSP state is distinct from empty |
| M3 | `web/tests/lsp.spec.js`: hover on a stdlib name yields content (only if M0 found the hole) |
| M4 | `web/tests/lsp.spec.js`: go-to-def on a stdlib name opens a read-only tab; the tab is absent from `_turiTabs.projectEntries()`; jump-back returns to the origin |
| M5 | `tests/lsp/run-mcp-lsp.sh` and `tur_lsp_session_unit` for the kind mapping and `documentHighlight` |
| F1 | Footer link present with the right href, on a marketing page and on `/try` |

`window._turiLsp` (`web/main.js:442`) and `window._turiTabs`
(`web/main.js:1702`) already expose enough to assert all of this without
sleeping on the server.

Per the repo rule: any `bash tests/run.sh` invocation for M5 runs with a
12-minute (720000ms) timeout.

---

## 9. Risks

- **M0 comes back empty on (2).** If the server returns null for stdlib
  definitions, M4's read-only-tab work has nothing to open and the phase shrinks
  to F12 plus the jump-back stack. That is why M0 is first.
- **Minimap on narrow panes.** Mitigated by the width gate and the toggle
  (§5.1). The failure mode is cosmetic, reversible by the user, and visible
  immediately.
- **Bundle size.** M1 and M2 add none -- Monaco's minimap and quick-outline are
  already in the bundle. M4's `turi_wasm_read_file` is a handful of bytes of C.
  M5's kind field is a widened struct.
- **Read-only tabs leaking into saved projects.** The two exclusions in §5.4
  (`buildProjectEntries`, `persistTabs`) are the whole mitigation and both are
  cheap to test; a spec for each is listed in §8.
- **Stale service worker after M4/M5.** §6. The mitigation is a release cut, not
  a code change.

---

## 10. Execution record (2026-08-28)

Every phase landed. What follows is the delta between the plan as written and
the plan as built, plus the two things this container could not check.

### 10.1 What each phase became

| Phase | Landed as |
|---|---|
| **M0** | §4.1. Measured against the real `tur lsp` over stdio, not the page. |
| **M1** | `minimap: { enabled, renderCharacters: false, showSlider, size, maxColumn }`, `overviewRulerLanes: 3`, a `minimapColors` block resolved out of CSS custom properties (`cssHex`/`withAlpha`, refusing anything Monaco's `Color.fromHex` would paint red), a width gate on measured editor width, and a persisted More-menu toggle that outranks the gate. |
| **M2** | A Symbols toolbar button opening a `.more-menu`-shelled popover, filled on open from `lspClient.documentSymbols()`, sorted by position, kind-labelled, caret-tracking, with four distinct states. Monaco's own quick-outline is reachable from a footer row in the same popover -- M2a's action, made discoverable rather than shipped as a second button. |
| **M3** | `lookupDoc` on the adapter; `provideHover` falls back to it and marks the result `_from the stdlib docs_`. |
| **M4** | `turi_wasm_read_file`, a `read-file` worker message, `onOpenExternal` + `onBeforeNavigate` on the adapter, `openReadOnlyTab`, a padlocked read-only tab excluded from `buildProjectEntries` and `persistTabs`, F12, Ctrl/Cmd+Alt+- and a Back button. |
| **M5** | `LspSymbol.kind`, `textDocument/documentHighlight`, and the compiler-builtin fallback M0 turned up. |
| **F1** | Both footers and the sidebar. |

### 10.2 Departures from the plan, and why

- **§5.4 recommended option (2), `turi_wasm_read_file`, over exporting `FS`.**
  Built as recommended. It refuses anything that is not a `.tur` file under
  the stdlib root, checks the separator after the prefix (so `/stdlibx/...`
  is not "inside" `/stdlib`), and rejects any `..` segment. Its refusals are
  all one signal -- `NULL` -- because the page's response to "not allowed",
  "not there" and "could not read" is identical, and distinguishing them
  would only report which paths exist. Seven refusal cases are asserted in
  `tests/lsp/wasm_backend_test.c`.

- **Read-only tabs are excluded from the LSP document set**, which §5.4 did
  not say. `getTabs` filters them. Opening `/stdlib/list.tur` as
  `file:///project/list.tur` would have the server analyse the stdlib as if
  the user had pasted it into their project, and publish diagnostics against
  code nobody in the playground can fix. The cost is that hover and the
  outline are inert inside a stdlib buffer; the outline says so in its own
  sentence rather than claiming the file defines nothing.

- **The jump-back stack pushes from inside the definition provider**
  (`onBeforeNavigate`), not from the F12 keybinding §5.4 suggested. The
  provider is the one place every route to a definition passes through, so
  Cmd+click and the context menu push too -- a keybinding-side push would
  have covered F12 only.

- **The More menu is now visible at every width.** It was mobile-only, on the
  reasoning that its rows duplicate desktop buttons. Two rows no longer do:
  "Force update", which predates this work, and the minimap toggle -- which
  would otherwise have been unreachable on exactly the widths where the
  minimap is on.

- **A `wordPattern` was added to the language configuration.** Monaco's
  default breaks on `-`, `?`, `!` and `/`, so `nil-value` was three words --
  wrong for double-click selection, for the range a completion replaces, and
  for the name M3 looks up. It is now the same character class as the
  server's `is_ident_char`.

- **M5 grew a third item**, per §4.1: `builtin_describe` in
  `src/compiler/builtins.c`, consulted by `on_hover` and `on_signature_help`
  after `find_symbol` misses. `println` and `*` now hover with their overload
  set, marked `built-in operator`. Definition on a builtin stays `null` --
  there is no source location, and inventing one would send the editor
  somewhere.

- **`documentHighlight` scans the text, not the index.** §5.5 said "off the
  same index `on_definition` reads", but that index holds definition spans
  only; occurrences are not in it. `lsp_scan_occurrences` walks the buffer
  skipping line comments, string literals, nesting `#| |#` blocks, and
  inline-C fences -- the last because a C identifier that happens to spell a
  Turmeric one is a different language's variable. The definition is reported
  as Write, every use as Text.

### 10.3 Verified here

C side:

- `bash tests/run.sh` -- **2712 passed, 0 failed**, run with the required
  12-minute timeout.
- `tur_lsp_session_unit`: 63 passed, 0 failed (8 new tests), leak detection on.
- `tur_lsp_wasm_backend_unit`: 35 passed, 0 failed (3 new tests), leak
  detection on.
- The four M0 answers, re-measured after M5: `println` and `*` now hover with
  their overload sets, `Point` reports SymbolKind 23 against `eight`'s 13, and
  `documentHighlight` on `twice` returns two ranges out of four textual
  occurrences.

Browser side, with one caveat below:

- `footer.spec.js` -- 4 passed.
- `minimap.spec.js` -- 4 of 5 passed; the fifth runs code and needs the eval
  worker.
- `mobile.minimap.spec.js` -- 3 passed.
- `lsp.spec.js`, the two new navigation describes -- 12 passed.
- `lsp.spec.js`, the pre-existing scripted-adapter describe -- 7 passed, so
  the hover and definition providers still behave after being rewritten.
- Regression checks on the specs this touched indirectly:
  `lang-picker.spec.js` (7), `mobile.tabs.spec.js` +
  `mobile.tabs-migration.spec.js` (9). All passed.

And then for real, in CI on #787, against a bundle the job builds with `emcc`:
the desktop suite went from 51 tests to 87 once these specs were named in it
(see §10.5), with the same single pre-existing failure in both runs
(`smoke.spec.js` "Force update", a `Cannot redefine property: reload` that
newer Chromium rejects). **All 36 added tests passed**, including the
`LSP integration — providers` describe, which until then had skipped itself on
every run because the committed wasm had no LSP exports.

**The caveat, which applied locally.** `web/public/turmeric.{js,wasm}` and
`web/public/doc-names.json` are gitignored build outputs, absent from a fresh
clone, and this container has no `emcc` to regenerate them -- so every spec
that waits on `#wasm-status-text` reaching "Ready" would hang. The runs above
used a scratch config that pins Playwright to the container's own Chromium and
swaps that wait for `window._turiEditor`. Nothing else was changed, and the
scripted-server describes never touch the eval worker at all. The scratch
config and the derived spec copies were deleted; only the real specs are
committed.

Three defects were found this way and fixed, all of which would have shipped:

- **The Back button was never hidden.** `.btn { display: inline-flex }` is a
  class rule and beats the user agent's `[hidden] { display: none }`, so
  `renderJumpBack()` set the attribute to no effect and the button sat in the
  toolbar from page load. `web/styles.css` already carried a note about this
  exact trap for `.lsp-status`; it now carries `.btn[hidden]` too.
- **Opening Symbols from the ⋯ menu opened and closed it in one event.** The
  forwarded click kept bubbling to the document-level outside-click closer,
  whose target -- the ⋯ row -- is outside the popover it had just opened. The
  ⋯ delegate now stops propagation, which is the same fix the "Language..."
  row already carried.
- **The hover fallback had no table to read.** `fetchDocNames()` ran only
  after a successful WASM boot, so M3 was inert for the whole window where it
  is most wanted and permanently inert when that boot failed. It is now also
  kicked when the language server starts.

### 10.4 Not verified, and why

- **`mobile.minimap.spec.js` has not actually run anywhere.** Locally it was
  driven on Chromium at a phone viewport, because this container ships no
  WebKit. In CI it did not run either -- see §10.5. The width gate it asserts
  is engine-independent, so the local run is meaningful, but it is not the
  same thing as the spec passing on the engine its project names.
- **`https://c.turmeric-lang.com` was not fetched** (§7.5). This container's
  egress proxy denies CONNECT to `turmeric-lang.com` and every subdomain of
  it, including `spices.turmeric-lang.com`, which the footer already links --
  so the denial says nothing about whether the host serves. Someone with
  network access should load it before this reaches the marketing pages: a
  404 in the footer of every page is worse than no link.

### 10.5 Two specs that were passing by not running

Both found by reading the CI logs on #787 rather than trusting the job's
green, and both the same defect one level up from the one
`docs-offline.spec.js` already carries a note about.

- **The desktop step runs a hardcoded list of spec paths.** Playwright only
  picks up what is named there, so `minimap.spec.js`, `footer.spec.js` and
  `lsp.spec.js` -- the last of which had never been on it at all -- went
  green by never being invoked. Named now; the list went from 51 tests to 87.

- **The mobile project is WebKit, and only Chromium was installed.** All 32
  mobile tests died at `browserType.launch: Executable doesn't exist at
  .../webkit-*/pw_run.sh` before any test body ran, and the step's
  `continue-on-error` hid it. That is the whole mobile suite -- tabs, project
  load and download, the split handle, the PWA manifest, mobile LSP --
  asserting nothing, for as long as the runner image has shipped without it.
  A non-blocking `playwright install webkit` step now precedes it: the job's
  gate is `deploy-gate.spec.js` on Chromium, and a WebKit download that fails
  on some runner must leave the mobile suite as unrun as it is today rather
  than redden a job that was going to pass.

One pre-existing desktop failure is left alone, deliberately:
`smoke.spec.js` "Force update clears caches and reloads" throws
`TypeError: Cannot redefine property: reload` from its own `page.evaluate`,
before it reaches any application code. Newer Chromium makes
`window.location.reload` non-configurable. It fails identically on the head
before this work and is unrelated to it, so it belongs in its own change.

### 10.6 Before shipping

§6 stands and now applies: M4 and M5 change `src/`, so
`web/public/turmeric.{js,wasm}` must be regenerated and a release cut, or a
returning visitor is served the stale module from the service worker's
cache-first precache and sees none of it. M1, M2, M3 and F1 are
JS/HTML/CSS-only and ride the normal asset hash.
