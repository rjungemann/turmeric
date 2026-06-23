# constrained-generic element dispatch is ambiguous when the element type is itself a container (nested container instances)

Repo: rjungemann/turmeric
Found by: turmeric-spices Track C U2 (json container Encode/Decode instances)
Verified on: turmeric 0.22.0, main @ c153600 (post #490/#491), built from source (build-release)
Severity: Medium-Low. Flat container instances (Vec/Option of scalars and of
  structs) work after #490/#491; this blocked NESTED container instances
  (e.g. (Vec (Option int)), (Cons (Option int))) -- the remaining gap for json U2.

Status: **RESOLVED.** The constraint discharge now matches a struct-headed
applied instance, so a container-typed element dispatches via its `(C A)`
constraint. Regression fixture:
`tests/fixtures/constrained-generic-nested-container-element-dispatch/`.

## Summary

#490/#491 made a constrained generic instance dispatch the class method on its
concrete element type `A` (recovered via `(:: e A)`), and this works when `A`
resolves to a unique instance -- scalars (int/bool/cstr/float) and structs
(defstruct/derive-json) all dispatch and carry values correctly.

It still failed when `A` is ITSELF a container type. Inside
`(definstance C [Cons] [(C A)] ...)`, dispatching `(c receiver)` on a receiver
of type `(Cons (Option int))` raised `TUR_E0020_AMBIGUOUS_DISPATCH`, even
though a `C [Option]` instance was in scope to discharge the `(C A)` constraint
with `A = (Option int)`.

## Root cause

The dispatcher's exact-match path (`elab_method_call`, `elab_typeclasses.c`)
accepts the `C [Cons]` instance for a `(Cons (Option int))` receiver only if its
`(C A)` constraint is satisfied -- i.e. only if an instance exists for the
substituted element type `(Option int)`. That check runs through
`typeclass_env_lookup_instance` (`typeclass.c`).

That lookup compares the query type's top-level kind against each instance's
`type_args[0].kind`. It normalized the **query** type's `TY_APP` head down to
its `TY_STRUCT` constructor (so `(Option int)` becomes `TY_STRUCT Option`), but
compared that against the instance's **raw** kind. An instance declared with an
*applied* head -- `[(Option A)]` -- stores `type_args[0]` as `TY_APP(Option, A)`
(kind 21), whereas a *bare* head -- `[Cons]`/`[Vec]` -- stores it as
`TY_STRUCT` (kind 18). So the comparison was `TY_STRUCT` (normalized query) vs
`TY_APP` (raw instance) -- never equal. `C [Option]` was not found, the
`(C A)` constraint was deemed unsatisfied, the `C [Cons]` instance was demoted
to a name-only fallback, and with >1 name-matching instance the dispatcher
reported `TUR_E0020`.

The flat case (`(Cons int)`) worked because a plain `int` element is `TY_INT`,
never a `TY_APP`, so no head normalization was needed and the kinds lined up.
Direct dispatch on an `(Option int)` value also worked -- the main dispatch loop
has its own `TY_APP`-vs-`TY_APP` head comparison; only the constraint-discharge
helper `typeclass_env_lookup_instance` lacked the symmetric normalization.

## Fix

`src/compiler/typeclass.c`, `typeclass_env_lookup_instance`: normalize the
instance's head the same way as the query head. A struct-headed `TY_APP`
instance (`(Option A)`) is walked down to its `TY_STRUCT` constructor before the
kind compare and the struct-identity check, so a struct-normalized query
(`(Option int)`) matches it. Purely additive: ADT-headed `TY_APP` instances
(no struct head) still compare raw `TY_APP`-vs-`TY_APP` exactly as before, and
distinct struct constructors (Option vs Vec) are still discriminated by the
struct-identity check.

With the fix, dispatch resolves the full constraint chain: a
`(Cons (Option int))` receiver routes `C [Cons]` -> `C [Option]` -> `C [int]`.

## Repro (now passing)

    (defclass Tag [a] (tag [x] : int))
    (definstance Tag [int] (tag [x] 7))
    (definstance Tag [(Option A)] [(Tag A)]
      (tag [x] (if (.is-some x) (+ 100 (tag (.value x))) 0)))
    (defn tag-head [A] [(Tag A)] [c : (Cons A)] : int
      (tag (:: (.head c) A)))
    (definstance Tag [Cons] [(Tag A)]
      (tag [x] (+ 1000 (let [c (:: x (Cons A))] (tag-head c)))))
    (defn main [] : int
      (let [nested (:: (list (:: (some 5) (Option int))) (Cons (Option int)))]
        (println (tag nested)))   ;; 1107 = Cons(1000) + Option(100) + int(7)
      0)

Before: `TUR_E0020_AMBIGUOUS_DISPATCH` ('.tag' matches 2 instances). After:
prints `1107`, proving each layer's instance ran.

## Impact

json U2 container instances now dispatch nested containers
(`(Cons (Option int))`, etc.) via the `(C A)` constraint chain -- verified
against turmeric-spices `spices/json` (the encode path reaches codegen with no
ambiguity). The previously-documented workaround (wrap the inner container in a
named newtype with its own instance) is no longer required for dispatch.

## Related / still open

The report's *literal* repro used `(Vec (Option int))` plus `vec-push!` of a
parametric-struct value. Dispatch there is also fixed, and the SEPARATE,
pre-existing codegen defect it tripped (pushing a parametric-struct or nested
heap-container value into a `Vec` did not bridge the value to the int64 element
carrier) has since been resolved too -- see
`docs/archive/vec-push-heap-struct-element-not-carrier-cast.md`. With both
fixes the literal `(Vec (Option int))` repro builds and runs.
