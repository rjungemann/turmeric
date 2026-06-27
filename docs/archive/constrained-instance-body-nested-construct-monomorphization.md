# Constrained-instance-body nested-construct monomorphization (under lowering)

**RESOLVED (2026-06-27).**  Both fixtures pass under the force-lower probe:
`nested-construct-byvalue-decode` (`42 / hi / 3.25 / 99`) and
`constrained-loop-vec-push-byvalue-result-element` (`3 / 0 / 1 / 2`).  Default
suite 1863/0.

The constrained-loop half landed in three steps (defects #1, then #2+#3 together);
see the "Remaining: constrained-loop" section below for the resolved breakdown.

**Severity:** medium (seam-4 / defstruct-as-defadt graduation blocker; not a
default-path bug). 2 fixtures, both resolved.

## What was fixed (the "five emit-time gaps")

The body of a constrained parametric instance that builds a NESTED construct over
a return-dispatched inner method -- `(definstance Dec [Option] [(Dec A)] (dec
[tag] (ok (some (ok-val (:: (dec tag) (Result A cstr)))))))` -- now threads the
constraint element type `A` through every seam under the defstruct->defadt
lowering.  Five coordinated emit-side fixes:

1. **Constraint-var binding at return dispatch** (`elab_typeclasses.c`,
   `elab_try_return_dispatch`): besides binding the class var (`a -> (Option
   cstr)`), bind the instance's standalone constraint vars (`A` of `[(Dec A)]`,
   `param_idx == -1`) positionally against the pinned dispatch value's app args
   (`A -> cstr`).  Without it the spec collapses every element to the int64
   carrier representative.

2. **Return-dispatch per-element minting trigger** (`emit_module.c`,
   `body_has_dispatch_on_app_tyvar` + `type_mentions_bound_tyvar`): a
   RETURN-dispatched inner `(:: (dec tag) (Result A cstr))` re-dispatches per `A`
   even though the receiver (`tag`) is concrete and the carrier ABI is unchanged,
   so a per-`A` spec must be minted.  Detected when the call's result type embeds
   a bound constraint var resolved to a CONCRETE element.  Gated by
   `g_bhd_detect_return_dispatch` to fire at top level and inside instance-method
   spec bodies but NOT inside a plain constrained-`defn` spec (where the result is
   consumed as the int64 carrier -- see the `build` caveat below).

