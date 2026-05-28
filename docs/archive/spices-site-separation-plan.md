# Plan: Separate Spice Docs into spices.turmeric-lang.com

> **Status:** Draft
> **Last Updated:** 2026-05-25
> **Type:** Infrastructure / Docs
> **Tracked across repos:** `turmeric` (cleanup), `turmeric-spices` (new site)

---

## Goal

Move all generated spice documentation out of the `turmeric` repo and into the
`turmeric-spices` repo, served at its own Cloudflare domain
(`spices.turmeric-lang.com`). The main site (`turmeric-lang.com`) keeps stdlib
API docs and guides but no longer owns or generates spice HTML.

**Why:** the current arrangement causes tight coupling between the two repos.
`just docs` in `turmeric` must have `../turmeric-spices/` checked out, a
sibling-repo assumption that is fragile in CI, blocks clean release tagging,
and means both repos must be updated together whenever the doc pipeline
changes.

---

## Scope summary

| Area | Current location | After |
|------|-----------------|-------|
| Generated HTML | `turmeric/docs/html/spices/` | `turmeric-spices/docs/html/` |
| Doc generator | `turmeric/tools/genspices.py` | `turmeric-spices/tools/genspices.py` |
| Style assets | borrowed from `turmeric/web/` | copied into `turmeric-spices/web/` |
| Cloudflare Worker | `turmeric/web/wrangler.jsonc` | `turmeric-spices/web/wrangler.jsonc` |
| Justfile targets | `turmeric/Justfile` `spices`, `check-spices` | `turmeric-spices/Justfile` |
| Nav link (main site) | `/docs/html/spices/` (relative) | `https://spices.turmeric-lang.com` (absolute) |
| Nav link (spices site) | `/` (turmeric-lang.com) | `https://turmeric-lang.com` (absolute) |
| REPL symbol search | merged into `web/public/doc-names.json` | fetched at runtime from `spices.turmeric-lang.com/doc-names-spices.json` |

---

## Phase 1 -- Set up the spices site in `turmeric-spices`

### 1.1 Copy and adapt `tools/genspices.py`

Copy `turmeric/tools/genspices.py` into `turmeric-spices/tools/genspices.py`.
Change the following:

- Remove the `SPICES_REPO = Path('../turmeric-spices')` assumption; replace
  with `SPICES_REPO = Path('.')` (the script now runs from inside the repo).
- Change `--out` default from `docs/html/spices/` to `docs/html/`.
- Update `PAGE_HEADER` nav links:
  - "Home" `href` from `/` to `https://turmeric-lang.com`
  - "Guides" `href` from `/docs/html/guides/` to `https://turmeric-lang.com/docs/html/guides/`
  - "API Docs" `href` from `/docs/html/api/` to `https://turmeric-lang.com/docs/html/api/`
  - Remove the "Spices" nav item (already on the spices site).
  - "Try It" `href` from `/try` to `https://turmeric-lang.com/try`
- Update the logo `href` from `/` to `https://turmeric-lang.com`.
- Update the footer from `tools/genspices.py` to `tools/genspices.py (turmeric-spices)`.
- Update `GITHUB_BASE` if the spices repo URL changes.
- Import helpers (`SIDEBAR_TOGGLE_JS`, etc.) locally: copy the minimal set
  from `turmeric/tools/genguides.py` and `turmeric/tools/gendocs.py` into
  `turmeric-spices/tools/genhelpers.py`, or vendor them inline. The dependency
  on the `turmeric` repo's `tools/` directory must be severed.

### 1.2 Copy style assets

Create `turmeric-spices/web/` with:

```
web/
  public/
    logo.svg          -- copy from turmeric/web/public/
    logo-icon.svg     -- copy from turmeric/web/public/
    favicon.ico       -- copy from turmeric/web/public/
  styles/
    style.css         -- copy from turmeric/docs/html/api/style.css
    styles.css        -- copy from turmeric/web/styles.css
    vars.css          -- copy from turmeric/web/vars.css
    site.css          -- copy from turmeric/web/site.css
  wrangler.jsonc
  package.json
```

The generated HTML pages reference `style.css` via relative paths. For the
spices site the equivalent is at the root so relative references from
`docs/html/<name>/api/style.css` work unchanged.

### 1.3 Add Cloudflare Worker config

Create `turmeric-spices/web/wrangler.jsonc`:

```jsonc
{
  "$schema": "node_modules/wrangler/config-schema.json",
  "name": "turmeric-spices",
  "main": "./worker.js",
  "compatibility_date": "2026-05-25",
  "observability": { "enabled": true },
  "assets": {
    "directory": "./dist",
    "binding": "ASSETS"
  },
  "compatibility_flags": ["nodejs_compat"],
  "routes": [
    { "pattern": "spices.turmeric-lang.com", "custom_domain": true }
  ]
}
```

