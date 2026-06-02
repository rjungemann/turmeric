# Plan: Rename `.tursweet` → `.tur.sweet` and `--lang tursweet` → `--lang sweet-exp` Globally

## Goal

Two related changes across both the `turmeric` repository and the sibling
`turmeric-spices` repository:

1. Replace the `.tursweet` file extension with `.tur.sweet` in all source
   code, documentation, tests, and web assets.
2. Rename the `--lang tursweet` CLI dialect flag to `--lang sweet-exp`,
   keeping `tursweet` as a deprecated alias so existing scripts don't
   break immediately.

The `#lang sweet-exp` directive string is **not** changed -- it already uses
the canonical name.

---

## Scope Summary

### `turmeric` repo

| Area | Files / Locations | Nature of change |
|------|-------------------|-----------------|
| C source -- extension detection | `src/compiler/reader.c:3133` | Update `strcmp` string |
| C source -- extension detection | `src/main.c:3810` (length 9 → 10, suffix string) | Update length constant and string |
| C source -- extension detection | `src/main.c:7730` (length 9 → 10, suffix string) | Update length constant and string |
| C source -- help text / comments | `src/main.c:3946`, `4010`, `7709` | Update string literals / comments |
| C source -- `--lang` dispatch | `src/main.c:4020`, `4067` | Rename primary dialect name; keep `tursweet` as deprecated alias |
| Test harness | `tests/run-fmt.sh:183-191` | Update test name, comment, and `--lang` value to `sweet-exp` |
| Web HTML | `web/index.html:72,184,251` | Update `data-sweet-exp` attribute values |
| Web dist HTML | `web/dist/client/index.html:71,183,250` | Same (generated; regenerate from source) |
| Docs -- guides | `docs/guides/tur-run-guide.md` (and `.html`) | Update prose references |
| Docs -- upcoming | `docs/upcoming/syntax-guide-plan.md` | Update prose references |
| Docs -- archive | `docs/archive/history/data-literals-plan.md` | Update prose references |
| Docs -- archive | `docs/archive/history/tur-fmt-plan.md` | Update prose references |
| Docs -- archive | `docs/archive/function-type-kind-plan.md` | Update prose references |
| Web dist docs | `web/dist/client/docs/**/*.html` | Update prose (regenerate from source) |
| Web dist docs | `web/dist/docs/**/*.md` | Update prose (regenerate from source) |
| `CHANGELOG.md` | Multiple entries | Update prose references |
| `CLAUDE.md` | Lines 288-289 | Update prose references |
| `AGENTS.md` | Line 18 | Update prose reference |

No actual `.tursweet` files exist in the `turmeric` repo tree today (only
references to the extension in docs and source code).

### `turmeric-spices` repo

No `.tursweet` files or references were found in this repo as of this plan.
A sweep should be performed before the rename lands to confirm the repo is
clean, and again after the `turmeric` changes are published so that any new
spice files added in the interim are caught.

---

## Detailed Steps

### Phase 1 -- C source changes (`turmeric`)

1. **`src/compiler/reader.c`** -- update the extension check:
   ```c
   // before
   if (strcmp(ext, ".tursweet") == 0) {
   // after
   if (strcmp(ext, ".tur.sweet") == 0) {
   ```

2. **`src/main.c`** -- three locations:

   a. Extension-detection helper (around line 3810):
   ```c
   // before
   if (n >= 9 && strcmp(name + n - 9, ".tursweet") == 0) return true;
   // after
   if (n >= 10 && strcmp(name + n - 10, ".tur.sweet") == 0) return true;
   ```

   b. Directory-walk condition (around line 7730):
   ```c
   // before
   (an > 9  && strcmp(a + an - 9,  ".tursweet") == 0) ||
   // after
   (an > 10 && strcmp(a + an - 10, ".tur.sweet") == 0) ||
   ```

   c. Comments / help strings (lines 3946, 4010, 7709) -- update to
      `.tur.sweet` in the plain-text descriptions.

### Phase 2 -- `--lang` flag rename (`turmeric`)

1. **`src/main.c`** -- dialect dispatch (around line 4067):
   ```c
   // before
   } else if (strcmp(lang, "tursweet") == 0 || strcmp(lang, "sweet-exp") == 0 || strcmp(lang, "sweet") == 0) {
   // after -- sweet-exp is now primary; tursweet kept as deprecated alias
   } else if (strcmp(lang, "sweet-exp") == 0 || strcmp(lang, "sweet") == 0 || strcmp(lang, "tursweet") == 0) {
   ```
   Also update the help text (around line 4020) to list `sweet-exp` as the
   canonical dialect name and mark `tursweet` as a deprecated alias:
   ```
   // before
   "  Dialects for --lang:  turmeric (default)  tursweet  curly-infix  neoteric\n"
   // after
   "  Dialects for --lang:  turmeric (default)  sweet-exp  curly-infix  neoteric\n"
   "  (tursweet is a deprecated alias for sweet-exp)\n"
   ```

