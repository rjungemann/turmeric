# Try Turmeric PWA + Mobile Layout Plan

## Goal

Turn `/try` into a progressive web app installable to a mobile home screen,
give it a non-scrolling mobile layout (editor on top, console on bottom),
make the editor/console split draggable, and persist editor state across
refresh.

Scope is limited to `web/try/` and the assets it pulls in (`web/main.js`,
`web/styles.css`, `web/public/`, `web/vite.config.js`). No compiler changes.

## Status

Shipped. All in-scope sections landed; archived to `docs/archive/`.

Done:
- §1.1 Web app manifest + meta tags + 192/512/apple-touch icons.
- §1.2 Service worker (`web/public/sw.js`) -- precache on install, cache-first
  for static + WASM, network-first for `/docs/*` and HTML navigations,
  `CACHE_VERSION` constant bumped per release, kill-switch `web/public/sw-kill.js`,
  registration from `web/main.js` on `load` with scope `/`.
- §1.3 Install affordances -- `beforeinstallprompt` captured, "Install" button
  injected into the editor toolbar, hidden on `appinstalled`; iOS Safari gets
  a one-time "Add to Home Screen" hint banner.
- §2.1 / §2.1a / §2.2 / §2.3 / §2.4 -- mobile viewport, focus-zoom suppression,
  non-scrolling layout, toolbar density, horizontally scrollable editor header.
- §3 Draggable split -- `#split-handle` markup, CSS hit target with wider
  invisible padding (`::before`), pointer drag handler with clamp to
  `[0.15, 0.85]`, keyboard nudge (arrows / Home / End), double-click reset,
  `localStorage` persistence under `tur.try.split.h.v1` / `tur.try.split.v.v1`,
  Monaco `layout()` triggered after each drag.
- §4 Editor persistence -- single-buffer keys (`tur.try.buffer.v1`,
  `tur.try.cursor.v1`, `tur.try.scroll.v1`, `tur.try.console.v1`) +
  "Reset workspace" flow. Multi-tab (`tur.try.tabs.v1` /
  `tur.try.activeTab.v1`) is **deferred** -- the underlying multi-tab UI
  doesn't exist yet (the "+ New" button is a placeholder), so persisting
  tab state would have nothing to persist.
- §5 `/sw.js` + `/sw-kill.js` + `/manifest.webmanifest` entries in
  `web/public/_headers`.
- §6 Mobile Playwright project (`devices['iPhone 13']`) with
  `tests/mobile.split-and-pwa.spec.js` covering layout, split persistence,
  editor buffer persistence, SW registration + WASM cache, and manifest
  reachability. Lighthouse PWA audit deferred to the `web-perf` skill on
  next release verification (`chrome-devtools-mcp`).

