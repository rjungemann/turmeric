# tur add points users at a tur update subcommand that does not exist

**Severity: low** (misleading CLI guidance). Found twice independently in the
2026-08-20 docs audit; merged.
**Status: RESOLVED** -- both messages reworded to the flows that exist.

## Repro

`tur add` on an already-declared dependency printed "Use `tur update <name>` to
change the ref" (src/compiler/pkg.c:4477), but no `update` subcommand exists --
`CANONICAL_COMMANDS` (src/main.c:8172) has no "update" row (`upgrade` is the
installed-tool pipeline, a different thing).

Related drift in the same file: src/compiler/pkg.c:4655 printed "Run
`tur fetch` to generate cmake/SpiceDeps.cmake", but fetch writes
`cmake/CMakeLists.txt`. `SpiceDeps` is the name of the cmake *project*
declared inside that file (pkg.c:2966) -- which is where the wrong filename
came from.

## Resolution

Reworded rather than implementing a new subcommand. The fix direction offered
both, and `tur fetch --update` (pkg.c:4678) already is the supported
re-resolve flow, so a `tur update` would be a second spelling of it rather
than new capability.

- Duplicate dep: *"'geom' is already a dependency. To change its ref, edit the
  `:spices` entry for 'geom' in build.tur, then run `tur fetch --update`."*
- cmake dep added: *"Run `tur fetch` to generate cmake/CMakeLists.txt."*

Both sites carry a comment explaining what the old text got wrong, so the
misnomer does not creep back.

## Tests

`tests/spice-resolver-tests.sh`, cases `MSG1` and `MSG2`. Each asserts the
dead string is **absent** as well as the replacement being present -- a test
that only checked the new text would pass even if the old advice were
restored alongside it.

## Guides updated

- docs/guides/package-management-guide.md -- error-message table row.
- docs/guides/consuming-spices-guide.md -- Common Error Messages row, and the
  `SpiceDeps.cmake` -> `cmake/CMakeLists.txt` reference in the cmake-deps
  section.
- docs/guides/developing-spices-guide.md -- same `SpiceDeps.cmake` reference.
