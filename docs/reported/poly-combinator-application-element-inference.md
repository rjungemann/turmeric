---
title: Applying a closure-returning polymorphic combinator doesn't infer its element type
severity: MEDIUM. Blocks *using* fully-polymorphic higher-order combinators;
they must still be spelled monomorphically at each element type.
status: OPEN. Split off 2026-07-02 from poly-defn-inner-lambda-codegen.md (the
codegen-drop half of that report is resolved).
---

# Closure-returning combinator application doesn't ground its element tyvar

## Symptom

A polymorphic combinator whose result is a closure over a parametric ADT
compiles and (as of 2026-07-02) emits its inner lambda correctly.  But
*applying* it at a concrete element type fails to infer the type parameter, so
the returned closure's result type stays a bare tyvar and a downstream `match`
rejects its arms.

```turmeric
(defdata PRes [a]
  (PFail)
  (POK a int))

(defn or-parser [A] [p : (fn [int] (PRes A)) q : (fn [int] (PRes A))]
    : (fn [int] (PRes A))
  (fn [xs : int] : (PRes A)
    (match (p xs)
      (POK v rest) (POK v rest)
      (PFail)      (q xs))))

(defn always-fail [xs : int] : (PRes int) (PFail))
(defn always-ok   [xs : int] : (PRes int) (POK 42 xs))

(defn main [] : int
  (let [combined (or-parser always-fail always-ok)]   ;; A should ground to int
    (match (combined 7)
      (POK v rest) v
      (PFail)      (- 0 1))))
```

```
error [TUR-E0001]: match: arm types are incompatible --
  expected tyvar (from earlier arm), got int
```

`A` should unify to `int` from the arguments' `(fn [int] (PRes int))` type, so
`(combined 7)` should have type `(PRes int)` and the arms `v : int` / `(- 0 1)`
should agree.  Instead `A` is never grounded through the closure result, so the
first arm's `v` stays the bare tyvar.

## Not the codegen drop

This is the *application-site* inference gap, distinct from the codegen drop
resolved in
[../archive/poly-defn-inner-lambda-codegen.md](../archive/poly-defn-inner-lambda-codegen.md).
Evidence they are separate:

- The reduced repro there (a defined-but-unapplied `or-parser`, `main` returns
  0) now builds and runs.
- The **monomorphic sibling** compiles and runs (prints `42`):

  ```turmeric
  (defn or-int [p : (fn [int] (PRes int)) q : (fn [int] (PRes int))]
      : (fn [int] (PRes int))
    (fn [xs : int] : (PRes int)
      (match (p xs)
        (POK v rest) (POK v rest)
        (PFail)      (q xs))))
  ```

So the failure is specifically grounding the combinator's `[A]` from a
function-typed argument's own result type, threaded through the combinator's
closure return.

## Root cause (direction, not yet pinpointed)

`elab_call`'s type-binding collection (`call_collect_type_bindings` /
`call_instantiate_type`) grounds a callee's tyvars from argument types, and the
result-context path binds from a ground `expected_type`.  Here `A` appears only
*inside* the fn-typed parameters' result (`(fn [int] (PRes A))`) and inside the
combinator's own closure result (`(fn [int] (PRes A))`).  Neither the argument
scan (which would need to descend into the argument's `result_full_type` and
unify `(PRes A)` against the argument's `(PRes int)`) nor the return-context
path (the enclosing `let` has no annotation, so `expected_type` is absent)
grounds it, and the closure value carries the ungrounded tyvar through to the
call site.

## Fix directions

- In the argument-driven binding collection, when a parameter's declared type is
  a function type carrying the callee's tyvars in its `result_full_type`, unify
  that against the *actual* argument's `result_full_type` (e.g. `(PRes A)` vs
  `(PRes int)` -> `A = int`).  Today only the top-level parameter type is
  unified; nested-through-fn-result tyvars are missed.
- This aligns with the end-to-end monomorphization plan
  (docs/upcoming/end-to-end-monomorphization-plan.md): monomorphize the
  combinator call site so `A` is resolved before the closure result is typed.

## Impact

Every classical parser/combinator library (`or`, `bind`, `map`, `then`,
`many`) that is written as a polymorphic closure-returning `defn` can be
*defined* but not *applied* generically -- each use still needs a monomorphic
instance per element type (`or-int`, `or-expr`, ...), exactly the boilerplate
the tutorial rewrite was trying to remove.

## Related

- [../archive/poly-defn-inner-lambda-codegen.md](../archive/poly-defn-inner-lambda-codegen.md)
  -- the codegen-drop half (resolved 2026-07-02); this is the follow-on
  application-site inference gap.
- [../archive/defdata-parametric-inference-and-elab-match-segv.md](../archive/defdata-parametric-inference-and-elab-match-segv.md)
  -- the original parametric-constructor inference work.
