---
title: StructDef deletion -- scope (slice 5 proper)
category: Planning
description: Evidence-based scope for the final slice-5 phase of structdef-retirement -- deleting the StructDef type and the TY_STRUCT kind. Measured 2026-06-30, after B1-B4 completed the def-less placeholder migration. The headline finding is that StructDef is nearly dead already -- one residual producer stands between the current tree and zero-producer StructDef.
---

# StructDef deletion -- scope (slice 5 proper)

Companion to [structdef-retirement-plan.md](structdef-retirement-plan.md).
That plan's slice 5 ("Delete `StructDef` -- the bulk") was written before the
B-track (B1-B4) landed and estimated the deletion as a **LARGE, all-or-nothing,
198-ref** effort dominated by (a) migrating the tyvar/placeholder representation
and (b) untangling typeclass dispatch from `StructDef` identity. **Both of those
fears are now retired**, so this document re-scopes the deletion from measured
current state rather than the stale estimate.

## What changed since the plan's slice-5 estimate

- **B1-B4 DONE** -- every def-less `TY_STRUCT` placeholder producer is migrated
  to a named `TY_TYVAR`, and `assert(def != NULL)` at the three hot readers
  (`type_name`, `type_c_name`, `append_type_mangle`) proves no def-less
  `TY_STRUCT` is constructed anywhere (suite green 1874/0 with the asserts
  live). The "migrate the tyvar representation" sub-project the plan called the
  riskiest remaining piece is **already done**.
- **`defopaque` migrated** to an opaque `AdtDef` (slice 5 step 1) -- no longer a
  `StructDef` producer.
- **`defstruct` lowers to a record `AdtDef`** for every shape the gate accepts
  (slices 1-4 widened it to scalars/pointers/fn/aggregate/parametric/applied/
  `exists`/`:heap`/`:linear`/`:affine`/`:copy`/`:move`/`:no-auto-ctor`).

## Current footprint (measured 2026-06-30)

| Symbol | refs | files |
|---|---|---|
| `StructDef` | **222** | 21 (incl. `src/turi/eval.c`, which the 198 estimate missed) |
| `TY_STRUCT` | **~380** | ~22 |

Heaviest `StructDef` files: `elab_structs.c` (36), `types.c` (30),
`elab_typeclasses.c` (24), `turi/eval.c` (20), `emit_module.c` (18),
`types.h` (17), `emit_expr.c` (17).

## The headline finding: StructDef is one fix away from zero producers

`e->struct_defs[]` (the registry every named-`TY_STRUCT` producer reads) is
written by **exactly one function**, `elab_register_struct_def`, called from
**two sites**, both gated on `!defstruct_lowers_to_adt`:

- `elab_structs.c:1354` -- the residual `defstruct` elaboration path.
- `elab_toplevel.c:1092` -- its pre-pass forward-declaration stub.

An instrumented full-suite run (temporary `fprintf` at the residual `StructDef`
allocation, `elab_structs.c:1334`) shows the residual path fires for **exactly
one struct in the entire fixture suite**:

```
1  B5-RESIDUAL-STRUCTDEF: World   (tests/fixtures/defstruct-grouped-field-specs)
```

`World` there is macro-generated grouped field specs
(`(defworld World (pos vel))` -> `[gens : int [pos : int] [vel : int]]`). No
stdlib type and no other fixture hits the residual path. The `defstruct` gate
lowers everything else.

**Consequence:** once the grouped-field-specs case lowers, `defstruct_lowers_to_adt`
is always true, `elab_register_struct_def` is never called, `e->struct_defs[]`
stays empty, and **no named `TY_STRUCT` is ever produced**. Combined with the
B4 proof that no def-less `TY_STRUCT` is produced either, **the entire
`TY_STRUCT` kind becomes uninstantiated** -- every `as.struct_.def` reader and
every `TY_STRUCT` switch arm becomes provably dead code. The deletion is then a
mechanical dead-code sweep, not a semantic rewrite.

### The typeclass-entanglement fear resolves to dead-branch deletion

The plan flagged instance dispatch keying on `StructDef` identity
(`elab_typeclasses.c`: 24 `StructDef` / 75 `TY_STRUCT` refs) as "the riskiest
part." But lowered structs are already `AdtDef`s and instance selection already
dispatches on `AdtDef` for them (that is how the suite passes today). With no
named `TY_STRUCT` produced, the `StructDef`-identity dispatch branches are
unreachable -- deleting them is removal of dead code, not a dispatch rewrite.

### The interpreter (`turi/eval.c`) is a consumer with ADT fallbacks

`turi/eval.c`'s 20 `StructDef` refs are all reads of a make-struct value's
attached `def` (`TuriStruct.def`, documented "may be NULL"), and it already has
the `CtorDef`-field-name fallback for the NULL case (`eval.c:2787`). So the
interpreter needs its `StructDef` reads redirected to the `CtorDef`/`AdtDef`
path, not new logic.

## Prerequisite

