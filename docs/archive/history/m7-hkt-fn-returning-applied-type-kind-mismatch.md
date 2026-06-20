---
title: HKT class method with a fn param returning an applied type `(m b)` fails
  kind-check on instance elaboration (TUR-E0012)
severity: medium (expressiveness hole -- blocks the monadic HKT method shapes:
  Monad `bind`, MonadError, Applicative `ap`; not a miscompile, a hard error;
  reachable today only via the experimental M7 by-value HKT class shapes, but it
  fires regardless of the TUR_M7_HKT flag)
status: RESOLVED 2026-06-19 (kind-threading fix; archived). Layer-4 by-value
  emit for the bind body is a separate follow-on, tracked in
  docs/reported/m7-hkt-bind-body-byvalue-emit.md.
---

> **RESOLVED 2026-06-19.** Root was as diagnosed: during `parse_typeclass_method`
> the class param kinds were not threaded into `type_expr_from_form`, so an HKT
> param `^m` used in an applied position resolved to a `TY_TYVAR` with
> `hkt_kind = KIND_STAR`. That STAR-kinded head, reconstructed by
> `call_instantiate_type` -> `type_app` at the instance call site, tripped
> `kind_of_type_app` (TUR-E0012). Fix: thread `type_param_kinds` through
> `parse_typeclass_method` and build a parallel `eff_kinds` array (class kinds +
> KIND_STAR for the m7-collected method tyvars), passed to the five
> `type_expr_from_form` calls in place of NULL (`elab_typeclasses.c`). The bind
> probe (`docs/upcoming/v2/m7-hkt-probe-bind.tur`) now elaborates and reaches
> codegen. Flag-off suite 1683/0; zero flag-on regressions across a 196-fixture
> typeclass sweep (the lone flag-on failure, `instance-method-return-carrier-bridge`,
> fails identically at the prior commit). The remaining wall -- the by-value
> emit of the bind body (the then-branch `(k (.value ma))` is a call returning
> `(m b)`, not an in-body construct) -- is a distinct emit follow-on, NOT a
> kind-check issue; see docs/reported/m7-hkt-bind-body-byvalue-emit.md.

# HKT method `k : (fn [a] (m b))` kind-mismatches on instance elaboration

## One-line summary

Declaring an HKT class method whose parameter is a *function returning an
applied HKT type* -- e.g. `bind`'s continuation `k : (fn [a] (m b))` -- and
then writing any `definstance` for it fails kind-checking with TUR-E0012
("cannot apply a type of kind '*' as a type constructor"). This blocks the
by-value monadic HKT method shapes (Monad `bind`, MonadError, Applicative `ap`)
for Phase 4.2 of the end-to-end monomorphization plan, even though the Functor
`fmap` shape works end-to-end.

## Repro

`docs/upcoming/v2/m7-hkt-probe-bind.tur` (committed), or minimally:

```turmeric
(defclass KA [^m]
  (f1 [ma : (m a) k : (fn [a] (m b))] : (m b)))
(definstance KA [Option]
  (f1 [ma k] (if (some? ma) (k (.value ma)) (none))))
(defn main [] : int 0)
```

```sh
TUR_M7_HKT=1 ./build/tur run /tmp/k.tur   # and without the flag -- same error
```

Observed (both flag states):

```
error [TUR-E0012]: kind mismatch (TUR-E0012): cannot apply a type of kind '*'
  as a type constructor; expected an arrow kind (* -> * or higher)
```

## Isolation (what does / doesn't trigger it)

- The class **declaration alone** kind-checks fine; the error fires only once a
  `definstance` is elaborated.
- A method whose fn param returns a **bare element** `k : (fn [a] b)` (the
  Functor `fmap` shape, `docs/upcoming/v2/m7-hkt-probe.tur`) does **not**
  trigger it -- that shape works end-to-end.
- A method whose **receiver** is `(m a)` and whose **result** is `(m b)` but
  with **no fn param** does not trigger it.
- So the trigger is specifically a **function parameter whose RETURN type is an
  applied HKT type `(m b)`** (the defining feature of `bind`).

## Root cause (localized, not yet fully traced)

`type_app` (`src/compiler/types.c:2653`) computes its result kind via
`kind_of_type_app` (`src/passes/kind_check.c:182`), which emits TUR-E0012 when
the head type's `hkt_kind` is not an arrow ladder. During `definstance`
elaboration the class param `m` (kind `* -> *`) is substituted by the instance's
constructor (`Option`, a parametric struct of kind `* -> *`) into the method
param types. For the fn-nested occurrence `(fn [a] (m b))`, the rebuilt
`(Option b)` TY_APP ends up with a **head whose `hkt_kind` is `KIND_STAR`**, so
`kind_of_type_app` rejects it.

`elab_subst_class_tyvars` (`elab_typeclasses.c:1585`) only recurses through
TY_TYVAR / TY_APP -- it does **not** descend into TY_FN -- so the fn-nested
`(m b)` is substituted by a *different* type-substitution path (the one walking
TY_FN param/result types during instance method elaboration), and that path
rebuilds the inner TY_APP with a STAR-kinded head. The receiver `(m a)` does not
go through that fn-walking path, which is why only the fn-nested form breaks.

## Proposed fix directions

1. Preferred: make whichever substitution path walks TY_FN preserve the
   substituted head's `hkt_kind` (so a `* -> *` constructor stays `* -> *` after
   substitution), OR rebuild the inner TY_APP via `type_app` with the
   correctly-kinded head. Validate that `kind_of_type_app` then succeeds.
2. Alternatively: teach `elab_subst_class_tyvars` to descend into TY_FN
   param/result types and own the substitution uniformly (so the head-kind is
   set consistently in one place), then route instance method type substitution
   through it.

## Validation

- `TUR_M7_HKT=1 ./build/tur run docs/upcoming/v2/m7-hkt-probe-bind.tur` should
  reach codegen (then exit 21 once the layer-4 emit handles the bind body shape
  -- a separate follow-on: the then-branch `(k (.value ma))` is a by-value
  `(m b)`-returning call, not an in-body construct, so `m7_body_constructs_byvalue`
  must also admit a tail that is a call returning the same `(f b)` family).
- `bash tests/run.sh` stays 1683/0 (no stdlib uses this shape today, so a
  correct fix is inert for the existing suite).
