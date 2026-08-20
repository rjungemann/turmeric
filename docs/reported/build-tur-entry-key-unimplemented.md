# :entry in build.tur is documented but not implemented

**Severity: low**. Found in the 2026-08-20 docs audit.

## Repro

A manifest with `:entry "src/foo.tur"` plus two `.tur` files under `src/` ->
`tur run: cannot determine entry point`.

## Root cause

src/main.c:4559 -- ":entry key in build.tur (not yet in PkgManifest --
future)".

## Fix direction

Parse `entry` into `PkgManifest` (pkg.c) and honor it first in project-mode
entry resolution.

## Guides to update when fixed

- docs/guides/package-management-guide.md (entry-resolution table row)
