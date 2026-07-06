---
title: Tighten `:exports` map-form parsing so effect-row literals stop pretending to be maps -- Plan
category: Planning
description: The manifest reader silently accepts `#fx{...}` (an effect-row literal) as the value of `:exports` because both `#fx{...}` and `#map{...}` share the F_MAP tag and `parse_exports` only checks the tag. This produces a category-error API surface -- effect rows are not keyed dictionaries -- that has already been copied into two fixtures and will be copied again the next time an LLM edits a manifest. This plan closes the hole at the parser (reject non-map provenances with a real diagnostic) and at the fixtures (convert to `#map{...}`), and adds regression coverage so the mistake cannot re-land silently.
---

# Tighten `:exports` map-form parsing -- Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-07-05
> **Type:** Parser hardening + fixture cleanup
> **Scope:** small (single function + two fixtures + one new fixture)

## Why this exists

`build.tur`'s `:exports` field is defined as either:

- a **map** whose keys are module names (`"app/main"`) and whose values are
  vectors of exported symbols, or
- a **vector** of source-file paths (legacy).

Today the parser at `src/compiler/pkg.c:415` (`parse_exports`) only checks
`f->tag == F_MAP`. The Turmeric reader tags **three** different surface
syntaxes with `F_MAP`:

| Surface syntax   | `F_MAP.fx_prov`      | Intended semantics |
|------------------|----------------------|--------------------|
| `#{...}`         | `PROV_FX_LEGACY`     | legacy bare map / legacy effect row |
| `#map{...}`      | `PROV_NONE` (map)    | explicit map literal |
| `#fx{...}`       | `PROV_FX_EXPLICIT`   | **explicit effect row** (not a map) |

Because `parse_exports` looks only at the tag, `:exports #fx{...}` -- a
literal effect-set -- is silently accepted as if it were a map. Two fixtures
in the tree already ship this bug:

- `tests/fixtures/build-project-smoke/build.tur:5`
- `tests/fixtures/result-typedef-multi-module/build.tur:5`

An LLM editing manifests reaches for the wrong spelling because both
"explicit map" and "explicit effect row" show up in the fx-row-syntax-rename
migration notes and the surface forms are visually parallel (`#map{` vs
`#fx{`). The parser accepts the wrong one, no diagnostic fires, and the
mistake propagates. This will keep happening unless the parser stops
accepting effect-row provenances in a value slot that means "map."

## Non-goals

- Not touching the effect-row reader (`read_fx_row`) or the semantics of
  `#fx{...}` anywhere it is legitimately used (function effect annotations).
- Not deprecating bare `#{...}` maps; that is a separate migration.
- Not changing the on-disk layout of `PkgManifest.exports` -- still a
  `char **` of module-name keys.

## Design

### Change 1 -- reject effect-row provenances at the map site

In `parse_exports` (`src/compiler/pkg.c:415`):

1. If `f->tag == F_MAP`, additionally require `f->fx_prov == PROV_NONE` **or**
   `f->fx_prov == PROV_FX_LEGACY` **only when we can distinguish a legacy
   bare map from a legacy bare effect row at this site**. Since the value
   slot of `:exports` is unambiguously "map or vector," the safest tightening
   is:
   - Accept `PROV_NONE` (explicit `#map{...}`) silently.
   - Accept `PROV_FX_LEGACY` (bare `#{...}`) silently -- this preserves every
     existing spice manifest in the wild that predates `#map{...}`.
   - **Reject `PROV_FX_EXPLICIT`** (`#fx{...}`) with a real diagnostic. Effect
     rows are never a valid `:exports` value.

2. Diagnostic (new code, e.g. `TUR-E0620` in the pkg-manifest range -- verify
   the next-free number in `src/diag/codes.h` before assigning):

   ```
   TUR-E0620: `:exports` expects a map literal (`#map{...}`) or a vector of
   source paths; got an effect-row literal (`#fx{...}`).

   note: `#fx{...}` is the effect-set spelling used in function type
   annotations. To declare exported modules, write:

     :exports #map{
       "app/main" ["main"]
       "app/util" ["double-it"]
     }
   ```

   Emit through the standard `diag_error_at(form_span(f), ...)` path so the
   caret lands on the `#fx{` token.

3. Return `false` from `parse_exports` on rejection. `pkg_manifest_read`
   already treats a partial-parse as a hard error path; a manifest with a
   malformed `:exports` should refuse to load rather than silently ship an
   empty exports list.

### Change 2 -- fix the two fixtures

Convert both to `#map{...}`:

- `tests/fixtures/build-project-smoke/build.tur`
- `tests/fixtures/result-typedef-multi-module/build.tur`

These are the only two occurrences in-tree (`grep -rn "#fx{" tests/fixtures/*/build.tur`).
No snapshot regen expected -- `:exports` is a manifest-time concern; the
generated C is unaffected.

### Change 3 -- regression fixture (negative)

Add `tests/fixtures/manifest-exports-fx-row-rejected/`:

- `build.tur`: `(defpackage foo :name "foo" :version "0.1.0" :exports #fx{ "app/main" ["main"] })`
- `expected.stderr`: matches `TUR-E0620` and the `#fx{` caret line.
- `expected.exit`: non-zero.
- Drive via `tur build .` (or the smallest command that runs
  `pkg_manifest_read`).

### Change 4 -- positive fixture already covered

`build-project-smoke` becomes the positive `#map{...}` case after Change 2;
no new positive fixture is required. If the reviewer wants belt-and-braces,
add a `manifest-exports-legacy-bare-map` fixture using `#{...}` to lock in
that we keep accepting the legacy form.

### Change 5 -- migrate downstream repos in lockstep

