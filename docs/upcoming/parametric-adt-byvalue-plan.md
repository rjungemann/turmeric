---
title: Parametric ADT by-value monomorphisation
category: Planning
description: Make a concrete monomorphisation of a single-variant parametric flat product (e.g. (Pair2 int float)) flow BY VALUE -- a flat tur_adt_Name__args aggregate -- instead of the int64 heap-pointer carrier. The parametric analog of CONV-S1/B1-B4 and the foundation the :heap typed-pointer ADT ABI builds on. Heavy prerequisite for CONV-S1 (defstruct->defadt) graduation.
---

# Parametric ADT by-value monomorphisation -- plan

## Status at a glance (2026-06-25)

- **P1 (gate + plumbing, hard-off)** -- **LANDED**. `bash tests/run.sh` 1825 /
  0, zero churn. Gate-on smoke test reproduces exactly the 4 expected crossings.
- **P2 (Crossing A: match / field-access / result-init on app receiver)** --
  **NOT STARTED**.
- **P3 (Crossing B: by-value app value into a carrier ctor field)** --
  **NOT STARTED**.
- **P4 (flip `g_adt_app_byvalue` on + regen snapshots)** -- **NOT STARTED**.
- **Step 5 (`:heap` typed-pointer ADT ABI)** -- **NOT STARTED**, layered on
  top of P4. This is the last gap before CONV-S1 graduation.

**Next action:** P2 (Crossing A). Independent of CONV-S1 B4 (the
functor-applied-to-self HKT crossing) -- the predicate excludes residual-tyvar
fields, so M7 reconciliation is not a prerequisite for P2-P4.

**Downstream gate:** CONV-S1 graduation -- retiring `StructDef` and lowering
every `defstruct` to `defadt` ([`defstruct-as-defadt-plan.md`](defstruct-as-defadt-plan.md)
slice 5) -- is blocked on **step 5 of this plan**. There is no separate
tracker for the `:heap` typed-pointer ABI; it lives here.

## Why

