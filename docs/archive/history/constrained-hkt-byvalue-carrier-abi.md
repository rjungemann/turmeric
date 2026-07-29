---
title: By-value carriers through the constrained-HKT poly carrier (return-ABI mismatch)
status: RESOLVED (2026-07-29) -- dictionary-passed path; two narrower seams remain
area: compiler (src/compiler/elab_typeclasses.c)
---

# Gap 2: by-value carriers segfaulted through the poly carrier

## Symptom

A constrained kind-polymorphic fn instantiated at a by-value carrier -- the
stdlib `Option`, a real 2-word struct rather than an int-carrier `defopaque` --
segfaulted:

    (defn poly-bind [^m] [^Monad m x : (m int)] : (m int)
      (bind x (fn [v] (some (* v 2)))))
    (defn use-opt [g (forall [(m :: * -> *)] [(Monad m)] (-> (m int) (m int)))] : int
      (unwrap-or (g (some 5)) -1))
    ;; => Segmentation fault

## Root cause

Not the crossing itself -- Path A boxing already handled that. The generated C
showed `use_opt` boxing its `(Option int)` into a heap pointer on the way in and
dereferencing the returned pointer on the way out, and
`__inst_Monad_bind_Option` consuming that pointer correctly.

The fault was the **continuation**. `(fn [v] (some (* v 2)))` returns
`(Option int)`, so its poly thunk was emitted as

    static tur_adt_Option__int __poly_1309(void *env, int64_t x) { ... }

and then packed with a raw cast:

    (tur_poly_fn_t){ NULL, (int64_t(*)(void*,int64_t))__poly_1309 }

The instance invokes it as `((int64_t (*)(void*, int64_t))k.fn)(k.env, ...)`. On
x86-64 a 16-byte struct returns in RAX:RDX while an `int64_t` returns in RAX, so
the instance read a partial value, returned it as the result carrier, and the
caller dereferenced it as a pointer.

The machinery to prevent this already existed --
`ensure_aggregate_spill_shim` (`emit_module.c`), gated on
`EX_POLY_WRAP.boxes_aggregate`. That flag was set only at the rank-2/rank-3
forall argument sites (`elab_call.c`), never at the typeclass-method `:fn`
argument site (`elab_typeclasses.c`), whose comment reasoned that a typed `:fn`
continuation "is consumed BY VALUE via a concrete-cast call site". True for a
*concrete* receiver; false when the receiver is the abstract constructor and the
call lowers to a dictionary-slot dispatch with the int64 carrier ABI.

## Fix

At the typeclass-method poly-wrap site, set `boxes_aggregate` when the receiver
is this body's abstract constraint variable. The receiver is `(m int)` -- a
TY_APP spine -- so the check walks to the head before comparing against
`cur_hkt_constraint_tyvar`; a bare TY_TYVAR receiver (`obj_is_abstract_tyvar`)
also qualifies.

A concrete receiver is deliberately excluded: it resolves to the instance's own
by-value entry point and must keep consuming the struct directly, which is what
the original gate protected. `ensure_aggregate_spill_shim` is itself defensive
-- it returns NULL unless the result really is a by-value aggregate -- so the
change is a no-op for carrier-returning continuations.

## Verification

- `hkt-constrained-byvalue-carrier` -- `Monad`-constrained poly fn through a
  rank-2 `forall`, instantiated at the stdlib `Option`.
- `hkt-constrained-byvalue-bind-pure` -- `bind`-then-`pure` at `Option`,
  exercising the gap-1 return-dispatch fix and this one together.

Suite: 2403 passed, 0 failed (2401 before, plus the two new fixtures).

## Still open

- Monomorphized spec return for a *wider* by-value container --
  [../constrained-hkt-byvalue-carriers.md](../constrained-hkt-byvalue-carriers.md), seam 1
  (since RESOLVED by Route B).
- `Result`'s binary head cannot fill a unary `(m int)` -- same report, seam 2.
- A lifted continuation keeps the representative instance -- RESOLVED same day
  via Route B, see
  [../constrained-hkt-lifted-lambda-keeps-representative-instance.md](../constrained-hkt-lifted-lambda-keeps-representative-instance.md).
