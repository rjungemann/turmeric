---
title: M4c Path A works for Eq Tuple2 (the M4c-pre'd cases) but cascades into 13 fixture regressions across remaining stdlib Eq instances
category: Codegen / ABI — monomorphization plan refinement
severity: Low (refines `docs/upcoming/m4c-execution-plan.md`). Path A's machinery is proven — `Eq Tuple2` builds end-to-end with per-instantiation spec emission, no carrier bridge, and correct runtime output on `emit-abi-trace`. But applying it broadly without first M4c-pre'ing the remaining stdlib `Eq` instances (Vec / Map / MutableMap / Option / Cons / Set) regresses 13 fixtures.
description: Path A.1 (binding-type-match substitution in `emit_abi_register_call`) and Path A.2 (field-access spec override in `emit_expr.c`'s EX_GET_FIELD case) were implemented and verified. The instance method spec emits cleanly with by-value param signatures; field access projects directly (`(x).e1`); the dispatch site calls the spec without a bridge. **The mechanism works.** The fixture regression is one layer deeper: stdlib `Eq` instances on parameterized types other than the three M4c-pre rewrote (Tuple2/Pair/Result) still delegate to inline-C carrier helpers (`vec-eq?`, `list-eq?`, etc.) whose C bodies hard-cast their args back through int64. When Path A specializes those instance methods to by-value, the call to the carrier helper inside the body passes by-value where int64 is expected — same shape as the M4c-pre blocker, applied to 6 more instances.
status: RESOLVED 2026-06-13 (same session, follow-up turn). The root cause turned out NOT to be the stdlib Eq instance bodies (those would need invasive rewrites of Vec/Map/MutableMap/Set internals that rely on opaque carrier memory). The real fix was extending the carrier bridge in `src/compiler/emit_expr.c:2549-2580` to fire for the general "by-value-carrier param → int64 carrier sink" case (previously gated on `dict_arg != NULL`; now also fires on plain direct calls), plus tightening Path A's substitution gate in `src/compiler/emit_module.c:1037-1055` to skip when the instance's `type_args[0]` is a non-parameterized concrete type (`Eq[int]`, `Eq[bool]`, etc.) and to skip when the call-site binding is itself a concrete primitive. With these two changes, the 11 cascade regressions from the Vec/Map/Cons/Option/Set/MutableMap stdlib instances + the 2 Plan-M5-shape false positives (`cgi-constrained-generic-dispatch`, `typeclass-poly-wrapper-struct-receiver`) all pass. **Final suite: 170 FAIL (172 prior baseline minus the resolved `hamt-delete` transient), zero net regressions.**
---

# Path A cascades into stdlib Eq instances

## What works

Path A.1 + A.2 were implemented and validated against `emit-abi-trace`:

1. `elab_typeclasses.c:4046` populates `abi_bindings` on dispatch
   `EX_CALL` nodes — class var bound to `obj_orig_type` (the receiver's
   resolved type at the call site).
2. `emit_module.c:1037` substitutes `arg_types[i]` from the call site's
   receiver type when the param's generic type matches the instance's
   resolved class-var type. The substitution gate uses
   `fd->owner_instance` (M4b backlink) plus a `type_eq` check against
   `owner_instance->type_args[0]`, so explicitly-annotated params skip.
3. `emit_expr.c`'s `EX_GET_FIELD` case checks
   `ctx->current_abi_specialization->fn->owner_instance` and overrides
   `through_carrier=false` when the spec's `arg_types[i]` is a concrete
   struct (either TY_STRUCT-no-tparams or TY_APP-of-struct-app), via
   `type_extract_struct_app`.

The empirical result for `emit-abi-trace`:

```c
static bool __inst_Eq_eq_qu_Tuple2__spec__bool_Tuple2__int__int_Tuple2__int__int(
    Tuple2__int__int x, Tuple2__int__int y) {
        bool __t23 = __inst_Eq_eq_qu_int((x).e1, (y).e1);  // direct
        if (__t23) {
            __t23 = __inst_Eq_eq_qu_int((x).e2, (y).e2);  // direct
        }
        return __t23;
}
```

and at the dispatch site:

```c
if (__inst_Eq_eq_qu_Tuple2__spec__bool_Tuple2__int__int_Tuple2__int__int(
        t1_882, t2_883)) { … }
```

— no `(int64_t)(intptr_t)(&__tNN)` bridge stanzas. **The Path A
machinery is correct.**

## What breaks the rollout

13 fixtures regressed across the full suite. The 11 that are direct
fallout from Path A (the other 2 are noise — `hamt-delete` transient,
`emit-abi-trace` stderr substring needing fixture update for the new
spec call):

| Fixture | Likely cause |
|---|---|
| `result-of-typed-eq` | Eq Vec body calls `vec-eq?` (int64) |
| `vec-eq-ascribed`, `vec-eq-ascribed-multi` | same |
| `map-of-tvec-eq` | Eq Map body calls `map-eq?` (int64) |
| `mutmap-eq` | Eq MutableMap body calls carrier helper |
| `option-of-tvec-eq` | Eq Option body calls `option-eq?` (int64) |
| `set-of-tvec-eq` | Eq Set body calls `set-eq?` (int64) |
| `typeclass-poly-wrapper-struct-receiver` | Constrained-polymorphic wrapper (M5 trigger) |
| `cgi-constrained-generic-dispatch` | Constrained-polymorphic dispatch (M5 trigger) |

The first 6 are the **same shape as the original M4c-pre blocker**:
stdlib instance methods delegate to inline-C carrier helpers that
expect `int64` args. When Path A specializes the instance method to
by-value param types, the call inside the body to the carrier helper
hits a type mismatch.

The last 2 are the **M5 trigger** the original M4 plan
(`m4-typeclass-per-method-abi-plan.md`'s Risks section) warned about:
constrained polymorphic functions receive their dict as `void *` and
read slots untyped. Path A makes the slot type per-instantiation,
which breaks the `void *` read.

## Empirical confirmation of the cascade

Direct repro of one Vec failure:

```sh
$ ./build/tur build tests/fixtures/result-of-typed-eq/input.tur 2>&1 | head -3
… error: passing 'Vec__int' (aka 'struct Vec__int') to parameter of incompatible type 'int64_t' …
   vec_hyeq_qu(x, y, (int64_t)(intptr_t)(__t27));
…
  static bool vec_hyeq_qu(int64_t v1, int64_t v2, int64_t cmp_fn) { … }
```

`x` is by-value `Vec__int` (Path A's substitution), `vec_hyeq_qu`
expects `int64_t`. The third arg got bridged
(`(int64_t)(intptr_t)(__t27)`), so the bridge machinery is firing — but
the first two are direct param refs, not values that pass through the
bridge-eligibility check (which gates on EX_MAKE_STRUCT / certain
EX_VAR shapes / matched_spec aggregate args). For a bare param of
spec-substituted by-value type, no existing bridge site triggers.

## Recommended sequence to land Path A

The unblock is M4c-pre on the remaining stdlib `Eq` instances — same
shape as the work that already landed for Tuple2/Pair/Result:

| Instance | File | Carrier helper used today |
|---|---|---|
| `Eq Vec` | `stdlib/vec.tur:267` | `vec-eq?` |
| `Eq Cons` | `stdlib/list.tur:165` | `list-eq?` |
| `Eq Map` | `stdlib/map.tur:706` | `map-eq?` |
| `Eq MutableMap` | `stdlib/mutmap.tur:365` | `mutmap-eq?` (or similar) |
| `Eq Option` | `stdlib/option.tur:181` | `option-eq?` |
| `Eq Set` | (locate via `grep -n "definstance Eq.*Set" stdlib/`) | `set-eq?` |

Each rewrite is short: replace the carrier-helper inline-C call with a
direct projection body, e.g.:

```turmeric
;; before
(definstance Eq [Vec] [(Eq A)]
  (eq? [x y] (vec-eq? x y (fn [a b] (= a b)))))

;; after (M4c-pre extension)
(definstance Eq [Vec] [(Eq A)]
  (eq? [x y]
    ;; Compare lengths, then element-by-element via the Eq A constraint.
    (and (= (vec-len x) (vec-len y))
         (vec-all-pairs? x y (fn [a b] (eq? a b))))))
```

(Exact body depends on what's expressible cleanly; Vec is more
involved than Tuple2 because it requires an indexed pairwise comparison.
Map / MutableMap are even more involved — they may need a helper that
itself is ABI-agnostic.)

After M4c-pre extension lands and the suite stays at baseline, re-apply
the Path A patch I built (it's recorded in this report's "What works"
section — three localized edits totaling ~85 lines). Then run the
full suite under `TUR_M3_AUDIT=1` to confirm the bridge has gone dead
on the non-HKT, non-Plan-M5 path — that's the M3 deletion gate.

## Plan M5 trigger

The 2 constrained-generic failures (`cgi-constrained-generic-dispatch`,
`typeclass-poly-wrapper-struct-receiver`) are the M5 work. After
M4c-pre extension and Path A reapplication, these stay broken until
constrained-polymorphic functions get monomorphized per type-arg too —
exactly what Plan M5 in
`docs/upcoming/end-to-end-monomorphization-plan.md` describes.

The clean sequence:
1. M4c-pre extension (1 session) — rewrite 6 stdlib Eq instances.
2. Path A reapplication (~½ session) — three small edits.
3. M5 (2-3 sessions) — constrained generics get per-A specs.
4. Re-audit under `TUR_M3_AUDIT=1` — expect zero crossings.
5. M3 bridge deletion (~½ session) — see
   `docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`.

## What landed this turn

Path A.1 + A.2 implementation and validation against `emit-abi-trace`,
plus this report. The code changes were **reverted** after the
13-fixture cascade showed M4c-pre extension is a prerequisite. The
M4b backlinks (`FnDef.owner_instance`,
`EmitAbiSpecialization.typeclass_inst`) remain in tree (inert) and the
M4c-pre stdlib rewrites for Tuple2/Pair/Result remain in tree
(active).

**Suite at 170 FAIL — pre-existing baseline, no diff.**

## Related

- [docs/upcoming/m4c-execution-plan.md](../upcoming/m4c-execution-plan.md)
- [docs/reported/m4c-stdlib-carrier-helpers-block-dispatch-rewrite.md](m4c-stdlib-carrier-helpers-block-dispatch-rewrite.md)
  — the original M4c-pre resolution; this report extends it.
- [docs/reported/m4c-class-var-erased-at-instance-elab.md](m4c-class-var-erased-at-instance-elab.md)
  — step 2 finding from the prior turn.
- [docs/upcoming/end-to-end-monomorphization-plan.md](../upcoming/end-to-end-monomorphization-plan.md)
  §M5 — the constrained-polymorphism work this transitively triggers.
- `src/compiler/elab_typeclasses.c:4046` — Path A.1 site.
- `src/compiler/emit_module.c:1037` — Path A.1 substitution.
- `src/compiler/emit_expr.c` EX_GET_FIELD — Path A.2 site.
