---
title: defstruct -> defadt lowering
category: Planning
description: Lower `defstruct` to a single-variant record `defadt` so structs flow through the by-value ADT path, completing struct/ADT convergence. Gated behind the `defstruct-as-defadt` experiment; lands incrementally, leaf-scalar structs first.
---

# `defstruct` -> `defadt` lowering

## Goal

A `defstruct` is the single-variant, record-shaped case of an algebraic data
type. The CONV-S* work (S0/S2/S3/S4) already taught single-variant **record
ADTs** to support every struct surface operation -- field access `(.f v)`,
keyword construction `(P :x 1 :y 2)`, positional construction `(P 1 2)`, `match`
with by-name binding, and `with` functional update. CONV-S1/B1-B3 then gave a
leaf single-variant non-parametric ADT a **by-value** C representation
(`adt_is_byvalue_product`).

With both halves in place, a `defstruct` can be *lowered* to an equivalent
`(defdata Name (Name [fields]))` and reuse one code path for products. This
retires the parallel struct machinery and makes "struct" pure surface sugar.

## Mechanism: form-level desugar

The lowering is a syntactic rewrite performed in `elab_defstruct`
([`elab_structs.c`](../../src/compiler/elab_structs.c)):

```
(defstruct P [a : int b : int])      ==>   (defdata P (P [a : int b : int]))
(defstruct P :copy [a : int])        ==>   (defdata P :copy (P [a : int]))
```

The field-vector syntax is identical between a `defstruct` field list and a
record-variant field list (CONV-S0), so the rewrite is structural: wrap the
field vector in a `(Name <vec>)` constructor form, carry the `:copy` annotation,
and dispatch to `elab_defdata`. All AdtDef / constructor-binding / field-accessor
machinery is reused -- no hand-built `AdtDef`.

## Incremental gating (this is in-flight)

Behind the `defstruct-as-defadt` experiment (`--enable=defstruct-as-defadt`).
Default off: every `defstruct` still elaborates as a `StructDef`, so the suite is
byte-for-byte unchanged.

**Slices 1-4: scalar and by-value-aggregate structs.** A `defstruct` is lowered
when *all* of:

- old-style single-vector field syntax,
- non-parametric (no type-param vector),
- not `:heap`, not `:linear`,
- **every field a primitive scalar** (`int`/`float`/`bool`/`cstr`/sized
  ints+floats) **or a bare ADT-typed field** (slice 4 -- a by-value ADT field is
  inlined, a carrier ADT field is boxed, both correct).

Anything else (`rc`/`ref`/`weak`/`ptr`/`fn`/parametric, or a bare struct-typed
field) still elaborates as a struct, even with the flag on. A bare struct-typed
field is deliberately excluded: a struct's by-value-ness is not yet known when
the top-level type pre-pass pre-registers a sibling that nests it (the stub looks
trivially copyable), so admitting it could make the pre-pass and full
elaboration disagree on whether the outer lowers. With the flag on a leaf-scalar
nested struct has itself already lowered to an ADT, so the common nested case
still lowers; only a genuinely non-lowering struct field holds the outer on the
struct path. The lowered program is behaviourally identical (verified by flag-on
fixtures).

## Known gaps (later slices, before graduation)

A record ADT does not yet cover these struct-only behaviours; each must land
before the gate widens past leaf-scalar and before the experiment graduates
(deletes the gate, makes lowering always-on, removes the `StructDef` path):

- **`rc<Struct>` field access auto-deref** -- ~~`Type.as.rc.struct_def` carries
  a `StructDef*`; there is no `rc.adt_def` equivalent~~ **LANDED (slice 2).** A
  `Type.as.rc.adt_def` slot now mirrors `struct_def` (set by `type_rc_adt`, wired
  in `elab_rc_of` when wrapping a TY_ADT value); the dot-accessor in
  `elab_typeclasses.c` auto-derefs an `rc<ADT>` receiver to its record variant,
  and `EX_GET_FIELD` codegen reads the field through the rc-block's value pointer
  (by-value: direct cast; carrier: int64-carrier load). Fixture:
  `conv-rc-adt-field-access`.
- **Large-struct pass-by-pointer** (`type_struct_pass_by_ptr`, >16 bytes) --
  ~~ADTs have no size-gated calling convention~~ **LANDED (slice 3).** A by-value
  ADT product whose fields sum to >16 bytes now adopts the struct convention:
  `adt_byval_pass_by_ptr` (types.c) gates it, `type_struct_pass_by_ptr` returns
  true for the `TY_ADT` case, and the param is emitted as
  `const tur_adt_<Name> *`.  The three ADT-specific emit sites follow suit --
  field access reads through `->` for a pbp receiver, the `match` scrutinee binds
  as a pointer, and the call site takes the address (`&tmp`) / forwards a pbp
  param's pointer directly.  A lowered >16-byte leaf-scalar `defstruct` is now
  ABI-identical to the struct path (same `const T*`, same `&` call site). Fixture:
  `conv-byval-adt-large-pbp`.
