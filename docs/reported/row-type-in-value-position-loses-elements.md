---
title: A #row{...} used as a value-type annotation is silently accepted and loses its elements
category: Latent defect / missing kind validation
severity: Misleading diagnostic (prints `#row{}` for any row) plus a soundness
  gap (a kind-`[*]` row is accepted where a kind-`*` value type is required).
  Not a miscompile today because such a parameter can never be called, but it
  is a "works by luck" hole that should be closed in Layer 4.
status: RESOLVED in Layer 4 -- a bare row in value-type position is now a
  TUR-E0012 kind error (`fn_type_from_form` guard), so it never reaches the
  lossy `TY_FN` parameter storage. Regression fixture:
  tests/fixtures/errors/row-in-value-type-position.
---

# `#row{...}` in value-type position loses its elements

> **RESOLVED (Layer 4).** The repro below now emits the expected kind error
> instead of being accepted. `fn_type_from_form` rejects any value-type
> annotation whose resolved type has kind `[*]` (`KIND_TYPEROW`); the row
> arg-kind check (`check_row_type_arg_kind`) enforces that rows only flow into
> `^&`-marked row-kinded parameter slots. Kept for history.

## Summary

With the Layer 3 surface syntax (`#row{T1 T2 ...}` -> `TY_TYPEROW`, see
`docs/reported/variadic-hkt-rows-missing.md`), a row literal is accepted in a
*value*-type annotation -- e.g. a function parameter type -- where it should
instead be a **kind error** (a row has kind `[*]`, but a value's type must have
kind `*`). Worse, when it is accepted, the row's element types are dropped:
every row prints as the empty `#row{}` in later diagnostics.

## Minimal repro

```turmeric
;; -Xdata-literals
(defn f [x : #row{int bool}] : int 5)
(println (f 0))
```

Observed (`tur emit-c -Xdata-literals`):

```
error [TUR-E0001]: function 'f' arg 1: expected #row{}, got int
```

Two defects in one line:

1. **Elements lost.** The parameter type was `#row{int bool}`, but the
   diagnostic prints `#row{}`. `#row{int}` and `#row{cstr cstr cstr}` all print
   `#row{}` too -- the element list is universally dropped.
2. **Should never have type-checked this far.** A row is kind `[*]`; a function
   parameter must be kind `*`. The annotation should be rejected at elaboration
   with a kind error, long before call-site arg checking.

Expected: a kind error at the *annotation*, e.g.

```
error: `#row{int bool}` is a type-level row (kind [*]); a value cannot have row
type. A row may only appear as a type argument to a row-kinded constructor.
```

## Root cause

- **Element loss:** `TY_FN` stores parameter types as a `TypeKind arg_kinds[]`
  array (`src/compiler/types.h`), not full `Type` structs; full types are only
  retained for rank-2 / polymorphic params via `arg_full_types[]`. A
  `TY_TYPEROW` is neither, so only its `TypeKind` survives into the function
  signature. At the call site the "expected" type is reconstructed from the
  bare `TypeKind`, yielding a default/empty row -- hence `#row{}`. The printer
  (`type_name_buf`, `src/compiler/types.c`) is correct; it is handed an empty
  row. (The diagnostic at `elab_call.c:2982` uses `type_print`, which is fine.)
- **Missing kind check:** type-application argument kinds are not validated in
  v1 (`kind_of_type_app` notes "arg kind not validated separately"), and value
  *annotation* positions do not check that the resolved type has kind `*`. So a
  row slips into a parameter slot unchallenged.

## Why it is not urgent

A parameter of row type can never actually be supplied an argument (rows have
no runtime values), so such a function is uncallable and no wrong code is
emitted. It is a latent ergonomics/soundness hole, not a live miscompile.

## Proposed fix (Layer 4)

Close it as part of the row-kinded-parameter work:

1. **Reject rows in value-type position.** When a parameter / return / let /
   struct-field / cast annotation resolves to a type whose `hkt_kind ==
   KIND_TYPEROW`, emit a kind error. This is sound and needs no expected-kind
   threading -- it is a post-resolution check on the annotation.
2. **Validate type-application argument kinds.** A row is only well-kinded as an
   argument to a constructor parameter declared with kind `[*]` (the Layer 4
   row-kinded marker). Applying a row where kind `*` is expected (and vice
   versa) becomes a `TUR-E0012`.

Once (1) lands, the repro above is a clean compile error and the element-loss
symptom is moot (no row ever reaches `TY_FN` parameter storage). A regression
fixture should live at `tests/fixtures/errors/row-in-value-type-position/`.

## Validation of a fix

- The repro emits the kind error above (add as an `errors/` fixture).
- A *positive* row use -- a row passed as a type argument to a Layer 4
  row-kinded constructor -- still type-checks and prints its full elements in
  any diagnostic.
