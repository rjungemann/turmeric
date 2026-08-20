# No barrier primitive (barrier-new/barrier-wait)

**Severity: low** -- threading-guide.md documented one (pre-fix); nothing
exists. Found in the 2026-08-20 docs audit.

## Repro

`grep -rn barrier stdlib/ src/` -> nothing.

## Fix direction

Counting barrier over mutex+condvar in a `stdlib/sync.tur` (or blessing the
STM tvar+check sketch as the canonical pattern).

## Guides to update when fixed

- docs/guides/threading-guide.md
- docs/guides/stm-tutorial.md
