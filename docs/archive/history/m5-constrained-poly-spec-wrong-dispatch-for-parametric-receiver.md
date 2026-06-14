---
title: M5 constrained-polymorphic spec body picks wrong Eq instance for parametric receiver
severity: silent miscompile (would be hard cc error) -- FULLY FIXED (elab + emit)
date: 2026-06-13
---

## Update (same day, follow-up commits)

**Elab-side root cause identified and patched.**  Two related bugs in
`elab_typeclasses.c` and `typeclass.c`:

1. **Incomplete `is_primitive` list at the obj_ck determination
   (`elab_typeclasses.c:3640-3647`) and the KIND_ARROW iteration
   (`elab_typeclasses.c:3675-3679`).**  Only TY_INT/TY_FLOAT/TY_CSTR/...
   were listed; sized variants TY_INT8/TY_FLOAT32/... weren't.  A
   receiver of type `:float32` got `obj_ck = KIND_ARROW` (treated as a
   type constructor) and an `Eq float32` instance got
   `type_ok = !inst_is_primitive = true` (treated as non-primitive), so
   the iteration silently matched primitive instances against parametric
   receivers.  Symmetric fix in `typeclass.c:354-359` for the
   `typeclass_env_lookup_instance_by_key` KIND_ARROW path.

2. **Constraint check at `typeclass.c:typeclass_instance_constraints_
   satisfied` rejected TYVAR-substituted constraints.**  For `(Vec A)`
   receiver with the `Eq Vec` instance's `(Eq A)` constraint, the
   required_type resolved to a TYVAR and the env lookup returned NULL
   ("no instance for TYVAR").  The constraint failed, `Eq Vec` was
   silently dropped, and the iteration fell through to a primitive
   fallback.  Fix: when the substituted `required_type.kind == TY_TYVAR`,
   tentatively accept -- the outer defn's own constraint
   (`defn f [A] [(Eq A)] ...`) guarantees the instance will exist at
   every monomorphization site; the spec emit re-resolves through the
   concrete A via `emit_reresolve_method_call`.

After these two fixes, the spec body now binds the correct method:
`__inst_Eq_eq_qu_Vec__spec__bool_Vec__int_Vec__int` (the per-Vec-int
Path A spec of `Eq Vec`).  Suite stays at 1562/88 (~3 below baseline,
all flake variance -- zero snapshot diffs, confirmed by full
regen-and-compare).

## Resolved: emit-side arg-bridge direction (commit follow-up to a301229e)

`find_matched_abi_spec` now consults `specialized_call_exprs[]` first
(emit_expr.c:25-50).  When emit_module.c has interned a spec for this
exact call Expr*, the spec is looked up by `clone_name` instead of by
type-matching `spec->arg_types[i]` against the IR call's still-abstract
`args[i]->type`.  The arg-bridge at emit_expr.c:2587 then sees
`matched_spec != NULL` and skips the carrier coercion, leaving the
by-value `Vec__int` args alone.

Fixture: `tests/fixtures/m5-constrained-poly-vec-eq/`.  Suite remains
at 1563/88 (was 1562/88 -- +1 is the new fixture; identical FAIL set).

## Original analysis (preserved for context)

The probe still doesn't compile end-to-end -- the call's args still go
through `CK_CONCRETE -> CK_CARRIER` bridging (the inner call site
treats the callee as the carrier-ABI `__inst_Eq_eq_qu_Vec` binding,
not the by-value spec name that `emit_reresolve_method_call` produces).
The emitted C:

```c
return __inst_Eq_eq_qu_Vec__spec__bool_Vec__int_Vec__int(
    (int64_t)(intptr_t)(&__t28),    // <-- bridge-to-carrier
    (int64_t)(intptr_t)(&__t29));   // <-- bridge-to-carrier
```

passes `int64_t` to a `Vec__int` formal -- cc error.

The two emit decisions (call-name re-resolution at `emit_core.c:967` /
arg-bridging at `emit_expr.c:2511-2622`) are made independently and
disagree on which target ABI is in effect.  The arg-bridging path
should consult the same per-call spec-target decision the name
re-resolution uses; or alternatively, the name re-resolution should
emit the carrier-target name when args have been bridged to int64.

This is the M5 emit-side follow-up.  Tracked separately so the elab
fix can ship without it.

---

## Summary

