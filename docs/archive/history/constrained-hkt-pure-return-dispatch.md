---
title: Return-directed methods (`pure`/`empty`) on a constrained abstract type constructor
status: RESOLVED (2026-07-29)
area: compiler (src/compiler/elab_typeclasses.c, src/compiler/elab_fns.c)
---

# `pure` on the abstract `m` of a constrained poly fn

## Symptom

Inside a constrained kind-polymorphic function, receiver-directed class methods
resolved but return-directed ones did not:

    (defn poly [^m] [^Applicative m x : (m int)] : (m int) (ap x x))    ; OK
    (defn poly [^m] [^Applicative m x : (m int)] : (m int) (pure 7))    ; error

    error: no instance 'Applicative tyvar'

Same class, same constraint, same function -- only the dispatch direction
differed. Because nearly every monadic combinator ends in `pure`, this made
`Monad m => ...` express `m a -> m a` transformations but not `a -> m a`
construction, which is most of the point.

## Root cause

Two independent narrownesses, both of which had to be widened:

1. **`elab_typeclasses.c`, return-dispatch abstract-tyvar branch.** When the
   dispatch type bound to an abstract tyvar, the code searched for a
   representative instance with `type_args[0].kind == TY_INT`. That is a
   kind-`*` assumption: a higher-kinded class never has an `int`-headed
   instance, because every `Applicative`/`Monad` instance head is a type
   *constructor*. No representative was found, so it fell through to
   `typeclass_env_lookup_instance` with a tyvar and hard-errored.

   The receiver-directed path (`obj_is_abstract_tyvar` in `elab_method_call`)
   already had the analogous representative-selection logic, which is why `ap`
   and `bind` worked. The fix mirrors it for the higher-kinded case.

2. **`elab_fns.c`, ambient-constraint recording.** The ambient constraint
   (`cur_hkt_constraint_class` / `_tyvar` / `cur_hkt_dict_binding`) was recorded
   only when `n_constraints == 1`. A body needing two classes on one type
   constructor -- `[^Monad m ^Applicative m ...]`, i.e. every bind-then-pure
   combinator -- recorded no ambient at all, so even the widened branch above
   had nothing to key on. Now N constraints are accepted as long as they all
   pin the same higher-kinded type variable.

## Fix

`elab_typeclasses.c`: in the abstract-tyvar return-dispatch branch, when the
bound tyvar names the current body's constraint variable, select the ambient
representative instance (or the first instance of `tc` implementing the method)
as the polymorphic base. Emit-side re-resolution then specializes per
instantiation, exactly as for the kind-`*` representative.

The gate is the *tyvar* match, not class identity: only the first of several
constraints is recorded as ambient, so keying on `cur_hkt_constraint_tyvar`
covers the others. An unconstrained `(pure 42)` with no expected type still gets
the "add a type ascription" diagnostic -- the new path requires an ambient
constraint to fire.

`elab_fns.c`: relax the single-constraint gate to "all constraints pin the same
higher-kinded tyvar".

## Verification

The important property is that this is *not* a static representative: `pure`
must reach the instance the caller chose. `hkt-constrained-pure-two-instances`
compiles one `make` body and instantiates it at two Applicatives whose `pure`
tags its payload differently, then asserts `107` and `207`. A statically bound
representative would print one of them twice -- a silent miscompile, strictly
worse than the error this replaced.

Fixtures:

- `hkt-constrained-pure-return-dispatch` -- `pure` alone (single constraint),
  bind-then-pure (two constraints on one constructor), and `ap` (receiver
  direction, unchanged).
- `hkt-constrained-pure-two-instances` -- caller-chosen instance across a rank-2
  `forall`.

Suite: 2401 passed, 0 failed (2399 before, plus the two new fixtures).

## Correction (2026-07-29, same day)

The claim above holds for `pure` in the poly fn's **own body** (re-resolved per
spec) and in the **dict-passed** rank-2 path (the 107/207 fixture). It does NOT
hold for `pure` inside a **lifted continuation** -- `(bind x (fn [v] (pure ...)))`
-- where the lambda is emitted outside any specialization and keeps the
`Applicative [Schema]` representative. That produces numerically right answers
only because Schema's tag word reads as Option's `is_some`. Resolved same day by Route B (dictionary passing) -- see
[../constrained-hkt-lifted-lambda-keeps-representative-instance.md](../constrained-hkt-lifted-lambda-keeps-representative-instance.md).

## Still open

The by-value carrier restriction is a separate fault and remains open --
[../constrained-hkt-byvalue-carriers.md](../constrained-hkt-byvalue-carriers.md) (since RESOLVED,
with the unary-head limit extracted to
[../constrained-hkt-abstract-var-requires-last-param-free.md](../constrained-hkt-abstract-var-requires-last-param-free.md), since RESOLVED).
The abstract `m` must still be an int-carrier `defopaque`; stdlib `Option` and
`Result` crash through the poly carrier.
