# Generic `^Show a` / `^ShowString a` dispatch mis-grounds opaque-over-ptr args to the int carrier

**Status:** RESOLVED (2026-07-20). Fixed in `src/compiler/emit_module.c`:
`body_has_dispatch_on_app_tyvar` (the `instance_changes` gate in
`emit_abi_register_call`) now fires for a class var bound to a bare nominal
`TY_ADT` -- including an opaque newtype like `String` -- not only a parametric
`TY_APP`. An opaque-over-int64 type collapses to the carrier (so `abi_changes`
stays false), but its instance differs from the baked `int` representative, so a
per-nominal-type ABI spec must be minted; without it the base clone dispatched
through `__inst_Show_show_int`. Regression fixture:
`tests/fixtures/generic-show-dispatch-opaque/`. The stage-1 `show-line` /
`print-show` wrappers now dispatch correctly for `String` / `StringSlice`
arguments. Kept for the paper trail.

**Severity:** medium (silent miscompile: prints a pointer as a decimal integer)

## Summary

A *generic* function constrained by a typeclass whose only tie to the type
variable is a value parameter (`(defn f [^Show a] [x : a] ...)`) mis-dispatches
when called with a `defopaque`-over-`ptr<void>` argument such as `String`: the
specializer grounds `a` to the `int` carrier instance instead of the concrete
instance, so the body runs `Show[int]` (or `ShowString[int]`) over the raw
pointer word and renders it as a decimal integer.

This is **pre-existing** -- it reproduces with the shipping `Show` typeclass and
is not introduced by the show-owned-result-plan stage-1 `ShowString` work. It is
the same carrier-fallback family documented for `__inst_Show_show_T` in
`src/turi/interpreter_natives.c` and for the collection-loop phantom-type
witnesses in `stdlib/typeclass-show.tur`.

## Minimal repro

```turmeric
(load "stdlib/string.tur")
(defn my-show [^Show a] [x : a] : cstr (show x))
(defn main [] : int
  (do
    (println (my-show (int->string 0)))   ; want "0", prints e.g. 93971086676640
    (println (my-show 42))                ; "42"  (scalar carriers dispatch fine)
    0))
```

`(my-show 42)` prints `42`; `(my-show (int->string 0))` prints the `String`
payload pointer as a decimal. Direct `(show (int->string 0))` /
`(show-string (int->string 0))` (no generic wrapper) are correct -- the bug is
specifically the generic-wrapper monomorphization, where a `String` argument
resolves to the int carrier instance.

## Impact on show-owned-result-plan stage 1

`stdlib/typeclass-show-string.tur` ships `show-line` / `print-show` convenience
wrappers (`(defn show-line [^ShowString a] [x : a] ...)`). They render correctly
for primitives and the typed collections, but inherit this limitation for
`String` / `StringSlice` arguments (the value is shown as its carrier pointer).
Direct `(show-string x)` on those types is correct; only the polymorphic
wrappers are affected. Documented here rather than worked around, since the fix
belongs in the generic-constraint specializer, not in the `ShowString` surface.

## Fix directions

The specializer needs to ground a `^Class a` type variable from the *declared
static type* of the argument (which is `String`, distinct from `int`) rather
than falling back to the int64 carrier representative when the concrete type
lowers to `ptr<void>`. Compare how the collection Show loops sidestep it by
carrying an explicit `(Vec A)` / `(Set A)` structural witness so
monomorphization has a non-carrier handle to dispatch against -- a wrapper with
only `x : a` has no such witness, so the carrier wins.
