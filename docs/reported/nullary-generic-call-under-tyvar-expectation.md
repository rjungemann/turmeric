# A nullary generic call under a tyvar-shaped expectation is rejected against itself

**Severity: low-medium** (expressiveness hole with a one-token workaround).
Found 2026-09-19 while resolving
[self-typed-heap-parametric-field-unsupported](../archive/self-typed-heap-parametric-field-unsupported.md);
it reproduces on a non-recursive def, so it is not that shape's.

## Repro

```turmeric
(defstruct W :heap [A] (val A) (opt (Option A)))
(defn main [] : int
  (let [w (make-struct W 8 (none))]
    (println (.val w)))
  0)
;; error [TUR-E0001]: function 'W' arg 2:
;;   expected (type-app Option tyvar 'A'), got (type-app Option tyvar 'A')
```

The same with a user nullary generic:

```turmeric
(defstruct Box :heap [A] (val A))
(defn box-nil [A] [] : (Box A) (:: 0 (Box A)))
(defn wrap [A] [v : A b : (Box A)] : (Box A) (make-struct Box v))
(wrap 3 (box-nil))
;; function 'wrap' arg 2: expected (type-app Box tyvar 'A'), got (type-app Box tyvar 'A')
```

Both back ends reject it identically (the check is in elaboration).

## Workaround

Ascribe the instantiation: `(:: (none) (Option int))`, `(:: 0 (Box int))`.

## Root cause (probable)

The ctor / call argument check compares the argument's type against the
param's declared type with the callee's own type variables still free on BOTH
sides, and a nullary generic's result carries its OWN fresh `'A`. Two
distinct tyvars named `A` are not `type_eq`, and no unification step binds
the callee's `A` to the argument's before the mismatch is reported. A
saturated argument (`(some 3)`) grounds its `A` first and so never hits it.
Start in `elab_call.c` at the arg-mismatch report (the branch that prints
`arg_full_types`) and the tyvar-expected path just above it.

## Fix direction

Unify instead of comparing: when the param type and the arg type are both
applications over the same head and the arg's type args are the callee-local
tyvars of a nullary call, bind them from the expectation (or, when the
expectation is itself a tyvar, defer the check to the call's instantiation).
