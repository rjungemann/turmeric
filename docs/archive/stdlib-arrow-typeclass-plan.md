# Plan: Fix `stdlib/arrow.tur` Typeclass Method Calls

> **Status:** Complete
> **Last Updated:** 2026-05-28
> **Type:** Stdlib
> **Tracks:** KB-011 (see `docs/archive/history/known-bugs.md`)

---

## Overview

`stdlib/arrow.tur` fails to load:

```
$ ./build/tur check stdlib/arrow.tur
stdlib/arrow.tur:325:3: error: 'arr' is not a function or continuation
```

`Arrow` is a typeclass with methods `arr`, `>>>`, `first`, `second`.
Two helper functions in the same file (`arrow-id`, `arrow-comp`) call
these names *as if they were free functions* rather than via typeclass
dispatch.  The elaborator looks up `arr` in value scope, finds only the
typeclass-method binding, and rejects the call.

The file is not auto-loaded by `src/main.c`, so the bug is dormant for
most users -- but no test exercises `arrow.tur` directly, so any regression
in arrow code goes silently unnoticed.  The
`tests/fixtures/stdlib-arrow/input.tur` fixture inlines its own
combinators instead of using the stdlib helpers.

---

## Root cause

```turmeric
;;; stdlib/arrow.tur:325
(defn arrow-id [^arr a]
  (arr (fn [x] x)))            ; arr is an Arrow method -- needs dispatch

(defn arrow-comp [^arr a b c f g]
  (>>> f g))                   ; >>> is an Arrow method -- needs dispatch
```

The `^arr a` syntax declares a constraint (`a` is an Arrow), but
inside the body there's no Arrow-typed *receiver* in scope to dispatch
on.  The naked call `(arr ...)` therefore has no instance to pick.

This is a stdlib coding mistake, not a compiler bug -- typeclass
dispatch needs a receiver.  But the file's intent (provide free
helpers parameterised over any Arrow instance) is reasonable; it just
needs to be expressed as either:

1. **Explicit dispatch on a sentinel argument**, threading the Arrow
   instance through as a value.
2. **A different shape entirely** -- e.g., expose the inline-C
   `__arrow_call*` helpers as the public surface and drop the
   typeclass-dispatching `arrow-id`/`arrow-comp` wrappers.

Option 2 is simpler and matches what the in-tree `Arrow [->]` instance
already does internally; option 1 is more polymorphic but requires
machinery (a "phantom dictionary" pattern) that doesn't yet exist in
Turmeric.

---

## Plan

### Phase 1 -- Add a `stdlib-arrow-load` fixture

A failing fixture that does nothing but `(load "stdlib/arrow.tur")`
catches the regression before it spreads.  Mark it `requires.compiled`
(typeclass dispatch in the interpreter may differ from compiled
behaviour); start with `expected.stderr` matching the current error,
then flip to `expected.stdout` once the file loads cleanly.

### Phase 2 -- Drop the broken helpers (Option A)

Remove `arrow-id` and `arrow-comp` from `stdlib/arrow.tur`.  Callers
that want an identity arrow write `(.arr arr-instance (fn [x] x))`
directly with their own Arrow-typed receiver in scope.

Rationale: the helpers are tiny, the inline form at the call site is
clearer once you accept that typeclass dispatch needs a receiver, and
the rejected alternatives (routing through `__arrow_call*` inline-C
helpers, or renaming the typeclass methods) both add ABI surface that
we'd then need to maintain forever for marginal expressiveness gain.

Before deleting, grep `../turmeric-spices/` and any vendored spices
for `arrow-id` / `arrow-comp` to confirm zero external callers (see
Risks below).

### Phase 3 -- Add a load-time regression

Once Phase 2 lands, augment `tests/run.sh` (and `tests/run-turi.sh`)
to compile-check every file in `stdlib/` on each test run.  This
catches any future stdlib file that ships in a non-loading state.

A minimal implementation: add a `stdlib-checks` target to CMake that
runs `./build/tur check stdlib/<file>.tur` for each `.tur` in
`stdlib/`, gated by `requires.compiled`.

### Phase 4 -- Promote the inlined arrow combinators

The fixture `tests/fixtures/stdlib-arrow/input.tur` currently
inlines `arr` / `>>>` / `first` / `second`.  Once `stdlib/arrow.tur`
loads cleanly, port that fixture to `(load "stdlib/arrow.tur")` and
delete the inline duplicates.  This makes the fixture an
end-to-end regression for the file's public API.

---

## Out of scope

- Adding a "phantom dictionary" / explicit-dictionary-passing form
  to Turmeric's typeclass system.  That's a larger language design
  question.
- Adding more Arrow instances (`Kleisli`, `Arrow [Function]`, etc.)
  to stdlib.  This plan only restores the file to loading state.

---

## Risks

- **Hidden callers of `arrow-id` / `arrow-comp`.**  The grep before
  Phase 2 should turn up zero usages outside the file itself
  (because the file doesn't load), but search
  `../turmeric-spices/` and any vendored spices too.
- **Stdlib drift.**  If `arrow.tur` ships features that depend on
  `arrow-id` (e.g., an `Arrow.empty` default method), removing the
  helper has knock-on consequences.  Verify the file is otherwise
  free of internal callers.

---

## Verification

- `./build/tur check stdlib/arrow.tur` succeeds.
- The Phase 1 load fixture passes.
- The Phase 4 fixture passes (now using stdlib instead of inline
  combinators).
- `tests/run.sh` and `tests/run-turi.sh` both green.