Deferred to a v2 follow-up:
- Multi-tab persistence (waits on real multi-tab UI).
- Doc-panel last-symbol persistence (Open question #1).
- Service worker precache of Monaco chunks (Open question #3 -- runtime
  cache picks them up on first use today).

## 1. PWA shell

The page is already served as static assets via the Cloudflare Worker
(`web/worker.js`) with `turmeric.wasm` next to `turmeric.js`. To install
to a home screen we need a manifest, an icon set, and a minimal service
worker that satisfies install criteria and gives the WASM a cached fast
path.

### 1.1 Web app manifest

- Add `web/public/manifest.webmanifest`:
  - `name: "Try Turmeric"`, `short_name: "Turmeric"`
  - `start_url: "/try/"`, `scope: "/try/"`
  - `display: "standalone"`
  - `background_color` / `theme_color` matching `vars.css`
  - `icons: [192, 512]` PNG + the existing `logo-icon.svg` as `purpose: "any maskable"`
- Reference it from `web/try/index.html`:
  - `<link rel="manifest" href="/manifest.webmanifest">`
  - `<meta name="theme-color" content="...">`
  - `<meta name="apple-mobile-web-app-capable" content="yes">`
  - `<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">`
  - `<link rel="apple-touch-icon" href="/icons/apple-touch-icon.png">`
- Generate the PNG icons once and check into `web/public/icons/`. iOS does
  not honor `maskable` SVGs; ship a 180×180 apple-touch-icon PNG.

### 1.2 Service worker

- New file `web/public/sw.js` (served at `/sw.js` so its scope is the
  whole origin). Strategy:
  - **Precache** on `install`: `/try/`, `/main.js`, `/styles.css`,
    `/site.css`, `/site.js`, `/turmeric.js`, `/turmeric.wasm`,
    `/doc-names.json`, `/favicon.svg`, manifest, icons.
  - **Runtime**: cache-first for `/turmeric.wasm` and same-origin static
    assets, network-first for `/docs/*` and HTML navigations (so the
    standalone shell is offline-capable but doc pages stay fresh).
  - Bump a `CACHE_VERSION` constant on each release; old caches are
    deleted in `activate`.
- Register from `web/main.js` on `load`:
  ```js
  if ('serviceWorker' in navigator) {
    window.addEventListener('load', () => {
      navigator.serviceWorker.register('/sw.js', { scope: '/' });
    });
  }
  ```
- Cloudflare worker (`web/worker.js`) needs to keep serving `/sw.js`
  with `Content-Type: application/javascript` and `Service-Worker-Allowed: /`.
- Add `/sw.js` to `web/public/_headers` with
  `Cache-Control: no-cache` so a new service worker is picked up on next
  page load.

### 1.3 Install affordances

- Capture `beforeinstallprompt`, stash the event, and show a small
  "Install app" button in the editor toolbar on supported browsers.
  Hide once `appinstalled` fires or when `matchMedia('(display-mode: standalone)')`
  is true.
- On iOS (no `beforeinstallprompt`) show a one-time "Add to Home Screen"
  hint when running in mobile Safari and not standalone.

## 2. Non-scrolling mobile layout

Right now `@media (max-width: 1024px)` stacks panes vertically and lets
the page scroll. On phones we want a fixed-viewport split: editor (top)
+ console (bottom), nothing else scrolls.

The UI must **fill the entire mobile viewport** and **never scroll the
page itself** -- the editor pane and console pane scroll internally,
but the outer document does not. The *only* exception is when the
soft keyboard is shown: the viewport shrinks (via
`interactive-widget=resizes-content`), and any necessary scroll-into-view
that the OS performs to keep the caret visible is allowed. As soon as
the keyboard hides, the layout returns to the fixed-viewport split.

### 2.1 Viewport

- Update the meta viewport in `web/try/index.html` to:
  `<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover, interactive-widget=resizes-content">`
- Use `100dvh` (dynamic viewport height) for the app shell so the iOS URL
  bar collapsing doesn't reveal page chrome below the console.

### 2.1a Suppress iOS focus-zoom

Tapping into Monaco (or the REPL input) on iOS Safari triggers an
auto-zoom-to-input whenever the focused element's computed `font-size`
is below 16px. Once zoomed, the page also gains horizontal scroll --
two regressions for the price of one. Both must be prevented:

- Monaco's editor font is 13px today. Override the
  `.monaco-editor .inputarea` (the hidden textarea that actually
  receives focus) to `font-size: 16px` inside the mobile media query.
  This does **not** affect rendered code (Monaco draws its own glyphs);
  it only affects the invisible textarea iOS reads to decide whether
  to zoom.
- The visible REPL input (`#repl-input`) and the doc-search input
  (`#doc-search`) must also be `font-size: 16px` on mobile for the
  same reason.
- Do **not** disable user zoom via `maximum-scale=1` or
  `user-scalable=no` -- that is an accessibility footgun. Fixing the
  font-size on focused inputs is enough.
- After applying, verify on a real iOS device: tapping the editor must
  keep `visualViewport.scale === 1` and must not introduce horizontal
  scroll on the document.

### 2.2 Layout changes (`web/styles.css`)

- In `@media (max-width: 1024px)`:
  - `body { overflow: hidden; height: 100dvh; }`
  - `#app { display: flex; flex-direction: column; height: 100dvh; }`
  - `site-nav` and `.footer` collapse to a thin top bar (or hide footer
    entirely in standalone mode -- gated by `@media (display-mode: standalone)`).
  - `.main { flex: 1 1 auto; min-height: 0; }`
  - `.repl-container { display: grid; grid-template-rows: var(--split, 1fr) 6px 1fr; height: 100%; }`
  - `.editor-pane`, `.console-pane { min-height: 0; overflow: hidden; }`
  - Monaco container gets `height: 100%` and `automaticLayout: true`
    (already on) so the resize observer fires when the split moves.
- Hide the doc panel by default on mobile; promote it to a full-screen
  overlay when opened.

### 2.3 Toolbar density

- Collapse the editor toolbar buttons (`Run`, `Clear`, `Format`, `Share`,
  Examples) into icon-only on `max-width: 600px`; the `Run` button stays
  labeled. Move Examples into a bottom-sheet on small screens.

### 2.4 Horizontally scrollable tab/command bar

The `.editor-header` (tabs on the left, action buttons on the right)
overflows on small screens today and the right-side buttons get clipped.
Make it scroll horizontally with a drag-to-pan gesture.

- CSS on `.editor-header` (or its inner row):
  - `display: flex; flex-wrap: nowrap; overflow-x: auto; overflow-y: hidden;`
  - `scroll-snap-type: x proximity;` so dragged-to positions settle
    cleanly on tab boundaries.
  - `scrollbar-width: none;` + `::-webkit-scrollbar { display: none; }`
    -- the bar shouldn't show a permanent scrollbar; users discover it
    by dragging.
  - `overscroll-behavior-x: contain;` so a horizontal drag at the edge
    doesn't trigger browser back-navigation on iOS.
  - Add fade gradients (`mask-image` linear gradient) on the left/right
    edges that appear only when the bar is actually scrollable -- a JS
    `ResizeObserver` toggles a `.has-overflow` class.
- The `.editor-tabs` and `.editor-actions` groups stay as
  `flex-shrink: 0` children so nothing gets squashed; horizontal scroll
  is the only response to overflow.
- Drag-to-scroll JS (`web/main.js`):
  - On `pointerdown` inside the header (but **not** on a `<button>` /
    `<select>` -- those still click), record `startX = e.clientX`,
    `startScroll = el.scrollLeft`, and `setPointerCapture`.
  - On `pointermove`, `el.scrollLeft = startScroll - (e.clientX - startX)`.
    If movement exceeds a 6px threshold, set a `data-dragging` flag and
    swallow the next `click` so the drag doesn't accidentally fire a
    tab switch.
  - On `pointerup` / `pointercancel`, release capture, clear the flag
    on next tick.
  - Touch devices get native momentum scrolling for free via
    `-webkit-overflow-scrolling: touch` -- the JS handler is primarily
    for mouse/trackpad users on narrow desktop windows.
- Keep the active tab visible after a tab switch:
  `activeTabEl.scrollIntoView({ inline: 'nearest', block: 'nearest' })`.
- Same treatment applies to the console action row if it ever overflows
  (it doesn't today, but the same utility class -- e.g. `.h-scroll-drag`
  -- can be reused).

## 3. Draggable split

Single resizer that works for both desktop (horizontal split, existing)
and mobile (vertical split, new).

### 3.1 Markup

In `web/try/index.html`, between `.editor-pane` and `.console-pane` add:
```html
<div class="split-handle" id="split-handle" role="separator"
     aria-orientation="vertical" tabindex="0"></div>
```
The CSS flips `aria-orientation` based on the media query (logical, not
literal -- it's the axis perpendicular to drag).

### 3.2 CSS

- `.repl-container` uses CSS custom property `--split` (a fraction or
  percentage). Desktop: `grid-template-columns: var(--split, 1fr) 6px 1fr;`
  Mobile: `grid-template-rows: var(--split, 1fr) 6px 1fr;`.
- `.split-handle` is a 6px hit target with a wider invisible padding zone
  (`::before` 16px on touch) for finger drags. Cursor `col-resize` /
  `row-resize` based on the same media query.

### 3.3 JS (`web/main.js`)

- Pointer Events API (`pointerdown` / `pointermove` / `pointerup`,
  `setPointerCapture`) for unified mouse + touch + stylus.
- On drag, compute the new fraction from container bounding rect; clamp
  to `[0.15, 0.85]`; write `--split` on `.repl-container`.
- Persist the fraction to `localStorage` under `tur.try.split` (separate
  keys for the two orientations: `tur.try.split.h` / `tur.try.split.v`).
- Keyboard: arrow keys on the focused separator nudge `--split` by 2%.
- Double-click resets to 1fr / 1fr.

## 4. Editor state persistence

Goal: when the page reloads (including after a PWA cold start), the
editor opens to exactly what the user had.

### 4.1 What to persist

In `localStorage` under namespaced keys:
- `tur.try.tabs.v1` -- JSON of `[{ id, name, content }]` (current tab
  list including new tabs the user created).
- `tur.try.activeTab.v1` -- the active tab id.
- `tur.try.cursor.v1` -- `{ lineNumber, column }` per tab id.
- `tur.try.scroll.v1` -- Monaco scroll top per tab id.
- `tur.try.split.h.v1` / `tur.try.split.v.v1` -- from §3.
- `tur.try.console.v1` -- last N (default 200) console lines so the
  output panel isn't blank on reload. Cleared by the existing "Clear
  Console" button.

### 4.2 Hook points in `web/main.js`

- On Monaco model change (`onDidChangeModelContent`), debounce 250ms and
  write the tab content. Per-tab key, so tab switching doesn't thrash.
- On `onDidChangeCursorPosition` / `onDidScrollChange`, debounce 500ms
  and persist cursor + scroll.
- On startup, after Monaco is created, hydrate from `localStorage`
  *before* falling back to the default Hello World. Order:
  1. URL hash share-link (highest -- preserves existing share behavior)
  2. `localStorage`
  3. Default example
- Tutorial mode (`?tutorial=...`) bypasses hydration to avoid stomping
  the tutorial step content; tutorial progress can land in
  `tur.try.tutorial.v1` later.

### 4.3 Versioning + reset

- The `.v1` suffix lets us bump the schema without parsing old shapes.
- Add a "Reset workspace" menu item under the existing Clear button's
  overflow that clears every `tur.try.*` key and reloads. Guard with a
  confirm.
- Wrap every read in a try/catch -- a corrupt entry shouldn't brick the
  editor.

### 4.4 Quota

`localStorage` is ~5MB; tab content is small. If we ever exceed quota
(e.g. user pastes a huge file), catch `QuotaExceededError` and degrade
to in-memory only with a one-time console warning.

## 5. Worker / headers

- `web/worker.js`: ensure `/sw.js` is served from `web/public/` like any
  other static asset. No new routing needed if the directory is already
  the static root.
- `web/public/_headers`:
  - `/sw.js -> Cache-Control: no-cache, Service-Worker-Allowed: /`
  - `/manifest.webmanifest -> Cache-Control: public, max-age=3600`
  - Keep `/turmeric.wasm` long-cache (it's content-hashed via the build).

## 6. Testing

- Existing Playwright suite (`web/tests/`, `web/playwright.config.js`)
  gets:
  - A mobile viewport project (`devices['iPhone 13']`) running the same
    smoke tests plus split-drag + reload-persistence.
  - A test that registers the service worker, reloads with `offline: true`,
    and confirms the editor still boots.
- Lighthouse PWA audit run from `chrome-devtools-mcp` (see the `web-perf`
  skill) -- target: installable, manifest valid, SW registered.

## 7. Rollout order

1. Mobile layout + viewport fix (no JS state, immediate UX win).
2. Editor persistence (small, isolated to `main.js`).
3. Draggable split (depends on 1's grid layout).
4. PWA manifest + icons (no SW yet -- gets "Add to Home Screen" working).
5. Service worker + offline cache (last; needs careful cache-busting and
   a kill-switch route).

Each step is independently shippable; only step 5 is reversible-with-care
(an SW that caches the wrong thing is hard to dislodge from users'
browsers -- ship a `/sw-kill.js` that unregisters, as insurance).

## Open questions

- Should the doc panel also persist its last opened symbol? Probably yes
  in v2.
- Should the share-link URL hash *also* be written when state changes,
  so the URL stays a permalink to the current buffer? Today it only
  reflects the last explicit Share click. Leave as-is for this plan.
- Monaco itself is large; the SW precaching the WASM is the bigger win.
  Decide whether to precache Monaco's chunks or let runtime cache pick
  them up on first use (probably the latter -- precache budget stays small).
