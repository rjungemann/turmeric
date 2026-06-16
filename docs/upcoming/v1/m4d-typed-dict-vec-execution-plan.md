---
title: M4d -- Typed/Conditional Dict Emit for Eq[Vec] (root-2 crossings) -- Execution Plan
category: Planning -- ABI / Codegen, end-to-end monomorphization
description: The concrete plan for the dominant residual M3 audit bucket -- the 80 `Vec int` carrier crossings (root 2). Grounded in the emitted C of vec-of-tvec-eq / data-literal-typed-empty as of the post-Vec-producer-slice tree (commit 600e859). Establishes that these crossings live in a DEAD-but-emitted Eq[Vec] carrier base whose only referent is an unread dict singleton, and sequences the conditional-dict-emit work that removes them.
---

# M4d -- Typed/Conditional Dict Emit for Eq[Vec] -- Execution Plan

This is **root 2** in the bucket-A breakdown of
[m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](../../reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md):
the **80 `Vec int` carrier crossings** that dominate the 93-crossing residual
after the Vec producer slice (commit `600e859`). It is the implementation arm of
the parent plan's **M4** ("non-HKT typeclass instances switch to per-method
ABI") for the specific case the audit still flags.

## The finding that grounds everything (verified in emitted C)

Concrete `.eq?` dispatch on a `(Vec int)` already calls the **typed
per-instantiation spec** `__inst_Eq_eq_qu_Vec__spec__..(Vec__int *, Vec__int *)`
directly -- M4c (Path A) landed that. So the typed dispatch path is clean.

The crossings live in the **int64 carrier base** that M4c keeps for the dict:

```c
static bool __inst_Eq_eq_qu_Vec(int64_t x, int64_t y) {           // carrier base
    int64_t lx = vec_len_byval__spec__..((Vec__int *)(intptr_t)(x));   // crossing
    int64_t ly = vec_len_byval__spec__..((Vec__int *)(intptr_t)(y));   // crossing
    ... vec_eq_loop_byval__spec__..((Vec__int *)(intptr_t)(x), (Vec__int *)(intptr_t)(y), ...); // crossing
}
typedef struct dict_Eq_Vec { bool (*eq_qu)(int64_t, int64_t); } dict_Eq_Vec;
static dict_Eq_Vec dict_Eq_Vec_singleton = { .eq_qu = __inst_Eq_eq_qu_Vec };
```

Two facts, both confirmed by grep over `vec-of-tvec-eq` and
`data-literal-typed-empty`:

1. **The carrier base is never called.** `__inst_Eq_eq_qu_Vec(` appears only as
   its forward decl, its definition, and the dict-singleton initializer -- there
   is no call site. Concrete dispatch goes to the `__spec`.
2. **The dict singleton is never read.** `dict_Eq_Vec_singleton` appears only at
   its own definition -- no abstract `(defn f [A] [(Eq A)] ...)` consumes it in
   these fixtures.

So the carrier base + dict singleton are **dead code** in every fixture that
only does concrete Vec dispatch. clang DCEs them at `-O2` (no binary cost), but:

- the **emit-c audit** counts the 3 dead casts per fixture (~30 of the 80), and
- emitting them keeps **`emit_carrier_bridge` reachable**, which blocks the M3
  goal of down-scoping/deleting the bridge.

The remaining ~50 `Vec int` crossings are the **live** cases: the synthesized
element-comparator thunks `__fn_N(int64_t, int64_t)` for recursive
`Vec[Vec[int]]` eq (the closure/fat-comparator ABI is uniform int64, so the
thunk casts to `Vec__int *` to call the typed spec), plus the `vec-eq-loop-byval`
carrier base reached through them. Those are a separate, harder sub-problem
(typed closure carriers) -- see "Phase 2".

## Why the dead carrier base survives today

`emit_abi_fn_skip_generic` (`emit_module.c:2195`) already suppresses a generic
function's carrier body once it has been specialized and no carrier call is
observed (`!emit_abi_has_carrier_call`). It does NOT fire for the Eq[Vec]
carrier base because:

- the **dict singleton initializer** `.eq_qu = __inst_Eq_eq_qu_Vec` is a
  genuine C-level reference to the symbol, so even if the body were skipped the
  initializer would dangle (link error). The dict keeps the base alive.

Therefore the dead carrier base cannot be removed in isolation -- the **dict
singleton must be conditionally emitted too**. That is the crux of M4d.

## Phase 1 -- conditional dict + carrier-base emission (the ~30 dead crossings)

Emit the non-HKT instance's int64 carrier base **and** its `dict_X_<args>`
struct + singleton **only when the dict is actually consumed** somewhere in the
program. "Consumed" = at least one of:

- an `EX_DICT` method dispatch whose `instance` is this instance and whose
  receiver type is still abstract at the call (the genuine carrier-dispatch
  path), or
- a **bare-dict-value** use -- the singleton address handed to a constrained
  polymorphic function (`(defn f [A] [^&: Eq A] ...)`), a witness table slot
  (`witnesses[i] = &dict_X_singleton`, emit_expr.c:4656), or any
  `(int64_t)(intptr_t)(&dict_X_singleton)` materialization (emit_expr.c:1427).

When NO such consumer exists, the dict + carrier base are dead; skip both.

### Implementation sketch

1. **Dict-consumption scan** (new, in the emit_abi scan pass, `emit_module.c`):
   walk every item; for each `EX_DICT` dispatch with an abstract receiver and
   each bare-dict-value materialization, mark `inst->dict_consumed = true`
   (new `bool` on `TypeClassInstance`, default false). The two emit sites that
   take the singleton address (1427, 4656) and the abstract-dispatch path are
   the complete set -- the same three forms the M3 audit's "bare dict value"
   note enumerates.
