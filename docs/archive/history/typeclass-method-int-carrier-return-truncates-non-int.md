# Typeclass-method int64-carrier return truncates a non-int instance result

> **Status:** Fixed for explicit instance annotations (2026-06-04); generic
> return-type polymorphism remains out of scope (see Scope below).

**One-line summary:** A typeclass whose method is declared with an int64
return (e.g. the `Functor` stub's `fmap : int`) numerically truncated an
instance result of a different register class -- a `:float`-returning
instance printed `6` for `6.5`, and a `:cstr`/`:ptr` result printed as an
integer -- because the instance return-type parser did not recognise
`:float` (and silently fell back to the class's int64 carrier).

**Severity:** Silent data loss (lossy numeric conversion at the return
boundary). No crash; the value is quietly truncated/reinterpreted.

## Context

Surfaced while auditing the type-passing gaps after the
[poly-to-fat typed-shim work](../upcoming/poly-to-fat-typed-shim-plan.md).
That work made a `:float` closure *argument* round-trip into a `^fat` sink;
this is the orthogonal *result* boundary: the typeclass method's own
declared return type.

The `Functor` class stub declares the carrier return:

```turmeric
(defclass Functor [^f] (fmap [container [fn :fn]] : int))
```

`: int` is a deliberate int64 carrier. An instance that returns a value of
a different register class must opt into a concrete return type via an
explicit annotation -- but the instance return-type parser in
`elab_typeclasses.c` recognised only `int`/`bool`/`cstr`/`void`/`ptr`. A
`: float` annotation matched none of them, was silently consumed, and the
return defaulted to the class's `: int`. The instance body's `double` was
then converted to `int64_t` at the emitted `return` (e.g. `6.5` -> `6`),
and the monomorphic dispatch + `println` saw `:int`.

## Minimal repro (pre-fix)

```turmeric
(load "stdlib/json.tur")
(load "stdlib/schema.tur")
(defstruct BoxW [A] (raw :int))
(definstance Functor [BoxW]
  (fmap [container fn] : float (+ 3.0 3.5)))   ; : float was silently ignored
(defn dummy [x : int] : int x)
(defn main [] : int
  (let [b (:: (make-struct BoxW (schema/int)) (BoxW int))]
    (println (.fmap b dummy)))                  ; printed 6 (pre-fix); 6.5 (fixed)
  0)
```

## Root cause

`elab_typeclasses.c`, instance method elaboration: the return-type
annotation parser (the `if (kw) { ... }` block after the method params)
matched a fixed keyword set and dropped anything else to the class default.
For `: float` the emitted impl was `static int64_t __inst_Functor_fmap_BoxW(
...)` -- the `double` body result truncated at the `return`. The dispatch
emits a direct call to that impl (monomorphic case), and the per-instance
dict field is typed from the same impl signature, so the whole chain
inherited the int64 carrier.

## Resolution

Extended the instance return-type parser to recognise `:float`, `:float32`,
`:float64`, and to fall back to `type_expr_from_form` for any other
compound type form (e.g. `: (Vec int)`, a struct/ADT name) instead of
silently dropping it to the class carrier. When the resolver cannot resolve
a form, the class default is still used (back-compat).

With the real return type in hand:

- the emitted impl becomes `static double __inst_Functor_fmap_BoxW(...)`,
- the per-instance dict field is `double (*fmap)(int64_t, tur_poly_fn_t)`
  (each instance gets its own dict struct type, so no vtable pointer
  mismatch), and
- the monomorphic dispatch is a direct call, so the `double` result
  propagates to `println` (`%g`), printing `6.5`.

Regression coverage: `tests/fixtures/typeclass-instance-float-return/`
(prints `6.5`). `bash tests/run.sh`: 1351 passed, 0 failed; no churn (the
change only adds recognised forms; existing int/bool/cstr/ptr instances and
un-annotated instances are unaffected).

## Scope / remaining work

- **Explicit annotation required.** An instance returning a non-int64 type
  must annotate the method (`: float`). Without it the class's int64 carrier
  is used and a float result still truncates. Inferring the return type
  from the instance body (and diagnosing a body/declared-return register
  mismatch) is a separate, larger change.
- **Generic (dict-passing) consumers.** A function generic over `Functor f`
  that calls `fmap` through a *uniform* dict sees the class's `: int`
  carrier; distinct instances with distinct return register classes cannot
  share one dict signature. True return-type polymorphism across instances
  (monomorphising the consumer per instance, or carrying the return type in
  the class) is out of scope here. The fix covers the common monomorphic
  dispatch where the instance is statically known at the call site.
