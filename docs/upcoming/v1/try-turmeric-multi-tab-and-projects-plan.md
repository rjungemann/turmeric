## Try Turmeric: Multi-tab UI, Project Zip Download, and Zip Load

> **Status:** Phase 1 landed (multi-tab UI + persistence + migration);
> Phases 2 (download-as-zip) and 3 (load-from-zip) not started.
> **Type:** Web platform / Try Turmeric
> **Predecessor:** [`docs/archive/try-turmeric-pwa-mobile-plan.md`](../../archive/try-turmeric-pwa-mobile-plan.md)

## Goal

Promote `/try` from a single-buffer playground to a small in-browser
"project" surface:

1. A real **multi-tab editor** where each tab is a `.tur` file with its
   own persisted buffer, cursor, and scroll.
2. **Download the whole tab set as a zip** that mirrors a `tur init`
   project layout (`build.tur` + `src/*.tur`), so the user can hand it
   straight to a local `tur build`.
3. **Load a zip back** (drag-and-drop or file picker) to repopulate the
   tab set, with the active tab restored from the zip's manifest.

This is a follow-on to the shipped Try Turmeric PWA work; it deliberately
does not touch the compiler, the WASM runtime, or the doc panel. Scope is
`web/try/`, `web/main.js`, `web/styles.css`, `web/public/`, and the
Playwright tests in `web/tests/`.

## Status

Phase 1 (multi-tab UI + persistence + migration) is live in `web/main.js`
and `web/styles.css`. Each tab carries an independent Monaco
`ITextModel` (so undo history survives switches), the `+ New` button
creates `untitled-N.tur`, hover-`×` closes (disallowed when one tab
remains), double-click renames inline, and the tab set persists under
`tur.try.tabs.v1` / `tur.try.activeTab.v1` with a one-shot migration
from the legacy `tur.try.buffer.v1` / `cursor` / `scroll` keys.

Drag-to-reorder (plan §1.3 bullet 3) is intentionally deferred -- not
load-bearing for Phases 2 and 3, which only need a stable tab list.
Playwright specs (§6) are still to land.

## 1. Multi-tab UI

### 1.1 Data model

Each tab is a small JS object owned by `main.js`:

```js
{
    id: 'tab-1',               // stable id, generated on creation
    name: 'main.tur',          // user-editable filename, must end in .tur
    content: '',               // current buffer
    cursor: { lineNumber: 1, column: 1 },
    scrollTop: 0,
    createdAt: 1734567890123,
}
```

