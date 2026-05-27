# Plan: `?` Operator Return Type Alignment

> **Status:** Draft Plan
> **Last Updated:** 2026-05-27
> **Type:** Compiler / Stdlib
> **Tracks:** KB-009 (see `docs/known-bugs.md`)

---

## Overview

`(unsafe (? (get-value b)))` fails with:

```
error: if condition must be bool, got int
```

The `?` operator macro lowers to a call to `err?` (the
`Result`-discriminant predicate), but `err?` is declared `:int`
instead of `:bool` -- or the lowering pass picks up a stale `:int`
overload.  Either way, the surrounding `if` rejects the condition.

This bug is small in scope but cuts off the early-return idiom for
`Result`-typed code, which is one of the most-used patterns in
stdlib I/O.

---

## Root cause

Three places need to agree:

1. `stdlib/result.tur` -- declares `err?`.  Should return `:bool`.
2. The `?` macro -- expands `(? expr)` into code like
   `(if (err? expr) (return expr) (ok-val expr))`.  The condition
   form must accept whatever `err?` returns.
3. The elaborator's overload-resolution pass -- picks an `err?`
   specialisation for the call site.  If multiple overloads exist
   (one `:int`, one `:bool`), the wrong one may win.

The KB-009 entry suggests the `:int` declaration is the immediate
culprit, but the macro expansion may also be casting the result to
`:int` explicitly.  Both need an audit.

---

## Plan

### Phase 1 -- Reproduce

Add `tests/fixtures/result-question-op/` with the reproducer from
KB-009.  Confirm `expected.stderr` matches the current
`if condition must be bool, got int` diagnostic; this freezes the
buggy behaviour so the fix produces a visible diff.

### Phase 2 -- Audit `err?` declaration

1. `grep -n 'defn err?' stdlib/result.tur`.  Confirm the return type
   annotation.
2. If it's `:int`, change to `:bool` and update the inline-C body to
   return a `bool` (or an `int` cast to bool -- depends on whether
   `:bool` is a distinct C type in the Turmeric ABI; check
   `src/compiler/types.c` for `TY_BOOL`'s C name).
3. Audit sister predicates `ok?`, `some?`, `none?` for the same
   issue; fix them in the same pass for consistency.

### Phase 3 -- Audit `?` macro lowering

1. `grep -rn 'defmacro.*?' stdlib/` -- find the `?` macro definition.
2. Inspect the expansion.  If it inserts a redundant `(= 0 ...)` or
   `(if (!= 0 ...))` wrapper that forces an `:int` condition, drop
   it -- the underlying predicate now returns `:bool`.

### Phase 4 -- Update or add tests

The KB-009 fixture from Phase 1 changes from "expected.stderr"
(error) to "expected.stdout" (output) once the fix lands.

Add positive coverage:

| Fixture | Scenario |
|---|---|
| `result-question-op/` | `(? r)` inside an `:int`-returning function |
| `result-question-op-chain/` | Two `?` operators in sequence |
| `option-question-op/` | Same pattern on `Option` (if the macro supports it) |

---

## Out of scope

- Generalising `?` to user-defined error-monad types.  Today it's
  hardcoded to `Result`; making it polymorphic via a `Try` typeclass
  is a separate effort.
- Renaming `err?` to `error?` for clarity.  The name is in active
  use across stdlib and would be a churn-heavy rename.

---

## Risks

- **Bool-int conflation in inline-C bodies.**  Turmeric's `:bool`
  may not map to `_Bool` -- it could be a `TY_INT8` or similar.
  Changing the return type from `:int` to `:bool` may produce a
  different C signature that breaks callers using the
  cast-to-int idiom.  Mitigation: do a full `grep` for `err?`
  callers in stdlib *and* `tests/fixtures/` before changing the
  declaration.
- **Macro hygiene.**  If `?` lowers to a form that pattern-matches
  the predicate's return shape, the change may need to land in
  both files atomically.

---

## Verification

- KB-009 reproducer fixture passes (positive output).
- `(? r)` works on both top-level and nested positions.
- All Phase 4 fixtures pass under `run.sh` and `run-turi.sh`.
- No regressions in existing `Result`-using fixtures
  (`tests/fixtures/result-*`).
