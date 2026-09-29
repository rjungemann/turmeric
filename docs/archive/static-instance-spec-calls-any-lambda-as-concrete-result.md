# A static instance specialization calls an `any`-returning lambda as if it returned the concrete type

**RESOLVED 2026-09-29.**  Pinned by
`tests/fixtures/saffron-static-instance-spec-any-lambda` (the repro, plus a
summing `.foldl` and a `.foldr`).

## Resolution

Fix direction 2, generalised.  A function value whose signature differs from
the slot it fills only in WHERE `any` appears is a different calling
convention (`any` is the 16-byte `tur_tagged_t`), not a subtype, so it is now
marshalled with an adaptor -- the `any` bridge, `elab_fn_any_bridge`
(`elab_call.c`):

    (let [__sfnb_N <arg>]
      (fn [__sa0 : W0 ...] : WR (cast? (__sfnb_N (cast? __sa0 H0) ...) WR)))

A slot that is `any` in the target where the value's is concrete gets a
checked `cast` down; a concrete target slot over an `any` one is widened by
the ordinary call / `: any` return.  The already-elaborated argument is bound
to the temp directly, so a lambda literal is not lifted twice.  It declines
unless both signatures are ground, arities match, and every slot pair agrees
except for `any`.

At a method dispatch (`elab_method_call`) it runs before the poly-fn packing,
with the method's type variables solved as the spec's bindings are, so the
adaptor and the spec agree.  The target is the signature the INSTANCE BODY
calls the parameter through: its binding's `poly_type`, not the class's -- and
for a Saffron instance body, that signature with every bare, otherwise
unpinned type-variable parameter read as `any`, because a Saffron body
ascribes such an argument to `any` (D8 Q3; the decision is now one helper,
`saffron_bare_tyvar_param_widens`, shared by that rewrite and the bridge).
For the repro, Foldable's `(fn [b a] b)` at `b := float` becomes the target
`(fn [any any] float)`: the lambda's `any` result is cast to `float`.

The same bridge closed half of
[concrete-result-fn-passed-where-an-any-result-fn-is-expected](concrete-result-fn-passed-where-an-any-result-fn-is-expected.md)
(the plain-call direction).


**Severity: high (silent wrong answer on Linux, crash on Windows).** Filed
2026-09-26 while driving the open-reports PR's Windows suite to green.

## Summary

When a typeclass method call on a Saffron value resolves STATICALLY -- the
receiver's type is known, so the call goes to the instance specialization
`__inst_Foldable_foldl_Two__spec__double_...` rather than through `__tur_dm` --
the specialization is instantiated with the accumulator at the concrete type of
`init` (`double`). The lambda argument, though, is an ordinary Saffron lambda:
unannotated, so every parameter AND its result are `any`, and it lifts to a
`__poly_N` returning `tur_tagged_t`. The specialization then calls that lambda
through a prototype that says it returns `double`:

```c
double __ps_214 = (((double (*)(void*, tur_tagged_t, tur_tagged_t))f.fn)(f.env, ...));
```

It is a mismatched function-pointer call, so what comes back depends on the
ABI:

- **SysV x86-64 (Linux, macOS Intel):** the 16-byte struct comes back in
  `rax:rdx`; the caller reads `xmm0`, which holds whatever was last put there.
  When the lambda's body happens to have computed a float into `xmm0` (an
  `(+ acc x)` does), the answer is right by accident. Otherwise it is garbage.
- **Win64:** a 16-byte struct is returned through a hidden result pointer passed
  in `rcx`, so the callee writes its result through the caller's `f.env` and
  reads its own env from the next register. The program crashes with no output.

## Repro

```turmeric
#lang saffron
(load "stdlib/rc.tur")
(defdata Two [a] (Two [l : a r : a]))
(definstance Foldable [Two]
  (foldl [t init f] (f (f init (.l t)) (.r t)))
  (foldr [t init f] (f (.l t) (f (.r t) init))))
(defn main []
  (let [t (Two 1.5 2.25)]
    (println (.foldl t 0.0 (fn [acc x] (if (> x 2.0) acc x))))
    (println (.foldl t 0.0 (fn [acc x] 9.75))))
  0)
```

`tur --interpret` prints `1.5` and `9.75`. `tur run` on x86-64 Linux printed
`2.25` and `4.68416e-310`, and exited 0.

The same `.foldl` with an `(fn [acc x] (+ acc x))` lambda prints the right sum
on Linux, which is how the original `saffron-dyn-method-in-argument-position`
fixture passed there while crashing on the Windows runner. That fixture now
routes its receivers through `any` parameters, so every call goes through
`__tur_dm` -- the shape it was written to pin -- and no longer reaches this
defect. Nothing in the suite does today.

## Root cause

The specialization's `b` (accumulator/result) is grounded from `init`'s static
type, while the function argument's result is `any`: the two disagree on the
C type of the same value, and nothing bridges them. The emitter spells the
callee cast from the specialization's view (`double`), not from the value's
own fn type (`tur_poly_fn_t` over a `tur_tagged_t`-returning shim).

Where the cast is built: the call of a `tur_poly_fn_t`-held parameter in the
cloned instance body -- the "Phase F" arm under `phase_f_concrete` in
`src/compiler/emit_expr.c` (~line 8896), which spells
`((R (*)(void*, ...))f.fn)(f.env, ...)` with `R` from the call's own type.

## Fix directions

1. Elaboration: when the specialization is chosen, unify the lambda's result
   with `b`. An unannotated Saffron lambda whose body can produce the concrete
   type could be re-typed to return it, or
2. Emission: when the declared result of the specialization's `f` is concrete
   but the argument's fn type returns `any`, pass an adaptor (unbox the
   `tur_tagged_t` result to `b`'s C type, with the tag check the `any` cast
   already makes), exactly as the seam into a typed fn parameter does
   (`saffron-seam-into-typed-fn-param`).
3. Either way, add a fixture: the repro above, plus the Windows runner will
   crash on it until fixed, so it pins both ABIs.

## Related

- [fn-param-call-prototype-spelled-from-the-call-not-the-fn](fn-param-call-prototype-spelled-from-the-call-not-the-fn.md)
  (filed 2026-09-28): the same Phase F line spells the prototype from the
  call's view in two more ways -- the erased result in a spec (a silent wrong
  answer in typed Turmeric, no `any` involved) and the arguments' types where
  the fn takes `any`. One fix, spelling the prototype from the fn value's own
  type, should cover all three.
