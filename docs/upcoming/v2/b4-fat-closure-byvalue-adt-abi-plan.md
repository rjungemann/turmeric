---
title: B4 -- fat-closure ABI for by-value-ADT closure params (Re/Expr by-value)
category: Planning -- CONV-S1 tail / M7-adjacent
description: Bring the recursive non-parametric HKT carriers (Re, Expr) onto the by-value ADT path by giving the fat-closure ABI a single, uniform representation for a by-value-ADT closure parameter. Spun out of struct-adt-convergence-s1-bridging-findings.md (B4). Priority work -- the closure-ABI representation disagreement is in the same hazard family as the carrier/by-value mismatches that surface in the debugger (pretty-printer mis-decode, in-frame-eval wild deref).
---

# B4 -- fat-closure ABI for by-value-ADT closure params

## Status / provenance

Spun out of
[`struct-adt-convergence-s1-bridging-findings.md`](../struct-adt-convergence-s1-bridging-findings.md)
(the B4 item) so the CONV-S1 findings doc can close. **Priority work.** The
closure-ABI representation disagreement described here is in the same hazard
family as the carrier/by-value mismatches that show up in the debugger
(pretty-printer mis-decode, in-frame-eval wild deref, locals rendering as the
wrong type) and as the `nested-carrier-match-loses-concrete-element` reported
bug -- one closure boundary, two incompatible representations of the same
ADT. Land this.

**Re-spiked 2026-06-25, post-M7-graduation.** M7's by-value-HKT dispatch is
graduated -- **default ON** (`g_m7_hkt_enabled = true`, `src/runtime/globals.c`;
flipped 2026-06-19, `TUR_M7_HKT=0` opts back to the legacy carrier). M7
graduating did **not** deliver the ABI change described here. Re-running the
gate widening on today's tree reproduces the identical 9 `cc` errors. This is
a distinct, still-unbuilt change and is next.

## Status -- slice 1 LANDED (2026-06-25)

Slice 1 (the int64-wide single-carrier-wrapper case, covering `Re` and `Expr`)
is implemented behind the `byvalue-recursive-carrier` experiment
(`--enable=byvalue-recursive-carrier`). Flag-off is a byte-identical no-op (the
legacy carrier path is untouched); flag-on greens all five `hkt-cata-*-carrier`
fixtures, which now carry a `flags` file opting into the experiment. The full
default suite is green (`bash tests/run.sh`: 0 failed).

What landed:

- **Gate (`src/compiler/types.c`).** `adt_is_byvalue_product_d` admits a
  single-variant, single-field product whose sole field is an `(F Self)`
  type-application (kept on the int64 carrier) when the flag is on -- the
  8-byte wrapper case (`Re`, `Expr`). A new predicate
  `adt_is_byval_recursive_carrier_wrapper` names exactly this shape.
- **ABI = reinterpret, not box (the key decision).** A single-carrier wrapper's
  by-value representation *is* its int64 carrier, so it crosses the fat-closure
  boundary by reinterpret with **no heap box and no deref**. This sidesteps the
  ownership/free contract slice 2 still owes for the `> 8 byte` case.
- **Call site (`src/compiler/emit_expr.c`).** The fat-closure cast keeps the
  by-value aggregate param type (so it matches the unchanged thunk), and an
  erased-carrier arg is reconstructed into the aggregate with a designated
  compound literal `(tur_adt_X){ .as.<Ctor>._0 = (carrier) }`
  (`emit_byval_recursive_carrier_reconstruct`). A concrete-aggregate arg passes
  through untouched.
- **Field read (`src/compiler/emit_expr.c`).** Both match-arm binders (if-chain
  and switch) read a recursive-carrier-wrapper field as the raw int64 carrier
  (no `*(T*)` deref), since no box was ever made on the producer side.
- **Thunk (`src/compiler/emit_fns.c`).** *Unchanged.* Because an 8-byte
  single-eightbyte struct and `int64_t` share the SysV register ABI, keeping the
  thunk's by-value `tur_adt_X` parameter and reconstructing the aggregate at the
  call site is sufficient and avoids touching M7 thunk emission entirely.

## Status -- slice 2 LANDED (2026-06-25)

Slice 2 (the wide, `> 8 byte` by-value-ADT closure-param case) is implemented
behind the same `byvalue-recursive-carrier` experiment. The gate now also admits
a multi-field recursive carrier (e.g. `Expr = (Roll :int (ExprF Expr))`, 16
bytes); flag-off remains a byte-identical no-op and the full default suite is
green, with a new `hkt-cata-wide-byvalue-carrier` fixture exercising the wide
path (deep tree, three discriminating algebras).

