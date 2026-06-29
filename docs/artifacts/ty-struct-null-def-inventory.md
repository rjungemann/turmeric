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

## Producers (3)

| # | Site | Purpose | Migration target |
|---|------|---------|------------------|
| P1 | `elab_fns.c:2437-2440` | Single-occurrence `defn` type-param dropped to "unresolved" placeholder when only one position references it. | `type_tyvar_named` carrying the param name (same as the open-binder fix). |
| P2 | `elab_types.c:2321-2323` | Type-app argument that is an unknown name, e.g. `(Option Unknown)`. Comment: "opaque type constructor … TY_STRUCT with no def emits void* in codegen". | `type_tyvar_named(arg_sym->name)`. Codegen for an unresolved tyvar already lowers to `int64_t` (`types.c:3294-3295`), matching the documented "container values are int64_t-sized opaque handles" intent. |
| P3 | `elab_typeclasses.c:2266-2268` | Unknown type-arg name in `definstance`, same shape as P2. Tracks the symbol via `type_arg_syms[i]`. | `type_tyvar_named(kw->name)`. |

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

Three small, independently-committable commits, each with the suite green:

1. **P1**: `elab_fns.c` single-occurrence param → named tyvar.  Local to
   one elaboration helper; the only consumer touched is `elab_fns.c:82`
   (becomes "named tyvar?" check).
2. **P2**: `elab_types.c` unknown type-app arg → named tyvar.  Touches the
   EX2 phantom-binder consumers at `elab_types.c:2767/2835/2867` and the
   `elab_call.c:5363` accept-both shim.
3. **P3**: `elab_typeclasses.c` unknown instance type-arg → named tyvar.
   Drops the `type_args[i]` def=NULL shim at line 2575 and the abstract
   tyvar checks at 4611/5159/5728.

After all three:

4. **B4**: replace the remaining `as.struct_.def == NULL` checks with the
   named-tyvar equivalent (or delete dead branches), then install
   `TUR_ASSERT(t.as.struct_.def != NULL)` in `type_struct`-adjacent
   readers (`type_name`, `type_c_name`, etc.).  The assertion failing in
   the suite at this point would identify a missed producer.

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
