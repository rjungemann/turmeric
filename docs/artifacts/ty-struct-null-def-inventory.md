---
title: TY_STRUCT{def=NULL} producer/consumer inventory
category: Planning
description: Inventory pass for slice 5 de-risking task B1 of structdef-retirement-plan. Catalogs every site that creates a def-less TY_STRUCT placeholder or reads as.struct_.def tolerating NULL, so the B2/B3 per-site migration can proceed in safely-committable increments.
---

# TY_STRUCT{def=NULL} producer/consumer inventory

Per [structdef-retirement-plan.md](structdef-retirement-plan.md) slice-5
de-risking task **B1**.  Goal: enumerate every site that produces or consumes
the def-less `TY_STRUCT` placeholder so the per-producer/per-consumer
migrations (B2/B3) can land as small commits and B4 can install
`TUR_ASSERT(def != NULL)` once both sides are clear.

## Background

`TY_STRUCT{def=NULL}` is overloaded as the elaborator's "type variable /
unknown / opaque type-constructor argument" placeholder.  A partial migration
to named `TY_TYVAR` already landed for the open-binder skolem case (see
[docs/archive/history/open-binder-skolems-not-distinguishable.md](../archive/history/open-binder-skolems-not-distinguishable.md)
"Direction A", 2026-06-12): the parse-time site at `elab_types.c:451` now
emits `type_tyvar_named(sym->name)` instead of an anonymous struct.  Several
consumer sites grew "prereq shims" that accept both shapes during the
transition; the remaining producers are what slice-5 needs cleared.

## Producers

> **B1 undercount corrected during B2 (2026-06-30).** The original inventory
> listed only 3 producers (P1-P3). An exhaustive `struct_.def = NULL` sweep of
> `src/compiler/` while executing B2 found **8** def-less `TY_STRUCT` producers.
> Six of the eight are now migrated to named `TY_TYVAR` (B2 DONE for them,
> suite green 1874/0 after each); the remaining two are the general
> unknown-type-name fallback, deferred to slice 5 -- see below.

| # | Site | Purpose | Migration target | Status |
|---|------|---------|------------------|--------|
| P1 | `elab_fns.c` demote-lone-param pass | Single-occurrence `defn` type-param dropped to "unresolved" placeholder when only one position references it. | `type_tyvar_named` carrying the param name (same as the open-binder fix). | **DONE** |
| P2 | `elab_types.c` `elab_type_app` unknown arg | Type-app argument that is an unknown name, e.g. `(Option Unknown)`. | `type_tyvar_named(arg_sym->name)`. Codegen for an unresolved tyvar already lowers to `int64_t`, matching the documented "container values are int64_t-sized opaque handles" intent. | **DONE** |
| P3 | `elab_typeclasses.c` unknown instance type-arg | Unknown type-arg name in `definstance`, same shape as P2. Tracks the symbol via `type_arg_syms[i]`. | `type_tyvar_named(kw->name)`, marked `hkt_kind = KIND_ARROW` (opaque constructor). | **DONE** |
| P4 | `elab_types.c` `^f`/`^^f` HKT type-param ref | Higher-kinded type-param reference in a type position; kind carried in `hkt_kind`. Missed by B1. | `type_tyvar_named(type_params[idx]->name)`, `hkt_kind` preserved. | **DONE** |
| P5 | `elab_typeclasses.c` `(Ctor a b)` partial-app head | Unresolved fully-applied 2-param instance-head constructor. Missed by B1. | `type_tyvar_named(ctor_sym2->name)`, `hkt_kind = KIND_ARROW2`. | **DONE** |
| P6 | `elab_typeclasses.c` `(Ctor arg)` partial-app head | Unresolved partial-app instance-head constructor. Missed by B1. | `type_tyvar_named(ctor_sym->name)`, `hkt_kind = KIND_ARROW2`. | **DONE** |
| P7 | `elab_types.c` unknown bare type name fallback (`type_expr_from_form`) | General "unknown bare type name" fallback in the type-expr elaborator. | `type_tyvar_named(sym->name)`. Codegen effect: a container element that lands here (bare `none`, unresolved element) stays polymorphic (uniform-carrier `ctor_Option`) instead of minting a placeholder `Option__struct` monomorph -- 92 snapshots regenerated, verified uniform by a global diff scan. | **DONE** |
| P8 | `elab_types.c` unknown keyword type name fallback (`type_expr_from_form`) | Keyword-name analog of P7. | `type_tyvar_named(sym->name)`. | **DONE** |

> **All 8 producers migrated (2026-06-30).** `src/compiler/` now has **zero**
> `struct_.def = NULL` producers and no live `type_from_kind(TY_STRUCT)` calls;
> every remaining `kind = TY_STRUCT` site assigns a non-NULL `def`. The one
> build failure P7/P8 originally exposed
> (`conv-defstruct-result-struct-field-typedef-order`) was fixed first by the
> `EX_DEFAULT_OF` boxing guard, so P7/P8 landed as a pure
> element-lowering + snapshot regen. **B4 (the `def != NULL` assertion) is now
> unblocked.**

