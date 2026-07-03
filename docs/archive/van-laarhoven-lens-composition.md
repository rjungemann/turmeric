# van Laarhoven lens composition (RESOLVED)

**Status:** RESOLVED. Composing two van Laarhoven lenses with ordinary function
composition works end to end -- `view`/`set`/`over` through a composed lens
(fixture `van-laarhoven-lens-compose/`, returning 7 / 700 / 2 / 0 / 42).

**Severity (was):** medium (the last blocker to composing van Laarhoven optics).

## Summary

A composed lens is a constrained rank-2 body that focuses through an ADAPTER
lambda handed to another constrained rank-2 lens:

```
(defn line-a-x [^f] [^Functor f g : (-> int (f int)) s : Line] : (f Line)
  (line-a (fn [p : Point] : (f Point) (point-x g p)) s))
```

Getting this to compile and run correctly took four fixes, landed across this
work item's commits.

## The gaps

### Gap A -- nested-lambda kind recovery

The adapter's `(f Point)` annotation names the enclosing rank-2 `f : * -> *`. A
nested lambda only saw its own local type params, so `f` resolved kind-`*` and
`(f Point)` tripped TUR-E0012. Fixed by recording each enclosing signature
quantifier's constructor kind (`sig_tyvar_kinds`) and recovering it in
`type_expr_from_form` (fixture `hrt-nested-lambda-enclosing-kind/`).

### Gap B1 -- dictionary forwarding for a DIRECT nested constrained rank-2 call

`(defn my-x [^f] [^Functor f g ...] (point-x g s))` used to defer the nested
`Functor f` obligation (the pin is the enclosing abstract `f`, not a ground type)
and resolve to the callee's plain carrier base -- an arbitrary hardcoded instance
plus a thin fn-pointer call on the fat-boxed `g` -> SIGSEGV. Fixed by exposing the
enclosing fn's single higher-kinded constraint on `Elab`
(`cur_hkt_constraint_class`/`_tyvar`) and redirecting the nested call to the
callee's dict-clone with the enclosing dict forwarded (fixture
`van-laarhoven-lens-delegate/`).

### Gap B2a -- rank-2 arg-type preservation

`line-a`'s expected `g'` param (`(-> Point (f' Point))`) and the adapter's actual
result (`(f Point)`) unify structurally, but the adapter's CONCRETE param full
types are not preserved -- the actual `Point` arg-position arrived as a def-less
kind reconstruction, and `call_collect_type_bindings` failed a spurious
`type_eq(Point-with-def, def-less-Point)` at that concrete position. Fixed: in the
`TY_FN` case, when the actual's arg full type was lost to kind reconstruction and
the expected position carries no named tyvar (nothing to bind) and the kinds
agree, skip it instead of `type_eq`-ing -- the concrete-compat check already ran
at the call's kind level. Full types present on both sides are still compared
strictly. This lets `f' := f` bind and the adapter type-check.

### Gap B2b -- ambient-dict capture in the lifted adapter lambda

The adapter's body calls `point-x` at the abstract `f`, needing the Gap B1 dict
forwarding -- but the adapter is lifted to a top-level function and does not
receive the enclosing dict-clone's dict parameter. Fixed by modeling the ambient
dict as a synthetic in-scope `Binding` (`is_ambient_dict`) created for each
constrained rank-2 fn body. The nested forwarding references it as an ordinary
`EX_VAR`, so the adapter lambda captures it through the normal free-variable
machinery (`collect_free_vars`); `name_for_binding` lowers an uncaptured reference
to the enclosing dict-clone's dict parameter, and the captured value stored in the
closure env is that same dict. So the lifted adapter forwards the caller's ACTUAL
dict -- Const for `view`, Identity for `set`/`over` -- not a hardcoded singleton.

### Gap B2c -- :heap struct fat-box crossing (latent bug exposed)

With the dict threaded, the adapter still segfaulted: its `p : Point` param was
misclassified as a "wide by-value ADT" and dereferenced through a bogus box
pointer, while `line-a` passes `s.a` (a `Point *`) directly. Point is `:heap`
(pointer-carried), so it is a typed pointer, never a wide by-value aggregate.
`type_is_wide_byval_adt` was missing the `def->is_heap` guard its sibling
`adt_app_byval_pass_by_ptr` already has; composition is the first case with a
struct-typed closure param crossing the fat box, exposing the latent bug. Fixed by
adding the guard.

## Fixtures

- `hrt-nested-lambda-enclosing-kind/` -- Gap A (nested `(f X)` annotation).
- `van-laarhoven-lens-delegate/` -- Gap B1 (direct dict forwarding, both functors).
- `van-laarhoven-lens-compose/` -- full composition via view/set/over (this
  resolution): 7 / 700 / 2 / 0 / 42.

## History

Split from the resolved van Laarhoven generic-inference report. See
[docs/archive/van-laarhoven-generic-inference-gap.md](van-laarhoven-generic-inference-gap.md)
and the mode-B plan
[docs/upcoming/v1/constrained-hkt-forall-mode-b-plan.md](../upcoming/v1/constrained-hkt-forall-mode-b-plan.md).
