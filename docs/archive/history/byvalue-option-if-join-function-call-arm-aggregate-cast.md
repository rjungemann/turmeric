# Paper trail: by-value Option/Result if-join with a function-call arm

Resolved 2026-07-04. Companion to the resolved report at
`docs/archive/byvalue-option-if-join-function-call-arm-aggregate-cast.md`.

## Diagnosis

`emit_if_value` (`src/compiler/emit_expr.c`) declares the merge temp by value
when either arm is a recognised by-value carrier-ABI producer
(`fn_body_tail_byvalue_carrier_type`), then per arm either assigns directly (if
the arm already emits by value, `fn_body_tail_emits_byvalue_carrier_abi`) or
bridges carrier->concrete (`emit_carrier_bridge`, a `(T *)(intptr_t)(...)`
reconstruct).

For `(if b (some 99) (known))` with `(defn known [] : (Option int) (some 5))`:

- `(some 99)` lowers to a by-value `some__spec__...` and is recognised
  (`call_spec_result_byval_aggregate`), so `if_bv = tur_adt_Option__int` and the
  temp is declared by value.
- `(known)` -- an ordinary direct call whose C signature is
  `static tur_adt_Option__int known()` (already by value) -- was recognised by
  NONE of the leaf predicates, so the else arm got the carrier bridge:
  `tur_option_t *__t57 = (tur_option_t *)(intptr_t)(known());` -- casting the
  struct rvalue to a pointer. cc: "aggregate value used where an integer was
  expected".

## Fix

New leaf predicate `call_ordinary_defn_byval_aggregate(ctx, call, out_ty)`:

- `call` is a non-poly `EX_CALL` whose `fn_binding` is a GLOBAL, non-generic
  (`!is_poly_fn && !poly_type`), non-inline-C, non-`#{Construct}` defn with a
  declared `result_full_type`.
- Mirrors `emit_fns.c`'s C-return-type decision: for a non-inline-C body the
  signature is `emit_type_c_name(ctx, result_full_type)` (the `int64_t` fallback
  there is inline-C only). So the call yields the aggregate by value whenever
  that c-name is a real struct name (`!= "int64_t"`, not a `T *` pointer) for a
  non-heap ADT/APP/STRUCT.
- Deliberately does NOT gate on `!type_uses_carrier_abi(r)`: a concrete
  `(Option int)` is carrier-ABI at the type level yet is still emitted by value
  from an ordinary defn body. (This is why reusing
  `call_emits_byval_concrete_aggregate` verbatim -- which has that gate and is
  `__inst_`-scoped -- did not work.)

Wired into:

- `fn_body_tail_emits_byvalue_carrier_abi` default (leaf) case -> the arm is not
  bridged.
- `fn_body_tail_byvalue_carrier_type` `EX_CALL` case -> the type is recovered so
  a two-call `if` join (`(if b (ka) (kb))`, both ordinary defns) also declares
  the merge temp by value.

## Gating iterations (what the exclusions are for)

1. `!type_has_concrete_codegen_layout` in the first cut made the predicate never
   fire: `type_has_concrete_codegen_layout((Option int))` is false for the
   abstract TY_APP even though `emit_type_c_name` already yields
   `tur_adt_Option__int`. Replaced with the direct c-name test (mirrors what
   emit_fns actually emits).
2. `#{Construct}` templates (`some`/`none`/`ok`/`err`) had to be excluded: they
   lower context-dependently and inside a carrier-returning generic spec
   (`option_map__spec__...`) emit their bare int64-carrier base `some(..)`, not
   the by-value monomorph. A first cut without this exclusion regressed
   `option-map-capturing-closure` (`__t = some(..)` assigned an int64 to an
   aggregate temp). Those construct calls stay owned by
   `call_construct_emits_byval_aggregate` / `call_spec_result_byval_aggregate`,
   which distinguish the by-value-spec case from the carrier base.

## Verification

`tests/fixtures/byvalue-option-if-join-call-arm/` (stdout): Option through an
`if` join with a call arm in either position, both-call arms, and a let-tail
arm. Result verified out-of-tree via an inline-C `{is_ok,ok_val,err_val}`
extractor (the in-tree `ok?`-on-param path hits a separate pre-existing bug --
see below).

`bash tests/run.sh` -> `1933 passed, 0 failed`.

## Adjacent pre-existing bug found (filed, not fixed here)

`docs/reported/byvalue-result-param-ok-predicate-materialize-bad-cast.md`:
`(defn f [r : (Result int cstr)] : int (if (ok? r) 1 0))` miscompiles the
by-value Result parameter materialization (`tur_adt_Result__int__cstr __t = r;`
where `r` is the int64 carrier). The Option analogue (`some?` on an `(Option
int)` param) is fine. Reproduces with no `if` join of calls and on the pre-fix
compiler, so it is independent of this fix.

There is also a type-inference gap (not filed separately, noted here): an `if`
join whose ground arm is `(ok N)` does not pin the `err` phantom `B` from the
sibling arm (`(if b (ok 3) (known-res))` -> `B` stays a free tyvar, TUR-E0001),
so the `ok` arm needs an ascription. This is the Result analogue of the
return-directed-methods sibling-inference case and is left to that track.
