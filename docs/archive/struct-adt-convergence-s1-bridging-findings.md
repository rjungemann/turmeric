---
title: CONV-S1 By-Value Merge -- Bridging Findings
category: Planning
description: Concrete site-by-site findings from a spiked attempt at CONV-S1 (make single-variant non-GADT non-parametric ADTs flow by value). The by-value codegen core is straightforward and correct; the work that makes it land is the byval<->carrier bridging at every crossing plus untyped-param inference. This doc enumerates the exact sites and the eight fixtures that gate it, so the merge can be scoped deliberately rather than discovered mid-flight.
---

# CONV-S1 (by-value merge) -- bridging findings

## Status at a glance (2026-06-25)

- **B1 (box/unbox + representation dispatch)** -- **LANDED**, gate off.
- **B2 (untyped-param refinement)** -- **LANDED**, gate off.
- **B3 (flip gate for leaf products + field-store crossing)** -- **LANDED, gate
  LIVE** for leaf products (no aggregate-bearing / recursive fields). Suite
  green: `bash tests/run.sh` 1813 / 0.
- **B4 (reconcile with M7 by-value HKT)** -- **NOT LANDED; reframed as
  out-of-scope/moot (2026-06-25).** M7's by-value HKT is now **graduated --
  default ON** (`g_m7_hkt_enabled = true`), so the old "sequence with M7
  graduation" framing is satisfied on the timeline -- **but a fresh post-M7 spike
  reproduces the identical 9 `cc` errors**: M7 graduating did NOT deliver, and
  does not unblock, the fat-closure-ABI change B4 needs (carry a by-value-ADT
  closure param as the int64 carrier uniformly). That change was never part of
  M7's scope and remains unbuilt. **NOT a prerequisite for CONV-S1 proper** --
  real `defstruct`s never have functor-applied-to-self fields, so the
  `defstruct -> defadt` path never reaches this crossing. The B3 leaf-and-pointer
  gate is the permanent boundary; build the fat-closure ABI only if some future
  feature actually demands recursive-HKT-carrier by-value. See the B4 section
  below for the re-spike details.
- **CONV-S1 proper (lower `defstruct` to `defadt`)** -- **LANDING incrementally
  behind `--enable=defstruct-as-defadt`.** Scalar, by-value-aggregate (slice 4),
  **pointer** (`rc`/`ref`/`lref`/`weak`/`ptr<void>` -- slice 5), and bare **`fn`**
  (slice 6) fields lower; the slice-5/6 work also closed two flag-independent
  record-ADT gaps -- `rc<ADT>` field-deref and `fn`-field capability calls.
  Tracked in [`defstruct-as-defadt-plan.md`](defstruct-as-defadt-plan.md);
  graduation (now slice 7) -- retire the `StructDef` path, lower bare-struct /
  parametric / typed-fn fields unconditionally -- remains.

**Net status:** the CONV-S1 boundary is the B3 leaf gate. B4 is open but
out-of-scope for graduation. Next action lives in the defstruct-as-defadt
plan, not here.

## Background

This documents a **spike** of CONV-S1 from
[`struct-adt-convergence-plan.md`](struct-adt-convergence-plan.md): making a
single-variant, non-GADT, **non-parametric** ADT flow *by value* (a flat
`tur_adt_<Name>` C aggregate) instead of through the heap-pointer int64
carrier, as the prerequisite for lowering `defstruct` to `defadt`.

The spike was **reverted** -- the tree is green and unchanged. The by-value
codegen *core* worked and was verified end-to-end for the typed case; what it
revealed is that the suite-green requirement needs the full **byval<->carrier
bridging merge** (the effort the plan deferred), not a contained increment.
This doc captures the exact terrain so the merge can be planned with eyes open.

## What the by-value core is (small, correct, verified)

Gate the by-value representation on a single predicate added next to
`adt_is_flat_product` ([`types.h:302`](../../src/compiler/types.h)):

