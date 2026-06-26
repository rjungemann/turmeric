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
  ints+floats), **a pointer field** (`rc<T>`/`ref<T>`/`lref<T>`/`weak<T>`/
  `ptr<void>` -- slice 5; an 8-byte carrier slot whatever the inner type, with
  drop glue for the owning `rc`/`ref`/`weak` ones per slice 2), **a bare `fn`
  field** (slice 6; an 8-byte carrier slot, ctor casts the function pointer to
  int64, capability calls dispatch through the record ADT), **or a bare
  ADT-typed field** (slice 4 -- a by-value ADT field is inlined, a carrier ADT
  field is boxed, both correct).

**Slice 5 (pointer-field widening).** The lowering gate
(`defstruct_fields_all_primitive`) now admits pointer-kinded fields. A pointer
field's representation does not depend on the (possibly not-yet-known) inner
type's by-value-ness, so the pre-pass / full-elaboration lowering decision never
disagrees on a `rc<Struct>` sibling the way it would on a bare struct field.
Slice 5 also closed the **`rc<ADT>` field-access** gap the lowering comment
named: a record-variant field annotated `rc<Inner>` (where `Inner` is an
in-scope struct or single-variant record ADT) now carries the inner layout on
its `full_type` (`adt_rc_inner_full_type` in
[`elab_structs.c`](../../src/compiler/elab_structs.c), the ADT analogue of DS3's
`lookup_rc_inner_struct_def`), so `(.f (.rcfield v))` auto-derefs through the rc
exactly as a `defstruct` `rc<Struct>` field does -- previously it raised "no
typeclass method found". This fix is flag-independent (it fixes hand-written
record ADTs too). The field still stores as the `TY_RC` carrier, so layout is
unchanged. Fixtures:
`conv-defstruct-pointer-field-lowering` (flag-on parity), `conv-rc-adt-record-field-deref`
(the closed `rc<ADT>` field-deref gap).

**Slice 6 (`fn`-field widening).** The gate now also admits a bare `fn` field.
A bare `fn` is an 8-byte carrier slot like a pointer field; two crossings were
wired for parity: (1) the by-value product ctor casts an `fn` argument to the
int64 carrier (`(int64_t)(intptr_t)`, the same coercion a struct literal does on
its `fn` field) so construction emits no `-Wint-conversion` note
([`emit_expr.c`](../../src/compiler/emit_expr.c)); (2) the **capability-field
call** `(.handler v arg ...)` now dispatches through a single-variant record ADT
-- the elaborator's capability-call path gained a `TY_ADT` branch mirroring the
`TY_STRUCT` one ([`elab_typeclasses.c`](../../src/compiler/elab_typeclasses.c)),
and the indirect-call codegen reads the field's kind / `full_type` from the
`CtorField` when the `EX_GET_FIELD` has a NULL `StructDef`
([`emit_expr.c`](../../src/compiler/emit_expr.c)). Like the `rc<ADT>` fix, (2) is
flag-independent: calling an `fn` field on any hand-written record ADT now works
(previously "no typeclass method found").
Fixtures: `conv-defstruct-fn-field-lowering` (flag-on parity),
`conv-adt-record-fn-field-call` (the closed record-ADT fn-capability-call gap).

