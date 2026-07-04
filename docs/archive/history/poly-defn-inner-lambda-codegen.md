---
title: Polymorphic defn whose body returns an inner lambda drops the lambda in codegen
severity: MEDIUM. Blocks fully-polymorphic higher-order combinators.
status: RESOLVED 2026-07-02. The inner lambda's carrier base is now emitted when
its enclosing carrier-emitted defn takes its address.
---

# Polymorphic defn + inner-lambda body: codegen references an undefined `__fn_NNNN`

## Resolution (2026-07-02)

Two layers were involved.

**Elaboration.**  The reduced repro also failed *earlier*, at the inner match:
`(POK v rest)` (with `v : A`) inferred the bare `TY_ADT` `PRes` while the peer
arm `(q xs)` inferred the full `(PRes A)`, so the arms would not unify
("expected adt, got app").  The narrowest correct fix is in the match-arm
consistency check (`src/compiler/elab_structs.c`, new
`match_arm_type_compatible`): a bare `TY_ADT` arm and a `TY_APP` arm over the
*same* ADT def are compatible, and the unified result is promoted to the more
specific `TY_APP`.  (An earlier attempt to force the constructor itself to
build `(PRes A)` in `elab_call.c` was reverted -- it also fired for generic
library constructor bodies like `tuple2`, where the bare-`TY_ADT` carrier
result is intentional, and segfaulted `conv-defstruct-accessor-unbox`.)

**Codegen (the reported drop).**  The inner lambda `__fn_N` is generic-unsafe
(its signature names the enclosing `A`), so `emit_abi_fn_skip_generic` skips its
carrier base on the theory that it is emitted only through per-callsite
specialization clones.  But an ABI-invariant generic like `or-parser` -- whose
`:fn` params/result are boxed closures on the int64 carrier -- is itself
carrier-emitted (not generic-unsafe) and takes the *address* of that thunk in
its env-construction.  The carrier-relay closure previously only chased *calls*
from *generic-unsafe* enclosers, so it missed this address-of reference.  Fix in
`src/compiler/emit_module.c`: `emit_abi_carrier_relay_walk` now handles
`EX_CLOSURE` / `EX_VAR` references to lifted-lambda thunks (noting a carrier
call so the thunk's base is kept), and the driver visits every function that
will itself be emitted, not just the generic-unsafe ones.

The reduced repro below now elaborates, builds, and runs.  The regression
fixture `poly-defn-inner-lambda` guards it.

### Known follow-on (separate issue)

*Applying* such a combinator at a concrete element type -- e.g.
`(or-parser always-fail always-ok)` where the arguments are
`(fn [int] (PRes int))` -- still fails to infer `A = int` through the
closure-returning HOF, surfacing as "match: arm types are incompatible --
expected tyvar, got int" at the *use* site.  That is the return-type / closure-
result monomorphization gap tracked by the end-to-end monomorphization plan,
not the codegen drop this report is about; the monomorphic sibling (`or-int`,
no `[A]`) compiles and runs.  Filed separately as
[poly-combinator-application-element-inference.md](../reported/poly-combinator-application-element-inference.md).

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