The full editor state is `{ tabs: Tab[], activeId: string }`. A single
Monaco editor instance is reused; switching tabs swaps the underlying
`monaco.editor.ITextModel` rather than re-creating the editor (so undo
history per file is preserved by Monaco's own model store).

### 1.2 Tab strip

The existing `.editor-tabs` row gains:

- One `<button class="tab-button">` per tab, with the active tab styled
  as it is today.
- A close affordance on hover (`×` button inside each tab; not on the
  active tab if it's the last one).
- A double-click-to-rename interaction that swaps the label for a small
  `<input>` (Enter commits, Escape cancels).
- A `+ New` button that opens a fresh `untitled-N.tur` tab where `N` is
  the smallest integer that produces a non-colliding name.

### 1.3 Interactions

- **Switching tabs** persists the outgoing tab's cursor/scroll, swaps
  the Monaco model, and applies the incoming tab's cursor/scroll.
- **Closing the last tab** is disallowed (we always keep one tab
  open -- closing the last would leave the editor blank with no UI to
  create a new one without first opening a side panel).
- **Reordering** by horizontal drag of the tab buttons. Reuses the
  pointer-capture pattern from `initHScrollDrag` in `main.js`; a 6px
  drag threshold disambiguates click-vs-drag.

### 1.4 Run semantics

`Run` (Ctrl/Cmd+Enter) executes the **active tab's** content.

Cross-tab imports are out of scope for v1 of this plan -- the WASM
runtime currently flattens the input to a single buffer. A follow-on
"virtual project mount" plan can teach the WASM driver about other tabs
later. The download-as-zip path below is the workaround in the
meantime: export the project, run `tur build` locally.

## 2. Persistence

Builds on the keys reserved in the predecessor plan:

| Key | Shape | Notes |
| --- | --- | --- |
| `tur.try.tabs.v1` | `Tab[]` (with `content`, `cursor`, `scrollTop`) | One write per debounced change |
| `tur.try.activeTab.v1` | `string` (active tab id) | Cheap, write on switch |

### 2.1 Migration from single-buffer

On first load of the multi-tab build:

- If `tur.try.tabs.v1` is absent **and** `tur.try.buffer.v1` is present,
  synthesize one tab named `main.tur` with the legacy buffer's
  content/cursor/scroll, write the new keys, and delete the legacy keys
  (`tur.try.buffer.v1`, `tur.try.cursor.v1`, `tur.try.scroll.v1`).
- If `tur.try.tabs.v1` is also absent and no legacy buffer exists, seed
  the tab set with one `main.tur` tab holding the current default Hello
  World example.

The migration runs exactly once per browser -- after it writes
`tur.try.tabs.v1`, the legacy branch never fires again.

### 2.2 Hydration order

Same priority as today, applied to the tab set rather than a single buffer:

1. URL hash share-link (still maps to a single tab named after the hash
   slug -- on hydration, the share-link buffer replaces the active tab's
   content and the rest of the tab set is preserved).
2. `localStorage` tab set (or migration).
3. Default Hello World tab (only if both above are empty).

Tutorial mode (`?tutorial=...`) bypasses tab hydration entirely; the
tutorial owns a single ephemeral "tutorial" tab that is not persisted.

### 2.3 Reset workspace

`resetWorkspace()` already clears `tur.try.*`; extend it to also clear
the multi-tab keys. No new UI is needed.

### 2.4 Quota

Per-tab writes go through `safeWrite`, which already handles
`QuotaExceededError` by disabling persistence for the session. For
large projects this is fine -- the user can still download the zip
before reloading.

## 3. Download project as zip

### 3.1 Trigger

A new `Download` button in the editor toolbar (icon-only on
`max-width: 600px`, like the other actions). Sibling of the existing
`Share` button.

### 3.2 Zip layout

The zip mirrors the layout produced by `tur init`:

```
try-turmeric-<slug>.zip
  build.tur
  src/
    main.tur
    <other-tab-name>.tur
    ...
  .turmeric/
    workspace.json    # the active tab id + tab metadata, for re-import
```

- `build.tur` is generated from a small template: project name (derived
  from the zip's slug), declared `:main "main.tur"` if a `main.tur`
  exists in the tab set, otherwise the first tab is the main.
- `src/<name>.tur` files come from each tab's content. Tab names that
  do not end in `.tur` get the extension appended; names with path
  separators or other shell-unfriendly characters are slugified.
- `.turmeric/workspace.json` carries the in-browser-only metadata
  (active tab id, tab order, cursor positions) so a round-trip
  download-then-upload preserves the user's session exactly.

### 3.3 Implementation

- Use a tree-shakeable client-side zip library; preferred:
  [`fflate`](https://github.com/101arrowz/fflate) (≈12 KB minified,
  zero deps, sync zip in a worker). Acceptable alternative: a hand-rolled
  store-only zip writer (no deflate) -- text files compress poorly and a
  ~30-line writer in `main.js` keeps the dependency surface flat.
- Generate the blob in-memory, then trigger a download via an `<a>`
  with `URL.createObjectURL`.
- Filename: `try-turmeric-<timestamp>.zip` by default; the user can
  rename in the save dialog.

### 3.4 No server round-trip

The zip is built entirely in the browser. The Cloudflare worker is
unchanged. This keeps the feature available offline (PWA standalone
mode) and avoids any privacy footgun -- user code never leaves the tab.

## 4. Load project from zip

### 4.1 Trigger

Two entry points, both wired to the same handler:

- **Drag-and-drop**: drop a `.zip` anywhere on the editor pane. A
  drop-target overlay appears on `dragenter` (covers the editor, dims
  the rest) and disappears on `dragleave` / `drop`.
- **Open menu item**: a small `Open` button next to `Download` opens a
  hidden `<input type="file" accept=".zip">`.

### 4.2 Parse rules

- Reject anything that isn't a zip (magic bytes `PK\x03\x04`) with a
  toast.
- Walk the entries; for each `*.tur` under `src/` create a tab. The
  filename (minus `src/`) becomes the tab name.
- If `.turmeric/workspace.json` is present and parses, restore active
  tab id, tab order, and per-tab cursor/scroll from it. Tabs whose
  workspace entry references a missing file are silently dropped.
- If no `workspace.json`, the active tab is the entry named `main.tur`
  (or the first tab in alphabetical order if there is no `main.tur`).
- A zip whose `src/` is empty (or whose only `.tur` files are 0 bytes)
  is rejected with a toast -- no destructive replace.

### 4.3 Conflict resolution

Loading a zip is a **replace**, not a merge. Show a confirm:

> "Loading this project will replace your current tabs. The current
> workspace is in your browser's localStorage and can be recovered by
> reloading without confirming."

(Implementation note: don't actually write the new tab set to
`localStorage` until the user confirms. A reject leaves the existing
tab set intact.)

### 4.4 Size limit

Reject zips larger than 5 MB to avoid blowing localStorage quota. The
in-memory parse limit is generous (50 MB) so a too-big zip still surfaces
a clean error rather than an OOM.

## 5. Worker / headers

No changes. The zip lib runs client-side; the SW caches it like any
other static module.

## 6. Tests

Add to the existing Playwright suite under `web/tests/`:

- `mobile.tabs.spec.js` -- create / switch / close / rename, reload,
  active tab restored, per-tab cursor restored.
- `mobile.zip-roundtrip.spec.js` -- create three tabs, click Download
  (capture via Playwright's download API), then drag the zip back onto
  the editor and assert the tab set + active tab match.
- A migration spec that seeds the legacy single-buffer keys, reloads,
  and asserts the synthesized `main.tur` tab exists and the legacy
  keys are gone.

Manual checks before release:

- Round-trip a downloaded zip through a local `tur build .` to confirm
  the generated `build.tur` is valid.
- Verify the Open / drop affordance works on iOS Safari (file input
  with `accept=".zip"`) -- iOS sometimes restricts custom file types in
  PWAs.

## 7. Rollout

1. Multi-tab UI + persistence + migration (largest single piece,
   independently shippable).
2. Download-as-zip (tiny addition once tabs exist).
3. Load-from-zip + drop overlay (depends on 2 for the format).

Each step is independently revertable; only step 1 carries a migration
risk, mitigated by writing the new keys before deleting the legacy ones
(if the migration write fails, the legacy keys survive intact).

## Open questions

- Should the share-link URL hash encode the entire tab set, or stay
  single-buffer as today? Encoding the set bloats the URL quickly; a
  zip download is the better mechanism for "share a project." Keep
  share-link single-tab for v1.
- Does the WASM driver eventually need to see other tabs (e.g. for
  multi-file `import` resolution)? A separate plan, gated on the
  driver growing a virtual file-system hook.
- Should `tur init`-style templates be selectable when creating a new
  workspace? Out of scope for v1 -- the default Hello World is enough;
  curated templates can land via the existing Examples dropdown.
