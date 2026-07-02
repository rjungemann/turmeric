# `ok`/`some` float payload value-converts into the int64 carrier result-box

**Severity:** low (float-only; int/cstr/struct payloads round-trip correctly;
surfaces only when a constrained class method declares a parametric
`(Result a cstr)` / `(Option a)` result that stays on the legacy carrier box
and the element instantiates to `float`).

## Summary

When a `#{Construct}` (`ok`/`some`) stores a `float` payload into the legacy
`tur_result_box_t` / option carrier (an `int64_t` field) the spec body passes
the `double` straight into the `int64_t` ctor parameter -- a C **value**
conversion (`2.5 -> 2`) -- while every reader of that field **bit-reinterprets**
it back (`(union { int64_t s; double d; }){.s = field}.d`). The two halves
disagree, so a float round-tripped through the carrier box comes back as
garbage (`9.88131e-324`).

## Minimal repro

`tests/fixtures/constrained-defn-cons-return-monomorphize` (under
`TUR_FORCE_LOWER=1`) -- the last line:

```turmeric
(defclass C [a] (one [i : int] : (Result a cstr)))
(definstance C [float] (one [i] (ok 2.5)))
...
(println (thead (:: (rec 0 2) (Cons float))))   ;; expects 2.5, prints garbage
```

The other 7 output lines (int/cstr list-lengths + the cstr `thead`) are
correct; only the float `thead` is wrong.

## Root cause (file:line)

`emit-c` of the float `ok` spec:

```c
static int64_t ok__spec__int64_t_double(double x) {
    return ctor_Result(true, x, (int64_t){0});   /* x:double -> int64_t param */
}
```

`ctor_Result`'s `ok_val` parameter is `int64_t` (the legacy
`tur_result_box_t { bool is_ok; int64_t ok_val; int64_t err_val; }`), so `x` is
value-converted, not bit-reinterpreted. The consumer side already does the
reinterpret (see the generated `ok_val__spec__*_Result__float__cstr` body and
the `rec` spec call site), so the fix is to make the **producer** agree: when a
construct stores an inline-reinterpret scalar (`carrier_is_inline`, i.e.
float/float32/sub-int64) into a carrier `int64_t` field, bit-reinterpret it
(`((union { double s; int64_t d; }){.s = x}).d`) instead of passing it raw.

This is the construct-arg boxing analogue of `emit_carrier_bridge`'s
concrete->carrier inline-scalar case (`carrier_is_inline`,
src/compiler/emit_core.c ~L3217); the construct-template ctor-call emit does not
route its payload args through that bridge.

## Fix directions

- In the `#{Construct}` ctor-call arg emit, when the payload's concrete type is
  an inline-reinterpret scalar (`carrier_is_inline`) and the target ctor field
  is the int64 carrier, wrap the arg in the same `union`-reinterpret
  `emit_carrier_bridge` uses for a concrete->carrier inline crossing. cstr /
  ptr / int payloads need no wrap (already int64-compatible), matching the
  bridge's existing behavior, so the change is float/float32/sub-int64-only and
  should not disturb the (passing) int/cstr paths.
- Scope note: this is the *legacy carrier* result box. The by-value Result/
  Option path constructs the aggregate directly and is unaffected; the bug only
  bites when the parametric class-method result stays carrier-boxed.

## Status

**Resolved.**

- **Float carrier boxing fix:** As outlined in the Fix directions, the float-into-carrier-box value-conversion issue has been fully resolved. In `src/compiler/emit_expr.c` (at `EX_MAKE_STRUCT` / constructor arg emission), the compiler now correctly wraps float, float32, and float64 arguments destined for a carrier-erased `int64_t` constructor field in a bit-reinterpreting union wrapper (similar to `emit_carrier_bridge`), preventing truncation or numerical coercion.
- **Seam resolution:** An outstanding residual `-Wint-conversion` compilation warning/error on modern Clang that blocked the `cstr` path of the monomorphized test fixture `constrained-defn-cons-return-monomorphize` has also been resolved. In `src/compiler/emit_fns.c`, the compiler now evaluates the resolved type (via `emit_resolve_type`) of the return value expression. When it resolves to a pointer, `cstr`, or function type returning through the `int64_t` carrier return slot, it is correctly cast to `(int64_t)(intptr_t)` to preserve correctness and compile cleanly.
- **Verification:** The entire `constrained-defn-cons-return-monomorphize` test fixture now passes perfectly under both compilation (`tests/run.sh`) and interpreter (`tests/run-turi.sh`) suites.
