# Effect-Row Syntax: rename `#{...}` → `#fx{...}`

Status: proposed
Owner: unassigned
Track: syntax hygiene; near-term, single PR (~1.5 days).

## Generated docs convention

Phase progress/report write-ups generated as part of this plan live under
[`docs/artifacts/`](../artifacts/), not `docs/upcoming/`. `docs/upcoming/`
is reserved for the plans themselves; artifacts are the by-products of
executing them.

## Goal

Move the effect-row annotation syntax from `#{...}` to `#fx{...}` so that
(a) effect rows are self-describing at the call site, in line with the
existing `#map{...}` / `#set{...}` / `#row{...}` / `#refine{...}` /
`#r{...}` reader-literal family, and (b) the bare `#{...}` slot is freed
for a future reader form.

Today the same byte sequence -- bare `#{` -- means "effect row" in a
signature position and is visually indistinguishable from what a reader
might later want to spell "anonymous set" or "untyped map." Promoting
the effect-row form to `#fx{...}` removes the ambiguity and gives the
reader-literal family a consistent shape (`#<tag>{...}`).

## Non-goals

- Changing the **semantics** of effect rows. The Form AST stays
  `F_MAP`; capability sets, row polymorphism, and the `#{Unsafe Net}`
  multi-tag spelling all behave identically.
`@{...}` (the existing alternate sugar for effect rows) is also
deprecated in this pass -- see Phase 2. Two ways to spell the same
thing was the original problem; adding `#fx{...}` without retiring the
others would perpetuate it.
- Picking the eventual reuse of `#{...}`. The slot stays **reserved**
  through at least one release after removal; concrete reuse is a
  separate plan.

## Why now

- `#fx{...}` parallels every other reader literal we ship; `#{...}` is
  the lone exception.
- Reader real estate is finite. Reclaiming `#{...}` before it grows
  more downstream users is cheaper than reclaiming it later.
- Migration is fully mechanical (`#{` → `#fx{`), so the rewrite is
  low-risk and reviewable as a single diff per tree.

## Current state (measured)

- **~699** `.tur` files use `#{...}` as effect rows (stdlib + tests +
  examples + web).
- **~152** docs files mention `#{...}` in prose or samples.
- One reader dispatch -- `src/compiler/reader.c:2790` -- maps `#` + `{`
  to `read_map`, which produces `F_MAP`. The sibling alternate sits at
  `read_at` (`src/compiler/reader.c:612`) for `@{...}`.
- Test fixture snapshots (`tests/fixtures/**/expected.c`) are largely
  unaffected: effect rows do not survive into emitted C verbatim. A
  small number of diagnostic-text snapshots that quote the source will
  move and must be regenerated in the same PR (per CLAUDE.md
  "Fixture Snapshots" -- fixture churn is not a deferral reason).

## Plan

### Phase 1 -- Reader: dual-accept `#fx{...}` and legacy `#{...}`

Files: `src/compiler/reader.c`.

1. Add a `#fx{...}` branch in `read_form` before the existing
   `#` + `{` map branch. Both branches construct an `F_MAP`; the only
   distinguishing data is a one-byte provenance tag stored on the Form
   (`PROV_FX_EXPLICIT` for `#fx{`, `PROV_FX_LEGACY` for bare `#{`).
2. Order matters. The new `#fx{` dispatch must:
   - Be tried **before** `#` + `{` (otherwise `#fx{` reads as the
     symbol `fx` followed by an unrelated `{...}`).
   - Lose to the existing more-specific reader literals
     (`#map{`, `#set{`, `#row{`, `#refine{`, `#r{`, `#s(`, `#json...`).
     None of those start with `#fx{`, so this is automatic.
3. Wire the provenance tag through `Form` (add a small enum + one byte
   on the struct; null-sentinel = "not an fx row").

### Phase 2 -- Hard deprecation warning on legacy `#{...}` and `@{...}`

Files: elaboration (`src/compiler/elab*.c` or wherever effect rows are
resolved); `src/compiler/reader.c` (for the `@{...}` path); diagnostics
table.

4. Add `TUR-W00XX` ("legacy `#{...}` effect row; prefer `#fx{...}`")
   emitted from elaboration when:
   - The form's provenance is `PROV_FX_LEGACY`, **and**
   - The form is resolved as an effect row (not, e.g., some future
     reuse of `#{...}`).

   Gating on elab -- not on the reader -- is deliberate: it preserves
   the freedom to repurpose `#{...}` later without producing false
   positives during the grace period.
5. Add `TUR-W00XY` ("legacy `@{...}` effect row; prefer `#fx{...}`")
   on the same schedule. Provenance for `@{...}`-originated rows
   (`PROV_FX_AT_LEGACY`) is set at `read_at` (`reader.c:612`); the
   warning fires from the same elab site as `TUR-W00XX`.
6. **Hard-warn from day one**, no `--no-warn=fx-legacy` opt-out. Both
   forms are still accepted by the reader during the grace window, but
   every use produces a diagnostic. Migration is fully mechanical and
   the warning *is* the migration prompt -- a quiet release would just
   delay the work.

### Phase 3 -- Mechanical migration of in-tree sources

Files: `stdlib/**/*.tur`, `tests/fixtures/**/*.tur`, `examples/**/*.tur`,
`web/**/*.tur`, `docs/**/*.md` (sample blocks).

