---
title: Plan M4 — Non-HKT typeclass instances switch to per-method ABI
category: Planning -- ABI / Codegen rework
description: Concrete sub-plan for `docs/upcoming/end-to-end-monomorphization-plan.md` §M4 ("Non-HKT typeclass instances switch to per-method ABI"). Unblocks Plan M3 (`docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`).
---

# M4 — non-HKT typeclass instances switch to per-method ABI

## Current state (audit, this session)

The dict struct for a non-HKT instance like `Eq[int]` already carries
**typed** function pointers:

```c
typedef struct dict_Eq_int {
    bool (*eq_qu)(int64_t, int64_t);
} dict_Eq_int;
```

So does `Eq[bool]`, `Eq[cstr]`, `Eq[float]`. These work because the type-arg
is a primitive — `type_c_name` resolves to a single concrete C name.

But for **parameterized** types — `Eq[Tuple2]`, `Eq[Option]`, `Eq[Result]` —
the dict struct collapses to the carrier:

```c
typedef struct dict_Eq_Tuple2 {
    bool (*eq_qu)(int64_t, int64_t);
} dict_Eq_Tuple2;
```

and the matching instance method is also carrier-shaped:

```c
static bool __inst_Eq_eq_qu_Tuple2(int64_t x, int64_t y) { … }
```

The reason: at definstance elaboration time, the type-arg is `(Tuple2 A B)`
(TY_APP with abstract TYVARs), and `type_c_name(TY_APP)` falls back to
`int64_t` whenever any spine arg is unresolved.

**This is exactly the carrier ABI the carrier-bridge** (`emit_carrier_bridge`)
**exists to bridge.** At a call site like `__inst_Eq_eq_qu_Tuple2(t1, t2)`
where `t1`/`t2` are by-value `Tuple2__int__int`, the bridge spills each to a
local and passes its address as `int64_t`. Removing the bridge regresses
exactly these call sites — confirmed empirically in
`docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`.

## Audit data

Direct `TUR_M3_AUDIT=1` runs on the four fixtures that regress when the
bridge is deleted:

