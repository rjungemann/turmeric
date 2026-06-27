# Constrained-instance-body nested-construct monomorphization (under lowering)

**Severity:** medium (seam-4 / defstruct-as-defadt graduation blocker; not a
default-path bug). 1 fixture remaining.

## Status (2026-06-27)

`nested-construct-byvalue-decode` is **RESOLVED** under the force-lower probe
(`42 / hi / 3.25 / 99`).  `constrained-loop-vec-push-byvalue-result-element`
still build-fails under force-lower on its own three documented defects (below);
it no longer regresses the default path.  Default suite: 1863/0.

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

### Defects #2/#3 STILL OPEN: build's `(ok acc)` payload mis-binding

`build`'s tail `(:: (ok acc) (Result (Vec A) cstr))` (acc : `(Vec A)`, a :heap
Vec that rides the int64 carrier as a pointer) interns an `ok` spec with
`arg0 = tur_adt_Option__int` -- the Vec's ELEMENT -- instead of the Vec, producing
`ok__spec__int64_t_tur_adt_Option__int__h1((int64_t)(intptr_t)(acc))`: the spec
expects a by-value `Option__int` but the call passes the Vec pointer as int64.
The construct payload arg type collapses because the `(ok acc)` call carries an
abi-binding whose NAME is the enclosing spec's constraint var `A` (the `ok`
template's element tyvar was elaborated under that same name), so the rehydration
path resolves the payload to `A`'s value (`Option__int = (Option int)`, the Vec's
element) rather than the actual argument `acc : (Vec (Option int))`.  Because the
Vec is :heap, the correct lowering keeps `(ok acc)` on the plain carrier `ok`
(pointer cast to int64); minting any by-value spec is wrong.

Fix direction (next pass): derive a construct payload arg type from the actual
argument expression's spec-resolved type when the binding-derived type comes from
a name-colliding constraint var -- a construct over a spec parameter whose type is
a :heap/parametric container must not collapse to the container's element.  (A
direct attempt to prefer `emit_spec_arg_type_for_binding` for construct payload
args in the owned arg loop did not take effect -- `build`'s `(ok acc)` reaches the
interning through a construct path that bypasses that loop; the override must be
placed where the construct's `arg_types[]` are finalized for interning, or the
elab-side abi-binding for a construct whose element is `(Container ConstraintVar)`
must record the element type, not the bare constraint var.)  Then the `ok-val`
accessor and `vec-push!` carrier bridges should fall in line.

## Notes

- Default suite is unaffected (only the lowered representation triggers the bug).
- Independent of the (resolved) heap-cons field-read cluster.
