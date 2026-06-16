---
title: M4c execution plan — per-instantiation typeclass-method specs (Path A)
category: Planning -- ABI / Codegen rework
description: Step-by-step execution plan for Plan M4c (per-instantiation dispatch), oriented toward **Path A: preserve the class-var TYVAR through instance elab**. Builds on M4b's `FnDef.owner_instance` + `EmitAbiSpecialization.typeclass_inst` backlinks (inert) and M4c-pre's stdlib-body rewrites (active). Step 2's empirical re-shaping (`docs/reported/m4c-class-var-erased-at-instance-elab.md`) revealed that the class variable (`a` in `Eq[a]`) gets resolved to the instance's bare type-arg at instance elab (elab_typeclasses.c:2427), erasing the TYVAR before the method binding exists — so step 1's call-site bindings have nothing to substitute against. Path A keeps the TYVAR on the binding and reshapes downstream consumers to handle it.
---

> **Path orientation:** Path A is chosen. Path B (per-instantiation
> method emit at instance elab time) was the alternative — see
> [docs/reported/m4c-class-var-erased-at-instance-elab.md](../reported/m4c-class-var-erased-at-instance-elab.md)
> "Two paths forward" for the tradeoff. Path A is the smaller change
> and keeps the existing dict-emit shape; Path B would re-architect
> dict-singleton emit on a deferred worklist.

## Path A — what changes (revised entry point)

Path A keeps `param_type` (the binding's ABI-emit type) unchanged — the
instance method still emits as `bool __inst_Eq_eq_qu_Tuple2(int64_t,
int64_t)` for the **carrier fallback symbol**. But:

1. **`method_params[i]->type` keeps the class-var TYVAR** instead of
   being resolved to the instance's bare type-arg.
   `elab_typeclasses.c:2427` (`elab_param_type = type_args[0]`) becomes
   conditional: when the class variable is the literal param type, keep
   `TY_TYVAR(name="a")` on the binding rather than rewriting to the
   resolved instance type.
2. **Body elaboration handles TYVAR-typed params.** Field-access forms
   like `(.fst x)` where `x : a` need to know that `a` resolves to a
   struct for `(.fst …)` to resolve. The instance's `type_args[0]`
   carries the resolved struct; thread it through as scope context so
   the field-access elaborator can look through the TYVAR.
3. **`arg_full_types[i]` carries the TYVAR.** With the binding's type
   being a real TYVAR, `&method_params[i]->type` (set in step 2 of the
   M4b backlinks work) IS a TYVAR — substitution now does real work.
4. **The dispatch call's `abi_bindings` (already populated in step 1 of
   the original plan)** binds `a` → the call site's receiver type
   (`obj_orig_type`, not `best_inst->type_args[0]`). At
   `emit_module.c:1053-1065`, substituting through this binding rewrites
   the param's `TY_TYVAR("a")` to the concrete receiver type (e.g.
   `Tuple2__int__int`). `abi_changes` becomes true and the per-call-site
   spec gets interned.
5. **The per-instantiation method spec is emitted** using the existing
   spec machinery, with the body re-elaborated under the call-site
   bindings (`x : Tuple2__int__int` instead of `x : a`).
6. **Dispatch site emit** (`emit_expr.c:1749`) consults
   `find_matched_abi_spec` (already wired for direct calls) and emits a
   direct call to the spec clone, dropping the `(intptr_t)` cast.
7. **The carrier fallback symbol stays** for HKT-class dispatches and
   call sites where the receiver type is still abstract — those keep
   the uniform-carrier ABI.

The non-trivial bit is **step 2** above (body elaboration on TYVAR
params). That's the focused engineering work of Path A.

## State at start

Already shipped, inert until this plan executes:

- `FnDef.owner_instance` (`src/compiler/expr.h:407`, populated in
  `src/compiler/elab_typeclasses.c:2611`).
- `EmitAbiSpecialization.typeclass_inst`
  (`src/compiler/emit_internal.h:107`, populated in
  `src/compiler/emit_module.c:697-717`).

Already shipped, active:

- M4c-pre stdlib body rewrites — Eq Tuple2 / Pair / Result instances now
  consult by-value fields directly via recursive `Eq A`/`Eq B` dispatch
  (`stdlib/{tuple,pair,result}.tur` at the `(definstance Eq …)` lines).

Audit point of departure (this is the baseline behavior M4c retires):

```c
/* tests/fixtures/emit-abi-trace/input.tur — (.eq? t1 t2) */
Tuple2__int__int __t22 = t1_882;            // <- bridge: spill by-value
Tuple2__int__int __t23 = t2_883;
if (__inst_Eq_eq_qu_Tuple2((int64_t)(intptr_t)(&__t22),  // <- bridge: address cast
                           (int64_t)(intptr_t)(&__t23)))
```

The carrier bridge fires twice (`concrete→carrier`) to spill each
by-value `Tuple2__int__int` to an int64 handle. M4c's goal is to make
these inline-bridge stanzas disappear: dispatch to a method whose C
signature is already `bool(Tuple2__int__int, Tuple2__int__int)`.

## Why dispatch calls don't generate specs today

`emit_abi_register_call` (`src/compiler/emit_module.c:951`) is the spec
worklist's intake. It returns early at line 959 when
`call->as.call_.abi_bindings` is NULL or empty:

```c
const AbiTypeBinding *bindings = call->as.call_.abi_bindings;
uint8_t n_bindings = call->as.call_.n_abi_bindings;
if (!bindings || n_bindings == 0) return;
```

A grep over `src/compiler/elab_typeclasses.c` finds **zero** writes to
`out->as.call_.abi_bindings`. So every typeclass-dispatch call goes
through L959 and never registers a spec. The unspecialized
`__inst_<Class>_<method>` carrier symbol is the only emit.

## The 5 concrete steps

### Step 1 — populate `abi_bindings` on dispatch EX_CALL nodes

**File:** `src/compiler/elab_typeclasses.c:4009` (the `expr_new(…,
EX_CALL, …)` that builds the dispatch call).

**Change:**

```c
Expr *out = expr_new(e->arena, EX_CALL, result_type, call->span);
…
out->as.call_.fn_binding = best_method->binding;
out->as.call_.fn_expr    = NULL;
out->as.call_.args       = call_args;
out->as.call_.n_args     = n_args + 1;
out->as.call_.dict_arg   = dict_expr;
+   /* M4c step 1: populate abi_bindings from the resolved instance's
+    * type-args so emit_abi_register_call can mint a per-instantiation
+    * spec for the instance method.  best_inst->type_args[] holds the
+    * concrete Type for each typeclass parameter; mirror its name from
+    * best_inst->typeclass->type_params[]. */
+   if (best_inst && best_inst->n_type_args > 0
+       && best_inst->typeclass->type_param_kinds
+       /* M4 carve-out: skip HKT classes — they keep the carrier ABI
+        * per the M6/M7 plan. */
+       && /* all type_param_kinds[i] == KIND_STAR */) {
+       AbiTypeBinding *bindings = arena_alloc(e->arena,
+           best_inst->n_type_args * sizeof(AbiTypeBinding));
+       for (uint8_t i = 0; i < best_inst->n_type_args; i++) {
+           bindings[i].name = best_inst->typeclass->type_params[i]->name;
+           bindings[i].type = best_inst->type_args[i];
+       }
+       out->as.call_.abi_bindings = bindings;
+       out->as.call_.n_abi_bindings = best_inst->n_type_args;
+   }
return out;
```

**Validation gate after step 1:**

```sh
cmake --build build -j && bash tests/run.sh > /tmp/m4c_step1.txt 2>&1 &
# wait, then:
grep -c '^FAIL ' /tmp/m4c_step1.txt   # MUST be ≤ 172
```

**Expected:** No regression at this step — populating bindings alone
doesn't change codegen. If `emit_abi_register_call` now actually
processes these calls, look for ONE new symbol in the C of an
emit-abi-trace direct call: `__inst_Eq_eq_qu_Tuple2__spec__bool_Tuple2__int__int_Tuple2__int__int`
or similar. Confirm via:

```sh
./build/tur emit-c tests/fixtures/emit-abi-trace/input.tur | grep '__inst_Eq_eq_qu_Tuple2__spec'
```

