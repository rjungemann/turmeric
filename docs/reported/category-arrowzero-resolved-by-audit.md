# category-arrowzero: resolved-by-audit (no stdlib change)

**Summary:** `docs/upcoming/category-arrowzero-implementation-plan.md` T0 audit
(2026-06-05) resolved to outcome **(c)** -- tur-signal's v1 (Tier 1) surface
calls neither `Category.ident` nor `ArrowZero.zeroArrow`, so no `Category` /
`ArrowZero [(->)]` typeclass surface ships. The plan collapses to a no-op for
`stdlib/arrow.tur`; this is a decision record, not a defect.

**Severity:** none (planning decision). Filed per the plan's T0(c) instruction
to record the audit outcome.

## What was checked

- **`docs/upcoming/tur-signal-rebuild-plan.md`** Tier 1 surface table (the v1
  contract). Composition is `effects-chain` over `Vec<SF Sample Sample>`,
  implemented with the **bare `compose-float` combinator**, not the
  `Arrow`/`Category` typeclass `>>>`/`comp`. The identity-shaped Tier 1 symbols
  (`time-signal`, `invert`, ...) are concrete `:float -> :float` functions, not
  the polymorphic `ident :: arr a a`. No symbol produces or consumes a zero
  arrow.
- **`stdlib/`** has no `Default` typeclass / `default-of`, so the plan's D3a
  (`ArrowZero [(->)]` constrained on `Default b`) was unbuildable regardless of
  the audit outcome.

> The sibling `../turmeric-spices/tur-signal/` checkout is absent in-container
> (`requires.spices`), so the source-level grep could not be run directly; the
> rebuild plan's Tier 1 table -- the design of record -- was used as the
> authoritative cross-check.

## Decision

Do not ship typeclass surface tur-signal does not exercise. `Category` and a
`(->)` `ArrowZero` stay available as a cheap sequel: the unblocking mechanism
(PR #261, return-type dispatch for nullary arrow methods) is in the tree and
proven by `tests/fixtures/arrow-instance-nullary`. Reopen the plan if a real
consumer (Kleisli/SF arrow, or a tur-signal Tier 2 combinator) calls
`ident`/`zeroArrow` through dispatch.

See the appended **Audit (T0)** section in
`docs/upcoming/category-arrowzero-implementation-plan.md` for the full findings.