2. **`tests/run-fmt.sh`** -- update the test to use `--lang sweet-exp`, and
   add a second test confirming `--lang tursweet` still exits 0 (deprecated
   alias smoke-test):
   ```sh
   # before
   NAME="fmt-lang-tursweet-stdin"
   printf '...' | "$TUR" fmt --stdin --lang tursweet > /dev/null 2>&1
   # after
   NAME="fmt-lang-sweet-exp-stdin"
   printf '...' | "$TUR" fmt --stdin --lang sweet-exp > /dev/null 2>&1
   # ... then a second block:
   NAME="fmt-lang-tursweet-alias-stdin"   # deprecated alias still accepted
   printf '...' | "$TUR" fmt --stdin --lang tursweet > /dev/null 2>&1
   ```

### Phase 4 -- Web assets (`turmeric`)

- **`web/index.html`** -- update `data-sweet-exp` attribute values:
  ```html
  <!-- before -->
  data-sweet-exp="effects.tursweet"
  <!-- after -->
  data-sweet-exp="effects.tur.sweet"
  ```
  Repeat for `typeclasses.tursweet` → `typeclasses.tur.sweet`.

- **`web/dist/client/index.html`** -- same substitution (this file is a
  build artifact; update it in place or regenerate via `just web-dev`).

### Phase 5 -- Documentation prose (`turmeric`)

Run `sed` substitutions across all Markdown and HTML documentation files.
Do the extension rename first, then the flag rename:

```sh
# In the turmeric repo root:
find docs/ CLAUDE.md AGENTS.md CHANGELOG.md \
     web/index.html web/dist/ \
     -type f \( -name "*.md" -o -name "*.html" \) \
  | xargs sed -i '' 's/\.tursweet/.tur.sweet/g'

# Rename --lang tursweet references (prose / examples in docs)
find docs/ CLAUDE.md AGENTS.md CHANGELOG.md \
     -type f \( -name "*.md" -o -name "*.html" \) \
  | xargs sed -i '' 's/--lang tursweet/--lang sweet-exp/g'
```

Specific file to check manually:
- `docs/archive/history/tur-fmt-plan.md:34` -- example uses `--lang tursweet`

Review the full diff before committing to catch any unintended replacements.

### Phase 3 -- Fixture / test `.tursweet` files (if any are added before this lands)

If any `.tursweet` files appear in `tests/fixtures/` or elsewhere before this
work lands, rename them:

```sh
find . -name "*.tursweet" | while read f; do
  mv "$f" "${f%.tursweet}.tur.sweet"
done
```

Regenerate `tests/fixtures/*/expected.c` snapshots if any fixture filenames
appear in generated C output.

### Phase 6 -- `turmeric-spices` repo sweep

```sh
cd ../turmeric-spices
# Check for any .tursweet files
find . -name "*.tursweet"
# Check for prose references
grep -r "\.tursweet\|--lang tursweet" --include="*.md" --include="*.tur" --include="*.sh" .
```

Rename any files found, update `--lang tursweet` → `--lang sweet-exp` in prose,
and update extension references with the same `sed` commands used in Phase 5.

### Phase 7 -- Build, test, and verify

```sh
# In turmeric repo:
just configure
just build
just test

# Spot-check extension detection:
echo '#lang sweet-exp' > /tmp/smoke.tur.sweet
echo '(defn f [] :int 1)' >> /tmp/smoke.tur.sweet
./build/tur run /tmp/smoke.tur.sweet

# Spot-check --lang sweet-exp flag:
echo '(defn f [] :int 1)' | ./build/tur fmt --stdin --lang sweet-exp

# Confirm deprecated alias still accepted:
echo '(defn f [] :int 1)' | ./build/tur fmt --stdin --lang tursweet
```

All `FAIL` lines from `bash tests/run.sh` must be zero before opening a PR.

---

## Edge Cases & Notes

- **`--lang tursweet` (deprecated alias)**: Keep `tursweet` accepted silently
  so existing scripts don't break. Consider emitting a deprecation warning to
  stderr in a follow-up once users have had time to migrate.
- **`#lang sweet-exp` directive**: Unchanged.
- **`.tur.sweet` dot in file names**: Most shells and editors handle
  multi-dot extensions correctly. LSP and editor plugins that key off the
  last extension component (`.sweet`) may need updating separately -- file
  an issue if problems arise.
- **`web/dist/` files**: These are checked-in build artifacts. Update them
  in-place with `sed` rather than relying on a web build step to keep the PR
  self-contained.
- **Snapshot fixtures**: Run the snapshot regeneration loop from `CLAUDE.md`
  after any codegen change if fixtures reference `.tursweet` filenames in
  their expected output.

---

## Commit Strategy

Suggested split:

1. `chore: rename .tursweet → .tur.sweet and --lang tursweet → --lang sweet-exp in C source`
   (Phases 1-2 -- the load-bearing code changes)
2. `chore: rename .tursweet → .tur.sweet and --lang tursweet → --lang sweet-exp in docs and web`
   (Phases 4-5 -- documentation and web assets)
3. `chore(spices): rename .tursweet → .tur.sweet and --lang tursweet → --lang sweet-exp`
   (Phase 6 -- sibling repo, separate PR)
