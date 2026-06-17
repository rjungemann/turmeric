---
title: TCO inside ABI specs — unblock pure-Turmeric iteration for stdlib collection Eq instances
category: Planning — ABI / Codegen / Control flow
description: Vec / Map / MutableMap / Set / Cons all currently dispatch through inline-C carrier helpers (`vec-eq?`, `map-eq?`, etc.) because their iteration is opaque-memory and Turmeric's TCO doesn't compose with ABI specialization. Lifting three narrow TCO restrictions lets us write the iteration bodies in pure Turmeric (recursive accumulator pattern → self-tail-call → loop after TCO), and those bodies dispatch via Path A specs to retire the carrier helpers. The user's direction: "this is the time to make breaking changes — get it right now rather than sweeping it under the rug." This plan picks Vec as the first target because it has the simplest iteration shape, then walks through the cascade.
---

# TCO + Path A → pure-Turmeric stdlib collection iteration

## Why now

Path A is fully landed (commits `0fd565fe..935a04b8`). The Cons rewrite
in flight (suite verifying as this plan is written) proves that
**recursive direct-projection bodies work** once the spec
clone-name/signature consistency issue is gated. Cons's recursion uses
the EX_ASCRIBE bridge widening to project `.head`/`.tail` directly.

But the audit (`docs/archive/history/m4-suite-wide-bridge-audit-2026-06-13.txt`)
shows **14 fixtures × ~82 bridge crossings** remain, almost all due to
stdlib's collection-Eq instances (`Vec`, `Map`, `MutableMap`, `Set`)
delegating to inline-C carrier helpers. Cons cleared up because it's
field-projectable; the rest need **iteration** because their state
is opaque heap memory walked by index or by hash bucket.

**The blocker for pure-Turmeric iteration is TCO inside ABI specs**:
the natural shape is

```turmeric
(defn vec-eq-loop [A] [x : (Vec A) y : (Vec A) i : int len : int
                       ^fat cmp : (fn [A A] : bool)] : bool
  (if (= i len) true
      (if (cmp (vec-get x i) (vec-get y i))
          (vec-eq-loop x y (+ i 1) len cmp)   ;; <-- self-tail-call
          false)))
```

Without TCO, deep iteration on 100k-element vecs stack-overflows. With
TCO, this lowers to a loop. The Path A spec for
`vec-eq-loop__spec__Vec__int_…` has by-value `Vec__int` params — and
the existing TCO gate at `emit_fns.c:894` disqualifies it
(`!use_abi_spec`).

## Three TCO restrictions that need to lift

`src/compiler/emit_fns.c`:

### Restriction 1 — `!use_abi_spec` (eligibility gate)

```c
bool tco_eligible = !body_diverges && fd->body->kind != EX_INLINE_C &&
    !(result_kind == TY_NIL && !is_main) && !is_main && !use_abi_spec &&
    tco_params_simple(ctx, e, fd) && tco_mark(ctx, fd, fn_name, fd->body) > 0;
```

**Why it's there:** Conservative — TCO predates per-instantiation
specs. The author wasn't sure backedge param-reassignment composed
with spec body emit.

**Why lifting is safe:** The `__tur_tailcall:` label and the
"reassign params + goto" loop are pure C-level constructs. They don't
care whether the function is a spec or not. The only concern is that
the param C types in the reassignment match the function's declared C
signature — which the spec emit already establishes via
`current_abi_specialization->arg_types`.

**Edit:** Remove `&& !use_abi_spec` from the gate. Suite delta
checked per restriction.

### Restriction 2 — `type_uses_carrier_abi` rejection in `tco_params_simple`

```c
static bool tco_params_simple(EmitCtx *ctx, const Expr *fn_e, FnDef *fd) {
    …
    if (type_uses_carrier_abi(rpty)) return false;
}
```

**Why it's there:** For pre-Path-A defns whose params were typed
`(Vec A)` (TY_APP), `type_uses_carrier_abi` returns true and the
backedge would re-emit the param as int64 — but the original C param
is also int64, so it matched. The restriction was redundant for the
carrier-ABI case but harmless. For non-carrier-ABI cases
(TY_STRUCT-no-tparams), the existing gate passes.

**Why lifting needs care:** Post-Path-A, a TY_APP param like
`(Vec int)` inside a spec emits as `Vec__int` (concrete by-value).
The reassignment `x = new_x;` is a valid C struct assignment IF the
RHS is also `Vec__int`. The Path A field-access override at
`emit_expr.c:3878` already handles the receiver side; the reassign
side needs the same shape.

**Edit:** Read the spec's resolved param C type via
`current_abi_specialization->arg_types[i]` and gate on whether THAT
emits as int64 or as a concrete struct. Concrete struct: keep TCO.
int64 carrier: keep current rejection.

This is a `tco_param_type` rewrite to consult the spec when present:

```c
static Type tco_param_type(EmitCtx *ctx, const Expr *fn_e, FnDef *fd, uint8_t i) {
    if (ctx && ctx->current_abi_specialization
        && i < ctx->current_abi_specialization->n_args) {
        return ctx->current_abi_specialization->arg_types[i];
    }
    /* … existing fn_e logic … */
}
```

### Restriction 3 — `dict_arg` in `tco_is_self_call`

```c
static bool tco_is_self_call(FnDef *fd, const char *fn_cname, const Expr *call) {
    …
    if (call->as.call_.dict_arg) return false;         /* typeclass dispatch */
}
```

