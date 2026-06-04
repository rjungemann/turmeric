---
title: stdlib HKT typeclass consolidation (T1) is blocked by orphan-rule / preload-scope / dot-dispatch interactions
category: Bug Report
description: Executing docs/upcoming/stdlib-hkt-consolidation-plan.md phase T1 (add Functor/Applicative/Monad/Alternative to Option and Bifunctor/MonadError to Result) is not cleanly achievable on the current compiler. The orphan rule forces the instances into the auto-preloaded option.tur/result.tur, which (a) makes Functor[Option] collide with user-defined fmap-bearing classes under erased-receiver `.fmap` dot-dispatch, (b) requires preloading new Monad/Bifunctor class stubs that collide with the many fixtures/programs that locally redefine those classes, and (c) churns all 121 codegen snapshots via global gensym renumbering. Separately, Result's [A=ok B=err] parameter order makes the conventional kind-(*->*) instances (Functor/Monad/MonadError) inexpressible, and the plan's downstream-consumer premise does not match the tree. No code shipped; this records the blockers so the prerequisite compiler work can be scoped.
---

# stdlib HKT consolidation T1 is blocked on the current compiler

## Summary

Severity: **plan-blocker / expressiveness hole** (no miscompile -- the
attempted instances either break existing valid programs or cannot be
written at all). No code was shipped beyond this report.

`docs/upcoming/stdlib-hkt-consolidation-plan.md` phase T1 asks for:

- `Functor` / `Applicative` / `Monad` / `Alternative` for `Option`,
- `Bifunctor` / `MonadError` for `Result`,
- one downstream consumer migrated as proof.

I implemented all of these, got them building and passing a dedicated
runtime fixture (the instances themselves are *correct* -- `fmap`/`bind`/
`ap`/`alt-or`/`bimap` all produced the right values), then discovered the
approach breaks the existing test corpus and the wider language in ways
that cannot be patched without compiler changes. Details below, in order
of how fundamental they are.

## Blocker 1 -- the only non-orphan home for the instances is auto-preloaded

The orphan-instance rule (`TUR-E0013`) requires an instance to live in the
module that defines the typeclass **or** a module that owns one of the type
arguments. For `Functor [Option]` the candidates are:

