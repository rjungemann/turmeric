---
title: Result's [A=ok B=err] parameter order blocks Functor/Monad/MonadError instances
category: Bug Report
description: Result is declared (defstruct Result [A B]) with the ok type FIRST and the err type SECOND. The conventional right-biased kind-(* -> *) instances (Functor, Monad, MonadError) must vary the ok type while holding err fixed, but a partially-applied instance head fixes the LEFTMOST parameter (as `(Either E)` does). On Result that fixes ok and varies err -- the opposite of the convention -- so those instances are inexpressible. Surfaced executing docs/upcoming/stdlib-hkt-consolidation-plan.md (T1); only Bifunctor[Result] could be shipped, MonadError[Result] was deferred. Status: RESOLVED (T4 -- trailing-parameter instance head `(Result _ B)`; ok-biased Functor/Monad/MonadError shipped, Applicative deferred).
---

# Result's `[A=ok B=err]` parameter order blocks Functor/Monad/MonadError

> **Status:** RESOLVED (T4, this session) via **Fix #1**. The instance-head
> parser now accepts a one-`_`-hole partial application `(Result _ B)` that
> fixes a *trailing* parameter and leaves an earlier one free, and
> `stdlib/result.tur` carries the ok-biased `Functor`, `Monad`, and
> `MonadError [(Result _ B)]` instances. See the [Resolution](#resolution)
> section for the implementation, a latent dispatch-ABI bug the work surfaced
> and fixed, and the one deferred piece (`Applicative`, blocked by an unrelated
> `pure` dispatch limitation).
>
> Fix #2 (flip `Result`'s parameter order) and Fix #3 (workers only) were
> considered and rejected -- see the plan's "Out of scope" section.

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
   expressible without touching `Result`. Most general. **CHOSEN** -- scheduled
   as T4 in `docs/upcoming/stdlib-hkt-consolidation-plan.md`. Generalizes
   beyond `Result`: any future kind-`(* -> * -> *)` type whose "interesting"
   arm is not leftmost benefits automatically. Implementation point of entry
   is the `F_LIST` branch of `parse_instance_head`
   (`src/compiler/elab_typeclasses.c` ~line 1304), which already recognizes
   the leftmost-fix case `(constructor arg)`.
2. **Flip `Result` to `[B=err A=ok]`** to mirror `Either`. Rejected -- breaking
   layout/source change to a core auto-preloaded type; churns every
   `ok-val`/`err-val`/`Result` site, and only solves the `Result` instance of
   a general problem.
3. **Ship `Bifunctor [Result]` only** (done in T1) plus `result-map` /
   `result-bimap` workers, and document the omission (this report). Rejected
   as a final state -- this is effectively the current behavior, and leaves
   `MonadError [Result]` permanently inexpressible.

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

## Resolution

Resolved via **Fix #1** (trailing-parameter instance heads) in this session.
The element arm of these kind-`(* -> *)` instances is erased to the `int64_t`
carrier (the class stubs declare method results as `: int`, and the method
bodies are carrier-level inline-C), so the fix did **not** require a new
type-system notion of "which arm varies." What was actually needed was (a) a
surface to spell "fix the err arm, vary ok," and (b) a dispatch path that
threads a concrete by-value `Result` receiver as the `int64_t` carrier.

### 1. Trailing-parameter instance-head hole `(Result _ B)`

`src/compiler/elab_typeclasses.c` -- the `F_LIST` partial-application branch
in the instance-head parser (the same one that recognizes the leftmost-fix
`(Either E)` / `(result int)` forms) now also accepts the 3-element hole form
`(Ctor _ B)` / `(Ctor A _)`. Exactly one `_` marks the free parameter; the
other names the fixed arm. The fixed arm becomes the `TY_APP` argument, which
is exactly what the existing machinery needs: a kind-`(* -> *)` head carrying
the constructor's identity (for dispatch + the orphan-instance check). Two
holes (`(Result _ _)`) is a hard error -- it would require multi-parameter
abstraction, out of scope here.

### 2. Latent dispatch-ABI bug (fixed alongside)

A *partially-applied* instance head whose receiver is a concrete by-value
struct (an ascribed `(Result int int)`) mis-marshalled the receiver: the
dispatch site materialized a by-value `Result__int__int` temp and passed its
address into the method's `int64_t` carrier signature, emitting invalid C
(`error: invalid initializer` / `aggregate value used where an integer was
expected`). The bare-head case (`Bifunctor [Result]`) was unaffected because
its receiver param was already forced to the `int64_t` carrier; the partial-app
`TY_APP` path missed that coercion. Fixed in the method-param substitution
(`elab_typeclasses.c`): a `TY_APP` receiver type now lowers to the `int64_t`
carrier just like a parameterized-`TY_STRUCT` receiver. This bug affected
**any** partial-application-head instance on a by-value struct receiver
(including the documented `Functor [(Either E)]` shape had it been used on a
by-value struct), not just the new `(Result _ B)` syntax.

### 3. The instances

`stdlib/result.tur` now carries `Functor`, `Monad`, and
`MonadError [(Result _ B)]` (right-biased: map/sequence the ok arm, thread err
through unchanged). A new `MonadError` class stub
(`stdlib/typeclass-monaderror.tur`, `throw-error` / `catch-error`) is preloaded
alongside the other HKT stubs in both the AOT and eval-worker preload lists
(`src/main.c`).

### Validation

- `tests/fixtures/hkt-stdlib-result-ok-biased/` -- exercises `fmap` / `bind` /
  `throw-error` / `catch-error`, including this report's two-line proof case
  (`fmap` over `(ok 21)` => 42, over `(err 5)` => 5 unchanged).
- `tests/fixtures/instance-head-hole-pair/` -- validates the hole mechanism on
  a non-`Result` user type (a binary `Box` struct), independent of stdlib.
- `tests/fixtures/errors/instance-head-two-holes/` -- a `(Box _ _)` two-hole
  head must error.
- Full suite green (`bash tests/run.sh`: 1478 passed, 0 failed), interpreter
  (`run-turi.sh`) and flags (`run-flags.sh`) harnesses green, leak-clean under
  ASan/LSan.

### Deferred: `Applicative [(Result _ B)]`

`Applicative`'s `pure` takes no receiver of the instance type, and the class
stub declares its result as `: int` (erased), so `pure` is **argument**-
dispatched rather than return-dispatched. With both `Applicative [Option]`
(shipped in T1) and `Applicative [Result]` in scope, `(pure x)` becomes
ambiguous and the enclosing ascription cannot disambiguate an argument-
dispatched call. This is the cross-cutting "ambient dot-dispatch collision"
limitation noted in the T1 context above -- it equally affects `Option`,
`Parser`, `Backtrack`, and `Goal` (`empty` has the same shape) -- and is
independent of the instance-head expressiveness this report is about. The
report's named blockers (`MonadError [Result]` and the ok-biased
`Functor`/`Monad [Result]`) are all delivered; `Applicative` is left as a
follow-up for whoever addresses return-vs-argument dispatch disambiguation for
no-receiver typeclass methods.
