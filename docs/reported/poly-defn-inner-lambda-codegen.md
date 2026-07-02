---
title: Polymorphic defn whose body returns an inner lambda drops the lambda in codegen
severity: MEDIUM. Blocks fully-polymorphic higher-order combinators.
status: OPEN. Found 2026-07-01 while rewriting the parsec-tutorial fixture in HOC style.
---

# Polymorphic defn + inner-lambda body: codegen references an undefined `__fn_NNNN`

## Symptom

A polymorphic combinator that constructs and returns an inner `fn` value
compiles through elaboration but fails at the C-compile stage with:

```
error: use of undeclared identifier '__fn_1274'
error: call to undeclared function '__fn_1276'; ISO C99 and later do not
       support implicit function declarations
```

Reduced repro:

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

(defn main [] : int 0)
```

The generated C constructs the closure environment struct and stashes
`__fn_1274` (the intended lambda body's function pointer) into it, but
`__fn_1274` is never defined -- the polymorphic lambda emission was
skipped.  A monomorphic sibling of the same combinator (e.g. spelled
`or-int` without the `[A]` type parameter) compiles and runs fine, so
the failure is specifically the polymorphic-`defn` + inner-`fn` combo.

## Why it matters

Every classical combinator library (`or`, `bind`, `map`, `then`, `many`)
is a polymorphic `defn` whose body returns an inner lambda.  Without
this working, higher-order combinators must be spelled monomorphically
-- one instance per element type -- which forces the parser tutorial
(`tests/fixtures/parsec-tutorial/input.tur`) to define `or-int`,
`or-expr`, `map-int-to-expr`, etc. as separate top-level functions
instead of `(or-parser p q)` once.

## Elaboration side is fine

The elaborator produces a well-typed AST -- both arms of the inner
match now unify at TY_APP(PRes, A) (fixed 2026-07-01 in
`src/compiler/elab_call.c` / `elab_fns.c`).  Only the C emit stage
drops the lambda function.

## Fix directions

- Trace the lambda-body emission path (search for the numbered `__fn_`
  emit) and check the guard that skips emission when the enclosing
  defn is polymorphic.  The polymorphic defn wants the lambda's body
  emitted once (with the outer's tyvars still in scope) and its
  function pointer stashed into the env exactly as the monomorphic
  path already does.
- Alternatively, monomorphize each call site so the lambda's `[A]` is
  resolved before emit.  This aligns with the "end-to-end
  monomorphization" plan
  (docs/upcoming/end-to-end-monomorphization-plan.md).

## Related

- [defdata-parametric-inference-and-elab-match-segv.md](../archive/defdata-parametric-inference-and-elab-match-segv.md)
  -- the elab-side inference gap for parametric constructors (resolved).
- [defdata-parametric-forward-decl-inference.md](defdata-parametric-forward-decl-inference.md)
  -- sibling forward-referenced defn compound return-types.
