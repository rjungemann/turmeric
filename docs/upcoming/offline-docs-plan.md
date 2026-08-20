---
title: Offline Docs and In-App Docs for Try Turmeric (OD)
category: Planning
description: One rendered-docs artifact that serves three consumers -- the website, an in-app docs browser inside Try Turmeric that works offline, and a local/CLI story for docs without any network.
---

# Offline Docs + Try Turmeric Embedding (OD)

**Status:** proposal, not started. Requested 2026-08-20: (1) an
offline-available version of the guides and API docs; (2) guides and API docs
easily accessible *from* Try Turmeric -- which, since Try has offline mode,
suggests embedding them so both problems collapse into one.

The short answer this plan builds out: **they do collapse into one.** Generate
a chrome-free "docs pack" (nav index + per-page HTML fragments) from the same
generators that already build the site docs, ship it as static assets of the
Try PWA, render it in an in-app docs pane, and let the existing service worker
precache it **unconditionally**. The website, the in-app viewer, and offline
mode then all read the same artifact, and nothing is written twice.

**Settled up front: the pack is always precached.** There is no "download
docs for offline" toggle and no opt-in. Installing Try Turmeric means having
the docs, in the same way it already means having the compiler -- offline
completeness is the feature, and a toggle would be one more state to explain,
test, and get wrong. The size budget in 2.1 exists to *protect* that decision:
if the pack ever outgrows the budget, the fix is a smaller pack, not a
switch.

## 1. What exists today (checked against the tree)

The pipeline (`Justfile`, `docs:` / `guides:` / `spices:` recipes):

| Generator | Input | Output | Tracked? |
|---|---|---|---|
| `tools/gendocs.py` | `stdlib/*.tur` `;;;` docstrings | `docs/html/api/` HTML; `stdlib/docstrings.tur` (runtime lookup table); `web/public/doc-names.json` (search index) | outputs gitignored except `docstrings.tur` |
| `tools/genguides.py` | `docs/guides/*.md` (137 guides, ~2.3 MB markdown) | `docs/html/guides/` HTML | gitignored |
| `tools/genspices.py` | sibling `../turmeric-spices/` | `docs/html/spices/` + spice name JSON | gitignored |

How docs reach the site: `web/public/docs` is a **symlink to `../../docs`**,
so Vite copies the docs tree into `dist/docs/` and the worker serves
`/docs/html/guides/`, `/docs/html/api/`, `/docs/html/spices/` on
turmeric-lang.com. `just deploy-web` depends on `web` -> `wasm` -> `docs`, so
a deploy always regenerates -- freshness is already solved.

Try Turmeric's offline mode (`web/public/sw.js`): a PWA scoped to `/try/`,
whose service worker **precaches** the shell, `turmeric.{js,wasm}`,
`doc-names.json`, and icons; runtime strategy is cache-first for same-origin
static assets and **network-first for HTML navigations and `/docs/*`**. So
today the rendered docs are reachable offline *only if that exact page was
visited before going offline*, and a cold offline navigation to `/docs/...`
falls back to the REPL shell -- the docs are precisely the thing offline mode
does not cover. `CACHE_VERSION` is stamped from `VERSION` at build time
(`injectSwVersion` in `web/vite.config.js`), so eviction-on-release already
works and OD inherits it for free.

What the REPL already has (`web/main.js`): a **doc panel** -- search over
`doc-names.json`, docstring text via the wasm export `turi_doc_lookup`
(backed by `stdlib/docstrings.tur`, *baked into the wasm*, so symbol-level
docs already work fully offline), spice-symbol summaries from the JSON, and an
"Open full docs" link that **navigates away from `/try/`** to
`/docs/html/api/index.html`, losing REPL state. That link is the seam OD2
replaces.

So the gap is narrow and specific: the *rendered pages* (guides, per-module
API pages) are not offline and not in-app; everything symbol-sized already is.

## 2. Design: one docs pack, three consumers

### 2.1 The artifact

A build output `web/public/docs-pack/` (gitignored like its siblings),
emitted by the existing generators -- not a new renderer:

```
web/public/docs-pack/
  index.json            # version, nav tree, titles, categories, search strings
  guides/<slug>.html    # chrome-free fragments: the article content only
  api/<module>.html     # same, one per stdlib module
  spices/<spice>.html   # same, when ../turmeric-spices is present
```

