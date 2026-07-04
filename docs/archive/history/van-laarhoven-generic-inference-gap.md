# van Laarhoven lens: generic focus-type inference through a rank-2 argument

**Status:** RESOLVED (gap 1 fixed; gap 2 no longer reproduces). One milder,
independent sub-item survives as an open report -- see
[docs/reported/method-result-functor-inference.md](../reported/method-result-functor-inference.md).

**Severity (was):** medium (blocked an ergonomic generic `view`/`set`/`over`;
the encoding itself always worked at concrete types).

## Summary

The van Laarhoven lens encoding is expressible on Turmeric's type-erased HRT --
`view`/`set`/`over` work end to end when written per-focus at a concrete `S`/`A`
(`tests/fixtures/van-laarhoven-lens-concrete/`, returning 3/30/4/99). What did
NOT work was a *single generic* combinator

```
(defn view [S A]
    [l (forall [(f :: * -> *)] [(Functor f)] (-> (-> A (f A)) S (f S)))
     s : S] : A
  (get-const (l (fn [x : A] : (Const A A) (mk-const x)) s)))
```

`(view point-x p)` left the focus type `A` abstract -- it printed a bare `tyvar`
(TUR-E0006 "first arg type tyvar") instead of `3`.

The generic form now works: `tests/fixtures/van-laarhoven-lens-generic/` defines
generic `view`/`set`/`over` (each with its own `[S A]` outer params) and returns
3/30/4/99, matching the concrete fixture.

## Root cause and fix (gap 1)

**Outer type params did not bind from a rank-2 (forall-typed) argument.** When a
generic function `view` takes a poly *value* parameter
`l : forall f. (-> (-> A (f A)) S (f S))`, the outer tyvar `A` appears *only*
inside the forall-typed parameter. The call-site binding collection
(`call_collect_type_bindings`, `elab_call.c`) had no `TY_FORALL` case -- it fell
to `type_eq`, which fails between two distinct foralls -- so `view`'s own params
`A`/`S` never bound from the passed lens's type, and the result `A` stayed a
tyvar.

**Fix** (`src/compiler/elab_call.c`): a targeted, purely-additive
`call_collect_forall_outer_bindings` at the outer-type-param binding site (the
GS2 full-type comparison in the regular call path). It descends the expected
forall body against the actual argument's (peeled) body, collecting bindings for
the callee's outer tyvars while treating the forall's OWN bound vars (`f`) as
match-anything **wildcards** that never bind. So `(f A)` vs `(g int)` pins
`A := int` without the quantified `f` (whose name need not match the actual's
bound var) polluting or rejecting.

Two properties keep it from regressing the ~12 rank-2 fixtures the original
report worried about (`currying-rank2-partial`, `forall-dict-*`, `hrt-*`):

1. It is **purely additive** -- it only *records* outer-tyvar bindings and never
   sets `arg_ok`. The authoritative arg-type check and the rank-2 `EX_POLY_WRAP`
   machinery run separately, so a *non-generic* callee whose forall param body
   mentions only forall-bound vars (e.g. `use-konst`'s
   `(forall [a] (-> a (-> a a)))`) contributes zero bindings and is untouched.
   (An earlier attempt that gated `arg_ok` on the descent, and shadowed bound
   vars by pre-seeding them to themselves, wrongly rejected `use-konst` /
   `phase-f-poly-concrete` -- `type_eq(tyvar a, tyvar A)` is false when the
   actual's bound-var name differs. The wildcard treatment fixes that.)
2. It is confined to the regular call path's outer-param site; the rank-2
   arg-matching path in `elab_poly_call` keeps using the plain
   `call_collect_type_bindings`, so its existing forall handling is unchanged.

**Codegen follow-on** (`src/compiler/emit_module.c`): once `A` binds and the
generic `view` type-checks, its body's inner lambda `(fn [x : A] : (Const A A)
(mk-const x))` is a lifted, generic-unsafe (tyvar-typed) thunk that crosses the
poly carrier boxed in a fat shim (`EX_FN_TO_FAT`). `emit_abi_carrier_relay_walk`
had no `EX_FN_TO_FAT` case, so it never noted a carrier call on the thunk and
`emit_abi_fn_skip_generic` suppressed the thunk's carrier base -- leaving the fat
box referencing an undeclared `__fn_N`. Added `EX_FN_TO_FAT` (recurse into the
boxed reference) and `EX_FN` (note a lifted-lambda thunk referenced directly as a
value) cases to the relay walk, mirroring the existing `EX_VAR`/`EX_CLOSURE`
handling.

## Gap 2 (direct constrained-HKT call) -- no longer reproduces

The original report's gap 2 -- a *direct* (non-rank-2-parameter) call to a
constrained-HKT lens returning garbage -- no longer reproduces on this branch.
`(get-const (point-x g s))` at `Const` returns 3, and the `Identity` path
(`run-id (point-x g s)`) returns the mapped struct (30/4), for the
carrier-compatible `Const`/`Identity` functors the encoding uses. The MB4
advances that made the concrete fixture work already route the caller-chosen
functor's dict on the direct path for these carrier-compatible opaques.

## Gap 3 (method-result functor inference) -- still open

A bare `fmap`/`mk-const` call's result type still does not infer the functor:
`(fmap (mk-box 5) f)` reports `(type-app ? ?)` and an explicit ascription
(`(:: ... (Box int))`) or a fixed-index constructor (`mk-const [A] [x : A] :
(Const A A)`) is required. This is independent of the generic-lens deliverable
(the lens fixtures use fixed-index constructors) and is tracked as its own open
report: [method-result-functor-inference.md](../reported/method-result-functor-inference.md).

## Fixtures

- `tests/fixtures/van-laarhoven-lens-concrete/` -- per-focus (concrete) form.
- `tests/fixtures/van-laarhoven-lens-generic/` -- generic `view`/`set`/`over`
  (this resolution). Both return 3/30/4/99.