**DS-A -- lower grouped field specs (the last producer).**  The plan's task
`A2`. `defstruct_fields_all_primitive` (`elab_structs.c:845`) walks the field
vector expecting `F_SYM` field names and bails (`return false`) on a grouped
sub-vector `[b : int]` (an `F_VEC`). Direct grouped specs (`Mixed`) already
lower -- only the macro-generated variant (`World`) still trips the gate,
because the pre-pass at `elab_toplevel.c:1075` and the elaboration check at
`elab_structs.c:1105` see the field form at different normalization stages. Fix:
run the same grouped-spec flattener the elaborator uses **before/inside** the
gate at both check sites so they agree. Small, self-contained, its own commit
with the suite green. After it, re-run the instrumented probe and confirm the
residual count is **zero**.

## Deletion slices (after DS-A)

Each lands with the by-value suite green (1874/0) and, where codegen moves,
regenerated snapshots in the same commit.

- **DS-B -- prove zero producers.**  Add an `assert(0)`-style guard (or a hard
  error) at `elab_register_struct_def` and the two residual sites, or a
  producer-count ratchet (plan task E). Suite green with the guard live proves
  `StructDef` has no producers -- the precondition for deletion. Cheap, and it
  ratchets against regressions while the deletion is in flight.

- **DS-C -- delete the now-dead `TY_STRUCT` consumer paths, per file.**  The
  bulk of the ~380 `TY_STRUCT` refs. Each `case TY_STRUCT:` / `if (... kind ==
  TY_STRUCT)` / `as.struct_.def` read is now unreachable (DS-B proves it).
  Delete arm-by-arm, file-by-file, rebuilding + suiting between files so a
  mistaken "not actually dead" branch surfaces immediately. Suggested order
  (leaf consumers first, dispatch last):
  1. codegen: `emit_expr.c`, `emit_module.c`, `emit_core.c`, `emit_fns.c`,
     `emit_stmt.c`, `emit_cps.c`.
  2. passes: `kind_check.c`, `effect_check.c`.
  3. elaboration leaves: `elab_call.c`, `elab_fns.c`, `elab_types.c`,
     `elab_core.c`, `elab_forms.c`, `elab_memory.c`, `elab_toplevel.c`.
  4. typeclass dispatch: `elab_typeclasses.c`, `typeclass.c` (dead
     `StructDef`-identity branches).
  5. interpreter: `turi/eval.c` -- redirect the `TuriStruct.def` reads to the
     `CtorDef`/`AdtDef` path (the NULL fallback already exists).

- **DS-D -- delete the type + kind.**  Once no code reads them:
  - remove `StructDef` / `StructField` from `types.h`, the `as.struct_` union
    member, and the `TY_STRUCT` enumerator;
  - remove the `Expr` carriers in `expr.h` (`make_struct_.def`,
    `set_field_.def`, carrier `box_struct`/`target_struct` -- 8 refs);
  - collapse the mangler's placeholder convention: `TY_STRUCT`'s `"struct"`
    arm in `append_type_mangle` goes away, but `TY_TYVAR`/`TY_UNKNOWN` **must
    keep** the `"struct"` token (the carrier-name convention the
    baseline-ctor-option-struct-mangling fix established -- see that archive
    note); regenerate snapshots;
  - remove `elab_register_struct_def`, `e->struct_defs`/`n_struct_defs`/
    `cap_struct_defs`, and `defstruct_lowers_to_adt` (now always-true; its
    callers become unconditional).
  - `make-struct` / `set-field` / `.field` on a former struct already route
    through the record-`AdtDef` ctor + `CtorDef` fields, so the surface forms
    keep working; DS-D only removes the dead `StructDef`-typed plumbing behind
    them.

## Risk / sequencing notes

- **Not actually all-or-nothing anymore.**  DS-A + DS-B are independently
  committable; DS-C is a long but *incremental* dead-code sweep (green between
  files); only DS-D's final type/kind removal is a single atomic commit, and by
  then everything reading the type is already gone.
- **Snapshot churn**: DS-A and the mangler collapse in DS-D touch codegen;
  regenerate in-commit per the CLAUDE.md fixture rule. DS-C is mostly
  dead-branch deletion with no codegen movement.
- **Keep the placeholder mangle token.**  The single sharpest footgun: do NOT
  drop the `"struct"` mangle token when removing the `TY_STRUCT` arm --
  `TY_TYVAR`/`TY_UNKNOWN` still need it or `(Option <tyvar>)` monomorphs
  desync (the exact regression the baseline mangling fix repaired).
- **Verify with ASan/UBSan on** (the default suite build) so a mistakenly-live
  branch that now dereferences a removed union member is caught, not silently
  miscompiled.
- **External surface**: check the sibling `../turmeric-spices/` checkout (if
  present) and the wasm/web glue don't define structs that hit the residual
  path before DS-A; the in-tree suite says none do.

## Bottom line

The deletion the plan sized as a large, risky, all-or-nothing rewrite is, after
B1-B4, a **one-fix prerequisite (DS-A) + a mechanical dead-code sweep**. The
semantic risk (tyvar representation, typeclass dispatch) is already retired; what
remains is volume, and it decomposes into safely-committable, suite-green steps.