- **Fragments, not pages.** Each file is the rendered article body (the same
  HTML the site pages wrap in site chrome), no `<head>`, no nav, no CSS. The
  in-app viewer supplies styling; the site keeps its own wrapper. This is a
  refactor inside `genguides.py`/`gendocs.py` -- render the body once, wrap it
  twice -- so the site and the pack cannot drift.
- **`index.json` is the contract**: `{version, generated, guides: [{slug,
  title, category, description, words}], api: [{module, title, symbols:
  [...]}]}`. The `words` field carries a lowercase search string per page so
  client search needs no extra index. Stamp the `VERSION` in it; the viewer
  shows it, which makes "are my offline docs stale?" answerable at a glance.
- **Size budget -- the mechanism that keeps precache-always honest.** 2.3 MB
  of guide markdown plus the API pages renders to roughly 5-8 MB of
  fragments, ~1-2 MB over the wire with the compression the worker already
  gets. Against an already-precached `turmeric.wasm` that is small enough
  that unconditional precaching is not a judgement call. Because the whole
  design leans on that, it is a *checked* number: the pack emitter fails the
  build if the raw pack exceeds a budget (start at 16 MB), so an accidental
  image or a generated-page explosion is a build error at `just docs` time,
  not silent PWA bloat discovered by a user on a phone. Images referenced by
  guides get copied into the pack and counted.

  **If the budget trips, the response is to shrink the pack, never to make
  precaching conditional.** In descending order of preference: compress or
  drop oversized images; exclude a doc *category* from the pack (the pack is
  Try's docs, and it does not have to carry every generated page the website
  carries); split rarely-read reference pages into a second, lazily-fetched
  tier that the pane loads on demand and the SW caches on first use. Each of
  those keeps "installed means complete" true for what the pack claims to
  contain, which a toggle does not.
- Cross-links between guides (`[x](y.md)` -> `y.html`) are rewritten to
  pack-relative slugs at generation time, so the viewer resolves them without
  guessing. External links open in a new tab.

Explicitly rejected alternatives, with reasons, so they do not resurface:

- **Iframe the existing `/docs/html/` pages inside Try** -- offline only for
  visited pages (network-first runtime cache), duplicated site chrome inside
  a pane, and scroll/navigation state trapped in a nested browsing context.
  Same-origin iframes do work under the site's COOP/COEP headers, but there
  is no offline story without precaching the whole `/docs/html/` tree, which
  drags in full site chrome per page.
- **Bake the guides into the wasm** like `docstrings.tur` -- pays the full
  docs size in wasm download and memory for every REPL user whether or not
  they open a guide; fragments as static assets are lazy and cacheable.
- **Precache the whole symlinked `/docs/` tree** -- the symlink ships the
  *entire* docs source tree (archive, reported, upcoming, notes) into `dist/`
  today; precaching it would multiply the PWA footprint for content Try does
  not need. (Side finding, worth its own small fix -- see OD5.)

### 2.2 Consumers

1. **The website** -- unchanged pages, now assembled from the shared
   fragments. `/docs/html/*` URLs, chrome, and SEO stay as they are.
2. **Try Turmeric in-app viewer** (OD2) -- fetches `index.json` +
   fragments, renders in a pane; never navigates away from the REPL.
3. **Offline** -- the service worker precaches the pack (OD3); the CLI/local
   story reuses the same rendered output (OD4).

## 3. The plan

### OD1 -- emit the docs pack

- **Do:** teach `genguides.py` and `gendocs.py` a `--emit-pack <dir>` flag
  (spices folded in the same way `--extra-json` already folds spice names);
  a tiny merger writes `index.json`. Wire into the `docs:` recipe so
  `just docs` always produces site HTML *and* pack. Add the size-budget
  check here.
- **Accept:** `just docs` on a clean tree produces a pack whose `index.json`
  lists all 137 guides and every stdlib module; every fragment parses as an
  HTML fragment; every intra-pack link resolves (a link-checker pass in the
  emitter, same spirit as `tools/check-guide-pairs.py`); budget check trips
  on a deliberately oversized fixture input.
- **Note:** `genguides.py` imports the `markdown` module -- absent from this
  build container. The recipe already runs on machines that have it; CI for
  OD needs it declared (`tools/requirements.txt` or an inline check with a
  clear error, matching the loud-failure style of `web-dev:`).

### OD2 -- the in-app docs browser in Try

