# No value-carrying Mutex<T>/RwLock<T> or scoped with-lock helpers

**Severity: low** (ergonomics) -- stdlib ships only raw `mutex-*`/`rwlock-*`
handles; the scoped closure API (`with-lock`/`read-lock`/`write-lock`) that
threading-guide.md used to document does not exist (corrected in the
2026-08-20 docs audit).

## Repro

`grep -rn "with-lock" stdlib/` -> nothing.

## Fix direction

A `with-lock` macro over stdlib/concurrent.tur's `mutex-guard-lock`/
`mutex-guard-unlock`, optionally a value-cell Mutex struct.

## Guides to update when fixed

- docs/guides/threading-guide.md
- docs/guides/stm-guide.md (lock comparisons)
