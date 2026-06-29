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

1. **Applied-type (`TY_APP`) fields -- highest value, lowest risk. DONE
   (2026-06-29).**
   Widened `defstruct_field_type_lowerable` to accept a *user* `TY_APP` field
   (`(Option cstr)`, `(Box X)`, `(Dense m A)`, `(Tbl #row{..})`), while keeping
   built-in compound forms -- `(lref T)`/`(& T)`/borrow, `exists`/`forall`,
   `handler`/`arrow`/session/role/global/project -- on the struct path (they are
   their own TypeKinds, not `TY_APP`, and a couple carry struct-path-only
   diagnostics like the `:copy`-over-linear-field check).  The codegen tail the
   caveat predicted:
   - `adt_field_is_inline_byval` gained a `TY_APP` arm so a by-value monomorph
     field (`tur_adt_Option__cstr`) is stored INLINE and read directly (no
     cast-to-aggregate).
   - the record-ADT typedef emitter pre-flushes each inline-by-value `TY_APP`
     field monomorph (mirroring the struct path) so the embedding aggregate sees
     a complete typedef.
   - the `Result`/`Option` box-as-pointer (`tur_adt_T *` slot) was extended from
     value-structs to non-parametric by-value record ADTs (a lowered `defstruct`
     like `User`), centralised in `adt_field_is_ros_pointer_box` and given
     precedence over the B4 wide-element int64 box; the construction, extraction,
     forward-decl, unused-slot-default, and carrier->concrete decode sites all
     consult it.
   Cleared the largest carve-out group; full by-value suite green (1871/0).
2. **`:no-auto-ctor` suppression -- mechanical. DONE (2026-06-29).**  The ADT
   lowering now honours `:no-auto-ctor`: `defstruct_lowers_to_adt` no longer bails
   on it; `elab_defdata` accepts a `:no-auto-ctor` keyword (the lowering forwards
   it) and records `AdtDef.no_auto_ctor`.  The value-namespace constructor binding
   is still created (make-struct rewrites `(make-struct Name ...)` to the ctor call
   `(Name ...)` and needs it), but a DIRECT `(Name ...)` call is rejected in
   elab_call -- gated by a transient `make_struct_ctor_rewrite` flag that
   make-struct sets around its own rewrite, mirroring the struct path's
   `!no_auto_ctor` gate.  Negative fixture `errors/struct-no-auto-ctor-rejects-call`
   still rejects; new positive fixture `struct-no-auto-ctor-make-struct` confirms
   make-struct + field access on the lowered record.  Suite green (1871/0).
3. **`exists` (`TY_EXISTS`) fields -- moderate. DONE (2026-06-29).**  An
   `exists`-pack field is carried as the int64 existential-record pointer
   (existential packing already boxes a wide aggregate payload into that carrier
   slot), so it lowers like any scalar carrier field once
   `defstruct_field_type_lowerable` accepts `exists`/`exists_u` (only `forall`
   stays excluded -- it is not a value-carrying field form).  The witness/dict
   plumbing the caveat flagged needed no new work: `pack`/`open`/`.field`
   dispatch over the lowered record reuse the existing existential machinery.
   One construction-side gap had to be re-closed: the lenient ADT-ctor rewrite
   make-struct uses relaxed the DS1 per-field check, so a raw `42` passed where
   `(exists [a] [(C a)] a)` is expected slipped through (and SEGV'd at `open`);
   the make-struct rewrite now re-imposes the TY_EXISTS full_type check.
   Fixtures `exg4-pack-into-struct(-via-let)`, `exists-pack-multifield-struct`,
   `exists-pack-constrained-byval-struct` (positives) and
   `errors/defstruct-compound-field-mismatch` (negative) all green; suite
   1872/0.  With slices 1-3 done the only carve-out left is `:linear` (slice 4).
4. **`:linear` outer structs. DONE (2026-06-29).**  (The plan's "zero `defstruct
   :linear` fixtures" premise was off -- `inline-c-struct-return-cstr-params`'s
   `Handle` plus the stdlib `Socket`/`FileHandle`/`MutexGuard` exercise it.)  The
   key realisation: linearity enforcement keys on the TYPE's `copy_kind` (and the
   bindings derived from it), not on `StructDef` identity, so the record-ADT path
   needs no new exactly-once modeling -- it only has to carry `CK_LINEAR` on the
   lowered type.  Changes: `AdtDef` gains `is_linear`/`is_affine`; `type_adt`
   derives `copy_kind`/`substruct` from them exactly as `type_struct` does;
   `elab_defdata` parses a `:linear` keyword (the lowering forwards it) and sets
   `def->is_linear`; `defstruct_field_type_lowerable`/`defstruct_lowers_to_adt`
   stop bailing on `:linear`.  Verified the exactly-once discipline survives the
   lowering: new negative fixture `errors/struct-linear-double-use` (double-use ->
   TUR-E0101) and positive `struct-linear-lowered`; the stdlib resource types and
   all existing linear fixtures stay green (1874/0).
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