- **`:heap` typed-pointer ABI** -- no ADT equivalent of the parametric
  `Vec<T> *` lowering.
- **Nested by-value struct fields** -- ~~a struct-typed field is inlined in a
  struct but carried as an int64 in an ADT~~ **LANDED (slice 4).** The
  representation gate `adt_is_byvalue_product` (types.c) now admits a field that
  is itself a by-value aggregate (a non-heap/non-opaque/drop-glue-free struct, or
  a by-value ADT product) and stores it **inline by value** -- the same flat
  layout a struct gives a nested struct field -- instead of boxing it behind the
  int64 carrier.  `adt_field_is_inline_byval` is the single representation gate
  shared by every site: the typedef + ctor emitters (`adt_ctor_field_c_type`,
  emit_module.c, both the whole-program and early-file paths), the ctor-arg
  store (which skips the B3 box for an inline field, emit_expr.c), `EX_GET_FIELD`
  (reads the aggregate with no cast / no deref), the `match` field-bind
  (binds the aggregate directly), and the pass-by-ptr size calc (sums an inline
  field's own bytes, types.c).  `resolve_ctor_field` (elab_structs.c) records the
  inline field's `full_type` so codegen can spell the aggregate.  A recursive HKT
  fixed-point (`Re`/`Expr`) is parametric and self-references through an
  `(ExprF Expr)` TY_APP field, so it stays on the carrier path -- B4's concern,
  not a prerequisite here.  Fixtures: `conv-byval-adt-nested-inline` (snapshot:
  inline layout, pbp ABI, by-value match), `conv-defstruct-nested-adt-lowering`
  (flag-on parity).
- **Drop-glue for by-value ADTs with `rc`/`weak` fields** -- ~~the struct path
  emits drop-glue; the ADT path does not yet~~ **LANDED (slice 2).** A by-value
  ADT (`adt_is_byvalue_product`) whose sole variant carries `rc`/`ref`/`weak`
  fields (`AdtDef.needs_drop_glue`) now emits `drop_glue_tur_adt_<Name>` /
  `walk_glue_tur_adt_<Name>` next to its typedef (shared
  `emit_adt_byval_drop_glue`, used by both `emit_adt_typedef_and_ctors` and the
  early-file mirror), and `EX_RC_OF` wraps such a value via `rc_cb_alloc_struct`
  with that glue -- so dropping the outer `rc` releases the inner owned fields,
  exactly as for a struct. Fixture: `conv-byval-adt-rc-drop`.

## Sequencing

1. **Slice 1 (now)**: leaf-scalar lowering behind the flag; flag-off no-op,
   flag-on fixture proves behavioural parity. *(this change)*
2. rc<ADT> field-access + drop-glue for by-value ADTs. **DONE** -- drop-glue
   (`conv-byval-adt-rc-drop`) and `rc<ADT>` field-access auto-deref
   (`conv-rc-adt-field-access`) both landed.
3. pass-by-ptr / large-aggregate ABI reconciliation; nested by-value fields.
   **pass-by-ptr DONE** (`conv-byval-adt-large-pbp`); nested by-value fields
   folded into slice 4 (needs the gate to admit aggregate fields).
4. Widen the gate to non-parametric record structs with nested by-value
   aggregate fields; converge codegen. **DONE** -- `adt_is_byvalue_product`
   admits by-value-aggregate fields and stores them inline by value, with codegen
   converged across the typedef/ctor/field-access/match/pass-by-ptr sites
   (`conv-byval-adt-nested-inline`); the `defstruct` lowering gate accepts bare
   ADT-typed fields (`conv-defstruct-nested-adt-lowering`). Recursive HKT
   fixed-points stay on the carrier path (B4, below). Bare struct-typed fields
   and `rc`/`ref`/`ptr`/`fn`/parametric fields remain for graduation.
   B4 (reconcile recursive/HKT carriers `Re`/`Expr` with the by-value path,
   per the s1-bridging findings) is **not** a prerequisite for lowering real
   structs. **Update (2026-06-25):** M7's by-value-HKT dispatch has graduated
   (default ON, `g_m7_hkt_enabled = true`), but a fresh post-graduation spike
   shows that does NOT unblock B4 -- the fat-closure-ABI change B4 needs was never
   in M7's scope and the gate-widening still produces the same 9 `cc` errors. B4
   is reframed as out-of-scope/moot for this lowering: a real `defstruct` cannot
   express a functor-applied-to-self field, so lowering never reaches the
   crossing. See the s1-bridging findings B4 section.
5. Graduate: delete the gate, lower unconditionally (including bare struct /
   pointer fields), retire the `StructDef` surface path, regenerate the affected
   snapshots in one change.

See [struct-adt-convergence-s1-bridging-findings.md](struct-adt-convergence-s1-bridging-findings.md)
for the by-value representation work this builds on.
