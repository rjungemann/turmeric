---
title: Return-directed typeclass methods (`pure`, `empty`) require an explicit
  type ascription at every call site
severity: MEDIUM. Ergonomic gap in the migrated by-value Applicative /
  Alternative surface. Does not break correctness -- the compiler emits a clear
  diagnostic asking for the ascription -- but it forces `(:: (pure ...) (Opt
  int))` boilerplate at every call site that could otherwise be inferred from
  the surrounding context.
status: OPEN. Filed 2026-07-02 during the `TUR_M7_HKT` retirement investigation
  (docs/upcoming/tur-m7-hkt-flag-retirement-plan.md). Independent of the
  retirement -- fixing this does not require the flag, and retiring the flag
  does not require fixing this.
---

# Return-directed methods don't propagate context-supplied element types

## Symptom

Applicative `pure` and Alternative `empty` are declared with a phantom element
type parameter (`pure :: a -> f a`, `empty :: f a`). When called in a position
where the surrounding context fully determines `f a` (a `let` with an
ascription, a function return type, a match arm sibling), the elaborator still
refuses to ground the method and asks for a manual ascription.

```turmeric
(defmodule main)

(defn main [] : int
  (let [x (pure 42)]
    0))
```

```
error: cannot infer type for return-directed method 'pure'; add a type
  ascription, e.g. (:: (pure ...) T)
```

Adding `(:: (pure 42) (Opt int))` (or an outer annotation the context could
supply) makes the program compile.

## Why this is a gap, not the intended design

The diagnostic is the intended fallback when there is genuinely no context.
The gap is that even when context exists, it isn't threaded to the method.
Concretely:

- `(let [x : (Opt int) (pure 42)] ...)` -- the binding's declared type carries
  the full `f a`, but the method call is elaborated before the binding site's
  annotation is consulted.
- `(defn f [] : (Opt int) (pure 42))` -- the enclosing return type is
  available but not propagated as an `expected_type` into the method call.
- `(match cond (True) (pure 1) (False) (some-known-opt))` -- the sibling arm
  has a concrete `(Opt int)`, but the earlier `pure 1` arm is elaborated first
  with no `expected_type`.

All three shapes could ground the method without user-visible ascriptions.

## Root cause (direction)

`elab_call` / the typeclass dispatch layer looks up a method's tyvars from
argument types (`a` in `pure :: a -> f a` grounds to `int` from the `42`), but
the *container* tyvar `f` appears only in the result. The current code paths
that could ground it -- `expected_type` from the caller's context, or a
same-scope sibling in a `match`/`if` -- either aren't consulted or aren't
consulted early enough.

## Impact

Every use of `pure` / `empty` in migrated stdlib code needs an explicit
ascription, which defeats a large chunk of the ergonomic argument for the
by-value HKT surface. In practice this pushes users toward instance-specific
constructors (`(Some 42)` instead of `(pure 42)`), which works around the
issue but undermines Applicative as a general interface.

## Fix directions

1. Thread `expected_type` from `let`-with-annotation, `defn` return type, and
   match-arm-context into the method call site so a return-directed method
   can consult it before demanding an ascription.
2. For `match` / `if`, resolve the join type of the arms lazily: if the first
   arm is a return-directed method call and a later arm is ground, retype the
   method against the joined arm type instead of failing on the first arm.
3. Long-term, aligns with the end-to-end monomorphization plan
   ([../upcoming/end-to-end-monomorphization-plan.md](../upcoming/end-to-end-monomorphization-plan.md))
   which already needs per-call-site typing information for by-value HKT
   dispatch.

## Related

- [class-method-level-hkt-tyvar-grounding.md](class-method-level-hkt-tyvar-grounding.md)
  -- a distinct HKT-dispatch inference gap surfaced in the same investigation.
- [../upcoming/tur-m7-hkt-flag-retirement-plan.md](../upcoming/tur-m7-hkt-flag-retirement-plan.md)
  -- the flag retirement is independent of this fix and should not wait.
