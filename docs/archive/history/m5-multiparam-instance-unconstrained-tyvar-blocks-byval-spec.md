# M5: unconstrained instance type-ctor param blocks by-value spec interning

## RESOLVED 2026-06-15

Fixed via the two coordinated changes proposed below (direction 1 + the
emit-composition extension):

1. **Elaborator** (`elab_typeclasses.c`, `elab_definstance`): the instance
   head's *full* type-ctor param list (e.g. `K` and `V` of
   `MutableMap [K V]`) is now pushed into `e->sig_tyvars` for pass-2 body
   elaboration, not only the constraint-named ones. So a bare `K` in
   `(:: x (MutableMap K V))` resolves to a named tyvar instead of an opaque
   `TY_STRUCT`, and the call's `abi_bindings` record `K` as `TY_TYVAR`.
2. **Emit composition** (`emit_module.c`, `emit_abi_register_call`): the
   instance-method augmentation now resolves **all** type-ctor params by
   name from the receiver struct's own `type_params` (`recv`'s head
   `StructDef`), paired with the resolved `elem_buf` types -- not just the
   constrained ones from `type_param_constraints`.

`stdlib/mutmap.tur`'s `Eq [MutableMap]` was rewritten to the by-value path
(`mutmap-eq?-byval` + shared `mutmap-eq-storage?` core). The
`mutmap_eq__byval__spec__..._MutableMap__int__int_...` spec now interns and
the instance spec body calls it with by-value args (zero `(intptr_t)(&...)`
spills); the M4c Path A bridge no longer fires for `mutmap-eq` (measured by
env-gated probe + emit-c sweep -- it now fires only for
`m5-lambda-aft-tyvar-prior-accepts-concrete`). Suite green at 1635/0; 75
codegen snapshots regenerated (gensym-counter churn from the new stdlib
helpers). Pinned at runtime by `tests/fixtures/mutmap-eq`.

The original report follows.

---

## Summary

A typeclass instance over a **multi-parameter** parametric struct
(`Eq [MutableMap]` for `MutableMap [K V]`, `Eq [Map]` for `Map [K V]`,
etc.) cannot have its int64-carrier straddle retired the same way
`Eq [Vec]` / `Eq [Cons]` were (the landed M5 by-value rewrite), because
the instance's **unconstrained** type-ctor param (`K`) is recorded in the
instance-method call's `abi_bindings` as a concrete `TY_STRUCT`, not as an
abstract `TY_TYVAR`. The constrained param (`V`, via `(Eq V)`) is carried
as a `TY_TYVAR` and resolves correctly; the unconstrained one does not, so
no by-value spec is interned for a by-value helper called from the instance
body, and the spec body falls back to passing a by-value struct into the
helper's int64 carrier base -- a hard `cc` type error.

**Severity:** expressiveness / ergonomics gap, **not** a miscompile. The
shipped `Eq [MutableMap]` (carrier path through `mutmap-eq?`) is correct;
this only blocks the *by-value monomorphization* rewrite. Its practical
cost is that the M5 "carrier-bridge count -> 0" goal and the M3 accessor-
side bridge deletion stay blocked: `MutableMap` remains a live
`CK_CONCRETE -> CK_CARRIER` producer at `emit_expr.c` (the M4c Path A
bridge).

## Context: what already landed

The `Eq [Vec]` / `Eq [Cons]` by-value rewrite and the Finding-7 per-call
`(call, active-spec)` clone keying **landed** in commit `deee4c6`
("fix(m5): gap 4 FIXED ...") and the suite is green (1635 passed, 0 failed
as of 2026-06-15). `stdlib/vec.tur` now carries `vec-len-byval` /
`vec-eq-loop-byval`, and `vec-eq-ascribed-multi` (the multi-element-type
case Finding 7 unblocked) passes. The
`docs/upcoming/m5-residual-straddle-retirement.md` doc predates this and
still describes the rewrite as "reverted"; that is stale.

With the Vec/Cons producers gone, the two M5-target bridge sites now fire
for only a handful of fixtures (measured by env-gated probe + an `emit-c`
sweep over all fixtures):

- `emit_expr.c` M4c Path A `CK_CONCRETE -> CK_CARRIER` (the
  `(int64_t)(intptr_t)(&...)` spill for a by-value carrier aggregate passed
  to an int64-sink helper): fires for `mutmap-eq` and
  `m5-lambda-aft-tyvar-prior-accepts-concrete`.
- `emit_expr.c` EX_ASCRIBE `CK_CONCRETE -> CK_CARRIER`: fires for
  `m5-instance-spec-constraint-var` and `m5-spec-body-ascription-bridge`
  (the two fixtures that exist specifically to *pin* bridge behaviour).

So `mutmap-eq` is the one remaining "real" (non-pin) producer of the M4c
Path A bridge that a by-value rewrite could retire.

## Minimal repro

Attempted by-value rewrite of `Eq [MutableMap]` mirroring the Vec one:

```turmeric
;; shared inline-C core over two raw storage pointers (K/V-independent)
(defn mutmap-eq-storage? [pa : ptr<void> pb : ptr<void> ^fat val-cmp] : bool
  ```c ... walk *(__tur_mm_storage*)pa vs pb ... ```)

;; by-value wrapper: receiver taken by value so the carrier base derefs at
;; the call boundary, like vec-len-byval
(defn mutmap-eq?-byval [K V]
  [x : (MutableMap K V) y : (MutableMap K V) ^fat val-cmp] : bool
  (mutmap-eq-storage? (.storage x) (.storage y) val-cmp))

(definstance Eq [MutableMap]
  [(Eq V)]
  (eq? [x y]
    (mutmap-eq?-byval (:: x (MutableMap K V))
                      (:: y (MutableMap K V))
                      (fn [a b] (= a b)))))
```