6. Migration script (`tools/migrate-fx-rows.py`) rewrites both `#{` →
   `#fx{` and `@{` → `#fx{` with these guards:
   - Skip inside string literals (`"..."`).
   - Skip inside `;`-prefixed comments (`;`, `;;`, `;;;`).
   - Skip inside inline-C fences (` ```c ... ``` `).
   - For `#{`: already a no-op on `#map{`, `#set{`, `#row{`,
     `#refine{`, `#r{`, `#json...` (the tag char between `#` and `{`
     makes the regex unambiguous), but the script asserts no such tag
     is reached.
   - For `@{`: only rewrite when `@` is in *prefix position*
     (whitespace or `[`/`(` to the left), not when `@` is a deref
     applied to a `{...}` map (which is not idiomatic but is legal).
     Spot-check the diff for any `(deref {...})` shape before
     committing.
   - Dry-run by default; the operator reviews the diff before applying.
     This is non-negotiable -- per [[feedback_no_global_rename]] a
     prior bulk rewrite mangled names and string contents, so the
     script must produce a reviewable diff, not an in-place rewrite.
7. Land one commit per source tree (`stdlib`, `tests/fixtures`,
   `examples`, `web`, `docs`) so a single bad replacement can be
   reverted without taking the rest with it.
8. After each tree: run `bash tests/run.sh` with `timeout: 600000` per
   the strict 10-minute-timeout rule, then regen `expected.c`
   snapshots for any fixture whose diagnostic text moved (the
   CLAUDE.md "Fixture Snapshots" recipe).

### Phase 4 -- Documentation sweep

Files: `docs/guides/syntax-guide.md`, `docs/guides/reader-forms-guide.md`,
`docs/guides/contract-types-guide.md`, effects guides under
`docs/guides/`, the sweet-exp section of `CLAUDE.md`,
`stdlib/docstrings.tur` (e.g. the `Proc` / `Rand` entries that show
`#{Proc}` / `#{Rand}` in their examples), and `tools/gendocs.py`
templates if they hard-code the form.

9. Reader-forms guide grows a "deprecated forms" subsection: bare
   `#{...}` redirects to `#fx{...}`, with the `TUR-W00XX` code and the
   expiry release.
10. Re-run `tur run docs` to regenerate `docs/api/` (do not hand-edit
    -- per CLAUDE.md "Generated Docs").

### Phase 5 -- Graduation (one or two minor releases later)

11. Remove the bare `#{...}` reader dispatch and the `@{...}` effect-row
    branch in `read_at`. Both become hard errors:
    - `TUR-E00XX` for `#{...}`: "reserved reader form; use `#fx{...}`,
      `#map{...}`, or `#set{...}`."
    - `TUR-E00XY` for `@{...}` in prefix position: "removed effect-row
      sugar; use `#fx{...}`." (Bare `@x` deref is unaffected.)
12. The `#{...}` slot stays **reserved** -- the reader rejects it with
    a pointed error rather than silently accepting it as something
    new. Picking a reuse is an explicit non-goal of this plan and
    requires a separate proposal; the reserved-error message is the
    forcing function that keeps that decision deliberate.

## Risk and mitigations

| Risk | Mitigation |
|---|---|
| Bulk rewrite mangles non-effect-row text (cf. [[feedback_no_global_rename]]) | Script is dry-run-by-default; operator reviews diff; tree-by-tree commits; full suite green after each tree. |
| Sweet-exp interaction (`#{...}` inside a t-expr line) | The reader dispatch runs *under* the sweet-exp layer, identical to `#map{...}`. Verified by the existing data-literal coverage; add one sweet-exp fixture for `#fx{Unsafe Net}` to lock it in. |
| Snapshot churn lands in a separate PR | Forbidden by CLAUDE.md ("Fixture churn is not a deferral reason") -- regen in the same PR. |
| Downstream spices (e.g. `../turmeric-spices`) lag the rename | The deprecation window is the whole point of dual-accept; spices flip on their own cadence within the grace window. The `tests/fixtures` that depend on the spices repo (`requires.spices` markers) auto-skip when the sibling checkout is absent, so we don't block on it. |

## Decisions (resolved 2026-06-26)

- **Warning loudness**: hard warn from day one. No `--no-warn=fx-legacy`
  opt-out; every legacy use produces a diagnostic during the grace
  window. Folded into Phase 2.
- **`@{...}` fate**: deprecated in the same pass as `#{...}`. Reader
  keeps accepting it through the grace window with `TUR-W00XY`;
  removed in Phase 5. Folded into Phases 2, 3, and 5.
- **Reuse target for `#{...}`**: explicitly left open. The plan's
  finish line is "reserved," not "reassigned." Reuse requires a
  separate proposal.

## Estimated cost

- Phase 1 (reader + provenance + dual-accept): ~half day.
- Phase 2 (warning wiring): folded into Phase 1.
- Phase 3 (migration script + tree-by-tree review + suite + snapshots):
  ~half day.
- Phase 4 (docs sweep + regenerated `docs/api/`): ~half day.
- Phase 5 (graduation): trivial, one commit, in a future release.

Total for Phases 1-4 (the deliverable PR): ~1.5 days of focused work.