**Slice 7 (typed-`fn`-field widening).** The gate now also admits a *typed* `fn`
field `(fn [..] ..)`. Though it is an F_LIST type form (which the leaf check
otherwise rejects), a typed `fn` is still an 8-byte function-pointer carrier slot
exactly like a bare `fn` -- its argument/return signature only feeds type
checking, never layout -- so it lowers like a scalar carrier. The one crossing:
the struct path stores a typed `fn` field as a *concrete* function pointer and
calls it directly, but a record-ADT field stores every `fn` (typed or bare) as
the int64 carrier, which is not directly callable. The capability-call codegen's
direct-call shortcut ([`emit_expr.c`](../../src/compiler/emit_expr.c)) is now
gated on a non-NULL `StructDef`, so a record-ADT typed-`fn` field falls through to
the same intptr_t-cast path bare `fn` already uses (the pointer type is
specialised from the call's arg/result C types). Like slice 6's fix this is
flag-independent: a typed-`fn` capability call on any hand-written record ADT now
works (previously it emitted a non-callable int64-carrier call and failed at
`cc`). Fixtures: `conv-defstruct-typed-fn-field-lowering` (flag-on parity),
`conv-adt-record-typed-fn-field-call` (the closed record-ADT typed-fn-call gap).

**Slice 8 (bare-struct-field widening + `make-struct` compatibility).** The gate
now admits a **bare (nullary) user-type field** -- a struct, an ADT, an opaque
newtype, or a forward-declared sibling.  The lowering decision is made
*syntactically* (any bare, non-primitive, non-pointer symbol is a user-type
reference), so it no longer resolves the field's binding kind and the top-level
type pre-pass and full elaboration always agree -- even when the field references
a sibling declared later in the file, whose stub is unresolved at pre-pass time.
This retires the old "a struct's by-value-ness is not yet known at pre-pass time"
hazard that kept struct fields on the struct path.

The field's *representation* is then chosen at codegen time from the fully
resolved inner type, and `resolve_ctor_field`
([`elab_structs.c`](../../src/compiler/elab_structs.c)) records the field's
nominal `full_type` for the inners the codegen handles end-to-end: a by-value
aggregate inner is stored INLINE (slice 4), and a `:heap` struct inner is an
int64 typed-pointer carrier (`type_is_heap_struct` guards every byval<->carrier
bridge, and the by-value/carrier ctor now casts a `:heap` argument to the int64
carrier with `(int64_t)(intptr_t)`, mirroring the slice-6 fn cast, so the ctor
call emits no `-Wint-conversion`).  A carrier ADT inner (multi-variant /
parametric / drop-glue) is deliberately left `full_type` NULL -- it stays an
opaque int64 carrier, exactly as the *struct* path erases an ADT field's type, so
the two paths agree (both reject reading it back typed) instead of the ADT path
silently miscompiling.

Slice 8 also closes a flag-on compatibility gap shared by every earlier slice:
**`make-struct` on a lowered struct.**  `(make-struct Name args...)` on a name
that has lowered to a single-variant record ADT now rewrites to the auto-bound
constructor call `(Name args...)` -- positional or keyword -- which the record-ADT
path already elaborates (`elab_make_struct`, elab_structs.c).  Like the other
record-ADT fixes this is flag-independent: `make-struct` on a hand-written
single-variant record ADT works too.  Fixtures:
`conv-defstruct-struct-field-lowering` (by-value + `:heap` struct fields, flag-on
parity), `conv-defstruct-make-struct-lowering` (positional + keyword make-struct
on a lowered struct).

Anything else (parametric or `:heap` *outer* structs, and the carrier-ADT /
opaque / drop-glue *field* cases described above) still keeps the struct path or
the type-erased carrier, even with the flag on.  The lowered program is
behaviourally identical for the admitted cases (verified by flag-on fixtures).

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
- **`:heap` typed-pointer ABI** -- ~~no ADT equivalent of the parametric
  `Vec<T> *` lowering~~ **FOUNDATION + NON-PARAMETRIC `:heap`-STRUCT LOWERING
  LANDED (seam 3, 2026-06-26).** A `:heap` record ADT lowers to a typed pointer
  `tur_adt_<Name> *`, and a non-parametric `:heap` *struct* now auto-lowers with
  full parity (`type_is_heap_adt` + `:heap` exclusions at pbp / carrier-ABI /
  `match`-scrutinee / ctor-arg-cast). Fixtures `conv-heap-adt-typed-pointer`,
  `conv-defstruct-heap-struct-lowering`. The **parametric** stdlib `:heap`
  containers (`Vec`/`Map`/`Set`/`MutableMap`) remain on the struct path.
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
5. Widen the gate to **pointer fields** (`rc`/`ref`/`lref`/`weak`/`ptr<void>`).
   **DONE** -- `defstruct_fields_all_primitive` admits pointer-kinded fields (an
   8-byte carrier slot whatever the inner type, so pre-pass / full-elab never
   disagree), and the `rc<ADT>` field-access gap is closed via
   `adt_rc_inner_full_type` (record-variant `rc<Inner>` fields carry the inner
   layout, so `(.f (.rcfield v))` auto-derefs -- the flag-independent fix also
   covers hand-written record ADTs). Fixtures:
   `conv-defstruct-pointer-field-lowering`, `conv-rc-adt-record-field-deref`.
6. Widen the gate to a **bare `fn` field**. **DONE** -- the by-value ctor casts
   the function pointer to the int64 carrier, and the capability-field call
   `(.handler v arg)` dispatches through a single-variant record ADT (a `TY_ADT`
   branch in the elaborator's capability-call path + CtorField-aware
   indirect-call codegen, both flag-independent so hand-written record ADTs gain
   fn-field calls too). Fixtures: `conv-defstruct-fn-field-lowering`,
   `conv-adt-record-fn-field-call`.
6a. Widen the gate to a **typed `fn` field** `(fn [..] ..)`. **DONE** -- an F_LIST
   type form, but representationally identical to a bare `fn` (an 8-byte
   function-pointer carrier), so it lowers like a scalar carrier. The
   capability-call direct-call shortcut is gated on a non-NULL `StructDef`, so a
   record-ADT typed-`fn` field uses the same intptr_t-cast path as a bare `fn`
   (flag-independent; hand-written record ADTs gain typed-fn calls too). Fixtures:
   `conv-defstruct-typed-fn-field-lowering`, `conv-adt-record-typed-fn-field-call`.
6b. Widen the gate to a **bare struct-typed field** (and any bare user-type
   field).  **DONE** -- the gate decision is now *syntactic* (any bare
   non-primitive symbol is admitted), so the pre-pass / full-elab agreement holds
   for forward-referenced sibling types and the old by-value-ness hazard is gone.
   `resolve_ctor_field` records a field's `full_type` for by-value-aggregate and
   `:heap` struct inners (the latter cast to the int64 carrier at the ctor call,
   no `-Wint-conversion`); a carrier ADT inner stays type-erased, matching the
   struct path.  This slice also closed **`make-struct` on a lowered struct**:
   `(make-struct Name args...)` rewrites to the constructor call `(Name args...)`
   (flag-independent; hand-written single-variant record ADTs gain make-struct
   too). Fixtures: `conv-defstruct-struct-field-lowering`,
   `conv-defstruct-make-struct-lowering`.
   Remaining for graduation: `:heap` *outer* structs and parametric fields.
7. Graduate: delete the gate, lower unconditionally (including parametric / `:heap`
   structs), retire the `StructDef` surface path, regenerate the affected
   snapshots in one change.

See [struct-adt-convergence-s1-bridging-findings.md](struct-adt-convergence-s1-bridging-findings.md)
for the by-value representation work this builds on.

## Graduation progress (2026-06-25)

Graduation (step 7) was attempted. The gate was widened to **parametric
(non-`:heap`) structs**, which surfaced that the entire autoloaded stdlib
(`Option`/`Result`/`Pair`/`Vec`/`Map`/`Set`/`List`/...) is parametric and must
work as record ADTs before the gate can come off. Three **flag-independent**
enabler fixes landed (they also fix hand-written parametric record ADTs); the
remaining work is a sequence of codegen seams, each substantial.

### Landed (flag-independent; suite 1833 passed, the 7 flag-on `conv-defstruct-*`
fixtures are the graduation canaries and stay red until the seams below close)

