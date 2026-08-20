# Arc has a runtime but no stdlib/language surface

**Severity: low** (expressiveness hole) -- `src/runtime/arc.{c,h}` exists but
there is no `arc*` defn anywhere in stdlib/; `tests/fixtures/arc-basic`
hand-rolls the control block in inline C. threading-guide.md used to document
`arc`/`arc-clone`/`arc-deref` as built-ins (corrected in the 2026-08-20 docs
audit).

## Fix direction

`stdlib/arc.tur` opaque-handle wrappers (arc-new/clone/drop/get + weak) over
the existing runtime, mirroring `rc/*`.

## Guides to update when fixed

- docs/guides/threading-guide.md (Arc section)
