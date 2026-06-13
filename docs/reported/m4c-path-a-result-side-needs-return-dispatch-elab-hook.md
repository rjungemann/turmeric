---
title: M4c Path A result-side substitution needs an elab hook for return-dispatch typeclass methods
category: Codegen / ABI — monomorphization plan refinement
severity: Low. Path A landed for arg-side substitution (commits `0fd565fe` and earlier); `emit-abi-trace` now has zero bridge crossings. The two residual `carrier→concrete` crossings on `typeclass-return-dispatch-result-wrapped` and `typeclass-method-parameterized-result-decode` follow a different elab path that doesn't reach `emit_abi_register_call` at all — a probe with `TUR_M4C_RES=1` instrumentation confirmed `__inst_Dec_dec_int` is never seen by the spec scanner.
description: For typeclass methods like `(defclass Dec [a] (dec [v : int] : (Result a cstr)))` — where the class variable appears in the **return type** rather than a param — dispatch is "return-dispatched": the ascription `(:: (dec 42) (Result int cstr))` picks the instance. The dispatch call's EX_CALL is constructed by a different elab path than the receiver-dispatched case Path A.1 hooked. As a result, `out->as.call_.abi_bindings` is never populated for return-dispatch calls, `emit_abi_register_call` never reaches them, and no per-instantiation spec is emitted. The bridge therefore still fires at the EX_ASCRIBE site to unbox the int64 carrier dispatch result.
status: OPEN. Path A's arg-side mechanism is sound and shipped; the return-side needs a parallel elab hook (in the return-dispatch path of `elab_typeclasses.c`) to populate `abi_bindings` the same way the receiver-dispatch path does. Then the existing `emit_abi_register_call` machinery should mint the right spec without further emit changes.
---

# Return-dispatch typeclass calls need a parallel Path A.1 elab hook

## What's working

`emit-abi-trace`'s `(.eq? t1 t2)` dispatch on `Tuple2 int int` is now a
direct call to
`__inst_Eq_eq_qu_Tuple2__spec__bool_Tuple2__int__int_Tuple2__int__int(t1,
t2)` — no bridge stanzas. The fixture audits clean under
`TUR_M3_AUDIT=1`. All 6 stdlib `Eq` instances on parametric containers
(Vec, Cons, Map, MutableMap, Option, Set) work via the bridge
extension at `emit_expr.c:2549-2580`.

## What's still bridging

```
$ TUR_M3_AUDIT=1 ./build/tur build tests/fixtures/typeclass-return-dispatch-result-wrapped/input.tur 2>&1 | grep m3-audit
[m3-audit] bridge carrier->concrete type=(type-app (type-app Result int) cstr)
[m3-audit] bridge carrier->concrete type=(type-app (type-app Result cstr) cstr)

$ TUR_M3_AUDIT=1 ./build/tur build tests/fixtures/typeclass-method-parameterized-result-decode/input.tur 2>&1 | grep m3-audit
[m3-audit] bridge carrier->concrete type=(type-app (type-app Result int) cstr)
```

Both fixtures have the shape

```turmeric
(defclass Dec [a] (dec [v : int] : (Result a cstr)))
(definstance Dec [int]  (dec [v] (ok v)))
(definstance Dec [cstr] (dec [v] (ok "hello")))

(let [r (:: (dec 42) (Result int cstr))]
  …)
```

The class variable `a` is in the **return type**, not a param. Dispatch
is resolved by the ascription `(:: (dec 42) (Result int cstr))` picking
the `Dec[int]` instance. The emitted C:

```c
ok_val__spec__int64_t_Result__int__cstr(
    (*(Result__int__cstr *)(intptr_t)(__inst_Dec_dec_int(INT64_C(42)))))
```

`__inst_Dec_dec_int(int64_t v) → int64_t` is the carrier-ABI dispatch
target. The `(*(Result__int__cstr *)(intptr_t)(…))` is the bridge
unboxing the int64 to a by-value `Result__int__cstr`.

