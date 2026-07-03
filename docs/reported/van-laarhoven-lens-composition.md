# van Laarhoven lens composition: dictionary forwarding through a nested constrained rank-2 call

**Severity:** medium (blocks composing van Laarhoven optics with ordinary
function composition -- the encoding's headline ergonomic; `view`/`set`/`over`
themselves work, concrete and generic).

## Summary

`view`/`set`/`over` work end to end (fixtures `van-laarhoven-lens-concrete/`,
`van-laarhoven-lens-generic/`). What does NOT yet work is **composition**: a lens
that focuses `Line -> Point -> int` by chaining `line-a` (`Line -> Point`) and
`point-x` (`Point -> int`), i.e. the van Laarhoven `l1 . l2`.

Written by hand, a composed lens is a constrained rank-2 body that calls another
constrained rank-2 lens, forwarding its own abstract functor `f`:

```
(defn line-a-x [^f] [^Functor f g : (-> int (f int)) s : Line] : (f Line)
  (line-a (fn [p : Point] : (f Point) (point-x g p)) s))
;; or, point-free: (line-a (point-x g) s)
```

## Two gaps, one fixed

### Gap A -- nested-lambda kind recovery (FIXED)

The inner closure `(fn [p : Point] : (f Point) ...)` annotates its result with the
enclosing rank-2 `f : * -> *`. A nested lambda only sees its own local type
params, so `f` resolved as a fresh kind-`*` variable and `(f Point)` tripped
TUR-E0012 ("cannot apply a type of kind '*' as a type constructor").

Fixed: the enclosing signature now records each quantifier's constructor kind
(`sig_tyvar_kinds`, in lockstep with `sig_tyvars`), and `type_expr_from_form`
recovers a higher kind for a free type name that matches an enclosing signature
tyvar. A nested `(f X)` annotation used *locally* now kind-checks and runs
(fixture `hrt-nested-lambda-enclosing-kind/`, returns 3).

### Gap B -- dictionary forwarding for a nested constrained rank-2 call (OPEN)

With the kind recovered, the call `(line-a innerG s)` fails to elaborate:

```
error [TUR-E0001]: function 'line-a' arg 1:
  expected (fn [<adt>] : (type-app ? ?)), got (fn [<adt>] : (type-app ? ?))
```

and the point-free `(line-a (point-x g) s)` fails as
`expected (fn ...), got ptr<void>` (the partially-applied poly value crosses as a
carrier box).

Root cause: `line-a` is itself a constrained rank-2 poly fn
(`forall f'. Functor f' => (-> Point (f' Point)) -> Line -> (f' Line)`). Calling
it from `line-a-x` unifies its bound `f'` with `line-a-x`'s *own* abstract `f`.
The mode-B constraint machinery (`elab_poly_call`) only discharges a constraint
when its var pins to a **ground** type -- when the pin is a tyvar it explicitly
defers ("resolved via return context; defer", elab_call.c ~5779-5784), because a
concrete instance dict cannot be resolved for an abstract `f`. So no dict is
selected, and the rank-2 arg-vs-forall unification for `g` is left in the
inconsistent `(type-app ? ?)` state that surfaces as TUR-E0001.

What is actually needed: when a constrained rank-2 callee's constraint var pins
to an **enclosing rank-2 quantifier** (not a ground type), forward the enclosing
function's dictionary for that same constraint (the hidden MB1 `Functor f`
carrier parameter `line-a-x` already receives) as the callee's leading dict
argument -- dictionary *forwarding*, as opposed to the concrete-instance
*resolution* MB1/MB2 do today. The rank-2 arg-matching must also accept a
`(-> Point (f Point))` value (closure or partial application) for the callee's
`(-> Point (f' Point))` param under the `f' := f` binding, threading the abstract
functor instead of erasing it to a bare carrier.

## Fix directions (gap B)

- In the MB1 constraint-discharge loop (`elab_poly_call`, elab_call.c ~5707), add
  a branch for `concrete.kind == TY_TYVAR`: if the pinned tyvar names an
  enclosing rank-2 quantifier that carries an in-scope `Functor` dict parameter,
  emit an `EX_DICT`/dict-param reference that forwards that dict rather than
  deferring.
- Accept the rank-2 arg whose result functor is the same abstract `f` (bind
  `f' := f` and keep the arg's `(f Point)` type visible instead of collapsing to
  `ptr<void>` / `(type-app ? ?)`), the nested-call analogue of MB4's
  functor-wrapping-arg handling.
- Until this lands, compose by hand at concrete whole types via the shipped
  profunctor-by-record `stdlib/lens.tur` (see docs/guides/lens-guide.md).

## History

Split from the resolved van Laarhoven generic-inference report; gap A landed with
this report. See
[docs/archive/van-laarhoven-generic-inference-gap.md](../archive/van-laarhoven-generic-inference-gap.md)
and the mode-B plan
[docs/upcoming/v1/constrained-hkt-forall-mode-b-plan.md](../upcoming/v1/constrained-hkt-forall-mode-b-plan.md).
