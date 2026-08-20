# REPL :doc builtin table still documents removed try/catch/throw

**Severity: low**. Found in the 2026-08-20 docs audit.
**Status: RESOLVED**.

## Repro

`tur repl`, then `:doc try` -> "(try body (catch err handler)) -- catch
runtime errors", but `(try ...)` no longer elaborates (exceptions were deleted
end-to-end in v0.25.0, CHANGELOG.md:1974).

## Root cause

src/turi/repl.c:768-770 (the "/* Error handling */" entries) were not removed
with the 0.25.0 deletion, so the prompt kept describing a form that had not
existed for eleven releases.

## Resolution

The three dead rows are replaced by the surface that actually shipped:

| `:doc` name | text |
|---|---|
| `panic` | `(panic msg) -- abort with an unrecoverable error` |
| `panic-with` | `(panic-with value) -- panic carrying a typed payload` |
| `catch-unwind` | `(catch-unwind thunk) -- run thunk, returning a Result whose err slot carries the Panic` |
| `catch-panic-of` | `(catch-panic-of Type thunk) -- like catch-unwind, but re-raises panics whose payload is not Type` |

`:doc try` / `:doc catch` / `:doc throw` now correctly report
*"no documentation for 'try'"*. A comment on the block records that these were
deleted in v0.25.0 so they do not get re-added.

## Tests

`tests/run-flags.sh`, case `repl-doc-no-exceptions` -- asserts both halves:
the removed strings are gone, all three dead names report no documentation,
and the replacement rows describe `panic` and `catch-unwind`. Without the
first half a future re-add would pass a test that only checked the new rows.

## Guides updated

None -- code-only. docs/guides/error-handling-guide.md was already correct.