## Why Path A doesn't fire

A debug probe inside `emit_abi_register_call`:

```c
if (getenv("TUR_M4C_RES") && fn_binding->name
    && strncmp(fn_binding->name->name, "__inst_Dec", 10) == 0) {
    fprintf(stderr, "[m4c-res-entry] %s n_bindings=%u dict_arg=%p\n", …);
}
```

prints nothing across the whole emit of either fixture. **The
`__inst_Dec_dec_int` call never reaches `emit_abi_register_call`** —
no spec is even scanned.

The reason is in `src/compiler/elab_typeclasses.c`: the
receiver-dispatch path at line 4009 (where Path A.1 hooks
`abi_bindings`) handles methods like `(eq? x y)` where the class var is
the receiver. Return-dispatch methods like `(dec v) : (Result a cstr)`
take a separate elaboration path — they're resolved by the surrounding
ascription's expected type, not by walking the receiver's type. That
separate path doesn't call into the same EX_CALL-construction site, so
my Path A.1 edit doesn't run.

A grep for "return-dispatch" / "return_dispatch" in
`elab_typeclasses.c` locates the relevant codepath (the existing
`method_is_return_dispatch(...)` helper is the discriminator).

## What's needed

Path A.1's mechanism is sound — the same `abi_bindings`-driven
substitution in `emit_abi_register_call` already handles return-type
specialization (the existing path at `emit_module.c:1094-1108` runs
`emit_abi_instantiate_type` on `result_type` from the call's bindings).
The missing piece is one elab edit on the return-dispatch path:

1. Locate the EX_CALL construction in `elab_typeclasses.c`'s return-
   dispatch path (search for `method_is_return_dispatch(...)`).
2. Mirror the abi_bindings population from line 4047 (the
   receiver-dispatch path), binding the class variable to the
   ascription's pinned type (the `expected_type` channel — for `(:: e
   (Result int cstr))`, the `Result int cstr` is what pins
   `a → int`).
3. The existing `emit_abi_register_call` then mints a
   `__inst_Dec_dec_int__spec__…` clone whose return type is
   `Result__int__cstr` by-value, the dispatch site calls it directly,
   and the EX_ASCRIBE bridge becomes dead code at that call.

Once both return-dispatch sites stop firing the bridge, the M3 audit
should report zero crossings across all 3 originally-blocking fixtures,
unblocking the bridge deletion in
`docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`.

## Validation when this lands

```sh
for f in emit-abi-trace typeclass-return-dispatch-result-wrapped \
         typeclass-method-parameterized-result-decode; do
  echo "== $f =="
  TUR_M3_AUDIT=1 ./build/tur build tests/fixtures/$f/input.tur 2>&1 | grep m3-audit
done
```

Expected: **zero `[m3-audit]` lines.**

When that holds, proceed to delete `emit_carrier_bridge` per
`docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`'s
"Validation when M4 lands" section.

## Related

- [docs/upcoming/m4c-execution-plan.md](../upcoming/m4c-execution-plan.md)
  — Path A's overall execution plan.
- [docs/reported/m4c-path-a-cascades-into-stdlib-eq-instances.md](m4c-path-a-cascades-into-stdlib-eq-instances.md)
  — Path A arg-side landing (resolved).
- [docs/reported/m4c-class-var-erased-at-instance-elab.md](m4c-class-var-erased-at-instance-elab.md)
  — Path A's original arg-side analysis (resolved).
- [docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
  — the M3 deletion gate this report aims at.
- `src/compiler/elab_typeclasses.c:4047` — receiver-dispatch
  abi_bindings population (Path A.1's hook).
- `src/compiler/elab_typeclasses.c` — `method_is_return_dispatch(...)`
  marks the codepath that needs the parallel hook.
- `src/compiler/emit_module.c:1094-1108` — existing result-type
  substitution that will work for return-dispatch once bindings are
  populated.