CONV-S1 graduation ("retire the `StructDef` surface path, lower every
`defstruct` to a `defadt`") is blocked because **parametric** and **`:heap`**
structs -- pervasive in stdlib (`Option`, `Pair`, `Vec`, `Map`, `Set`,
`List`) -- have no by-value / typed-pointer ADT lowering. A monomorphised struct
flows by value; a monomorphised single-variant flat-product ADT today still
flows through the int64 heap-pointer carrier:

```turmeric
(defdata Pair2 [A B] (Pair2 [a : A b : B]))
(defn fst [p : (Pair2 int float)] : int (match p (Pair2 a b) a))
```

emits (carrier):

```c
static int64_t ctor_Pair2__int__float(int64_t _0, double _1) {
    tur_adt_Pair2__int__float *__r = malloc(sizeof(*__r));   /* heap box */
    __r->as.Pair2._0 = _0; __r->as.Pair2._1 = _1;
    return (int64_t)(intptr_t)__r;
}
static int64_t fst(int64_t p) { ... (tur_adt_Pair2__int__float *)(intptr_t)(p) ... }
```

The monomorphised typedef `tur_adt_Pair2__int__float` already has the concrete
layout (`int64_t _0; double _1;`) -- it is *representationally* a by-value
aggregate, exactly like a monomorphised struct. Flipping it to by-value is the
parametric analog of the non-parametric CONV-S1/B1-B4 work, and the
representational foundation the `:heap` typed-pointer ADT ABI then builds on
(`:heap` = parametric flat product behind a typed pointer).

This is explicitly the layer the s1-bridging findings deferred ("Monomorphisation
of parametric flat ADTs as by-value ... is the next layer once this lands").

## The gate

A single predicate, hard-off today, mirrors `adt_is_byvalue_product`:

```c
/* types.c -- GATED HARD-OFF via `static const bool g_adt_app_byvalue` */
bool adt_app_is_byvalue_product(Type t);
```

True when `t` is a concrete monomorphisation of a single-variant, non-GADT,
**parametric** flat product whose every monomorphised field resolves to a
by-value-able concrete type (no residual tyvar / forall / non-concrete
application -- those stay M7's job). The plumbing keyed on it is in place behind
the gate:

- `type_c_name(TY_APP)` returns the by-value monomorph name
  (`type_register_adt_app(t)`) instead of `int64_t`.
- `type_uses_carrier_abi(TY_APP)` returns `false` so the value spills / boxes /
  derefs through the same `emit_carrier_bridge` machinery a monomorphised struct
  uses.
- `emit_registered_adt_app_rec` (types.c) emits the ctor returning the aggregate
  by value (no malloc, no tag), with the signature return type switched from
  `int64_t` to the monomorph typedef.

With the gate **off** the emitted C is byte-identical: `bash tests/run.sh` is
**1825 passed, 0 failed, zero snapshot churn**.

## The crossings (gate-on smoke test)

Flipping `g_adt_app_byvalue` to `true` and running the full suite reproduces
**exactly 4 failing fixtures**, in **two** crossing classes -- the parametric
analog of the non-parametric set, and notably *without* the M7 by-value-HKT
collision the s1-bridging findings feared (the predicate requires fully concrete
fields, so the HKT functor instantiations -- `ReF`, `ExprF` -- never qualify):

| Fixture | Class | Error |
| --- | --- | --- |
| `adt-param-match-type-pair` | A -- match / field read | `aggregate value used where an integer was expected` |
| `kind-inference-adt`        | A -- match / field read | `aggregate value used where an integer was expected` |
| `positional-adt-poly-ok`    | A -- match / field read | `aggregate value used where an integer was expected` |
| `defdata-applied-type-field`| B -- field store        | `incompatible type for argument 1 of 'ctor_N'` |

### Crossing A -- match / field-access on an ADT-app receiver

The match emit ([`emit_expr.c`](../../src/compiler/emit_expr.c), the
`match_`/scrutinee path ~6543-6660) resolves the scrutinee to its base `AdtDef`
and computes the monomorph C name, but its by-value decision keys on
`adt_is_byvalue_product(adt)` -- which is `false` for a parametric base def
(`n_type_params != 0`). So an app scrutinee binds as a carrier pointer
(`tur_adt_X *__scrut = (tur_adt_X *)(intptr_t)(p)`) and reads fields with `->`,
but `p` is now the by-value aggregate. The same applies to the `EX_GET_FIELD`
ADT branch (~5186-5240) and the result-`{0}` init (~6578).

**Fix:** widen the by-value decision at each of these sites from
`adt_is_byvalue_product(adt)` to also accept
`adt_app_is_byvalue_product(scrutinee_type)` (an app whose base is a by-value
monomorph), then take the existing by-value scrutinee-bind / `.`-read / pbp
paths CONV-S1 already built for the non-parametric case. The pass-by-pointer
size gate (`adt_byval_pass_by_ptr`) must gain an app-aware sibling so a large
monomorph (`> 16` bytes) keeps the `const T *` ABI.

### Crossing B -- by-value ADT-app value into a ctor field slot

`defdata-applied-type-field` stores a by-value ADT-app value into another
constructor's field. The N-arg ctor-call store
([`emit_expr.c`](../../src/compiler/emit_expr.c) ~2944-3006) already boxes a
by-value *non-parametric* ADT (`emit_type_is_byvalue_adt`) into a carrier int64
field, and skips the box for an inline by-value field (slice 4). Neither branch
recognises a by-value **ADT-app** value, so it is handed straight to the int64
ctor param. **Fix:** extend `emit_type_is_byvalue_adt` (or add an app-aware
companion) so a by-value ADT-app value is boxed into a carrier field slot and
unboxed at the matching field-bind read -- the parametric analog of the B3
field-store crossing -- and skipped when the owning field is itself an inline
by-value aggregate.

## Recommended decomposition (each suite-green)

1. **P1 -- gate + plumbing, hard-off.** `adt_app_is_byvalue_product` predicate;
   by-value `type_c_name` / `type_uses_carrier_abi` / monomorph-ctor plumbing
   keyed on it; gate off so the suite is byte-identical. **LANDED** (this change)
   -- `bash tests/run.sh` is 1825 passed, 0 failed, zero churn; the gate-on smoke
   test reproduced exactly the 4 crossings above.
2. **P2 -- Crossing A.** Widen the match / `EX_GET_FIELD` / result-init by-value
   decision to accept `adt_app_is_byvalue_product`, and add the app-aware
   pass-by-pointer size gate. Verify the three Class-A fixtures build by value.
3. **P3 -- Crossing B.** App-aware box/unbox at the ctor-arg store and field-bind
   read. Verify `defdata-applied-type-field`.
4. **P4 -- flip the gate.** Turn `g_adt_app_byvalue` on; regenerate the
   handful of monomorph snapshots that move (only by-value flat-product apps);
   add a `conv-byval-adt-app-*` fixture. Verify suite-green with the gate live.
5. **`:heap` typed-pointer ADT ABI.** With by-value monomorph apps in place, add
   the `:heap` defadt attribute and the `tur_adt_X__A *` typed-pointer lowering
   (the parametric-`Vec<T> *` analog), so a `:heap` struct can lower to a `:heap`
   defadt. This is the last gap before CONV-S1 graduation (slice 5).

Then CONV-S1 graduation (retire `StructDef`, lower unconditionally) becomes
reachable: parametric structs lower to by-value monomorph apps, `:heap` structs
to typed-pointer apps, and the remaining non-parametric field kinds
(`rc`/`ref`/`ptr`/bare struct) widen the lowering gate.

## Note on M7

The s1-bridging findings flagged that the parametric/HKT by-value path "collides
with the existing M7 by-value-HKT machinery" and should be "sequenced with (or
after) M7's by-value-HKT graduation." The smoke test shows the *concrete-field*
subset gated here does **not** touch that machinery (the functor instantiations
`ReF`/`ExprF` carry a residual tyvar field and are excluded by the predicate).
The M7 reconciliation (B4) remains a separate effort for the
functor-applied-to-self carriers; it is not a prerequisite for P2-P4 here.
