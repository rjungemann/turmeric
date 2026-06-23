# vec-push! of a :heap parametric-struct element is not cast to the int64 carrier (cc error)

Repo: rjungemann/turmeric
Found by: building the `(Vec (Option int))` repro for the nested-container
  dispatch fix (docs/archive/constrained-generic-dispatch-container-typed-element-ambiguous.md).
Severity: Medium. Blocked building a `(Vec T)` whose element `T` is a parametric
  struct (e.g. `(Vec (Option int))`) or a nested heap container
  (`(Vec (Vec int))`). Independent of typeclasses.

Status: **RESOLVED.** Both the value-argument bridge (the hard cc error) and the
symmetric carrier let-binding init (the milder -Wint-conversion warning) are
bridged. Regression fixture:
`tests/fixtures/vec-push-heap-struct-element-carrier-cast/`.

## Summary

Pushing a parametric-struct value into a `Vec` did not bridge the value
argument to the `int64` element carrier, so the generated C failed to compile:
the push helper takes the `int64_t` carrier but received a typed struct value.

## Repro (no typeclasses)

    (defn main [] : int
      (let [vo (:: (vec-new) (Vec (Option int)))]
        (vec-push! vo (:: (some 5) (Option int)))
        0))

Before the fix `tur build` emitted:

    error: incompatible type for argument 2 of 'vec_hypush_ex'
      vec_hypush_ex(..., some__spec__Option__int_int64_t(INT64_C(5)));
                         ^ Option__int
    note: expected 'int64_t' but argument is of type 'Option__int'

(`vec_hypush_ex(int64_t v, int64_t val)` -- the element slot is the int64
carrier.)

## Root cause

`vec-push! [A] [v : (Vec A) val : A]` has an inline-C body, so it is never ABI
specialized: `val` stays typed `int64_t` (the carrier) for every `A`. The
direct-call argument path bridged the receiver `(Vec A)` to the carrier but had
no bridge for the *value* argument when that value emits a CONCRETE carrier-ABI
type rather than the int64 carrier. Two element shapes hit this:

  * a by-value parametric struct -- `(:: (some 5) (Option int))` lowers to a
    by-value `Option__int` aggregate (the by-value migration; `Option__int` is a
    `struct`, not a pointer). Passing an aggregate to `int64_t` is a hard
    incompatible-type cc error.
  * a nested heap container -- `(:: inner (Vec int))` lowers to a `Vec__int *`
    pointer. Passing a pointer to `int64_t` is a -Wint-conversion warning.

The symmetric read side warned the same way: inside a specialized generic
instance body, a carrier let-binding initialised from a heap-pointer spec
receiver -- `(let [c (:: x (Cons A))] ...)`, where `x` lowers to
`Cons__Option__int *` -- emitted `int64_t c = x;` uncast.

## Fix

`src/compiler/emit_expr.c`:

  * direct-call argument path: when the callee param is a polymorphic tyvar that
    lowers to the int64 carrier (inline-C body, or an unspecialized generic
    body) and the argument emits a concrete carrier-ABI value
    (`expr_emits_byvalue_carrier_abi`), bridge it `CK_CONCRETE -> CK_CARRIER` via
    `emit_carrier_bridge`. That helper already picks the right form per
    representation: a by-value aggregate spills to a fresh local and takes its
    address; a heap pointer reinterprets through `intptr_t`; an inline scalar
    uses the union bridge. This is the exact mirror of the existing ACB
    (carrier->concrete) bridge for specialized callees.

  * let/letrec binding path: when a carrier (`int64_t`) binding's initialiser
    resolves -- through the active ABI spec, peeking past ascriptions to the
    inner param -- to a pointer-represented type, emit the
    `(int64_t)(intptr_t)` reinterpret. A pointer->intptr_t->int64_t cast is
    always valid and is a no-op for a value already the int64 carrier, so it
    only tightens codegen; a by-value aggregate init c-names without a `*` and
    keeps its by-value declaration.

## Verification

`(Vec (Option int))` push/get and `(Vec (Vec int))` nested push/get build with
no warnings and run correctly (fixture above). Full `bash tests/run.sh`: 1753
passed, 0 failed, no snapshot churn.

## Note on lifetime

The by-value aggregate bridge spills to a fresh local and stores its address as
the carrier (the established `emit_carrier_bridge` aggregate mechanism, "the
spill local stays live through the expression that consumes the carrier
value"). A by-value aggregate pushed into a `Vec` that outlives the enclosing
expression therefore stores an address into a stack temporary -- the inherent
semantics of crossing a by-value aggregate through the heap-container carrier,
not introduced by this fix. Within-scope use (the common case, and the fixture)
is correct. A general lifetime fix (heap-promoting by-value aggregate elements)
is out of scope here and is tracked as an open finding:
`docs/reported/vec-push-byvalue-aggregate-element-stores-dangling-stack-address.md`.