If the new spec symbol appears, step 1 is mechanically working. If it
doesn't appear, the issue is in step 2/3 below; revisit before
proceeding.

### Step 2 — verify the method binding's type has tyvars

**File to read:** the binding written for the method by
`elab_definstance` (~`src/compiler/elab_typeclasses.c:2520-2610`).
Find where `method_fd->binding->type` is set.

**Question to answer empirically:** is the binding's `type` (a TY_FN)
of `__inst_Eq_eq_qu_Tuple2` parameterized with `(Tuple2 A B)` (TY_APP
with TYVAR args), or is it pre-monomorphized to int64?

```sh
# Add a one-shot fprintf inside elab_definstance after the binding's
# type is set, gated by an env var:
if (getenv("TUR_M4C_DEBUG_BINDING_TYPE")) {
    fprintf(stderr, "M4C: instance %s.%s binding type = %s\n",
            tc_name->name, method_name,
            type_name(method_fd->binding->type));
}
```

**If the binding's signature uses TY_APP with abstract tyvars:** great,
step 3 will be able to substitute. Continue.

**If the binding's signature is pre-mono'd to int64:** the elab is
erasing the tyvars too early. Add a separate "polymorphic shadow type"
field on the FnDef (cf. `poly_type` already exists for some cases) and
populate it with the unsubstituted signature. Step 3 reads from there
instead.

### Step 3 — verify spec generation completes for the dispatch call

After step 1, `emit_abi_register_call` should walk in. Trace:

1. `bindings` is non-empty (from step 1).
2. `fn_binding` is the method's binding (set in elab, line 4018).
3. L989's early-out — `fn_expr` is NULL, `fn_binding->type.kind ==
   TY_FN`, `fn_binding->is_global` should be true for `__inst_*`
   methods. `closure_fn_binding` should be NULL.

If all four pass, the function proceeds to `emit_abi_find_fn_expr`
(line 994) to locate the FnDef expression. The instance method's FnDef
should be findable since elab_definstance emits an EX_FN_DEF for it.
Confirm with:

```sh
TUR_M4C_DEBUG_BINDING_TYPE=1 ./build/tur emit-c tests/fixtures/emit-abi-trace/input.tur 2>&1 | grep M4C
```

After step 3, the spec is emitted. The clone's body re-elaborates with
the substituted parameter types — `x : Tuple2__int__int` instead of
`x : (Tuple2 A B)`. The recursive `(eq? (.e1 x) …)` inside the body
dispatches through the appropriate concrete instance per the existing
dispatch machinery.

**Validation gate after step 3:**

- The C emit for emit-abi-trace contains a `static bool
  __inst_Eq_eq_qu_Tuple2__spec__…(Tuple2__int__int x,
  Tuple2__int__int y)` function with body that does
  `__inst_Eq_eq_qu_int((x).e1, (y).e1)` (no `(intptr_t)` cast on `x`).
- The full suite must remain ≤172 FAIL. (Both the carrier and the spec
  versions are emitted; only emission, no dispatch rewrite yet, so
  nothing should break.)

### Step 4 — rewrite the dispatch emit to call the spec

**File:** `src/compiler/emit_expr.c` — specifically the EX_CALL direct-
call path (not the indirect/fn_expr path). The direct-call path emits
`fn_name(args...)`; we want `fn_name` to be the spec clone's name when
one is available.

The existing machinery (`find_matched_abi_spec`) already does this for
ordinary defns; check whether it's called on the dispatch-from-direct-
binding path. Search:

```sh
grep -n 'find_matched_abi_spec\|matched_spec' src/compiler/emit_expr.c | head
```

If `matched_spec` lookup already runs for direct calls, the spec's
clone_name automatically replaces the bare binding name. Verify no
extra plumbing is needed.

**After step 4, the C should be:**

```c
__inst_Eq_eq_qu_Tuple2__spec__…__Tuple2__int__int_Tuple2__int__int(t1_882, t2_883)
```

with no `(int64_t)(intptr_t)(&__tNN)` spills around the args.

### Step 5 — drop the now-dead bridge stanzas

Run the M3 audit under TUR_M3_AUDIT=1 directly on the previously-bridge-
using fixtures:

```sh
for f in emit-abi-trace typeclass-return-dispatch-result-wrapped typeclass-method-parameterized-result-decode; do
  echo "== $f =="
  TUR_M3_AUDIT=1 ./build/tur build tests/fixtures/$f/input.tur 2>&1 | grep m3-audit