### Consumer updates B1 did not anticipate (landed with B2)

B1 listed the NULL-tolerant consumers it knew about; executing B2 surfaced
several more that had to move in lockstep:

- `elab_core.c binding_has_suspicious_param_annotation` -- accepts the named
  `TY_TYVAR` param placeholder (P1) in addition to the def-less `TY_STRUCT`.
- `kind_check.c type_effective_kind` -- routes `TY_TYVAR` through `hkt_kind`
  (a kind-`*` tyvar still reports `*`; a placeholder marked `KIND_ARROW`/`ARROW2`
  reports its arrow kind, mirroring the old def-less `TY_STRUCT` behaviour).
- `kind_check.c` `opaque_struct_arg` skip-promotion guard -- also treats an
  arrow-kinded `TY_TYVAR` instance head as ambiguous (P3).
- `build_inst_type_suffix` (elab) + the two dict-name mirrors
  (`emit_stmt.c`, `emit_core.c`) -- gained a `TY_TYVAR` arm that mangles by the
  tracked symbol name; without it every unknown-name instance collapsed to the
  `_T` suffix / `dict_<C>_T` struct (idempotent-guard swallow + ODR collision).

The construction at `elab_typeclasses.c:5195` (`base.kind = TY_STRUCT;
base.as.struct_.def = rc_struct_def;`) is **not** a def=NULL producer -- it
sets `def` from a non-NULL `rc.struct_def` for an rc-receiver auto-deref.

`type_from_kind(TY_STRUCT)` (zero-initialises the struct payload, so `def`
defaults to NULL) appears only in comments warning that callers must patch
the result -- there are no live `type_from_kind(TY_STRUCT)` call sites, only
`type_from_kind(TY_UNKNOWN)` ones.

## Consumers tolerating `def==NULL` (16 explicit NULL checks)

Grouped by file.  Each treats a def-less TY_STRUCT as "this is a tyvar /
unknown / opaque container head" rather than a real nominal struct.

### Elaboration

- `elab_call.c:5363` -- accepts both legacy anonymous TY_STRUCT{def=NULL}
  and named TY_TYVAR as the "abstract container head" for result-type
  unification.
- `elab_fns.c:82` -- treats a def-less TY_STRUCT param as an unresolved
  type variable for inference threading.
- `elab_types.c:1118` -- nameless TY_STRUCT(def=NULL) → re-promote to a
  named TY_TYVAR by matching against the surrounding type-param list.
- `elab_types.c:1186` -- same promotion for a return-type position.
- `elab_types.c:1335` -- type-variable detection in a fn-type's arg slot.
- `elab_types.c:1362` -- type-variable detection in a fn-type's return slot.
- `elab_types.c:2767` -- EX2 phantom-binder skolem matching: TY_APP arg
  being a def-less TY_STRUCT is treated as the bound tyvar.
- `elab_types.c:2835` -- existential body substitution: tyvars (both
  TY_STRUCT{def=NULL} and TY_TYVAR) resolve to TY_INT at codegen.
- `elab_types.c:2867` -- same body-substitution, value-position branch.
- `elab_typeclasses.c:2575` -- accepts both shapes when matching instance
  type-args against a class type variable.
- `elab_typeclasses.c:4611` -- abstract-tyvar bound check during instance
  resolution.
- `elab_typeclasses.c:5159` -- result-type tyvar check during dispatch.
- `elab_typeclasses.c:5728` -- "abstract tyvar" receiver detection at
  method-call dispatch.

### Codegen

- `emit_expr.c:6999` -- treats a def-less TY_STRUCT as the int64 carrier in
  a make-struct / call output.
- `emit_expr.c:7508` -- same in a parameter-type lowering branch.
- `emit_module.c:442` -- def-less TY_STRUCT is a tyvar/unknown for the
  module emitter's "skip nominal forward-decl" predicate.
- `types.c:3268` -- `type_c_name` returns `"int64_t"` for a def-less
  TY_STRUCT (same arm covers `is_opaque`).
- `types.c:2047` -- `type_name` returns `"<struct>"` when def is NULL
  (printer fallback only).

Plus the dozens of "`as.struct_.def &&`" guards in `elab_call.c`,
`emit_expr.c`, `elab_typeclasses.c`, `elab_memory.c`, `emit_cps.c`,
`elab_types.c`, `elab_fns.c` -- these short-circuit when def is NULL
without explicitly naming the placeholder case.  They become unconditional
once the producers are gone (B4 installs the assertion to prove it).

## Migration order for B2/B3

**B2 status (2026-06-30): ALL 8 producers migrated (P1-P8).** B4 unblocked.
Landed as seven commits, each with the by-value suite green (1874/0):