### Observed

```
__inst_Eq_eq_qu_MutableMap__spec__bool_MutableMap__int__int_MutableMap__int__int:
    return mutmap_hyeq_qu_hybyval(x, y, (void *)(intptr_t)(__t31));
                                  ^ MutableMap__int__int passed where
static bool mutmap_hyeq_qu_hybyval(int64_t x, int64_t y, void * val_cmp)
                                   ^ the int64 carrier base is expected
-> error: incompatible type for argument 1 (cc)
```

No `mutmap_eq_qu_byval__spec__...` is interned; only the carrier base
exists, and the instance *spec* body calls it with by-value args.

### Expected

A by-value spec
`mutmap_eq_qu_byval__spec__bool_MutableMap__int__int_MutableMap__int__int(MutableMap__int__int, MutableMap__int__int, void*)`
is interned and called from the instance spec body (exactly as
`vec_len_byval__spec__int64_t_Vec__int(...)` is in the landed Vec path), so
no carrier round-trip and no bridge.

## Root cause

Trace of `emit_abi_register_call` (`src/compiler/emit_module.c`) for the
`mutmap-eq?-byval` call under the active instance-method spec
`__inst_Eq_eq_qu_MutableMap__spec__...`:

```
bind[0] name=K kind=18 (TY_STRUCT) cname=int64_t
bind[1] name=V kind=36 (TY_TYVAR) cname=int64_t
spec-bind[0] name=a kind=21 (TY_APP) cname=MutableMap__int__int   ; class var
COMPOSED:
  composed[0] K kind=18 (TY_STRUCT)  <- UNCHANGED
  composed[1] V kind=3  (TY_INT)     <- resolved
```

Two layers:

1. **Emit composition only augments constrained params.** The gap-4
   augmentation at `emit_module.c:1023-1057` derives concrete bindings for
   the active instance spec from `inst->type_param_constraints[]` (param_idx
   + tyvar), i.e. only `V` (from `(Eq V)`). `K` has no constraint, so it is
   never added to the composition's `spec_bindings`.

2. **The deeper blocker: `K` is not a `TY_TYVAR` to begin with.** Even after
   extending the augmentation to unify the method's abstract receiver
   `param_types[0]` (`MutableMap K V`) against the resolved receiver
   `MutableMap int int` and splice **all** params by name, `K` stays
   unresolved -- because the call's `abi_bindings[0].type` for `K` is a
   `TY_STRUCT` (kind 18), not `TY_TYVAR("K")`. `emit_abi_instantiate_type`
   only substitutes `TY_TYVAR` nodes, so a name-keyed `spec_bindings` entry
   `K -> int` has nothing to rewrite. The constrained `V` works precisely
   because elaboration *does* carry it as `TY_TYVAR("V")`.

So the fix has to make the **unconstrained** instance type-ctor param flow
into the instance-method call's `abi_bindings` as an abstract `TY_TYVAR`,
the same representation the constrained param already gets.

## Proposed fix directions

1. **Elaborator (preferred, general).** When elaborating an instance method
   over a multi-param type ctor, introduce **every** type-ctor param
   (constrained *and* unconstrained) as a named abstract tyvar in the method
   body's type environment, so a call like `(mutmap-eq?-byval (:: x
   (MutableMap K V)) ...)` records `abi_bindings = {K -> TY_TYVAR("K"),
   V -> TY_TYVAR("V")}`. Then the existing emit composition (extended to
   resolve all params from the receiver `TY_APP`, a ~25-line addition that
   was prototyped and is safe for the Vec single-param case -- it yields the
   same `A -> int` either way) interns the by-value spec. Find where
   `elab_definstance` binds the constrained tyvar and mirror it for the
   unconstrained ones.

2. **Emit-side positional resolution (narrower, hackier).** When the active
   spec is an instance method, resolve each callee `abi_binding` positionally
   by unifying the method's `param_types[0]` against the resolved receiver
   and *replacing the binding's `.type`* (not just adding a name->type
   substitution) for params the name-keyed pass can't reach. Brittle; only
   helps the receiver-shaped case.

## Validation

- After the fix, the `Eq [MutableMap]` by-value rewrite above must intern
  `mutmap_eq_qu_byval__spec__..._MutableMap__int__int_...` and the M4c
  Path A bridge must stop firing for `mutmap-eq` (env-gated probe at the
  `emit_carrier_bridge(CK_CONCRETE, CK_CARRIER, ...)` site, `emit-c` sweep).
- `bash tests/run.sh` stays at 0 `FAIL` (the `mutmap-*` / hamt fixtures are
  the regressor zone -- watch `hamt-delete`, `mutmap-eq`, `mutmap-*`).
- Regenerate `tests/fixtures/*/expected.c` snapshots in the same PR
  (stdlib edit -> codegen churn).
- The M4c Path A bridge then fires only for
  `m5-lambda-aft-tyvar-prior-accepts-concrete` and the two dedicated pin
  fixtures, narrowing what M3 / the bridge-predicate cleanup must account
  for. Note: even after `mutmap-eq` is retired the bridge is **not** dead
  (those producers remain), so D.4's "delete the branch outright" still
  cannot proceed on `mutmap-eq` alone.
