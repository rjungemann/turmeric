# `some`/`ok` of a function element mistypes the spec arg as the fn's result

**Severity:** low (force-lower; HKT applicative/functor over an Option/Result
whose element is a `(fn ...)` with a non-int result). Default path unaffected.

## Summary

Constructing `(some <fn>)` where the wrapped value is a function whose result
is a float (or other non-int scalar) -- `(Option (fn [float] float))` -- mints a
`some` spec whose ARGUMENT type is the function's RESULT type (`double`) instead
of the int64 fat-closure handle the call actually passes:

```c
static tur_adt_Option__opaque some__spec__tur_adt_Option__opaque_double(double x) {
        return ctor_Option__opaque(true, x);   /* ctor wants int64 */
}
...
void *__t66 = __t65;                            /* the fat-closure handle */
... some__spec__tur_adt_Option__opaque_double(__t66);   /* void* -> double param */
```

`error: incompatible type for argument 1 of 'some__spec__tur_adt_Option__opaque_double'`.

The int-fn sibling works only by coincidence: `(fn [int] int)` mints
`some__spec__...opaque_int64_t(int64_t)`, and int64 happens to match both the
fat-closure handle ABI and the carrier.

## Affected fixtures (force-lower)

- `hkt-ap-fn-in-container` -- `(some (fn [x : float] : float ...))` / applicative `ap`.
- `hkt-stdlib-option-result-instances` -- the `(:: (fn ...) int)`-erased sibling
  (`some___spec__bool_...` arg mismatch).

## Root cause (CONFIRMED 2026-06-27 -- and why it is a dedicated pass)

A function-typed construct element rides as the opaque fat-closure handle
(`void *` / int64), but `(some <float-fn>)` mints `some__spec__...opaque_double`
(arg `double`) -- and the spec body `ctor_Option__opaque(true, x)` then passes
that `double` to the int64 ctor field too. `type_c_name` of the resolved arg is
`double` because the arg is the fn's RESULT type, not the handle. The int-fn
sibling is masked because int64 == the handle ABI.

The deeper problem is that at the `emit_abi_register_call` arg-loop the some
call's three pieces of type information all DISAGREE for the fn payload (traced
with a temporary `[SOME]` probe):

- the declared param `generic_arg` is the bare class tyvar `A` (TY_TYVAR);
- the recorded `abi_binding` is `A = double` (TY_FLOAT) -- the elaborator
  recorded the fn's RESULT type, not `(fn [float] float)` nor the int64 handle;
- `call->type` is the carrier-collapsed `(Option ...)` whose c-name is
  `int64_t` (TY_APP, element erased);
- the argument expression, walked through let/reinterpret/do, bottoms out at the
  closure-construction `EX_CALL` typed `TY_TYVAR` -- so there is no local
  "the payload is a function" signal either.

The AUTHORITATIVE `Option__opaque` result element is only recovered DOWNSTREAM
(the construct-recovered-byvalue threading from the consuming `ap`), after the
arg-loop has already fixed `arg_types[0] = double` from the binding. So a correct
fix must either (a) repair the elaboration binding to record the fn payload as
its handle/`(fn ...)` type rather than the fn's result, or (b) reconcile
`arg_types[0]` against the recovered result element just before
`emit_abi_intern_spec` (when `result_type` = `Option__opaque` is known) -- both
larger than a local arg-loop tweak.

A naive "normalize a TY_FN-typed generic element arg to int64" does NOT fire
here: `arg_types[0]` is already `TY_FLOAT` (the binding collapsed the fn to its
result) before the loop, never `TY_FN`.

## Status

Surfaced finishing the boxed-aggregate carrier-base work
(`docs/archive/lowered-option-result-construct-carrier-base.md`), which fixed the
non-fn members of that cluster (`positional-opaque-ok`/`-pap`,
`kleisli-arrow-instance`). This fn-element arg-typing case is independent of the
carrier representation and needs a focused pass on either the elaboration
binding for an fn construct-payload or the arg/result reconciliation at spec
intern; see also `docs/archive/ap-fn-in-container-monomorphization-plan.md` for
the by-value fn-in-container machinery this rides on.
