# :entry in build.tur is documented but not implemented

**Severity: low**. Found in the 2026-08-20 docs audit.
**Status: RESOLVED** -- `:entry` is parsed into `PkgManifest` and honored as
the first rung of project-mode `tur run` entry resolution.

## Repro

A manifest with `:entry "src/foo.tur"` plus two `.tur` files under `src/` ->
`tur run: cannot determine entry point`.

## Root cause

src/main.c:4559 -- ":entry key in build.tur (not yet in PkgManifest --
future)". The documented three-rung ladder (`:entry` > `src/main.tur` >
the single `.tur` under `src/`) only ever implemented rungs 2 and 3; the
first rung was a comment.

## Resolution

- `src/compiler/pkg.h` -- `char *entry` on `PkgManifest`.
- `src/compiler/pkg.c` -- parse `:entry` (`form_str_dup`, alongside
  `:build-dir`), emit it from the manifest writer, free it in
  `pkg_manifest_free`.
- `src/main.c` -- honor it ahead of `src/main.tur`. A relative path is
  resolved against the project root; an absolute path is taken as written.
  A `:entry` that does not name an existing file is a **hard error** rather
  than a fall-through to `src/main.tur`: silently running a different
  program than the manifest asked for is the failure mode worth being loud
  about. The rung-3 "cannot determine entry point" message now also names
  `:entry` as a way out.

Also made both entry-resolution error paths leak-clean (`spice_inc_dirs` /
`user_inc` were leaked on the pre-existing rung-3 failure, which LSan
reported on any `tur run` that could not resolve an entry).

## Tests

`tests/run-manifest-entry.sh` (ctest target `tur_manifest_entry`), 11
assertions: `:entry` selecting either of two ambiguous `src/` files,
`:entry` outranking an existing `src/main.tur`, an absolute `:entry`, the
dangling-`:entry` error (message, exit code, and no fall-through), the
unchanged rung-3 error plus its new `:entry` suggestion, and the unchanged
rung-2 `src/main.tur` fallback.

## Guides updated

- docs/guides/package-management-guide.md -- entry-resolution section gained
  the path-resolution rule, an example manifest, the dangling-`:entry` error
  text, and a note on which commands the key applies to.
