# `(unsafe ...)` is required by raw builtins yet warns TUR-W0033 "unreachable handler"

**Severity:** low (cosmetic, but it is contradictory diagnostics on a
mandatory form, and it becomes constant noise for the R4 visible-measure
shape -- every measure body that reads through `array-get-unchecked` /
`ptr-deref` carries one warning per enclosing `unsafe` block).

## Summary

The raw-memory builtins (`raw-malloc`, `array-get-unchecked`,
`array-set-unchecked`, `ptr-deref`, `ptr-write`, ...) hard-require an
enclosing `(unsafe ...)` block (`elab_unsafe.c:19` and siblings: "requires
an enclosing (unsafe ...) block").  But those builtins do not contribute
`Unsafe` to the inferred effect row, so the handler-reachability pass
(`src/passes/effect_check.c`, the TUR-W0033 sweep around line 1054) sees an
`unsafe` handler whose body "does not perform 'Unsafe'" and warns that the
clause is unreachable.  The user is told both "you must write this form"
and "this form does nothing" about the same expression.

## Minimal repro

The in-tree fixture `tests/fixtures/unsafe-array/input.tur` reproduces it
as-is:

```sh
./build/tur run tests/fixtures/unsafe-array/input.tur
# warning [TUR-W0033]: handler clause for 'Unsafe' is unreachable: the body
# does not perform 'Unsafe'
# ... then runs correctly.
```

Removing the `(unsafe ...)` block flips it to a hard error
("raw-malloc requires an enclosing (unsafe ...) block"), so the block is
not optional.

## Root cause

Two disconnected notions of "unsafe": the *syntactic gate* in
`elab_unsafe.c` (a per-builtin walk that looks for the enclosing form) and
the *effect row* the `unsafe` handler form handles.  The builtins satisfy
the first and never touch the second, so the row under the handler is
empty and TUR-W0033 fires by its own (correct) rule.

## Fix directions

Either end works; the first is more honest:

- Make the gated builtins actually perform/record `Unsafe` in the inferred
  row (then the handler is genuinely reachable and W0033 goes silent), or
- Suppress the W0033 sweep for an `unsafe` block that syntactically
  contains at least one builtin from the `elab_unsafe.c` gated set (the
  block is mandatory for them, so "unreachable" is never actionable
  advice there).

Found 2026-08-19 while probing the R4 visible-measure shape
(trusted-refinement-claims-plan.md).
