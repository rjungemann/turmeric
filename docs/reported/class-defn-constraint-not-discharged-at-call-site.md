---
title: A `^Class T` defn constraint is not re-discharged at the instantiating call site
category: Type checking -- typeclass constraint discharge (elab_call / instantiation)
severity: Medium. A `defn` carrying a typeclass constraint (`^Encode T`, or the
  three-vec `[T] [(Encode T)] [..]` form) is checked abstractly inside its own
  body, but when it is CALLED at a concrete type that has NO matching instance,
  the obligation is never re-checked. The error is deferred to emit / link time
  (or silently produces a wrong dispatch) instead of a clean call-site type
  error. Surfaces in the http-handler typeclass work: `json-ok` / `json-request`
  are `^Encode T`-constrained defns, and calling them at a type with no `Encode`
  instance is not flagged at the call site.
status: OPEN
---

# `^Class T` defn constraints are not discharged at call sites

## One-line summary

Constraints on a `defn` (`^Encode T`, `(Encode T)` in the middle vector) are
checked *abstractly* in the body -- you may call the class's methods on `T`
because `T` is assumed to have the instance -- but that assumption is **never
re-discharged** when the defn is instantiated at a concrete type at a call
site. Calling such a defn at a type with no instance is not a compile error.

## Repro (verified on this tree, tur 0.21.0)

```turmeric
(defclass HasFlag [W] (flag-of [^borrow w] : int))
(defopaque HandleHas    :int)
(defopaque HandleNoInst :int)
(definstance HasFlag [HandleHas] (flag-of [w] (:: w :int)))

;; show-it requires (HasFlag W).
(defn show-it [W] [(HasFlag W)] [^borrow w : W] : int
  (flag-of w))

(defn main [] : int
  (let [bad (:: 7 :HandleNoInst)]   ;; HandleNoInst has NO HasFlag instance
    (show-it bad)))                  ;; expected: call-site constraint error
```

```
$ ./build/tur check no-instance-callsite.tur
$ echo $?
0                                    ;; expected: error -- no HasFlag[HandleNoInst]
```

The constraint IS enforced *abstractly* in the body -- drop the `(HasFlag W)`
declaration and the body itself is rejected, proving the constraint is what
licenses the method call:

```turmeric
(defclass HasFlag [W] (flag-of [^borrow w] : int))
(defn show-it [W] [^borrow w : W] : int
  (flag-of w))                      ;; no constraint declared
;; => error: no typeclass method found for 'flag-of'   (exit 1)
```

So the abstract side works; the missing piece is the *concrete* re-discharge
at the instantiating call site.

## Root cause (file:line)

Constraints are parsed and stored on the FnDef:

- `src/compiler/elab_fns.c:940-978` -- collect `(Class TyVar)` forms from the
  optional middle vector ("Gap H item 1").
- `src/compiler/elab_fns.c:3191-3193` -- store them into the FnDef:
  ```c
  fd->constraints.constraints   = constraint_list;
  fd->constraints.n_constraints = n_constraints;
  fd->constraints.cap_constraints = n_constraints;
  ```
- `src/compiler/expr.h:429` (`FnDef.constraints`), `typeclass.h:192-210`
  (`TypeConstraint` / `ConstraintSet`).

A ready-made checker exists but is dead code:

- `src/compiler/typeclass.c:257-266`
  ```c
  bool constraint_set_satisfied(const ConstraintSet *cs, Type type,
                                const TypeClassEnv *env) {
      for (uint8_t i = 0; i < cs->n_constraints; i++) {
          TypeClassInstance *inst = typeclass_env_lookup_instance(
              env, cs->constraints[i].typeclass, &type, 1);
          if (!inst) return false;
      }
      return true;
  }
  ```
  `grep -rn constraint_set_satisfied src/` returns only the definition
  (`typeclass.c:257`) and its prototype (`typeclass.h:222`) -- **zero call
  sites**.

The call-elaboration paths that instantiate a generic defn at concrete types
never invoke it:

- `src/compiler/elab_call.c` `elab_call_fn` (~`:2531-2895`) -- has
  `fn_binding` (hence the FnDef and its `constraints`) but does no constraint
  check before returning the call expr.
- `src/compiler/elab_call.c` `elab_poly_call` (~`:4365-4476`) -- same gap for
  rank-2 polymorphic calls.

Instance-level constraints (the `Clone[Pair a b] => [Clone a, Clone b]` kind)
ARE discharged -- `typeclass_instance_constraints_satisfied`
(`typeclass.c:284-322`), called from typeclass dispatch -- but that is a
different layer from a constrained *defn*'s obligations.

## Fix directions

1. At the call site, once the constrained defn's type parameters are
   instantiated to concrete types, walk `fd->constraints` and call the
   existing `constraint_set_satisfied` (`typeclass.c:257`) with each
   constraint's resolved concrete `Type`; emit a `TUR-E*` "no instance
   `<Class> <Type>` for constrained call to `<fn>`" on failure. Hook it into
   `elab_call_fn` (`elab_call.c:~2531`) and `elab_poly_call`
   (`elab_call.c:~4365`).
2. `constraint_set_satisfied` only takes a single `Type`; a multi-tyvar /
   multi-constraint defn needs each `TypeConstraint.tyvar` mapped to its
   instantiated type first (the `param_idx` / `tyvar` fields on
   `TypeConstraint`, `typeclass.h:192-203`, exist for this). May warrant a
   small `constraint_set_satisfied_subst(cs, subst, env)` variant.
3. Add fixtures: a negative case (`constrained-defn-callsite-no-instance-rejected/`)
   and a positive control where the instance exists and the call type-checks.

## Notes / scope

- Verified against this checkout's `./build/tur` (0.21.0); repros are
  turmeric-side and self-contained.
- The concrete consumer is the http-handler typeclass plan in the spices repo
  (`docs/upcoming/v1/http-handler-typeclass-plan.md`): `json-ok` /
  `json-request` are `^Encode T`-constrained, so a call at an un-`Encode`-able
  payload slips through here rather than failing at the call site. That is a
  symptom of this turmeric-side gap, not a spice bug.