Create `turmeric-spices/web/worker.js` (minimal static-asset pass-through,
same pattern as `turmeric/web/worker.js`).

Create `turmeric-spices/web/package.json`:

```json
{
  "name": "turmeric-spices-site",
  "version": "0.1.0",
  "type": "module",
  "scripts": {
    "deploy": "wrangler deploy --config wrangler.jsonc"
  },
  "devDependencies": {
    "wrangler": "^4.93.0"
  }
}
```

### 1.4 Add Justfile

Create `turmeric-spices/Justfile` (or extend if one already exists):

```just
# Generate per-spice HTML docs from this repo's spices/ directory.
docs:
    python3 tools/genspices.py --out docs/html/ --emit-json docs/html/doc-names-spices.json

# Deploy docs to spices.turmeric-lang.com via Cloudflare.
# Requires wrangler to be authenticated (wrangler login).
deploy-web: docs
    cp -r docs/html web/dist
    cd web && npm install && npm run deploy

# Validate turmeric+sweet-exp code pairs in all spice READMEs.
check-docs:
    python3 tools/check-guide-pairs.py --spices
```

### 1.5 Publish `doc-names-spices.json` as a static asset

Ensure `just docs` writes `docs/html/doc-names-spices.json`. The Cloudflare
assets directory serves it at:

```
https://spices.turmeric-lang.com/doc-names-spices.json
```

This URL becomes the canonical source for spice symbol search (see Phase 3).

### 1.6 Add CI workflow

Create `.github/workflows/deploy-docs.yml` in `turmeric-spices`:

```yaml
name: Deploy spice docs
on:
  push:
    branches: [main]
  workflow_dispatch:
jobs:
  deploy:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with: { python-version: "3.12" }
      - run: pip install markdown
      - run: python3 tools/genspices.py --out docs/html/ --emit-json docs/html/doc-names-spices.json
      - uses: actions/setup-node@v4
        with: { node-version: "20" }
      - run: cd web && npm ci && npm run deploy
        env:
          CLOUDFLARE_API_TOKEN: ${{ secrets.CLOUDFLARE_API_TOKEN }}
```

---

## Phase 2 -- Clean up the `turmeric` repo

### 2.1 Update `just docs` in `Justfile`

Remove the dependency on `just spices` and the `--extra-json` flag:

```just
# Before:
docs: guides spices
    python3 tools/gendocs.py stdlib/ --out docs/html/api/ --emit-tur stdlib/docstrings.tur --emit-json web/public/doc-names.json --extra-json docs/html/spices/doc-names-spices.json

# After:
docs: guides
    python3 tools/gendocs.py stdlib/ --out docs/html/api/ --emit-tur stdlib/docstrings.tur --emit-json web/public/doc-names.json
```

Remove the `spices` target entirely.

### 2.2 Remove or gate `check-spices` / `check-docs`

Remove the `check-spices` target and remove `check-spices` from the
`check-docs` prerequisite. The equivalent check lives in
`turmeric-spices/Justfile check-docs`.

If `tools/check-guide-pairs.py --spices` contains logic that only applies to
the sibling repo path, remove the `--spices` mode. The `--spices` logic can be
copied/adapted into `turmeric-spices/tools/check-guide-pairs.py`.

### 2.3 Update `tools/gendocs.py` -- remove `--extra-json` merge

Remove the `--extra-json` CLI flag and the code path that reads and merges an
extra JSON file. The stdlib doc-names JSON should only contain stdlib symbols.

### 2.4 Update `PAGE_HEADER` in `tools/genspices.py` (if keeping a copy)

If a copy of `genspices.py` is kept in `turmeric` for local development
convenience, update `PAGE_HEADER` to link the "Spices" nav item to
`https://spices.turmeric-lang.com` instead of `/docs/html/spices/`.

Alternatively, delete `tools/genspices.py` from the `turmeric` repo entirely
once the spices site is stable.

### 2.5 Update `PAGE_HEADER` in `tools/genguides.py` and `tools/gendocs.py`

Both generators embed a `PAGE_HEADER` with a nav link:

```html
<a href="/docs/html/spices/">Spices</a>
```

Change to:

```html
<a href="https://spices.turmeric-lang.com">Spices</a>
```

This affects every regenerated guide and stdlib API page.

### 2.6 Delete `docs/html/spices/`

Once the spices site is live and confirmed working, delete the entire
`docs/html/spices/` tree from the `turmeric` repo. These files are no longer
served from `turmeric-lang.com`.

Add `docs/html/spices/` to `.gitignore` to prevent accidental re-commits if
`genspices.py` is run locally against the old path.

### 2.7 Update Cloudflare `wrangler.jsonc` routes

No changes needed; the existing `turmeric-lang.com` / `www.turmeric-lang.com`
routes are unchanged. The spices subdomain is a separate Cloudflare project.

---

## Phase 3 -- Update the web REPL symbol search