- **Do:** a docs pane in `/try/` (sibling of the existing doc panel --
  reachable from a toolbar button, a `:docs` meta-command, and the doc
  panel's "Open full docs" link, which stops navigating away and opens the
  pane instead). Contents:
  - nav tree from `index.json` (guides by category; API by module; spices
    when present);
  - client-side search over `index.json`'s `words` plus the existing
    `doc-names.json` (one search box, two result kinds: pages and symbols --
    symbols keep routing to the existing docstring panel);
  - fragment rendering with in-pane link interception (pack-relative links
    stay in the pane; external links open new tabs);
  - deep links: `/try/#doc=guides/backtracking-guide` so a docs location is
    shareable and restorable, composing with the existing URL-sharing hash
    scheme rather than clobbering it;
  - symbol-level integration: the docstring panel's per-symbol view gains a
    "full module docs" link into the pane's `api/<module>.html` anchor.
- **Style:** fragments carry no CSS, so the pane owns typography -- reuse the
  site's guide styles (`site.css` classes) scoped under the pane so guides
  look like the site, not like unstyled HTML. Code blocks get the same
  Prism/Monaco-adjacent highlighting the site uses. Mobile: the pane becomes
  a full-screen sheet (Try is responsive today; keep it so).
- **One editor-shaped opportunity, cheap here and nowhere else:** guides are
  full of runnable snippets. A "load into editor" affordance on `turmeric`
  /`sweet-exp` code blocks in the pane (copy into the Monaco buffer) turns
  every guide into an interactive tutorial for free. In-scope for OD2; the
  existing tutorial system stays untouched.
- **Accept:** open pane, browse a guide, follow a cross-link, search a term,
  deep-link restores; REPL state (buffer, console history) survives all of
  it; Playwright specs in `web/tests/` cover each.

### OD3 -- make it work offline

- **Do:** add the pack to `sw.js` precaching. Two mechanics, both already
  half-built in that file:
  - `PRECACHE_URLS` cannot list fragments statically, so the install step
    additionally fetches `/docs-pack/index.json` and precaches every path it
    lists -- the existing individually-added, skip-on-404 pattern extends
    naturally, and `index.json` doubling as the precache manifest means no
    second manifest to drift;
  - `/docs-pack/*` at runtime is **cache-first with background revalidate**
    (the existing `cacheFirst`), not network-first -- the pack is versioned
    by the SW cache name, and `CACHE_VERSION` already rotates per release, so
    staleness is bounded by release cadence, same as the wasm.
  - `/docs/html/*` (the full site pages) stays network-first; but its offline
    *fallback* changes from "serve the REPL shell" (today's confusing
    behavior) to a tiny offline page saying "you're offline -- these docs are
    available in Try" linking `/try/#doc=...` for the matching page when the
    path maps into the pack.
- **No toggle.** The pack is precached on install alongside the wasm,
  unconditionally, on every platform. Concretely this means: no setting, no
  prompt, no first-run "would you like docs offline?", and no code path in
  which the docs pane is present but its content is not. If the OD1 budget
  check ever fights this, OD1's shrink ladder applies -- the pack gets
  smaller, the guarantee does not get weaker.
- **Make the guarantee visible, since it is now unconditional.** The install
  is already failure-tolerant per URL (a fetch that 404s is skipped rather
  than bricking the worker), which means a partially precached pack is
  possible on a flaky first load. Record what landed: the SW writes a small
  `pack-status` entry (expected count from `index.json`, actual cached
  count, version) that the docs pane reads, so the pane can say "docs ready
  for offline" or "N of M pages cached -- reconnect to finish" instead of
  discovering a hole mid-flight. Re-attempt the missing entries on the next
  load rather than waiting for a version bump.
- **Accept:** Playwright offline spec, written to assert the *unconditional*
  guarantee rather than a happy path -- load `/try/` once online **without
  ever opening the docs pane**, flip the context offline
  (`context.setOffline(true)`), reload, then open the pane for the first
  time and navigate guides, API pages, cross-links and search: all green
  with zero network requests. (Visiting docs before going offline would pass
  even with today's network-first behavior, which is exactly the bug this
  phase fixes; the spec must not do it.) Plus: `pack-status` reports
  complete after a clean install; a spec that fails a subset of fragment
  fetches on install sees the pane report partial state and sees the
  missing entries recovered on the next online load. Bump `VERSION`,
  redeploy, confirm the old pack is evicted (the existing `activate` logic,
  now covering the pack cache).