```c
static inline bool adt_is_byvalue_product(const AdtDef *def) {
    return adt_is_flat_product(def) && def->n_type_params == 0;
}
```

(Parametric flat ADTs -- e.g. stdlib `Fix`, `Re`'s functor `ReF [a]` -- keep
the carrier ABI, matching how unspecialised parametric structs already behave;
their concrete monomorphic layout is the M7 by-value-HKT path's job.)

With that gate, six codegen sites flip to by-value:

1. **`type_c_name(TY_ADT)`** ([`types.c:2572`](../../src/compiler/types.c)) --
   return the `tur_adt_<mangled>` typedef name instead of `"int64_t"`. (Needs a
   stable name: cache it on `AdtDef` in an inline `char c_name_byval[256]` --
   **not** a heap pointer, or the leak-checked emit path fails LSan -- filled by
   a shared `adt_byval_c_name(def)` helper placed next to `mangle_field_name`
   in [`emit_core.c:929`](../../src/compiler/emit_core.c).)
2. **`type_uses_carrier_abi`** ([`emit_core.c:341`](../../src/compiler/emit_core.c))
   -- return `false` for a by-value product, so it spills/boxes/derefs through
   the same machinery as a non-parametric struct.
3. **Constructor emission** -- both the per-module
   `emit_adt_typedef_and_ctors` ([`emit_module.c:4689`](../../src/compiler/emit_module.c))
   **and** the early-file mirror ([`emit_module.c:8089`](../../src/compiler/emit_module.c))
   must return the aggregate by value (`tur_adt_S __r; __r.as.Ctor._i = _i;
   return __r;`) with the signature return type switched from `int64_t` to the
   typedef name. **Both** paths -- they are used in different build modes and
   must stay in lockstep.
4. **Field access** (`EX_GET_FIELD` ADT branch,
   [`emit_expr.c:5135`](../../src/compiler/emit_expr.c)) -- read `(sv).as.Ctor._i`
   directly, dropping the `(tur_adt_X *)(intptr_t)` carrier cast.
5. **`match`** (if-chain path,
   [`emit_expr.c:6488`](../../src/compiler/emit_expr.c)) -- declare the scrutinee
   by value (`tur_adt_S __scrut = (scrut);`) and bind fields with `.` instead of
   casting an int64 to a pointer and using `->`. (The flat path already skips the
   tag test, so the single arm is entered unconditionally -- correct.)

Verified: for a **typed** receiver, e.g.

```turmeric
(defdata S (S [a : int b : int]))
(defn sum [s : S] : int (match s (S a b) (+ a b)))
```

the emitted C is fully consistent by value:
`static tur_adt_S ctor_S(int64_t,int64_t)` returning by value,
`static int64_t sum(tur_adt_S s)`, `tur_adt_S __scrut = (s);`,
`tur_adt_S s = ctor_S(10,32); sum(s);`. Construction, by-value let, by-value
param ABI, by-value match, and field bind all agree.

## Why it is not suite-green: the byval<->carrier crossings

Running the full suite with the core in place yields **exactly 8 failures**,
all the same root cause -- a by-value ADT value meeting an `int64`/carrier-typed
slot with no bridge:

| Fixture | Crossing |
| --- | --- |
| `conv-single-variant-flat` | untyped param `sum [s]` lowered to `int64`, construction is by-value |
| `conv-kw-record-variant` | untyped param `sum3 [p]`, same |
| `gadt-adt-skolem` | by-value `Foo` stored as a field of the carrier GADT `Expr` (`Box (Foo)`) |
| `hkt-cata-fmap-byvalue-carrier` | by-value `Re` passed as a closure arg (`g.fn` expects `tur_adt_Re`, dispatch passes int64) |
| `hkt-cata-captureless-fn-carrier-arm` | same, `Re`/`Expr` through HKT closure |
| `hkt-cata-fn-arg-carrier` | same |
| `hkt-cata-fn-carrier-recursive` | same |
| `hkt-cata-mixed-fn-value-carrier` | same |

(`recursive-types/mutual-recursion` types its ADT params `: int` explicitly and
happened to stay consistent -- but it is the same hazard class and should be
treated as a ninth canary.)

### Crossing 1 -- untyped / `:int`-typed ADT params (inference)

An untyped param used as a `match` scrutinee is left as `TY_INT` (the int64
carrier): `param_kinds[...] = TY_INT` defaults in
[`elab_fns.c:1174,1791,3674,3764`](../../src/compiler/elab_fns.c), realised into
`fd->param_types[i]` around [`elab_fns.c:3434`](../../src/compiler/elab_fns.c).
The *use site* -- the match scrutinee `EX_VAR` -- gets refined to the ADT type
(`S`), but that refinement is **not** propagated back to the param binding. So
the signature emits `int64_t s` while the body reads `tur_adt_S __scrut = (s)`
-- invalid C, and the call passes a `tur_adt_S` to an `int64` param.

Two ways out, both with reach:

- **Refine the binding.** During match elaboration, when the scrutinee is a bare
  `EX_VAR` for an untyped param, write the resolved ADT type back to the param
  binding **and** `fd->param_types[i]`. Safe for multi-variant ADTs (they stay
  `int64` via `type_c_name`), only by-value products change -- but it changes
  the C type of every untyped ADT-matched param, so it must be suite-checked.
- **Bridge at the boundary.** Keep the param `int64`, **box** the by-value arg
  to a carrier at the call, and have `match`/field-access pick the carrier path
  when the scrutinee's *representation* is a carrier (see Crossing 4).

### Crossing 2 -- by-value ADT as a field of a carrier ADT/GADT

`gadt-adt-skolem`: `Foo` is a by-value product, but `Box (Foo)` stores it in a
carrier-GADT field whose C slot is `int64`. Today the constructor call
([`emit_expr.c:2944`](../../src/compiler/emit_expr.c)) passes the by-value
`tur_adt_Foo` straight into the `int64` ctor param, and the `match` field-bind
([`emit_expr.c:6517`](../../src/compiler/emit_expr.c) /
[`6610`](../../src/compiler/emit_expr.c)) casts the `int64` field to
`tur_adt_Foo`. Both need a **byval->carrier box** (malloc + copy, return int64)
at the store and a **carrier->byval unbox** (deref) at the read. This is the
same bridge by-value structs use when they cross into a carrier aggregate; the
helpers do not yet exist for ADTs.

### Crossing 3 -- by-value ADT as a closure / HKT argument

The five `hkt-cata-*` fixtures pass a by-value `Re`/`Expr` to a closure
(`fn [c : Re] : B`) invoked through the generic carrier closure ABI
(`TUR_APPLY*` / fat-closure shims). The closure param is now typed
`tur_adt_Re`, but dispatch hands it an `int64`. This is the deepest crossing and
**collides with the existing M7 by-value-HKT machinery**
([`emit_expr.c:2901-2943`](../../src/compiler/emit_expr.c) -- the per-instance
ctor-suffix path that already reads by-value `tur_adt_ReF__bool`). Any merge has
to reconcile the new non-parametric by-value path with M7's parametric
by-value-carrier path so they do not double-handle the same value.

### Crossing 4 -- representation-based dispatch (the unifying fix)

`match` and field-access currently decide by-value-vs-carrier from the
scrutinee's **logical type** (`e->as.match_.scrutinee->type`,
[`emit_expr.c:6439`](../../src/compiler/emit_expr.c)). The correct signal is the
scrutinee value's **actual C representation** (does its binding/expression emit a
by-value aggregate or an int64 carrier?). With box/unbox in place, an
`:int`-typed param that received a boxed ADT is a *carrier* and must use the
`(tur_adt_X *)(intptr_t)` path even though its logical type is the ADT -- exactly
the old behaviour. Until dispatch keys on representation, Crossings 1-3 cannot
be made consistent.

