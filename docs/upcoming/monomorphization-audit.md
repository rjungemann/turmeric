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

## 1. Bucket counts

| Bucket | Sites | Disposition |
|---|---|---|
| `passes_through_carrier` | ~20+ call sites in `emit_expr.c`; central bridge in `emit_core.c` | Removed by M2-M7 (route constructors / accessors / dispatch through monomorphized direct ABI) |
| `uses_int64_in_body` | 5 prelude helpers + ~10 stdlib defns | Replaced by `#{Construct}`-style emission templates in M2 |
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

**Disposition:** M2 replaces the constructor bodies with `#{Construct}`
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

## 7. hybrid_surprises

Places where the carrier convention is load-bearing in a non-obvious
way. Each later phase needs to be aware of these to avoid regressing.

### 7.1 Prereq 6 synthesized constructor body (M2's prototype)

- `src/compiler/emit_fns.c:607-699` -- synthesized body for polymorphic `ok` / `err` when payload is a value-struct.
  - `:624-625` detect `ok` / `err` by name; look up `tur_ok` / `tur_err`.
  - `:640-642` skip synthesis if the spec's return type is int64 (typeclass-instance-method dispatch context).
  - `:643-685` synthesize direct path: heap-allocate the payload copy, construct the by-value Result struct, return directly.
  - `:689` fall through to original inline-C body when return type is `int64_t` (the carrier case).
  - `:635-638` comment documents the gap: instance-method dispatch expects an int64 handle but the synthesized direct-by-value body contradicts it -- the surviving sliver of the open `polymorphic-ok-in-typeclass-instance-method-...` report.

This is the canonical pattern M2 generalizes: detect a polymorphic
stdlib constructor, conditionally emit direct struct construction
instead of the carrier round-trip. M2 replaces the by-name special
case with a `#{Construct}` annotation and makes the direct path the
default rather than the conditional path.

### 7.2 Instance-method return ABI mismatch (78589845's wrapper)

- `src/compiler/emit_fns.c:862-894` -- for non-spec instance methods whose body produces a by-value struct, emits an `int64_t` slot plus a malloc-copy-cast shim to reconcile.
  - `:867-878` the condition: `is_instance_method && type_uses_carrier_abi(result_type)` true *and* body codegen produces struct -> emit carrier wrapper.

M2 + M4 between them remove the precondition (the constructor body
won't produce a carrier-incompatible struct once `#{Construct}` is in
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
- Prereq 6 (PARTIAL): synthesized-body branch in `emit_fns.c:607-699` becomes the *default* path in M2.
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

1. **M2a -- LANDED (2026-06-13).** Generalized `emit_fns.c:607-699` to
   be `#{Construct}`-attribute-driven, with StructDef-based field
   inference (lone `:bool` discriminator + type-param-position matching
   for the payload slot). `ok` / `err` in `stdlib/result.tur` retagged
   `#{Construct}`. The open
   `polymorphic-ok-err-value-struct-payload` fixture passes; zero
   regressions in `tests/run.sh`. The carrier-fallback path is retained
   for the int64-return (typeclass-instance-method dispatch) context.
   `some` / `none` / `pair` / `cons` / `vec-of` stay on inline-C for
   now -- they reach `#{Construct}` via M2b's explicit `make-struct`
   body form (see `docs/upcoming/v2/m2b-make-struct-design.md`).
2. M2b -- introduce `(make-struct StructName :field val ...)` and
   `(default-of T)` core forms; retire the M2a inference path and the
   remaining stdlib inline-C constructor bodies. See M2b design doc.
3. M3 -- delete `emit_carrier_bridge`'s CK_CARRIER -> CK_CONCRETE
   path for accessors once M2b has removed the producers.
3. M4 -- read this audit's `dispatch_method_arg` section; dict shape
   is already correct, work is retiring the poly fatshim wrapper for
   non-HKT classes.
4. M5 -- worklist generalization for constrained-polymorphic defns.
   Read `hybrid_surprises` 7.3 and 7.5 first.
5. M6 -- HKT dispatch design pass. Read `hybrid_surprises` 7.3 +
   `existential_value` section; the HKT path may end up sharing the
   `tur_poly_fn_t` carrier rather than getting full per-(f, A)
   monomorphization, depending on binary-size measurements.
6. M8 -- rename `tur_ok` / `tur_err` to `tur_box_ok` / `tur_box_err`;
   gate prelude emission on reachability.
7. M9 -- delete `emit_carrier_bridge`, `expr_emits_byvalue_carrier_abi`,
   `type_uses_carrier_in_dispatch`, the `emit_byvalue_carrier_abi`
   binding flag, and the synthesized-body branch that's now the
   default path.
8. M10 -- re-run this audit; file any new hybrid surprises under
   `docs/reported/`.
