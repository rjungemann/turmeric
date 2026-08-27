# Try Turmeric: minimap, outline, and navigable definitions

> **Status:** Proposed
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