1. **P1**: `elab_fns.c` single-occurrence param → named tyvar. Consumer
   touched: `elab_core.c binding_has_suspicious_param_annotation`.
2. **P2**: `elab_types.c` unknown type-app arg → named tyvar.
3. **P3**: `elab_typeclasses.c` unknown instance type-arg → named tyvar. The
   entangled one -- carried the `type_effective_kind` / `opaque_struct_arg`
   kind-check consumers and the `build_inst_type_suffix` + two dict-name
   codegen mirrors (see "Consumer updates" above).
4. **P4**: `elab_types.c` `^f`/`^^f` HKT type-param ref → named tyvar.
5. **P5/P6**: `elab_typeclasses.c` partial-app constructor heads → named
   tyvar (one commit).
6. **EX_DEFAULT_OF boxing guard** (`emit_expr.c`): a prerequisite de-risking
   fix -- see below -- that removed the one hard build failure from P7/P8's
   blast radius before the migration landed.
7. **P7/P8**: `elab_types.c` general unknown-type-name fallback (bare + keyword)
   → named tyvar, **+ 92 regenerated snapshots** in the same commit.

**P7/P8 -- the once-deferred, now-landed pair:**

  These were the general unknown-type-name fallback in `type_expr_from_form`.
  The concern was blast radius: naively migrating changed codegen across 93
  fixtures. The investigation resolved it.

  **Root cause of the codegen delta (measured 2026-06-30, via a
  build-and-revert probe on `defn-basic`).** When an Option/Result element
  type comes from this fallback, the def-less `TY_STRUCT` element is treated
  as a *concrete opaque type* and the container is **monomorphised** -- e.g.
  `None` emits `ctor_Option__struct` with a minted `tur_adt_Option__struct`
  typedef (the `"struct"` mangle token). Migrating the element to a named
  `TY_TYVAR` makes it a *genuine type variable*, so the container stays
  **polymorphic** and `None` emits the uniform-carrier `ctor_Option` instead.
  The uniform-carrier form is arguably the *more correct* lowering (the minted
  `Option__struct` monomorph is a placeholder artifact), so most of the 93 are
  benign snapshot drift that a regen would absorb -- **but one was a genuine
  build failure**: `conv-defstruct-result-struct-field-typedef-order`
  (`incompatible types ... 'tur_adt_User' from 'tur_adt_User *'`).

  **That build failure is now FIXED (2026-06-30), independently of P7/P8.**
  Root cause: the Result/Option monomorph ctor loop *double-processed* the
  unused-variant slot -- the `EX_DEFAULT_OF` block materialised it as the
  pointer-box NULL `(T *){0}`, then the by-value boxing site re-wrapped it as
  `malloc + *tmp = (T *){0}` (storing a `T *` into a `T` slot). Guarding the
  boxing site with `arg->kind != EX_DEFAULT_OF` (`emit_expr.c`) passes the
  default directly; no-op on the current suite (the double-box only surfaces
  once the container element is a named tyvar). With that landed, P7/P8 came
  down to: (a) the polymorphic uniform-carrier lowering IS the intended one
  (the `Option__struct` monomorph was a def-less-element artifact -- every
  affected fixture still builds/runs/matches stdout, so the drift is pure
  cosmetics of the emitted C), and (b) a **92-snapshot regen** landed in the
  same commit. **P7/P8 DONE (2026-06-30); suite green (1874/0).**

Now unblocked:

- **B4**: replace the remaining `as.struct_.def == NULL` checks with the
  named-tyvar equivalent (or delete dead branches), then install
  `TUR_ASSERT(t.as.struct_.def != NULL)` in `type_struct`-adjacent
  readers (`type_name`, `type_c_name`, etc.).  The assertion failing in
  the suite at this point would identify a missed producer.  **Ready to
  attempt** -- all 8 producers are migrated, so the assert should hold; a
  firing assert would reveal a def-less `TY_STRUCT` arising from a path not
  in this inventory.

## Notes

- The named-TY_TYVAR migration is **not** a deletion of `TY_STRUCT` -- the
  kind keeps every named-defstruct/defopaque use.  Slice 5 still owns the
  type's removal; B1-B4 only remove the def=NULL overload.
- The codegen contract "def-less TY_STRUCT lowers to int64_t" is already
  matched by `TY_TYVAR`'s codegen arm (`types.c:3294-3295`), so the C
  output should not change for any program -- the migration is a typed-IR
  refactor.
- One named-tyvar predicate the migration needs (not yet a single helper):
  `is_unresolved_tyvar(Type)` returning true for both TY_TYVAR and the
  legacy TY_STRUCT{def=NULL} shape during the transition.  Introducing
  this helper in B2 step 1 and routing every consumer through it would
  collapse the per-shim edits to one site, then `TY_STRUCT{def=NULL}` can
  drop out of the helper in B4.