## Additional sites the merge must touch

- **~30 `kind == TY_STRUCT` byval-bridging checks** across
  `emit_expr.c` / `emit_core.c` / `emit_fns.c` must also accept by-value ADTs
  (e.g. `emit_carrier_return_override` [`emit_core.c:364`](../../src/compiler/emit_core.c),
  the box-spill eligibility loop [`emit_fns.c:733`](../../src/compiler/emit_fns.c),
  `type_struct_pass_by_ptr`). Grep: `kind == TY_STRUCT`.
- **Drop-glue for by-value ADTs with rc/weak fields** -- **CLOSED** (CONV-S1
  slice 2). The struct path
  ([`emit_module.c`](../../src/compiler/emit_module.c)) emits struct drop-glue;
  the by-value ADT path now emits the matching `drop_glue_tur_adt_<Name>` /
  `walk_glue_tur_adt_<Name>` via the shared `emit_adt_byval_drop_glue`, wired
  into `EX_RC_OF` through `rc_cb_alloc_struct`. A by-value `:copy` ADT is
  trivially copyable; a by-value non-`:copy` ADT carrying an rc field releases
  it when the wrapping `rc` drops. Fixture: `conv-byval-adt-rc-drop`.
- **Nested by-value aggregate fields** -- **CLOSED (CONV-S1 slice 4).** A
  by-value product whose field is itself a by-value aggregate (a
  drop-glue-free non-heap/non-opaque struct, or a by-value ADT product) now
  stores that field **inline by value**, exactly as a struct inlines a nested
  struct field, instead of boxing it behind the int64 carrier.
  `adt_is_byvalue_product` admits such a field via `adt_field_is_inline_byval`
  (the shared representation gate), and the typedef/ctor emitters, ctor-arg
  store (box skipped), field access, `match` field-bind, and pass-by-ptr size
  all key on it. Recursive HKT fixed-points (`Re`/`Expr`) are parametric and
  self-reference through a `(ExprF Expr)` TY_APP field, so they stay on the
  carrier path -- that is Crossing 3 / B4, below.