- **Parametric record-ADT field read.** The `EX_GET_FIELD` by-value gate
  (`emit_expr.c`, `case EX_GET_FIELD`) was app-aware-ified
  (`emit_type_is_byvalue_adt`) so a concrete monomorph (`(Box int)`, `TY_APP`)
  reads its field directly off the aggregate instead of the carrier-pointer
  deref (which cast an aggregate to a pointer -> `cc` error).
- **Parametric dot-accessor type.** The dot-accessor
  (`elab_typeclasses.c`, single-variant-record-ADT branch) now substitutes the
  receiver's concrete type args for a tyvar field type
  (`elab_adt_type_extract_args` + `adt_field_instantiate_type`, newly exported),
  so `(.val (Box 42))` types as `int` in untyped contexts (e.g. `println`).
- **Instance-head resolution (bare AND applied heads).** A `definstance` head --
  both the bare `[Option]` form and the applied `(Result _ B)` form
  (`elab_typeclasses.c`, the head-arg parse and the applied-ctor parse) -- now
  resolves through the **type-namespace** lookup (`elab_lookup_type_by_name`), so
  a single-variant record ADT whose constructor shares the type's name -- every
  lowered `defstruct`, and a hand-written `(defdata T [A] (T [...]))` -- resolves
  to the ADT *type*, not its ctor `fn` (which `scope_lookup`, being
  value-preferring, returned -> opaque `<struct>`).