done
```

**Success criterion:** zero `[m3-audit]` lines for these fixtures
(modulo the HKT-class cases, which keep the carrier ABI).

If criterion met, **proceed to M4d (bridge deletion)** per
`docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`'s
"Validation when M4 lands" section.

If not met, the remaining crossings are documented and become the
M4-extension scope (e.g. HKT-class bodies, existential pack/open, etc.).

### Step 5.5 — fixture regen

The dispatch-emit change produces new C for every call site that used
to bridge. Affected fixtures regenerate their `expected.c` snapshots
per CLAUDE.md's "Fixture Snapshots" recipe:

```sh
TUR=./build/tur
for dir in tests/fixtures/*/; do
  if [ -f "$dir/expected.c" ]; then
    input="$dir/input.tur"
    [ -f "$input" ] || input="$dir/$(basename $dir).tur"
    if [ -f "$input" ]; then
      flags=""
      [ -f "$dir/flags" ] && flags=$(cat "$dir/flags")
      "$TUR" $flags emit-c "$input" > "$dir/expected.c" 2>/dev/null
    fi
  fi
done
```

Expected churn: ~30-100 snapshots (estimate from `grep -lE
'__inst_Eq_eq_qu_(Tuple2|Pair|Result)' tests/fixtures/*/expected.c |
wc -l` at start of step).

## Risks and rollback

- **Step 1 regression risk:** low. Adding bindings without changing
  emit shouldn't break anything; if it does, the rollback is reverting
  the elab edit.
- **Step 2 risk:** medium. If the binding's type is pre-mono'd to
  int64, adding a "polymorphic shadow type" field is a non-trivial
  FnDef extension — and gets us back into M4 design territory.
- **Step 3 risk:** medium. If the FnDef can't be found by
  `emit_abi_find_fn_expr`, the spec path bails silently. Verify with
  the env-var trace; if NULL, add the FnDef registration path.
- **Step 4 risk:** low-to-medium. The direct-call path likely already
  uses `find_matched_abi_spec`; if not, this is a small targeted edit.
- **Step 5 risk:** low (mechanical regen).

**Rollback unit:** each step is a single commit. If any step's
validation gate fails, revert that step alone and document the
specific failure mode.

## Plan M5 trigger

Per `docs/upcoming/m4-typeclass-per-method-abi-plan.md`'s "Risks" section,
constrained polymorphic functions (`(defn fold-eq [A] [^&: Eq A] …)`)
currently receive their dict as `void *`. Once M4c lands and dict
structs vary per instantiation, the `void *` path becomes ill-typed
where constrained polymorphic code reads dict slots.

The Plan M5 work — monomorphize constrained polymorphic functions per
type-arg — must land alongside M4c or shortly after. M4c-as-described
above leaves the dict struct untouched (the spec works around the dict
rather than rewriting it), so the M5 trigger may not fire until later
phases revisit the dict layout. Confirm during step 5 validation.

## Related

- [m4-typeclass-per-method-abi-plan.md](m4-typeclass-per-method-abi-plan.md)
- [m4a-audit-findings.md](m4a-audit-findings.md)
- [m4b-handoff.md](m4b-handoff.md)
- [../reported/m4c-stdlib-carrier-helpers-block-dispatch-rewrite.md](../reported/m4c-stdlib-carrier-helpers-block-dispatch-rewrite.md)
  — M4c-pre's unblock; resolved.
- [../reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](../reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
  — the final M3 deletion this whole chain unblocks.
- `src/compiler/elab_typeclasses.c:4009` — step 1 site.
- `src/compiler/emit_module.c:951` — `emit_abi_register_call`.
- `src/compiler/emit_module.c:959` — the early-out abi_bindings check.
- `src/compiler/emit_expr.c` — step 4 emit path (`find_matched_abi_spec`).
