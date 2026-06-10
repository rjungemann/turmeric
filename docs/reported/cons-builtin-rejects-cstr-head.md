---
title: `cons` builtin rejects `:cstr` head, blocking spices-side paydown
category: Defect
severity: medium (expressiveness gap; blocks `spices-cons-workaround-paydown-plan.md` Step 1)
status: open
discovered: 2026-06-09
related:
  - docs/reported/defmodule-export-scoping-track.md (Defect B; F3+F6 introduced this builtin)
  - docs/upcoming/spices-cons-workaround-paydown-plan.md (premise broken by this defect)
  - commit bc2074ad (builtins.c entry adding `cons`)
---

## Summary

The user-callable `cons` builtin added in commit bc2074ad is registered with
signature `arity 2..2 arg=int result=int` (single arg-type slot, applied to
both head and tail). Calling it with a `:cstr` head -- the exact shape the
spices-side workaround paydown plan intends to migrate -- raises
`unknown function or operator 'cons'` with a `note: available overload: cons
arity 2..2 arg=int result=int`.

Repro (with current `./build/tur` from HEAD `9e5f3c30`):

```turmeric
(defmodule repro
  (defn mk [] : int
    (cons "hello" 0)))                ;; cstr head, int tail
```

```
error: unknown function or operator 'cons'
  (cons "hello" 0)
   ^^^^
note: available overload: cons arity 2..2 arg=int result=int
```

The same error fires from real call sites in
`../turmeric-spices/spices/c-dsl/src/c-dsl/builtins.tur:159` and
`../turmeric-spices/spices/glsl/src/glsl/stdlib.tur:47` when their
`__cons1`/`__cons` helpers (declared `[h : cstr ...]`) are rewritten to
`(cons head 0)` per the paydown plan.

## Why this is a bug, not by-design

1. The cell layout the builtin emits (`{int64_t head; int64_t tail;}`) is
   already pointer-agnostic -- it stores whatever 64-bit value the caller hands
   over. Existing `__tur_cons_of` / `tcons` helpers and the
   `__cdsl_cons` / `__glsl_cons` / `__gj_cons` walker structs documented in
   CLAUDE.md ("Cons-list manipulation in `#{Unsafe}` code") all rely on this
   pointer-as-int64 convention.
2. The pre-existing `__cons1` workaround was a `defn [h : cstr] : int` whose
   inline-C cast `(int64_t)(intptr_t)h` -- that cast is what's currently
   missing from the builtin's call-site type check.
3. The builtin is the *only* path to construct a cons list under project-mode
   `defmodule` (per the bc2074ad commit message: stdlib auto-load is skipped in
   project mode). Restricting it to `:int`-only heads forecloses the entire
   "cons list of strings/handles" pattern that c-dsl/glsl use, which is the
   spice-tree's documented idiom for passing fragment lists into codegen.

## Observed vs. expected

- **Observed**: `(cons (c-include-sys "stdio.h") 0)` -- where the head is
  `:cstr` -- is rejected at elaboration with `unknown function or operator
  'cons'` because no overload's arg type matches `:cstr`.
- **Expected**: the head is coerced (or accepted) as any pointer/int64-sized
  value. At minimum `:cstr` and any `:opaque`/handle type should pass;
  matching the `& rest` variadic policy ("rest type fully type-checked but
  primitives + opaques accepted at the declared identity") would be ideal.

## Root-cause pointers

- `src/builtins.c` -- the `cons` builtin entry uses a single `arg=int`
  parameter; the elaborator dispatch in `elab_call.c` (`g_uses_cons` gate)
  treats both positional args as `:int` without a coercion path.
- `src/emit_module.c` -- `emit_cons_helper` writes `static int64_t
  cons(int64_t head, int64_t tail)`, so the *C* signature is already
  pointer-agnostic; only the *elaborator* check is too strict.

## Proposed fixes

Pick one (in increasing scope):

1. **Lowest-risk**: at call sites, coerce non-`:int` head args to `:int` via
   the same path used for cstr-as-int64 inline-C arg passing (the receive side
   the inline-C carrier emits). This mirrors how `__cons1`'s `:cstr` parameter
   already implicitly fits in an `int64_t` register.
2. **Cleaner**: change the builtin's head-arg type slot from `:int` to a
   "pointer-or-int" wildcard recognized by elab (similar to how `& rest` with
   an `:opaque` declared type accepts the opaque identity). The C codegen is
   unchanged.
3. **Most general**: register two overloads -- `(cons :int :int) : int` and
   `(cons :cstr :int) : int` -- and let the existing overload resolver pick.
   Lowest blast radius if multi-arg-type-slot builtins are already supported
   elsewhere.

## How to validate a fix

- Add a fixture under `tests/fixtures/cons-builtin-cstr-list/` that calls
  `(cons "a" (cons "b" 0))` inside a `defmodule` and asserts the cells round-
  trip through `list-head` cast back to `:cstr`.
- Rerun the spices-side paydown plan
  (`docs/upcoming/spices-cons-workaround-paydown-plan.md`) end-to-end: the
  c-dsl `__cons1` removal + `(cons (c-include-sys "...") 0)` substitution
  should then build clean across `c-dsl`, `glsl`, `tourist`, `httpd`, `stats`,
  and `linalg`.
- `bash tests/run.sh` must remain zero-FAIL.

## Workaround until fixed

Keep the `__cons1` (c-dsl) and `__cons` (glsl) inline-C helpers in place.
They are documented as workarounds in
`docs/upcoming/spices-cons-workaround-paydown-plan.md`; that plan should be
held until this defect is resolved. The plan's Step 2 (audit for stray
`(load "stdlib/macros.tur")`) is independent and already verified clean
(grep returned no hits).
