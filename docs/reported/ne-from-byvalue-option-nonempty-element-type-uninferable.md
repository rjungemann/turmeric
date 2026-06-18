---
title: By-value `ne-from? : (Option (NonEmpty A))` leaves the element type `A` uninferable at call sites
category: Stdlib / Type inference -- Option none-as-NULL retirement (Track A, step 4 NonEmpty half)
severity: Medium ergonomics / expressiveness hole. Retyping `ne-from?` from the
  carrier `:int` Option to a by-value `(Option (NonEmpty A))` is now unblocked on
  the codegen side (the two compiler bugs that gated it are resolved -- see
  Related), but the retype is blocked by a genuine inference gap: `ne-from?`
  takes an UNTYPED carrier list (`xs : int`), so its result element type `A`
  has no witness and stays a free tyvar at every call site. Downstream consumers
  (`ne-head`, `println`) then fail to resolve an overload for a bare tyvar.
status: OPEN. `ne-from?` / `ne-unwrap` remain on the carrier ABI in
  `stdlib/refined.tur` with a pointer to this report. The BoundedIdx half of
  step 4 (`bidx-of?` / `bidx-unwrap` -> by-value `(Option BoundedIdx)`) IS
  landed -- BoundedIdx is ground, so it has no element-type ambiguity.
---

# `ne-from?` by-value retype: `A` is uninferable from an untyped list

## Context

`docs/reported/option-consumer-retype-byvalue.md` step 4 retypes the refinement
smart constructors to by-value `(Option X)`. The two compiler bugs that gated it
are now resolved:

- `docs/archive/zero-arg-construct-ground-byvalue-return.md` (0-arg `(none)` in a
  ground `(Option T)` return) -- fixed.
- `docs/archive/parametric-option-return-clone-struct-app-leak.md` (compiler leak
  on a doubly-nested parametric return) -- no longer reproduces on the current
  tree (verified with LSan on the exact repro).

The **BoundedIdx half** of the retype landed on the back of the ground fix:
`bidx-of? : (Option BoundedIdx)` and `bidx-unwrap [o : (Option BoundedIdx)]`
are now pure-Turmeric by-value, and `tests/fixtures/refined-bounded-idx/`
ascribes nothing at its `(some? ...)` sites. BoundedIdx is a *ground* type, so
the Option's element is fully determined.

## The NonEmpty blocker

`NonEmpty` carries a *phantom* element type `A` (`(defopaque NonEmpty [A] :int)`).
The smart constructor recovers a `NonEmpty` from an arbitrary list:

```turmeric
(defn ne-from? [A] [xs : int] : (Option (NonEmpty A))
  (if (tnil? xs) (none) (some (:: xs (NonEmpty A)))))
```

`xs : int` is the untyped carrier cons-list pointer -- it carries no element
type. So `A` appears only in the return position with no argument witness, and
at a call site it stays a free type variable:

```turmeric
(let [o (ne-from? (tcons 7 (tnil)))]      ; A unconstrained
  (if (some? o) (println (ne-head (ne-unwrap o))) (println -1)))
```

`(ne-head (ne-unwrap o))` has type `A` (a bare tyvar), and:

```
error [TUR-E0006]: operator lookup failed for 'println': got 1 arg(s),
                   first arg type tyvar
```

The carrier version sidesteps this entirely: it returns a bare `:int`, so there
is no `A` to infer, and `ne-unwrap` returns the concrete `(NonEmpty int)`.

## Why this is not just an inference quirk

The element type of a `NonEmpty` recovered from an *untyped* `:int` cons list is
genuinely unknowable -- there is no value-level or type-level witness to recover
it from. This is a design tension, not a missing unification rule: a sound
by-value retype needs `ne-from?` to take a *typed* list (e.g. `(Cons A)` /
`(List A)` / `(Vec A)`) so `A` has a witness, or to require callers to annotate
`A` at every use (which defeats the ergonomic goal of the retype).

## Proposed fix directions

1. Retype `ne-from?` to take a typed list -- `(defn ne-from? [A] [xs : (List A)]
   : (Option (NonEmpty A)) ...)` -- so `A` is recovered from the argument. This
   is the principled fix but cascades into the rest of the `NonEmpty` surface
   (`ne-of`, `ne-head`, `ne-tail`, `ne->list`, callers) which all thread the
   untyped `:int` carrier list today.
2. Keep `ne-from?` on the carrier ABI (current state) until the `NonEmpty`
   surface migrates to a typed list end-to-end.

## Validation

- Re-enable `ne-from? : (Option (NonEmpty A))` and `ne-unwrap [o : (Option
  (NonEmpty A))] : (NonEmpty A)`.
- `tests/fixtures/refined-nonempty/` drops its `(some? (:: o (Option int)))`
  ascriptions and calls `(some? o)` / `(ne-unwrap o)` directly, with the
  element type resolved (not a bare tyvar) at `(ne-head ...)`.

## Related

- `docs/reported/option-consumer-retype-byvalue.md` (step 4, NonEmpty half).
- `docs/archive/zero-arg-construct-ground-byvalue-return.md` (the ground fix
  that landed the BoundedIdx half).
- `docs/archive/parametric-option-return-clone-struct-app-leak.md` (the leak
  that no longer reproduces).