**Ownership/free contract (the prerequisite the plan demanded).** A wide
by-value-ADT element that lives in a parametric carrier monomorph is stored as
an int64 **heap-box pointer**, allocated in that monomorph's constructor. **The
box is owned by the enclosing carrier node** -- it lives exactly as long as the
node does. The fat-closure boundary only ever **borrows** the box: the box
pointer rides the int64 carrier into the closure, and the thunk **deref+copies**
the aggregate out at entry (the ADT is `:copy`, so the copy is the value). The
thunk never frees -- there is no per-call transfer of ownership and so no
double-free / use-after-free hazard. (Carrier nodes themselves are
process-lifetime, exactly as on the legacy carrier path; no new leak is
introduced beyond what carrier ADTs already have.)

What landed (on top of slice 1):

- **Monomorph layout (`src/compiler/types.c`, `emit_registered_adt_app_rec`).**
  A wide by-value-ADT element field of a parametric carrier monomorph
  (`type_is_wide_byval_adt`) is stored as `int64_t` (box pointer) rather than
  inline, so the monomorph layout AGREES with the generic int64 carrier the
  fmap spec reads. The monomorph ctor takes the element by value (the aggregate)
  and heap-boxes it into the int64 slot. `adt_byval_value_size_bytes` gives the
  `> 8` decision; `type_is_wide_byval_adt` is flag-scoped (returns false when
  the experiment is off, so flag-off layout is untouched -- an ordinary wide
  by-value ADT like a two-float `Pt` keeps its B3 treatment).
- **Field read (`src/compiler/emit_expr.c`).** An erased (int64) carrier element
  binding of a wide by-value ADT reads the box pointer raw (no deref); the
  pointer crosses to the fat closure as the carrier. A *concrete* wide by-value
  ADT match binding (ctype is the aggregate, e.g. `Pt`) still takes the B3
  deref -- the two are distinguished by whether the binding is the erased int64
  carrier.
- **Call site (`src/compiler/emit_expr.c`).** A wide by-value-ADT fat-closure
  arg casts the fn-pointer param to `int64_t` and passes the box pointer raw
  (no reinterpret -- the value cannot fit a register pair through the uniform
  slot).
- **Thunk (`src/compiler/emit_fns.c`).** *This is the emit_fns.c work slice 1
  avoided.* A closure thunk whose parameter is a wide by-value ADT now takes it
  as an `int64_t` box pointer (`needs_box_load`, the inverse of the existing
  box-spill) and deref+copies it into the by-value aggregate at body entry --
  borrow only, no free. The two forward-decl emitters
  (`emit_abi_forward_decl`, `emit_fn_forward_decls` in `src/compiler/emit_module.c`)
  mirror the int64 param so prototype and definition agree.

Still open: **slice 3** (graduate / retire the flag once the legacy carrier
suite is also reconciled).

## The problem in one sentence

A user closure `g : (fn [Re] B)` is invoked inside a generic `fmap`
specialisation through the fat-closure ABI; once `Re` flows by value, the
closure's fat function-pointer cast spells the param `tur_adt_Re` (by value),
but the carrier `fmap` spec hands it the `int64` the parametric `ReF` field
stores -- one closure, two incompatible call ABIs.

## Reproduction (exact)

Widen the by-value gate to admit recursive non-parametric products:

```c
/* src/compiler/types.c -- adt_is_byvalue_product */
bool adt_is_byvalue_product(const AdtDef *def) {
    return adt_is_flat_product(def) && def->n_type_params == 0;   /* admits Re/Expr */
}
```

Rebuild, then `tur build` any of the five fixtures
(`hkt-cata-fmap-byvalue-carrier`, `hkt-cata-captureless-fn-carrier-arm`,
`hkt-cata-fn-arg-carrier`, `hkt-cata-fn-carrier-recursive`,
`hkt-cata-mixed-fn-value-carrier`). Result: **9 identical `cc` errors** across
`Re` and `Expr`, all of the form:

```c
__t79 = ctor_StarF__float(((double (*)(void*, tur_adt_Re))g.fn)(g.env, x_1268));
//                                            ^^^^^^^^^^ expects by-value Re; x_1268 is int64
```

## Why it breaks (the emitted C, decoded)

`Re = (Roll (ReF Re))`; its sole field `(ReF Re)` is a `TY_APP` and stays on the
carrier, so the by-value `tur_adt_Re` is just an 8-byte wrapper around the
carrier int64:

```c
typedef struct tur_adt_Re { union { struct { int64_t _0; } Roll; } as; } tur_adt_Re;
```

The by-value monomorphised thunk is generated with a **by-value** param:

```c
typedef double (*tur_thunk_double_tur_adt_Re_t)(void *, tur_adt_Re);
static double __fn_1274__spec__double_void___tur_adt_Re(void *__env, tur_adt_Re c) {
    /* ... re_cata__spec__double_..._tur_adt_Re(alg, c) ... */
}
```

...but the carrier `fmap` spec
(`__inst_Functor_fmap_T__spec__..._h2`, scrutinee `tur_adt_ReF *`, `int64_t`
fields) calls that **same** `g.fn` with a raw `int64_t`:

```c
int64_t x_1268 = *(int64_t *)(intptr_t)(__scrut->as.StarF._0);
__t79 = ctor_StarF__float(((double (*)(void*, tur_adt_Re))g.fn)(g.env, x_1268));
```

The codegen is genuinely multi-representation: a `tur_adt_ReF__Re` typedef stores
`Re` **inline by value**, alongside the carrier `ReF` whose `a` field is `int64`;
different `fmap` specs disagree on the field representation, and they all share
one closure.

## Why the obvious local fix is wrong

Unboxing `x_1268` at the application (`*(tur_adt_Re *)(intptr_t)x_1268`) is only
correct if that int64 is a heap-boxed `Re` -- which depends on *which* `ReF` spec
feeds the value: the `ReF__Re` path stores `Re` inline (no box), the carrier
path boxes. Guessing wrong dereferences a non-pointer. The codebase already
records a prior wild-pointer deref at this very application site from exactly
this class of carrier->concrete assumption (`emit_expr.c:2397-2400`). A local
bridge trades a `cc` error for a latent segfault.

## The fix: one uniform ABI for a by-value-ADT closure param

Make the **fat-closure ABI carry a by-value-ADT parameter as the int64 carrier
uniformly**, so every call-site cast is `(void*, int64_t)` and the inline-storage
and carrier paths agree:

1. **Call site (every fat-closure application of a by-value-ADT param).** Force
   the function-pointer cast to `(R (*)(void*, int64_t, ...))` and pass the int64
   carrier. The value is *already* an int64 in the carrier `fmap` spec; in the
   `ReF__Re` inline spec, extract the carrier from the inline aggregate.
2. **Thunk entry.** Generate the thunk with an `int64_t` param and **reconstruct**
   the by-value aggregate from the carrier at entry:
   - **<= 8 bytes (e.g. `Re`)**: reinterpret -- `tur_adt_Re c = (tur_adt_Re){ .as.Roll._0 = __carrier };`. No malloc.
   - **> 8 bytes**: the carrier is a heap box; deref + copy at entry. This needs
     an **ownership/free contract** (who frees the box, and when) -- the genuinely
     hard, still-unbuilt part. Define it before widening past the int64-wide case.
3. **Closure creation / arg pass.** Symmetric: box a by-value-ADT arg into the
   int64 carrier (reinterpret when <= 8 bytes, heap box when wider) before it
   enters a fat closure.

Net: the cast is always `(void*, int64_t)`; `tur_adt_ReF__Re` inline storage and
the carrier `ReF` agree on a carrier `Re` at the closure boundary.

## Sequencing

1. **Re-only (int64-wide) slice.** Land steps 1-2 for the `<= 8 byte` case where
   box/unbox is a pure reinterpret (covers `Re` and `Expr`, both single-carrier
   wrappers). Gate behind `--enable=` per the experimental-features rule; flag-off
   no-op, flag-on greens the five `hkt-cata-*` fixtures. Even though the box/unbox
   is trivial for `Re`, the closure-ABI disagreement is the real blocker and
   there is no sensible partial that lands `Re` without this ABI change -- start
   here.
2. **Wide by-value-ADT slice.** Define the heap-box ownership contract for
   `> 8 byte` by-value-ADT closure params and extend steps 1-3.
3. **Graduate / retire the flag** once both the suite and the legacy carrier
   suite are green.

## Touch points

- `src/compiler/types.c` -- `adt_is_byvalue_product` gate widening (the trigger).
- `src/compiler/emit_expr.c` -- fat-closure application cast (phase-F-concrete
  path, ~2402; the `(void*, T)` cast that must become `(void*, int64_t)`).
- `src/compiler/emit_fns.c` -- M7 layer-4 by-value HKT thunk emission
  (`g_m7_hkt_enabled` sites, ~562 / ~1188 / ~1278); thunk param type +
  entry reconstruction.
- Fixtures: the five `hkt-cata-*-carrier` directories are the gate.

## Relationship to other plans

- Adjacent to
  [`parametric-adt-byvalue-plan.md`](../parametric-adt-byvalue-plan.md): both
  concern by-value representations of parametric/recursive ADTs, and a unified
  by-value-ADT carrier convention should be shared between them.
- In the same hazard family as the debugger plans
  ([`debugger-native-types-plan.md`](../debugger-native-types-plan.md),
  [`debugger-inframe-eval-plan.md`](../debugger-inframe-eval-plan.md),
  [`debugger-phase5-native-types-progress.md`](../debugger-phase5-native-types-progress.md))
  and the
  [`nested-carrier-match-loses-concrete-element`](../../reported/nested-carrier-match-loses-concrete-element.md)
  reported bug: all turn on whether the by-value monomorphization presents a
  single, predictable representation. A uniform fat-closure ABI removes one of
  the disagreement sites that pretty-printers and in-frame eval would otherwise
  have to defend against.
