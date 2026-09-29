# Calling a `fn` parameter in an instance body spells the prototype from the call, not from the fn

**RESOLVED 2026-09-29**, both halves.  Pinned by
`tests/fixtures/instance-fn-param-call-result-tyvar` and
`tests/fixtures/instance-fn-param-call-any-args`.

## Resolution

Neither half was really the emitter spelling the prototype from the wrong
place: in both, the ELABORATED call disagreed with the function value, and the
emitter faithfully spelled what it was given.  So both fixes are in
elaboration, and the emitter's Phase F arm is unchanged.

- **Result half.**  The spec a `: b` result mints was bound to the class
  variable only (`a := Pt`), so inside it `b` stayed a free type variable and
  every `b` the body held -- the call through `g` and its temp included --
  was spelled as the int64 carrier.  The non-HKT dispatch
  (`elab_typeclasses.c`, "M4c Path A step 1") now also records the METHOD's
  own type variables, solved from the arguments exactly as the M7 path solves
  an HKT method's (receiver first, then the arguments in order, first binding
  wins).  Only a ground solution is recorded; a name the class variable or
  the instance head already binds keeps that binding.  With `b := float` in
  the spec, the body's call is `double (*)(void*, double, double)`.
- **Argument half.**  `elab_poly_call` (the call through a `tur_poly_fn_t`
  carrier) never widened an argument: a typed carrier whose parameter is
  `any` -- an instance method's unannotated `g`, typed from its class --
  received the raw double.  It now widens a concrete argument to `any` where
  the carrier's parameter is `any`, the same `elab_coerce_to_any` the
  direct-call path's IT4 widen uses.

The third symptom named here,
[static-instance-spec-calls-any-lambda-as-concrete-result](static-instance-spec-calls-any-lambda-as-concrete-result.md),
was NOT covered by either fix -- there the function value itself returns
`any` where the spec expects `double`, which needs a marshalling adaptor at
the dispatch.  It was resolved the same day with the `any` bridge; see there.


**Severity: high.** A silent wrong answer in typed Turmeric with no `any` in
sight (the result half, below), and a panic from the same line when the fn's
parameters are `any` (the argument half). Compiled only; `--interpret`
answers. Filed 2026-09-28, found while reducing
[erased-instance-body-tags-a-type-variable-widened-to-any](erased-instance-body-tags-a-type-variable-widened-to-any.md).
The open
[static-instance-spec-calls-any-lambda-as-concrete-result](static-instance-spec-calls-any-lambda-as-concrete-result.md)
is a third symptom of the same line.

Inside an instance body, a call through a `fn`-typed method parameter is
emitted by casting `g.fn` to `R (*)(void *, A1, ...)` and calling it. `R` is
taken from the call expression's own type and each `Ai` from the argument
expression's type. Neither is necessarily the function value's actual
signature, and when they differ this is a mismatched function-pointer call.

## Repro -- the result half (silent wrong answer)

```turmeric
(defclass Ap2 [a]
  (ap2 [x : a g : (fn [float float] b)] : b))
(defstruct Pt [x : float y : float])
(definstance Ap2 [Pt]
  (ap2 [p g] (g (.x p) (.y p))))
(defn main [] : int
  (println (ap2 (Pt 1.5 2.25) (fn [a : float b : float] : float (+ a b))))
  0)
```

Measured 2026-09-28 with `./build/tur` (v0.56.2, x86-64 Linux, gcc 13):

- `tur --interpret`: `3.75` (expected).
- `tur run`: `4.61563e+18`, exit 0. That is 3.75's IEEE bit pattern read as an
  integer and converted to double.

The spec is minted correctly, and its result type is `double`. But the call
inside it is still spelled from the erased `b`:

```c
static double __inst_Ap2_ap2_Pt__spec__double_tur_adt_Pt_int64_t(tur_adt_Pt p, tur_poly_fn_t g) {
        int64_t __ps_177 = (((int64_t (*)(void*, double, double))g.fn)(g.env, (double)(p).x, (double)(p).y));
        if (tur_panicking) return ((double)0);
        return __ps_177;
}
```

The lambda (`static double __poly_12(void *, double, double)`) leaves its
result in `xmm0`. The caller reads `rax` and value-converts it. What `rax`
holds is whatever was last there, so the printed number is not even stable
across compilers or optimization levels.

## Repro -- the argument half (panic)

```turmeric
(defclass Ap2 [a]
  (ap2 [x : a g : (fn [any any] any)] : any))
(defstruct Pt [x : float y : float])
(definstance Ap2 [Pt]
  (ap2 [p g] (g (.x p) (.y p))))
(defn main [] : int
  (ap2 (Pt 1.5 2.25) (fn [a : any b : any] : any (do (println (cast a float)) a)))
  0)
```

`--interpret` prints `1.5`. `tur run` panics `cast: any holds unknown, not
float`. The body passes raw doubles where the fn takes `tur_tagged_t`:

```c
tur_tagged_t __ps_62 = (((tur_tagged_t (*)(void*, double, double))g.fn)(g.env, (double)(p).x, (double)(p).y));
```

Control: the same call in a plain `defn` whose parameter is `(fn [any any]
any)` prints `1.5` on both back ends. That path goes through the typed thunk
and wraps each argument in `TUR_TAG(4, <bits>)`.

## Root cause

`src/compiler/emit_expr.c`, the Phase F arm of the fn-value call:

- `phase_f_concrete` is decided at ~8770. It is true for the typed-carrier
  case (`fn_binding->poly_type` is a `TY_FN`), which is every one of these.
- The cast's result is `emit_type_c_name(ctx, e->type)` at 8922: the CALL's
  type. In a cloned instance spec that is still the erased body's type
  (`int64_t` for `b`), and in the static-instance-spec report it is the
  spec's `double` where the lambda returns `tur_tagged_t`.
- Each parameter is `emit_type_c_name(ctx, e->as.call_.args[i]->type)` at
  8931: the ARGUMENT's type (`double`), not the fn's declared parameter type
  (`any`). No widen is inserted.

The F5 comment above the arm says the intent is to "cast fn.fn to the exact
R(*)(void*, A...) signature" of the carrier binding. The code spells it from
the call site instead.

## Fix directions

1. Spell the prototype from the function value's own type:
   `fn_binding->poly_type` when `typed_carrier`, resolved through the spec's
   bindings (`emit_resolve_type`), so an instantiated `b` gives `double` and
   an `any` parameter gives `tur_tagged_t`.
2. Coerce at both ends of the call. Widen each argument to its declared
   parameter type (the `EX_UNION_INJECT` widen the plain-`defn` path already
   gets) and convert the declared result to the call's type (a checked unbox
   when the fn returns `any`, which is also the static-instance-spec report's
   fix 2). Ideally, elaboration inserts these as ordinary nodes, so the
   emitter's job is only to spell a signature that matches the value.
3. Add fixtures for both repros above on both back ends. The result half needs
   an exact-output check, since it exits 0.
