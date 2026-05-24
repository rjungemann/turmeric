# Design note: separate struct / GADT namespaces

**Status:** proposed. Tracks `Missing Feature 4` in `docs/test-suite-cleanup-plan.md`.
**Failing test:** `tests/fixtures/gadt-stdlib-vec-stdlib`.

## Problem

`stdlib/tvec.tur` defines `(defstruct Vec [A] ...)` and is auto-loaded.
`stdlib/gadt-vec.tur` defines `(defgadt Vec [a] ...)` and is loaded
explicitly via `(load "stdlib/gadt-vec.tur")`. When the GADT is loaded
after the struct, `:Vec` parameter / return type annotations inside
`gadt-vec.tur`'s defns elaborate to the tyvar fallback rather than to
either of the registered Vec types, so the file fails to elaborate.

The underlying cause is shared name resolution: both `defstruct` and
`defgadt` insert a global `Binding` for the type name. `type_expr_from_form`
calls `scope_lookup`, which walks the scope's reverse-ordered binding
list and returns whichever was added most recently. The fact that this
sometimes returns _neither_ is a separate bug worth fixing (see "Side
issue" below), but the structural problem is namespace collision.

## Goal

Allow `stdlib/tvec.tur` and `stdlib/gadt-vec.tur` to coexist without
either file having to rename `Vec`. The MF3 stdlib-shadow check (now
landed) prevents a user `(defn Vec ...)` from masking either, so the
only collision we need to resolve is struct-vs-GADT at the type
namespace.

## Proposal

Maintain two distinct type-resolution tables on `Elab`:

| Table              | Populated by         | Lookup priority |
| ------------------ | -------------------- | --------------- |
| `struct_defs[]`    | `defstruct`          | already exists  |
| `adt_defs[]`       | `defadt`, `defgadt`  | already exists  |

The tables already exist. What's missing is the resolution rule. Type
annotations (`:Vec`, return types, field types) currently delegate to
`scope_lookup`, which collapses both kinds into the global binding list.

**Rule:** in type-annotation context, `type_expr_from_form` should
search the two tables directly and prefer GADTs over structs when both
have the same name. Rationale:

- A GADT name appearing in a type annotation almost always means the
  GADT (the struct's "phantom type" projection is rare in practice and
  can be addressed later with a `Struct/Vec` qualified form if needed).
- Constructor names (`VCons`, etc.) are unambiguous because the GADT
  registers them but the struct doesn't.
- Struct field accessors (`vec-len`) are unambiguous because the struct
  registers them but the GADT doesn't.

The Binding-list lookup remains the fallback so user-defined type
aliases and forward declarations still resolve as today.

## Diagnostics

When both tables have the same name AND the call site is ambiguous (an
edge case the prefer-GADT rule resolves silently), emit a single
warning at the `defgadt`'s span: "GADT `Vec` shadows existing struct
`Vec`; uses of `:Vec` in type annotations resolve to the GADT."

No diagnostic in the common case (only one or the other exists), and
no diagnostic when both exist but the prefer-GADT rule applies
unambiguously.

## Side issue (orthogonal)

The failing fixture's current error -- "got tyvar" -- comes from
`type_expr_from_form` falling through to the opaque-struct fallback at
the bottom of the symbol branch (around `elab_types.c:270`). That
means `scope_lookup(e->scope, "Vec")` returned NULL despite the struct
Vec definitely being registered. Worth investigating in its own right;
it might be a load-preprocessing ordering bug or a scope-chain issue
specific to `(load ...)` -spliced forms. The proposal above only
addresses the namespace collision; the missing-lookup bug should land
in the same change set.

## Out of scope

- A `Struct/Vec` / `Gadt/Vec` qualified form for the rare case where
  both need to be referenced from the same scope. Defer until a real
  use case appears.
- Renaming `gadt-vec.tur`'s type. The plan rejected this in favor of
  fixing it at the right layer.

## Implementation sketch

1. Add `Type *elab_lookup_type_by_name(Elab *e, const Symbol *name)`
   to `elab_types.c`. Walk `adt_defs[]` first, then `struct_defs[]`.
2. Replace the `scope_lookup` call in `type_expr_from_form`'s F_SYM
   branch with a call to the new helper, falling back to scope_lookup
   if the helper returns NULL.
3. Investigate and fix the side issue.
4. Add a fixture that exercises the prefer-GADT path explicitly:
   defstruct Vec, then defgadt Vec, then a defn whose `:Vec`
   annotation should resolve to the GADT.

## Estimated effort

Half a day plus a careful review of every call site of
`type_expr_from_form` and `scope_lookup`-for-types, to make sure the
priority change doesn't regress some adjacent path. The "side issue"
might double that.
