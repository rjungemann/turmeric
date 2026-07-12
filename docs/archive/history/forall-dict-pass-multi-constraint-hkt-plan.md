# `forall-dict-pass` multi-constraint + HKT method-receiver dicts (Deficit 2)

**Status:** DONE / GRADUATED 2026-07-06. Phases 1-2 landed (multi-constraint
dict-clone frame + mixed HKT/scalar dispatch), the acceptance fixtures
(`forall-dict-two-scalar/`, `van-laarhoven-lens-show/`) compile clean and run,
and Phase 3 graduated the flag: `forall-dict-pass` is removed from
`EXPERIMENTS[]` and the runtime dictionary-passing paths are now always-on.
The one shape still unsupported -- a constraint method dispatched from inside a
nested lambda in the dict-clone body -- is rejected with a specific diagnostic
(TUR-E0311, never miscompiled) and tracked as an open follow-up in
[../reported/forall-dict-pass-nested-lambda-method.md](../reported/forall-dict-pass-nested-lambda-method.md);
its negative fixture is `tests/fixtures/errors/forall-dict-nested-lambda-method/`.

**Original status:** OPEN -- graduation blocker for `--enable=forall-dict-pass`.
**Split from:** `docs/archive/history/forall-dict-pass-codegen-and-scope.md` (Deficit 2).
**Predecessor:** `docs/archive/history/constrained-hkt-forall-mode-b-plan.md` (MB1/MB2 landed
single-constraint; this plan finishes the "Not yet" note at its line ~161).
**Experiment:** `forall-dict-pass` in `src/runtime/experiments.c` (prototype,
`expires_at` 0.28.0, `g_opt_forall_dict_pass`).

## Summary

Deficit 1 (the dict-clone mis-lowering class-method result types) is **fixed** --
`forall-dict-show/`, `forall-dict-mixed-return/`, and
`van-laarhoven-lens-concrete/` all emit clean C and run correctly. What remains
before `forall-dict-pass` can graduate is **scope**: the elaborator builds a
runtime dictionary for exactly **one** typeclass constraint. A rank-2 constrained
poly fn with two or more constraints -- the real van Laarhoven-with-`Show` shape
`(Functor f, Show a) => (a -> f a) -> s -> f s` -- is rejected with a clear
"only a single typeclass constraint is supported so far" error.

The single-constraint HKT `(f a)` method receiver already works (that is exactly
what `van-laarhoven-lens-concrete/`'s `point-x` exercises: a dict-cloned fn whose
`Functor f` constraint dispatches `fmap` on a `(f a)` receiver). The remaining
HKT work is making that receiver dispatch pick the **right dict among N** when an
HKT constraint (`Functor f`) coexists with a scalar one (`Show a`).

## Current single-constraint architecture (what this plan generalizes)

The call-site dict *resolution* is already N-capable; the *clone* and the *emit
dispatch* are hard-wired to one slot.

- **Call site resolves N dicts already.** `elab_call.c` (~`5905`-`6094`) loops
  over every constraint of the forall, resolves each instance
  (`typeclass_env_lookup_instance`), and collects an `EX_DICT` per constraint
  into `mb1_dicts[16]`. At `~6298`-`6305` all `mb1_n_dicts` are prepended as
  leading carrier args, in constraint order, and `poly_arg_mask` is shifted by
  the dict count. **No change needed on the resolve/prepend path for N dicts.**
- **The guard blocks N.** `elab_call.c:4613`-`4635` (`if (inner_poly_constrained)`):
  when the forall or the inner fn's `fn_constraints` has `nc != 1` it emits
  `"forall-dict-pass: only a single typeclass constraint is supported so far"`
  and returns `NULL` before building the clone.
- **`make_dict_clone` is single-slot.** `elab_call.c:5500`: bails if
  `inner_b->fn_constraints->n_constraints != 1`; reads `constraints[0].typeclass`;
  prepends exactly one int64 dict param (`dparam`); stamps the *scalar* fields
  `FnDef.dict_clone_param` / `FnDef.dict_clone_class`.