With these three fixes the **entire autoloaded stdlib elaborates AND compiles
under the flag** (`Option`/`Result`/`Pair`/`Vec`/`Map`/`Set`/`List`/... all
lower to record ADTs and the trivial program links + runs).  What remains is a
runtime *usage* seam, below.

### Remaining seams before the gate can come off

1. **By-value-app -> carrier bridge at carrier-ABI call boundaries (NEXT,
   highest leverage, central seam).**  Stdlib *compiles*, but actual *usage*
   fails at `cc`: `(eq? (some 5) (some 5))` passes a by-value monomorph
   (`tur_adt_Option__int`, by-value via P2-P4) into a carrier-ABI consumer
   expecting `int64_t`.  This is not limited to typeclass dispatch -- the same
   value flows into generic fns and helper functions (observed:
   `mutmap_eq_loop(int64_t)`), so the bridge is needed wherever a by-value ADT-app
   crosses into a carrier-ABI parameter.  The gating predicates
   (`type_uses_carrier_abi`, and `expr_emits_byvalue_carrier_abi` /
   `type_uses_carrier_in_dispatch`, all `emit_expr.c`) report a by-value app as
   *non-carrier* and so suppress the existing `emit_carrier_bridge` box-and-address
   path.  A *non-parametric* by-value ADT works because its instance method is
   emitted by-value; only the *parametric* (uniform-carrier) consumers mismatch.
   Reconciliation options: (a) bridge by-value apps to the carrier at carrier-ABI
   arg boundaries (preserves by-value; broad but `emit_carrier_bridge` already
   exists); (b) emit per-monomorph instance methods; (c) keep lowered parametric
   structs on the carrier representation (simplest-green, gives up the by-value
   win for them).  Unblocks all parametric stdlib usage at once.

   **Refined finding (2026-06-25):** the failure is not only an ABI bridge -- it
   is also an instance-**selection** bug.  `(eq? (some 5) (some 5))` emits a call
   to `__inst_Eq_eq_qu_MutableMap__spec__..._tur_adt_Option__int_...` -- i.e. the
   `Eq [MutableMap]` instance body specialised with `Option__int` params (its
   body then calls `mutmap_len`/`mutmap_eq_loop` on an Option).  The dispatch
   resolver picked the WRONG instance for a by-value `Option__int` receiver
   (expected `Eq [Option]`).  So before/with the bridge, the by-value ADT-app
   receiver must resolve to the correct instance head -- the per-(args) spec
   matcher is keying on the carrier-erased ABI and colliding `Option__int` with
   the `MutableMap` instance.  This is the deep core of the remaining work and
   lives in the typeclass dispatch/spec-selection machinery
   (`find_matched_abi_spec` / instance resolution), not just `emit_carrier_bridge`.

   **Located (2026-06-25).** The mis-selection is in `elab_method_call`
   (`elab_typeclasses.c`): the `KIND_STAR` arm (~5604-5605) compares
   `inst->type_args[0].kind == obj->type.kind` with NO TY_APP-head normalization,
   so an ADT-app receiver `(Option int)` (kind `TY_APP`, head `TY_ADT`) fails to
   match the `TY_ADT`-headed `Eq [Option]` instance and instead matches the first
   `TY_APP`-kinded instance; the `KIND_ARROW` arm (~5644-5652) normalizes a TY_APP
   head only for `TY_STRUCT`, not `TY_ADT`. Both arms need the ADT-head
   normalization + `adt_def` discrimination that `typeclass_env_lookup_instance`
   (typeclass.c) just gained.  **But selection alone is insufficient:** the
   selected parametric instance method is uniform-carrier (`int64_t`) while the
   monomorph value is by-value, so the ABI bridge must land in the SAME change for
   `(eq? (some 5) (some 5))` to compile.  Deferred together -- modifying the hot
   selection path in isolation buys no end-to-end win and risks the green suite.

   **DONE (2026-06-25), consuming direction.** Both landed: (a) the selection
   discrimination now normalizes ADT-app receivers and rejects cross-constructor
   matches (`elab_method_call`, the KIND_ARROW arm) AND `typeclass_env_lookup
   _instance` (typeclass.c) gained the symmetric ADT-head discrimination; (b) the
   by-value-app -> carrier bridge fires at the dispatch arg (`emit_expr.c`, the
   per-arg coercion loop) -- a by-value `TY_APP` monomorph arg passed to a class-
   tyvar / bare-parametric-ADT param of an un-specialized instance method is boxed
   via `emit_carrier_bridge`.  `(eq? (some 5) (some 5))` and hand-written
   parametric record-ADT `Eq`/`Functor` instances now compile and run; suite
   stays 1840/0.  **Still open -- the PRODUCING direction (seam 1b below).**
