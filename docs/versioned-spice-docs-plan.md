# Versioned Spice Docs Plan

> **Status:** Draft Plan (design only, no build)
> **Last Updated:** 2026-05-24
> **Type:** Tooling / Docs / Web Platform
> **Predecessor:** [`per-spice-docs-plan.md`](per-spice-docs-plan.md)

---

## Overview

Today every page under `/docs/html/spices/<name>/` reflects whatever is
checked out in the sibling `../turmeric-spices/` repo at doc-generation
time -- effectively the tip of `main`. The tag-per-spice release scheme
(`json-v0.1.0`, `opengl-v0.1.0`, ...) is already established, but the
website has no way to show docs for a tagged release alongside the
in-progress main version.

This plan adds versioned per-spice docs without disturbing the existing
tip-of-main flow. The high-level shape:

1. The on-disk layout grows a version segment:
   `/docs/html/spices/<name>/<version>/index.html` and `.../api/`.
2. `<version>` is the literal tag suffix after `<name>-v` (so
   `json-v0.1.0` becomes the version segment `v0.1.0`). The pseudo-version
   `main` is reserved for the current sibling-repo HEAD.
3. The un-versioned `/docs/html/spices/<name>/` becomes a thin redirect to
   the latest stable tag (falling back to `main` when no tagged release
   exists).
4. A version dropdown in the per-spice page header lets the user switch
   between versions without losing their scroll position on the API page.
5. The generator (`tools/genspices.py`) gains a `--versions` flag that
   accepts a list of tag suffixes per spice; for each it `git
   worktree`s the sibling repo at that tag and re-runs the existing
   per-spice render against it.
6. CI builds a fixed set of "supported versions" per release; HEAD-only
   builds for local dev stay one-step (no worktrees needed).

Out of scope for v1: cross-version diffing, search across versions,
draft / prerelease channels, dating each release in the dropdown.

---

## Why version docs at all

- **API stability boundary.** Once spices ship with tagged releases that
  users pin in `build.tur`, the docs need to match the tag the user
  pinned. Linking a `json-v0.1.0` user to `main`-tip docs creates churn
  and confusion when the API drifts.
- **Deprecation lifecycle.** Versioned docs are where deprecation
  markers (`^deprecated`, `Since:` notes) actually become useful -- you
  can see what was deprecated *between* `v0.1.0` and `v0.2.0`.
- **Cross-linking from `tur add`.** Eventually `tur add --ref json-v0.1.0`
  could print a URL pointing to the exact docs for that tag. That URL
  needs to exist and not 404.

---

## Current state (what to preserve)

- `tools/genspices.py` walks `../turmeric-spices/spices/*/` and renders
  per-spice pages plus a top-level index. It assumes HEAD of the sibling
  repo is the only thing to document.
- `tools/gendocs.render_tree(src, out, brand=...)` is the per-spice API
  renderer. It is content-agnostic; passing a different `src` is enough
  to generate against a different checkout.
- The top header (`PAGE_HEADER` in `tools/genspices.py`) hard-codes the
  nav links; the spice-name and version dropdown would live in a
  per-spice sub-header beneath it.
- `web/main.js` doc-search consults `web/public/doc-names.json` which
  already supports a `spice` tag per entry. A `version` field can be
  added later for cross-version search but is **not** required for v1.

---

## Target output layout

```
docs/html/spices/
  index.html                       -- top index, lists every spice + its latest version
  <name>/
    index.html                     -- redirect to <name>/<latest>/index.html
    api/                           -- (deleted) redirect lives here too
    main/                          -- the rolling "tip of main" docs
      index.html
      api/
        index.html
        <module>.html
        ...
    v0.1.0/                        -- frozen release docs
      index.html
      api/
        ...
    v0.2.0/
      ...
```

Properties:

- `<name>/index.html` always exists and always works -- it 302s (or
  emits a `<meta refresh>`) to the latest stable version.
- `<name>/main/` is regenerated on every `just docs` run.
- `<name>/<vX.Y.Z>/` is generated **once** when a release lands and
  thereafter is immutable in the website build artifact.
- The version dropdown lives in a sub-header rendered into every
  per-spice page, including `main`.

### URL scheme

