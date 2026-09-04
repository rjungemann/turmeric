---
title: Monomorphization Audit (M1)
category: Planning -- ABI / Codegen rework
description: Phase M1 deliverable of the end-to-end monomorphization plan. Catalogs every site in the compiler and stdlib that depends on the int64 carrier ABI, separates the cases that must move to direct ABI from the genuinely type-erased cases that stay, and flags the hybrid surprises subsequent phases need to keep in mind.
---

# Monomorphization Audit -- M1

This is the M1 deliverable of
[end-to-end-monomorphization-plan.md](../end-to-end-monomorphization-plan.md).
It catalogs every site that currently leans on the int64 carrier ABI,
buckets them by what kind of rework they need (or whether they stay),
and flags the hybrid surprises later phases must keep in mind.

The audit is keyed by code location with the columns the plan asked for:
`passes_through_carrier`, `uses_int64_in_body`, `parametric_struct_field`,
`dispatch_method_arg`, `existential_value`, plus a sixth bucket
(`hybrid_surprises`) for non-obvious load-bearing carrier conventions.

Updated as later phases land.

## 0h. Status snapshot — 2026-06-17 (post-#400 floor: 34 crossings; collection-Eq cascade down-scope COMPLETE)

After #400 (the pure-Turmeric TCO'd `Eq [Vec]` rewrite -- the by-value
`vec-eq-loop` self-tail-call lowered to a goto loop inside the `Vec__int *`
spec) the `TUR_M3_AUDIT=1` per-fixture sweep is **34 crossings / 10 fixtures**
(was 60 after #399), with **zero monomorphic deref-copy crossings**. All 34 are
permanent by-design boundaries:

- **22 `Vec int`** -- live element-comparator thunks `__fn_N(int64_t, int64_t)`
  for nested `Vec[Vec[...]]` eq. The comparator gets its args from `vec-get`
  over the int64 `data[]` buffer, so it casts `(Vec__int *)(intptr_t)` to call
  the typed `Eq [Vec]` spec. A **reinterpret cast** (Vec is `:heap`), not a
  deref-copy. The `Eq [Vec]` **carrier base no longer crosses** post-#400 (it
  delegates to carrier `vec_hylen`/`vec_hyeq_hyloop`). These 22 clear only via
  element-buffer monomorphization (matrix-excluded); the disposition is to
  **accept them** (M4d Phase 2b).
- **10 `Result`/`Option`** -- blessed inline-C `tur_ok`/`tur_some` construction
  (the deliberately blessed boundary; `inline-c-typed-result-option`,
  `decode-bool-carrier-instance-ascription`, etc.).
- **2 `SChan`** -- the type-erased channel path (`generic-relay-aggregate-result`).

The realistic M3 goal -- *delete the bridge from the monomorphic paths, keep it
for the cast / blessed-construction / type-erased boundary* -- is therefore
**met for the non-HKT collection-Eq cascade**. Remaining optional work: M4d
Phase 1 (post-emit dead-static DCE of unconsumed instance bases/dicts) is a
code-size cleanup needing a coordinated snapshot regen, no longer an
audit-crossing win. Full write-up:
`docs/archive/history/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`
("Update 2026-06-17 (post-#400 audit floor)").

## 0g. Status snapshot — 2026-06-16 (root 2: `Eq [Vec]` retired to carrier-based; audit 98 -> 70)

