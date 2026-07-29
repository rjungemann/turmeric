# An `ap` specialization records the wrong function-element type for its call site

**Severity:** medium. Was invisible and harmless; is now a hard `cc` failure in
two fixtures. Not a wrong-answer bug at any point -- see
[Why it only appeared now](#why-it-only-appeared-now).

**Status:** open. Exposed 2026-07-29 by the `append_type_mangle` injectivity fix
in [concrete-codegen-layout-kind-enumerations-drift](concrete-codegen-layout-kind-enumerations-drift.md).

**Failing fixtures:** `tests/fixtures/hkt-ap-fn-in-container`,
`tests/fixtures/conv-defstruct-option-fn-element` -- both `build failed`.

## Symptom

```
error: incompatible type for argument 1 of
  '__inst_Applicative_ap_Option__spec__tur_adt_Option__float_tur_adt_Option__fn1_int__int_tur_adt_Option__float'
  __auto_type __ps_190 = (..._Option__float_Option__fn1_int__int_Option__float(ff_1326, fa_1327));
```

Read the spec's name: result `(Option float)`, **arg1 `(Option (fn [int] int))`**,
arg2 `(Option float)`. That arg1 is wrong. At this call site `ff` is
`(Option (fn [float] float))` -- the fixture's last case, which exists precisely
to check that the result type grounds from the function element's *result*:

```turmeric
(let [ff (:: (some (fn [x : float] : float (+ x 0.5))) (Option (fn [float] float)))
      fa (:: (some 41.25) (Option float))]
  (println (opt-fval-x100 (ap ff fa))))                            ; 4175
```

So the emitter minted a specialization whose *recorded* argument type is the
`(fn [int] int)` from an earlier call site, while the C variable `ff_1326` it
passes has the float-fn type. `emit_abi_intern_spec` compares candidate specs
with `type_eq` per argument, and `type_eq` does distinguish those two `TY_FN`s
(it compares arity, `arg_kinds`, `result_kind`) -- so the fault is not the
*matching*. Something upstream handed `emit_abi_register_call` the wrong
`arg_types[0]` for this call site.

Both specs the program emits, for reference:

```
__inst_Applicative_ap_Option__spec__tur_adt_Option__int_..._fn1_int__int_..._Option__int     <- correct
__inst_Applicative_ap_Option__spec__tur_adt_Option__float_..._fn1_int__int_..._Option__float <- arg1 wrong
```

The second has a float result and a float second argument, so the specialization
*was* keyed on the float call site -- only the function-element argument kept the
other site's type.

## Why it only appeared now

Before the mangling fix, `TY_FN` had no case in `append_type_mangle` and fell to
`default: "opaque"`. Both `(fn [int] int)` and `(fn [float] float)` therefore
mangled to the same token, so both spellings of arg1 printed as
`tur_adt_Option__opaque` -- the declared parameter and the passed variable agreed
*as C types*, and the C compiler had nothing to complain about.

It was also genuinely harmless, which is worth stating so this is not mistaken
for a latent miscompile: a wrapped function is carried as a handle of one word
either way, so `Option__opaque` (`{bool is_some; int64_t value}`) is a correct
layout for both. The int64-through-`double` hazard described in the sibling
report applies to the *element field of a by-value product*, not to this
argument.

So the ordering is: the mangling fix did not introduce a defect, it removed the
coincidence that was hiding one. The two fixtures went from "passing with a
mismatch nobody could see" to "failing with the mismatch named".

## Fix directions

1. **Find where `arg_types[0]` comes from for this call.** The spec is correct in
   its result and second argument, so whatever computes the argument types is
   right for those and wrong for the function-in-container one -- likely reading
   the fn element from the instance's declared signature (or from the first
   instantiation it saw) rather than from the call site's own elaborated type.
   `emit_abi_register_call` in `src/compiler/emit_module.c` is the entry point;
   `elab_call.c`'s `abi_bindings` construction is the other candidate.
2. **Assert rather than trust.** Once (1) is understood, a cheap invariant is
   worth adding: a spec's recorded `arg_types[i]` should `type_eq` the elaborated
   type of the argument expression at every call site mapped to it. That is the
   property that was silently false here.

## Note on carrying these two red

Landed red deliberately, per CLAUDE.md's fixture policy: the mangling fix closes
a live silent-miscompile, and these two failures are a pre-existing defect made
visible, not a regression introduced by it. A hard `cc` error is a better
resting state than a merged C name. Reverting the mangling fix would make both
fixtures pass again and restore the collision -- not a trade worth making.