Because Change 1 turns a silently-wrong manifest into a hard parse error, the
`#fx{...}` -> `#map{...}` rewrite has to land in every downstream repo that
this tree drives, in the same session as the parser change. Otherwise the
next `tur build` in those trees breaks and blames the parser instead of the
manifest.

The three downstream trees to sweep, and the current hit list (as of
2026-07-05):

**`../turmeric-spices/` -- six manifests to convert:**

- `spices/rtmidi/build.tur:18`
- `spices/sdf-raylib/build.tur:22`
- `spices/rtaudio/build.tur:18`
- `spices/wav/build.tur:20`
- `spices/linalg/build.tur:12`
- `spices/plot/build.tur:21`

Vendored copies under
`spices/plot/spices/plutovg-plutovg-v0.1.0/spices/{rtmidi,sdf-raylib,rtaudio,wav}/build.tur`
are cached transitive-dep snapshots -- do **not** hand-edit them; refetch with
`tur fetch` from the plot spice after the source manifests are updated so the
cache regenerates cleanly. If refetch is not viable in this session, apply
the same `#fx{` -> `#map{` swap to the four cached files as an interim so the
tree still parses, and file a follow-up to re-run the fetch.

**`../turmeric-godot/`** -- no top-level `build.tur` in the repo. The only
hit under a `build.tur` name is
`examples/aot-bench/.godot/turmeric-cache/f142cec3a383c366/build.tur`, which
is a generated cache artifact under `.godot/`. Skip it; the Godot integration
regenerates that file on next build. Still, grep the repo for `:exports #fx{`
before concluding it is clean, in case a manifest lives under a non-standard
name.

**`turmeric/examples/`** -- no `build.tur` files today (the example dirs
build via `CMakeLists.txt` or single-file). Nothing to migrate; add a quick
grep sweep to confirm none have appeared by the time this plan lands.

Per-repo verification after the swap:

- `../turmeric-spices/`: `bash tests/run.sh` at the repo root (if present),
  or a targeted `tur build` in each converted spice's directory.
- `../turmeric-godot/`: `scons` (or the repo's usual build entry point) --
  a compile of the GDExtension is enough to prove the manifest reader is
  quiet.
- `turmeric/examples/`: `tur run` each example that has a runnable entry;
  today all pass without manifests, so a grep for `#fx{.*` in every `*.tur`
  under `examples/` is sufficient.

A one-line CHANGELOG entry in each downstream repo pointing at the parser
change is warranted so external users of those spices know the required
manifest fixup.

## Files touched

| File | Change |
|------|--------|
| `src/compiler/pkg.c` | `parse_exports`: tag check -> tag + provenance check; emit `TUR-E0620` on `PROV_FX_EXPLICIT` |
| `src/diag/codes.h` (or equivalent) | Register `TUR-E0620` |
| `tests/fixtures/build-project-smoke/build.tur` | `#fx{` -> `#map{` |
| `tests/fixtures/result-typedef-multi-module/build.tur` | `#fx{` -> `#map{` |
| `tests/fixtures/manifest-exports-fx-row-rejected/*` | new negative fixture |
| `../turmeric-spices/spices/{rtmidi,sdf-raylib,rtaudio,wav,linalg,plot}/build.tur` | `#fx{` -> `#map{` in `:exports` |
| `../turmeric-spices/spices/plot/spices/plutovg-*/spices/{rtmidi,sdf-raylib,rtaudio,wav}/build.tur` | regenerate via `tur fetch`, or hand-patch as interim |
| `../turmeric-godot/` | grep sweep only -- no known manifest hits (cache under `.godot/` regenerates) |
| `turmeric/examples/` | grep sweep only -- no `build.tur` files today |

## Verification

1. `cmake --build build -j` -- clean build.
2. `bash tests/run.sh 2>&1 | tee /tmp/exports-plan.log` (10-minute timeout,
   per CLAUDE.md strict rule) -- expect the two converted fixtures to pass
   and the new negative fixture to pass its stderr match.
3. Manual: `./build/tur build tests/fixtures/manifest-exports-fx-row-rejected`
   -- confirm `TUR-E0620` prints with a caret on `#fx{`.
4. Grep sweep: `grep -rn "#fx{" -- '*.tur' | grep -v '#fx{Unsafe\|#fx{Pure\|#fx{IO'`
   should return no `:exports` hits and no manifest hits.

## Risk / blast radius

- **In-tree**: two fixtures. Both are internal; no downstream depends on the
  `#fx{}` spelling of their `:exports`.
- **Ecosystem**: any external spice manifest that used `#fx{}` for
  `:exports` will now fail to parse. This is intentional -- that manifest
  was silently wrong -- and the diagnostic explains the fix. The
  fx-row-syntax-rename migration is recent enough that this is unlikely to
  have escaped into the wild, but a one-line CHANGELOG entry is warranted.
- **Legacy `#{...}` maps**: unaffected. `PROV_FX_LEGACY` continues to be
  accepted at this site.

## Why not "just add a warning"

A warning here fails the same way silent acceptance does: the next LLM edit
will not read warnings from a previous run, will copy an existing fixture as
a template, and will re-introduce the bug. The whole point is that the
parser makes the wrong spelling structurally impossible. A hard error at
parse time is the only fix that survives contact with copy-paste-from-example
authoring, human or otherwise.

## Follow-ups (out of scope for this plan)

- Consider whether `PROV_FX_LEGACY` (bare `#{...}`) should eventually warn
  in a manifest slot, nudging authors toward `#map{...}`. Separate
  migration; not on the v1 critical path.
- Audit other manifest slots that take `F_MAP` (`:bin`, `:cmake-deps`, ...)
  for the same tag-only check -- same trap, same fix shape. A quick grep in
  `src/compiler/pkg.c` for `tag == F_MAP` will surface them.