| URL | Behavior |
| --- | --- |
| `/docs/html/spices/` | Top spices index (unchanged). |
| `/docs/html/spices/json/` | Redirect to `/docs/html/spices/json/<latest>/`. |
| `/docs/html/spices/json/main/` | Always points at sibling-repo HEAD. |
| `/docs/html/spices/json/v0.1.0/` | Frozen v0.1.0 docs. |
| `/docs/html/spices/json/v0.1.0/api/` | API reference for v0.1.0. |
| `/docs/html/spices/json/latest/` | Optional shortcut: alias for current latest tag. |

### Latest-tag resolution

"Latest" is the highest semver among the tags matching
`^<name>-v\d+\.\d+\.\d+$` in the sibling repo at generation time, after
filtering out prereleases (`-alpha`, `-rc`, ...). Stored as
`docs/html/spices/<name>/.latest` so the redirect page can read it from
the build artifact without re-running tag lookup at request time.

---

## Components

### genspices.py: tag discovery and worktree fan-out

New CLI surface:

```
tools/genspices.py --out docs/html/spices/ \
                   --versions main,v0.1.0 \
                   --versions-file spices-versions.txt
```

- `--versions` is a global flag: every spice gets these versions if
  the corresponding tag exists.
- `--versions-file` is per-spice: a TOML or JSON file mapping spice
  short-name to a list of version segments. CI uses this.
- When `--versions` is absent, behavior is identical to today (single
  HEAD render at the un-versioned path) -- this keeps `just docs`
  one-step and fast for local dev.

Per-spice rendering loop becomes:

```python
for meta in metas:
    versions = resolve_versions(meta, cli_versions, versions_file)
    if not versions:
        # Legacy mode: render at <name>/ directly.
        render_one(meta, out_dir / meta['name'], version=None)
        continue
    for version in versions:
        src_root = checkout_version(meta, version)   # worktree or HEAD
        render_one(meta, out_dir / meta['name'] / version,
                   src_override=src_root, version=version)
    write_latest_redirect(meta, out_dir / meta['name'], versions)
```

`checkout_version` for `main` returns the sibling repo as-is. For a
tagged version, it materializes a `git worktree add` to a temp
directory at the matching tag (e.g. `json-v0.1.0`) and returns that.
Worktrees are torn down after rendering.

### Per-version sub-header (HTML)

A small bar inserted between the site header and the page content on
every `<name>/<version>/` page:

```html
<div class="spice-version-bar">
  <span class="spice-name">tur-json</span>
  <select class="spice-version-select" aria-label="Version">
    <option value="main">main (tip)</option>
    <option value="v0.2.0" selected>v0.2.0 (latest)</option>
    <option value="v0.1.0">v0.1.0</option>
  </select>
  <a class="spice-source" href="https://github.com/rjungemann/turmeric-spices/tree/json-v0.2.0/spices/json">source</a>
</div>
```

JS behavior:

- Selecting a version navigates to the same relative path under the
  chosen version: from `.../json/v0.1.0/api/json-parse.html` to
  `.../json/v0.2.0/api/json-parse.html`.
- Anchor preserved when present.
- Falls back to the version's index when the same page doesn't exist
  (e.g. a module was renamed across versions).

### Latest-redirect page

`docs/html/spices/<name>/index.html` (un-versioned) becomes a small
HTML page that does:

```html
<meta http-equiv="refresh" content="0; url=v0.2.0/">
<link rel="canonical" href="/docs/html/spices/json/v0.2.0/">
<p>Redirecting to <a href="v0.2.0/">tur-json v0.2.0</a>...</p>
```

`<meta refresh>` is sufficient -- no server-side route changes needed
(Cloudflare Pages serves static files directly).

Same shape for `<name>/api/index.html` (un-versioned API redirect).

### Top-level index update

The top spices table grows one column:

| Spice | Description | Tier | C dep | Latest | Docs |
| --- | --- | --- | --- | --- | --- |
| `tur-json` | JSON parsing... | 3 -- cmake-dep | yyjson 0.10.0 | `v0.2.0` (2026-04) | [docs](json/) |

"Latest" comes from the `.latest` file written per spice. The date is
optional polish.

### doc-names.json schema bump

For v1, leave `doc-names.json` pointed at the latest tagged version of
every spice. Add an optional `version` field per entry so future search
can disambiguate; absent = "latest for that spice".

A follow-up (v2) could emit one JSON payload per version and have the
web REPL fetch the version that matches the user's current `build.tur`
pins -- but that's well beyond v1 scope.

### docstrings.tur stays stdlib-only

