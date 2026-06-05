---
title: Per-monomorphization specialization of closure-returning generic defns
category: Planning
description: Implement "fix direction 1/2" from the resolved G2 report so a polymorphic, typed closure-returning defn (e.g. a generalized stdlib `>>>`) specializes its inner closure body per concrete result type. Removes the TUR-E0705 guard for the float case and lets `>>>` compose `:float -> :float` closures without the register-class miscompile reported in arrow-compose-float-closure-int64-thunk-mismatch.md.
---

# Per-monomorphization closure-body specialization -- Plan

## Why

`stdlib/arrow.tur`'s `>>>` is type-erased over its argument closures, so its
inner `(fn [x] (gv (fv x)))` is lowered once to the int64 thunk ABI. Composing
two `:float -> :float` closures then dispatches `double(void*,double)` bodies
through `int64_t(*)(void*,int64_t)` pointers -- undefined behavior that only
"works" by SysV register-class accident
(`docs/reported/arrow-compose-float-closure-int64-thunk-mismatch.md`).

The G2 report
(`docs/archive/history/poly-defn-shares-inner-closure-body-across-monomorphizations.md`)
resolved the *silent* miscompile by turning the float case into a hard error
(`TUR-E0705`). This plan implements the report's deferred "fix direction 1/2":
specialize the inner closure body per monomorphization so the float case is
register-class-correct, then remove the guard and generalize `>>>`.

Verified feasibility verdict: **MEDIUM-LARGE**. Elaboration + E0705 changes are
small and local; the emit-side inner-body cloning is genuinely new machinery
and the risk center.

## Confirmed root-cause map (file:line)

### Elaboration -- why the polymorphic typed spelling fails to type-check
`(defn cmp [A B C] [^fat f :(fn [:A] #{} :B) ^fat g :(fn [:B] #{} :C)] : ptr<void>
  (let [fv f gv g] (fn [x : A] : C (gv (fv x)))))` raises two errors:

- **A1 (call site, `expected (fn [] : ?), got ptr<void>`):**
  `call_collect_type_bindings` (`elab_call.c:86-127`) has no `TY_FN` case, so a
  `(fn [:A] :B)` expected param against a `ptr<void>` (or `(fn ...)`) actual
  falls to `default: type_eq(...)` -> false, leaving `A`/`B` unbound and
  tripping the arg-mismatch diagnostic at `elab_call.c:2522-2526`. The `^fat`
  acceptance at `elab_call.c:2238-2252` never gets to apply.
  **Fix:** add a `TY_FN` case that, for `actual.kind == TY_FN`, recurses over
  `arg_full_types[i]` <-> actual arg types and `result_full_type` <-> actual
  result; for `actual.kind == TY_PTR_VOID` (opaque fat box) accept the head and
  leave the fn's tyvars unbound from this argument (same precedent as the
  `TY_ADT`-vs-`TY_APP` head match already in the function). Tyvars then bind
  from the other argument / call result, as `constant`'s `A` binds from `val`.

- **A2 (inner body, `expected <struct>, got tyvar` at `(fv x)`):** the
  saturated positional loop computes `arg_ok = (args[i]->type.kind ==
  expected_arg_kind)` at `elab_call.c:2201`. There is a tyvar-*parameter*
  escape hatch at `elab_call.c:2219-2222`; we need the symmetric rule for a
  tyvar *argument* (`x : A` is `TY_TYVAR` in a generic body). Add next to 2219.

Both A-fixes only widen acceptance for the generic case; concrete calls still
match via kind-equality first, so no existing codegen changes.

### Monomorphization + inner-closure emit (the core change)
- Generic calls record an `AbiTypeBinding[]` on the `EX_CALL`
  (`elab_call.c:2961-2967`).
- Emit interns an `EmitAbiSpecialization` (`emit_module.c:412-458`,
  `emit_abi_intern_spec`); clone name via `emit_abi_clone_name`
  (`emit_module.c:388-403`).
- The OUTER spec body is emitted in the loop at `emit_module.c:2019-2028` with
  `ctx.current_abi_specialization` + `ctx.fn_name_override` set;
  `emit_resolve_type` (`emit_core.c:69-116`) rewrites the spec's tyvars to
  concrete types during that emit.
- The INNER `(fn ...)` is lifted to a *global* binding at
  `elab_fns.c:3290-3296` (`scope_add(&e->global, b)`) and emitted **once** in
  the top-level item loop at `emit_module.c:1945-1950` with
  `current_abi_specialization == NULL`. Its C return type comes from
  `emit_fn_result_type_from_type(binding->type)` (`emit_expr.c:255-260`,
  used at `emit_expr.c:2470` and `2520`) -- fixed, never re-resolved per spec.
- The outer defn's link to the inner body is
  `fn_binding->returns_closure_fn_binding` (set `elab_fns.c:2610,3296`; field
  `expr.h:59`).
- Both emit sites key the env struct by the **same** `closure->env_name`
  symbol: construction in `emit_expr.c:2453-2566` (cache at 2460-2480, env
  field type at 2502-2505, `thunk_result`/`thunk_sym` at 2520/2524), and the
  lifted-body env-struct emit + cast in `emit_fns.c:301-333` and
  `emit_fns.c:562-566`.

