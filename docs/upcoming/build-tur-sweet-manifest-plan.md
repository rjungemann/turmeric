# `build.tur.sweet` Manifest Support Plan

## Problem

`build.tur` is the canonical project manifest. Every walk-up, workspace scan,
and `tur init` code path hard-codes the filename `"build.tur"`. Authors who
prefer sweet-expression syntax for their source files have no way to write
their manifest in the same style -- they must context-switch to plain
s-expressions just for this one file, even if every `.tur` file in the project
uses `.tur.sweet`.

Concretely, today `tur check src/mymod.tur` walks up looking for a file named
exactly `build.tur`. A file named `build.tur.sweet` sitting in the same
directory is invisible to that walk and to every other manifest-discovery path.

## Goals

1. Treat `build.tur.sweet` as a fully equivalent, sweet-exp-parsed manifest
   wherever `build.tur` is accepted today.
2. Priority rule: `build.tur` takes precedence when both files exist in the
   same directory (same tie-break logic as extension-vs-`#lang`).
3. No behaviour change for projects that only have `build.tur`.
4. `tur init` generates `build.tur` (plain s-expr) by default; add a flag
   or prompt to emit `build.tur.sweet` instead.
5. All existing fixture snapshots pass without modification.

## Non-goals

- Supporting any other alternative manifest filename (e.g. `build.toml`,
  `package.tur`). Only the `build.tur.sweet` extension is in scope.
- Allowing `#lang sweet-exp` inside a `build.tur` plain file to switch
  readers. The `#lang` directive is already detected by `detect_lang` in
  `reader.c` and would work in principle, but that path is not surfaced to
  `pkg_manifest_read` today -- leave it for a follow-up.
- Round-trip write-back for `build.tur.sweet`. The comment-preserving
  round-trip writer (`tur add-spice`, `tur fetch`) operates on the AST level
  and already works file-agnostically; it just needs the correct path passed
  in. Write-back in sweet-exp syntax is a separate, harder problem (sweet
  unparsing) -- out of scope.

## Root Cause

Every manifest-discovery site calls one of:

```c
snprintf(candidate, sizeof(candidate), "%s/build.tur", dir);
stat(candidate, &st)
```

and passes the result directly to `pkg_manifest_read(candidate, &m)`.
`pkg_manifest_read` calls `read_all`, which calls `read_all_with_registry`.
That function respects `file->reader_type`, but `pkg_manifest_read` never sets
it -- it leaves `reader_type` at the zero-value `READER_TURMERIC` regardless
of the file extension.

Two independent fixes are therefore needed:

1. **Discovery layer** -- every `stat`-for-`build.tur` probe must also probe
   for `build.tur.sweet`, returning whichever exists (preferring the plain one
   when both do).
2. **Reader layer** -- `pkg_manifest_read` must set `file.reader_type` from the
   extension before calling `read_all`, so that a `.tur.sweet` manifest goes
   through the sweet-exp preprocessor.

## Affected Sites

### `src/main.c`

| Function | Location | Change needed |
|---|---|---|
| `find_project_root` | ~line 1900 | Probe both filenames; return the containing dir if either is found. |
| `find_spice_root` | ~line 1961 | Same. |
| `discover_manifest_reader_macros` | ~line 1985 | Build the manifest path via a helper that checks both names. |
| Directory-descent build loop | ~line 2199, 2259, 2365, 2396 | Replace bare `"build.tur"` comparisons with a helper. |

### `src/compiler/pkg.c`

| Function / site | Location | Change needed |
|---|---|---|
| Walk-up in `pkg_find_workspace` | ~line 1184, 1230 | Probe both; return whichever is found. |
| Walk-up in `pkg_find_project` | ~line 1311, 1343 | Same. |
| `pkg_find_member_manifest` | ~line 1462 | Probe both. |
| Transitive-dep sub-build loader | ~line 1595, 1598 | Probe both. |
| `tur info` manifest display | ~line 2469, 2476, 2478 | Show the actual filename found, not a hardcoded label. |
| `pkg_manifest_read` | ~line 336 | Set `file.reader_type = reader_type_from_extension(path)` before calling `read_all`. |
| Inline reads (`"build.tur"` literal) | ~lines 3246, 3394, 3481, 3775 | Replace with a helper that probes both names in cwd. |
| Workspace-guard `stat("build.tur", &ws_st)` | ~line 3203 | Probe both. |
| Refuse-if-exists guard in `tur init` | ~line 2899 | Refuse if either name exists; `tur init --sweet` emits `build.tur.sweet`. |

## Implementation Plan

### SW0 -- Helper: resolve manifest path

Add a small C helper (or inline macro) to both `src/main.c` and
`src/compiler/pkg.c` that, given a directory string, returns the path to
whichever manifest exists, preferring `build.tur`:

```c
/* Returns 1 if a manifest was found, writing its path into `out` (size `cap`).
 * Prefers build.tur; falls back to build.tur.sweet. */
static int resolve_manifest_path(const char *dir, char *out, size_t cap) {
    struct stat st;
    int n = snprintf(out, cap, "%s/build.tur", dir);
    if (n > 0 && (size_t)n < cap && stat(out, &st) == 0 && S_ISREG(st.st_mode))
        return 1;
    n = snprintf(out, cap, "%s/build.tur.sweet", dir);
    if (n > 0 && (size_t)n < cap && stat(out, &st) == 0 && S_ISREG(st.st_mode))
        return 1;
    return 0;
}
```

