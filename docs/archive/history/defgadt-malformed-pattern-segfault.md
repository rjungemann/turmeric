---
title: Malformed defgadt constructor crashes elab_match with a NULL deref
category: Reported
description: When a defgadt has a constructor list that fails to parse (e.g. a stray :copy keyword where a constructor list form is expected), the type is half-registered with a NULL constructor entry. A later (match ...) on that type dereferences the NULL CtorDef and segfaults instead of stopping at the earlier diagnostic.
---

# Malformed `defgadt` constructor crashes `elab_match` (NULL deref)

> **Severity:** compiler crash on invalid input (should be a clean
> diagnostic; ASan reports a SEGV).
> **Found:** 2026-06-04, while probing `:copy` support for
> [defgadt-copy-and-shared-bounds.md](defgadt-copy-and-shared-bounds.md).
> **Status:** FIXED 2026-06-04 in `src/compiler/elab_structs.c` (`elab_defgadt`).
> Regression fixture: `tests/fixtures/errors/defgadt-malformed-ctor-no-crash/`.

## Summary

A `defgadt` whose constructor section contains a non-list form (for example a
stray `:copy` keyword, which `defgadt` does not accept) emits the correct
diagnostic -- `defgadt: constructor must be a list form` -- but leaves the
`AdtDef` registered with a NULL entry in `ctors[]`. A subsequent `match` on a
value of that type walks `adt->ctors[ci]->name` and dereferences the NULL
`CtorDef`, crashing the compiler.

## Minimal repro

The original repro relied on `:copy` *not* being an accepted `defgadt`
annotation, so it parsed as a (non-list) constructor form. `:copy`/`:move`
are now accepted there, so the repro below uses a stray `:bogus` keyword to
hit the same "constructor must be a list form" path -- any non-list
constructor form reproduces it.

```turmeric
(defgadt Bound [A]
  :bogus
  (Inclusive int : (Bound int)))

(defn f [b : Bound] : int
  (match b (Inclusive v) v))

(defn main [] : int 0)
```

```sh
tur run -Xgadt repro.tur
```

## Observed vs. expected

- **Observed:**
  ```
  error: defgadt: constructor must be a list form
  src/compiler/elab_structs.c:2642:21: runtime error: member access within
    null pointer of type 'struct CtorDef'
  AddressSanitizer: SEGV ... in elab_match
  ```
- **Expected:** the parse error is emitted and elaboration stops cleanly (or
  the half-built `AdtDef` is not registered / has no NULL ctor slots), with no
  crash.

## Root cause

`src/compiler/elab_structs.c:2642`, inside `elab_match`:

```c
for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
    if (strcmp(adt->ctors[ci]->name, ctor_sym->name) == 0) {  // adt->ctors[ci] is NULL
```

When `elab_defgadt` hits the malformed constructor it bails after having
already bumped `n_ctors` / allocated the slot but before filling it, so
`ctors[ci]` stays NULL. `elab_match` assumes every slot in `[0, n_ctors)` is
populated.

## Proposed fix directions

1. In `elab_defgadt`, only increment `n_ctors` (or only register the `AdtDef`)
   once every constructor parsed successfully; on error, don't leave a
   half-built def in the registry.
2. Defensively skip / treat-as-error NULL `ctors[ci]` entries in the
   `elab_match` lookup loop.

Either alone removes the crash; (1) is the cleaner root-cause fix.

## Fix applied

Direction (1). In `elab_defgadt`, `ci` is now declared outside the
constructor-parse loop, and every error path inside that loop jumps to a new
`ctor_parse_error:` label that does:

```c
ctor_parse_error:
    /* slots [ci, n_ctors) are still NULL; truncate so a later (match ...)
     * doesn't deref a NULL CtorDef. Compilation already failed. */
    def->n_ctors = ci;
    return NULL;
```

Because every error path bails *before* `def->ctors[ci] = ctor;`, `ci` is
exactly the count of fully-populated slots. `elab_match` then only walks
populated entries. (The loop's bound uses the local `n_ctors`, so truncating
`def->n_ctors` on the error path does not affect parsing.)

## Validation

The repro now reports the `constructor must be a list form` error and exits
non-zero with no ASan SEGV. Locked in by the regression fixture
`tests/fixtures/errors/defgadt-malformed-ctor-no-crash/`, whose
`expected.diag` requires **both** `defgadt: constructor must be a list form`
and `is not a constructor of 'Bound'` -- the second diagnostic only prints if
elaboration survives the malformed constructor instead of crashing at
`match`, so a re-regression (NULL-deref crash) would drop it and fail the
fixture. Full suite: `1441 passed, 0 failed`.