The runtime `(doc name)` table built into the WASM binary still only
covers stdlib. Spice symbols live in the web-only `doc-names.json`.
Versioning that table is out of scope: when a user calls `(doc
json-parse)` in the REPL there is no version context to resolve
against. Leave it as-is.

---

## Phases

### Phase V0 -- prerequisites

- Confirm tag convention is stable: `<name>-v<MAJOR>.<MINOR>.<PATCH>`
  with no prerelease suffixes in shipped tags. **Required** before V1
  starts. If prereleases will exist, settle filter rules first.
- Add `.gitignore` entry for the temp worktree root (e.g.
  `.spices-worktrees/`).

### Phase V1 -- single tagged version per spice

- Implement `checkout_version`, `resolve_versions`,
  `--versions`/`--versions-file` in `tools/genspices.py`.
- Render one tagged version per spice in addition to `main`.
- Write `<name>/.latest` and the redirect index.
- Render the version sub-header on every per-spice page (sub-header
  empty/hidden when only `main` exists).
- Keep `--versions` opt-in so local `just docs` stays fast.

### Phase V2 -- multi-version selector

- Render the dropdown when 2+ versions exist.
- Add the JS that preserves the relative path on version switch.
- Update the top index to show the latest version column.

### Phase V3 -- CI integration

- Add a `just docs-release` target that takes `--versions-file
  spices-versions.txt` and emits the full multi-version tree.
- Wire CI to use `docs-release` and check `spices-versions.txt` in to
  the repo so the supported version matrix is reviewable in PRs.
- Add a small validator that fails when a tag listed in
  `spices-versions.txt` does not exist in the sibling repo.

### Phase V4 -- polish

- Add release dates (`git log -1 --format=%cs <tag>`) next to each
  version in the dropdown.
- Add a "deprecated in vX.Y.Z" annotation surface in the rendered
  cards when a defn carries `^deprecated` plus a `Since:` later than
  the current page's version.
- Consider migrating the un-versioned API redirect to a JS-shim
  instead of `<meta refresh>` so anchors survive cleanly.

---

## Open questions

- **Worktree cost.** Each tagged version requires a `git worktree
  add` + render + tear-down. On CI for 20 spices x 3 versions each,
  that's 60 worktree operations. Likely fine, but worth measuring on
  the largest spice (raylib) before committing.
- **Tag history for unborn spices.** What does the dropdown look like
  for a spice that has zero tags? Current proposal: only render
  `main`, hide the dropdown, no redirect at the un-versioned path
  (since "latest" doesn't exist). Confirm this is what we want.
- **Latest of latest.** If `tur-json` ships `v0.1.0` and `v0.2.0-rc1`,
  is "latest" the stable `v0.1.0` or the rc? Proposal: filter
  prereleases from "latest" but **render** them when listed in
  `--versions-file`. The rc just doesn't become the redirect target.
- **doc-names.json drift.** When a spice ships `v0.2.0` that removes a
  function, the latest `doc-names.json` won't have that symbol. Users
  pinned at `v0.1.0` could be confused when search returns nothing.
  Acceptable for v1; a v2 follow-up could expose per-version JSON.

---

## Risks and trade-offs

- **Build-time bloat.** Multi-version rendering multiplies HTML output
  by the number of supported versions. Keep the supported matrix tight
  (latest + previous one or two minor versions).
- **Stale tag points.** Once a v0.1.0 doc page exists in the build
  artifact, it ships forever even after v0.1.0 is unsupported. That
  is the intended behavior of versioned docs, but it means the
  cleanup story is "delete the directory in a future release" rather
  than a regenerate-from-scratch pipeline.
- **GitHub-fallback continues to be absent.** Generating a tagged
  version requires the sibling repo with full history available, same
  as before. CI must `git clone` (not just fetch HEAD) before
  invoking `docs-release`.
- **JS-driven version switching.** The dropdown's "preserve relative
  path" behavior depends on JS; users with JS disabled get a plain
  hyperlink to the version's index. Acceptable trade-off; an SSR
  approach is not worth the complexity.

---

## Out of scope

- Cross-version search bar in the web REPL.
- Diff view between two versions of the same API page.
- Tagged-spice docs for the WASM `(doc name)` runtime lookup.
- Auto-generating release notes from `Since:` and `Deprecated:`
  fields across versions.
- Localization of the version dropdown.
