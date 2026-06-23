# Drop `-X` Feature Flags Plan

## Context

`tur` ships 16 `-X` feature flags. Most are vestigial: the features they
gate are either complete and uncontroversial, already on-by-default with
the flag acting as a redundant re-enable, or already converted to
deprecation no-ops. The guide table at `docs/guides/compiler-flags-guide.md`
spells out the status:

| Flag | Status today |
|---|---|
| `-Xlinear` | Complete |
| `-Xsubstructural` | Complete (implies `-Xlinear`) |
| `-Xunique-types` | Partial (UT0–UT1 shipped) |
| `-Xgadt` | **Deprecated no-op** -- on by default, flag warns |
| `-Xunion-types` | Substantial (IT0–IT4) |
| `-Xintersection-types` | Substantial (IT0–IT4) |
| `-Xeffect-types` | Complete (ET0–ET4, LC0–LC3, MS0–MS4) |
| `-Xcontracts` | Complete, **on by default**; flag is a redundant re-enable |
| `-Xsessions` | Complete (SS0–SS8); implies `-Xsubstructural` |
| `-Xdynamic-vars` | Complete (DV0–DV4) |
| `-Xcallcc` | **Deprecated no-op** -- on by default, flag warns |
| `-Xsized-types` | Partial (SZ0–SZ4); runtime-checked today |
| `-Xdata-literals` | Substantial (in regular use across stdlib) |
| `-Xjson-reader` / `-Xschema-reader` | Substantial reader-side features |
| `-Xsymbols` | Complete |

Two flags (`-Xgadt`, `-Xcallcc`) are already accept-and-warn no-ops. Most
of the rest gate features the language now leans on -- the v1 ship-line
has effectively absorbed them. Carrying the flag surface forward into v1
means we ship a language that pretends to be configurable along axes
nobody actually configures, and forces every guide, fixture, and downstream
spice to keep restating "pass `-Xfoo`."

## Goals

1. **All 16 `-X` flags become accept-and-warn no-ops** in a single
   release. Every feature they gate is unconditionally on.
2. **Minor version bump** to **0.24.0** at the moment the no-op turn
   lands -- the language surface grows (every feature is now standard),
   no flag-passing code breaks (the flags are still accepted), so semver
   minor is the right bucket.
3. **Guides, fixtures, manifests, CI, and the docs site stop mentioning
   `-X` flags** except in a single "removed flags" note that points
   readers at the deprecation warning.
4. **One deprecation cycle, not two.** No "first turn into no-op, then
   later reject" -- pick a clean cutover at v1 and stop carrying the
   flag-parsing scaffolding then.

## Non-goals

- Finishing partial features (`-Xunique-types` UT2+, `-Xsized-types`
  static checking). The plan promotes **whatever is shipped today** to
  always-on; partial features stay partial and keep their existing
  diagnostics. Completion plans (`sized-types-completion-plan.md`, etc.)
  continue independently.
- Renaming any feature, restructuring any guide beyond the strict
  delete-the-flag-paragraph edits, or touching `--strict-effects` /
  `--keep-contracts` / `--no-contracts` / `--warn-unused-result` /
  `--lint-panic` / `--dump-*` / `--emit-abi-trace` -- those are
  diagnostic flags, not feature flags, and stay.
- Touching any `-X<future-feature>` flag we might add **after** the cut;
  this plan applies to the 16 listed above.

## Design

### Single switch

Today each flag drives a `g_opt_<feature>` (or similarly-named) bool in
`src/runtime/globals.h` plus a parser arm in `src/main.c` (each flag
appears twice -- once in the early scan around line 5466, once in the
late scan around line 11940). The features themselves consult the bool
at decision points.

After the cut:

- Every `g_opt_<feature>` global is **initialized to `true`** and
  **never reassigned**.
- Every feature-side `if (g_opt_<feature>)` guard is **deleted**.
- The parser keeps recognizing `-X<name>` for the 16 names, prints
  `TUR-W0050: -X<name> is no longer needed; the feature is on by
  default`, and continues. (Reuse the existing `-Xgadt`/`-Xcallcc`
  warning machinery.)
- The help text section listing `-X` flags is replaced with one line:
  `-X<name>  recognized for backwards compatibility; all language
  features are now on by default. See docs/guides/compiler-flags-guide.md
  for the removal list.`

This keeps every existing call site, Makefile, and `build.tur` working
unchanged through 0.24.x while telling the user the flag is dead.

### Implies graph collapses

The dependency arrows in the guide --
`-Xsubstructural ▶ -Xlinear`, `-Xsessions ▶ -Xsubstructural ▶ -Xlinear`,
`-Xsized-types ▶ -Xgadt` -- all disappear because each flag is independently
a no-op. The implies-resolution code in `src/main.c` can be deleted.

### `--strict-effects` and other diagnostics

`-Xeffect-types` currently implies `--strict-effects`. After the cut,
`--strict-effects` stays an **opt-in diagnostic flag** -- always-on
effect types do **not** imply always-on strict warnings. This preserves
today's "you can write unannotated effectful functions without noise"
default for code that hasn't migrated.

### Partial features

`-Xunique-types` (UT0–UT1 only) and `-Xsized-types` (SZ0–SZ4,
runtime-only) become always-on at their **current** completion level.
Users who would have hit the partial-feature edges with the flag will
hit them without the flag -- the diagnostics already exist
(`TUR-E0xxx` errors describing the not-yet-shipped bits) and continue
to fire. The guide's "Status" column drops to a one-sentence note per
feature pointing at the relevant completion plan.

