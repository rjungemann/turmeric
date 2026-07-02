---
title: Lower exotic built-in compound type forms as struct/ADT fields (umbrella)
category: Planning
description: Umbrella + shared context for EF-1..EF-4. Per-form plans are split out. Follow-up to structdef-retirement DS-A4, which made these forms hard errors as struct/ADT fields so the residual StructDef path could be deleted.
---

# Umbrella: lower exotic built-in compound type forms as struct/ADT fields

Per-form plans:

- [EF-1: `arrow` / `->`](structdef-exotic-field-arrow-plan.md)
- [EF-2: `handler`](structdef-exotic-field-handler-plan.md)
- [EF-3: `forall`](structdef-exotic-field-forall-plan.md) (design
  decision first: lower, reject, or shelve)
- [EF-4: session / `project` / `global` / `role`](structdef-exotic-field-session-plan.md)

Do EF-1 first (shortest delta, no new storage case), then EF-2
(establishes the "add a storage case" pattern), then EF-3 (design
gate), then EF-4 (apply the pattern eight times).

## Status / why this exists

As of structdef-retirement **DS-A4** (2026-06-30), a `defstruct` (or
`defdata`) field whose type is one of the *list-form* built-in compound
types is a **hard error**:

```
type form '(handler ...)' is not yet supported as a struct/ADT field; ...
```

The rejected forms are: `(forall ...)`, `(handler ...)`, the session
types
(`(Send ..)`/`(Recv ..)`/`(Choose ..)`/`(Branch ..)`/`(Rec ..)`/`(Timeout ..)`),
`(project ..)`, `(global ..)`, `(role ..)`. `(arrow ...)` / `(-> ...)`
now lowers (EF-1); see below.

This rejection was deliberate and load-bearing for the StructDef
deletion, **not** a considered decision that these forms should never
be fields. It was the simplest way to make the residual `StructDef`
producer path *unreachable* (the precondition for deleting the type):
every `defstruct` now lowers to a record ADT, and any field the ADT
path cannot represent errors cleanly here instead of silently falling
back to the legacy `StructDef` path.

The per-form plans linked above are the debt tickets to lower these
forms (on the record-ADT path), the way the borrow family
(`(lref T)` / `(& T)` / `(borrow-mut T)`) was lowered in DS-A3 and
`fn #fx{..}` capability fields in A1.

Not urgent: no in-tree fixture or stdlib file uses any of these as a
struct field (the DS-A4 rejection broke zero tests). Sequenced behind
the StructDef deletion itself (DS-C/DS-D) -- these forms are already
off the StructDef path, so lowering them does not block the deletion.

## What already lowers (precedent for the recipe)

- scalars, pointers, user types, parametric/applied types
  (`(Option cstr)` etc.)
- `fn` / `c-fn` field, incl. the `#fx{..}` effect-row annotation (A1)
  -- the effect row rides on `CtorField.effect_row` and `effect_check`
  reads it off the lowered `adt_ctor`.
- `arrow` / `->` field `(-> A B)` (EF-1) -- an alternate spelling of the
  `fn` type; shares the fn-type parser, lowers to `TY_FN`, and rides the
  same int64 carrier slot as the `fn` field. No new storage case.
- `exists`-pack fields (slice 3).
- the borrow family `(lref T)` / `(& T)` / `(borrow-mut T)` (DS-A3) --
  routed to the real type elaborator in
  `struct_field_type_from_form`, storage derived by
  `struct_field_storage_from_type`, and the `:copy`/linear check
  reproduces `TUR-E0102`.

Lowering an exotic form means following the same recipe.

## Shared root-cause map (rolled in from docs/reported)

`src/compiler/elab_structs.c`:

- **Gate.** `defstruct_field_type_lowerable` no longer keeps these on
  the struct path -- as of DS-A4 it returns `true` for every list form,
  so the struct takes the record-ADT path.
- **Rejection site.** `struct_field_type_from_form` (top of the
  `F_LIST` branch, the `DS-A4` block) emits the "not yet supported as a
  struct/ADT field" error and returns NULL for the exotic heads. This
  is the single place to relax per-form when lowering.
- **Why naive lowering failed originally.** Before DS-A3/DS-A4, these
  heads fell into the generic type-application loop in
  `struct_field_type_from_form`, which mis-parsed e.g. `(lref int)` as
  `apply(lref, int)` -> `TUR-E0012 "cannot apply a type of kind '*'"`.
  The fix pattern (borrow family) is to route the head to
  `type_expr_from_form` (which builds the correct `TypeKind`) and make
  `struct_field_storage_from_type` map that kind to a carrier slot.
- **Storage mapping.** `struct_field_storage_from_type` maps most
  compound kinds
  (`TY_TYVAR`/`TY_EXISTS`/`TY_FORALL`/`TY_STRUCT`/`TY_ADT`/`TY_APP`) to
  the int64 carrier (`TY_INT`); `TY_LREF`/`TY_REF*`/`TY_RC`/`TY_WEAK`
  keep their kind. A form whose kind is NOT covered falls to
  `default: *out_kind = t->kind`, which may not be a valid field
  storage kind -- that is the per-form gap to close (usually: add a
  case mapping it to the int64 carrier).
- **Copy/linear + effect interactions.** The borrow family needed the
  ADT `:copy` check to reproduce `TUR-E0102` for a linear field
  (`elab_structs.c` CONV-S5 block). A form with its own
  struct-path-only diagnostic (e.g. a substructural or effectful field)
  needs the equivalent reproduced on the ADT path.

## Shared verification (per-form plan phases refer to this)

- The **DS-B assert** in `elab_register_struct_def` must stay green: it
  fires if any `defstruct` reaches the residual `StructDef` path.
  Lowering a form keeps it on the ADT path, so the assert stays
  satisfied.
- To confirm a form actually lowers (not just stops erroring): build a
  fixture that **constructs** the struct and **reads** the field, run
  it, and diff stdout -- a form that resolves to a bad storage kind
  will fail at `tur build`/run, not just `emit-c`.
- Registry check (zero producers): the DS-B assert already enforces
  this at runtime across the suite; a manual sweep drops a `fprintf` in
  `elab_register_struct_def` (the single writer) -- not the alloc site,
  which misses the pre-pass stub path.

## Status table

Update as each per-form plan lands.

| Task | Heads                                     | Status  | Plan                                              |
|------|-------------------------------------------|---------|---------------------------------------------------|
| EF-1 | `arrow`, `->`                             | shipped (`685a0bf` route, `ffed924` fixture) | [arrow](structdef-exotic-field-arrow-plan.md)     |
| EF-2 | `handler`                                 | pending | [handler](structdef-exotic-field-handler-plan.md) |
| EF-3 | `forall`                                  | pending | [forall](structdef-exotic-field-forall-plan.md)   |
| EF-4 | session core, `project`, `global`, `role` | pending | [session](structdef-exotic-field-session-plan.md) |

## Relationship to the StructDef deletion

- DS-A4 (this plan family's predecessor) made the residual `StructDef`
  path unreachable so DS-C (delete dead `TY_STRUCT` consumer paths) and
  DS-D (delete the `StructDef` type + `TY_STRUCT` kind) can proceed --
  see [structdef-deletion-scope.md](structdef-deletion-scope.md).
- These plans (lowering the exotic forms) are **independent of and
  after** the deletion: the forms are already off the StructDef path,
  so their eventual lowering is a pure record-ADT feature addition,
  not a StructDef concern.
