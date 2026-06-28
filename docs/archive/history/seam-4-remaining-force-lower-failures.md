# seam-4 remaining force-lower failures -- fix paper trail

Resolves `docs/archive/seam-4-remaining-force-lower-failures.md`. Every fix is in
`src/compiler/emit_expr.c`; no change to the default by-value path (default suite
stays `1862 passed, 0 failed`). Each was reproduced under an uncommitted
`TUR_FORCE_LOWER` probe in `defstruct_lowers_to_adt` (`elab_structs.c`) that
forces lowering past the all-primitive gate, run with
`--enable=defstruct-as-defadt`.

## 1. defopaque-struct-payload-through-unsafe-lift (was BUILDFAIL)

A spec wrapper `box_set___spec__..._tur_adt_Pos(int64_t b, tur_adt_Pos v)` passed
its by-value `tur_adt_Pos v` straight to the generic inline-C carrier helper
`_un_unset_hyraw(int64_t, int64_t)` -- `incompatible type for argument 2`.

**Fix:** in the regular-call arg loop, mirror the existing TY_APP-monomorph
boxing block for the bare non-parametric by-value ADT case: when the resolved arg
type is a by-value `TY_ADT` (`emit_type_is_byvalue_adt`) and the callee param is a
genuine polymorphic carrier slot (`TY_TYVAR`/`FORALL`/`EXISTS`) of an inline-C or
unspecialised generic, bridge `CK_CONCRETE -> CK_CARRIER`. Emits
`_un_unset_hyraw(b, (int64_t)(intptr_t)(&__tmp))`.

## 2. fn-field-unboxed (was RUNFAIL / SIGSEGV)

A lowered record-ADT stores a `fn` field as the int64 carrier, but
`type_c_name(TY_FN)` returns the fn's RESULT C name. A `(fn [int32] int32)` field
therefore read as `(int32_t)(cb).op`, truncating the 64-bit function pointer to 32
bits before the call -> jump to a wild address. (An `int`-result fn dodged it:
`cty == int64_t`, an identity cast -- only sub-word fn fields segfaulted.)

**Fix:** in the record-ADT `EX_GET_FIELD` emit, when the field's `e->type.kind ==
TY_FN`, force both the outer cast `cty` and the spec-recovery `fld_rcty` to
`int64_t` (the field's real storage width). The `EX_CALL` fn_expr path then
re-specialises the pointer to the concrete arg/result signature, so the call is
`((int32_t (*)(int32_t))(intptr_t)((int64_t)(cb).op))(3)`.

## 3. letrec-self-recursive-carrier-struct-return (was BUILDFAIL)

A letrec self-recursive closure whose param/return is a wide `:copy` struct
(`Box{lo hi}`, >8 bytes) lowers the param to the int64 box-pointer carrier
(`int64_t __tur_b4box_acc`, deref+copied at thunk entry), but the closure-call
arg loop passed the raw `ctor_Box(...)` aggregate -- `incompatible type for
argument 3`.

**Fix:** in the `closure_fn_binding` call path, when an arg
`emit_type_is_wide_byval_adt`, heap-box it with `emit_carrier_bridge_escaping`
(`CK_CONCRETE -> CK_CARRIER`) so it matches the `__tur_b4box_*` carrier ABI.
Covers both the closure's own recursive self-call and the enclosing letrec's
direct invocation.

## 4. result-over-struct-with-option-field-typedef-order (already resolved)

Passed under the probe at the start of this pass -- the landed seam-4 PR (#570)
had already reordered the by-value base-ADT typedef before its app-monomorph. No
change needed.

## 5. typeclass-bounded-wrapper-heterogeneous-dispatch (was BUILDFAIL)

A bounded wrapper's carrier base `any_hyget(int64_t s, int64_t idx)` bakes one
concrete instance method (`__inst_StorageOps_sop_hyget_Sparse_tyvarVel`) whose
inline-C body returns `tur_adt_Vel` BY VALUE, while the base's own C return is the
int64 carrier -- `incompatible types when returning type 'tur_adt_Vel' but
'int64_t'`. The matched-spec and carrier-ABI gates in
`expr_emits_byvalue_carrier_abi` both miss it (the base has no abi spec; a lowered
defstruct is not carrier-ABI).

**Fix:** add `call_emits_byval_concrete_aggregate`, scoped to a typeclass
`__inst_`-prefixed callee whose `result_full_type` resolves to a by-value concrete
aggregate (mirroring emit_fns.c's `typed_byval_adt` signature decision). Wire it
into the two return-bridge tail predicates (`fn_body_tail_emits_byvalue_carrier_
abi` / `fn_body_tail_byvalue_carrier_type`) so the existing concrete->carrier
return spill (`ret_is_int64_carrier` branch in emit_fns.c) fires for the base. The
`__inst_` scope is required: an earlier unscoped version fired on an ordinary
recursive closure self-call returning a by-value struct (the letrec #3 fixture
under the *default* path) and tripped a spurious carrier deref.