| Fixture | Bridge crossings |
|---|---|
| `typeclass-return-dispatch-result-wrapped` | 2 × `carrier→concrete` (Result int cstr, Result cstr cstr) |
| `typeclass-method-parameterized-result-decode` | 1 × `carrier→concrete` (Result int cstr) |
| `emit-abi-trace` | 2 × `concrete→carrier` (Tuple2 int int) |
| `polymorphic-ok-err-value-struct-payload` | 0 (was a transient — M2b's prereq-6 path now covers it) |

Both directions are typeclass-dispatch related. The same fixtures + plus
~3-5 more parametric Result/Option/Tuple-using fixtures will need fixing.

## Design

### Per-call-site monomorphization of instance methods

Today: `__inst_Eq_eq_qu_Tuple2` is emitted **once** with int64 ABI.

After M4: `__inst_Eq_eq_qu_Tuple2__spec__Tuple2__int__int` is emitted
**per type-arg-tuple** seen at call sites — same shape as the existing
`emit_abi_intern_spec` infrastructure that monomorphizes regular defns.
The instance method's `FnDef` lives on `TypeClassInstance.method_impls[i]`;
it routes through the same `EmitAbiSpecialization` worklist as ordinary
defns, with the spec's `arg_types` driven by the call-site dispatch's
deduced bindings.

### Per-instantiation dict singletons

Today: `dict_Eq_Tuple2_singleton` is one global per definstance.

After M4: emit one `dict_Eq_Tuple2__int__int_singleton` per concrete type-arg
tuple observed at call sites. Each singleton's struct type carries the
matching typed function pointers. The call-site dispatch picks the singleton
by mangling the receiver's resolved type.

### Dispatch site rewrite

Today (in `emit_expr.c`'s call path):

```c
((bool(*)(int64_t, int64_t))(intptr_t)dict_Eq_Tuple2_singleton.eq_qu)(
    (int64_t)(intptr_t)&t1, (int64_t)(intptr_t)&t2)
```

After M4:

```c
dict_Eq_Tuple2__int__int_singleton.eq_qu(t1, t2)
```

No cast on the function pointer; no `&` on the args; no bridge needed at
the call site.

## Phases

### M4a — audit + ABI-binding infrastructure (1 session)

1. Cross-reference every dict-slot emit in `emit_stmt.c` (around line 451)
   with every call-site dispatch in `emit_expr.c` (search for
   `dict_arg`, `singleton`, the cast pattern above).
2. Catalog HKT vs non-HKT classes — the plan punts HKT to M6/M7, so
   `Functor`, `Applicative`, `Monad`, `Alternative`, `Bifunctor`,
   `Traversable` keep the carrier dict.
3. Extend `EmitAbiSpecialization` (or reuse it as-is) to cover instance
   methods — needs a flag distinguishing `__inst_*` clones from
   ordinary spec clones so the worklist scanner doesn't double-emit
   the carrier version.

### M4b — per-instantiation instance-method emit (1 session)

1. In `emit_module.c`'s spec-emission loop, when the spec's `fn->binding`
   is a typeclass-instance method on a non-HKT class, emit the spec as
   `__inst_<Class>_<method>__spec__<type-arg-mangle>` with typed C
   signature derived from `spec->arg_types` / `spec->result_type`.
2. The dispatch site (instance-method call through the dict) generates
   the spec request via the same path defns use.

### M4c — per-instantiation dict singleton + dispatch rewrite (1 session)

1. In `emit_stmt.c`'s dict-emit loop, emit one dict + singleton per
   observed instantiation rather than one per definstance. Worklist
   driven by the M4b call-site instantiations.
2. Rewrite the dispatch site in `emit_expr.c` to drop the carrier cast
   and the `&`-then-`int64_t` arg-bridging for non-HKT classes.

### M4d — bridge deletion + fixture regen (1 session)

1. Run full suite under `TUR_M3_AUDIT=1`. Expected: zero crossings
   *outside* HKT-class call sites.
2. Delete `emit_carrier_bridge`, `CarrierKind` enum, the 4 call sites
   in `emit_expr.c`, `tests/compiler/test_emit_carrier_bridge.c`, and
   the `tur_codegen_carrier_bridge` CMake target.
3. Regen fixtures (expected ~50-200 `expected.c` updates as dict struct
   names change from `dict_Eq_Tuple2` to `dict_Eq_Tuple2__int__int` and
   the `(int64_t)(intptr_t)` casts disappear).
4. Suite must stay at the 172 FAIL baseline.

## Risks / open questions

- **HKT carve-out.** The plan defers HKT to M6/M7 because the dispatch
  for `Functor f` over a generic combinator's `(f<A>)` requires knowing
  `f`'s concrete shape, which is a harder unification. The exact split
  ("non-HKT" vs "HKT") is well-defined in the codebase via the
  `TypeClass.is_hkt` field — keep that as the gate.
- **Existential-packed values** still want the carrier (the whole point
  of `pack` is to erase the concrete type). M4d must not delete the
  bridge if any non-typeclass call site still crosses; the audit catches
  that.
- **Constrained polymorphic functions** (`(defn fold-eq [A] [^&: Eq A] …)`)
  receive the dict via `void *` today; that path becomes ill-typed once
  dict structs vary per instantiation. **This is exactly Plan M5** — it's
  triggered by M4 and must land alongside or shortly after. Confirm M5
  is in scope of the same delivery window.

## Estimated cost

4 sessions ± 1, mostly in M4b/M4c. M4a is rote audit work; M4d is fixture
regen.

## Validation harness

- Per-phase: full suite at baseline 172 FAIL.
- After M4d: full suite stays at baseline, audit reports zero non-HKT
  bridge crossings.
- Spice-side: rerun `../turmeric-spices/spices/json` (heavy Result use,
  exercises the parametric carrier→by-value path).

## Related

- [docs/upcoming/end-to-end-monomorphization-plan.md](end-to-end-monomorphization-plan.md)
  §M4, §M5, §M6, §M7 — the parent plan.
- [docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](../reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
  — the blocked Plan M3 this design unblocks.
- [docs/reported/m2b-stdlib-migration-blocked-on-carrier-fallback.md](../reported/m2b-stdlib-migration-blocked-on-carrier-fallback.md)
  — the M2b finding that first pointed at M4 as the deeper dependency.
