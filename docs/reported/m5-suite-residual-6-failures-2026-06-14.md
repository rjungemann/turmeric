# M5 suite residual: 6 failing fixtures, 3 root causes

**Status:** Root cause A FIXED 2026-06-14 (suite now 4 failed); B and C
remain (deferred to the M5 effort).
**Severity:** mixed -- two are name-capture regressions introduced by the
M5 `Eq Cons` rewrite (real latent bugs that break ordinary user programs),
one is the known M5 carrier/concrete straddle.
**Discovered:** 2026-06-14, running the full suite after an unrelated change
(`bash tests/run.sh` -> `summary: 1620 passed, 6 failed`).

The 6 `FAIL` lines are present on `main` (39e9316) independent of any
working-tree change; verified by stashing local edits and rebuilding. They
group into three distinct root causes.

```
FAIL hkt-stdlib-option-result-instances        -- C: M5 carrier straddle
FAIL positional-opaque-ok                       -- A: tyvar `A` captured by global type
FAIL positional-pap-opaque-ok                   -- A: tyvar `A` captured by global type
FAIL rt-return-dispatch-basic                   -- B: method `default-of` shadowed by builtin
FAIL errors/rt-return-dispatch-unascribed       -- B: method `default-of` shadowed by builtin
FAIL errors/rt-missing-instance                 -- B: method `default-of` shadowed by builtin
```

---

## Root cause A -- a global type named `A` captures the tyvar `A` in `Eq Cons`

**Status: FIXED 2026-06-14.** An in-scope instance/defn type variable now
shadows a same-named global type at ascription sites. Two changes:

- `elab_typeclasses.c` (`elab_definstance`): collect the constraint-vector
  tyvars (e.g. `A` in `[(Eq A)]`) and push them onto `e->sig_tyvars` for the
  duration of pass-2 method-body elaboration (saved/restored).
- `elab_types.c` (`elab_ascribe`): thread `e->sig_tyvars` into
  `type_expr_from_form` as `type_params`, which the resolver checks before the
  global type table -- so `(:: t1 (Cons A))` resolves `A` to the tyvar.

Pinned by `positional-opaque-ok` / `positional-pap-opaque-ok` (now PASS). Full
suite: `1620->1622 passed`, no regressions.

**Severity:** real latent bug. Any user program that defines a top-level type
(`defopaque`/`defstruct`) named `A` -- a *very* common type-variable name --
now fails to compile stdlib, with an error pointing into `stdlib/list.tur`
rather than the user's code.

### Observed

```
stdlib/list.tur:175:13: error []: ambiguous method dispatch: '.eq?' matches
18 instances (... Eq[?] x12) -- receiver type is erased (int64_t).
```

Both fixtures are trivial:

```turmeric
(defopaque A :int)
(defn take-a [x : A] : int ```c return (int64_t)x; ```)
(defn mk-a   []     : A   ```c return 0; ```)
(defn main   []     : int (take-a (mk-a)))
```

### Minimal repro / bisection

- Rename the opaque from `A` to `Handle` (or anything but `A`) -> builds clean.
- An opaque named `B` -> builds clean. Only `A` triggers it.

### Root cause

M5 commit `004aed5` rewrote the `Eq Cons` instance from a one-line delegation

```turmeric
(definstance Eq [Cons] [(Eq A)] (eq? [x y] (list-eq? x y (fn [a b] (= a b)))))
```

into an explicit recursive body whose tail step ascribes the receiver:

```turmeric
(eq? (:: t1 (Cons A)) (:: t2 (Cons A)))   ; stdlib/list.tur:175
```

The `A` here is the instance's type parameter, and it should shadow any
global type of the same name. It does not: the name `A` in the ascription
resolves to the user's global `(defopaque A :int)`, so `(Cons A)` becomes
`(Cons <opaque A>)`, the `(Eq A)` constraint stops behaving as a dictionary
tyvar, the receiver erases to `int64_t`, and `.eq?` dispatch goes ambiguous.

This is a scoping/name-resolution precedence bug: **global type names win
over an in-scope instance/function type variable of the same name** at an
ascription site. Before `004aed5` the `Eq Cons` body had no bare-`A`
ascription, so the latent capture never fired.

### Proposed fix directions

1. Correct fix: make ascription type-name resolution consult the enclosing
   definition's bound type variables *before* the global type table, so an
   instance/`defn` tyvar `A` shadows a global type `A`. Touches the name
   resolver -- validate against the full suite plus a new fixture that
   defines `defopaque A` and exercises `Eq Cons`/`Eq Vec`.
