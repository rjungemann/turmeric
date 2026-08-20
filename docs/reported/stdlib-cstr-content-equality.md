# No stdlib cstr content-equality function

**Severity: low** -- `=` on cstr compares pointers, so guides and examples
each hand-roll strcmp wrappers. Found in the 2026-08-20 docs audit.

## Repro

stdlib/cstr.tur exports only `cstr-len`/`cstr-nth`/`cstr-sub`;
examples/datalog/minimal.tur:157 and the cli-args guide define local helpers.

## Fix direction

Add and export `cstr-eq? [a : cstr b : cstr] : bool` in stdlib/cstr.tur.

## Guides to update when fixed

- docs/guides/cli-args-guide.md
- docs/guides/datalog-02-minimal-impl.md
