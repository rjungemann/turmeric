# A function-typed element's tyvars are never substituted into a spec's arg types

*(Filed as "an `ap` specialization records the wrong function-element type for
its call site". That framing was disproven -- see
[Root cause](#root-cause--found-and-it-is-not-a-different-call-site).)*

**Severity:** medium. Was invisible and harmless; is now a hard `cc` failure in
two fixtures. Not a wrong-answer bug at any point -- see
[Why it only appeared now](#why-it-only-appeared-now).

**Status:** open, root cause **found** (2026-07-30) and a fix attempted and
**reverted** -- see [Attempted fix](#attempted-fix-and-why-it-was-reverted).
Exposed 2026-07-29 by the `append_type_mangle` injectivity fix in
[concrete-codegen-layout-kind-enumerations-drift](concrete-codegen-layout-kind-enumerations-drift.md).

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

So the emitter minted a specialization whose *recorded* argument type is an
int-fn while the C variable `ff_1326` it passes has the float-fn type.
`emit_abi_intern_spec` compares candidates with `type_eq` per argument and
`type_eq` does distinguish those two `TY_FN`s, so the fault is not the
*matching* -- something handed `emit_abi_register_call` the wrong
`arg_types[0]`.

(This section's original guess was that the int-fn came "from an earlier call
site". It does not: it is the declared parameter type, identical at every call
site, never substituted. See [Root cause](#root-cause--found-and-it-is-not-a-different-call-site).)

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

## Root cause -- found, and it is not "a different call site"

**The title of this report is wrong.** Nothing stale is involved and no other
call site contributes. Instrumented `emit_abi_intern_spec` to print each mint
with its source line and the ABI bindings in force:

```
[mint] line 34  ap :: Option__fn1_int__int Option__int    -> Option__int    | bindings: f=int64_t a=int64_t b=int64_t
[mint] line 51  ap :: Option__fn1_int__int Option__float  -> Option__float  | bindings: f=int64_t a=double  b=double
```

The bindings at line 51 are right (`a`/`b` = `double`), and there is no `TY_APP`
binding for the loop that adopts a receiver type to pick up. Every call site
gets the *same* arg0, because arg0 is the class method's declared parameter type
and it was never substituted at all. Line 34 is correct only because `int` is
what the erasure happens to leave behind.

Dumping the declared type of `ap`'s param 0:

```
[decl] p0 kind=TY_APP elem_kind=TY_FN arity=1
       arg_kinds[0]=TY_INT  result_kind=TY_INT          <- erased
       arg_full_types[0]->kind=TY_TYVAR
       result_full_type->kind=TY_TYVAR                  <- tyvars retained here
```

So `(f (a -> b))` keeps `a`/`b` **only** in the fn's out-of-line
`arg_full_types` / `result_full_type`. The in-line `arg_kinds` / `result_kind`
are `TypeKind` enums, which cannot name a tyvar, so instance elaboration erased
them to the int64 carrier. And `emit_abi_instantiate_type`
(`src/compiler/emit_module.c`) switches on only `TY_TYVAR`, `TY_APP`,
`TY_UNION`, `TY_INTERSECTION` -- **`TY_FN` falls to `default: return *t`**. The
`TY_APP` arm recurses into the element, the element is a `TY_FN`, and the
substitution stops there.

`emit_resolve_type` (`src/compiler/emit_core.c`) has the identical four-arm
switch and the identical gap, which is why the `__cps` twin's signature also
kept `Option__fn1_int__int` while its name and its other parameter substituted.

## Attempted fix, and why it was reverted

Added a `TY_FN` arm to both functions: substitute through `arg_full_types` /
`result_full_type`, then re-derive the erased `arg_kinds` / `result_kind` from
the results. It does exactly what it should at the mint site --

```
[mint] line 51  ap :: Option__fn1_float__float Option__float -> Option__float
```

-- and it clears both the `incompatible type` and the `conflicting types`
errors. **It is also badly wrong, and was reverted.** Measured:

| tree | `run.sh` |
| --- | --- |
| baseline | 2415 passed, **2** failed |
| + arm in `emit_abi_instantiate_type` | 2412 passed, **5** failed |
| + arm in `emit_resolve_type` as well | 501 passed, **1916** failed |

`emit_resolve_type` runs over essentially every type in emit, so correcting a fn
element's `result_kind` there changes what `type_c_name` spells for it
everywhere at once -- and `type_c_name(TY_FN)` returns, for a bare non-boxed fn,
**its result type's C name**. The whole tree's fn spellings move. Even the
narrower `emit_module` arm alone regressed three unrelated fixtures
(`arrow-compose-float`, `sf-compose-typed`,
`load-inside-defmodule-injects-names` -- all codegen mismatches).

The next layer is visible in the fixture that remains: with the substitution in
place, `(Option (fn [float] float))`'s field is emitted as `double`, because
`type_c_name(TY_FN)` spells a stored fn element as its result type rather than
as a closure handle:

```c
static tur_adt_Option__fn1_float__float ctor_Option__fn1_float__float(bool _0, double _1);
static ... some__spec__..._fn1_float__float_void__(void * x) {
    __auto_type __ps_209 = (ctor_Option__fn1_float__float(true, x));   /* void* -> double */
```

Before the substitution `result_kind` was `TY_INT`, so the field was spelled
`int64_t` -- a correct handle width by accident. That is the coincidence the
whole area currently rests on.

## Revised fix directions

The substitution gap is real and worth closing, but not on its own -- closing it
first is what breaks things, because the rest of the pipeline is calibrated to
the erased `int` and to `type_c_name`'s result-type spelling. In order:

1. **Fix how a stored fn element is spelled.** `type_c_name(TY_FN)`'s
   "bare function reference -> result type's C name" rule is right for a call
   target and wrong for a container field, which needs the closure handle. Until
   a fn element spells as a handle regardless of its result type, substituting
   the result type into it can only make the spelling worse.
2. **Then** add the `TY_FN` arm to `emit_abi_instantiate_type` and
   `emit_resolve_type` together, and expect codegen churn across fn-heavy
   fixtures -- the three that regressed above are the canaries.
3. Consider whether `arg_kinds` / `result_kind` should exist at all as a
   `TypeKind` shadow of `arg_full_types` / `result_full_type`. Two
   representations of one fact, one of which cannot express tyvars, is the
   underlying reason this bug exists.

## Fix directions (original, superseded)

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
