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

**Slice 1 (this change): leaf-scalar structs only.** A `defstruct` is lowered
only when *all* of:

- old-style single-vector field syntax,
- non-parametric (no type-param vector),
- not `:heap`, not `:linear`,
- **every field a primitive scalar** (`int`/`float`/`bool`/`cstr`/sized
  ints+floats) -- no `rc`/`ref`/`weak`/`ptr`/`fn`/struct/ADT fields.

Anything else still elaborates as a struct, even with the flag on. This subset
hits **none** of the record-ADT gaps below, so the lowered program is
behaviourally identical (verified by a flag-on fixture).

## Known gaps (later slices, before graduation)

A record ADT does not yet cover these struct-only behaviours; each must land
before the gate widens past leaf-scalar and before the experiment graduates
(deletes the gate, makes lowering always-on, removes the `StructDef` path):

- **`rc<Struct>` field access auto-deref** -- `Type.as.rc.struct_def` carries a
  `StructDef*`; there is no `rc.adt_def` equivalent, so `(.f rc-val)` over a
  lowered struct would not resolve.
- **Large-struct pass-by-pointer** (`type_struct_pass_by_ptr`, >16 bytes) -- ADTs
  have no size-gated calling convention (a representation/ABI change, not a
  correctness break, but must be reconciled before snapshots converge).
- **`:heap` typed-pointer ABI** -- no ADT equivalent of the parametric
  `Vec<T> *` lowering.
- **Nested by-value struct fields** -- a struct-typed field is inlined in a
  struct but carried as an int64 in an ADT.
- **Drop-glue for by-value ADTs with `rc`/`weak` fields** -- the struct path
  emits drop-glue; the ADT path does not yet (CONV-S1 "Additional sites").

## Sequencing

1. **Slice 1 (now)**: leaf-scalar lowering behind the flag; flag-off no-op,
   flag-on fixture proves behavioural parity. *(this change)*
2. rc<ADT> field-access + drop-glue for by-value ADTs.
3. pass-by-ptr / large-aggregate ABI reconciliation; nested by-value fields.
4. Widen the gate to all non-parametric record structs; converge codegen.
5. Graduate: delete the gate, lower unconditionally, retire the `StructDef`
   surface path, regenerate the affected snapshots in one change.

See [struct-adt-convergence-s1-bridging-findings.md](struct-adt-convergence-s1-bridging-findings.md)
for the by-value representation work this builds on.
