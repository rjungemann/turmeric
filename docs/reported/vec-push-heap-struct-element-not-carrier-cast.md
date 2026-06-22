# vec-push! of a :heap parametric-struct element is not cast to the int64 carrier (cc error)

Repo: rjungemann/turmeric
Found by: building the `(Vec (Option int))` repro for the nested-container
  dispatch fix (docs/archive/constrained-generic-dispatch-container-typed-element-ambiguous.md).
Severity: Medium. Blocks building a `(Vec T)` whose element `T` is a `:heap`
  parametric struct (e.g. `(Vec (Option int))`) when the pushed value is a
  function-call result. Independent of typeclasses.

## Summary

Pushing a `:heap` parametric-struct value into a `Vec` does not insert the
pointer->carrier cast on the value argument, so the generated C fails to
compile: the push helper takes the `int64_t` carrier but receives a typed
struct-pointer value.

## Repro (no typeclasses)

    (defn main [] : int
      (let [vo (:: (vec-new) (Vec (Option int)))]
        (vec-push! vo (:: (some 5) (Option int)))
        0))

`tur build` emits:

    error: incompatible type for argument 2 of 'vec_hypush_ex'
      vec_hypush_ex(..., some__spec__Option__int_int64_t(INT64_C(5)));
                         ^ Option__int
    note: expected 'int64_t' but argument is of type 'Option__int'

(`vec_hypush_ex(int64_t v, int64_t val)` -- the element slot is the int64
carrier.)

## Root cause (direction)

`Option` is `(defstruct Option :heap [A] ...)`, so `Option__int` is a heap
pointer typedef. The `vec-push!` call site emits the value argument
(`some__spec__Option__int_int64_t(5)`, type `Option__int`) without the
`(int64_t)(intptr_t)` pointer->carrier bridge that the carrier-typed element
slot requires. The receiver argument IS bridged
(`(int64_t)(intptr_t)(vo_...)`); the value argument is not.

A by-value single-field struct (e.g. `(defstruct Box [A] (v A))`) does NOT hit
this: it collapses to its field representation, so the push value is already an
`int64_t`. The bug is specific to a `:heap` (pointer-carried) element value
flowing into the carrier-typed push slot. A related but milder symptom is the
`-Wint-conversion` warning on the symmetric read side
(`int64_t c = x;` for an `x : (Cons A)` carrier bind inside a generic instance
body).

## Fix directions

Insert the carrier bridge on the value argument when a `vec-push!` (and any
generic container insert whose element slot is the int64 carrier) receives a
pointer-carried value (`:heap` struct, `:ptr<T>`, opaque). The bridge already
exists for receivers and for scalar/float element ascriptions
(`preserve_ascribe_for_bridge` / the EX_ASCRIBE emit in `emit_expr.c`); extend
it to a `:heap`-struct-typed element value reaching a carrier element slot.
