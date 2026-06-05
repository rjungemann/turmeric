---
title: Result's [A=ok B=err] parameter order blocks Functor/Monad/MonadError instances
category: Bug Report
description: Result is declared (defstruct Result [A B]) with the ok type FIRST and the err type SECOND. The conventional right-biased kind-(* -> *) instances (Functor, Monad, MonadError) must vary the ok type while holding err fixed, but a partially-applied instance head fixes the LEFTMOST parameter (as `(Either E)` does). On Result that fixes ok and varies err -- the opposite of the convention -- so those instances are inexpressible. Surfaced executing docs/upcoming/stdlib-hkt-consolidation-plan.md (T1); only Bifunctor[Result] could be shipped, MonadError[Result] was deferred. Status: OPEN.
---

# Result's `[A=ok B=err]` parameter order blocks Functor/Monad/MonadError

> **Status:** OPEN. T1 of the stdlib HKT consolidation otherwise landed
> (Option got Functor/Applicative/Monad/Alternative; Result got Bifunctor).
> This records the one instance family that remains inexpressible.

## Summary

Severity: **expressiveness hole** (no miscompile; the instances cannot be
written), and a **plan-blocker** for the `MonadError [Result]` deliverable.

`Result` is declared with the ok type as the *first* parameter:

```turmeric
(defstruct Result [A B] (is-ok :bool) (ok-val A) (err-val B))   ; A = ok, B = err
```

The conventional, right-biased instances operate on the **ok** arm and hold
the **err** arm fixed:

```
fmap        :: (a -> c) -> Result a b -> Result c b      -- vary ok, fix err
bind        :: Result a b -> (a -> Result c b) -> Result c b
throwError  :: b -> Result a b                           -- MonadError, err fixed
catchError  :: Result a b -> (b -> Result a b) -> Result a b
```

Expressing any of these as a kind-`(* -> *)` instance needs an instance head
that **fixes err and leaves ok free**. But Turmeric's partially-applied
instance heads fix the **leftmost** parameter. The documented precedent is
`Either`:

```turmeric
;; stdlib/either.tur  (L = err/Left first, R = ok/Right second)
;; "the partially-applied head `(Either E)` fixes the Left type parameter,
;;  leaving the kind-(* -> *) functor over the Right arm."
(definstance Functor [(Either E)] (fmap [container fn] ...))   ; maps Right=ok
```

`Either` works because its err arm is first. `Result` has ok first, so the
analogous `(Result A)` fixes **ok** and leaves **err** free -- a left-biased
functor, the inverse of the convention. There is no syntax to "fix the
trailing parameter," so `Functor [Result]`, `Monad [Result]`, and
`MonadError [Result]` are all inexpressible.

`Bifunctor [Result]` is unaffected -- it is a kind-`(* -> * -> *)` instance
naming both arms explicitly (`bimap [container fn-left fn-right]`), so no
partial application is needed. It shipped in T1 (see `stdlib/result.tur`).

## Observed vs expected

```turmeric
;; intent: map over the ok arm, leave err untouched
(definstance Functor [(Result A)]      ; A is positionally the FIRST param = ok
  (fmap [container fn] ...))
```

- **Observed:** `(Result A)` fixes the ok arm and leaves err as the functor
  variable, so `fmap` would transform the *error* payload -- inverse of the
  intended semantics.
- **Expected:** an instance whose `fmap`/`bind` transforms the *ok* arm.

## Root cause

`Result`'s parameter order `[A=ok B=err]` is the mirror image of `Either`'s
`[L=err R=ok]`. Combined with leftmost-only partial application in instance
heads (the mechanism that powers `(Either E)`), the ok-biased `(* -> *)`
projection is unrepresentable.

## Proposed fix directions

1. **Add instance-head support for fixing a trailing parameter** (e.g.
   `(Result _ B)` or a kind-level flip), so the ok-biased instances become
   expressible without touching `Result`. Most general.
2. **Flip `Result` to `[B=err A=ok]`** to mirror `Either`. Breaking
   layout/source change to a core auto-preloaded type; churns every
   `ok-val`/`err-val`/`Result` site -- almost certainly not worth it.
3. **Ship `Bifunctor [Result]` only** (done in T1) plus `result-map` /
   `result-bimap` workers, and document the omission (this report).

## Validation of a fix

Once a fix lands, the following should type-check and run with ok-biased
semantics:

```turmeric
(opt-val (fmap (:: (ok 21) (Result int int)) (fn [x] (* x 2))))  ; => ok(42)
(res-err (fmap (:: (err 5) (Result int int)) (fn [x] (* x 2))))  ; => err(5) unchanged
```

## Resolved during T1 (context)

Two infrastructure blockers found while executing T1 were *resolved* and are
noted here so this report is self-contained:

- **Ambient dot-dispatch collisions.** Adding stdlib `Functor`/`Monad` etc.
  for `Option` made `.fmap`/`.bind` (including the `.bind`/`.pure` emitted by
  the `do-m`/`for` macros in `stdlib/macros.tur`) ambiguous on erased
  `int64_t` receivers wherever a program also defines its own same-shaped
  instance. Fixed in `src/compiler/elab_typeclasses.c`: when an erased-receiver
  dispatch is otherwise ambiguous (`TUR_E0020`) and exactly one candidate is a
  **user** (non-`stdlib/`) instance, that local instance shadows the stdlib
  one. The change is purely additive -- it only affects cases that previously
  errored -- so no resolving dispatch regressed.
- **Class-stub preloading.** `Monad`/`Bifunctor` had no auto-loaded class
  stub, so instances in the preloaded `option.tur`/`result.tur` could not see
  them. Added `stdlib/typeclass-monad.tur` + `stdlib/typeclass-bifunctor.tur`
  and preloaded them (plus the existing Applicative/Alternative stubs) before
  the typed-collection modules. HKT test fixtures that locally redefined these
  classes were renamed to `Test*` to avoid the now-global names (extending the
  existing `TestFunctor` convention).

## Note on the plan's downstream premise

The plan claimed `httpd`/`csv`/`json` open-code `result-map`/`result-and-then`
chains for the new instances to replace. In the current tree `csv.tur` and
`json.tur` do not use `Result` at all, `httpd.tur` has no `Result` chains, and
`result-map` has zero call sites. There was no downstream consumer to migrate;
T1's instances are validated by
`tests/fixtures/hkt-stdlib-option-result-instances/` instead.