**Why it's there:** Pre-Path-A, a `dict_arg`-set call meant the
function call went through the dict singleton's slot via the indirect
`((bool(*)(int64_t, int64_t))(intptr_t)(dict.method))(args)` cast. A
self-call through the dict isn't a "direct" call and can't be backedged.

**Why lifting is safe post-Path-A:** Path A's elab at
`elab_typeclasses.c:4034` resolves the dispatch directly to the
instance method's binding: `out->as.call_.fn_binding =
best_method->binding; out->as.call_.fn_expr = NULL;`. The `dict_arg`
is still set as an annotation but the call IS a direct call — line 60
of `tco_is_self_call` already handles this with the binding identity
fast path.

**Edit:** Remove the `dict_arg` reject. The existing
`fn_binding == fd->binding` check (line 60) suffices.

## Per-restriction validation

Each lift is one commit. Run validation after each:

```sh
# After each lift, run the suite and check no fixture regresses:
bash tests/run.sh > /tmp/m4c_tco_N.txt 2>&1
grep -c '^FAIL ' /tmp/m4c_tco_N.txt    # must be ≤172

# Also probe a self-tail-call inside a spec actually loops:
cat > /tmp/tco_spec_probe.tur <<'EOF'
(defn count-down [A] [x : (Vec A) i : int] : int
  (if (= i 0) 0
      (count-down x (- i 1))))
(defn main [] : int
  (let [v (:: (vec-of) (Vec int))]
    (println (count-down v 100000))))   ;; would stack-overflow without TCO
EOF
./build/tur build /tmp/tco_spec_probe.tur -o /tmp/tco_probe
/tmp/tco_probe    # expect 0, not crash
```

## Vec rewrite (after all three lifts land)

```turmeric
;; M4c-pre-ext (Vec): replace the carrier-helper body with a pure-Turmeric
;; index loop.  Path A specializes vec-eq-loop per element-type; TCO inside
;; the spec turns the self-tail-call into a goto loop.  Both vec-eq? and
;; vec-len? helpers stay (used by external callers and the un-specialized
;; carrier dispatch).
(defn vec-eq-loop [A] [x : (Vec A) y : (Vec A) i : int len : int
                       ^fat cmp : (fn [A A] : bool)] : bool
  (if (= i len) true
      (if (cmp (vec-get x i) (vec-get y i))
          (vec-eq-loop x y (+ i 1) len cmp)
          false)))

(definstance Eq [Vec] [(Eq A)]
  (eq? [x y]
    (and (= (vec-len x) (vec-len y))
         (vec-eq-loop x y 0 (vec-len x) (fn [a b] (eq? a b))))))
```

Validation: `vec-eq-ascribed` and the chain
(`option-of-tvec-eq`, `vec-of-tvec-eq`, `result-of-typed-eq`,
`map-of-tvec-eq`, `set-of-tvec-eq`) reduce bridge crossings to zero
on the Vec layer. The audit count drops from ~60 (Vec-layer crossings)
to <10.

## After Vec: Map / MutableMap / Set

Same shape, harder iteration:
- **Map** uses a HAMT — recursive walk over the trie. The iteration
  primitive `hamt-fold` could be lifted to a TCO'd pure-Turmeric form
  with an accumulator. Larger language work; tracked separately.
- **MutableMap** — **DONE** (see
  `docs/archive/mutmap-int-handle-stand-in-blocks-carrier-retirement.md`).
  MutableMap's blocker turned out to be not iteration but an `:int`
  handle stand-in across its whole API: because `mutmap-new` returned
  `:int`, callers had to ascribe `(:: a (MutableMap int int))` to
  dispatch `.eq?`, forcing the carrier bridge. Retyping the API to honest
  `(MutableMap K V)` handles (mirroring `Eq[Vec]`) dropped `mutmap-eq`
  from 4 crossings to 0, no TCO lift required — the inline-C storage
  iteration core (`mutmap-eq-storage?`) stays, taking the K/V-agnostic
  `ptr<void>` directly.
- **Set** wraps a HAMT — same shape as Map.

Hold Map/Set for a follow-up phase. The Vec landing proved the pattern
and the TCO lifts; the MutableMap landing showed that a collection's
remaining crossings can be a type-hygiene defect rather than an
iteration blocker — always check the handle typing first.

## Risks

- **TCO restriction lifting may surface a real bug** the conservative
  gate was hiding. The probe ensures forward progress; if a fixture
  regresses, the per-restriction commit lets us roll back surgically.
- **Map's HAMT iteration in pure Turmeric is non-trivial**. May require
  an inline-C helper that takes a per-element fat-closure callback —
  still uses carrier ABI but at the element callback level, not the
  whole-map level. Out of scope here.
- **Existing fixtures that rely on the inline-C helpers as such**
  (not via Eq dispatch) keep working — the helpers stay as free
  defns; only the Eq instance body changes.

## Estimated cost

- 3 TCO restriction lifts: ½ session each, validated independently.
- Vec rewrite + fixture regen: ½ session.
- Suite validation + finding doc updates: ½ session.

Total: ~2 sessions if all three TCO lifts work cleanly. If one
surfaces a real issue, expand to 3-4 sessions.

## Related

- `docs/reported/m4-final-state-bridge-still-essential-for-collection-eq.md`
  — the audit this work reduces.
- `docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`
  — the M3 bridge deletion this transitively unblocks.
- `src/compiler/emit_fns.c:5-19` — CF1 TCO docs (existing).
- `src/compiler/emit_fns.c:894` — the eligibility gate.
- `src/compiler/emit_fns.c:35` — `tco_params_simple`.
- `src/compiler/emit_fns.c:53` — `tco_is_self_call`.
