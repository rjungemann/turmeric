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

## Summary

A `defgadt` whose constructor section contains a non-list form (for example a
stray `:copy` keyword, which `defgadt` does not accept) emits the correct
diagnostic -- `defgadt: constructor must be a list form` -- but leaves the
`AdtDef` registered with a NULL entry in `ctors[]`. A subsequent `match` on a
value of that type walks `adt->ctors[ci]->name` and dereferences the NULL
`CtorDef`, crashing the compiler.

## Minimal repro

```turmeric
(defgadt Bound [A] :copy
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

## Validation

The repro should report the `constructor must be a list form` error and exit
non-zero with no ASan SEGV. A regression fixture under
`tests/fixtures/errors/` with `expected.diag` containing
`constructor must be a list form` would lock this in.