Currently `just docs` merges spice symbols into `web/public/doc-names.json`
via `--extra-json`. After Phase 2.1 that merge is gone, so the web REPL search
bar will no longer surface spice symbols.

Two options:

**Option A (recommended): fetch at runtime.**
In the web REPL JavaScript (`web/main.js` or wherever the search bar is
populated), add a secondary fetch:

```js
const spiceNames = await fetch('https://spices.turmeric-lang.com/doc-names-spices.json')
  .then(r => r.json())
  .catch(() => []);
const allNames = stdlibNames.concat(spiceNames);
```

This keeps the two repos fully decoupled: when the spices site publishes a
new JSON, the REPL picks it up automatically on next page load without
requiring a `turmeric` redeploy.

**Option B: drop spice symbols from search.**
Simpler -- the REPL search only covers stdlib. Add a note in the UI that
spice symbols are searchable at `spices.turmeric-lang.com`.

Decide before completing Phase 2.3. If Option A, the `--extra-json` removal
is unconditional; if Option B, note it in a code comment.

---

## Phase 4 -- Update affected docs and guides

### Guides in `docs/guides/`

Search for references to `/docs/html/spices/` or relative paths to the spices
directory. Guides most likely to need updates:

| File | Change |
|------|--------|
| `consuming-spices-guide.md` | Link "spice docs" to `https://spices.turmeric-lang.com` |
| `developing-spices-guide.md` | Same; update the "docs are at..." passage if present |
| `package-management-guide.md` | Update any spice docs link |
| `cloudflare-deployment-guide.md` | Mention the second Worker for spices if relevant |

Run after Phase 2.5:
```sh
grep -rn 'docs/html/spices\|/spices/' docs/guides/
```
and update every hit.

### `README.md` at repo root

If the main README links to spice docs via a relative or absolute URL, update
to `https://spices.turmeric-lang.com`.

### `docs/html/spices/index.html` (generated)

Regenerated by Phase 1 genspices.py; no manual edits needed.

---

## Phase 5 -- Cloudflare DNS / dashboard

1. In the Cloudflare dashboard for the `turmeric-lang.com` zone, add a CNAME:
   ```
   spices  CNAME  <workers-subdomain>.workers.dev  (proxied)
   ```
   or add `spices.turmeric-lang.com` as a Custom Domain on the
   `turmeric-spices` Worker.

2. In the `turmeric-spices` Worker settings, add `spices.turmeric-lang.com`
   as a custom domain (same flow as the existing `turmeric-lang.com`
   registration).

3. Verify `https://spices.turmeric-lang.com` resolves before removing
   `docs/html/spices/` from the main site (Phase 2.6).

---

## Execution order

```
Phase 1   -- build the new site (no deletions yet)
Phase 5   -- configure DNS and verify the domain resolves
Phase 3   -- decide Option A vs B; implement REPL change
Phase 2   -- clean up turmeric repo (run just docs, verify)
Phase 4   -- update guides
```

Do not delete `docs/html/spices/` from `turmeric` until the new domain is
confirmed live and all nav links have been regenerated.

---

## Files changed (summary)

### `turmeric` repo

| File | Change |
|------|--------|
| `Justfile` | Remove `spices` target; remove `--extra-json` from `docs`; remove `check-spices` from `check-docs` |
| `tools/genspices.py` | Update nav href; or delete |
| `tools/genguides.py` | Update `PAGE_HEADER` Spices link to `https://spices.turmeric-lang.com` |
| `tools/gendocs.py` | Update `PAGE_HEADER` Spices link; remove `--extra-json` |
| `tools/check-guide-pairs.py` | Remove `--spices` mode |
| `web/main.js` (or equivalent) | Option A: fetch spice symbol JSON from spices domain |
| `docs/guides/consuming-spices-guide.md` | Update spice docs URL |
| `docs/guides/developing-spices-guide.md` | Update spice docs URL |
| `docs/guides/package-management-guide.md` | Update spice docs URL |
| `docs/html/spices/` | Delete entire directory |
| `.gitignore` | Add `docs/html/spices/` |

### `turmeric-spices` repo (new files)

| File | Notes |
|------|-------|
| `tools/genspices.py` | Adapted from turmeric; standalone |
| `tools/genhelpers.py` | Vendored rendering helpers from turmeric's genguides/gendocs |
| `tools/check-guide-pairs.py` | Copied/adapted spices mode |
| `web/wrangler.jsonc` | `spices.turmeric-lang.com` Worker config |
| `web/worker.js` | Static asset pass-through |
| `web/package.json` | wrangler dep + deploy script |
| `web/public/` | logo, favicon (copied from turmeric) |
| `web/styles/` | style.css, vars.css, site.css (copied) |
| `Justfile` | `docs`, `deploy-web`, `check-docs` targets |
| `.github/workflows/deploy-docs.yml` | CI: generate + deploy on push to main |
