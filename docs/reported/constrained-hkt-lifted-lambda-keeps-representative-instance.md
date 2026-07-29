---
status: open
severity: high
discovered: 2026-07-29
area: compiler (ABI specialization / lambda lifting)
---

# A lifted continuation keeps the representative instance

## Summary

Inside a constrained kind-polymorphic fn, a class-method call in a **lifted
lambda** -- the continuation of `(bind x (fn [v] (pure ...)))` -- is emitted once,
outside any specialization, and keeps whichever instance elaboration picked as
the representative. With the autoloaded stdlib that is `Applicative [Schema]`,
whose `pure` is `schema/always`.

The enclosing fn's own body is fine: receiver-directed dispatch re-resolves per
spec (fixed 2026-07-29, see
[../archive/history/constrained-hkt-spec-reresolve-hkt-dispatch.md](../archive/history/constrained-hkt-spec-reresolve-hkt-dispatch.md)),
and return-directed `pure` written directly in the body resolves correctly too.
Only the lifted lambda misses.

## Scope

| Shape | Dispatch | Correct? |
|---|---|---|
| Method call in a rank-2 `forall` body (dict-passed) | dict slot load | yes |
| Return-directed `pure` in the poly fn's OWN body | re-resolved per spec | yes |
| Receiver-directed `bind` in a monomorphized spec | re-resolved per spec | yes (fixed) |
| `pure` inside a LIFTED continuation | representative | **no** |

## Repro

    $ cat > /tmp/r.tur <<'EOF'
    (defn bind-then-pure [^m] [^Monad m ^Applicative m x : (m int)] : (m int)
      (bind x (fn [v] (pure (+ v 1)))))
    (defn main [] : int
      (println (unwrap-or (bind-then-pure (some 41)) -1))
      0)
    EOF
    $ ./build/tur run /tmp/r.tur
    42
    $ ./build/tur emit-c /tmp/r.tur | grep -A2 "^static int64_t __fn_"
    static int64_t __fn_1304(int64_t v) {
        __auto_type __ps_53 = (__inst_Applicative_pure_Schema((v) + (INT64_C(1))));

`Applicative [Schema]`'s `pure` is `schema/always`, which mallocs
`{12, value, 0, 0}`. The caller reads that pointer as an `Option`: `is_some`
lands on the tag word `12` (nonzero, so "some") and `value` on the payload. The
`42` is numerically correct **entirely by coincidence**.

Any functor whose layout does not happen to agree with Option's would return
garbage. This is a silent wrong-instance call, not a crash.

## Why it is dangerous for fixtures

A stdout-based fixture cannot see this. `tests/fixtures/hkt-constrained-byvalue-
bind-pure` originally used exactly this shape and passed while dispatching to
Schema; it was rewritten to put `pure` in the fn's own body, which genuinely
emits `__inst_Applicative_pure_Option`. **Do not add a `bind`-then-`pure` fixture
until this is fixed** -- it will pass while being wrong.

## Root cause -- a chain of four links

The lambda is lifted to a file-scope `__fn_<N>` and emitted once, so
`ctx->current_abi_specialization` is NULL in its body and
`emit_reresolve_disp_type` early-returns. Getting it cloned per specialization
means clearing four separate obstacles. Links 1-3 were implemented and verified
individually; **link 4 is the blocker** and the work was reverted because 1-3
without it only mint a clone nothing calls.

1. **The predicate is kind-`*` only.** `emit_call_dispatches_on_spec_tyvar`
   (`emit_module.c`) rejects anything but a bare `TY_TYVAR`. A higher-kinded call
   hands back the `(m int)` spine, so walk to its head first. (One-line change;
   verified.)

2. **The finder cannot see the lambda.** `emit_find_dispatch_spec_closure` only
   matches `EX_CLOSURE`. A NON-capturing continuation is lambda-lifted and packed
   by `EX_POLY_WRAP` around an `EX_VAR` reference, so it must also descend
   `EX_POLY_WRAP` / `EX_FN_TO_FAT` / `EX_POLY_TO_FAT` and match a lifted fn via
   `binding->source_fn_def`. (Verified.)

3. **The gate excludes constrained defns.** The caller runs the finder only when
   `fd->owner_instance` is set -- instance-method bodies. A constrained poly
   `defn` has the identical problem and never reaches it. Relaxing to
   `fd->owner_instance || fd->constraints.n_constraints > 0` admits it. (Verified;
   full suite stayed green at 2404.)

4. **The call site still references the ORIGINAL lambda.** With 1-3 in place a
   per-spec clone IS minted -- `__fn_1304__spec__int64_t_int64_t` appears -- but
   the enclosing spec still emits

       __inst_Monad_bind_Option(..., (tur_poly_fn_t){ NULL,
           (int64_t(*)(void*,int64_t))__poly_1306 })

   where `__poly_1306` is the `make_poly_wrapper` thunk built at ELABORATION time
   around the original `__fn_1304`. The wrapper binding is baked into the
   `EX_POLY_WRAP` node, so cloning the inner fn does not reroute anything; the
   wrapper needs a per-spec twin too.

## Fix directions

1. Land links 1-3 together with a fix for link 4, not before -- on their own they
   emit a dead spec.
2. For link 4, the natural place is the `EX_POLY_WRAP` emit (`emit_expr.c`, where
   `wn = raw_name_for_binding(wrapper_binding)`): inside an active specialization,
   if the wrapped inner fn has a registered per-spec clone, emit a twin wrapper
   forwarding to it. `ensure_aggregate_spill_shim` in `emit_module.c` is the
   existing pattern for minting a wrapper at emit time.
3. **Probably better than all of the above:** route return-directed methods on an
   abstract constructor through the constraint dictionary (a real slot load)
   rather than a representative. Correctness then does not depend on
   specialization at all, and the lifted lambda can capture the dict the way it
   captures anything else. This is already how the dict-passed rank-2 path gets
   the right answer -- see `hkt-constrained-pure-two-instances` (107/207).
4. Independently: prefer a representative whose layout is widest, or refuse to
   pick one when candidate layouts differ, so a miss degrades to a compile error
   rather than a silent wrong-instance call.

## Related

- [constrained-hkt-byvalue-carriers.md](constrained-hkt-byvalue-carriers.md)
- `docs/archive/history/constrained-hkt-pure-return-dispatch.md` -- the gap-1
  fix, whose representative selection this inherits.
