---
title: Lower exotic built-in compound type forms as struct/ADT fields
category: Planning
description: Follow-up to structdef-retirement DS-A4. A handful of built-in compound type forms (forall, handler, arrow, session/role/global/project) are currently REJECTED as struct/ADT fields so the residual StructDef path could be made unreachable. This plan tracks lowering them onto the record-ADT field path so they are supported again. Rolls in the earlier docs/reported note.
---

# Lower exotic built-in compound type forms as struct/ADT fields

## Status / why this exists

As of structdef-retirement **DS-A4** (2026-06-30), a `defstruct` (or `defdata`)
field whose type is one of the *list-form* built-in compound types is a **hard
error**:

```
type form '(handler ...)' is not yet supported as a struct/ADT field; ...
```

The rejected forms are: `(forall ...)`, `(handler ...)`, `(arrow ...)` / `(-> ...)`,
the session types (`(Send ..)`/`(Recv ..)`/`(Choose ..)`/`(Branch ..)`/`(Rec ..)`/
`(Timeout ..)`), `(project ..)`, `(global ..)`, `(role ..)`.

This rejection was deliberate and load-bearing for the StructDef deletion, **not**
a considered decision that these forms should never be fields. It was the
simplest way to make the residual `StructDef` producer path *unreachable* (the
precondition for deleting the type): every `defstruct` now lowers to a record
ADT, and any field the ADT path cannot represent errors cleanly here instead of
silently falling back to the legacy `StructDef` path.

**This plan is the debt ticket to lower these forms** so they are supported as
fields again (on the record-ADT path), the way the borrow family (`(lref T)` /
`(& T)` / `(borrow-mut T)`) was lowered in DS-A3 and `fn #fx{..}` capability
fields in A1.

Not urgent: no in-tree fixture or stdlib file uses any of these as a struct
field (the DS-A4 rejection broke zero tests). Sequenced behind the StructDef
deletion itself (DS-C/DS-D) -- these forms are already off the StructDef path,
so lowering them does not block the deletion.

## What already lowers (for reference / precedent)

- scalars, pointers, user types, parametric/applied types (`(Option cstr)` etc.)
- `fn` / `c-fn` field, incl. the `#fx{..}` effect-row annotation (A1) -- the
  effect row rides on `CtorField.effect_row` and `effect_check` reads it off the
  lowered `adt_ctor`.
- `exists`-pack fields (slice 3).
- the borrow family `(lref T)` / `(& T)` / `(borrow-mut T)` (DS-A3) -- routed to
  the real type elaborator in `struct_field_type_from_form`, storage derived by
  `struct_field_storage_from_type`, and the `:copy`/linear check reproduces
  `TUR-E0102`.

Lowering an exotic form means following that same recipe.

## Root cause (rolled in from docs/reported)

`src/compiler/elab_structs.c`:

- **Gate.** `defstruct_field_type_lowerable` no longer keeps these on the struct
  path -- as of DS-A4 it returns `true` for every list form, so the struct takes
  the record-ADT path.
- **Rejection site.** `struct_field_type_from_form` (top of the `F_LIST` branch,
  the `DS-A4` block) emits the "not yet supported as a struct/ADT field" error
  and returns NULL for the exotic heads. This is the single place to relax
  per-form when lowering.
- **Why naive lowering failed originally.** Before DS-A3/DS-A4, these heads fell
  into the generic type-application loop in `struct_field_type_from_form`, which
  mis-parsed e.g. `(lref int)` as `apply(lref, int)` -> `TUR-E0012 "cannot apply
  a type of kind '*'"`. The fix pattern (borrow family) is to route the head to
  `type_expr_from_form` (which builds the correct `TypeKind`) and make
  `struct_field_storage_from_type` map that kind to a carrier slot.
