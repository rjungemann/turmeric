---
status: open
severity: low
discovered: 2026-07-07
discovered-by: catch-unwind-thunk-closure-leak fix (escape-case test)
area: compiled backend (catch-unwind, carrier/by-value return bridge)
---

# Returning a `catch-unwind` Result directly as a function's by-value `(Result ...)` result miscompiles

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
