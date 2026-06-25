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
