---
title: M4c step 1+2 — class-var TYVAR erased at instance elaboration; per-call-site spec substitution can't recover it
category: Codegen / ABI — monomorphization plan refinement
severity: Low (refines `docs/upcoming/m4c-execution-plan.md` step 2). The M4b backlinks and M4c-pre stdlib rewrites are inert until M4c lands; this report explains why the M4c execution plan's step 2 can't proceed without a deeper elab change.
description: M4c step 1 (populate `abi_bindings` on dispatch EX_CALL nodes) and step 2 (verify the method's binding type has tyvars) were executed empirically. Step 1 works mechanically — bindings are populated and `emit_abi_register_call` walks the call to spec-generation. Step 2 surfaces the dependency: at instance elab (`src/compiler/elab_typeclasses.c:2440-2446`), the class variable (e.g. "a" in `Eq[a]`) is eagerly resolved into the instance's `type_args[0]` (e.g. bare `Tuple2` struct) when computing the method param's binding type. The resulting `arg_full_types` carries the resolved type, not the abstract TYVAR. Per-call-site substitution at `emit_abi_register_call` has nothing to substitute — the TYVAR is already gone — so the generated arg type matches the generic arg type and no spec is minted. Retiring the carrier-ABI bridge requires either (a) preserving the class-var TYVAR through instance elab and substituting it per call site, or (b) per-instantiation method emission at instance elab time (changes the dict slot's declared type, which then breaks every existing dispatch site that expects the uniform carrier).
status: RESOLVED 2026-06-13. Path A landed in a follow-up turn via a different substitution mechanism than this report originally proposed: instead of carrying TY_TYVAR through the method binding (which broke every existing emit signature), the substitution lives in `emit_abi_register_call` (`src/compiler/emit_module.c:1037-1055`) — it checks whether the param's generic type equals the instance's resolved `type_args[0]` (a parameterized struct) and overrides `arg_types[i]` from the call-site abi_bindings. Pair this with the bridge extension at `emit_expr.c:2549-2580` (general by-value-carrier-param → int64 sink) and the field-access spec override at `emit_expr.c:3878-3914`. See `docs/reported/m4c-path-a-cascades-into-stdlib-eq-instances.md` for the full landing story.
---

# Class-var TYVAR erased at instance elab

## Empirical findings from step 1+2

### Step 1 mechanically works

Added `abi_bindings` to dispatch EX_CALL nodes in
`src/compiler/elab_typeclasses.c:4046` (the `expr_new(…, EX_CALL, …)` for
dispatch). Bindings:

```c
bindings[i] = { .name = tc->type_params[i]->name,
                .type = best_inst->type_args[i] };
```

Gated on non-HKT class (all `type_param_kinds[i] == KIND_STAR`).

After this, `emit_abi_register_call` (`src/compiler/emit_module.c:951`)
reaches the `__inst_Eq_eq_qu_Tuple2` call, passes the L1001 bailout
(fn_expr NULL, fn_binding global TY_FN). Confirmed via
`TUR_M4C_DEBUG=1`:

```
[m4c-debug] reg_call: __inst_Eq_eq_qu_Tuple2 n_bindings=1 dict_arg=0x… fn_expr=0x0
[m4c-debug]   ->after L1001 OK; fn_expr=0x…
```

### Step 2 fails: substitution is a no-op

Added an arg-type trace at the per-param substitution loop
(`emit_module.c:1053-1065`):

```
[m4c-debug]   arg[0]: generic=Tuple2 subst=Tuple2
[m4c-debug]   arg[1]: generic=Tuple2 subst=Tuple2
```

The generic arg type is bare `Tuple2` (TY_STRUCT). The substituted arg
type is also bare `Tuple2`. `strcmp != 0` is false; `abi_changes` stays
false; no spec gets interned.