- `stdlib/typeclass-functor.tur` (the class's canonical home) -- but it is
  preloaded *before* `option.tur`, so the `Option` type is not yet in scope
  there; and
- `stdlib/option.tur` (the type's home) -- which **is** auto-preloaded.

Putting the instance in `typeclass.tur` fails with `TUR-E0013` even though
`typeclass.tur` contains a `defclass Functor`: the orphan check resolves the
class's home to the preloaded stub, not to `typeclass.tur`.

So the instances can only live in `option.tur` / `result.tur`, which every
program loads. That single fact drives blockers 2-4.

## Blocker 2 -- Functor[Option] breaks erased-receiver `.fmap` dot-dispatch

With `Functor [Option]` preloaded, three existing fixtures stop compiling:

```
tests/fixtures/hkt-closures/input.tur:61:20: error: ambiguous method
  dispatch: '.fmap' matches 2 instances (TestFunctor[option], Functor[Option])
  -- receiver type is erased (int64_t). Hint: annotate the receiver's type
  or use @TypeName syntax (see D1).
```

(also `hkt-multi-capture-hkt`, `hkt-single-capture-hkt-regression`.)

These fixtures define their own `TestFunctor` with an `fmap` method and call
it through `.fmap` dot-syntax on a value whose static type has been erased to
`int64_t`. Adding a *second* `fmap`-bearing instance for an option-shaped
type makes the dot-dispatch ambiguous. This is a **language-level
regression**: any real program that defines its own `fmap`-bearing class and
uses `.fmap` on an erased receiver would break the moment stdlib ships a
`Functor [Option]`. Rewriting the fixtures to annotate receivers would be
papering over a regression I introduced, which the project's "never rewrite a
test to dodge real breakage" rule forbids.

## Blocker 3 -- preloading Monad/Bifunctor stubs collides with local redefinitions

`Applicative`, `Alternative`, `Monad`, `Bifunctor` are **not** auto-preloaded
today (only `Functor`/`Eq`/`Clone`/`Hash` stubs are). To declare
`Monad [Option]` / `Bifunctor [Result]` in the preloaded type modules, their
classes must be preloaded too. Adding `typeclass-monad.tur` /
`typeclass-bifunctor.tur` to the preload list breaks fixtures that locally
declare these classes:

```
tests/fixtures/hkt-instances/input.tur:15:1: error: typeclass 'Monad' is
  already defined
```

Many `hkt-*` fixtures (e.g. `hkt-stdlib-suite`) `(defclass Monad ...)` /
`(defclass Bifunctor ...)` with their own signatures. `defclass`
redefinition is only tolerated when the signature is *identical*; the
stdlib `Monad` uses `(bind [ma [fn :fn]])` while the fixtures use
`(bind [ma fn])`, so they collide. These classes were evidently left out of
the preload set on purpose precisely so test/user code can redefine them.

## Blocker 4 -- 121-snapshot gensym churn

Adding any form to an auto-preloaded file shifts the global gensym counter,
renumbering `__fn_NNN` across every codegen snapshot. After the T1 edits
**all 121** `tests/fixtures/*/expected.c` snapshots changed (the new
`__inst_*_Option` / `__inst_Bifunctor_bimap_Result` defns plus pure
renumbering). The plan's Risks section anticipates snapshot churn, but the
blast radius here is the entire snapshot corpus for a small feature, and it
is moot anyway while blockers 2-3 keep the suite red.

## Blocker 5 -- Result's parameter order blocks Functor/Monad/MonadError

Even setting the preload problems aside, the conventional kind-`(* -> *)`
instances cannot be written for `Result`:

```turmeric
(defstruct Result [A B] (is-ok :bool) (ok-val A) (err-val B))  ; A=ok, B=err
```

Right-biased `fmap`/`bind`/`throwError`/`catchError` must vary the **ok**
arm and fix the **err** arm. But partially-applied instance heads fix the
**leftmost** parameter -- the documented precedent is `Either`:

```turmeric
;; stdlib/either.tur  (L=err first, R=ok second)
;; "the partially-applied head `(Either E)` fixes the Left type parameter,
;;  leaving the kind-(* -> *) functor over the Right arm."
(definstance Functor [(Either E)] (fmap [container fn] ...))  ; maps Right=ok
```

`Either` works because err is first. `Result` has ok first, so the
analogous `(Result A)` fixes ok and varies err -- a left-biased functor,
the inverse of the convention. There is no syntax to "fix the trailing
parameter," so `Functor [Result]`, `Monad [Result]`, and `MonadError
[Result]` are inexpressible. Only `Bifunctor [Result]` (kind
`(* -> * -> *)`, both arms named explicitly) is expressible.

## Blocker 6 -- the plan's downstream premise does not match the tree

The plan says `httpd.tur` / `csv.tur` / `json.tur` open-code `result-map` /
`result-and-then` chains to be replaced by the new instances. In the current
tree `csv.tur` and `json.tur` do not use `Result` at all (they return
`0`/`-1` sentinels), `httpd.tur` has no `Result` chains, and `result-map`
has **zero** call sites anywhere in stdlib. The "migrate one consumer as the
proof" acceptance item therefore has no target. `MonadError` likewise has no
existing hand-rolled analogue to "consolidate."

## What was verified to work (before reverting)

The instance *implementations* are correct; the blockers are all about
*where they can live*. With the instances preloaded, a dedicated program
produced:

```
fmap (some 21) *2        => 42
bind (some 20) (+1)      => 21
alt-or none (some 7)     => 7
pure 99                  => 99
ap (some (+bump)) (some 41) => 42   ; capturing closure, fat-box slot-0 dispatch
bimap (ok 5) neg inc     => ok 6    ; fn-right over ok arm
bimap (err 5) neg inc    => err -5  ; fn-left over err arm
```

## Proposed fix directions (prerequisite compiler work)

Pick one before re-attempting T1:

1. **Disambiguate erased-receiver dot-dispatch** so a stdlib
   `Functor [Option]` cannot collide with a user `TestFunctor[option]`
   (e.g. prefer the statically-known instance, or fall back to a designated
   default) -- removes blocker 2.
2. **Scoped / opt-in instances**: allow instances that are only active when a
   module is imported, so stdlib HKT instances live in an on-demand file
   (no preload, no churn, no global dispatch pollution) -- removes blockers
   1-4. This is the cleanest fit and matches how `schema.tur` already loads
   `typeclass-applicative.tur` on demand.
3. **Trailing-parameter instance heads** (e.g. `(Result _ B)` or a kind-level
   flip) -- removes blocker 5, lets `Functor`/`Monad`/`MonadError [Result]`
   be written without flipping the type.
4. **Flip `Result` to `[B=err A=ok]`** to mirror `Either` -- also removes
   blocker 5, but is a breaking change to a core auto-preloaded type and
   churns every `ok-val`/`err-val` site; almost certainly not worth it.

## Validation of a fix

Re-run T1 with the chosen mechanism and confirm:

- `Functor`/`Applicative`/`Monad`/`Alternative [Option]` and
  `Bifunctor [Result]` produce the values listed above;
- `bash tests/run.sh` is green with zero `FAIL` lines (and snapshots
  regenerated + committed if the instances end up preloaded);
- no existing `.fmap` / `defclass Monad` fixture regresses.

## Note on T2 / T3

`parsec` / `logic` / `backtrack` (plan phases T2/T3) need real nominal types:
`Parser` / `Goal` / `Backtrack` exist only as `ptr<void>` / list aliases in
comments, so `definstance Monad Parser` has nothing to dispatch on. Real
instances there require newtype wrappers threaded through each module (parsec
alone is ~764 lines) plus the same dispatch/preload concerns as above. They
also inherit blocker 3 (the modules already define `mzero`/`mbind`-style
helpers and would want the real classes preloaded). Out of scope until the
prerequisite compiler work lands.