Add a paired helper for the cwd-relative literal sites:

```c
static int resolve_manifest_cwd(char *out, size_t cap) {
    struct stat st;
    strncpy(out, "build.tur", cap); out[cap-1] = '\0';
    if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) return 1;
    strncpy(out, "build.tur.sweet", cap); out[cap-1] = '\0';
    if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) return 1;
    return 0;
}
```

### SW1 -- Reader type in `pkg_manifest_read`

In `pkg_manifest_read` (`src/compiler/pkg.c` ~line 369), after the
`SourceFile file = {0}` / `file.path = path` assignment, add:

```c
file.reader_type = reader_type_from_extension(path);
```

`reader_type_from_extension` already lives in `reader.c` and is exported via
`reader.h`; no new API surface is needed. This single line makes every
`pkg_manifest_read` call correctly run the sweet-exp preprocessor when the
path ends in `.tur.sweet`.

### SW2 -- Walk-up discovery in `src/main.c`

Replace every `snprintf(candidate, ..., "%s/build.tur", dir)` + `stat` pair
inside `find_project_root` and `find_spice_root` with a call to
`resolve_manifest_path`.

The return value of the walk-up functions is the *directory* (already
unchanged), so the callers that subsequently construct `"%s/build.tur"` to
pass to `pkg_manifest_read` must be updated to use `resolve_manifest_path` at
that point instead (see `discover_manifest_reader_macros`).

### SW3 -- Walk-up discovery in `src/compiler/pkg.c`

Same substitution for all `snprintf(mp, ..., "%s/build.tur", ...)` sites that
feed into `pkg_manifest_read`. The workspace-member probe at lines 1230 and
1343 follows the same pattern but uses a two-part path
(`"%s/%s/build.tur", anc, member`) -- extend `resolve_manifest_path` with a
two-segment variant or do the snprintf before calling it.

### SW4 -- Inline cwd-relative reads

Replace the four `pkg_manifest_read("build.tur", &m)` literal calls (lines
3246, 3394, 3481, 3775) and the guard `stat("build.tur", &ws_st)` (line 3203)
with `resolve_manifest_cwd` + conditional `pkg_manifest_read(resolved, &m)`.

### SW5 -- Directory-descent manifest skip

In the descent loop (`src/main.c` ~line 2365), the entry
`strcmp(ent->d_name, "build.tur") == 0` guard that skips the manifest must
also skip `"build.tur.sweet"`.

### SW6 -- `tur init` and `tur info`

- `tur init` refuses to run when `build.tur` *or* `build.tur.sweet` already
  exists (extend the `stat` guard at ~line 2899).
- Add `--sweet` flag to `tur init`; when present, scaffold
  `build.tur.sweet` (sweet-exp syntax for the initial `defpackage` form)
  instead of `build.tur`.
- `tur info` currently prints `"  build.tur\n"` unconditionally; replace with
  the actual filename that was found.

### SW7 -- Fixtures and tests

Add at least one new fixture directory:

```
tests/fixtures/pkg-sweet-manifest/
  build.tur.sweet      -- defpackage in sweet-exp syntax
  src/main.tur         -- trivial program
  expected.c           -- codegen snapshot (generated after implementation)
```

The fixture exercises that `tur check` and `tur emit-c` discover and parse the
sweet manifest correctly. Existing `pkg-*` fixtures are unchanged.

### SW8 -- Documentation and CLAUDE.md

Update the CLAUDE.md "Build System" section to mention that `build.tur.sweet`
is accepted as an alias for the manifest in sweet-expression syntax. Update
`docs/guides/developing-spices-guide.md` and `docs/guides/tur-run-guide.md`
similarly.

## Validation

After implementation:

1. `bash tests/run.sh 2>&1 | grep "^FAIL"` -- must be empty.
2. Manual smoke test:
   ```sh
   mkdir /tmp/sweet-spice && cd /tmp/sweet-spice
   cat > build.tur.sweet <<'EOF'
   defpackage "sweet-test"
     :version "0.1.0"
   EOF
   mkdir src && echo '(defn main [] : int 0)' > src/main.tur
   ./build/tur check src/main.tur   # must not error
   ./build/tur emit-c src/main.tur  # must produce valid C
   ```
3. Priority test: create both `build.tur` and `build.tur.sweet` in the same
   dir with different package names; `tur info` must report the name from
   `build.tur` (the plain file wins).
4. `tur init --sweet` in an empty directory must produce `build.tur.sweet`.
5. `tur init` in a directory that already contains `build.tur.sweet` must
   refuse with a clear error message.

## Order of Work

| Step | Depends on | Estimated scope |
|---|---|---|
| SW0 helper | -- | ~20 lines, 2 sites |
| SW1 reader type | SW0 | 1 line in `pkg_manifest_read` |
| SW2 walk-up in `main.c` | SW0 | ~6 call sites |
| SW3 walk-up in `pkg.c` | SW0 | ~6 call sites |
| SW4 cwd-relative reads | SW0 | ~5 call sites |
| SW5 descent skip | SW0 | 1 condition |
| SW6 init/info | SW0 | ~15 lines |
| SW7 fixture | SW1-SW5 | new fixture dir + snapshot |
| SW8 docs | SW7 | prose updates only |

All steps through SW6 can land in a single PR. SW7 (fixture) must land in the
same PR so the snapshot is consistent (`FAIL` on mismatch is a strict rule).
SW8 (docs) can follow separately.