1b. **By-value HKT construction inside a specialized instance body. DONE
   (2026-06-25) for Option.** `(fmap (some 5) f)`, `(bind (some 10) k)`,
   `(alt-or (none) (some 7))` now compile and run by-value with correct results.
   Five fixes: (i) `body_is_construct` recognizes a constructor-CALL body (a
   lowered struct's `make-struct` rewrites to the ctor call, so the body is no
   longer `EX_MAKE_STRUCT`); (ii) the family-recovery rehydration extracts an ADT
   app, not only a struct app, so a return-only-poly `(none)` gets `{A -> int}`;
   (iii) the three `construct_recovered_byvalue` gates accept a concrete ADT app
   (`type_app_is_concrete_adt`); (iv) the N-arg ctor suffix only trusts
   `type_adt_app_ctor_suffix` for a fully-concrete app and otherwise falls back to
   the active spec family, so the lowered ctor body (`(Option <erased> )`) emits
   `ctor_Option__int` in `none__spec`; (v) a concrete ADT-app spec PARAM is flagged
   `emit_byvalue_carrier_abi`, so the ACB carrier->concrete bridge no longer
   wrongly derefs a by-value `container`.  All flag-independent (helps hand-written
   record-ADT HKT instances too).  Suite stays 1840/0.  **Result is NOT yet done
   -- see seam 2.**
2. **`Result` construction codegen (mixed-type fields). DONE (2026-06-25) at
   baseline parity.** `(ok 42)`, `(err "boom")`, `ok?`, `ok-val`, `err-val` all
   compile and run under the flag (verified, matching the no-flag baseline).  Two
   fixes: (i) a `(default-of T)` arg to a by-value monomorph ctor with
   heterogeneous fields now emits the default in the FIELD's concrete C type
   (`(const char *){0}` for the `const char *` err slot of
   `ctor_Result__int__cstr`), recovered by substituting the active spec's concrete
   app args for the ctor field's tyvar; (ii) the by-value-app -> carrier bridge gate
   widened to `pk==TY_INT` and non-concrete `TY_APP` params, so a free generic fn
   whose `(Result A B)` param collapses to the int64 carrier (`ok?`) gets its
   by-value monomorph arg boxed.  Suite stays 1840/0.
   - *Not lowering bugs (pre-existing baseline limitations, reproduce WITHOUT the
     flag):* `fmap`/`bind`/`catch-error` on `Result` leave the result type
     under-determined (`(type-app ? ?)` / collapsed to `int`), so a following
     `ok-val` can't type it.  Out of scope for the lowering.
   - *Cosmetic:* `err-val` on a lowered Result emits a `-Wint-conversion` *warning*
     (`(int64_t)(r).as.Result._2` on the `const char *` field) -- compiles and runs
     correctly; the field-read cast uses the accessor's collapsed `int64` result
     type.  Tighten when convenient.
   - *Still open for FULL graduation:* the hand-written runtime layout
     (`tur_option_t`, `tur_result_box_t`, `tur_is_some`/`tur_opt_value`, the
     `MonadError` inline-C bodies) still assumes the struct/carrier representation;
     it coexists fine today (parity) but should be reconciled or retired when the
     gate comes off.
