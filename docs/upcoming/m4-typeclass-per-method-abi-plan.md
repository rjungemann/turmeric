---
title: Plan M4 — Non-HKT typeclass instances switch to per-method ABI
category: Planning -- ABI / Codegen rework
description: Concrete sub-plan for `docs/upcoming/end-to-end-monomorphization-plan.md` §M4 ("Non-HKT typeclass instances switch to per-method ABI"). Unblocks Plan M3 (`docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`).
---

# M4 — non-HKT typeclass instances switch to per-method ABI

## Status 2026-06-17 -- M4a audit refresh

M4a's three deliverables have all landed in earlier turns; remaining
phases (M4b/M4c/M4d) substantively landed in pieces and are tracked
in their now-archived per-piece docs (see *Related*). Concretely:

1. **Cross-reference of dict-slot emits vs. call-site dispatches** --
   dict-slot emit lives at `src/compiler/emit_stmt.c:399`-`574`
   (`EX_INSTANCE_DEF` arm). Call-site dispatch lives at
   `src/compiler/emit_expr.c:1507`-`1524` (`EX_DICT` arm, reading
   `<dict>_singleton.<method>`). Both paths route through
   `emit_dict_name(...)` -- one source of mangling. **No untyped
   `(int64_t)(intptr_t)` casts on the function pointer at the
   non-HKT dispatch site as of #400/#412.**

2. **HKT vs. non-HKT catalogue.** Gate is
   `TypeClass.type_param_kinds[i] != KIND_STAR` (used at
   `emit_module.c:729`; the field doubles as `is_hkt` per the plan
   sketch). Stdlib classification:

   | Non-HKT (per-method ABI) | HKT (uniform carrier) |
   |---|---|
   | `Eq`, `Ord`, `Show`, `Num`, `Clone`, `Hash` | `Functor`, `Applicative`, `Monad` |
   | `HasSchema`, `Display`, `Debug`, `Error` | `Alternative`, `Bifunctor`, `Foldable` |
   | `From`, `Into`, `Serializable`, `MapKey` | `Traversable`, `Comonad`, `MonadError` |
   | `Category`, `Arrow*` (arr is `*`) | |

3. **`EmitAbiSpecialization` extended for instance methods.** Field
   `typeclass_inst` at `emit_internal.h:136` is populated by
   `emit_abi_intern_spec` (`emit_module.c:726`-`737`) for non-HKT
   instance-method specs; consumed by the forward-decl emitter
   (`emit_module.c:2497`) and the spec-body return path
   (`emit_fns.c:506`, `:1092`, `:1172`) so the spec returns the
   concrete result type instead of the int64 carrier. HKT-class
   instance methods deliberately keep `typeclass_inst = NULL`.

### Current bridge audit floor

Direct sweep (this session, all `tests/fixtures/**/input.tur` under
`TUR_M3_AUDIT=1`):

| Direction | Count |
|---|---|
| `carrier->concrete` | 35 |
| `concrete->carrier` | 6 |
| **total** | **41** (11 fixtures) |

By type spine: 26 `Option <T>` (T = int / float / Device / cstr),
15 `Result <T> <E>`. No `Tuple2 int int`, no plain struct, no
`Vec`/`Map`/`Set`-style heap receiver -- the heap-receiver
fast-path (`emit_core.c:2492`-onwards) keeps those off the bridge.

Floor moved from "post-#400: 34 / 10" to **41 / 11** because of
**new by-design regression coverage**, not regression:

- PR #414 added `option-byvalue-param-none-safe` (4) and
  `option-control-form-construct` (4) -- new regression coverage
  for the carrier `#{Construct}` -> by-value `(Option A)` param
  path the PR itself enabled. The bridge MUST fire here by design.
- PR #415/#416 added `tail-call-inline-c-carrier-bridge` (5) and
  `result-bridge-tail-call-to-inline-c` (2) -- regression coverage
  for the tail-call-to-inline-C bridge those PRs added.

### Per-fixture classification (M4a deliverable)

Bucket A' -- regression coverage for the carrier bridge **by design**
(34 crossings / 7 fixtures): `tail-call-inline-c-carrier-bridge`,
`result-bridge-tail-call-to-inline-c`, `inline-c-typed-result-option`,
`eq-carrier-capturing-comparator`,
`decode-bool-carrier-instance-ascription`,
`instance-method-return-carrier-bridge`,
`typeclass-method-parameterized-result-decode`. These fixtures
exist *to* exercise the bridge; deletion would regress them.

Bucket B -- Option-construct-at-carrier-boundary regression
fixtures (12 crossings / 3 fixtures): `option-basic`,
`option-control-form-construct`, `option-byvalue-param-none-safe`.
The bridge fires because the test deliberately mixes the carrier
construction surface (`(some X)` / `(none)`) with by-value
`(Option A)` consumers -- this is the PR #414 codepath under test.

Bucket C -- tractable cascade, tracked elsewhere (8 crossings /
1 fixture): `option-consumers-byvalue-arg`. Tracked in
[docs/reported/option-consumer-retype-byvalue.md](../reported/option-consumer-retype-byvalue.md).
Cascade-coupled retypes (`refined.tur`'s `ne-from?`/`bidx-of?`
returning carrier `:int` Option, plus `kleisli.tur`'s `comp` /
`k-apply-raw` threading the carrier int64) gate retyping
`some?`/`unwrap-or`; `construct_recovered_byvalue`
generalisation gates `option-map`/`result-map`.

**Net:** the only tractable residual on the floor is bucket C
(8 crossings), and the sequencing is already captured in the
existing Track-A report. Buckets A' and B are by-design
boundaries the bridge is **kept** for, per the now-archived
[m4-final-state-bridge-still-essential-for-collection-eq](../archive/m4-final-state-bridge-still-essential-for-collection-eq.md).

### Next concrete work

- **Track A residual:** drive bucket C via
  `docs/reported/option-consumer-retype-byvalue.md` (the
  `construct_recovered_byvalue` non-instance-generic-spec
  generalisation + `refined.tur` retype).
- **Track A north-star:** Plan M5 (constrained polymorphic
  functions over a dict argument) -- the natural next phase
  after M4's per-method ABI infrastructure landed.
- **HKT extension (M6/M7):** Path A's mechanism extends to
  `Functor`/`Monad`/etc. once the M6 design pass picks the HKT
  dispatch shape.

The "M4 -- 4 sessions" estimate in
[end-to-end-monomorphization-plan.md](end-to-end-monomorphization-plan.md)
§M4 is now stale; the bulk landed across PRs #399, #400, #402,
#405, #407, #411, #412, #414, #415, #416. Treat M4 as
substantively done; the open work is bucket C plus the M5/M6/M7
sequel phases.

## Original plan (preserved for reference)

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
- [docs/archive/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](../archive/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
  — the original Plan M3 deletion gate (wholesale-deletion goal retired).
- [docs/archive/m4-final-state-bridge-still-essential-for-collection-eq.md](../archive/m4-final-state-bridge-still-essential-for-collection-eq.md)
  — final-state writeup; bridge stays for by-design boundaries.
- [docs/archive/m4c-execution-plan.md](../archive/m4c-execution-plan.md)
  — M4c execution paper trail.
- [docs/reported/option-consumer-retype-byvalue.md](../reported/option-consumer-retype-byvalue.md)
  — bucket C residual (8 crossings).
- [docs/reported/m2b-stdlib-migration-blocked-on-carrier-fallback.md](../reported/m2b-stdlib-migration-blocked-on-carrier-fallback.md)
  — the M2b finding that first pointed at M4 as the deeper dependency.
