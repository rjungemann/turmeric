# GADT type indices over constructor applications are phantom -- no compile-time length proofs

**Severity: low** (expressiveness; documented aspiration) -- "length-indexed
vector" recipes cannot deliver their headline guarantees. Found in the
2026-08-20 docs audit.

## Repro

stdlib/gadt-vec.tur's own docstring: "True compile-time length safety is
deferred to a future dependent-types phase". A `head` without a `VNil` arm is
a non-exhaustive-match error even on a provably non-empty `(Vec (Succ n))`,
hence `gvec-head-or` taking a default.

## Root cause

No type-level Nat evaluation/unification in the GADT index position
(elab_structs.c / elab_types.c treat the parameter as phantom).

## Fix direction

Dependent-ish index tracking for constructor-application indices, or at
minimum per-arm index refinement so `(Vec (Succ n))`-typed scrutinees drop the
`VNil` arm from the exhaustiveness set.

## Guides to update when fixed

- docs/guides/gadts-cookbook.md ("Length-Indexed Vectors" caveat)
- docs/guides/gadts-guide.md ("No dependent types" limitation)
- stdlib/gadt-vec.tur docstrings