3. **`:heap` typed-pointer ADT ABI. FOUNDATION + NON-PARAMETRIC `:heap`-STRUCT
   AUTO-LOWERING DONE (2026-06-26); parametric stdlib `:heap` deferred.** A
   `:heap` record ADT lowers to a typed pointer (`tur_adt_<Name>__<args> *`) to a
   heap-allocated header -- the ADT analogue of a `:heap` struct's `Name *`.
   Foundation (all gated on `AdtDef.is_heap`, so inert unless an ADT is `:heap`):
   `defdata :heap` parsing; `type_c_name` typed-pointer for a `:heap` `TY_ADT` and
   concrete `:heap` `TY_APP` monomorph; malloc'ing ctors in both the parametric
   monomorph emitter (types.c) and the non-parametric emitter + its early-file
   mirror (emit_module.c); `->` field access for a `:heap` receiver;
   `emit_type_is_byvalue_adt` / `adt_field_is_inline_byval` excluding `:heap` (a
   `:heap` ADT is a pointer carrier, never an inline/boxed aggregate);
   `resolve_ctor_field` recording a `:heap` / forward-stub ADT field's `full_type`.
   A hand-written `(defdata HCell :heap [A] (HCell [fst : A snd : int]))` lowers,
   constructs and reads back correctly (fixture `conv-heap-adt-typed-pointer`).

   **`:heap`-struct auto-lowering (2026-06-26).** The lowering gate now admits a
   **non-parametric** `:heap` struct, and the by-value-vs-`:heap` integration tail
   is closed.  The double-pointer / spurious-address-of bug (`bsum(&__t32)` where
   `__t32` is already `tur_adt_Big *`) came from a `:heap` ADT being treated as a
   `>16`-byte by-value aggregate; the same erasure misbound a `:heap` `match`
   scrutinee as an aggregate.  A new `type_is_heap_adt` predicate (types.c, the ADT
   analogue of `type_is_heap_struct`) plus `:heap` exclusions at each by-value
   site: **pbp** (`adt_byval_pass_by_ptr` / `adt_app_byval_pass_by_ptr` bail for
   `:heap`, so the param stays a single `tur_adt_Big *` and the call site drops the
   `&`); **carrier-ABI** (`type_uses_carrier_abi` returns false for a
   non-parametric `:heap` ADT, mirroring a non-parametric `:heap` struct);
   **`match` scrutinee** (`emit_expr.c` excludes `:heap` from the by-value branch,
   so it binds a pointer and reads `->`); **the ctor-arg cast** (a `:heap`-ADT
   argument to a struct's carrier field slot gets the `(int64_t)(intptr_t)` cast,
   so no `-Wint-conversion`).  A non-parametric `:heap` struct now lowers, with
   field access, function-arg passing (`:heap` value -> `:heap` param), `match`,
   `with` on a `:copy :heap` struct, keyword construction, and a `:heap` field of a
   non-heap struct all at parity with the struct path (fixture
   `conv-defstruct-heap-struct-lowering`).

   **Remaining (kept on the struct path by the gate):** a **parametric** `:heap`
   struct (the stdlib `Vec`/`Map`/`Set`/`MutableMap`, whose inline-C bodies assume
   the struct/typed-pointer rep) stays on the struct path until that
   reconciliation lands -- the largest remaining piece.
4. **Validation across every autoloaded parametric stdlib type**, then delete the
   gate + `g_opt_defstruct_as_defadt` + the `EXPERIMENTS[]` row, retire the
   `StructDef` surface path, and regenerate snapshots.

### Current state (suite green)

`bash tests/run.sh` is **1845 passed, 0 failed** with the gate widened to
parametric (non-`:heap`) structs and **non-parametric `:heap` structs**; the
flag-on `conv-defstruct-*` canaries pass (including the new
`conv-defstruct-heap-struct-lowering`).  The flag is still opt-in.  **`Option`
and `Result` -- the two hardest dedicated-codegen stdlib types -- now work under
the flag at parity with the no-flag baseline:** construction
(`some`/`none`/`ok`/`err`), accessors (`ok?`/`ok-val`/`err-val`/`unwrap-or`), and
HKT instances (`fmap`/`bind`/`alt-or` on Option) all compile and run with correct
results.  (`fmap`/`bind` on `Result` remain blocked by a *baseline* type-recovery
limitation, not the lowering.)

Seams 1, 1b, 2 are DONE, and seam 3 now covers the `:heap` ADT ABI foundation
(a hand-written `:heap` defadt lowers and runs) **and non-parametric `:heap`-struct
auto-lowering** (`conv-defstruct-heap-struct-lowering`).  Graduation order from
here: finish **seam 3** -- lower the **parametric** stdlib `:heap` containers
(`Vec`/`Map`/`Set`/`MutableMap`), whose inline-C assumes the struct rep, the last
representational gap -> **seam 4** (validate across every parametric stdlib
type, then delete the gate + `g_opt_defstruct_as_defadt` + the `EXPERIMENTS[]`
row, retire the `StructDef` surface path, regen snapshots).  The suite does not
yet exercise the by-value HKT usage the flag now supports, so adding flag-on
fixtures for it is part of seam 4.