- **Monomorphisation of parametric flat ADTs as by-value** (the type-app path,
  [`types.c:1393`](../../src/compiler/types.c)) is out of scope for the
  non-parametric stage but is the next layer once this lands.

## Recommended decomposition (smaller, each suite-green)

1. **B1 -- box/unbox primitives + representation-based dispatch.** Add the
   byval<->carrier box/unbox for ADTs and make `match`/field-access key on the
   value's C representation. Land it with the by-value gate **off** (no ADT
   flips yet) so it is a no-op refactor that the suite proves green.
   **LANDED (gate off).** The gated by-value core is in place behind
   `adt_is_byvalue_product(def)` ([`types.h`](../../src/compiler/types.h), a
   single predicate currently hard-`false`): `type_c_name(TY_ADT)` returns the
   interned `adt_byval_c_name(def)`; `type_uses_carrier_abi` reports a by-value
   ADT as a non-carrier concrete aggregate (so it spills/boxes/derefs through the
   existing generic `emit_carrier_bridge`, emit_core.c:3105/3343 -- **no new
   per-type primitive is needed**); both constructor emitters
   (emit_module.c:`emit_adt_typedef_and_ctors` and the early-file mirror) return
   the flat aggregate by value; and `match` (if-chain) plus field-access bind the
   scrutinee as an aggregate and read fields with `.`. With the gate off the
   emitted C is byte-identical -- `bash tests/run.sh` is **1813 passed, 0 failed,
   zero snapshot churn**. A temporary gate-on smoke test reproduced **exactly the
   8 spike failures** above (no more, no less), confirming the core is faithful
   and the residue is precisely the three crossings.

   **Remaining B1 wiring (carried into B3): the field-store crossing is NOT yet
   auto-covered.** The smoke test showed `gadt-adt-skolem` still fails under
   gate-on: a by-value `Foo` passed into the `int64` field slot of carrier-GADT
   `Box` is handed straight to the `int64` ctor param with no box. The generic
   bridge *primitive* now applies to ADTs, but it is not *invoked* at the ctor-arg
   store (emit_expr.c N-arg ctor call, ~2944) nor at the `match` field-bind read
   for a byval-in-carrier field. Wiring that invocation runs through code shared
   by every ctor call, so it was deferred rather than risk the gate-off no-op;
   it is now an explicit B3 prerequisite (below), not a free side effect.