### Version policy

- The PR that flips the switch lands the bump: `VERSION` 0.23.x → **0.24.0**.
- Use the existing `cut-minor-release` skill at land time.
- Changelog entry headline: "All `-X` feature flags are now accept-and-warn
  no-ops; every gated feature is on by default."
- A point release **immediately preceding** the cutover (0.23.N, with N =
  current + 1) lands the docs-side preamble warning ("the next minor
  release removes `-X` flag gating") -- this gives a one-tag window for
  downstream spice maintainers to notice without forcing a coordinated
  flag-pass migration.

## Work items

| # | Item | File(s) |
|---|------|---------|
| D1 | Pick the 0.23.N preamble release; add a "deprecation notice" line to `CHANGELOG.md` and `docs/guides/compiler-flags-guide.md`'s preamble. | `CHANGELOG.md`, `compiler-flags-guide.md` |
| D2 | In `src/main.c`, replace each `-X<name>` arm in both scans (around lines 5466 and 11940) with the no-op-warn pattern already used by `-Xgadt`/`-Xcallcc`. | `src/main.c` |
| D3 | Default every `g_opt_<feature>` in `src/runtime/globals.h` to `true`; keep the symbol so external code that touches it still links. | `src/runtime/globals.{h,c}` |
| D4 | Delete every `if (g_opt_<feature>)` / `if (!g_opt_<feature>)` guard at feature decision points. Audit via `grep -rn 'g_opt_' src/`. | various |
| D5 | Delete the implies-resolution block in `src/main.c` that propagates `-Xsessions ▶ -Xsubstructural ▶ -Xlinear` etc. | `src/main.c` |
| D6 | Strip `-X<name>` lines from `tur --help` / `tur emit-c --help` / any other usage string; replace with the single backwards-compat note above. | `src/main.c` |
| D7 | Rewrite `docs/guides/compiler-flags-guide.md`: drop the feature-flag table, drop the dependency graph, keep the diagnostic-flag table, add a short "Removed feature flags" section listing all 16 with a one-line "feature is now standard; see &lt;guide&gt;" pointer each. | `docs/guides/compiler-flags-guide.md` |
| D8 | Sweep `docs/guides/*.md` for `-X<name>` mentions in prose ("requires `-Xsessions`", "gated behind `-Xcontracts`", etc.). Delete the gating clause; the feature is now standard. | `docs/guides/*.md` |
| D9 | Sweep `tests/fixtures/**/*.tur` and any `expected.*` for `-X<name>` invocations. Drop the flag from compile commands; fixtures continue to pass because the feature is on by default. | `tests/fixtures/`, `tests/run.sh` if it ever passes `-X` |
| D10 | Sweep `build.tur` / `build.tur.sweet` across the repo (and the sibling `../turmeric-spices/` if present) for `:flags ["-X..."]` entries; delete them. | `**/build.tur*` |
| D11 | Sweep `.github/workflows/*.yml` for any `-X` flags in test/build invocations. | `.github/workflows/` |
| D12 | Update `CLAUDE.md` if it mentions `-X` flags. | `CLAUDE.md` |
| D13 | Cut **0.24.0** with the `cut-minor-release` skill. | `VERSION`, `CHANGELOG.md`, `README.md` |

D1 ships in the 0.23.N preamble release. D2–D13 ship together in the
0.24.0 release.

## Verification

Single check, run with the mandatory 10-minute timeout:

```sh
bash tests/run.sh 2>&1 | grep -E '^(FAIL|summary)'
```

The pass count should be unchanged (no fixture exercises "flag absent ▶
feature disabled" behavior; if any does, that fixture was testing the
flag rather than the feature and gets deleted in D9).

Smoke checks for the deprecation warning:

```sh
./build/tur -Xsessions check examples/hello.tur 2>&1 | grep 'no longer needed'
./build/tur -Xgadt     check examples/hello.tur 2>&1 | grep 'no longer needed'
```

Both must print exactly one `TUR-W0050` line and exit 0.

A grep gate before merge:

```sh
git grep -nE '"-X[a-z][a-z-]+"' -- ':!docs/guides/compiler-flags-guide.md' \
                                   ':!CHANGELOG.md' \
                                   ':!src/main.c'
```

Must print nothing -- the only places `-X<name>` strings live after the
cut are the parser arms (which still recognize them) and the
"Removed flags" section of the guide.

## Risk

- **Downstream `build.tur` files** outside this repo may pass `-X` flags;
  they will keep working (warn-only) for the entire 0.24.x line. There is
  no follow-up "now we reject the flag" release planned in this document
  -- if v1.0 wants to drop the recognizer entirely, that is a separate,
  later decision with its own deprecation window.
- **Partial features going always-on** (`-Xunique-types`, `-Xsized-types`)
  is the one user-visible behavior change. Today a user who does not pass
  the flag never sees the partial diagnostics; after the cut they do.
  This is the right trade -- the diagnostics are accurate and the partial
  surface is small -- but call it out in the changelog.
- **`--strict-effects` no longer auto-on with effect types.** Anyone who
  was relying on `-Xeffect-types` to also enable strict warnings must
  pass `--strict-effects` explicitly. Call out in the changelog.

## Out of scope

- Removing the `-X` recognizer entirely (a future v1.0+ cleanup).
- Adding a new `-X` flag mechanism for genuinely experimental features
  (if v1 ever needs one, design separately; this plan only retires the
  current 16).
- Renaming `--strict-effects` etc. to follow some future naming scheme.
