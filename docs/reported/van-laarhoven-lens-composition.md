# van Laarhoven lens composition: adapter-lambda dict capture + rank-2 arg preservation

**Severity:** medium (the remaining blocker to composing van Laarhoven optics with
ordinary function composition; `view`/`set`/`over` and same-focus *delegation*
between lenses already work).

## Summary

`view`/`set`/`over` work end to end (fixtures `van-laarhoven-lens-concrete/`,
`van-laarhoven-lens-generic/`), and a constrained rank-2 lens may now **delegate**
to another at its own abstract functor (fixture `van-laarhoven-lens-delegate/`).
What still does NOT work is **composition**: focusing `Line -> Point -> int` by
chaining `line-a` (`Line -> Point`) and `point-x` (`Point -> int`), the van
Laarhoven `l1 . l2`.

## Gaps and status

The composed lens

```
(defn line-a-x [^f] [^Functor f g : (-> int (f int)) s : Line] : (f Line)
  (line-a (fn [p : Point] : (f Point) (point-x g p)) s))
```

exercised three gaps; the first two are fixed.

### Gap A -- nested-lambda kind recovery (FIXED)

The inner closure annotates its result with the enclosing rank-2 `f : * -> *`. A
nested lambda only saw its own local type params, so `f` resolved kind-`*` and
`(f Point)` tripped TUR-E0012. Fixed by recording each enclosing signature
quantifier's constructor kind (`sig_tyvar_kinds`) and recovering it in
`type_expr_from_form` (fixture `hrt-nested-lambda-enclosing-kind/`).

### Gap B1 -- dictionary forwarding for a DIRECT nested constrained rank-2 call (FIXED)

A constrained rank-2 body that calls another constrained rank-2 fn at its own
abstract functor -- `(defn my-x [^f] [^Functor f g ...] (point-x g s))` -- used to
defer the callee's `Functor f` obligation (the pin is the enclosing abstract `f`,
not a ground type), resolving to the callee's plain carrier base: an arbitrary
hardcoded instance plus a thin fn-pointer call on the fat-boxed `g` -> SIGSEGV.

Fixed: while elaborating a constrained rank-2 fn's body, `Elab` exposes that fn's
single higher-kinded constraint (`cur_hkt_constraint_class`/`_tyvar`). A nested
call whose callee has a single matching `Functor` constraint pinning to that same
abstract functor is redirected to the callee's dict-clone with an **ambient dict**
prepended -- an `EX_DICT{is_ambient}` that lowers to the caller's own dict
parameter (`ctx->dict_dispatch_param_cname`) when the caller is emitted as a
dict-clone, and to the representative instance's singleton in the plain carrier
base. `van-laarhoven-lens-delegate/` shows `my-x` delegating to `point-x` through
both `Const` (view) and `Identity` (over): 3 / 30 / 4.

### Gap B2 -- adapter lambda: dict capture + rank-2 arg preservation (OPEN)

Real composition adapts the focus with an inner lambda
`(fn [p : Point] : (f Point) (point-x g p))` (or the point-free `(point-x g)`)
handed to `line-a`. This still fails:

```
error [TUR-E0001]: function 'line-a' arg 1:
  expected (fn [<adt>] : (type-app ? ?)), got (fn [<adt>] : (type-app ? ?))
;; point-free form: got ptr<void>
```

Two sub-problems remain:

1. **Rank-2 arg-type preservation.** Both `line-a`'s expected `g'` param
   (`(-> Point (f' Point))`) and the adapter's actual result (`(f Point)`) print
   as a def-less `(type-app ? ?)`, so `type_eq`/`call_collect_type_bindings`
   cannot bind `f' := f` and accept the arg. The full applied type `(f Point)` is
   being lost to `type_from_kind(TY_APP)` on one or both sides; it must be
   preserved so the abstract functor threads through the nested constrained
   rank-2 arg (the nested-call analogue of MB4's functor-wrapping-arg handling).
2. **Ambient-dict capture in a lifted lambda.** The adapter lambda's body calls
   `point-x` at the abstract `f`, so it needs the same dict forwarding as Gap B1 --
   but the lambda is lifted to a top-level function and does not receive the
   enclosing dict-clone's dict parameter. The ambient dict must be **captured**
   into the lambda's closure env (and `ctx->dict_dispatch_param_cname` sourced
   from the captured slot inside the lifted body), or the composition must be
   lowered so the adapter stays inside the dict-clone's scope.

## Fix directions (Gap B2)

- Preserve the `(f X)` full type across the adapter lambda's result and
  `line-a`'s `g'` param (stop degrading to `type_from_kind(TY_APP)`), then let the
  existing `call_collect_type_bindings` bind `f' := f` and the rank-2 pass-site
  box the arg as usual.
- Extend the Gap B1 ambient-dict mechanism to closures: when a lifted lambda's
  body needs the ambient dict, capture the enclosing dict parameter into the
  closure env and lower `EX_DICT{is_ambient}` to the captured slot.
- Until this lands, compose by hand at concrete whole types via the shipped
  profunctor-by-record `stdlib/lens.tur` (see docs/guides/lens-guide.md).

## History

Split from the resolved van Laarhoven generic-inference report. Gaps A and B1
landed with their fixtures; this report tracks the remaining Gap B2. See
[docs/archive/van-laarhoven-generic-inference-gap.md](../archive/van-laarhoven-generic-inference-gap.md)
and [docs/upcoming/v1/constrained-hkt-forall-mode-b-plan.md](../upcoming/v1/constrained-hkt-forall-mode-b-plan.md).