2. **B2 -- untyped-param refinement.** Propagate match-scrutinee ADT types to
   untyped param bindings. Still gate-off; verify no carrier fixture moves.
   **LANDED (gate off).** `elab_match` ([`elab_structs.c`](../../src/compiler/elab_structs.c),
   the ADT-inference patch site, ~3240) now writes the inferred ADT back onto the
   scrutinee's binding when that binding still carries the int64 default (an
   untyped param) -- never over an explicit annotation. The signature realiser
   ([`elab_fns.c`](../../src/compiler/elab_fns.c) param_types loop) runs after the
   body, so it preserves that refined ADT type for the parameter instead of
   collapsing it back to `type_from_kind(TY_INT)`; the second (`fn`/lambda)
   realiser already copies `params[i]->type` and picks it up for free. With the
   gate off `type_c_name(TY_ADT)` is `int64_t`, so the emitted signature is
   unchanged -- `bash tests/run.sh` stays **1813 passed, 0 failed, zero churn**.
   The gate-on smoke test dropped the residue from 8 to 6 genuine build failures:
   **`conv-kw-record-variant` now passes** and **`conv-single-variant-flat` now
   builds and runs** (its only gate-on delta is a snapshot drift that B3's gate
   flip will regenerate). The remaining failures are exactly the field-store
   crossing (`gadt-adt-skolem`, B3) and the five closure/HKT crossings (B4).
3. **B3 -- flip the gate for non-recursive, non-HKT products.** Turn on
   `adt_is_byvalue_product` for the simple cases (S, Point, Box/Foo). First wire
   the **field-store crossing** (box a by-value ADT arg into a carrier ctor field
   via `emit_carrier_bridge`, and unbox at the carrier field-bind read) -- the
   B1 smoke test proved this does not happen automatically -- so `gadt-adt-skolem`
   goes green as the gate flips.
   **LANDED (gate LIVE).** This is the first step that turns by-value on. Two
   refinements from the original sketch:
   - **Gate = leaf products, not the full predicate.** `adt_is_byvalue_product`
     ([`types.c`](../../src/compiler/types.c)) is now `adt_is_flat_product &&
     n_type_params == 0 && every field a scalar`. The "leaf" clause is the cheap,
     correct stand-in for "non-recursive": a recursive carrier's self-reference
     rides in a ctor field whose storage `kind` is collapsed to the int64 carrier
     (TY_INT) but whose `full_type` is the real `(ExprF Expr)` TY_APP -- so the
     gate inspects `full_type`, and `Expr`/`Re` (and any aggregate-bearing
     product) stay on the carrier path. A precise transitive-recursion check can
     widen this later once every crossing is wired; leaf is deliberately
     conservative and needs no caching. (The predicate moved from a `types.h`
     inline to a `types.c` function because it must dereference `full_type`, and
     `struct Type` is only forward-declared at that point in the header.)
   - **Field-store crossing wired by hand, not via the struct bridge.** A
     by-value ADT argument into a plain (non-suffixed) carrier constructor's int64
     field is boxed inline (malloc + copy -> int64) at the N-arg ctor call
     ([`emit_expr.c`](../../src/compiler/emit_expr.c)); the match field-bind (both
     the if-chain and switch paths) unboxes a by-value-ADT field by deref
     (`*(tur_adt_X *)(intptr_t)(__scrut->as.Ctor._i)`). A new
     `emit_type_is_byvalue_adt` helper is the single representation gate for both
     sides. With this, `gadt-adt-skolem` (by-value `Foo` stored in carrier-GADT
     `Box`) goes green.
   Result: `bash tests/run.sh` is **1813 passed, 0 failed** with the gate LIVE.
   Exactly one snapshot moved -- `conv-single-variant-flat` -- now showing the
   by-value `ctor_S` (returns the aggregate, no malloc), `sum(tur_adt_S)`, and a
   `tur_adt_S __scrut = (s)` match. The monomorphised-ctor (suffix) HKT path is
   untouched and stays B4's concern.
