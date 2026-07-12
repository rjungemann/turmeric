---
status: resolved
severity: low
discovered: 2026-07-07
discovered-by: catch-unwind-thunk-closure-leak fix (escape-case test)
resolved: 2026-07-08
area: compiled backend (catch-unwind, carrier/by-value return bridge)
---

# Returning a `catch-unwind` Result directly as a function's by-value `(Result ...)` result miscompiles

## Resolution (2026-07-08)

The function-return path now bridges the carrier box to the by-value aggregate
when the returned value is a `catch-unwind` box.

- `emit_fns.c` gains `fn_return_needs_carrier_result_bridge`: true when the
  function returns a by-value Result/Option struct (`ret_ctype` is that struct,
  not the int64 carrier) AND its linear tail (through `let`/`do`/ascription) is a
  `catch-unwind` / `catch-panic-of` box -- returned directly, or through a
  let-bound variable whose initializer is one (`fn_body_linear_tail` +
  `fn_body_binding_init`).  `tur_catch_unwind_box` always yields the int64 heap
  box, so this tail always needs the bridge.
- When it fires, the return emits
  `emit_carrier_bridge(CK_CARRIER, CK_CONCRETE, <declared return type>)`, whose
  canonical field-by-field box readback reconstructs the aggregate
  (`(Result__int__int){.is_ok = box->is_ok, .ok_val = box->ok_val, ...}`).  A
  raw `*(T*)` deref is deliberately not used -- it would misread a Result with
  sub-word payloads.

The gate is **structural** (does the tail come from a catch boundary) rather than
representational (does `emit_type_c_name` say "int64_t").  An earlier
representational cut mis-fired on the ordinary Result constructors: `ok`/`err`'s
`#{Construct}` body type also collapses to the int64 carrier under
`emit_type_c_name`, yet it EMITS the by-value aggregate -- bridging it derefed an
aggregate as a pointer (`aggregate value used where an integer was expected`).
Keying on the catch-box tail avoids the constructor path entirely (those stay on
the M4c/M5 branches).

Verified: the repro and a let/direct/caught fixture all compile and run
correctly (`ok?`/`err?` observe the right tag); no use-after-free under valgrind.
Regression fixture `tests/fixtures/catch-unwind-byvalue-result-return`.
`bash tests/run.sh`: 1976 passed, 0 failed.

### Remaining

The copied-out box is not freed after the bridge (a bounded per-return leak of
the `tur_result_box_t`), since a returned box has no single provable owner at
that point; freeing an arbitrary carrier Result there risks a double free. An
`if`/`match` tail whose arms are catch boxes is also not bridged (the linear-tail
walk stops at a branch); both are uncommon and left as-is.

---


## Summary

A function whose body's tail is a `let` binding a `catch-unwind` Result and then
returning that box fails to compile: the `let` value is emitted as the `int64_t`
carrier (the box pointer), but the function's declared return type is the
by-value aggregate `tur_adt_Result__int__int`, so `cc` rejects the `return`.

Pre-existing (not a regression); found while writing an escape-case test for the
thunk/box leak fix -- it is orthogonal to that leak work.

## Minimal repro

```turmeric
(defn make [] : (Result int int)
  (let [r (catch-unwind (fn [] : int 5))]
    r))
(defn main [] : int
  (let [r (make)]
    (if (ok? r) (ok-val r) 0)))
```

```sh
TUR=./build/tur
$TUR build /tmp/ret_box.tur -o /tmp/ret_box
# cc error: incompatible types when returning type 'int64_t'
#           but 'tur_adt_Result__int__int' was expected
```

## Root cause

`tur_catch_unwind_box` yields the box as `int64_t`, and `emit_let_value` declares
the binding as the `int64_t` carrier. When the `let` is a function-body tail
whose result type is a carrier-ABI aggregate emitted by value, the trailing
`return <let-value>` needs the carrier->by-value bridge that `emit_if_value`
/ `bridge_control_value_to_byvalue_temp` apply for other control forms, but the
box carrier here reaches the `return` without it.

## Fix directions

Bridge the `let`-tail carrier to the declared by-value aggregate at the return
site (the do/let/if companion of the existing
`fn_body_tail_byvalue_carrier_type` recovery), or represent the caught Result
binding as the by-value aggregate when the enclosing function returns it by
value. Bounded and low severity: catching then directly returning the raw
Result is uncommon (callers usually inspect it in place).
