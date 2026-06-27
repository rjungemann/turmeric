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

## Root cause (direction)

A function-typed construct element rides as the opaque fat-closure handle
(`void *` / int64), but the `some`/`ok` `#{Construct}` spec arg-type computation
resolves the element `(fn [float] float)` to its result kind (`double`) rather
than the opaque handle. The element should be treated as the fat-closure carrier
(int64) when it is a `TY_FN`, regardless of the fn's result type. Likely in the
abi spec `arg_types` derivation for a construct whose payload element is a
function (the float result leaks through a poly-to-fat-float / result-kind
path); the int case is masked because int64 == the handle ABI.

## Status

Surfaced finishing the boxed-aggregate carrier-base work
(`docs/archive/lowered-option-result-construct-carrier-base.md`), which fixed the
non-fn members of that cluster (`positional-opaque-ok`/`-pap`,
`kleisli-arrow-instance`). This fn-element arg-typing case is independent of the
carrier representation and is left for a focused HKT-fn-element pass; see also
`docs/archive/ap-fn-in-container-monomorphization-plan.md` for the by-value
fn-in-container machinery this rides on.
