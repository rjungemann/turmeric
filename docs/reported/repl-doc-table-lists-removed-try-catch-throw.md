# REPL :doc builtin table still documents removed try/catch/throw

**Severity: low**. Found in the 2026-08-20 docs audit.

## Repro

`tur repl`, then `:doc try` -> "(try body (catch err handler)) -- catch
runtime errors", but `(try ...)` no longer elaborates (exceptions were deleted
end-to-end in v0.25.0, CHANGELOG.md:1974).

## Root cause

src/turi/repl.c:768-770 (the "/* Error handling */" entries) were not removed
with the 0.25.0 deletion.

## Fix direction

Delete the three entries; optionally add `panic` / `catch-unwind` entries
pointing at the Result-based model.

## Guides to update when fixed

- none (code-only; docs/guides/error-handling-guide.md is already correct)
