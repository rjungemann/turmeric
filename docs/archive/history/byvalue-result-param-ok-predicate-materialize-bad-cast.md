# Paper trail: by-value Result parameter + carrier-`:int` predicate

Resolved 2026-07-04. Companion to the resolved report at
`docs/archive/byvalue-result-param-ok-predicate-materialize-bad-cast.md`.

## Diagnosis

`(defn f [r : (Result int cstr)] : int (if (ok? r) 1 0))` emitted:

```c
static int64_t f(const tur_adt_Result__int__cstr * r) {   // r is a POINTER (pbp: >16 bytes)
    tur_adt_Result__int__cstr __t56 = r;                  // BUG 1: aggregate = pointer
    if (ok_qu((*((int64_t)(intptr_t)(&__t56))))) { ... }   // BUG 2: extra `*` deref of an int64
}
```

Two independent arg-emission transforms fired on the same argument (traced with
a temporary `TUR_M3_AUDIT`/`TUR_DBG_BR` instrumentation, since removed):

1. The concrete->carrier arg bridge in `emit_expr.c` -- the block gated on
   `emit_arg->type.kind == TY_APP && adt_app_is_byvalue_product(...)` with a
   carrier param kind (here `ok?`'s param is `r : int`, `pk == TY_INT`) -- called
   `emit_carrier_bridge(CK_CONCRETE, CK_CARRIER)`, whose aggregate path spills
   `cname __t = src;` + `(int64_t)(intptr_t)(&__t)`. With `src == "r"` and `r` a
   `const T *`, the spill is `aggregate = pointer` (cc: invalid initializer).
2. The pass-by-ptr `(*(%s))` deref (`expr_is_pbp_param` + inline-C/extern-C
   callee + `type_struct_pass_by_ptr`), meant to hand a by-value copy to a callee
   that takes the struct by value, wrapped the bridge's output in another `*`.

`ok?` is `(defn ok? [r : int] #fx{} : bool ```c return tur_is_ok(r); ```)` -- a
carrier-`:int` sink, not a by-value-struct callee, so the pass-by-ptr deref was
never appropriate for it in the first place.

The Option analogue does not trip it: `(Option int)` is <=16 bytes so its param
is passed by value, and `some?` resolves to a by-value spec
`some___spec__bool_tur_adt_Option__int(o)` -- the carrier is never formed.

## Fix

Key insight: a pass-by-pointer struct *parameter* is a pointer to the by-value
aggregate, which IS a valid carrier handle -- the non-pbp branch of
`emit_carrier_bridge` spills the value precisely to hand back `&tmp`. And the
by-value `tur_adt_Result__int__cstr` layout (`{bool is_ok; int64_t ok_val;
const char *err_val}`) coincides with the carrier `tur_result_box_t` (`{bool
is_ok; int64_t ok_val; int64_t err_val}`) -- `is_ok` at offset 0, `ok_val` at 8
-- so `tur_is_ok` / `ok-val` / `err?` / `err-val` read a pointer to the by-value
aggregate correctly.

So the crossing is a plain `(int64_t)(intptr_t)(r)` cast, no spill. In the
carrier-param arg-bridge block:

```c
if (expr_is_pbp_param(ctx, emit_arg)) {
    raw = "(int64_t)(intptr_t)(" raw ")";   // pointer IS the carrier handle
    pbp_carrier_cast = true;
} else {
    raw = emit_carrier_bridge(ctx, body, raw, CK_CONCRETE, CK_CARRIER, emit_arg->type);
}
```

and the later pass-by-ptr `(*(...))` deref is guarded on `!pbp_carrier_cast` so
the two transforms no longer compound. A genuine by-value-struct callee (Tuple3+
into an inline-C/extern-C formal) still gets the deref: there the carrier-cast
block does not run (its param is a concrete struct, not the int64 carrier), so
`pbp_carrier_cast` stays false.

## Scope / caveat

The layout coincidence holds for 8-byte-shaped payloads (`int`, `cstr`, `ptr`,
pointer-boxed value). For a genuinely sub-word Result payload (`bool`,
`int8/16/32`, `float32`) the by-value aggregate packs at native widths while the
carrier box uses int64 slots, so `ok-val`/`err-val` (later fields) would misread
-- but `ok?`/`err?` (reading only `is_ok` at offset 0) are always fine, and the
same width caveat already applies to every carrier<->by-value crossing in the
tree. The prior behaviour did not compile at all; this makes the common
(pointer-shaped-payload) case correct.

## Verification

`tests/fixtures/byvalue-result-param-predicate/` (stdout): `ok?`/`ok-val`/`err?`
on `(Result int cstr)` params, ok and err values. Also checked out-of-tree:
`(Result cstr int)`, and the Option control still compiles/runs.

`bash tests/run.sh` -> `1934 passed, 0 failed`.
