---
title: Return-Type Dispatch for Nullary Arrow Methods
category: Planning
description: Resolve a nullary typeclass method whose dispatch variable is the function arrow (e.g. Category `ident`, ArrowZero `zeroArrow`) when no expected type is available, by adding a unique-instance fallback to return-type dispatch and threading the method's callable-closure result type through. Closes fix direction #3 of docs/reported/function-arrow-not-instantiable-as-typeclass-head.md.
---

# Return-Type Dispatch for Nullary Arrow Methods -- Plan

## Why

`docs/reported/function-arrow-not-instantiable-as-typeclass-head.md` is
**RESOLVED**: the function arrow `(->)` is now a first-class typeclass instance
head (`tests/fixtures/arrow-instance-basic`). That report left one caveat open
as **fix direction #3**:

> Note that even with #1, `arr :: (b->c) -> a b c` dispatches on the *result*
> type, which is the separate return-type-dispatch problem; the plan should
> treat that as an additional gate. Methods where the arrow value is an
> *argument* (`>>>`, `first`, `app`) dispatch fine once #1 exists.

Concretely, a **nullary** arrow method -- one with no argument to dispatch on,
whose result is the arrow itself -- cannot be resolved from arguments. The
planned `stdlib/arrow.tur` typeclass hierarchy has several:

- `Category`'s `id :: arr a a` (here written `ident`),
- `ArrowZero`'s `zeroArrow :: arr b c`,
- any "constant arrow" constructor that takes no arrow argument.

This plan closes that gap.

## Current behavior (measured at the post-report HEAD)

Class under test:

```turmeric
(defclass Category [a]
  (ident [] : a)            ; nullary -- dispatch variable only in the result
  (comp [f g] : a))

(definstance Category [(->)]
  (ident [] (fn [x] x))
  (comp [f g] (fn [x] (g (f x)))))
```

| Call site | Result | Note |
| --- | --- | --- |
| `(:: (ident) (fn [:int] :int))` then `(i 41)` | **works, prints 41** | ascription supplies the expected `TY_FN`; `rt_unify_return` binds the dispatch tyvar to it and `typeclass_env_lookup_instance` matches the arrow instance by `TY_FN` kind |
| `(let [i (ident)] (i 41))` | **error** `cannot infer type for return-directed method 'ident'` | no expected type in the binding |
| `(let [h (comp (ident) add1)] ...)` | **error** (same) | `comp`'s parameter is a fat-closure sink (`:ptr<void>`), so elaborating the `(ident)` argument provides no concrete `TY_FN` expected type |

