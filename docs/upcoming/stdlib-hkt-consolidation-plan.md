---
title: Stdlib HKT Typeclass Consolidation
category: Planning
description: Delete hand-rolled monad interfaces in parsec/logic/backtrack by adding real `Monad` and `Alternative` instances. Add missing `Bifunctor` / `MonadError` instances on `Result` and `Option` so downstream consumers (httpd, csv, json) stop open-coding `result-map` / `result-and-then` chains. Pure consolidation; no new typeclass hierarchies.
---

# Stdlib HKT Typeclass Consolidation -- Plan

> **Type:** stdlib API hardening -- typeclass consolidation
> **Prerequisites:** HKT phases S1-S8 are complete (see
> `project_hkt_phase.md`).

## Motivation

HKT phases S1-S8 are complete. Three stdlib modules still ship
hand-rolled monad interfaces:

- `stdlib/parsec.tur` -- `pmzero`, `preturn`, `pmplus`, `pmbind`
- `stdlib/logic.tur` -- `mzero`, `mreturn`, `mplus`, `mbind`
- `stdlib/backtrack.tur` -- same shape, third copy

Separately, `result.tur` and `option.tur` are missing `Bifunctor` /
`MonadError` instances; downstream (`httpd.tur`, `csv.tur`, `json.tur`)
re-implements `result-map` / `result-and-then` chains by hand.

This is pure consolidation: every instance has a hand-written analogue
already in tree.

## Design

```turmeric
(definstance Monad       Parser    ...)
(definstance Alternative Parser    ...)
(definstance Monad       Logic     ...)
(definstance Alternative Logic     ...)
(definstance Monad       Backtrack ...)
(definstance Alternative Backtrack ...)

(definstance Bifunctor   Result    ...)
(definstance MonadError  Result    ...)
```

After the instances land, `parsec` / `logic` / `backtrack` users get
`for` / `do-m` from `stdlib/macros.tur` for free, and the bespoke
combinators (`pmzero` etc.) can be deleted. Downstream `result-map`
open-coding in `httpd` / `csv` / `json` becomes `fmap` / `bimap` /
`>>=` against the new instances.

## Phasing

1. **T1** -- `Bifunctor` / `MonadError` for `Result`; convert one
   downstream consumer (`csv.tur`) as the proof. Smallest test surface
   of the three; ship first.
2. **T2** -- `Monad` / `Alternative` for `Parser`; delete bespoke
   combinators. Migrate the parser tutorial.
3. **T3** -- `Logic`, `Backtrack`. Largest test surface; ship last.

## Out of scope

- **Refactoring the underlying parser / logic engine internals.**
- **Adding new typeclass hierarchies** (Comonad-Free, Profunctor,
  etc.); this is purely consolidation of what stdlib already inlines.

## Risks

- **Snapshot churn** on every fixture that uses parsec / logic /
  backtrack; the bespoke combinators get replaced with `>>=` / `fmap`
  call sites which have different mangled names in `expected.c`.
  Follow the snapshot regeneration recipe in `CLAUDE.md`.
- **Downstream coupling.** Each of `httpd` / `csv` / `json` open-codes
  the same pattern slightly differently. T1 should land the instance
  *and* one consumer migration so the pattern is established; T1.x
  follow-ups handle the others.

## Acceptance

- `Bifunctor` / `MonadError` instances exist for `Result` and (where
  applicable) `Option`.
- `Monad` / `Alternative` instances exist for `Parser`, `Logic`,
  `Backtrack`.
- Bespoke `pmzero` / `mreturn` / etc. exports are removed (or kept as
  thin one-line aliases marked deprecated, depending on downstream
  callers).
- At least one downstream module per typeclass is migrated to use
  `for` / `do-m` / `>>=` against the new instance.
- `bash tests/run.sh` passes with zero `FAIL` lines.
- `tur run docs` regenerated.

## Cross-references

- Builds on the completed HKT work (`project_hkt_phase.md`).
- Independent of opaque-handle / linearity / session / effects /
  refinement work; can land in any order relative to them.
- Split out from the original umbrella `stdlib-advanced-typing-plan`.
