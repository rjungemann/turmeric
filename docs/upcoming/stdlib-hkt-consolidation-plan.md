---
title: Stdlib HKT Typeclass Consolidation
category: Planning
description: Delete hand-rolled monad interfaces in parsec/logic/backtrack by adding real `Monad` and `Alternative` instances. Add missing `Bifunctor` / `MonadError` instances on `Result` and `Option` so downstream consumers (httpd, csv, json) stop open-coding `result-map` / `result-and-then` chains. T4 extends instance-head syntax to fix a trailing type parameter (`(Result _ B)`) so the ok-biased `Functor`/`Monad`/`MonadError [Result]` instances become expressible without breaking `Result`'s `[A=ok B=err]` layout.
---

# Stdlib HKT Typeclass Consolidation -- Plan

> **Type:** stdlib API hardening -- typeclass consolidation
> **Prerequisites:** HKT phases S1-S8 are complete (see
> `project_hkt_phase.md`).
>
> **Status:** T1, T2, T3 landed; **T4 landed** (except the deferred
> `Applicative [Result]` -- see below), unblocking the ok-biased `Result`
> instances that T1 could not ship.
> - **T1** -- `Option` got Functor/Applicative/Monad/Alternative; `Result` got
>   `Bifunctor`. `MonadError [Result]` (and the ok-biased
>   `Functor`/`Monad [Result]`) remain blocked by Result's `[A=ok B=err]`
>   parameter order combined with leftmost-only partial application in instance
>   heads. See `docs/reported/result-param-order-blocks-functor-monad.md`.
>   **Resolution path:** T4 below (Fix #1 from the report -- generalize the
>   instance-head mechanism, do not flip `Result`'s layout).
> - **T2** -- `Parser` is now a kind-`(* -> *)` opaque carrying
>   Functor/Applicative/Monad/Alternative; validated by
>   `tests/fixtures/hkt-stdlib-parser-instances/`.
> - **T3** -- `Backtrack` and logic `Goal` are kind-`(* -> *)` opaques carrying
>   the same four instances; validated by
>   `tests/fixtures/hkt-stdlib-backtrack-instances/` and
>   `tests/fixtures/hkt-stdlib-logic-instances/`.
> - **T4** (DONE except `Applicative`) -- extended instance-head partial
>   application so a *trailing* parameter can be fixed with a one-`_`-hole head
>   (`(Result _ B)`; the `_`-hole surface was chosen over a kind-level `Flip`),
>   and added the ok-biased `Functor`/`Monad`/`MonadError [(Result _ B)]`
>   instances. Surfaced and fixed a latent dispatch-ABI bug (partial-app-head
>   instances mis-marshalled a by-value struct receiver). `Applicative` is
>   deferred: its no-receiver `pure` is argument-dispatched and collides with
>   `Applicative [Option]` -- a cross-cutting return-vs-argument dispatch
>   limitation, independent of instance-head expressiveness. Closes
>   `docs/reported/result-param-order-blocks-functor-monad.md` (its named
>   blockers `MonadError`/ok-biased `Functor`/`Monad` are all shipped).
>   Fixtures: `hkt-stdlib-result-ok-biased`, `instance-head-hole-pair`,
>   `errors/instance-head-two-holes`.
>
> The enabling pattern for all three: each instance method delegates to the
> module's existing int-carrier worker. The worker declares its continuation
> parameter `^fat`, so handing it the typeclass-method's poly closure lets the
> compiler box it into the fat-closure protocol automatically (EX_POLY_TO_FAT /
> the N-ary `__tur_poly_to_fat<N>` carrier landed in #252). The instance bodies
> are therefore pure Turmeric -- no hand-written poly-to-fat box. See the
> per-module "HKT typeclass instances" sections.

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
4. **T4** (DONE, except `Applicative`) -- **trailing-parameter instance heads,
   then ok-biased `Result` instances.** Implements Fix #1 from
   `docs/reported/result-param-order-blocks-functor-monad.md`. Chosen over
   flipping `Result`'s layout (Fix #2 -- breaking churn to a preloaded core
   type) or shipping workers only (Fix #3 -- effectively the current state;
   leaves the hole open). Generalizes the mechanism so any future
   kind-`(* -> * -> *)` type whose "interesting" arm is not leftmost benefits
   automatically.

   **T4a -- instance-head mechanism.** Extend the type-application parser in
   `parse_instance_head` (`src/compiler/elab_typeclasses.c` near line 1304,
   where `(constructor arg)` is currently recognized as `TY_APP` with the
   *trailing* parameter free) to also accept a *hole* form that fixes a
   trailing parameter and leaves an earlier one free. Two surface candidates
   to evaluate during T4a-0:
   - `(Result _ B)` -- explicit wildcard at the free position. Reads
     directly; mirrors existing `(Result int)` partial-application syntax.
   - A kind-level `Flip` combinator -- `[(Flip Result B)]` reduces to
     "Result with arms swapped"; reuses the existing leftmost-fix path with
     no new parser surface, but introduces a new kind-level form.

   Recommended default: `(Result _ B)`. It's local to the instance-head
   parser, requires no kind-system surface, and the wildcard reads as
   "this slot stays free." Document the decision in T4a-0 before coding.

   **T4a-0 decision (DONE):** chose the **`_`-hole** surface (`(Result _ B)`)
   over a kind-level `Flip`. Rationale confirmed during implementation: the
   element arm of these instances is erased to the `int64_t` carrier (class
   stubs declare results `: int`; method bodies are carrier-level inline-C), so
   no kind-system / arm-swap machinery is needed -- fixing the *named* arm and
   carrying the constructor identity is sufficient for dispatch and the
   orphan-instance check. The change stayed local to the instance-head parser,
   so `Flip` would have added machinery for no benefit. No `docs/design/`
   sketch needed.

   Steps:
   - T4a-1 Parser: extend the `F_LIST` branch in `parse_instance_head` to
     accept `(<ctor> <args...>)` with exactly one `_` hole among the
     args; build the same `TY_APP` representation the leftmost-fix path
     produces, but with the free slot recorded at the hole's position.
     Reject multiple holes (would require multi-parameter abstraction;
     out of scope for T4).
   - T4a-2 Kind check: confirm the hole position is the one varying for
     the class's expected kind (e.g. `Functor` expects kind `* -> *`, so
     exactly one hole, in any position).
   - T4a-3 Method dispatch / orphan-instance bookkeeping: the resolver
     currently keys instances by the constructor symbol + fixed-arg
     pattern; extend the key so `(Result _ B)` and `(Result A _)` are
     distinct instances of the same class on the same constructor.
   - T4a-4 Error messages: a class that expects kind `* -> *` applied to
     a fully saturated `(Result A B)` should suggest the hole syntax.

   **T4b -- the actual instances.** Add to `stdlib/result.tur`:
   ```turmeric
   (definstance Functor    [(Result _ B)] (fmap ...))      ;; varies ok, holds err
   (definstance Applicative[(Result _ B)] (pure ...) (ap ...))
   (definstance Monad      [(Result _ B)] (bind ...))
   (definstance MonadError [(Result _ B)] (throw-error ...) (catch-error ...))
   ```
   These delegate to existing int-carrier workers exactly as T1's `Option`
   instances do; no new fat-closure plumbing required.

   **T4c -- validation.**
   - Extend `tests/fixtures/hkt-stdlib-option-result-instances/` (or a new
     `hkt-stdlib-result-ok-biased-instances/` fixture) to exercise
     `fmap` / `bind` / `do-m` / `throw-error` / `catch-error` against
     `Result`, including the report's two-line proof case:
     ```turmeric
     (opt-val (fmap (:: (ok 21) (Result int int)) (fn [x] (* x 2))))
     (res-err (fmap (:: (err 5) (Result int int)) (fn [x] (* x 2))))
     ```
   - Add a fixture that exercises the hole syntax on a non-`Result`
     constructor (e.g. a tiny `defstruct Pair [A B]`) so the mechanism is
     validated independently of `Result`.
   - Add a negative fixture: `(definstance Functor [(Result _ _)] ...)`
     must error (multiple holes) with the message produced in T4a-1.

   **T4d -- close the report.** Move
   `docs/reported/result-param-order-blocks-functor-monad.md` to
   `docs/reported/resolved/` (or whatever the project convention is at
   the time) and add a one-paragraph postscript pointing at the T4
   commit and fixture.