3. **Outer-construct payload arg recovery** (`emit_module.c`,
   `emit_abi_register_call` Gap #4 block): a CARRIER-result construct (`(ok ...)`
   whose `(Result (Option A) cstr)` rides the carrier) has its payload arg binding
   collapsed to `Option__int` at elab; recover it top-down from the active
   instance-method spec's METHOD result family (`(Result a cstr)` instantiated
   through the spec bindings -> `(Result Option__cstr cstr)`, unified against the
   construct's own generic result).  Gated to typeclass-instance-method specs
   (`owner_instance != NULL`) so a plain constrained `defn` (`build`'s `(ok acc)`)
   is untouched.  Arg-types only -- does not flip the carrier result to by-value.

4. **Carrier-box readback deref-unbox for wide by-value aggregate fields**
   (`emit_core.c`, `emit_carrier_bridge`, BOTH the struct-app and lowered-ADT
   canonical readback paths): a wide by-value aggregate field (`(Result (Option
   int) cstr)`'s ok_val, `(Result Box cstr)`'s ok_val) is stored BOXED in the
   canonical carrier box, so a direct `(Option__int)(box->ok_val)` cast is an
   illegal int64->aggregate conversion -- deref-unbox instead.  Excludes
   opaque/transparent newtypes carried INLINE as the int64 carrier (c-name
   `int64_t`, e.g. `defopaque :ptr<void>`), which are the value, not a box pointer.
   Also frees the substituted compound field type (a previously-latent leak the
   leaf-only assumption masked).

5. **Float element reinterpret at the carrier-return and consumer seams**
   (`emit_fns.c` return path + `emit_expr.c` arg path): a generic accessor
   (`ok-val`) declared `: A` returns the int64 carrier, but inside a `(Result
   float cstr)` spec its body tail is a concrete `double` -- a plain
   `return <double>;` through an `int64_t` result NUMERICALLY converts (3.25 -> 3).
   Bit-reinterpret via the carrier bridge.  Gated on a TYVAR-declared result so a
   genuine `: float` poly-fn carried through the int64 slot (numeric convention)
   is untouched.  Companion: `emit_expr.c` skips the carrier->concrete deref when
   the arg's own matched spec already returns the concrete by-value aggregate
   (`ok_val__spec__tur_adt_Box_...` returns `tur_adt_Box` by value, not a box
   pointer -- double-unbox guard).

The new `emit_var_spec_arg_type` call site (the boxing-path spec arg lookup) is
registered in `docs/artifacts/crossing-routing-audit.txt` (10 sites).

## Remaining: constrained-loop-vec-push-byvalue-result-element

Its own header documents three combined defects that surface (build error) once a
by-value Result/Option element forces a monomorph in the `build` loop:

1. **build's `(dec i)` result is declared as the int64 carrier** while the
   nested-instance dispatch redirect mints a by-value `(Result (Option int) cstr)`
   spec for it -- `int64_t r = <by-value Result>` is a cc type error.  The redirect
   spec is correct; build's `r` binding must be declared at the by-value Result
   type when the constraint element is by-value.

2. **return-only-poly accessor** (`ok-val` of the by-value Result) and

3. **vec-push carrier bridge** spilling at the accessor's collapsed int64 type.

These are `build`-specific (a plain constrained `defn`, not an instance method),
so the gaps-1..5 fixes deliberately do NOT fire there (gap #2's
`g_bhd_detect_return_dispatch` is off inside a plain-defn spec, gap #3 requires
`owner_instance`).

### Defect #1 RESOLVED (2026-06-27): the dec mis-dispatch

`build`'s `(dec i)` at `A = (Option int)` re-resolved to `(Option int)` (an
applied type) but `emit_concrete_inst_method_fndef` could not match the
`Dec [Option]` instance -- whose head is the bare `Option` type constructor (a
`TY_ADT` under lowering) -- against the application, so the redirect found no
FnDef, fell back to the int-carrier `dec_int` representative, and minted an
ill-typed `__inst_Dec_dec_int__spec__tur_adt_Option__int_int64_t` (body returns
`Result__int__cstr`, declared `Option__int`).  Root cause: `emit_inst_head_matches`
(emit_core.c) had a "bare type-constructor head matches an application" branch for
`TY_STRUCT` patterns only; added the symmetric `TY_ADT` branch.  `build`'s
`(dec i)` now correctly emits `__inst_Dec_dec_Option(i)` on the carrier
(`int64_t r`).  Default suite 1863/0.

### Defects #2/#3 RESOLVED (2026-06-27): build's `(ok acc)` payload mis-binding

`build`'s tail `(:: (ok acc) (Result (Vec A) cstr))` (acc : `(Vec A)`, a :heap
Vec that rides the int64 carrier as a pointer) interned an `ok` spec with
`arg0 = tur_adt_Option__int` -- the Vec's ELEMENT -- instead of the Vec, then
double-boxed the :heap pointer into a `Vec **`, so at runtime the vec read back
empty (`vec-len = 0`, "tvec index out of bounds").

Root cause: `build`'s `(ok acc)` records the abi-binding `{A -> (Vec A)}` at elab
-- the NAME `A` is the `ok` construct template's own element tyvar, which happens
to collide with `build`'s constraint var `A`, and the VALUE is the container
`(Vec A)`.  Because `(Vec A)`'s element tyvar is unresolved, `type_c_name((Vec
A))` collapses to `int64_t`, so emit's carrier-collapse **rehydration path**
(emit_module.c, "(2) re-hydrate carrier-collapsed bindings by name") mistook it
for a collapsed bare constraint var and REPLACED it by name with the active
spec's `A -> (Option int)` -- dropping the `(Vec ...)` wrapper and minting `ok`
over the element instead of the container.

Fix (emit_module.c): the rehydration path now skips a binding whose VALUE is a
`TY_APP` (a parametric container).  A container over the constraint var is the
COMPOSITION path's job -- it instantiates `(Vec A)` through `A -> (Option int)`
to `(Vec (Option int))` -- so the binding correctly stays the :heap Vec, `(ok
acc)` rides the plain carrier `ok` (pointer -> int64), and the `ok-val` accessor
and `vec-push!` bridges (defect #3) fall in line with no further change.  Only a
bare scalar/tyvar value is a genuine carrier collapse the rehydration should
touch.

(An earlier emit-side override of the interned `arg_types[0]` was a dead end: it
made `build` compile but left the spec BODY's `emit_resolve_type(A) ->
Option__int` inconsistent with the corrected signature, double-boxing the pointer
-- the rehydration-skip fix instead keeps the binding itself correct so signature
and body agree.)

## Notes

- Default suite is unaffected (only the lowered representation triggers the bug).
- Independent of the (resolved) heap-cons field-read cluster.
