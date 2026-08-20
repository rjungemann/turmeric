# #json-str?<T> and #json-file<T> readers unimplemented

**Severity: low** (the guide already flags them as future work);
`#json-str?` emits a "not yet implemented" diagnostic. Found in the
2026-08-20 docs audit.

## Root cause

src/compiler/reader.c:1866-1893 (RD2).

## Fix direction

Implement `#json-str?<T>(e)` as a `schema-decode`-based Result expansion;
`#json-file<T>` as read-file + decode.

## Guides to update when fixed

- docs/guides/schema-guide.md