So the binary/argument-dispatched methods (`comp`/`>>>`/`first`) already work
(report fix #1), and the **expected-type-present** nullary case already works.
The remaining failure is **only** the no-expected-type nullary case.

## Root cause (file:line)

- `elab_try_return_dispatch` (`src/compiler/elab_typeclasses.c:2605`) is the only
  path that resolves a return-only-dispatch method. When `e->expected_type ==
  NULL` it errors immediately (`elab_typeclasses.c:2648-2654`) -- there is no
  fallback that uses the *set of registered instances* to disambiguate.
- When it does resolve, it builds the result `EX_CALL` with type `bound`
  (`elab_typeclasses.c:2720`), the unified/ascribed type. It never consults the
  method impl's `result_full_type`, so without an expected type there is no
  result type to give the call -- and even if we picked the instance, the
  caller applying the result (`(i 41)`) needs the method's *callable boxed
  closure* result type (recovered in pass 2 by the report-#1 work) to be
  threaded here.
- The decline I added for argument-bearing arrow calls
  (`elab_typeclasses.c:2658`, `typeclass_has_arrow_instance`) is correctly gated
  on `call->as.list.len > 1`, so a **nullary** `(ident)` stays in
  return-dispatch (it does not decline) -- which is exactly the path this plan
  extends.

Supporting machinery that already does the right thing once an instance/bound
is chosen:

- `rt_unify_return` (`elab_typeclasses.c:2601`): binds a bare-tyvar return to
  the expected type wholesale -- so `bound` becomes the arrow's `TY_FN`.
- `typeclass_env_lookup_instance` (`src/compiler/typeclass.c:80`): matches by
  top-level `TypeKind`, so a `TY_FN` `bound` selects the arrow instance.

## Design

Two complementary mechanisms. **Mechanism B is the core deliverable**;
Mechanism A is an optional inference improvement.

### Mechanism B -- unique-instance fallback (core)

In `elab_try_return_dispatch`, before erroring on a missing expected type, try
to resolve from the instance set:

1. Collect the registered instances of `tc` that implement `meth` (walk
   `env->instances`, filter `inst->typeclass == tc` with a non-NULL
   `inst->method_impls[midx]`).
2. **If exactly one such instance exists**, select it without an expected type.
3. Set `bound` to that instance's head type (its `type_args[0]`), so the
   downstream `typeclass_env_lookup_instance`/result wiring is uniform.

Scope/gating: restrict the fallback to the case where the unique instance's
head is the function arrow (`type_args[0].kind == TY_FN`). This keeps the change
aligned with the report and avoids changing behavior for existing return-only
classes (`default-of`, `schema-of`, ...), which have multiple instances and so
never hit a *unique*-instance fallback anyway. (A strictly-more-general "exactly
one instance of any kind" rule is a clean follow-up -- it is a pure improvement,
since the alternative today is a hard error -- but is intentionally out of scope
here to keep the blast radius minimal.)

This single mechanism closes **both** failing cases above: `(ident)` resolves to
the only `Category` instance regardless of surrounding context.

### Thread the callable result type (required for B)

Whichever way the instance is chosen with no expected type, the result `EX_CALL`
must carry a **callable** type so a caller can apply it (`(i 41)`). Mirror the
result-type logic already in `elab_method_call`
(`elab_typeclasses.c`, the `result_full_type && boxed` branch added by report
#1):

- if `impl->binding->type.as.fn.result_full_type` is a **boxed** `TY_FN`, use it
  as the `EX_CALL` result type (the arrow body `(fn [x] x)` was refined to a
  boxed closure in pass 2);
- otherwise fall back to `bound` (the current behavior, correct for the
  ascription path).

Factor this into a small helper so `elab_method_call` and
`elab_try_return_dispatch` share one definition.

### Mechanism A -- expected-type propagation into fat-closure args (optional)

For a future class that has an arrow instance **and** other instances (so B's
uniqueness gate does not fire), inference can still be improved: when an
argument is elaborated for a fat-closure (`is_fat`) parameter, set
`e->expected_type` to the function-arrow marker for that sub-elaboration. A
nested nullary return-dispatch method then unifies its result tyvar against the
arrow marker (`rt_unify_return` already binds a bare tyvar wholesale) and selects
the arrow instance.

This requires knowing the callee's fat params *before* elaborating the argument.
In `elab_method_call` the receiver `obj` is elaborated before dispatch is
resolved, so the hint must be derived structurally (e.g. "the method has an
arrow instance" -> hint arrow for its non-receiver args). Because this is
heuristic and only matters for multi-instance arrow classes that do not yet
exist, it is a **stretch goal**, documented here but not required for the plan
to land.

## Tasks

### T1 -- Prerequisite check
Confirm report #1 is in the tree: `tests/fixtures/arrow-instance-basic` passes,
and the ascription case `(:: (ident) (fn [:int]:int))` already resolves. If
either is absent, stop -- this plan depends on the arrow instance head and the
pass-2 boxed-result refinement.

### T2 -- Shared callable-result helper
Extract the "prefer `result_full_type` when it is a boxed `TY_FN`, else use the
carrier/`bound`" logic into one helper, and route the existing
`elab_method_call` result-type computation through it. No behavior change;
pure refactor verified by the report-#1 fixtures staying snapshot-stable.

### T3 -- Unique arrow-instance fallback in return dispatch
Implement Mechanism B in `elab_try_return_dispatch`: before the
`!e->expected_type` error, scan for a unique arrow instance of `tc`+`meth`;
if found, set `bound` to its head and proceed. Use the T2 helper for the result
`EX_CALL` type so `(i 41)` is callable.

### T4 -- Preserve the ambiguity error
When no expected type is present and the class has **more than one** instance
(or the unique instance is not an arrow), keep emitting the existing
`cannot infer type ...; add a type ascription` diagnostic. Add a targeted
message variant for the "ambiguous between arrow and N other instances" case if
it reads more clearly. This path must remain an error -- never silently pick an
instance when the choice is genuinely ambiguous.

### T5 -- Fixtures
- `tests/fixtures/arrow-instance-nullary` (stdout-only, mirroring
  `arrow-instance-basic`): a `Category [(->)]` instance exercising
  - standalone `(let [i (ident)] (i 41))` -> `41`,
  - composition `(let [h (comp (ident) add1)] (h 41))` -> `42`,
  - the ascription path `(:: (ident) (fn [:int]:int))` (regression for the
    already-working case).
- A negative/error fixture (if the suite supports expected-error fixtures;
  otherwise document in the plan) for a class with two instances where a nullary
  method without ascription must still error.

### T6 -- Full suite + report/plan updates
- `bash tests/run.sh` -> zero `FAIL`.
- Update `docs/reported/function-arrow-not-instantiable-as-typeclass-head.md`:
  move fix direction #3 from "out of scope" to resolved (with the nullary-arrow
  caveat about multi-instance ambiguity).
- Update `docs/upcoming/stdlib-arrow-typeclass-reintroduction-plan.md`: drop the
  "return-type dispatch for nullary arrow methods (fix direction #3) as the
  remaining caveat" note now that it is addressed.

## Edge cases

- **Nullary method, two instances, no expected type** -> ambiguous, must error
  (T4). Ascription or a future Mechanism-A hint resolves it.
- **Nullary method used where an expected `TY_FN` exists** (ascription, a
  concretely-fn-typed binding/param) -> already works; T2's helper must not
  regress it (it falls through to `bound`).
- **`comp`/`>>>`/`first`** (argument-dispatched) -> unaffected; they decline
  return-dispatch via the existing `call->as.list.len > 1` guard.
- **Result application arity** -> `(i 41)` is callable only because the arrow
  body `(fn [x] x)` was refined to a boxed `TY_FN` in pass 2 (report #1). A
  nullary arrow method whose body is *not* a literal closure (e.g. returns
  another arrow method's result) inherits whatever that sub-expression's type
  is; verify with a fixture if such a shape is added.

## Risks

- **Over-eager fallback.** Gating to a *unique arrow* instance (T3) keeps the
  fallback from firing for existing multi-instance return-only classes. If the
  general "unique instance of any kind" rule is later adopted, re-audit the
  return-only classes (`default-of`, `schema-of`, `decode!`) -- each currently
  relies on the expected type and has multiple instances, so they should be
  unaffected, but confirm by fixture.
- **Result-type threading.** The T2 refactor touches the hot
  `elab_method_call` result path; the report-#1 closure-return snapshots
  (`instance-closure-return-*`, `instance-intra-method-dispatch`) are the guard
  rail -- they must stay byte-identical.

## Out of scope

- Mechanism A (expected-type propagation into fat-closure arguments) beyond the
  documented stretch description.
- General "unique instance of any kind" return-dispatch (a follow-up).
- The bare-function-layer limitation that a raw `(arr f)` result is applied via
  `TUR_APPLY1` rather than direct call -- unchanged and orthogonal.

## Validation summary

- Repros: standalone and composed `(ident)` both run without ascription;
  ascription path still runs.
- `tests/fixtures/arrow-instance-nullary` green; `arrow-instance-basic` and the
  `instance-closure-return-*` snapshots unchanged.
- `bash tests/run.sh` -> `... passed, 0 failed`.
