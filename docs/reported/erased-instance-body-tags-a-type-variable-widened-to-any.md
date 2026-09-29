# An erased instance body widens a type-variable value to `any` with a meaningless tag

**Severity: medium-high.** Compiled only; `--interpret` answers correctly. It
usually ends in a clean panic, the first time a dynamic operator, `println` or
`cast` reads the value. But `type-of` answers `unknown` and `(is? x float)`
answers false, so a program that branches on the value's type silently takes
the wrong branch. It reaches both Saffron and typed Turmeric. Filed 2026-09-28.
It was found while splitting S9's residue out of
[saffron-lang-plan](../archive/saffron-lang-plan.md), as the "`: any` result"
aside in
[saffron-dyn-witness-fn-arity-defaults-unary](saffron-dyn-witness-fn-arity-defaults-unary.md).
That note guessed "the elements reach the lambda un-boxed". They do not: they
reach it boxed, but with a tag no consumer understands.

A constructor-class instance (`definstance C [One]` for a `(defclass C [^t]
...)`) is emitted once, ERASED: the head's element type `a` rides the int64
carrier. When the method's declared result is a concrete `any`, the call-site
binding `a := float` changes neither the C signature nor the instance the call
reaches. So no specialization is minted and the call runs that erased body.
There, widening an `a`-typed value to `any` has no concrete type to tag with,
and it tags with the type-variable KIND itself: `TUR_TAG(36, ...)`, where 36 is
`TY_TYVAR`. No `any` consumer knows that tag.

## Repro

Saffron, minimal:

```turmeric
#lang saffron
(defclass Box1 [^t]
  (unbox1 [b : (t a)] : any))
(defdata One [a] (One [v : a]))
(definstance Box1 [One]
  (unbox1 [o] (.v o)))
(defn main []
  (let [x (.unbox1 (One 1.5))]
    (println (type-of x))
    (println (if (is? x float) "float" "not a float"))
    (println (+ x 2.25)))
  0)
```

Measured 2026-09-28 with `./build/tur` (v0.56.2, x86-64 Linux):

- `tur --interpret`: `float`, `float`, `3.75` (expected).
- `tur run`: `unknown`, `not a float`, then `panic at ...: +: no operator for
  a value of that type argument`, exit 134. The first two lines are the silent
  wrong answers; the panic comes only when an operator reads the value.

Typed Turmeric, same shape:

```turmeric
(defclass Box1 [^t]
  (unbox1 [b : (t a)] : any))
(defdata One [a] (One [v : a]))
(definstance Box1 [One]
  (unbox1 [o] (.v o)))
(defn main [] : int
  (println (cast (unbox1 (One 1.5)) float))
  0)
```

`--interpret` prints `1.5`. `tur run` panics `cast: any holds unknown, not
float`.

The shape it was found in: a class whose `fn` parameter consumes the elements,
with an `: any` result. The instance body widens each element into the Saffron
lambda's `any` parameters, and the lambda's `+` panics:

```turmeric
#lang saffron
(defclass Comb [^t]
  (comb [ta : (t a) g : (fn [a a] b)] : any))
(defdata Two [a] (Two [l : a r : a]))
(definstance Comb [Two]
  (comb [t g] (g (.l t) (.r t))))
(defn go [x] (.comb x (fn [a b] (+ a b))))
(defn main []
  (println (go (Two 1.5 2.25)))
  0)
```

`--interpret` prints `3.75`. `tur run` panics `+: no operator for a value of
that type argument`. It is the same with a statically known receiver (`(.comb
(Two 1.5 2.25) ...)`), with int elements, and with an identity lambda (the
panic moves to `println`). So it is not the dynamic-dispatch witness, not
float-specific, and not about `+`.

Controls:

- Declare the result `: a` in `Box1` (or `: b` in `Comb`): prints `1.5` /
  `3.75` on both back ends. A result that mentions a type variable forces a
  specialization.
- A plain generic `defn` widening its `A` to `any` (`(defn wrap [A] [x : A] :
  any x)`) prints `1.5` on both. Plain generics are monomorphized.

## Root cause

The emitted erased body is the whole defect:

```c
static tur_tagged_t __inst_Box1_unbox1_One(int64_t o) {
        return TUR_TAG(36, (int64_t)(intptr_t)((int64_t)((tur_adt_One *)(intptr_t)(o))->v));
}
```

Two pieces:

1. **The widen emits a tag for an unresolved type variable.** The
   `EX_UNION_INJECT` arm of `emit_value` (`src/compiler/emit_expr.c`, the tag
   is read at ~7560 and the final scalar arm is at ~7703) asks
   `emit_any_type_id` (`src/compiler/emit_module.c:820`) for the payload's id.
   For a `TY_TYVAR` that is neither named nor a fn, it falls through to
   `any_box_tag_for_type`, which returns the TypeKind: 36. `type-of` maps 36
   to "unknown", `is?` matches nothing, and the dynamic operators report "a
   value of that type argument".
2. **Nothing mints the spec that would have a concrete type.**
   `emit_abi_register_call` (`src/compiler/emit_module.c:5045`) specializes an
   instance call only when the binding changes the ABI (`abi_changes`, from
   ~5785) or the instance a call inside the body dispatches to
   (`instance_changes`, 6184, via `body_has_dispatch_on_app_tyvar` at 4155). An
   `any` result and an erased element parameter spell the same C for every
   binding, and a widen is not a dispatch, so both stay false. The call goes to
   the erased body, where piece 1 fires.

The interpreter carries a runtime tag on every value, so it never needs the
binding and answers correctly.

## Fix directions

1. Mint the spec: make a widen to `any` whose payload type mentions a bound
   type variable count as `instance_changes` (a third trigger beside the
   dispatch and return-dispatch probes in `body_has_dispatch_on_app_tyvar`).
   In the spec the payload type is concrete, so the widen tags `float` (4) and
   stores the IEEE bit pattern, as it already does in a monomorphized `defn`.
   This is the fix that matches what `: a` / `: b` already get.
2. Never emit `TUR_TAG(TY_TYVAR, ...)`. With 1 in place, reaching the widen
   with an unresolved type variable is an emitter bug. Make it an internal
   error (or a TUR-E diagnostic naming the instance), so the next shape fails at
   compile time instead of producing an `unknown` that `is?` can silently
   branch on.
3. Add fixtures for both back ends: the Saffron repro above (including its
   `type-of` / `is?` lines, which are the silent half) and the typed one.

## Related

- [static-instance-spec-calls-any-lambda-as-concrete-result](static-instance-spec-calls-any-lambda-as-concrete-result.md)
  and
  [fn-param-call-prototype-spelled-from-the-call-not-the-fn](fn-param-call-prototype-spelled-from-the-call-not-the-fn.md):
  the same instance bodies, a different defect (the prototype of the call
  through the `fn` parameter).
- [saffron-dyn-witness-fn-arity-defaults-unary](saffron-dyn-witness-fn-arity-defaults-unary.md):
  the note this was found beside.
