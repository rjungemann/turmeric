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

## Root cause

Not pinpointed. The lambda is lifted to a file-scope `__fn_<N>` and emitted once,
so `ctx->current_abi_specialization` is NULL in its body and
`emit_reresolve_disp_type` cannot fire (it early-returns without an active spec).

There is machinery for re-emitting a lifted closure per specialization --
`emit_subtree_dispatches_on_spec_tyvar` / `emit_call_dispatches_on_spec_tyvar`
(`emit_module.c`), added by constrained-instance-element-dispatch-in-closures.
Relaxing its `dt.kind != TY_TYVAR` gate to walk to the head of a `(m int)` spine
was tried and **had no effect**, so a different mechanism governs whether this
particular lambda is cloned per spec. Find that first.

## Fix directions

1. Determine what decides that the `bind` continuation is emitted once rather
   than per spec. The closure-re-emission predicate above is the obvious
   candidate but demonstrably is not the deciding path here.
2. Once the lambda is cloned per spec, the existing re-resolution should handle
   the body -- `emit_dispatch_tyvar` already understands the higher-kinded
   dispatch position as of the fix above.
3. Alternative: route return-directed methods on an abstract constructor through
   the constraint dictionary (a real slot load) rather than a representative, so
   correctness does not depend on specialization at all. That is how the
   dict-passed path already gets the right answer.
4. Prefer a representative whose layout is widest, or refuse to pick one when
   candidate layouts differ, so a miss degrades to a compile error rather than a
   silent wrong-instance call.

## Related

- [constrained-hkt-byvalue-carriers.md](constrained-hkt-byvalue-carriers.md)
- `docs/archive/history/constrained-hkt-pure-return-dispatch.md` -- the gap-1
  fix, whose representative selection this inherits.