This happens **before** my M4c step 2 plumbing (adding `arg_full_types`
on the instance method's `fn_type`) takes effect, because the
underlying `arg_full_types[i]` points at
`&method_params[i]->type` which is itself bare `Tuple2` — not a TYVAR.

### Root cause: class-var erasure

`src/compiler/elab_typeclasses.c:2440-2446`, the per-method param-type
computation during instance elab:

```c
} else if (elab_param_type.kind == TY_APP) {
    param_type = TYPE_INT;
}
```

Earlier at line 2427:

```c
elab_param_type = type_args[0];
```

When the method's typeclass-declared param type is the bare class variable
(`a` in `Eq[a]`), `elab_param_type` is resolved to `type_args[0]` — for
`Eq Tuple2` that's the bare `Tuple2` struct constructor. The TYVAR `a` is
gone before the method binding is even created.

The method's param binding stores `elab_param_type` directly. So
`method_params[i]->type` is `Tuple2`, never `a`. Any
`arg_full_types[i] = &method_params[i]->type` plumbing carries the
already-resolved type. Substitution at the call site has nothing to do.

This is the fork in the road:

## Two paths forward

### Path A — preserve the class-var TYVAR

Change `src/compiler/elab_typeclasses.c:2427` so that for non-HKT
parameterized type-args, the param binding's type carries the class-var
TYVAR (e.g. `TY_TYVAR(name="a")`), not the resolved `type_args[0]`. Then
my `arg_full_types` plumbing from step 2 gives the spec system real
TYVARs to substitute.

**Implications:**

- The instance method's emit signature derives from the param binding
  type, so it would no longer be `int64_t x` — it would lower as
  whatever class-var "a" lowers to. For carrier-mode (the only mode
  today) that's `int64_t`, so on the surface the emitted signature
  doesn't change. But every consumer of `method_params[i]->type`
  needs to handle "a" as a TYVAR, not the resolved struct — including
  body elaboration (`(.fst x)` where x : a wouldn't resolve fields).
- The body would need a way to know that "a" maps to a struct so
  `(.fst x)` resolves. Today the body sees x : Tuple2 directly.

This is a careful elab refactor — probably ~½–1 session of focused
work, and a thorough fixture re-run.

### Path B — per-instantiation method emit at instance elab

Skip the call-site substitution entirely. At instance elab time, emit
**one method clone per (instance, type-arg-tuple) the dict ever
references**. The dict struct's slot type matches the clone's signature.

**Implications:**

- The dict struct + singleton can't be emitted from `definstance` alone
  — they need a worklist of "which instantiations were observed at
  call sites" before the dict can close.
- This couples typeclass-emit with the existing per-instantiation spec
  worklist for ordinary defns. The infrastructure exists
  (`EmitAbiSpecialization`), but the wiring is non-trivial.

This is closer to a Plan M5-shaped piece of work (constrained
polymorphism). 1-2 sessions of careful work, larger fixture-regen.

## What landed this turn

Code changes during the step-1+2 probe (debug instrumentation +
`arg_full_types` plumbing on the instance method's fn_type, +
abi_bindings on dispatch) were **reverted** after the substitution
diagnostic confirmed they had no effect. The reverts were clean — suite
returns to 170 FAIL (172 pre-existing minus the resolved transient
`hamt-delete`).

Net deliverable: this report, which refines the M4c execution plan's
step 2 from "expected: low risk — pre-mono'd to int64 → add poly shadow
type" to:

> **Step 2 actually requires either (a) preserving the class-var TYVAR
> through instance elab — and threading it through body elaboration so
> `(.fst x)` resolves on x : a — or (b) per-instantiation method emit
> coupled with a deferred dict-singleton worklist. Pick the path before
> starting the next M4c attempt.**

## Validation when M4c step 2 lands

The diagnostic that confirms either path is working:

```sh
TUR_M4C_DEBUG=1 ./build/tur emit-c tests/fixtures/emit-abi-trace/input.tur 2>&1 \
  | grep -A 2 "reg_call: __inst_Eq_eq_qu_Tuple2"
```

Expected output for a working step 2:

```
[m4c-debug] reg_call: __inst_Eq_eq_qu_Tuple2 n_bindings=1 …
[m4c-debug]   ->after L1001 OK; fn_expr=…
[m4c-debug]   arg[0]: generic=int64_t subst=Tuple2__int__int
[m4c-debug]   arg[1]: generic=int64_t subst=Tuple2__int__int
```

Note `subst` differs from `generic` — that's the signal `abi_changes`
becomes true and the spec gets interned.

Once the diagnostic shows substitution, steps 3-5 of the original M4c
execution plan can resume.

## Related

- [docs/upcoming/m4c-execution-plan.md](../upcoming/m4c-execution-plan.md)
  — the plan this report refines (step 2 specifically).
- [docs/m4b-handoff.md](../m4b-handoff.md)
- [docs/reported/m4c-stdlib-carrier-helpers-block-dispatch-rewrite.md](m4c-stdlib-carrier-helpers-block-dispatch-rewrite.md)
  — M4c-pre's unblock (resolved).
- [docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
  — the M3 deletion blocker this whole chain aims at.
- `src/compiler/elab_typeclasses.c:2427` — class-var resolution to instance type-arg.
- `src/compiler/elab_typeclasses.c:2440-2446` — parameterized-struct carrier override.
- `src/compiler/emit_module.c:1053-1065` — per-param substitution loop where the
  no-op was observed.