- **Emit dispatch is single-slot.** `emit_fns.c:452`-`456` copies the scalar
  `fd->dict_clone_param`/`dict_clone_class` into `ctx->dict_dispatch_param_cname`
  / `ctx->dict_dispatch_class`. `emit_core.c:1676` (`emit_call_is_dict_param_-
  dispatch`) matches a method call whose instance's `typeclass ==
  ctx->dict_dispatch_class`; `emit_core.c:1685`-`1770` (`emit_call_name`) emits
  `((<ret>(*)(...))((void **)(intptr_t)<dict>)[<slot>])` where `<dict>` is the
  one `dict_dispatch_param_cname` and `<slot>` is the method's index in that one
  class's method list. The dict struct field order it indexes into is emitted in
  class-method order by `emit_stmt.c` (~`581`-`603`).

The generalization is therefore: **carry a vector of (class, dict-param) pairs
instead of a scalar pair, and dispatch each method call to the pair whose class
owns the method.**

---

## Phase 1 -- Multi-constraint dict-clone frame

Goal: a genuinely polymorphic constrained fn with N>=1 constraints is dict-cloned
with one dict param per constraint, and each class-method call in its body
dispatches through the dict param for that method's own class.

### Task 1.1 -- widen the FnDef descriptor from scalar to vector
- In `src/compiler/expr.h`, replace `FnDef.dict_clone_param` (single `Binding *`)
  and `FnDef.dict_clone_class` (single `TypeClass *`) with parallel vectors:
  `Binding *dict_clone_params[MAX_FN_CONSTRAINTS]`,
  `TypeClass *dict_clone_classes[MAX_FN_CONSTRAINTS]`, and `uint8_t
  n_dict_clone`. Keep `n_dict_clone == 0` meaning "not a dict-clone".
- Pick `MAX_FN_CONSTRAINTS` = the existing constraint-count cap (the `mb1_dicts`
  array is `[16]`; reuse 16 or the `ConstraintSet` cap). Document the bound.
- Grep every reader of the scalar fields and update (see Tasks 1.4, 1.5):
  `emit_fns.c` (2 sites: `~452`, and the return-type branches at `~481`, `~691`,
  `~1341`, `~1441`), `emit_module.c` (forward-decl + spec paths at `~4950`,
  `~5033`), `emit_core.c` (dispatch), `mono_specs.c` (`resolve_orig_lens`).
  A dict-clone is still recognized by `n_dict_clone > 0`.

### Task 1.2 -- generalize `make_dict_clone` to N constraints
- `elab_call.c:5500` `make_dict_clone`: drop the `n_constraints != 1` bail.
- Allocate `on + n_constraints` params: prepend one int64 dict param per
  constraint, **in constraint order** (matching the call site's `mb1_dicts`
  prepend order at `elab_call.c:6298`), before the original `on` params.
- Populate `cf->dict_clone_params[i]` / `cf->dict_clone_classes[i]` for each
  constraint `i`; set `cf->n_dict_clone = n_constraints`.
- Update the arity-overflow check (`(uint32_t)on + n_constraints > MAX_FN_ARITY`)
  and `akinds`/`ptypes` construction accordingly (the current loop assumes one
  leading dict).
- Keep the existing MB4 fn-param `boxed` marking for `(-> A (f A))` params.

### Task 1.3 -- widen the guard
- `elab_call.c:4613`-`4635`: remove the `nc != 1` / `n_constraints != 1`
  rejection. Keep a guard only for genuinely unsupported shapes discovered
  during Phase 2 (e.g. a constraint whose var is not pinned by any argument);
  make any residual error message specific about what is unsupported, not a
  blanket "single constraint only".
- Keep the mode-A monomorphic-inner path (`else` branch at `~4636`) unchanged; it
  already forwards `nc` ignored dict slots via `make_poly_wrapper_ex(...,
  n_lead_ignore = nc, ...)`.

### Task 1.4 -- per-class dispatch in emit
- `emit_fns.c:452`-`456`: instead of copying a scalar param/class into
  `ctx->dict_dispatch_param_cname` / `ctx->dict_dispatch_class`, install the
  full vector. Add `ctx->dict_dispatch_n`, `ctx->dict_dispatch_classes[]`, and
  `ctx->dict_dispatch_param_cnames[]` (parallel to the FnDef vectors), saved and
  restored around the body emit exactly as the scalar pair is today.
- `emit_core.c:1676` `emit_call_is_dict_param_dispatch`: return true when the
  method call's instance `typeclass` matches **any** of
  `ctx->dict_dispatch_classes[0..n)`.
- `emit_core.c:1696`-`1765` `emit_call_name`: look up the matching class index
  `k` by `instance->typeclass == ctx->dict_dispatch_classes[k]`, then emit the
  dispatch against `ctx->dict_dispatch_param_cnames[k]` (not the single
  `dict_dispatch_param_cname`). The `<slot>` computation (method index within
  that class) is unchanged.

### Task 1.5 -- forward-decl / spec parity
- `emit_module.c` (`~4950`, `~5033`) and `mono_specs.c` (`resolve_orig_lens`,
  `~486`) currently branch on the scalar `dict_clone_class`. Update to
  `n_dict_clone > 0`. The dict-clone return type stays the int64 carrier
  (unchanged from Deficit 1); only the recognition predicate widens.

### Phase 1 acceptance
- A two-scalar-constraint fixture (below, `forall-dict-two-scalar/`) compiles
  clean and runs: `poly` with `(^Show a ^Ord a x)` passed as
  `(forall [a] [(Show a) (Ord a)] (-> a cstr))`, dispatching `show` through dict
  slot 0 and `rank`/`compare` through dict slot 1, resolving the right instance
  per type. `bash tests/run.sh` green.

---

## Phase 2 -- Mixed HKT + scalar constraints (the real lens-with-`Show` shape)

Goal: `(Functor f, Show a) => (a -> f a) -> s -> f s` works end to end -- one HKT
constraint whose method receiver is `(f a)` coexisting with a scalar constraint.

### Task 2.1 -- HKT constraint pin under multiple constraints
- `elab_call.c:5905`-`5974`: the per-constraint pin loop already handles both a
  bare-tyvar arg (`vname` directly, `~5929`), a direct `(f a)` arg (MB2,
  `~5943`), and a nested `f` inside a fn-typed param (MB4, `~5955`). Verify it
  pins each of the N constraint vars independently -- an HKT `f` from the lens's
  `g : (-> A (f A))` arg AND a scalar `a` from a value arg -- with no
  cross-contamination. Add coverage where a single call pins two different vars.

### Task 2.2 -- HKT `(f a)` method dispatch selects the right dict
- With Task 1.4's per-class lookup, a `fmap` call (class `Functor`) inside the
  dict-clone body must resolve to the `Functor` dict slot even when a `Show` dict
  slot precedes/follows it. Confirm `emit_call_name`'s class-index match (1.4)
  keys on the method's owning class, not slot position, so `fmap` and `show`
  land on their respective slots regardless of constraint order.
- MB2.5 (aggregate `(f a)` return boundary, `emit_core.c:1711`-`1736`): the
  dispatched return type is derived from the representative instance method's
  declared result (the carrier for a class-var `(f b)` result). Re-verify this
  still holds when the active dict is slot k>0, i.e. the return-type derivation
  does not implicitly assume slot 0.

### Task 2.3 -- `poly_arg_mask` / `poly_agg_arg_mask` under N dicts
- `elab_call.c:6305`: `poly_arg_mask` is shifted left by `mb1_n_dicts`. Confirm
  the same shift is applied to `poly_agg_arg_mask` (the by-value aggregate arg
  mask) so a `(f a)` aggregate argument is still recognized after N leading dict
  slots -- the mode-B "Not yet" note flags exactly this as untouched for the
  multi-dict case.

### Phase 2 acceptance
- Fixture `van-laarhoven-lens-show/` (below): a lens whose focus also carries a
  `Show a` obligation used inside the traversal, i.e. the `(Functor f, Show a)`
  shape, compiles clean and runs. `bash tests/run.sh` green.

---

## Phase 3 -- Fixtures, snapshots, and flag graduation

### Task 3.1 -- fixtures
Add, each with `flags` = `--enable=...forall-dict-pass`, `expected.stdout`, and
an `expected.c` snapshot (mirroring `forall-dict-mixed-return/`):
- `tests/fixtures/forall-dict-two-scalar/` -- two scalar constraints
  `(Show a) (Ord a)` (Phase 1).
- `tests/fixtures/van-laarhoven-lens-show/` -- `(Functor f, Show a)` lens
  (Phase 2).
- A negative fixture under `tests/fixtures/errors/` for any shape still
  deliberately rejected after this plan (if any remains), asserting the specific
  diagnostic rather than the old blanket message.

### Task 3.2 -- suite + snapshot reconciliation
- Regenerate any moved `expected.c` snapshots in the same change
  (the CLAUDE.md regen loop with per-fixture `flags`). Confirm
  `bash tests/run.sh` is green (10-min timeout per the strict rule).

### Task 3.3 -- graduate the flag
Once Phases 1-2 land and the suite is green:
- Delete the `forall-dict-pass` row from `EXPERIMENTS[]` in
  `src/runtime/experiments.c`, remove the `g_opt_forall_dict_pass` global, and
  make the gated paths (`if (g_opt_forall_dict_pass)` in `elab_call.c`)
  always-on -- following the retirement pattern in
  `docs/upcoming/retire-graduation-ready-hkt-flags-plan.md`.
- Update `tur experiments` expectations and any test asserting the row exists.
- Move this plan and `docs/archive/history/constrained-hkt-forall-mode-b-plan.md`'s
  remaining live references to reflect graduation; archive this plan to
  `docs/archive/`.

### Task 3.4 -- cross-references
- Update `docs/upcoming/retire-graduation-ready-hkt-flags-plan.md` (the
  `forall-dict-pass` rows) to point at graduation instead of the open deficit.

---

## Risks / notes

- **Constraint order is load-bearing.** The call site prepends dicts in
  constraint order (`elab_call.c:6298`) and the clone must declare its dict
  params in the *same* order (Task 1.2), because `poly_arg_mask << mb1_n_dicts`
  assumes positional correspondence. Per-class *dispatch* (Task 1.4) keys on the
  class identity, so it is order-independent -- but the *frame layout* is not.
- **`emit_carrier_holds_ptr` / generic-carrier return bridges (Deficit 1)** are
  orthogonal and already landed; they should need no change for N constraints,
  but the Phase 1/2 fixtures will exercise them under multi-dict frames and are
  the regression signal if an assumption leaked.
- Keep the mode-A monomorphic-inner subsumption (`n_lead_ignore = nc`) working:
  a monomorphic fn passed where the forall has N constraints must still accept
  and ignore N dict slots.