2. Narrow stopgap: gensym/alpha-rename the tyvar in the rewritten `Eq Cons`
   (and any sibling `Eq Vec`/`Eq Tuple2`) body so it cannot collide with a
   user global. Cheap, unblocks the two fixtures, but leaves the underlying
   capture bug for any other stdlib body that ascribes a bare tyvar.

## Root cause B -- user class method `default-of` is shadowed by the builtin

**Fixtures:** `rt-return-dispatch-basic`, `errors/rt-return-dispatch-unascribed`,
`errors/rt-missing-instance`.
**Severity:** reserved-name collision / expressiveness hole. A user cannot
define a typeclass method named `default-of`; the call is intercepted by the
compiler builtin used in `make-struct` phantom-field lowering
(`(default-of B)` etc.).

### Observed

```
error: default-of: takes exactly one type argument: (default-of T)
```

emitted at the *call site* of the user method, before return-directed
dispatch runs. The two `errors/` fixtures consequently get the wrong
diagnostic:

| fixture | expected.diag | actual |
|---|---|---|
| rt-return-dispatch-unascribed | `cannot infer type for return-directed method 'default-of'` | `default-of: takes exactly one type argument` |
| rt-missing-instance | `no instance 'Default cstr'` | `default-of: takes exactly one type argument` |

### Minimal repro / bisection

The fixtures define their own method `default-of`:

```turmeric
(defclass Default [a] (default-of [] : a))
(definstance Default [int] (default-of [] 42))
(let [i (:: (default-of) int)] ...)
```

Rename the method to `zero-of` everywhere -> all three pass (return-directed
dispatch itself is healthy). Only the name `default-of` triggers it.

### Root cause

`default-of` became a special builtin form (one mandatory *type* argument:
`(default-of T)`) consumed by the M5 `make-struct` phantom-typeparam
lowering. The parser/elaborator resolves the bare `(default-of)` call to that
builtin before it ever considers the user's class method of the same name, so
the arity check fires first.

These fixtures predate the builtin and deliberately used `default-of` as the
canonical "return-position-only dispatch" example; the builtin landed under
the same name and shadowed them.

### Proposed fix directions

1. Let a user-declared `defclass` method shadow the builtin within its scope
   (resolve class methods before builtin forms when a matching class is in
   scope), so `default-of` can be both. This is the principled fix and keeps
   the fixtures' intent.
2. If `default-of` is to be reserved, rename it to a non-colliding internal
   form (e.g. `__default-of`) so user code can use the plain name, and
   re-point the three fixtures. (Renaming the fixtures alone would dodge a
   real reserved-name bug -- do not do that without also freeing the name.)

## Root cause C -- M5 carrier/concrete straddle in Option HKT instances

**Fixture:** `hkt-stdlib-option-result-instances`.
**Severity:** the genuine M5 monomorphization residual (already tracked
conceptually in `docs/upcoming/m5-residual-straddle-retirement.md`), surfacing
here as a hard `cc` error rather than a miscompile.

### Observed

```
__fn_895:  return some((x) + (INT64_C(1)));
  error: incompatible types when returning 'int64_t' but 'Option__int' was expected
__poly_897: return __fn_895(__poly_x0_900);
  error: incompatible types when returning 'Option__int' but 'int64_t' was expected
```

The `(fn [x] (some (+ x 1)))` lambda passed to `bind` is monomorphized to
return `Option__int`, but `some`'s specialized signature returns the int64
carrier; the surrounding poly wrapper expects the inverse. Producer-side and
consumer-side of the Option Monad/Functor instance disagree on whether the
arm is carrier or by-value -- the CK_CONCRETE <-> CK_CARRIER straddle the M5
plan is in the middle of retiring.

### Proposed fix direction

Falls under the existing residual-straddle retirement; not a new bug. Track
there. Listing it here only so the suite's red state is fully accounted for.

## Validation

- A/B are bisected to a single colliding name each (`A`, `default-of`);
  renaming the colliding identifier makes each fixture pass, proving the
  dispatch/return-dispatch machinery is otherwise correct.
- A is bisected to commit `004aed5` (the `Eq Cons` rewrite).
- A fix for A should add a fixture that defines `(defopaque A :int)` *and*
  uses `(= some-cons-list other-cons-list)` to lock the shadowing behavior.
- A fix for B should keep the three existing fixtures' method name as
  `default-of` (that is the regression-witness); they must go green without
  being renamed.

## Cross-references

- `docs/upcoming/end-to-end-monomorphization-plan.md` -- the M5 effort.
- `docs/upcoming/m5-residual-straddle-retirement.md` -- root cause C.
- `docs/reported/make-struct-phantom-typeparam-lowering.md` -- origin of the
  `default-of` builtin behind root cause B.
- `CLAUDE.md` -- Fixture Snapshots / "a codegen mismatch is a real failure"
  and the Reporting Bugs rule under which this was filed.
