# A static instance specialization calls an `any`-returning lambda as if it returned the concrete type

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
