# `[^Class a]` defn type-param vector registers no typeclass constraint

**Severity:** medium (expressiveness/metadata hole; behavior is currently
rescued by the ABI specializer + turi recovery heuristics)

## Summary

In the two-vector defn spelling

```turmeric
(defn my-show [^Show a] [x : a] : String (show x))
```

the `[^Show a]` vector is parsed by `elab_defn`'s TYPE-PARAM vector branch
(src/compiler/elab_fns.c:4297), whose caret handling
(elab_fns.c:4327) treats **every** `^name` as a higher-kinded type
parameter: `^Show` is stripped to a KIND_ARROW type param literally named
`Show`, and `a` becomes an independent star-kinded type param. **No
`TypeConstraint` is created**, so `fd->constraints.n_constraints == 0` and
`binding->fn_constraints` stays NULL.

Contrast the middle-vector form `(defn f [A] [(Show A)] [x : A] ...)`,
which registers the constraint (elab_fns.c:4463) -- and the
params-vector caret path (elab_fns.c:4700), which distinguishes
lowercase `^f` (kind var) from uppercase `^Class` (constraint) and DOES
register. The type-param-vector branch makes no such distinction.

~66 fixture/stdlib files use the `[^Class a]` spelling, so today the class
name lands in scope as a bogus HKT type param and the constraint is
invisible to:

- call-site obligation discharge (`fn_constraints` backlink, elab_fns.c:7409);
- the turi apply-time constraint-dict path (`frame_bind_constraint_dicts`,
  src/turi/eval.c) -- which is why `generic-show-dispatch-opaque` and
  `string-slice` still need the `gde_reresolve_method` head-name heuristic
  (see docs/upcoming/turi-dict-passing-plan.md step-4 measurements);
- any future tooling that reads constraint metadata.

Dispatch still works in both engines because the compiled path leans on the
ABI specializer's per-nominal-type specs and turi on the gde_* heuristics.

## Minimal repro

```turmeric
(defclass P [a] (pv [x : a] : int))
(definstance P [int]  (pv [x] 1))
(definstance P [cstr] (pv [x] 2))
(defn f1 [^P a] [x : a] : int (pv x))   ;; fd->constraints.n_constraints == 0
(defn f2 [A] [(P A)] [x : A] : int (pv x)) ;; == 1
```

Instrument elab_fns.c:7401 (`fd->constraints.n_constraints` store) and
observe f1 = 0, f2 = 1.

## Fix directions

In the type-param-vector caret branch (elab_fns.c:4327), mirror the
params-vector distinction (elab_fns.c:4712): a `^Name` whose first
post-caret char is uppercase resolves as a typeclass and becomes a pending
constraint applied to the NEXT symbol in the vector (the binder), instead
of minting a KIND_ARROW type param named after the class. Risk: this turns
~66 files' defns into *formally constrained* defns, activating call-site
discharge and (in turi) the dict path -- likely correct, but it needs a
fixture sweep (the van-laarhoven-lens and forall-dict families are the
blast radius) before landing. Until then the turi dict path simply skips
these defns (no constraint metadata to read).
