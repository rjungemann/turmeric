# van Laarhoven lens: generic focus-type inference through a rank-2 argument

**Severity:** medium (blocks an ergonomic generic `view`/`set`/`over`; the
encoding itself works at concrete types).

## Summary

The van Laarhoven lens encoding is expressible on Turmeric's type-erased HRT --
`view`/`set`/`over` work end to end when written per-focus at a concrete `S`/`A`
(see `tests/fixtures/van-laarhoven-lens-concrete/`, which returns 3/30/4/99).
What does NOT yet work is a *single generic* combinator

```
(defn view [S A]
    [l (forall [(f :: * -> *)] [(Functor f)] (-> (-> A (f A)) S (f S)))
     s : S] : A
  (get-const (l (fn [x : A] : (Const A A) (mk-const x)) s)))
```

`(view point-x p)` leaves the focus type `A` abstract -- it prints a bare
`tyvar` (TUR-E0006 "first arg type tyvar") instead of `3`.

## Root cause

Two independent inference gaps sit between the working concrete form and a
generic one:

1. **Outer type params do not bind from a rank-2 (forall-typed) argument.**
   When a generic function `view` takes a poly *value* parameter
   `l : forall f. (-> (-> A (f A)) S (f S))`, the call-site binding collection
   (`call_collect_type_bindings` / `call_type_has_named_tyvar`, `elab_call.c`)
   has **no `TY_FORALL` case** -- it falls to `type_eq`, which fails for two
   distinct foralls, so `view`'s own params `A`/`S` never bind from the passed
   lens's type. Adding a naive `TY_FORALL` case that descends into the forall
   body *does* bind `A`/`S`, but it regresses ~12 existing rank-2 fixtures
   (`currying-rank2-partial`, `forall-dict-*`, `hrt-*`) -- the descent interacts
   badly with how those sites already match forall params. A correct fix needs
   to bind only the *enclosing* generic's tyvars while leaving the forall's own
   bound var (`f`) alone, and to not perturb the existing rank-2 arg-matching
   path.

2. **A direct (non-rank-2-parameter) call to a constrained-HKT lens
   mis-dispatches.** Calling the lens directly -- `(point-x g s)` inlined, e.g.
   from a `view` *macro* rather than through a rank-2 `l` parameter -- compiles
   but returns garbage: the direct-call path does not route `fmap` through the
   caller-chosen functor's dict the way the rank-2-argument path (MB1/MB2's
   dict-clone) does. So the "expand `view` as a macro" workaround for gap (1)
   does not sidestep it.

A third, milder gap: a bare `fmap`/`mk-const` call's result type does not infer
the functor (`(fmap (mk-box 5) f)` reports `(type-app ? ?)`); an explicit
ascription (`(:: ... (Box int))`) or a fixed-index constructor
(`mk-const [A] [x : A] : (Const A A)`) is needed.

## What works today (mode B, this branch)

The compiler advances landed for MB4 make the concrete form work:

- Parametric-instance dict naming: the pass-site `emit_dict_name` now reads the
  source symbol for a partially-applied instance head (`(Const r)`), matching the
  definition site -- previously `dict_Functor_T_...` vs `dict_Functor_Const_...`
  (undeclared-symbol link error).
- Higher-kinded constraint pinning through a *nested* function parameter: the
  `Functor f` dict resolves when `f` appears inside `g : (-> A (f A))` (not just
  a top-level `(f a)` argument).
- Fat-box crossing of the functor-wrapping function `g`: a capturing closure
  (`set`/`over`'s `\a -> Identity (h a)`) now crosses the poly carrier as a
  uniform fat box and is fat-dispatched inside the lens body (previously a thin
  function-pointer call jumped into the closure env -> SIGSEGV).

## Fix directions

- Gap 1: a targeted `TY_FORALL` unification that (a) only fires when a *generic*
  callee has a forall-typed parameter, (b) binds the enclosing generic's tyvars,
  (c) shadows the forall's own bound vars so they don't leak, and (d) preserves
  the existing rank-2 arg-matching for the fixtures that regressed.
- Gap 2: route a direct call to a constrained-HKT poly fn through the same
  dict-resolution the rank-2-argument path uses (MB1's `mb1_dicts` prepend +
  the dict-clone), or reject it with a clear diagnostic instead of
  miscompiling.
- Gap 3: recover a class-method call's result functor from the receiver's
  concrete type when the result is `(f b)` (method-result inference).

Until these land, the shipped lens stays the profunctor-by-record encoding
(`stdlib/lens.tur`); the van Laarhoven form is demonstrated at concrete types by
the fixture above.
