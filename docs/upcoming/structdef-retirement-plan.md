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
5. **Delete `StructDef` -- the bulk, separately scoped. IN PROGRESS.**

   **Step 1 -- migrate `defopaque` off `StructDef`. DONE (2026-06-29).**  The
   slice-5 footprint below missed a *second, live* `StructDef` producer:
   `defopaque`.  Every `(defopaque ...)` allocated a `StructDef` with
   `is_opaque=true` (58 fixtures + 31 stdlib files), so `StructDef` could not be
   deleted without first re-homing opaque newtypes.  An opaque newtype is now an
   **opaque `AdtDef`** (`n_ctors == 0`, `is_opaque=true`) carried on `TY_ADT` --
   reusing the ubiquitous ADT kind so typeclass dispatch, kind inference,
   `type_eq`, mangling and the REPL needed no changes (a 0-ctor ADT is already
   steered to the int64 carrier by `adt_is_byvalue_product`).  Ported the opaque
   short-circuits onto the ADT path: `propagate_app_discipline`,
   `type_app_is_concrete_adt`, `type_uses_carrier_abi` (opaque ADT is a plain
   int64, not a carrier aggregate -- mirrors the old non-parametric opaque-struct
   rule), `ascribe_to_opaque`, the ADT typedef + ADT-app forward-decl emitters
   (skip opaque), a new `def_is_opaque_type_decl` guard at the four
   global-emission sites (the opaque type-decl `EX_DEF` no longer carries a
   `struct_def`), `type_phantom_hides_aggregate` + `sk_find_serializable` (CPS),
   and the `exists`-open phantom-app projection (keep the applied form for an
   opaque ADT head so the bound index survives per-open skolem renaming).  Suite
   green (1874/0).  `StructDef.is_opaque` now has **zero producers**; the ~40
   `is_opaque`-on-`StructDef` reads are dead and get removed with the type.

   **Remaining steps (the deletion proper) -- two more plan-inaccuracies found:**
   - *The residual `defstruct` `StructDef` path is NOT unused* (the claim below
     is wrong).  An instrumented full-suite run shows **6 fixtures** still take
     it, across three distinct field categories that the record-ADT path does not
     yet host: (a) **effect-annotated `fn` fields** -- `run : fn #fx{Write}`
     (Action / App / Emitter / Printer); `CtorField` carries no `effect_row`, so
     lowering needs `effect_check` to read it from the lowered field; (b)
     **grouped field specs** -- `[a : int [b : int]]` (Mixed); the gate
     re-derives from the raw form and bails on the nested `F_VEC` before
     `elab_defstruct` flattens it; (c) **applied/`Dense` fields** -- World
     (`(Dense m A)`).  Each must lower (or be explicitly rejected) before the
     residual path can be removed.
   - *`TY_STRUCT{def=NULL}` is load-bearing as the tyvar / unknown-type /
     opaque-type-constructor-argument placeholder* across `elab_types.c`,
     `elab_fns.c`, `elab_typeclasses.c`, `kind_check.c`, `typeclass.c`.  Deleting
     the `TY_STRUCT` kind therefore also requires migrating every def-less
     placeholder to `TY_TYVAR` -- a sizeable sub-project of its own, not captured
     by the "198 refs" count, and the riskiest remaining piece (it changes the
     type-variable representation the unifier and kind checker key on).

   Original footprint (still accurate for the `StructDef`-identity refs):
   With slices 1-4 done the gate lowers every `defstruct` whose fields are
   scalars / pointers / `fn` / aggregates / parametric / applied / `exists`, and
   every annotation (`:copy`/`:move`/`:heap`/`:linear`/`:no-auto-ctor`).  The only
   shapes still on the `StructDef` path carry a *built-in compound* field form --
   `(lref T)`, `(& T)`/borrow, `forall`, `handler`/`arrow`/session/role/global/
   project -- which are their own `TypeKind`s, not user type applications.  **None
   appear in stdlib or the fixture suite today**, so the gate is effectively
   always-true in practice; a clean deletion will still want those field forms
   either hosted on the record-ADT path or explicitly rejected first.
   Footprint as of 2026-06-29: **198 references across 19 files** under
   `src/compiler/`.  This phase is all-or-nothing (you cannot half-delete a type)
   and does NOT decompose into the safely-committable increments slices 1-4 did,
   so it remains its own dedicated work.  Original breakdown:
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

## Slice 5 de-risking (prep work, separately committable)

Before attempting the all-or-nothing deletion, the following can land as
independent commits in the slices 1-4 cadence.  Each shrinks the deletion's
blast radius or proves a precondition.

