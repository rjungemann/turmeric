# Generic instance returning a `Result` with a by-value-struct ok-arm miscompiles (int64 carrier vs struct)

> **RESOLVED 2026-06-22.** Fixed in `src/compiler/emit_expr.c`
> (`emit_control_result_temp_decl`). The wrapping `let`/`do` result temp now
> recovers the by-value carrier-ABI aggregate type from the matched ABI spec
> (via `fn_body_tail_byvalue_carrier_type`), exactly as `emit_if_value` already
> did -- so the outer `let` temp and the inner `if` temp agree on representation
> instead of straddling `int64_t` vs the by-value struct.
>
> **Validation.** The repro below (and the by-value-struct / control variants)
> compile and run, printing `5` / `42`. The compiled suite is green
> (`summary: 1752 passed, 0 failed`). Against turmeric-spices json, the
> `Decode [Option]` instance now emits `Result__Option__int__cstr __t54;`
> (by-value) for both `int` and `cstr` element types and reaches the C compile;
> the only remaining failure there is the unrelated missing `yyjson.h` system
> header (a network-fetched cmake-dep), not a codegen mismatch.
>
> **Root cause.** A control form's result temp is declared by
> `emit_control_result_temp_decl`. Its int64-carrier branch is gated on
> `!fn_body_tail_emits_byvalue_carrier_abi`, whose leaf check
> (`expr_emits_byvalue_carrier_abi`) bails early when the construct call's own
> `e->type` is collapsed to the int64 carrier (`TY_INT`) -- so it reports the
> tail as *not* by-value even though the matched `ok`/`err` spec returns the
> by-value `Result__Option__int__cstr`. `emit_if_value` already avoided this by
> consulting `fn_body_tail_byvalue_carrier_type` (which recovers the type from
> the matched spec and does not gate on the collapsed `e->type`), so the inner
> `if` temp came out by-value while the outer `let` temp came out `int64_t`. The
> fix teaches `emit_control_result_temp_decl` to prefer the same
> by-value-carrier type, keeping the two halves consistent end to end.
>
> Original report follows.

Repo: rjungemann/turmeric
File to create: docs/reported/generic-instance-result-byvalue-struct-okarm-carrier-mismatch.md
Found by: turmeric-spices Track C U2 Phase B (json Decode container instances)
Verified on: turmeric 0.22.0, main @ c153600, built from source (build-release)
Severity: Medium. Blocks json `Decode [(Option A)]` (and `Decode [Map str]`,
  same shape). `Decode [Vec]` is unaffected -- Vec is pointer/int-carried, so
  its `(Result (Vec A) cstr)` ok-arm is scalar.

## Summary

A generic constrained instance whose method returns a `Result` with a
BY-VALUE-STRUCT ok-arm parameterized by the instance's type variable -- e.g.
`(Result (Option A) cstr)` -- type-checks but fails at C codegen. The specialized
instance assigns/returns the whole by-value `Result` struct through an `int64_t`
carrier slot:

    error: incompatible types when assigning to type 'int64_t'
           from type 'Result__Option__int__cstr'
    error: incompatible types when returning type 'int64_t'
           but 'Result__Option__int__cstr' was expected

(An earlier formulation using `result-map` instead of a manual re-wrap produced
the dual cast `(Option__int)(int64_carrier)` -- "conversion to non-scalar type
requested" -- same root: the by-value struct is round-tripped through the int64
carrier in the generic-instance specialization.)

The same `Result` type built in a plain (non-generic, non-instance) function
works correctly, so the defect is specific to generic-instance specialization
when the Result's ok-arm is a by-value struct.

## Repro (no inline C)

    ;; a generic constrained instance returning (Result (Option A) cstr)
    (defclass Producer [a] (produce [seed : int] : (Result a cstr)))

    (definstance Producer [int]
      (produce [seed] (:: (ok seed) (Result int cstr))))

    ;; generic over the container; selected by the ascribed return type
    (definstance Producer [Option] [(Producer A)]
      (produce [seed]
        (let [r (:: (produce seed) (Result A cstr))]
          (if (.is-ok r)
            (:: (ok (some (ok-val r))) (Result (Option A) cstr))
            (:: (err (err-val r))      (Result (Option A) cstr))))))

    (defn main [] : int
      (let [r (:: (produce 5) (Result (Option int) cstr))]
        (if (.is-ok r)
          (if (.is-some (ok-val r)) (println (:: (.value (ok-val r)) :int)) (println "none"))
          (println "err"))
        0))

`tur check` is clean; `tur run` fails at the C compile with the two
`incompatible types ... int64_t ... Result__Option__int__cstr` errors above,
inside `__inst_Producer_produce_Option__spec__Result__Option__int__cstr_*`.

## Control (works)

The identical Result type, branches, and accessors in a plain function compile
and run (prints 5):

    (defn f [b : bool] : (Result (Option int) cstr)
      (if b (:: (ok (some 5)) (Result (Option int) cstr))
            (:: (err "x")      (Result (Option int) cstr))))
    (defn main [] : int
      (let [r (f true)]
        (if (.is-ok r)
          (if (.is-some (ok-val r)) (println (:: (.value (ok-val r)) :int)) (println "none"))
          (println "err"))
        0))

And the analogous generic instance with a POINTER/int-carried ok-arm --
`(Result (Vec A) cstr)` -- compiles and runs correctly (json `Decode [Vec]`),
which is why only the by-value-struct ok-arm (`Option`, and `Map`) is affected.

## Expected

A generic instance returning `(Result (Option A) cstr)` should thread the
by-value `Result` struct by value (as the concrete-function case already does),
not assign/return it through an `int64_t` carrier slot.

## Impact

json U2 Phase B: `Decode [Vec]` works and is ready to land; `Decode [(Option A)]`
and `Decode [(Map str A)]` are blocked until a generic instance can return a
Result whose ok-arm is a by-value struct. Workaround would be per-element-type
concrete instances (combinatorial), so the generic instances are held pending
this fix.
