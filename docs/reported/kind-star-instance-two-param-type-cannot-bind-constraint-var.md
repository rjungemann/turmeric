# A kind-* typeclass instance over a 2-parameter type cannot fix one type arg and bind a constraint var to the other

Repo: rjungemann/turmeric
Found by: turmeric-spices Track C U2 (json Encode/Decode [(Map cstr V)])
Verified on: turmeric 0.22.0, main @ 9f165db, built from source (build-release)
Severity: Medium. Blocks json `Encode`/`Decode [(Map str V)]` (JSON objects) via
  the typeclass surface. Single-parameter containers (`Encode [Vec]`,
  `Encode [(Option A)]`) work; the wall is specifically 2-parameter types.

## Status

**Instance-head expressiveness: FIXED.** A no-hole, fully-applied 2-parameter
head -- `(definstance Tag [(Map cstr V)] [(Tag V)] ...)` -- now parses as a
kind-* nested type application and binds the free arm `V` to the value slot,
keeping the class at kind *. See "Fix" below.

**Residual (still open): runtime per-element dispatch for heap-carried 2-param
containers.** `Map` is a heap-allocated opaque handle carried as `:int`; both
`(Map cstr cstr)` and `(Map cstr int)` have the SAME C representation, so the
instance method is not element-monomorphized and an in-body `(encode (:: (map-get
m k) V))` still bakes the int-carrier representative instance. This is the same
gap the spice's `Encode [Cons]` flags ("Needs #475/#479 for the per-element
dispatch") -- a `(Cons A)` is likewise a heap pointer carried as `:int`. By-value
containers (`Option`, `Vec`) escape it because their elements are reified into
distinct C types (`Option__cstr`/`Option__int`, `Vec__cstr`/`Vec__int`). Closing
the residual means minting per-element specs keyed on the static type even when
the carrier signature is identical (the #475 track). It is orthogonal to the head
form and is NOT addressed here.

## Summary

For a kind-* typeclass (e.g. `Encode [a]` / `Decode [a]`, method `encode : a ->
cstr`), there was no instance-head form for a 2-parameter type like `(Map K V)`
that BOTH (a) keeps the class at kind * and (b) connects a constraint type
variable to one of the type's parameters:

  * Bare head `[Map]` (like the working `[Vec]`) keeps the class kind * and
    type-checks, but the constraint var `V` in `[(Encode V)]` is not linked to
    any `Map` parameter, so it DEFAULTS to int. The `(Encode V)` dictionary is
    then resolved as `Encode[int]` at instance selection, regardless of any
    `(:: x (Map cstr V))` ascription in the body. Values dispatch via the int
    instance -> wrong output for non-int value types.

  * A partially-applied head that WOULD bind `V` -- `(Map _ V)` or
    `(Map cstr V)` -- flipped the whole class to kind `* -> *`, so every kind-*
    instance (the int/bool/cstr/float primitives) failed with TUR-E0012; or the
    no-hole `(Map cstr V)` was rejected outright ("instance head must mark the
    free parameter with exactly one '_'").

Single-parameter containers escape this: `(Vec A)` / `(Option A)` have exactly
one parameter, so `[(Encode A)]` binds it without any partial application and
without a kind conflict. The 2-parameter case had no working middle ground.

## Repro A -- bare [Map] keeps kind * but V defaults to int (no inline C)

    (defclass Tag [a] (tg [x] : int))
    (definstance Tag [int]  (tg [x] 10))
    (definstance Tag [cstr] (tg [x] 20))

    ;; bare head: type-checks (class stays kind *), but V is unbound
    (definstance Tag [Map] [(Tag V)]
      (tg [x]
        (let [m (:: x (Map cstr V))]
          (tg (:: (map-get m "k") V)))))

    (defn main [] : int
      (let [m (:: (map-assoc (:: (map-new) (Map cstr cstr)) "k" "v")
                  (Map cstr cstr))]
        (println (tg m))    ;; want 20 (Tag[cstr]); ACTUAL 10 (Tag[int], V defaulted)
        0))

Runs; prints `10`.

## Repro B -- the V-binding head flipped the class kind (no inline C)

    (defclass Tag [a] (tg [x] : int))
    (definstance Tag [int]  (tg [x] 10))
    (definstance Tag [(Map _ V)] [(Tag V)]
      (tg [x] (let [m (:: x (Map cstr V))] (tg (:: (map-get m "k") V)))))
    (defn main [] : int 0)

`tur check` (before the fix):

    error [TUR-E0012]: kind mismatch: instance of 'Tag' provides a kind-'*'
      type for parameter 1 which expects kind '* -> *'

The `(Map cstr V)` head was rejected earlier with "instance head must mark the
free parameter with exactly one '_'".

## Fix

`src/compiler/elab_definstance` (src/compiler/elab_typeclasses.c) now accepts a
no-hole, fully-applied 2-parameter head `(Ctor a b)` -- the kind-* counterpart of
the single-`_` partial-application head (which serves kind-(* -> *) classes).
Each argument is parsed by a new helper `parse_instance_head_arg`: a primitive
keyword or known struct/ADT name resolves to its concrete type; any other bare
name becomes a head type variable (a named `TY_TYVAR`). The head is built as a
nested `TY_APP` -- app(app(Ctor, a), b) -- whose kind steps one rung down the
ladder per application, so a binary (ARROW2) constructor applied to two args
lands at kind * and a STAR-declared class stays STAR. The downstream machinery
already handles a nested-`TY_APP` head: `m7_collect_tyvar_bindings` unifies it
against the concrete receiver to bind `V` to the value slot, instance selection
discriminates on the constructor identity and concrete leftmost arm, and the
dispatch call attaches the resulting `V -> <value type>` binding.

Regression test: `tests/fixtures/definstance-applied-binary-head-kind/` --
`(definstance Tag [(Map cstr V)] [(Tag V)] ...)` type-checks, the primitive
`[int]`/`[cstr]` instances still dispatch (10/20, proving the class stayed kind
*), and the 2-param-head instance is selected for a `Map` receiver (42).

## Expected (now delivered for the head form)

    (definstance Encode [(Map cstr V)] [(Encode V)] (encode [x] ...))

with `V` bound to the map's value type and the class staying at kind *.

## Impact

json U2: the `Encode`/`Decode [(Map str V)]` instance head can now be written
with `V` bound to the value slot. The HAMT iteration / `__json-obj-build`
machinery and the yyjson object-iteration plan all work. The remaining blocker
for non-int value types is the heap-carrier per-element dispatch residual noted
under "Status" (#475-class), shared with `Encode [Cons]`.