### TUR-E0705 guard
`elab_call.c:2929-2952`: fires when `n_type_bindings>0`, callee has
`returns_closure_fn_binding`, inner `result_full_type` is a bare `TY_TYVAR`,
and that tyvar binds to a float kind (2938).

## Implementation stages

> No stage before 3 delivers a working float `>>>`: with only A in place, a
> retyped polymorphic `>>>` type-checks but still hits E0705 on float. Land 1->4
> as one change (or behind a flag) -- do not ship A alone.

1. **Stage A (elaboration acceptance).** Add the `TY_FN` case to
   `call_collect_type_bindings` (elab_call.c:86) and the tyvar-argument
   acceptance near elab_call.c:2219. New positive fixture: a *generic* compose
   used at `:int` (does not hit E0705) compiles and runs. Full suite green.

2. **Stage B+C (per-spec inner-body clone) -- the risk center.** In the
   spec-emit loop (emit_module.c:2019-2028), when the outer defn has
   `returns_closure_fn_binding` whose inner body mentions a spec tyvar,
   compute a spec suffix (reuse `emit_abi_clone_name`) and:
   - emit a suffixed clone of the lifted inner `EX_FN_DEF` under the same
     `current_abi_specialization`, so its params/return/env-field/dispatch
     types all resolve via `emit_resolve_type` to the concrete `A`;
   - thread the suffix through `ctx` so the OUTER spec body's `EX_CLOSURE`
     emit (emit_expr.c:2453) uses the suffixed `env_name` (intern a new
     symbol) and suffixed `thunk_sym`, and computes `thunk_result` (2470/2520)
     and the `(fv x)`/`(gv ...)` dispatch typedef
     (emit_expr.c:1934-1947, 1995-2007) and env-field type (2502-2505) through
     `emit_resolve_type` instead of raw;
   - dedup inner clones per distinct concrete `A`;
   - **invariant:** when `current_abi_specialization == NULL`, behavior is
     byte-identical (no churn for non-generic closures). When the inner tyvar
     resolves to an integer-class carrier, the clone is byte-identical to
     today's `__fn_N` modulo a name suffix.
   - Note: `emit_resolve_type` (emit_core.c:72-114) only substitutes
     `TY_TYVAR/TY_APP/TY_UNION/TY_INTERSECTION`; if an inner result is a nested
     `(fn ...)` rather than a bare tyvar, add a `TY_FN` case there.

3. **Stage D (remove guard).** Delete the E0705 block (elab_call.c:2929-2952)
   once B+C make both the dispatch *and* the captured-value env field
   (direction 2) float-correct. Convert
   `tests/fixtures/errors/poly-closure-result-tyvar-float/` from an error
   fixture to a passing codegen+stdout fixture (expected `440\n440\n1\n`).
   Update the resolved report to "RESOLVED via direction 1/2".

4. **Stage E (generalize `>>>`).** Retype `stdlib/arrow.tur:45` to the
   polymorphic typed spelling, regenerate the arrow/closure snapshots, rewrite
   `tests/fixtures/sf-compose-typed/` to call stdlib `>>>` on `:float` SFs,
   and flip the G7 amber note in
   `docs/archive/history/language-readiness-for-typed-signal-plan.md` to green.

## Risk + validation
- Fixtures to diff/regenerate (shared-inner-body / `>>>` / sibling
  register-class families): `errors/poly-closure-result-tyvar-float`,
  `stdlib-arrow`, `stdlib-arrow-load`, all `arrow-instance-*`,
  `arrow-capturing-closure`, `operator-mangle-pair`, `sf-compose-typed`,
  `fat-closure-float-compose`, `instance-closure-return-*`,
  `cstr-returning-closure-thunk`, `fn-typed-return-thin-closure`, `bare-fat-*`,
  `curried-fn-typed-param-application`, `module-spec-*`.
- Key risks: (1) env-struct cache not spec-aware -> C redefinition or
  wrong-layout reuse; (2) missing inner-clone dedup -> duplicate
  `__fn_N__spec__double` defs; (3) int/cstr/ptr specializations silently
  changing codegen via clone renames; (4) nested-fn inner results not resolved
  by `emit_resolve_type`.
- Per CLAUDE.md: regenerate every `tests/fixtures/*/expected.c` with
  `tur emit-c`, confirm `bash tests/run.sh` zero `FAIL` (leak detection on,
  ~1480 fixtures), commit snapshots in the same change.

## Cross-references
- Trigger: `docs/reported/arrow-compose-float-closure-int64-thunk-mismatch.md`.
- Resolved precursor (diagnose direction):
  `docs/archive/history/poly-defn-shares-inner-closure-body-across-monomorphizations.md`.
- Unblocks the G7 amber edge in
  `docs/archive/history/language-readiness-for-typed-signal-plan.md` and the
  central combinator of `docs/upcoming/tur-signal-rebuild-plan.md`.
</content>