### OD4 -- offline outside the browser (the other half of problem 1)

Cheapest-first; each step is independently shippable:

- **`tur doc <name>` in the CLI.** The lookup table (`docstrings.tur`) ships
  in the binary's stdlib already; the REPL's `(doc ...)` just isn't reachable
  from a shell. A `tur doc map/insert` subcommand printing the same text is a
  small CLI wrapper over an existing table and needs no rendered docs at all.
- **A docs tarball per release.** `just docs` output
  (`docs/html/` + the pack) archived as `turmeric-docs-<version>.tar.gz` and
  attached to the GitHub release by the `cut-*-release` flow. Anyone can
  download and open `index.html` locally; zero new rendering code.
- **`tur docs [--open|--serve]`.** Looks for installed rendered docs
  (`share/doc/turmeric/` when the Homebrew formula installs the tarball --
  one formula stanza), falls back to opening the online docs with a note.
  `--serve` binds a localhost static server for the strict-file-URL cases
  (some browsers block `file://` cross-page navigation).
- **Not doing here:** man pages for the language (the `man/` dir already
  covers the CLI), or embedding rendered guide HTML in the `tur` binary.
- **Accept:** on a machine with the formula install: `tur doc vec-push`
  prints a docstring with no network; `tur docs --open` lands in local
  guides; both degrade with clear messages when docs are absent.

### OD5 -- tidy the serving story

Two small repairs surfaced by the audit, worth doing while in the area:

- **Stop shipping the whole docs source tree.** The `web/public/docs ->
  ../../docs` symlink copies archive/reported/upcoming markdown into every
  deploy. Point the symlink (or a copy step) at `docs/html/` only, and fix
  the `/docs/html/*` URL space accordingly (or keep URLs stable with a
  narrower symlink `web/public/docs/html -> ../../../docs/html`). Decide
  in-phase; keep site URLs unchanged.
- **Docs presence check for the pack** in `web-dev:`, mirroring the existing
  loud failures for `turmeric.wasm` / `doc-names.json`, so a fresh clone
  serving a docs-pane that finds nothing is a build error with instructions,
  not a mystery.

### Recommended order

OD1 -> OD2 -> OD3 ship together as the user-visible feature (each lands
independently, but the payoff is the third). OD4's first bullet (`tur doc`)
is a quick win that can go any time; the tarball rides the next release cut;
OD5 folds into whichever of OD1/OD3 touches the neighboring lines first.

## 4. Risks

| Risk | Mitigation |
|---|---|
| Site pages and pack drift apart | One renderer emits both (fragment rendered once, wrapped twice); drift is structurally impossible rather than policed |
| Pack bloats the PWA | Size budget enforced at generation (build error, not a runtime surprise); images counted; over-budget is answered by OD1's shrink ladder -- precaching stays unconditional |
| First load is flaky, so the "always offline" promise is quietly untrue | SW records a `pack-status` (expected/actual/version); the pane surfaces incomplete state and the missing entries are re-fetched on the next load |
| Stale offline docs after release | Existing `CACHE_VERSION`-from-`VERSION` rotation already evicts; `index.json` carries the version and the pane displays it |
| SW install slowed by many fragment fetches | Fragments precache after the shell, so the REPL is usable before the pack finishes; install is already per-URL and failure-tolerant, and `pack-status` makes an incomplete pack visible rather than silent |
| Deep-link hash scheme collides with URL code sharing | `#doc=` composes with the existing compressed-hash format; a Playwright spec pins both coexisting |
| `genguides.py` markdown dependency missing on a build machine | Loud preflight check with install instructions, in the style of the existing `web-dev:` guards |
| Symlink narrowing (OD5) breaks site URLs | Keep the public URL space fixed; only the filesystem mapping changes; prod smoke tests (`playwright.config.prod.js`) cover the doc routes |

## 5. Open questions

1. Should spices docs be in the pack when `../turmeric-spices` is absent at
   build time -- i.e., is the deploy machine guaranteed to have the sibling
   checkout? (Today's `doc-names.json` merge has the same dependency, so
   whatever it does, the pack should match.) Note this interacts with
   precache-always: if spice pages can be missing from a given build, the
   pane must not offer nav entries for them, or "installed means complete"
   becomes false through the side door.
2. Does the tour (`web/tour/`) want the same pane, or is Try enough for v1?