A constrained-polymorphic defn `(defn f [A] [(Eq A)] [x : (Vec A) ...] (eq? x y))`
specializes correctly for non-parametric `A` (int, cstr, float) but
emits a **wrong** dispatch when the receiver type is itself parametric
in `A`, e.g. `(Vec A)` instantiated at `A = int`.

## Minimal repro

```turmeric
(defn check-eq-vec [A] [(Eq A)] [x : (Vec A) y : (Vec A)] : bool
  (eq? x y))

(defn main [] : int
  (let [a (:: (vec-of) (Vec int))
        b (:: (vec-of) (Vec int))]
    (do
      (vec-push! a 1)
      (vec-push! b 1)
      (if (check-eq-vec a b) 0 1))))
```

## Observed

```c
static bool check_eq_vec__spec__bool_Vec__int_Vec__int(Vec__int x, Vec__int y) {
    return __inst_Eq_eq_qu_float32(x, y);  // WRONG: float32 instance with Vec__int args
}
```

The cc error caught by ASan build:

```
error: passing 'Vec__int' (aka 'struct Vec__int') to parameter of
incompatible type 'float'
    return __inst_Eq_eq_qu_float32(x, y);
                                   ^
note: passing argument to parameter 'x' here
    static bool __inst_Eq_eq_qu_float32(float x, float y) { ... }
```

## Expected

The spec body should dispatch through the `Eq Vec` instance method
(which itself has a per-Vec-element-type spec from M4c Path A):

```c
static bool check_eq_vec__spec__bool_Vec__int_Vec__int(Vec__int x, Vec__int y) {
    return __inst_Eq_eq_qu_Vec__spec__bool_Vec__int_Vec__int(x, y);
}
```

`__inst_Eq_eq_qu_Vec__spec__bool_Vec__int_Vec__int` is emitted by the
M4c Path A specialization machinery for the `Eq Vec` instance; the
M5 spec body just needs to find it.

## Why this works for non-parametric A

For `(eq? x y)` with `x : A` (bare TY_TYVAR receiver):
- A=int: spec is `check_eq_int__spec__bool_int64_int64`, dispatches to
  `__inst_Eq_eq_qu_int`. Correct.
- A=double: spec is `check_eq_int__spec__bool_double_double`,
  dispatches to `__inst_Eq_eq_qu_float`. Correct.

For `(eq? x y)` with `x : (Vec A)` (parametric receiver), only the
parametric case mis-dispatches. The receiver lookup at elab time sees
`TY_APP(fn=Vec, arg=TY_TYVAR)` and picks `Eq Vec` — that's correct.
The bug is somewhere later: the **substituted** call-site binding for
the spec body, when A binds to int, picks `Eq float32` instead of
re-resolving through `Eq Vec`'s own per-A spec.

The `float32` choice is suspicious — float32 is alphabetically near
the start of the instance list, suggesting the elab/emit falls back
to index 0 of a sorted-by-name instance set when the proper resolution
fails.

## Root cause hypothesis

The spec body re-elaboration / per-spec emit walks the body's
`EX_CALL`s and tries to substitute the typeclass-method binding
(`__inst_Eq_eq_qu_X`) with one matching the spec's bound A. For a
**bare-tyvar receiver** (`x : A`), the substitution sees A=int and
picks `Eq int`'s impl directly. For a **parametric receiver**
(`x : (Vec A)`), the substitution doesn't know to first resolve
`(Vec int)`, then look up the matching `Eq Vec` instance, then thread
through that instance's own Path A spec.

Likely lives in `emit_module.c`'s spec emit path (the per-spec
re-traversal of the body) or in `emit_typeclasses.c`'s instance-method
binding selector when the call site's receiver is in spec-bound
TY_APP shape.

## Workaround

None at the user level — the call simply fails to compile. The
current stdlib avoids this shape because every constrained-polymorphic
Eq consumer uses a bare-tyvar receiver, not a parametric one.

## Severity

Without `Vec`/`Map`/`Option`-aware constrained-polymorphic helpers,
M5 is incomplete. The whole point of the plan's example
`(defn fold-eq [A] [^&: Eq A] [xs : (Vec A) y : A] ...)` requires this
to work — and the example currently mis-compiles in the same way.

## Related

- `docs/upcoming/end-to-end-monomorphization-plan.md` — M5 phase.
- `docs/upcoming/monomorphization-audit.md` — Section 10 step 8.
- `src/compiler/emit_module.c:951-1170` — `emit_abi_register_call` /
  spec interning.
- `src/compiler/elab_typeclasses.c:4042` — Phase H §1 direct call
  resolution.