## Out of scope

- **Refactoring the underlying parser / logic engine internals.**
- **Adding new typeclass hierarchies** (Comonad-Free, Profunctor,
  etc.); this is purely consolidation of what stdlib already inlines.
- **Flipping `Result`'s parameter order** to `[B=err A=ok]` to mirror
  `Either` (the report's Fix #2). Rejected: `Result` is auto-preloaded
  and used pervasively; the churn to `ok-val` / `err-val` / every
  consumer would dwarf the elaborator change in T4 and provides no
  generalization beyond `Result`.
- **Multi-hole instance heads** (e.g. `(Triple _ B _)` for some
  `(* -> * -> * -> *)` constructor). T4 admits exactly one hole; richer
  abstraction would need a small kind-level lambda and is its own plan.

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
  `Alternative` for `Option`. (Ok-biased `Functor`/`Monad`/`MonadError [Result]`
  intentionally deferred to T4, which adds the trailing-parameter instance-head
  mechanism that makes them expressible without flipping `Result`'s layout --
  tracked in `docs/reported/result-param-order-blocks-functor-monad.md`.)
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
- [ ] **T4** -- Instance-head syntax admits a single hole at any position
  (`(Result _ B)`), validated by a fixture on a non-`Result` constructor and
  a negative fixture rejecting multi-hole heads.
- [ ] **T4** -- `Functor`/`Applicative`/`Monad`/`MonadError [Result]` ok-biased
  instances exist in `stdlib/result.tur` and are exercised by a fixture
  including the report's two-line proof case.
- [ ] **T4** -- The report
  `docs/reported/result-param-order-blocks-functor-monad.md` is marked
  resolved (or moved per the prevailing convention) with a pointer to the
  T4 commit and fixture.

## Cross-references

- Builds on the completed HKT work (`project_hkt_phase.md`).
- Independent of opaque-handle / linearity / session / effects /
  refinement work; can land in any order relative to them.
- Split out from the original umbrella `stdlib-advanced-typing-plan`.
