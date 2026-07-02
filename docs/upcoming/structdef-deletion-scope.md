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
registry writer (`elab_register_struct_def`, the single writer of
`e->struct_defs[]`) shows the residual `StructDef` path fires for **two field-shape
categories, six structs total, across the whole fixture suite**:

```
grouped field specs      : Mixed, World   (tests/fixtures/defstruct-grouped-field-specs)  -- plan task A2
effect-annotated fn field: Action, App, Emitter, Printer                                   -- plan task A1
    (effect-subtype-capability / effect-type-alias / effect-struct-field-row /
     capability-effect-poly; each `[run : fn #fx{Eff}]`)
```

> **Correction (2026-06-30):** an earlier probe placed at the *allocation*
> site (`elab_structs.c:1334`) reported only `World` and led this doc to claim
> "exactly one residual struct." That probe was wrong -- it missed the
> pre-pass-stub-reuse path (`elab_toplevel.c:1092`), through which the
> effect-fn structs register their `StructDef`. Probing the single registry
> **writer** is the correct measure and reveals both categories. No stdlib type
> hits the residual path; the gate lowers everything else.

- **A2 (grouped field specs) -- DONE.**  The lowering gate read the raw `call`
  and bailed on the grouped `[name : type]` sub-vector while `elab_defstruct`
  flattened it into a local, so the two disagreed. Fixed by a shared
  `defstruct_flatten_grouped_field_vec` helper run in both the gate and the
  elaborator. Mixed + World now lower; suite green (1874/0).
- **A1 (effect-annotated fn field) -- REMAINING.**  See prerequisite below;
  this is an effect-soundness change, not just a gate tweak.

**Consequence:** once BOTH A1 and A2 lower, `defstruct_lowers_to_adt` is always
true, `elab_register_struct_def` is never called, `e->struct_defs[]` stays
empty, and **no named `TY_STRUCT` is ever produced**. Combined with the B4
proof that no def-less `TY_STRUCT` is produced either, **the entire `TY_STRUCT`
kind becomes uninstantiated** -- every `as.struct_.def` reader and every
`TY_STRUCT` switch arm becomes provably dead code. The deletion is then a
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

## Prerequisites (reach zero `StructDef` producers)

**DS-A / A2 -- lower grouped field specs. DONE (2026-06-30).**  The lowering
gate read the raw `call` and `defstruct_fields_all_primitive`
(`elab_structs.c:845`) bailed on a grouped `[name : type]` sub-vector (`F_VEC`),
while `elab_defstruct` flattened the grouped specs into a *local* before
building fields -- so the gate and elaborator disagreed and grouped-spec
structs took the residual `StructDef` path. Fixed by extracting the flattening
into a shared `defstruct_flatten_grouped_field_vec` helper and running it in
both the gate (before `defstruct_fields_all_primitive`) and `elab_defstruct`.
Mixed + World now lower; suite green (1874/0), no snapshot churn.

**DS-A2 / A1 -- lower effect-annotated fn fields. DONE (2026-06-30).**  A field
`[run : fn #fx{Eff}]` reads as the `fn` type token followed by a separate
`F_MAP` effect row. Both parts landed (gate skip + effect-soundness plumbing:
`CtorField.effect_row`, record-parser attach, `effect_check` reads it off
`adt_ctor` when the `StructDef` is absent). New negative fixture
`errors/struct-lowered-capability-effect-leak` (a `#fx{}` fn calling the lowered
capability field is rejected TUR-E0009) locks the soundness in. Suite green
(1875/0). The original description of the two parts, for the record:

1. *Gate.* The gate walker (`defstruct_fields_all_primitive`) treats the
   effect-row `F_MAP` after a `fn`/`c-fn` type as a stray non-`F_SYM` field
   name and bails. It must skip an optional trailing `F_MAP` after a `fn` type,
   mirroring the struct-path parser (`elab_structs.c:1582`).
2. *Effect soundness (the load-bearing part).* This is NOT just a gate tweak.
   `effect_check.c:161` reads `def->fields[fidx].effect_row` off the
   **`StructDef`** when a `.field` access returns a fn field, and merges that
   row into the current effect (calling the stored fn performs the effect). The
   lowered record ADT has no `StructDef`, so unless the effect row is carried
   onto the ADT field, effect tracking is **silently dropped** -- unsound. So
   A1 must:
   - add `EffectRow *effect_row` to `CtorField` (`types.h:196`);
   - have the record-ADT / `defdata` field parser read the `#fx{...}` map after
     a `fn` field into `CtorField.effect_row` (mirroring `elab_structs.c:1596`);
   - make the lowered-record `.field` access carry that row to the
     `EX_GET_FIELD`-equivalent node (or have `effect_check` look it up via the
     `AdtDef`/`CtorField` when the `StructDef` is absent);
   - extend `effect_check.c:161` to read the row from the ADT field.
   The four fixtures above are positive tests, so the gate-only change would
   make them *pass* while dropping effect tracking -- do NOT ship the gate skip
   without the `CtorField.effect_row` plumbing, or effect checking regresses
   silently. Add a negative fixture (a `.run` call in a context that forbids the
   effect must still error) to lock the soundness in.

**DS-A3 -- handle the list-form built-in compound field types. DONE
(2026-06-30, option (b): lower them).**  The sole remaining `StructDef`
producer was a `defstruct` field written as an F_LIST built-in compound type;
only `(lref int)` occurred (one negative fixture). Chosen approach: **lower the
borrow family onto the record-ADT path.**
  - `struct_field_type_from_form` now routes `(lref T)`/`(borrow-mut T)` to the
    real type elaborator (TY_LREF/TY_REF_MUT), as it already did for
    fn/arrow/forall/exists; `(& T)` routes via the existing has_amp path.
  - `defstruct_field_type_lowerable` drops lref/&/borrow-mut from the reject
    list.
  - the ADT `:copy` check reproduces the struct path's precise `TUR-E0102`
    "cannot copy linear field" diagnostic, so the lowering is
    diagnostic-transparent (the Bad fixture's `expected.diag` is unchanged).

  *Update -- DS-A4 (2026-06-30):* the remaining exotic forms (`forall`,
  `handler`, `arrow`, session/role/global/project) are now **rejected** as
  struct/ADT fields (a clean diagnostic in `struct_field_type_from_form`), so
  every `defstruct` takes the record-ADT path and the residual `StructDef`
  producer path is **unreachable** -- not merely unused. This is the true
  zero-producer precondition for DS-C/DS-D. Lowering those forms so they are
  supported as fields again is tracked separately in
  [structdef-exotic-field-forms-plan.md](structdef-exotic-field-forms-plan.md)
  (independent of, and after, the deletion). New negative fixture:
  `errors/defstruct-exotic-compound-field-rejected`.

**DS-B -- prove zero producers. DONE (2026-06-30).**  `assert(0)` installed in
`elab_register_struct_def` (single writer of `e->struct_defs[]`), with the
registration kept for NDEBUG release safety. The full by-value suite is **green
(1875/0) with the assert live** -- proving no `defstruct` produces a `StructDef`
across every fixture. Combined with B4 (no def-less `TY_STRUCT`), **the
`TY_STRUCT` kind is now uninstantiated**; DS-C/DS-D are unblocked and the assert
ratchets against regressions while they land.

> **Nearly there, but NOT zero yet -- DS-B found a third residual
> (2026-06-30).**  After A1+A2, a *hard-assert* DS-B guard in
> `elab_register_struct_def` (stronger than the earlier fprintf probe, which
> under-counted the error-path fixtures) fired for exactly **one** fixture:
> `errors/defstruct-copy-noncopy-compound-field`, `(defstruct Bad :copy
> [r (lref int)])`. So the built-in-compound-field category the plan flagged
> ("(lref T)/(& T)/forall/handler/...") is NOT empty in the suite after all --
> the plan's "None appear in the fixture suite today" was wrong.
>
> The residual is very narrow:
> - **Keyword/leaf forms lower.**  `[ptr : lref<int>]` (linear-lref-struct-field),
>   `[handler : fn]`, etc. all lower fine -- only Bad tripped the assert.
> - **Only the LIST form `(lref int)` does not.**  It reaches
>   `defstruct_field_type_lowerable`'s F_LIST arm, which keeps the built-in
>   compound forms on the struct path; naive lowering mis-parses `(lref int)`
>   as a type application (`TUR-E0012 cannot apply a type of kind '*'`), so the
>   ADT field parser would need per-form support.
> - It appears in exactly **one negative fixture** (testing the "cannot copy
>   linear field" diagnostic, `TUR-E0102`).
>
> **Resolved (2026-06-30):** DS-A3 lowered the borrow family (option (b)) and
> DS-B's hard assert is now in tree and green -- zero producers reached. See the
> DS-A3 / DS-B entries below.

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
