# tur add points users at a tur update subcommand that does not exist

**Severity: low** (misleading CLI guidance). Found twice independently in the
2026-08-20 docs audit; merged here.

## Repro

`tur add` on an already-declared dependency prints "Use `tur update <name>` to
change the ref" (src/compiler/pkg.c:4477), but no `update` subcommand exists --
`CANONICAL_COMMANDS` (src/main.c:8172) has no "update" (`upgrade` is the
installed-tool pipeline, a different thing).

Related message drift in the same file: src/compiler/pkg.c:4655 prints "Run
`tur fetch` to generate cmake/SpiceDeps.cmake" but fetch writes
`cmake/CMakeLists.txt`.

## Fix direction

Implement `tur update <name> --ref <ref>` (edit build.tur + refetch), or
reword both messages to the supported flow (edit `build.tur`, then
`tur fetch --update`). Fix the SpiceDeps.cmake message either way.

## Guides to update when fixed

- docs/guides/package-management-guide.md (quotes the message verbatim)
- docs/guides/consuming-spices-guide.md (Common Error Messages table)
