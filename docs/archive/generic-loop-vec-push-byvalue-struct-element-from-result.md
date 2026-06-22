# Generic loop pushing a Result-decoded by-value-struct element into a `(Vec A)` miscompiles

> **RESOLVED 2026-06-22.** Fixed across three emit-side seams; the by-value
> intersection now compiles and runs. Validation: the verbatim repro prints `3`;
> a new regression fixture
> `tests/fixtures/constrained-loop-vec-push-byvalue-result-element/` decodes
> indices `0..2` into a `(Vec (Option int))` and reads the stored values back
> (`3 / 0 / 1 / 2`). Controls A (scalar element) and B (direct by-value push)
> still pass, as does the two-int-param `Decode [doc:int val:int]` shape that
> mirrors the json spice's real class. Full compiled suite: **1756 passed, 0
> failed** (1755 + the new fixture).
>
> **Root cause -- three independent defects that only ganged up for the by-value
> struct element** (scalar and `:heap`/pointer elements dodged all three):
>
> 1. **Nested-instance redirect assumed param 0 was the dispatch receiver.**
>    `emit_abi_try_nested_instance_dispatch_redirect` (`src/compiler/emit_module.c`)
>    forced `arg_types[0] = *resolved`, which is right for a receiver-dispatched
>    method (`Enc`/`enc [x : a]`) but wrong for a **return-dispatched** one
>    (`Dec`/`dec [seed : int] : (Result a cstr)`): `seed` is always `int`. The
>    spec was minted taking `Option__int seed`, and the re-dispatch then passed an
>    `Option__int` where the inner `int` instance expected `int64_t`. Fix: detect
>    the return-dispatched class type variable (mirrors
>    `method_is_return_dispatch` in `elab_typeclasses.c`); for such a method every
>    parameter keeps its own declared type, and the result family is recovered by
>    substituting that class variable with `resolved` into the **class** method's
>    declared return (`a := (Option int)` -> `(Result (Option int) cstr)`),
>    instead of the element-binding instantiation that collapses the result to the
>    int64 carrier.
>
> 2. **The return-only-poly accessor `ok-val` stayed on the int64 carrier.** The
>    monomorphizer's carrier-recovery block (`emit_module.c`) only un-collapsed a
>    recovered `TY_STRUCT` result, and additionally gated on
>    `!type_uses_carrier_abi`. `(Option int)` is a `TY_APP` **by-value carrier-ABI
>    aggregate**, so it was rejected and the `(ok-val r)` spec kept returning
>    `int64_t` while its body produced `Option__int` (the cc "incompatible types
>    when returning `Option__int`"). Fix: accept any concrete by-value aggregate
>    (`TY_APP` or `TY_STRUCT`, concrete codegen layout, not a `:heap` struct, not
>    the bare `int64_t` carrier).
>
> 3. **The `vec-push!` carrier bridge spilled at the collapsed type.**
>    `expr_emits_byvalue_carrier_abi` (`src/compiler/emit_expr.c`) bailed on the
>    `type_uses_carrier_abi(e->type)` guard before consulting the matched spec --
>    `(ok-val r)`'s elab type had collapsed to the plain `TY_INT` scalar. And the
>    escaping heap-promote bridge was handed `emit_arg->type` (that same
>    `int64_t`), so it declared an `int64_t` temp and copied an `Option__int` into
>    it. Fix: check the matched spec's resolved result for `EX_CALL` **before** the
>    `e->type` guard, and spill the bridge at the real by-value type via
>    `fn_body_tail_byvalue_carrier_type`.
>
> Original report follows.

---

Found by: turmeric-spices Track C U2 Phase B (json Decode [(Vec (Option int))])
Verified on: turmeric 0.22.0, main @ ac202f5 (post #492/#493), built from source (build-release)
Severity: Medium-Low. Blocks nested decode `Decode (Vec (Option int))` and, more
  generally, decoding a JSON array whose element is a BY-VALUE (:copy) struct.
  Scalar elements and pointer/:heap-struct elements are unaffected, as are the
  scalar `Decode [(Option A)]` and nested `Encode (Vec (Option int))` paths.

## Summary

A generic constrained loop that (a) obtains a value of the constraint type `A`
by unwrapping a return-directed method result -- `(ok-val (:: (m ...) (Result A
cstr)))` -- and (b) `vec-push!`es it into a `(Vec A)`, miscompiles when `A` is a
by-value struct such as `(Option int)`. It type-checks, then fails at C codegen
with a cluster of carrier/struct mismatches:

    error: invalid initializer
    error: incompatible type for argument 1 of '__inst_Dec_dec_int'
    error: incompatible types when returning type 'Option__int'
           but 'int64_t' {aka 'long int'} was expected

The middle error is telling: at `A = (Option int)` the element method appears to
re-resolve to the `int` instance (`__inst_Dec_dec_int`), and the by-value
`Option__int` is shuttled through the int64 carrier (the other two errors).

Neither half fails on its own (see controls); only the combination
-- generic loop + Result-unwrapped by-value-struct element + `vec-push!` -- does.

## Repro (no inline C)

    (defclass Dec [a] (dec [seed : int] : (Result a cstr)))

    (definstance Dec [int]
      (dec [seed] (:: (ok seed) (Result int cstr))))

    (definstance Dec [Option] [(Dec A)]
      (dec [seed]
        (let [r (:: (dec seed) (Result A cstr))]
          (if (.is-ok r)
            (:: (ok (some (ok-val r))) (Result (Option A) cstr))
            (:: (err (err-val r)) (Result (Option A) cstr))))))

    ;; generic loop: decode element i as A, push ok-val into (Vec A)
    (defn build [A] [(Dec A)] [n : int  i : int  acc : (Vec A)] : (Result (Vec A) cstr)
      (if (>= i n)
        (:: (ok acc) (Result (Vec A) cstr))
        (let [r (:: (dec i) (Result A cstr))]
          (if (.is-ok r)
            (do (vec-push! acc (ok-val r)) (build n (+ i 1) acc))
            (:: (err (err-val r)) (Result (Vec A) cstr))))))

    (defn main [] : int
      (let [r (:: (build 3 0 (:: (vec-new) (Vec (Option int))))
                  (Result (Vec (Option int)) cstr))]
        (if (.is-ok r) (println (vec-len (ok-val r))) (println "err"))   ;; want 3
        0))

`tur check` clean; `tur run` fails at the C compile with the three errors above.

## Controls (both work)

A) Same loop, SCALAR element -- replace every `(Vec (Option int))` with
   `(Vec int)` (and the Result types accordingly): prints `3`.

B) Direct push of a by-value Option, no decode loop:

    (defn main [] : int
      (let [v (:: (vec-new) (Vec (Option int)))]
        (vec-push! v (:: (some 5) (Option int)))
        (println (vec-len v)) 0))     ;; prints 1

So `vec-push!` of a by-value `Option` is fine, and the generic loop is fine for
scalars; the defect is the intersection.

## Expected

`build` specialized at `A = (Option int)` should decode each element via
`Dec [(Option ...)]` (not `Dec [int]`) and push the by-value `Option__int` into
the `(Vec (Option int))` by value, returning a 3-element vector.

## Impact

json U2: `Decode [Vec]` (scalar/pointer elements) and scalar `Decode [(Option
A)]` work and are landed; nested `Decode (Vec (Option int))` -- and decoding a
JSON array of any by-value-struct element -- is blocked on this. Arrays of
`:heap` structs (pointer-carried) are expected to be unaffected. Workaround:
keep array element types scalar or `:heap` until this lands.