**A. Close the 6-fixture residual `defstruct` -> `StructDef` path.**  Goal: get
the residual `StructDef` producer count to zero fixtures, so the gate is
provably dead before the type goes away.
  - A1. **Effect-annotated `fn` fields** (Action/App/Emitter/Printer --
    `run : fn #fx{Write}`).  Add `effect_row` to `CtorField` (or stash it
    alongside the field type); teach `effect_check` to read the row off the
    lowered ADT field rather than the `StructDef` field.  Lowest-risk -- purely
    additive metadata.
  - A2. **Grouped field specs** (`[a : int [b : int]]` -- Mixed fixture).
    `defstruct_lowers_to_adt` re-derives from the raw form and bails on the
    nested `F_VEC`.  Hoist the gate after `elab_defstruct` has flattened the
    spec (or run the flattener inside the gate).  Pure gate-ordering fix.
  - A3. **Applied `(Dense m A)` field on World.**  Slice 1 already handles
    `TY_APP`, so the question is *why* World still trips the gate -- likely a
    kind/sized interaction (`Dense`'s `m` size param).  ~30-min investigation
    commit first; may already be one-line.

**B. Migrate `TY_STRUCT{def=NULL}` placeholders to `TY_TYVAR`.**  The single
biggest risk reduction.  Lands entirely before slice 5 because it does not
require deleting `TY_STRUCT` -- only redirecting the def-less producers.
  - B1. Inventory pass: grep every site that creates `TY_STRUCT` with
    `def=NULL` (or reads `as.struct_.def` and tolerates NULL).  Land the
    inventory as a comment block / short doc.  **DONE (2026-06-28):**
    [ty-struct-null-def-inventory.md](../artifacts/ty-struct-null-def-inventory.md) -- 3
    producers (P1 `elab_fns.c:2437`, P2 `elab_types.c:2321`, P3
    `elab_typeclasses.c:2266`) and 16 explicit NULL-tolerant consumers
    across elaboration + codegen.  Direction-A precedent
    ([open-binder-skolems...](../archive/history/open-binder-skolems-not-distinguishable.md))
    already migrated one parse-time producer, so P1-P3 follow the same
    `type_tyvar_named(name)` pattern and the consumer shims it left
    behind become the B3 work-list.
  - B2. Per-producer migration, one commit each: tyvar placeholders ->
    `make_tyvar(...)`; unknown-type placeholders -> existing "unknown" kind
    (or new `TY_UNKNOWN`); opaque-type-constructor-argument placeholders ->
    likely `TY_TYVAR` with a bound name.
  - B3. Per-consumer migration: each NULL-tolerant read site switches to the
    new kind.  Unifier and kind-checker changes go in their own commits with
    focused fixtures.
  - B4. Land a `TUR_ASSERT(def != NULL)` on `as.struct_.def` reads once all
    producers are converted -- this *proves* the migration is complete before
    slice 5 starts.

**C. Pre-factor the typeclass dispatch site.**  Introduce a
`type_head_key(Type *)` helper returning a stable identity for instance lookup;
the current implementation just calls through to `StructDef*` for structs.
Port every instance-lookup site in `elab_typeclasses.c` (25 refs) to use the
helper.  When `StructDef` goes away the helper switches to `AdtDef*` and only
one file changes.

**D. Pre-factor `Expr` carriers off `StructDef*`.**  `expr.h` has 8 refs
(`make_struct_.def`, `set_field_.def`, carrier `box_struct`/`target_struct`).
A typedef alias (`typedef StructDef TypeHead;` today, `typedef AdtDef TypeHead;`
after) lets every Expr field rename land mechanically in slice 5 instead of
mixing with semantic changes.

**E. Snapshot the StructDef footprint as a regression guard.**  Tiny test that
greps `src/compiler/` for `StructDef` references and asserts the count stays at
or below the current 198.  New uses creep in during long refactors; a numeric
ratchet catches them.

Recommended order: B first (highest leverage, most independent), then A in
parallel, then C and D as small mechanical commits, then E as a guard.  With B
done the slice-5 deletion shrinks from "rewrite type identity + migrate tyvar
representation + delete the type + chase 6 residual fixtures" to roughly
"rename the kind and delete the dead code."

### Slice 5 status, 2026-06-29 (post-B1; baseline-mangling unblocker DONE)

- **B1 DONE** -- inventory landed
  ([ty-struct-null-def-inventory.md](../artifacts/ty-struct-null-def-inventory.md)).
  3 producers (P1 `elab_fns.c:2437`, P2 `elab_types.c:2321`,
  P3 `elab_typeclasses.c:2266`) and 16 explicit NULL-tolerant consumers.
- **Baseline-mangling unblocker DONE (2026-06-29)** -- the suite gate is
  honest again: **`bash tests/run.sh` is green (1874/0)**.  The fix landed in
  `append_type_mangle` (`src/compiler/types.c`).  The defect was a *name
  collision*, not a missing definition: `TY_FN` (a `void *` by-value handle),
  `TY_TYVAR`, and def-less `TY_STRUCT` all mangled to `"opaque"`, so
  `(Option <fn>)` and `(Option <placeholder>)` collided on
  `tur_adt_Option__opaque` with different ABIs (`void *` by-value vs int64
  boxed) under one include guard.  The interim patch that routed def-less
  `TY_STRUCT` to `"opaque"` had made it worse by merging the placeholder onto
  `TY_FN`'s token.  The fix restores the convention the committed snapshots
  already encoded (`Option__opaque` = always `void *`/TY_FN;
  `Option__struct` = always int64/placeholder): def-less `TY_STRUCT` mangles to
  `"struct"`, `TY_TYVAR`/`TY_UNKNOWN` are split out of the `default:` arm onto
  the **same** `"struct"` token (so def-emitter and call site agree on the
  placeholder name), and `TY_FN` stays `"opaque"`.  Snapshots regenerated in
  the same change.  Full write-up:
  [docs/archive/baseline-ctor-option-struct-mangling.md](../archive/baseline-ctor-option-struct-mangling.md).
- **B2 mostly DONE (2026-06-30): 6 of 8 producers migrated to named
  `TY_TYVAR`; 2 deferred.**  Landed as five commits, each with the by-value
  suite green (1874/0).  A `struct_.def = NULL` sweep during the work showed
  B1's "3 producers" count was an undercount -- there are **8**.  Migrated:
  P1 (`elab_fns.c` single-occurrence param), P2 (`elab_types.c` unknown
  type-app arg), P3 (`elab_typeclasses.c` unknown instance type-arg -- the
  typeclass-dispatch-entangled one, which also carried the `type_effective_kind`
  / `opaque_struct_arg` kind-check consumers and the `build_inst_type_suffix`
  + two dict-name codegen mirrors), P4 (`elab_types.c` `^f`/`^^f` HKT
  type-param ref), and P5/P6 (`elab_typeclasses.c` partial-app constructor
  heads).  **Deferred (P7/P8):** the general unknown-type-name fallback in
  `type_expr_from_form` is load-bearing -- built-in types with no `defstruct`
  (`str`, ...) route through it and `type_effective_kind` reads it as an opaque
  `KIND_ARROW` constructor.  A naive migration changed codegen across 93
  fixtures (built + reverted); it needs the tyvar-kind question settled and
  belongs to the slice-5 deletion, not incremental B2.  See the corrected
  producer table + consumer list in
  [ty-struct-null-def-inventory.md](ty-struct-null-def-inventory.md).
- **B4 remains blocked** -- installing `TUR_ASSERT(def != NULL)` now would
  fire on the two deferred P7/P8 fallback producers.  (Note: a migrated
  `TY_TYVAR` placeholder and a residual def-less `TY_STRUCT` placeholder still
  mangle identically -- both to `"struct"` -- so the partial migration does not
  risk a call/def name mismatch.)
- **Separate, low-priority follow-up found while fixing this**: the multi-file
  split path (`tur build <dir>` / `emit_header`) drops four ADT monomorph
  typedefs from `input.h` (incl. the fully-concrete `Endo__int`/`Schema__int`),
  unrelated to the mangler and not affecting the suite gate (the suite builds
  single-file via `emit_program`).  See
  [docs/reported/split-path-missing-adt-monomorph-typedefs.md](../reported/split-path-missing-adt-monomorph-typedefs.md).

The slice-5 deletion itself remains gated on B; A/C/D/E are not affected.

## Recommendation

**Status (2026-06-29): slices 1-4 DONE; slice 5 step 1 (defopaque migration)
DONE; slice 5 deletion proper is the remaining work.**  Slices 1-4 landed as four
separate commits.  Slice 5 step 1 (migrate `defopaque` off `StructDef`) landed as
a fifth, each with a green by-value suite (1874/0).

`defopaque` was a prerequisite the original slice-5 scope missed entirely -- it
is now migrated to an opaque `AdtDef`, so `StructDef` has **zero opaque
producers** left.  The remaining producer is the residual `defstruct` path, which
-- contrary to the original plan -- is still used by 6 fixtures (effect-`fn`,
grouped-spec, and applied-field categories; see slice 5 above).

The remaining deletion is larger and riskier than the original "198 refs / 19
files" estimate because of two findings made during step 1: (a) the residual
`defstruct` path must first host (or reject) three field-form categories before
it can be removed, and (b) `TY_STRUCT{def=NULL}` is the elaborator's
tyvar/unknown-type placeholder, so deleting the `TY_STRUCT` kind also means
migrating every def-less placeholder to `TY_TYVAR`.  It remains the large,
all-or-nothing phase: instance selection keys on `StructDef` identity, so it
ripples into typeclass resolution + codegen.  None of this is urgent: the
lowering is correct and complete for real code today (the residual path still
compiles the 6 structs correctly), so the deletion stays sequenced behind
anything that fixes real defects.