`Eq [Vec]` was the last collection instance on the **by-value direction**
(`vec-len-byval` / `vec-eq-loop-byval` over `(:: x (Vec A))`), so its carrier
base bridged `(Vec__int *)(intptr_t)` back to the typed `*-byval` specs in every
program -- the dominant root-2 `Vec int` `carrier->concrete` deref crossings.
Post-#377 `Vec` is `:heap` (a typed pointer), so `(:: x :int)` is now a pure
pointer->int cast (not the by-value struct widening that #369 found "closed"),
exactly as for `Eq [Cons]`. Rewrote `Eq [Vec]` to delegate to the carrier-based
`vec-eq?` with an element-comparison closure -- byte-identical in shape to
`Eq [Cons]` -- and added the emit support (`emit_var_spec_arg_type` +
concrete-`:heap`-pointer-to-`:int` EX_ASCRIBE/`preserve_ascribe_for_bridge`
routing through `emit_carrier_bridge(CK_CONCRETE, CK_CARRIER)`) so the typed
direct-dispatch spec relabels its `Vec__int *` receiver cleanly (no
`-Wint-conversion`). The now-dead `vec-len-byval` / `vec-eq-loop-byval` twins
were deleted (plan step 5, Vec only). Suite **1653/0** (77 snapshots
regenerated), interpreter gate **1209/2** (documented pre-existing), spice json
**6/6** / ecs **22/30** (8 ecs failures identical on baseline). M3 audit:
**17 fixtures / 70 crossings** (was 98); the residual is by-design carrier
boundaries (`:heap` casts, blessed inline-C construction, the typed
Result/Option-at-dispatch M4 dict-ABI item, MutableMap's still-`:int` producers,
type-erased SChan) -- the old deref-copy root-2 shape is gone. Full write-up:
`docs/archive/history/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`
("Update 2026-06-16").

## 0f. Status snapshot — 2026-06-15 (M5 D.4 COMPLETE -- producer-side bridge count -> 0)

Both `CK_CONCRETE -> CK_CARRIER` producer bridges are now deleted:

- **M4c Path A** (`emit_expr.c`): deleted (commit `f134708`) -- zero producers
  after Option C.
- **EX_ASCRIBE** (`emit_expr.c`): deleted. Extended the Option C twin redirect
  to fire inside instance-method specs (resolve an element-erased bare-`EX_VAR`
  receiver via the active spec's `arg_types[]`), migrated the two pin fixtures
  off the `(:: x :int)` idiom, then deleted the bridge. Emit-c sweep over the
  full suite (incl. `-Xdata-literals`): zero producers.

The remaining `emit_carrier_bridge` callers are all `CK_CARRIER -> CK_CONCRETE`
(accessor-side unbox -- M3's separate target) plus the existential / HKT
carrier producers the plan keeps. Suite green 1636/0, zero snapshot drift.
Pins: `m5-instance-spec-constraint-var` (instance-method-spec redirect),
`m5-spec-body-byval-redirect` (return-position element-type redirect). Full
write-up: `m5-residual-straddle-retirement.md` (session 10) +
`docs/archive/history/m5-exascribe-bridge-d4-blocked-on-redirect-coverage.md`.
**M5's residual-straddle retirement is complete**; M3 is unblocked.

**Next: Vec typed-pointer vertical slice** (M3 sequencing step 2). Execution
plan written 2026-06-15:
`docs/archive/vec-typed-pointer-vertical-slice-plan.md`. Converts the Vec
primitives from the int64 carrier to the matrix-mandated `Vec__A *` typed-pointer
ABI, driving the ~114 `Vec int` carrier-bridge crossings toward 0 and retiring
the by-value-header copy that is a latent mutation miscompile. Baseline re-
verified green (1639/0).

## 0. Status snapshot — 2026-06-14 (post-M5 emit-side follow-up)

Today's landings:

- **M5 elab dispatch fix** (`a301229e`): `is_primitive` lists at
  `elab_typeclasses.c:3640-3647` / `:3675-3679` and the symmetric
  `typeclass.c:354-359` KIND_ARROW iteration learned the sized numeric
  variants (TY_INT8/16/32/64, TY_UINT*, TY_FLOAT32/64); plus
  `typeclass_instance_constraints_satisfied` tentatively accepts a
  TYVAR-substituted constraint (the outer defn's own `(Eq A)` already
  guarantees the instance at every monomorphization site).
- **M5 emit arg-bridge fix** (this session): `find_matched_abi_spec`
  now consults `specialized_call_exprs[]` first and looks the spec up
  by `clone_name`.  The arg-bridge at `emit_expr.c:2587` and the
  call-name resolution at `emit_core.c:1030` now agree on the target
  ABI for constrained-poly defns with parametric receivers (e.g.
  `(defn f [A] [(Eq A)] [x : (Vec A) y : (Vec A)] (eq? x y))`).
  Fixture: `tests/fixtures/m5-constrained-poly-vec-eq/`.

Together these close the report at
`docs/archive/history/m5-constrained-poly-spec-wrong-dispatch-for-parametric-receiver.md`.
The plan's canonical M5 example
`(defn fold-eq [A] [^&: Eq A] [xs : (Vec A) y : A] ...)` shape now
compiles end-to-end.

## 0a. Status snapshot — 2026-06-13 (post-M4c-pre-ext)

Since the last audit refresh (`0506bab2`), the following landed:

- **M4c Path A**: per-instantiation specs for non-HKT typeclass-instance
  methods on parameterized concrete-layout types (Vec, Cons, Tuple2).
  See `docs/archive/m4c-execution-plan.md`.
- **M4c-pre-ext (Cons, Vec)**: stdlib `Eq Cons` and `Eq Vec` rewritten as
  pure-Turmeric recursive/index loops, retiring direct dependence on
  `list-eq?` / `vec-eq?` inline-C helpers at the Eq dispatch site. Helpers
  remain for carrier-ABI consumers. See
  `docs/archive/history/tco-in-abi-specs-for-stdlib-iteration.md`.
- **TCO lifts #1, #2, #3** (`53c23aed` and predecessors): the
  `__tur_tailcall:` label loop now composes with ABI specialization, with
  spec-aware `tco_param_type`, and with typeclass-method-dispatch
  self-calls (Path A's `dict_arg`-annotated direct call).
- **Poly-defn recursive return-type inference** (`b1a782bf`): pass-1
  forward-decl in `elab_toplevel.c` now skips the `[TypeVars]` (and
  optional `[Constraints]`) vector before computing `ret_idx`, so a
  recursive call inside `(defn f [A] [...] : bool ...)` sees the
  declared `:bool` instead of the carrier `TY_INT` default.
- **By-value → carrier let-binding bridge** (`970bd9a6`): the EX_ASCRIBE
  emit handler grew a symmetric CK_CONCRETE → CK_CARRIER widening for
  `(let [xi (:: x :int)] ...)` patterns inside Path A specs, with
  spec-aware cname resolution (`current_abi_specialization->arg_types[i]`
  lookup via `ctx->fn_params`). This was the last blocker for Vec Eq's
  rewrite.
- **Map finding** (`677c0024`): `Eq Map` doesn't get a Path A spec —
  not because of its 2-constraint shape (`[(Eq K) (Eq V)]`), but because
  `(defstruct Map [K V] (carrier :int))` is a **transparent int newtype**
  whose `type_c_name` short-circuits to `"int64_t"` in every
  instantiation, so Path A's `abi_changes` check correctly identifies no
  ABI delta. See `docs/archive/why-path-a-bails-on-map-eq-instance.md`.

Suite: `1564 passed, 86 failed` — identical to the pre-session baseline.

### Counts since last refresh

| Token / marker | Audit baseline | Current |
|---|---|---|
| `emit_carrier_bridge` call sites | 4 | 7 (added 3 EX_ASCRIBE widenings — symmetric direction + Cons + Vec) |
| `(int64_t)(intptr_t)` casts (emit_*) | ~112 | ~88 (counted across emit_*.c) |
| `type_uses_carrier_abi` checks | ~20+ | ~36 (more dispatch-side gates as Path A landed) |
| `expr_emits_byvalue_carrier_abi` checks | 5 | 7 |
| `emit_byvalue_carrier_abi` Binding flag refs | ~6 | 9 |

The bridge call count went **up**, not down — Path A's by-value spec
params surface new sites where the bridge must fire to interface with
remaining carrier-ABI stdlib helpers (`vec-len`, `vec-get`, `vec-eq?`).
M3/M9 still target zero, but the path goes through M2b → M3 → M5 first,
not through M4c alone. M4c carved up the carrier-vs-by-value boundary;
it did not yet retire the bridge.

## 1. Bucket counts

| Bucket | Sites | Disposition |
|---|---|---|
| `passes_through_carrier` | ~20+ call sites in `emit_expr.c`; central bridge in `emit_core.c` | Removed by M2-M7 (route constructors / accessors / dispatch through monomorphized direct ABI) |
| `uses_int64_in_body` | 5 prelude helpers + ~10 stdlib defns | Replaced by `#fx{Construct}`-style emission templates in M2 |
| `parametric_struct_field` | Struct-field-type lowering pass added in 78589845; ~3 emit sites | Stays for value-struct payloads; cleaned up to consume monomorphized ABI in M3 |
| `dispatch_method_arg` | Dict-struct emit in `emit_stmt.c`; per-instance method emit in `emit_module.c`; fatshim in `emit_module.c` | Dict struct already typed per-instance (non-HKT path is codegen-compatible). Fatshim retired in M4-M5. HKT dispatch is M6 / M7. |
| `existential_value` | Pack / open in `emit_expr.c`; `tur_poly_fn_t` plumbing; HAMT int64 path | **Stays.** These are the documented inhabitants of the carrier ABI after M8. |
| `hybrid_surprises` | ~6 spots where carrier convention is load-bearing in non-obvious ways | Read during M4 / M6 design; tracked here so we don't regress |

Aggregate token search across `src/compiler/emit_*.c`:

| Token / marker | Count |
|---|---|
| `(int64_t)(intptr_t)` casts | ~112 |
| `tur_poly_fn_t` references | ~60 (compiler) + 1 (runtime) |
| `type_uses_carrier_abi` checks | ~20+ |
| `expr_emits_byvalue_carrier_abi` checks | 5 |
| `emit_carrier_bridge` calls | 4 (1 definition) |

## 2. passes_through_carrier

Core infrastructure -- the bridge and predicate that decide and effect a
carrier-vs-by-value conversion:

- `src/compiler/emit_core.c:273` -- `type_uses_carrier_abi()` predicate.
- `src/compiler/emit_core.c:2223` -- `emit_carrier_bridge()` definition.
  - `:2234-2242` carrier -> concrete deref via `*(...*)(intptr_t)(...)` (pointer payloads) or bitwise union (inline scalars).
  - `:2244-2258` concrete -> carrier via union or heap spill + `(int64_t)(intptr_t)(&tmp)`.
- `src/compiler/emit_core.c:968-1062` -- return-dispatch path: when a call returns a polymorphic type with no matching spec, return value is lowered to int64 and cast back at the use site. `:1057-1062` is the choose-spec-vs-carrier branch.

Call-site / dispatch boundary lowering (representative -- 20+ total in `emit_expr.c`):

- `src/compiler/emit_expr.c:107` -- check `emit_byvalue_carrier_abi` binding flag.
- `src/compiler/emit_expr.c:129` -- `expr_emits_byvalue_carrier_abi()` reads the binding flag.
- `src/compiler/emit_expr.c:432, 574` -- set the flag on let-bound parameters.
- `src/compiler/emit_expr.c:1698` -- cast struct value to carrier at call site when arg's ABI disagrees.
- `src/compiler/emit_expr.c:1704` -- check the by-value-carrier predicate at the call site.
- `src/compiler/emit_expr.c:1708-1709` -- `emit_carrier_bridge(CK_CONCRETE -> CK_CARRIER)` for call args.
- `src/compiler/emit_expr.c:2329` -- `find_matched_abi_spec()` locates per-call-site ABI specialization.
- `src/compiler/emit_expr.c:2468-2470` -- bridge for dispatch-free specialized call args (CK_CARRIER -> CK_CONCRETE).
- `src/compiler/emit_expr.c:2478` -- accessor (e.g. `tupleN-Nth`, `ok-val`) deref against matched spec.
- `src/compiler/emit_expr.c:2528-2534` -- typeclass method arg bridge (CK_CONCRETE -> CK_CARRIER).
- `src/compiler/emit_expr.c:4074-4076` -- existential pack/open carrier bridge (this one stays; see bucket 5).
- `src/compiler/emit_expr.c:4273` -- CK_CARRIER -> CK_CONCRETE for an
  EX_ASCRIBE casting plain `:int` to a concrete aggregate (e.g.
  `(:: t (Cons int))` where `t` is a raw int param). Added during M4c
  Path A for the Cons recursive-projection case.
- `src/compiler/emit_expr.c:4299` -- CK_CARRIER -> CK_CONCRETE for an
  EX_ASCRIBE casting plain `:int` to a concrete TY_APP. Added during
  M4c-pre-ext for the Cons let-binding case.
- `src/compiler/emit_expr.c:4350` -- CK_CONCRETE -> CK_CARRIER for the
  symmetric case: a by-value spec param ascribed back to `:int` (e.g.
  `(let [xi (:: x :int)] ...)` inside `Eq Vec`'s spec body). Added
  2026-06-13 for the Vec Eq rewrite. Spec-aware: looks up the param
  index in `ctx->fn_params` and pulls the monomorphized type from
  `current_abi_specialization->arg_types[i]` so the spill local is
  declared with the correct `Vec__int`-style C name.

Parameter ABI flag plumbing (sites that set / propagate the per-binding
`emit_byvalue_carrier_abi` flag):

- `src/compiler/emit_fns.c:568, 591` -- set on param emission for ABI specs.
- `src/compiler/emit_fns.c:562-591` -- when a param's declared type uses carrier ABI but the concrete C dispatch signature does not, mark the binding so call sites bridge.

**Disposition:** every site above is gone once M2-M5 route polymorphic
constructors, accessors, and constrained-polymorphic dispatch through
their natural C types. M9 then deletes the bridge / predicate / flag.

## 3. uses_int64_in_body

### 3.1 Prelude helpers (`emit_module.c`)

- `src/compiler/emit_module.c:2169-2198` -- emits `tur_some()`, `tur_ok()`, `tur_err()`, `tur_ok_value()`, `tur_err_value()` into every translation unit. Each is `int64_t in -> int64_t out`, implementing box / unbox via heap allocation.
- `src/compiler/emit_module.c:2163` -- comment: "tur_some/tur_ok flow transparently into the stdlib accessors."

These are the inhabitants the rework retires for the non-existential path
and renames (`tur_box_ok` / `tur_box_err`) in M8 for what remains.

### 3.2 Stdlib polymorphic constructors

| Defn | Location | Body |
|---|---|---|
| `ok`  | `stdlib/result.tur:40`  | `return tur_ok((int64_t)(intptr_t)x);` |
| `err` | `stdlib/result.tur:63`  | `return tur_err((int64_t)(intptr_t)e);` |
| `some` | `stdlib/option.tur:31` | `return tur_some(x);` |
| `throw-error` (macro) | `stdlib/result.tur:347` | expands to `return tur_err(err);` |

### 3.3 Stdlib polymorphic accessors / functor methods

- `stdlib/result.tur:267-268` -- `bimap` uses `tur_ok()` / `tur_err()` on the result fields.
- `stdlib/result.tur:319-320` -- Result Functor `fmap` returns via `tur_ok()` / `tur_err()`.
- `stdlib/option.tur:205` -- Option Functor `fmap` returns via `tur_some()`.
- `stdlib/result.tur:118` -- `ok-val [r : (Result A B)] : A` -- declared as `(.ok-val r)`; bridge in the emitter deref's the carrier slot.
- `stdlib/result.tur:133` -- `err-val [r : (Result A B)] : B` -- same shape.
- `stdlib/pair.tur:42` -- `pair-fst [p : (Pair A B)] : A`.
- `stdlib/pair.tur:56` -- `pair-snd [p : (Pair A B)] : B`.
- `stdlib/vec.tur:67` -- `vec-get [A] ... : A` (inline-C carrier path).

**Disposition:** M2 replaces the constructor bodies with `#fx{Construct}`
emission templates; M3 deletes the bridge that makes the accessors
look-through-carrier work, since after M2 they operate on real
by-value structs.

## 4. parametric_struct_field

This bucket landed in commit 78589845 ("emit: fix typeclass instance
method ABI for value-struct payloads"):

- `src/compiler/types.c` -- `struct_field_c_type()` lowers a parametric field `T` to `T*` when `T` is a concrete value-struct (heap-pointer field).
- `src/compiler/types.h` -- function declaration added in the same commit.
- `src/compiler/emit_expr.c:3792-3794` -- comment: "value-struct, the field slot is a heap pointer (T *) not the inline T value. struct_field_c_type rule that picks the pointer layout."
- `src/compiler/emit_expr.c:3700-3731` -- `EX_MAKE_STRUCT` emission, with field-type lookup and conditional heap boxing.

**Disposition:** the field-pointer lowering is the right shape for
value-struct payloads even under monomorphization (the parametric
struct's *layout* doesn't change between carrier and direct ABI -- only
the ABI of values flowing through its accessors does). M3 keeps the
lowering rule and drops only the accessor's carrier-deref bridge.

## 5. dispatch_method_arg

### 5.1 Dict struct generation (already typed per instance)

- `src/compiler/emit_stmt.c:456-532` -- `emit_typeclass_dict()` already generates per-instance dict structs with typed method-pointer fields.
  - `:469-530` each method slot is a typed function pointer (not a uniform `int64_t (*)(...)`).
  - `:506` return slot type matches the method impl's signature.
  - `:516-528` param slot types are concrete (`const T*` for pass-by-ptr structs; `tur_poly_fn_t` for rank-2 params).
- `src/compiler/emit_stmt.c:535-560` -- dict singleton initializers with method-impl pointers.

**M4-rest dispatch side -- 2026-06-13.**  The corresponding call-site
emit at `emit_expr.c:1685-1793` had two redundant patterns:
  1. The default direct-call path: when `best_method->binding` is
     resolved (the typical case), the call lowers to a direct
     `__inst_X_method(args)` rather than going through the dict at
     all.  Phase H §1 in `elab_typeclasses.c:4042-4051`.  Most fixtures
     hit this path -- zero `dict_X_singleton.method(...)` calls in
     `tests/fixtures/*/expected.c`.
  2. The fallback dispatch path (rare): when `fn_expr` is `EX_DICT`,
     the emitted call wrapped the slot in
     `((ret_t (*)(...))(intptr_t)(dict_X_singleton.method))(args)`.
     The slot was already typed by emit_stmt.c, so the intptr_t round-
     trip was dead weight.  Commit (this) gates direct call on
     `is_direct_dict_dispatch` (gf->kind == EX_DICT &&
     method_name[0] != '\0'), mirroring the Phase E typed-fn-field
     path -- arg casts still apply (a TY_FN arg destined for an
     int64_t slot keeps its fn->int64 cast).

### 5.2 Per-instance method return-type emit (78589845 fix)

- `src/compiler/emit_module.c:1777-1825` -- instance-method return-type emit mirrors `emit_fns.c`. Detects non-spec method whose body produces a by-value struct.
  - `:1782-1825` if `is_instance_method && type_uses_carrier_abi(spec->result_type)` is false, body emits direct struct; else emits `int64_t` slot + spill.

### 5.3 Polymorphic fatshim (the carrier-bound dispatch path)

- `src/compiler/emit_module.c:450-483` -- `emit_poly_fatshim()` generates shims that read int64 dict slots and re-type them: `int64_t *__b = (int64_t *)__e; ... ((R (*)(void *, A0, ...))(intptr_t)__b[1])(...)`.
  - Comment at `:463` -- "Slot 1 holds the method's real (typed) N-ary fn pointer; slot 2 its env. The carrier erases the signature to int64_t (*)(void *, int64_t...); this shim re-types it back."

**Disposition:** the dict struct shape is already correct for M4 (non-HKT
classes). M4's work is removing the fatshim wrapper around it -- call
sites read the typed pointer directly. M5 monomorphizes the
constrained-polymorphic *consumers* (`(defn fold-eq [A] [^&: Eq A] ...)`)
so the dict-typed argument is concretely typed at each callsite. M7
revisits the fatshim for HKT classes when M6's option is picked.

## 6. existential_value -- STAYS

These are the genuinely type-erased inhabitants the carrier ABI will
keep serving after the rework.

### 6.1 Pack / open

- `src/compiler/emit_expr.c:4080-4090` (`EX_EXISTS_PACK`) -- value lowered to int64 in the existential's payload slot.
- `src/compiler/emit_expr.c:4185-4212` (`EX_EXISTS_OPEN`) -- payload unboxed via `void*` dereference.
- `stdlib/existential.tur:39` -- `showable` packs `(pack x (exists [a] [(list Show a)] a))`.
- `stdlib/existential.tur:56` -- `show-it` opens via `(open s [a v] ...)`.

### 6.2 `tur_poly_fn_t` -- first-class polymorphic functions

- `src/compiler/emit_module.c:2426` -- `typedef struct { void *env; int64_t (*fn)(void *, int64_t); } tur_poly_fn_t;`
- `src/compiler/emit_expr.c:420, 569` -- let-bound poly-fn aliases declared as `tur_poly_fn_t`.
- `src/compiler/emit_expr.c:2348-2349` (Phase HRT1) -- `EX_POLY_WRAP` emits `tur_poly_fn_t` struct literal.
- `src/compiler/emit_expr.c:2443` -- `*(tur_poly_fn_t*)(intptr_t)(...)` deref for polymorphic param passing.
- `src/compiler/emit_expr.c:3859-3914` (Phase CCL / HRT4) -- fat closure packed into `tur_poly_fn_t { env, fn }`.
- `src/compiler/elab_fns.c:1290` -- rank-2 polymorphic parameter represented as `tur_poly_fn_t` at the C level.

### 6.3 Heterogeneous HAMT (int64-keyed, int64-valued)

- `src/runtime/hamt.h:139` -- HAMT keyed by `int64_t`; values stored as `void*` (bit-reinterpreted from int64).
- `src/runtime/hamt.h:215-218` -- `tur_hamt_set_eq_o`, `tur_hamt_has_eq_o`, etc.
- `src/runtime/hamt.h:226` -- `tur_hamt_eq_dynamic(int64_t a_handle, int64_t b_handle, int64_t val_cmp)` for heterogeneous equality.
- `stdlib/hamt.tur:43-138` -- `hamt/new` / `hamt/set` / `hamt/get` wrappers (80+ refs).

**Disposition:** all of the above keep their carrier representation
after M8. M8's job is to (a) rename `tur_ok` / `tur_err` to
`tur_box_ok` / `tur_box_err` so it's clear they're the existential's
box, and (b) suppress prelude emission when no user code references them.

#### Carrier-essential helper inventory (Phase 4.1/4.3, 2026-06-18)

Per [`docs/archive/phase4-carrier-helper-inventory.md`](upcoming/v2/phase4-carrier-helper-inventory.md),
the following stdlib helpers walk a heterogeneous HAMT with no element type
available, so they are **carrier-essential** -- they stay inline-C on the
int64 carrier (each now carries a `;;` NOTE in source) and are the legitimate
non-zero crossings the Phase 5 bridge predicate must still permit:

- `stdlib/set.tur` -- `set-eq?` / `set-eq-full` (HAMT iteration via
  `tur_hamt_iter_*`).
- `stdlib/map.tur` -- `map-eq-raw?` / `map-eq-raw-k?` (HAMT iteration).

Everything else the inventory examined is already by-value or is a standalone
public-API helper that does NOT back a typeclass dispatch (the non-HKT `Eq`
instances were migrated to by-value field access in M4c). The remaining
dispatch-backing carrier surface is the **HKT instance method bodies**, which
are gated on the Phase 3.0 element-type threading (see the plan); they are NOT
carrier-essential -- they become by-value once Phase 3 lands.

## 7. hybrid_surprises

Places where the carrier convention is load-bearing in a non-obvious
way. Each later phase needs to be aware of these to avoid regressing.

### 7.1 Prereq 6 synthesized constructor body — RETIRED (2026-06-13)

Commit `61252971` deleted the M2a shape-inference path (the
`__tur_p6_r.is_ok = true; memset(...); __tur_p6_r.ok_val = payload;`
emission).  Stdlib `ok` / `err` / `some` / `none` / `pair` / `tcons-of`
already had explicit `(make-struct ...)` bodies; the normal
EX_MAKE_STRUCT emit at `emit_expr.c:3700-3731` handles the field-by-
field construction including heap-spill for value-struct payload
fields via `struct_field_c_type`'s pointer lowering.

Empirical: disabling the inference branch produced **zero** fixture-
snapshot diffs across the suite (1564/86 baseline), AND cleaner C:

    before:
      Result__User__cstr __tur_p6_r;
      __tur_p6_r.is_ok = true;
      memset(&__tur_p6_r.ok_val, 0, sizeof(__tur_p6_r.ok_val));
      memset(&__tur_p6_r.err_val, 0, sizeof(__tur_p6_r.err_val));
      User *__tur_p6_payload = (User *)malloc(sizeof(User));
      *__tur_p6_payload = __tur_inbox_x;
      __tur_p6_r.ok_val = __tur_p6_payload;
      return __tur_p6_r;

    after:
      User *x = (User *)malloc(sizeof(User)); *x = __tur_inbox_x;
      return (Result__User__cstr){.is_ok = true, .ok_val = x};

The `(default-of B)` for the missing payload slot lowers to the
designated-initializer's implicit zero, eliminating the explicit
memset stanza.

What's left of `emit_fns.c:783-820`: a narrow heap-spill stanza that
sets up `__tur_inbox_X` pointers for any `#fx{Construct}` body that
still casts via `(int64_t)(intptr_t)x` in inline-C.  Stdlib has none
today; the stanza is defensive cover for future user code.

Still open: the orthogonal `m2b_carrier_synth` path
(`emit_fns.c:680-720`) for `#fx{Construct}` defns whose spec emits in
the int64-carrier-return context (typeclass-instance-method
dispatch).  Retired only when M4 reworks dict slots to per-instance
typed pointers.

### 7.2 Instance-method return ABI mismatch (78589845's wrapper)

- `src/compiler/emit_fns.c:862-894` -- for non-spec instance methods whose body produces a by-value struct, emits an `int64_t` slot plus a malloc-copy-cast shim to reconcile.
  - `:867-878` the condition: `is_instance_method && type_uses_carrier_abi(result_type)` true *and* body codegen produces struct -> emit carrier wrapper.

M2 + M4 between them remove the precondition (the constructor body
won't produce a carrier-incompatible struct once `#fx{Construct}` is in
place; the dict won't expect a carrier slot once M4 lands), so this
shim becomes dead and is deleted in M9.

### 7.3 Typed fat-closure vs polymorphic fat-closure split

- `src/compiler/emit_expr.c:2064-2139` -- typed fat-closure layout (slot 0 is typed thunk ptr) vs polymorphic fat-closure (int64_t fn + env in heterogeneous container).
- `src/compiler/emit_expr.c:1895-2023` -- emit branch depends on whether the closure is `^fat` (typed) or captures a rank-2 poly fn (`tur_poly_fn_t` carrier).
- `src/compiler/emit_expr.c:2687-2796` -- fat closure struct emission: polymorphic thunk stored as `(int64_t)(intptr_t)...` carrier; typed thunk stored directly.

M5 / M7 design needs to be aware of this split. The polymorphic-fat
side stays under the carrier ABI (rank-2 polymorphic capture is
type-erasure) -- only the *typed* side needs the monomorphized ABI
flowing through it.

### 7.4 Return-dispatch fallback to carrier

- `src/compiler/emit_core.c:968-1062` -- when a call returns a polymorphic type with no matching ABI spec, the return value is lowered to int64 and cast back at the use site. `:1057-1062` is the spec-vs-carrier choice.

M5 needs the worklist to be exhaustive enough that this fallback never
fires for monomorphizable call sites. Until M5, the fallback is what
keeps unspecialized call sites working; after M5, it should be
unreachable except via the existential / `tur_poly_fn_t` path.

### 7.5 Closure-env layout signaling the carrier convention

- `src/compiler/emit_expr.c:2687-2796` -- the closure env stores `__fn` as `(int64_t)(intptr_t)` when the captured fn is polymorphic. This was a fix-site in `closure-env-layout-for-pass-by-pointer-struct-param-captures` -- the env's layout is load-bearing for the dispatch shim's ABI assumptions.

Note for M5: monomorphizing a constrained-polymorphic consumer means
its captures are typed too; the closure env must store the typed fn
pointer directly, not the int64 carrier.

### 7.6 `tur_apply` shim ABI assumption

- `src/compiler/emit_module.c:450-483` -- poly fatshim reads `int64_t` dict slots and casts to typed fn pointers. Assumes dict.method_i layout is `int64_t (*)(void *, int64_t, ...)`.

After M4 the dict shape is typed per-instance, so this shim's
assumption is wrong for non-HKT classes. M4 either retires the shim
for non-HKT instances or restricts it to the HKT path (M7 decides).

## 8. Open carryover from existing reports

The reports the plan supersedes -- their residue lives in code as
"applies when carrier" branches that disappear with the rework:

- `typeclass-method-parameterized-result-carrier-mismatch` (Prereqs 1-3, FIXED): carrier-bridge logic in `emit_carrier_bridge` and `expr_emits_byvalue_carrier_abi` is dead post-M2+M3.
- `closure-env-layout-for-pass-by-pointer-struct-param-captures` (FIXED): closure env layout decision at `emit_expr.c:2687-2796` is dead for typed-closure path post-M5.
- Prereq 5 (FIXED): return-dispatch wrapper-extract at `emit_core.c:968-1062` is dead post-M5.
- Prereq 6 (RETIRED 2026-06-13): the M2a inference branch at
  `emit_fns.c:783-945` is deleted (commit `61252971`).  Normal
  EX_MAKE_STRUCT emit handles the construction; designated-
  initializer C output replaces the memset-then-assign synth. See
  Section 7.1.
- `polymorphic-ok-in-typeclass-instance-method-with-value-struct-payload.md` (OPEN): lands implicitly with M2 + M4.

M10 re-runs this audit, confirms each of the above is gone (or
documents why it survives), and files new reports for any residual
hybrid surprise.

## 9. Cost-curve quick read

Rough estimates for representative projects -- to be re-measured before
M5 / M7 commit:

- Polymorphic stdlib constructors / accessors with at least one
  monomorphization: ~15 (Result, Option, Pair, Vec, Cons, HAMT helpers).
- Concrete element types used at call sites across the test suite and
  `../turmeric-spices/`: probably 30-60 (int, cstr, several user
  structs, a few opaque handles). Naive N*M for constructors/accessors
  alone: ~450-900 monomorphizations across the whole codebase.
- Non-HKT typeclass instances per class * concrete type: existing
  instance count is already the upper bound (no new clones; M4 just
  re-types existing dict slots).
- HKT combinators (`lift2`, `>>=`, `<$>`, etc.) under option 1 of M6:
  per-`(f, A, B, ...)` clone. ECS spice's `Query` machinery is the
  pressure point; quantify before M6's design pass.

The 10-30% C compile-time regression target from the plan looks
plausible but is not validated yet. M1 deliberately does not commit
to it; M5 must measure.

## 10. Next steps

1. **M2a -- LANDED (2026-06-13) and SUBSEQUENTLY RETIRED.**
   Generalized inference shipped as the `#fx{Construct}`-driven synth
   in `emit_fns.c:607-699`.  Once M2b's `(make-struct ...)` body
   form landed (elaborator side at `elab_structs.c` / `elab_types.c`,
   emit at `emit_expr.c:3700-3731` / `:1138`), stdlib migration to
   explicit make-struct bodies made the inference path dead code; it
   was deleted in commit `61252971`.  See Section 7.1 for the before/
   after C emission diff.
2. **M4c Path A -- LANDED (2026-06-13).** Per-instantiation
   typeclass-instance-method specs for non-HKT instances on
   parameterized concrete-layout types. Vec/Cons/Tuple2 ship with
   `__spec__` clones; dispatch site unboxes int carrier and calls the
   typed spec body directly. Map deliberately not specialized
   (transparent int newtype; see Section 0).
3. **M4c-pre-ext (Cons, Vec) -- LANDED.** `Eq Cons` and `Eq Vec`
   stdlib instances are pure-Turmeric loops over Path A specs. Cons
   uses field-projection recursion; Vec uses a let-bound int alias to
   pass the carrier through `vec-len`/`vec-get` inside the
   `vec-eq-loop` helper. TCO inside the spec turns each into a goto
   loop. The inline-C `list-eq?` / `vec-eq?` helpers remain for
   external carrier-ABI consumers but are no longer the dispatch path.
4. **M4c-pre-ext (Map, MutableMap) -- NOT PURSUED THIS PHASE.** Map
   has no Path A spec (transparent int newtype). MutableMap has a
   real spec and 4 bridge crossings, but its pure-Turmeric rewrite
   requires exposing slot-by-index accessors (`mutmap-cap`,
   `mutmap-slot-tag`, etc.) — larger surface change than warranted
   while the M2b/M3/M5 trunk hasn't landed.
5. **M2b core forms -- LANDED (pre-session).** `(make-struct
   StructName :field val ...)` and `(default-of T)` core forms
   elaborate cleanly (`elab_structs.c` keyword form,
   `elab_types.c:elab_default_of`, EX_MAKE_STRUCT / EX_DEFAULT_OF
   emit).  Stdlib `ok` / `err` / `some` / `none` / `pair` / `tcons-of`
   bodies migrated to make-struct.
6. **M2b M2a-inference retirement -- LANDED (2026-06-13).** Deleted
   the shape-inference synth in `emit_fns.c:783-945` (commit
   `61252971`).  Normal EX_MAKE_STRUCT emit handles the
   construction; 125 lines removed; zero fixture diffs; cleaner C
   output (designated initializer instead of memset-then-assign).
   See Section 7.1.
7. **M2b residual** -- the orthogonal `m2b_carrier_synth` path
   (`emit_fns.c:680-720`) for `#fx{Construct}` defns emitted in the
   int64-carrier-return context (typeclass-instance-method dispatch)
   stays until M4 reworks dict slots.  HKT method bodies
   (`fmap`/`pure`/`ap`/`bind`/`throw-error` for `Option`/`Result`)
   keep their inline-C `tur_ok` / `tur_some` calls for the same
   reason -- they're in carrier-dispatch position.
8. M3 -- delete `emit_carrier_bridge`'s CK_CARRIER -> CK_CONCRETE
   path for accessors once M2b has removed the producers.  The
   symmetric CK_CONCRETE -> CK_CARRIER path (added 2026-06-13 for the
   Vec rewrite) is needed only while M4c-pre-ext stdlib helpers
   straddle the carrier boundary; M5 retires it.

   **M3 deletion is currently blocked.**  The bridge call sites at
   `emit_expr.c:1750, 2511, 2575, 2619, 4273, 4299, 4350` are all
   load-bearing for the present monomorphization shape: M4c Path A
   specs interface with carrier-ABI stdlib helpers (vec-len /
   vec-get / vec-eq?) by bridging both directions at the let-binding
   and call-arg boundaries.  Pulling out the bridge needs HKT-method
   migration AND the dict layout rework first (M4-rest + M5).  The
   audit's earlier framing -- "M3 follows mechanically from M2b" --
   was optimistic; M3 actually rides on M4-rest.
6. M3 -- delete `emit_carrier_bridge`'s CK_CARRIER -> CK_CONCRETE
   path for accessors once M2b has removed the producers. The
   symmetric CK_CONCRETE -> CK_CARRIER path (added 2026-06-13 for the
   Vec rewrite) is needed only while M4c-pre-ext stdlib helpers
   straddle the carrier boundary; M5 retires it.
7. **M4-rest -- LANDED (2026-06-13) by trim, not by rework.**  The
   audit's earlier framing -- "dispatch dict struct is still
   uniform-carrier-shape, the M4 plan's per-method-typed slots is not
   done" -- was wrong.  The dict struct WAS already typed per
   instance method (see Section 5.1 + commit `78589845`).  What was
   missing was the call-site direct-call format for the rare
   `fn_expr == EX_DICT` dispatch case; that landed in
   `emit_expr.c:1707` (new `is_direct_dict_dispatch` flag composed
   with Phase E's typed-fn-field path).  Zero fixture-snapshot diffs
   because the EX_DICT fn_expr branch is itself dormant -- Phase H §1
   (`elab_typeclasses.c:4042`) already routes the typical case
   through `best_method->binding` direct calls.  M4-rest is plumbing
   for a dormant path; the real dict-shape change is M5 (per-call-site
   monomorphization of constrained-polymorphic consumers).
8. M5 -- worklist generalization for constrained-polymorphic defns.
   Read `hybrid_surprises` 7.3 and 7.5 first. The bridge count drops
   to 0 only after M5 retires the residual cross-helper carrier
   straddle introduced by M4c-pre-ext.
9. M6 -- HKT dispatch design pass. Read `hybrid_surprises` 7.3 +
   `existential_value` section; the HKT path may end up sharing the
   `tur_poly_fn_t` carrier rather than getting full per-(f, A)
   monomorphization, depending on binary-size measurements.
10. M8 -- rename `tur_ok` / `tur_err` to `tur_box_ok` / `tur_box_err`;
    gate prelude emission on reachability.
11. M9 -- delete `emit_carrier_bridge`, `expr_emits_byvalue_carrier_abi`,
    `type_uses_carrier_in_dispatch`, the `emit_byvalue_carrier_abi`
    binding flag, and the synthesized-body branch that's now the
    default path.
12. M10 -- re-run this audit; file any new hybrid surprises under
    `docs/reported/`.