2. **Gate the dict emit** (`emit_stmt.c:399` `EX_INSTANCE_DEF`): when
   `tc->is_hkt == false` and `!inst->dict_consumed`, emit neither the dict
   struct, the singleton, nor force-keep the carrier base. (HKT instances always
   emit -- their dispatch is inherently dict-driven, M6/M7.)
3. **Let `emit_abi_fn_skip_generic` drop the now-unreferenced carrier base.**
   With the singleton gone, the carrier base has no referent and the existing
   skip path (or a small extension recognizing instance-method carrier bases
   whose only caller was the dropped dict) elides it. Verify the forward decl is
   dropped in lockstep (emit_module.c:2390 region).

### Risk + validation for Phase 1

- **Link errors** if the consumption scan misses a consumer -> the dict is
  skipped but something references it. Mitigate by making the scan
  conservative: when in doubt (any abstract-receiver dispatch, any address-of),
  mark consumed. A missed *non*-consumer only leaves a dead dict (status quo);
  a missed consumer is a link error, so bias toward keeping.
- **Separate compilation**: an exported instance's dict may be consumed in
  another TU. Gate Phase 1 to `!ctx->separate_compilation` OR keep the dict for
  exported instances. Confirm with the spice build (ecs/json link across TUs).
- Validation: `bash tests/run.sh` green; `TUR_M3_AUDIT=1` sweep shows the
  ~30 dead-carrier-base `Vec int` crossings gone (and the same for any other
  non-HKT instance whose dict is unconsumed -- this generalizes beyond Vec, so
  expect a broader drop). Snapshot regen for any fixture whose emitted C loses a
  dead dict (could be large -- coordinate one regen window). Spice roundtrip.

Phase 1 is the **safe, high-value increment** and removes the dead crossings for
*every* non-HKT instance, not just Eq[Vec]. It does not need typed dict slots at
all -- it just stops emitting provably-dead carrier dispatch.

## Phase 2 -- typed element-comparator thunks (the ~50 live crossings)

The recursive `Vec[Vec[int]]` eq synthesizes an element comparator
`__fn_N(int64_t a, int64_t b)` that casts `a`/`b` to `Vec__int *` to call the
inner `Eq[Vec]` spec. The cast is forced because the comparator is passed
through the **uniform int64 fat-closure / `^fat val-cmp` ABI** that
`vec-eq-loop-byval` / `vec_hyeq_qu` consume.

Options (a design sub-pass, analogous to the parent plan's M6):

- **2a. Typed comparator parameter.** Give the byval eq helpers a typed
  comparator param (`bool (*)(Vec__int *, Vec__int *)`) per element
  monomorphization, so the synthesized thunk is typed and the cast disappears.
  Rides the existing per-spec machinery; the helper's comparator slot becomes
  part of its spec signature.
- **2b. Accept the thunk cast as the type-erased boundary.** The comparator IS a
  first-class function value crossing a uniform-ABI boundary; per the matrix's
  roadblock 4 the carrier survives at genuine type-erasure. If 2a's per-element
  comparator-typed helper specialization proves too broad, classify these as the
  permanent carrier boundary and down-scope (not delete) the bridge around them.

Default: try 2a for the `Eq`-family helpers (the audit's whole live residual is
Eq-comparator thunks); fall back to 2b if the comparator-typed spec explosion is
worse than the win.

## Phase 3 -- re-audit + bridge down-scope

After Phases 1-2, re-run the `TUR_M3_AUDIT=1` sweep. Target: `Vec int` crossings
-> near 0 (modulo any 2b-classified permanent comparator boundary). At that
point the only `emit_carrier_bridge` callers are the matrix's documented
type-erased boundaries (existential / `@Any` / `tur_poly_fn_t` / blessed
inline-C `tur_ok`), and the bridge can be **down-scoped** (not deleted -- matrix
roadblock 4) to exactly those, per the M3 sequencing.

## Sequencing note

Phase 1 is independent and shippable on its own (it is dead-code elimination, no
ABI change, no typed dict slots). Do it first -- it is the larger crossing win
(generalizes to all non-HKT instances) at the lower risk. Phase 2 is the genuine
ABI change and should follow once Phase 1 quantifies how many live crossings
actually remain.

## Validation harness (all phases)

1. `bash tests/run.sh`: zero new `FAIL`; one coordinated snapshot regen per
   phase that changes shared codegen (Phase 1 may drop dead dicts from many
   snapshots).
2. `bash tests/run-turi.sh`: interpreter parity baseline (1206/2).
3. `TUR_M3_AUDIT=1` per-fixture sweep tracking the `Vec int` count down.
4. Spice roundtrip `../turmeric-spices/spices/{ecs,json}` (ecs is the heavy Vec
   + dispatch user; the cross-TU dict-consumption case lives here).

## Related

- [docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](../../reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
  -- the audit + bucket breakdown this plan's root-2 owns.
- [docs/upcoming/m4-typeclass-per-method-abi-plan.md](../m4-typeclass-per-method-abi-plan.md)
  / [docs/upcoming/m4c-execution-plan.md](../m4c-execution-plan.md) -- M4a-c,
  the per-instantiation typed spec work this builds on.
- [docs/upcoming/v1/vec-typed-pointer-vertical-slice-plan.md](vec-typed-pointer-vertical-slice-plan.md)
  -- the Vec producer slice (root 1) this is the sequel to.
