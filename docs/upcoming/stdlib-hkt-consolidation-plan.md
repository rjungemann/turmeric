---
title: Stdlib HKT Typeclass Consolidation
category: Planning
description: Delete hand-rolled monad interfaces in parsec/logic/backtrack by adding real `Monad` and `Alternative` instances. Add missing `Bifunctor` / `MonadError` instances on `Result` and `Option` so downstream consumers (httpd, csv, json) stop open-coding `result-map` / `result-and-then` chains. Pure consolidation; no new typeclass hierarchies.
---

# Stdlib HKT Typeclass Consolidation -- Plan

> **Type:** stdlib API hardening -- typeclass consolidation
> **Prerequisites:** HKT phases S1-S8 are complete (see
> `project_hkt_phase.md`).
>
> **Status:** T1, T2, T3 landed.
> - **T1** -- `Option` got Functor/Applicative/Monad/Alternative; `Result` got
>   `Bifunctor`. `MonadError [Result]` remains blocked by Result's `[A=ok B=err]`
>   parameter order (see
>   `docs/reported/result-param-order-blocks-functor-monad.md`).
> - **T2** -- `Parser` is now a kind-`(* -> *)` opaque carrying
>   Functor/Applicative/Monad/Alternative; validated by
>   `tests/fixtures/hkt-stdlib-parser-instances/`.
> - **T3** -- `Backtrack` and logic `Goal` are kind-`(* -> *)` opaques carrying
>   the same four instances; validated by
>   `tests/fixtures/hkt-stdlib-backtrack-instances/` and
>   `tests/fixtures/hkt-stdlib-logic-instances/`.
>
> The enabling pattern for all three: each instance method delegates to the
> module's existing int-carrier worker, boxing the typeclass-method's
> `tur_poly_fn_t` continuation into the fat-closure protocol via
> `__tur_poly_to_fat1`. See the per-module "HKT typeclass instances" sections.

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

1. **T1** (DONE) -- `Bifunctor` for `Result`; `Functor`/`Applicative`/`Monad`/
   `Alternative` for `Option`. `MonadError [Result]` deferred (param-order
   blocker). The plan's downstream-consumer premise did not hold (`csv`/`json`
   do not use `Result`; `result-map` has no call sites), so the instances are
   validated by `tests/fixtures/hkt-stdlib-option-result-instances/` instead.
2. **T2** (DONE) -- `Functor`/`Applicative`/`Monad`/`Alternative` for `Parser`.
   The bespoke `bind-parser` / `or-parser` combinators are kept as the internal
   int-carrier workers the instances delegate to; the public surface is the
   typeclass methods (`bind` / `alt-or` / `fmap` / `ap`) plus typed `(Parser A)`
   constructors, so parser code uses `do-m` / `for`. Validated by
   `tests/fixtures/hkt-stdlib-parser-instances/`. (The self-contained
   `parsec-tutorial` fixture builds its own from-scratch library and is left as
   a pedagogical example; it does not import `stdlib/parsec`.)
3. **T3** (DONE) -- `Backtrack` (list/non-determinism monad) and logic `Goal`
   (`UState -> [UState]`) each get the four instances. For `Goal`, `bind` is
   conjunction (threading the substitution, MonadState-style) and `alt-or` is
   disjunction (`disjoined`); `empty` is `fail`. Validated by
   `tests/fixtures/hkt-stdlib-backtrack-instances/` and
   `tests/fixtures/hkt-stdlib-logic-instances/`.

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

- [x] `Bifunctor` instance exists for `Result`; `Functor`/`Applicative`/`Monad`/
  `Alternative` for `Option`. `MonadError [Result]` deferred -- inexpressible
  given Result's `[A=ok B=err]` order (tracked in
  `docs/reported/result-param-order-blocks-functor-monad.md`).
- [x] `Functor`/`Applicative`/`Monad`/`Alternative` instances exist for
  `Parser`, `Backtrack`, and logic `Goal`.
- [x] The bespoke monad combinators are retained as the internal int-carrier
  workers the instances delegate to (not separate user-facing interfaces); the
  public surface is now the typeclass methods + `do-m` / `for`.
- [x] A validating fixture per typeclass exercises `do-m` / `bind` / `alt-or` /
  `fmap` against the new instances
  (`hkt-stdlib-{option-result,parser,backtrack,logic}-instances`).
- [x] `bash tests/run.sh` passes with zero `FAIL` lines.
- [x] `tur run docs` regenerated (`stdlib/docstrings.tur`,
  `web/public/doc-names.json`).

## Cross-references

- Builds on the completed HKT work (`project_hkt_phase.md`).
- Independent of opaque-handle / linearity / session / effects /
  refinement work; can land in any order relative to them.
- Split out from the original umbrella `stdlib-advanced-typing-plan`.
