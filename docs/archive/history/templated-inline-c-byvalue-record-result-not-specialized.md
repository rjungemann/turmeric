# Templated inline-C return-only-poly helper not specialized for a by-value RECORD result under lowering

**RESOLVED (2026-06-27).**  The fix landed exactly where the diagnosis pointed:
`emit_abi_register_call`'s return-only-poly recovery (`recovered_byvalue`,
emit_module.c) accepted a `TY_STRUCT`/`TY_APP` by-value result but not a lowered
`TY_ADT` record, so the helper stayed on the int64 carrier base.  Extending the
gate to a non-:heap concrete by-value `TY_ADT` un-collapses the result, sets
`abi_changes`, and interns the per-element spec (with the `__TUR_TY_A__`-
substituted body).  Cleared `typeclass-assoc-type-method-return`,
`generic-inline-c-struct-through-unsafe`, and
`typeclass-assoc-type-parametric-struct-element`; default suite 1863/0.
Original report below.

**Severity:** medium (seam-4 / defstruct-as-defadt graduation blocker; not a
default-path bug). 2 fixtures.

## One-line summary

A return-only-polymorphic **templated** inline-C helper (`dense-get [A] : A`
with a `__TUR_TY_A__` body) whose result `A` resolves to a by-value RECORD
(`Pos`) is, under the `defstruct-as-defadt` lowering, NOT specialized per element
type -- the call resolves to the int64 carrier base (`dense_hyget` /
`_un_undense_hyget`, whose `__TUR_TY_A__` is `int64`), so a wrapper / instance
spec that returns the by-value `tur_adt_Pos` does `return <int64 base>(...)`
("incompatible types when returning int64_t but tur_adt_Pos expected").  Because
`Pos` is **wide** (16 bytes), there is NO leaf return bridge: the int64 base
reads `arr[idx]` with an `int64_t*` stride and only ever sees the first 8 bytes,
so the helper body itself must be re-emitted with `__TUR_TY_A__ = tur_adt_Pos`.

## Minimal repro

`tests/fixtures/typeclass-assoc-type-method-return` and
`tests/fixtures/generic-inline-c-struct-through-unsafe` under the force-lower
probe (`TUR_FORCE_LOWER=1`), or once the experiment graduates:

```turmeric
(defstruct Pos [x : int])            ; (typeclass-assoc) -- here Pos is 1 field
(defn dense-get [A] [s : (Dense A) idx : int] : A
  ```c return ((__TUR_TY_A__*)(intptr_t)s)[idx]; ```)
;; (Dense Pos) get/insert round-trip -> sop-get returns Pos
```

(In `generic-inline-c-struct-through-unsafe` the helper is `__dense-get [A] : A`
reached through `(unsafe ...)`, so the carrier base is the unsafe-handler wrapper
`_un_undense_hyget`; `Pos` there is `{x;y}` = 16 bytes, unambiguously wide.)

## Root cause (located)

`abi_changes` IS set for the helper (emit_module.c ~L2725:
`type_c_name(generic_result)="int64_t"` vs `type_c_name(result_type)="tur_adt_Pos"`
differ), so a spec *should* be interned.  But:

- **typeclass-assoc**: no `dense_get__spec__tur_adt_Pos` is emitted at all; the
  instance-method spec body's inner `(dense-get s idx)` resolves to the int64
  base `dense_hyget`.  At default (Pos is a `TY_STRUCT`) the spec
  `dense_get__spec__Pos` IS emitted (an inline-C clone with `__TUR_TY_A__=Pos`)
  and the instance body calls it -- so the struct->ADT flip is the only change.
- **generic-inline-c-struct-through-unsafe**: `dense_get__spec__tur_adt_Pos` IS
  emitted but as a WRAPPER (`return _un_undense_hyget(d, idx)`), not an inline-C
  clone -- it calls the int64 carrier base rather than re-emitting the
  `__TUR_TY_A__`-substituted body.

The spec-forcing/selection paths that re-emit a templated inline-C body with the
substituted `__TUR_TY_A__` (and the abi-scan that interns the inner call's spec)
recognize a by-value `TY_STRUCT` result but not a lowered by-value `TY_ADT`
record -- so under lowering the helper falls back to the carrier base.

## Fix directions

The helper must be specialized per element type with `__TUR_TY_A__ =
tur_adt_Pos` so its body reads the array at the correct (16-byte) stride and
returns the aggregate by value.  This lives in the abi-spec interning / scan
machinery (emit_module.c: the templated-inline-C spec path + `emit_abi_scan_expr`
child-spec interning), extending the by-value-result recognition from `TY_STRUCT`
to a non-:heap concrete by-value `TY_ADT`/ADT-app.  No leaf return bridge is
viable -- `Pos` is wide, so the int64 carrier base is lossy.

This is high-blast-radius machinery (M3/M4/M5 abi-spec system); it wants
dedicated care rather than a quick patch, hence reported rather than forced.

## Notes

- Default suite is unaffected (only the lowered ADT representation of a by-value
  record element triggers it).
- Related but distinct from the (landed) wide-by-value-element accessor-unbox +
  ctor-box: that fixed the *consumer* (ok-val/unwrap) and the *ctor*; this is the
  *producer* helper's own specialization.
