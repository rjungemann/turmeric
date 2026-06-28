---
title: StructDef surface-path retirement
category: Planning
description: Post-graduation follow-up to defstruct-as-defadt. With `defstruct` lowering always-on, the legacy `StructDef` path survives only for a few gate carve-outs. This plan scopes the work to widen those carve-outs and ultimately delete the `StructDef` type. LARGE effort; sliced.
---

# StructDef surface-path retirement

## Context

`defstruct-as-defadt` graduated 2026-06-28 (see
[defstruct-as-defadt-plan.md](defstruct-as-defadt-plan.md)): a `defstruct` now
lowers to a single-variant record `defadt` unconditionally.  The legacy
`StructDef` elaboration + codegen path is **retained** only because
`defstruct_lowers_to_adt` (`src/compiler/elab_structs.c`) still rejects a few
struct shapes, which keep the struct path.  Those carve-outs are *correct* (no
bugs); retiring `StructDef` is pure architectural cleanup, not a fix.

This is a **LARGE** effort and should be its own dedicated work, not rushed.
`StructDef` is the *type identity* for a struct (a `TY_STRUCT`'s `as.struct_.def`
is a `StructDef*`), so it is woven through the type representation, the Expr AST,
elaboration, typeclass instance dispatch, and four codegen files.

## What still keeps the StructDef path (the carve-outs)

From `defstruct_lowers_to_adt` and `defstruct_field_type_lowerable`
(`elab_structs.c` ~859-907 / ~768-779):

1. **`:linear` outer struct** (`elab_structs.c:870-871`) -- exactly-once
   (`CK_LINEAR`) substructural semantics are not modeled on the record-ADT path.
   **0 `defstruct :linear` fixtures exist.**
2. **`:no-auto-ctor` outer struct** (`elab_structs.c:872-875`) -- the record-ADT
   path always binds a value-namespace constructor; the struct path is what keeps
   the `(Name ...)` call form rejected.  **1 fixture**
   (`errors/struct-no-auto-ctor-rejects-call`, a negative test).
3. **Any field whose type is a compound `(F_LIST)` form other than a `fn`**
   (`defstruct_field_type_lowerable`) -- specifically **applied/parametric type
   fields** (`(Option cstr)`, `(Box X)`, `(Dense m A)`, `TY_APP`) and
   **`exists`-pack fields** (`TY_EXISTS`).  This is the load-bearing category.
   - Applied-type fields: ~8-12 fixtures -- e.g.
     `applied-struct-instance-element-discrimination`,
     `defstruct-field-byvalue-parametric-struct`,
     `result-over-struct-with-option-field-typedef-order`,
     `macro-defstruct-field-type-unquote`, `exists-pack-multifield-struct`
     (`(Dense m A)` field), `typed-field-row-accept`.
   - `exists` fields: 3-4 fixtures -- `exg4-pack-into-struct(-via-let)`,
     `errors/defstruct-compound-field-mismatch`.

Everything else lowers (scalars, pointer fields, bare/typed `fn`, bare user-type
fields, parametric, `:heap`, `:copy`/`:move`).

## Slices

1. **Applied-type (`TY_APP`) fields -- highest value, lowest risk (NEXT).**
   Widen `defstruct_field_type_lowerable` to accept a `TY_APP` field, then make
   the record-ADT product's field-type resolver + codegen handle it the way a
   `let`/param of that type is already handled (the by-value-aggregate inline
   storage + `adt_field_instantiate_type` tyvar substitution already exist).
   Clears the largest carve-out group.  **Caveat:** expect a codegen tail like
   the graduation had -- each new field representation (a `(Option X)` value
   field, a nested monomorph, the `User`-pointer-slot interaction landed during
   graduation) needs its own crossing verified.  Drive a force-widen probe (bypass
   only the `TY_APP` rejection) and promote each build/stdout failure to a fixture
   + fix, exactly as the graduation did.
2. **`:no-auto-ctor` suppression -- mechanical.**  Teach the ADT lowering to
   suppress the auto-bound value-namespace ctor for a `:no-auto-ctor` struct (so
   `(Name ...)` stays rejected), then lower it.  1 negative fixture.
3. **`exists` (`TY_EXISTS`) fields -- moderate.**  Existential packing already
   boxes aggregates into an int64 carrier slot; host a `TY_EXISTS` field on the
   record product reusing that machinery.  The witness/dict-thunk plumbing on
   field access needs care.
4. **`:linear` outer structs -- hard, unexercised.**  No record-ADT modeling of
   `CK_LINEAR` exactly-once semantics exists, and there are zero `defstruct
   :linear` fixtures.  Natural LAST slice (or leave on StructDef longest); write
   fixtures first.
5. **Delete `StructDef` -- the bulk, separately scoped.**  Once the gate is
   effectively always-true, migrate every `StructDef`-keyed site to the ADT def
   and delete the type.  **Footprint: ~197 references across 19 files** under
   `src/compiler/`:
   - type core: `types.c` (30), `types.h` (15) -- struct identity is a
     `StructDef*`;
   - Expr/AST: `expr.h` (8) -- `make_struct_.def`, field-access/`set_field_.def`,
     carrier `box_struct`/`target_struct`;
   - elaboration: `elab_structs.c` (37), `elab_typeclasses.c` (25 -- instances
     dispatch on `StructDef` identity), `elab_fns.c`/`elab_call.c`/`elab_core.c`
     and friends;
   - codegen: `emit_expr.c` (17), `emit_module.c` (16), `emit_core.c`/`emit_fns.c`/
     `emit_stmt.c`.
   The typeclass-dispatch entanglement is the riskiest part: instance selection
   keys on `StructDef` identity, so this ripples into typeclass resolution +
   codegen, not just `defstruct` elaboration.

## Recommendation

Slices 1-2 are worth doing when there is appetite for the cleanup; they shrink
the carve-out to just `exists` + `:linear` and are individually contained (with
the usual codegen-tail caveat).  Slice 5 (the `StructDef` deletion) is the large,
risky phase and should only start once slices 1-4 make the gate effectively
always-true.  None of this is urgent: the carve-outs are correct today, so this
is cleanup, sequenced behind anything that fixes real defects.