- **Storage mapping.** `struct_field_storage_from_type` maps most compound kinds
  (`TY_TYVAR`/`TY_EXISTS`/`TY_FORALL`/`TY_STRUCT`/`TY_ADT`/`TY_APP`) to the int64
  carrier (`TY_INT`); `TY_LREF`/`TY_REF*`/`TY_RC`/`TY_WEAK` keep their kind. A
  form whose kind is NOT covered falls to `default: *out_kind = t->kind`, which
  may not be a valid field storage kind -- that is the per-form gap to close
  (usually: add a case mapping it to the int64 carrier).
- **Copy/linear + effect interactions.** The borrow family needed the ADT
  `:copy` check to reproduce `TUR-E0102` for a linear field (`elab_structs.c`
  CONV-S5 block). A form with its own struct-path-only diagnostic (e.g. a
  substructural or effectful field) needs the equivalent reproduced on the ADT
  path.

## High-level breakdown (to be split into tasks later)

Ordered easiest -> hardest. Each task: route the head in
`struct_field_type_from_form`, ensure `struct_field_storage_from_type` yields a
carrier slot, add a **positive** fixture that constructs the struct, reads the
field, and (where relevant) exercises the field's semantics, plus regenerate any
snapshots. Remove the head from the DS-A4 rejection block as it lands.

- **EF-1: `arrow` (`(-> A B)`).**  Lowest-hanging fruit -- it is just a function
  type, structurally identical to `fn` (which already lowers). Likely: route
  `sym_arrow` to `type_expr_from_form` like `fn`, confirm storage is the `TY_FN`
  carrier, add a fixture with a `[f (-> int int)]` field. Update the DS-A4
  rejection comment/fixture accordingly.
- **EF-2: `handler` (`(handler E V R)`).**  A handler value is a runtime object;
  as a field it is a carrier handle. Route to `type_expr_from_form` (yields
  `TY_HANDLER`), add a `TY_HANDLER` carrier case to
  `struct_field_storage_from_type`, fixture that stores and invokes a handler
  field. Watch for handler-specific lifetime/effect checks that the struct path
  applied.
- **EF-3: `forall` (`(forall [a] T)`).**  Rank-N polymorphic value in a field.
  The gate comment historically called it "not a value-carrying field form" --
  first decide whether a `forall` field is meaningful at all; if yes, carrier as
  int64; if no, keep it rejected (and move it out of this plan into a permanent
  "unsupported" note). Needs a poly-wrapper interaction review.
- **EF-4: session / `project` / `global` / `role` types.**  The most exotic;
  session-typed struct fields are niche. Group them; each needs the route +
  storage + a fixture, and a review of the session/role typechecking that the
  struct path performed. Consider whether any should stay rejected permanently.

## Verifying (per task, and the invariant DS-A4 protects)

- The **DS-B assert** in `elab_register_struct_def` must stay green: it fires if
  any `defstruct` reaches the residual `StructDef` path. Lowering a form keeps it
  on the ADT path, so the assert stays satisfied.
- To confirm a form actually lowers (not just stops erroring): build a fixture
  that **constructs** the struct and **reads** the field, run it, and diff
  stdout -- a form that resolves to a bad storage kind will fail at
  `tur build`/run, not just `emit-c`.
- Registry check (zero producers): the DS-B assert already enforces this at
  runtime across the suite; a manual sweep drops a `fprintf` in
  `elab_register_struct_def` (the single writer) -- not the alloc site, which
  misses the pre-pass stub path.

## Relationship to the StructDef deletion

- DS-A4 (this plan's predecessor) made the residual `StructDef` path unreachable
  so DS-C (delete dead `TY_STRUCT` consumer paths) and DS-D (delete the
  `StructDef` type + `TY_STRUCT` kind) can proceed -- see
  [structdef-deletion-scope.md](structdef-deletion-scope.md).
- This plan (lowering the exotic forms) is **independent of and after** the
  deletion: the forms are already off the StructDef path, so their eventual
  lowering is a pure record-ADT feature addition, not a StructDef concern.