4. **B4 -- reconcile with M7 by-value HKT.** Bring the recursive/HKT carriers
   (`Re`, `Expr`) onto the unified by-value path without double-handling.
   **NOT LANDED; spun out to its own plan --
   [`v2/b4-fat-closure-byvalue-adt-abi-plan.md`](v2/b4-fat-closure-byvalue-adt-abi-plan.md).**
   It needs a dedicated **fat-closure ABI change** (carry a by-value-ADT closure
   param as the int64 carrier uniformly: reinterpret when <= 8 bytes, heap-box
   with an owner when wider; reconstruct at thunk entry; force every call-site
   cast to `(void*, int64_t)`). A re-spike on 2026-06-25 confirmed this is **not**
   delivered by M7's by-value-HKT graduation (default ON since 2026-06-19): the
   gate widening still produces the identical 9 `cc` errors across `Re`/`Expr`,
   because the by-value thunk takes `tur_adt_Re` while the carrier `fmap` spec
   calls the same `g.fn` with a raw `int64`. Even `Re` -- whose `tur_adt_Re` is
   just an 8-byte carrier wrapper, so its box/unbox is a reinterpret -- cannot be
   moved over on its own; the closure-ABI disagreement is the blocker, not the
   box/unbox width, so there is no sensible partial that lands `Re` without the
   full ABI change. **Out-of-scope/moot for CONV-S1:** nothing requires `Re`/`Expr`
   by-value (they are correct on the carrier path; M7 already gives them by-value
   monomorphisations where dispatch needs it), and a real `defstruct` cannot
   express a functor-applied-to-self field, so `defstruct -> defadt` never reaches
   this crossing. See the spun-out plan for the full reproduction, decoded C, the
   segfault hazard of a naive local bridge, and slicing.

   **Boundary decision.** The **B3 leaf gate is the principled CONV-S1
   boundary**: aggregate-bearing / recursive products keep the carrier ABI. This
   does not block "CONV-S1 proper" (below): a real `defstruct` never has a
   functor-applied-to-self field (`(ExprF Expr)`) -- recursive structs reference
   themselves through `rc<Self>` / `ref<Self>` pointer fields, which are carriers,
   not inline aggregates -- so `defstruct -> defadt` lowering targets exactly the
   leaf-and-pointer-field products the gate already flips. B4 widens coverage to
   the HKT fixed-point fixtures; it is not a prerequisite for lowering real
   structs.
5. **CONV-S1 proper -- lower `defstruct` to `defadt`.** The lowering needs the
   representation byte-identical for the products a `defstruct` actually produces
   -- single-variant, non-GADT, non-parametric, fields scalar or pointer
   (`rc`/`ref`/`ptr`) or nested struct/ADT. Per the B4 boundary decision this is
   the **leaf-and-pointer-field** subset the B3 gate already flips (extended to
   pointer/rc fields as needed); B4 (HKT fixed-points) is NOT a prerequisite,
   since `defstruct` cannot express a functor-applied-to-self field. Sequence:
   extend the gate to admit pointer/`rc`/`ref` fields (verify green), then lower
   `defstruct` to `defadt` and regenerate the ~57 `defdata` snapshots in the same
   change.

Each of B1-B4 is independently testable and keeps `bash tests/run.sh`
(timeout 600000) green; the gate flip (B3) is the only step that should move any
snapshot, and only for the simple-product fixtures.
