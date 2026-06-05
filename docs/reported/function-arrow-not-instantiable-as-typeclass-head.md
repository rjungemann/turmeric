# Function arrow `(->)` is not instantiable as a typeclass instance head

**One-line summary:** `definstance C [(->)]` is rejected outright, and the
opaque-name workarounds (`[->]`, `[Fn]`) silently type the method parameters
as an opaque struct rather than as callable functions -- so the entire
`Arrow [(->)]` typeclass layer envisioned by
`docs/upcoming/stdlib-arrow-typeclass-reintroduction-plan.md` is currently
**inexpressible**.

**Severity:** Expressiveness hole / hard blocker for the arrow-typeclass
reintroduction plan. Not a miscompile -- the compiler correctly errors -- but
it is a latent gap that blocks a planned feature, plus a smaller secondary
defect (a misleading "OK" for opaque-name heads that cannot actually work).

## Context

`stdlib-arrow-typeclass-reintroduction-plan.md` lists three hard
prerequisite gates (sum types / `Either`, closure-returning instance-method
codegen, operator-name mangling). All three are present and verified (see the
readiness block appended to that plan). The plan then assumes the central
deliverable -- `definstance Arrow [(->)]` with methods that compose and apply
real functions -- "compiles once both gaps are closed."

It does not, for a **fourth, unlisted reason**: the typeclass machinery has no
representation of the function-arrow type constructor as an instance head whose
method parameters are typed as callable functions.

## Minimal repro

### 1. `(->)` is rejected as an instance head

```turmeric
(defclass Arrow [a]
  (arr [f] : a))

(definstance Arrow [(->)]      ; also (-> b c)
  (arr [f] f))

(defn main [] : int 0)
```

```
error: unsupported type argument in definstance
 | (definstance Arrow [(->)]
 |                     ^^^^
```

**Observed:** hard error.
**Expected (per plan):** an `Arrow` instance at the function arrow.

### 2. The opaque-name workaround compiles but is a false positive

`[->]` and `[Fn]` are *accepted* -- but only because an unrecognized name
falls through to "opaque type constructor" (`TY_STRUCT`, `def == NULL`). The
head is then an arbitrary nominal stand-in with no connection to real
function/closure values. Any method body that actually treats the instance
value as a function fails:

```turmeric
(defclass Arrow [a]
  (comp [f g] : a))

(definstance Arrow [->]
  (comp [f g] (fn [x] (g (f x)))))   ; compose f then g

(defn add1 [x : int] : int (+ x 1))
(defn dbl  [x : int] : int (* x 2))

(defn main [] : int
  (let [h (comp add1 dbl)]
    (println (h 3)))                 ; want 8
  0)
```

```
error: 'g' is not a function or continuation
 |   (comp [f g] (fn [x] (g (f x)))))
 |                       ^^^^^^^^^
```

**Observed:** the parameters `f`/`g` are typed as the opaque `->` struct, so
they are not callable; the instance cannot be written.
**Expected:** within `Arrow [(->)]`, the arrow-typed parameters should be
callable functions.

## Root cause

`src/compiler/elab_typeclasses.c`, instance-head parsing (the `F_VEC`
type-argument loop, roughly lines 1251-1380):

- A type argument is accepted only when it is a type keyword/symbol
  (`elab_typeclasses.c:1253`) or a 2-element `(ctor arg)` partial application
  (`elab_typeclasses.c:1305`). The form `(->)` is a 1-element list and
  `(-> b c)` is a 3-element list, so both fall to the `else` at
  `src/compiler/elab_typeclasses.c:1378` -> `"unsupported type argument in
  definstance"`.
- An *unknown bare symbol* (e.g. `->`, `Fn`) is silently demoted to an opaque
  `TY_STRUCT` with `def == NULL` (`src/compiler/elab_typeclasses.c:1296-1301`).
  This is the right behavior for heap-pointer containers (`option`, `vec`), but
  for the function arrow it produces a head that can never carry callable
  parameters -- there is no `TY_FN`/arrow kind threaded into the instance's
  method parameter types. Hence repro #2's "not a function" error.

There is simply no `TypeKind` path that maps an instance head to "the function
arrow," and no place that gives an Arrow method's `a b c`-typed parameter the
type of a callable closure.

## Proposed fix directions

1. **Recognize the function arrow as a first-class instance head.** Teach the
   instance-head parser to accept `(->)` (and/or a reserved tycon name) and map
   it to a dedicated arrow `TypeKind`/HKT constructor of kind `* -> * -> *`,
   distinct from the opaque-struct fallback. Method parameters whose declared
   type is the class param applied to two args (`a b c`) must then resolve to a
   callable function/closure type (the existing `^fat`/`:ptr<void>` callable
   representation used by the bare-function arrow layer), so bodies like
   `(g (f x))` type-check.

2. **At minimum, stop the false positive.** Until #1 lands, reject `[->]`/`[Fn]`
   (or any head that a class then tries to *call*) instead of silently demoting
   to an opaque struct, so the gap surfaces at the instance head rather than
   deep in a method body.

3. **Return-type dispatch.** Note that even with #1, `arr :: (b->c) -> a b c`
   dispatches on the *result* type, which is the separate return-type-dispatch
   problem; the plan should treat that as an additional gate. Methods where the
   arrow value is an *argument* (`>>>`, `first`, `app`) dispatch fine once #1
   exists.

## Validation of a fix

- Repro #1 emits C and runs; repro #2 prints `8`.
- A fixture `arrow-instance-basic/` (plan T10.1) builds an arrow network via
  `arr`/`>>>`/`first` *through dispatch* and matches the bare-function output
  (plan T10.2), with snapshot-stable `expected.c`.
- The existing bare-function fixtures `tests/fixtures/stdlib-arrow` and
  `tests/fixtures/arrow-capturing-closure` remain unchanged (plan T11).

## Disposition

`stdlib-arrow-typeclass-reintroduction-plan.md` is **blocked** on this gap and
must stay scaled back, per its own "If a prerequisite cannot be restored,
revert this plan and stay scaled back" clause. `stdlib/arrow.tur` is left
untouched (bare-function layer only). This report is the prerequisite that the
plan's Task 1 should have listed as a fourth hard gate.
