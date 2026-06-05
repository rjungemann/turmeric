# category-arrowzero: audit outcome (c) -- OVERRIDDEN, surface shipped

> **Status update (2026-06-05):** the resolved-by-audit recommendation below was
> **overridden by the maintainer**. `Category` and an honest `ArrowZero` (via a
> Kleisli-over-Option example) were implemented rather than deferred. See the
> **Implementation** section of
> `docs/upcoming/category-arrowzero-implementation-plan.md` for what shipped.
> This file is retained as the record of the audit reasoning that still explains
> *why the `(->)` `ArrowZero` is not honest* and why the example uses Kleisli.

**Summary (audit, as run):** `docs/upcoming/category-arrowzero-implementation-plan.md`
T0 audit (2026-06-05) found that tur-signal's v1 (Tier 1) surface calls neither
`Category.ident` nor `ArrowZero.zeroArrow` -- composition is the bare
`compose-float` combinator and the identity-shaped symbols are concrete
`:float -> :float` functions. On those grounds the audit *recommended* deferring
the typeclass surface (outcome (c)). The maintainer chose to ship it regardless.

**Severity:** none (planning/decision record).

## What was checked (still accurate)

- **`docs/upcoming/tur-signal-rebuild-plan.md`** Tier 1 surface table: no
  symbol produces or consumes `ident`/`zeroArrow` through dispatch.
- **`stdlib/`**: no `Default` typeclass / `default-of`, so the plan's D3a
  (`ArrowZero [(->)]` constrained on `Default b`) was unbuildable. The
  implementation therefore took D3b (Kleisli over Option) for an honest zero.

> The sibling `../turmeric-spices/tur-signal/` checkout is absent in-container
> (`requires.spices`), so the rebuild plan's Tier 1 table -- the design of
> record -- was used as the authoritative cross-check.

## Outcome

Overridden. `Category` + `Category [(->)]` landed in `stdlib/arrow.tur`; a worked
`Kleisli` arrow with `Category`/`ArrowZero` instances landed in
`stdlib/kleisli.tur`; fixtures `category-instance-basic` and
`kleisli-arrow-instance` cover them. The `(->)` `ArrowZero` is still not shipped
(no honest inhabitant) -- the Kleisli instance is its honest home.
