# M5 suite residual: 6 failing fixtures, 3 root causes

**Status:** ALL THREE root causes FIXED 2026-06-14. Full suite
`1626 passed, 0 failed` (from the original 6 failures). A = tyvar/global
shadowing at ascriptions; B = `default-of` builtin vs user method; C =
return-ABI disagreement for carrier-producing lifted lambdas (3 emit sites
realigned to the int64 carrier). One benign `-Wint-conversion` residual noted
under root cause C as a follow-up.
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

**Status: FIXED 2026-06-14.** `elab_call.c:1192` now gates the builtin
interception on `!elab_name_is_typeclass_method(e, name)`: when a user
typeclass declares a method named `default-of`, the call falls through to the
return-dispatch path (which resolves the instance from the expected-type
channel) instead of being captured by the `(default-of T)` builtin. Stdlib
never declares a `default-of` class, and its make-struct payload fills
(`(default-of A)` in option.tur / result.tur) are elaborated before any user
class is registered, so they still hit the builtin -- verified by a combined
program that declares `Default` *and* uses `ok`/`err`/`some`. All three
fixtures PASS; full suite `1622->1625 passed`, no regressions.

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

**Status: FIXED 2026-06-14.** The coordinated three-site fix landed; the
fixture passes and the full suite is `1626 passed, 0 failed`. See "Resolution"
below.

**Fixture:** `hkt-stdlib-option-result-instances`.
**Severity:** the genuine M5 monomorphization residual (was tracked
conceptually in `docs/m5-residual-straddle-retirement.md`), surfacing
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

### Deeper analysis (2026-06-14 investigation)

This is NOT a regression: the fixture was added in `0fd565f` and has never
passed -- it is an aspirational target of the in-flight monomorphization, not
previously-working behavior.

The disagreement is **bidirectional** -- two call conventions want opposite
return ABIs for the same `(Option int)`-typed value, and the `#{Construct}`
helpers sit in the middle producing only the carrier form:

1. **Direct-call named defn wants by-value + a deref bridge.** A minimal
   `(defn wrap [x : int] : (Option int) (some x))` fails identically:
   ```
   static Option__int wrap(int64_t x) { return some(x); }   // some() returns int64_t
   ```
   The caller already bridges by value -- `Option__int __t = wrap(...);
   some_qu((int64_t)(intptr_t)(&__t))` -- so the *intended* ABI here is a
   by-value return.  The bug is only that the body `return some(x)` hands back
   the int64 carrier; it needs a `CK_CARRIER -> CK_CONCRETE` deref
   (`return *(Option__int*)(intptr_t)some(x);`).  emit_fns.c already has
   exactly this unbox at the return site (the `*(%s *)(intptr_t)%s` branch),
   but it is gated tightly on `use_abi_spec && typeclass_inst != NULL`
   (M4c Path A only).  A contained experiment generalizing that branch to
   non-spec carrier-returning bodies (guarded by a "tail leaf is a
   `#{Construct}` helper or `__inst_` method" predicate) fixes the `wrap`
   case with **zero suite regressions** -- but does not green the fixture,
   because:

2. **A lifted lambda dispatched through a poly wrapper wants the int64
   carrier.** `(fn [x] (some (+ x 1)))` lifts to `__fn_895`, which is invoked
   *only* through `__poly_897(void*, int64_t) { return __fn_895(x0); }` (the
   fat/`tur_poly_fn_t` ABI is int64-in/int64-out).  Here the inner lambda
   should return the **carrier int64** -- the opposite of `wrap` -- so that
   both `return some(...)` and the wrapper's `return __fn_895(...)` are
   int64-consistent.  Instead `__fn_895` is monomorphized to return by-value
   `Option__int` (it is emitted as an ABI **spec**, so it bypasses the
   non-spec return-emit path entirely), and `__poly_897` then mismatches in
   the reverse direction.

So a complete fix must coordinate THREE emit sites with a single carrier
discipline: (a) the non-spec return-emit (direct calls -> by-value + deref
bridge), (b) the **spec** return-emit and lifted-lambda return type
(poly-dispatched lambdas -> int64 carrier), and (c) `make_poly_wrapper` so the
wrapper and its inner agree.  This is precisely the
`CK_CONCRETE <-> CK_CARRIER` straddle retirement; prior principled attempts in
this area regressed the suite (see the "+1 hamt-delete regressor" notes in
`m5-instance-spec-doesnt-propagate-constraint-var-bindings.md`), so it is not
a contained fix.

### Resolution (2026-06-14)

The actual root cause turned out to be narrower and cleaner than the
"three-way carrier discipline" framing above feared: the lifted lambda
`__fn_895` had **no `result_full_type`**, so the three emit sites that decide
its C return type fell into their respective `else` branches and *disagreed*:

| site | else-branch logic | result for `__fn_895` |
|---|---|---|
| signature (`emit_fns.c`) | `fd->body->type` -> `_body_c` | `Option__int` (wrong) |
| forward decl (`emit_module.c`) | `fd->body->type` -> `_body_c` | `Option__int` (wrong) |
| body-return (`emit_fns.c`) | `emit_type_from_kind(result_kind)` | `int64_t` (right) |

The body-return block was already correct (the body value `some(...)` IS an
int64 carrier, and the lambda is dispatched through the int64-in/int64-out
fat/poly thunk).  The two `_body_c` sites over-eagerly used the by-value
aggregate name.  Aligning all three to int64 for a *carrier-producer* body
fixes it -- and because `__fn_895` now returns int64, its poly wrapper
`__poly_897` (`return __fn_895(x0)`, int64->int64) becomes consistent for
free.  **No `make_poly_wrapper` change was needed.**

A new shared predicate `fn_body_tail_is_carrier_producer` (every tail leaf is
a `#{Construct}` helper or `__inst_` method, i.e. emits the int64 carrier)
discriminates a carrier-producer lambda (-> int64) from a genuine by-value
aggregate body like `(Pair float float)` via `make-struct` (-> keep
`_body_c`).  The three coordinated changes:

1. `emit_fns.c` signature `else` branch -- carrier-producer body emits int64.
2. `emit_module.c` forward-decl `else` branch -- same (mirror), via the
   now-shared predicate exported in `emit_internal.h`.
3. `emit_fns.c` body-return -- a `CK_CARRIER -> CK_CONCRETE` deref bridge for
   the *other* convention: a **named** defn (which DOES have a
   `result_full_type`, so it keeps the by-value `Option__int` return that its
   caller spills) whose body is a carrier producer.  This is the `wrap` case
   (`(defn wrap [x:int] : (Option int) (some x))`) and generalizes the
   existing M4c spec-only deref past its `typeclass_inst` gate.

The two return conventions now coexist correctly: a lifted lambda dispatched
through the int64 thunk returns the carrier; a directly-called named defn
returns by value and the body deref-bridges a carrier producer.

### Residual -- FIXED 2026-06-14

The Applicative `ap` line emitted a `-Wint-conversion` warning:
`int64_t ff_919 = some(__t26)` where `__t26` is a `void *` closure box passed
into `some`'s int64 carrier param without a `(int64_t)(intptr_t)` cast -- a
"works by luck" same-width pointer->int conversion (`(some (:: (fn ...) int))`
stashes a closure into the Option payload).

Root cause: the direct-call arg emitter (`emit_expr.c`) **strips** the
`(:: <closure> int)` ascription to its inner closure before emitting, so its
`needs_fn_cast` test -- which consulted only the *original* arg's type (now
`TY_INT`) -- missed that the emitted value is a `void *`.  And `some`'s param
is `TY_TYVAR`, whose carrier-cast branch was gated to inline-C bodies only,
even though the generic (`!matched_spec`) emit of a make-struct/carrier body
declares the param `int64_t` (`static int64_t some(int64_t)`).

Fix (two narrow changes at the direct-call arg path):
1. `needs_fn_cast` also consults the *stripped* `emit_arg`'s type (`TY_FN` /
   `TY_PTR_VOID`), not just the pre-strip arg type.
2. the `TY_TYVAR`/`TY_FORALL`/`TY_EXISTS` carrier-param branch fires for the
   generic emit (`!matched_spec`) as well as inline-C bodies; a matched spec
   (concrete C param type) keeps the existing no-cast handling.

Now emits `some((int64_t)(intptr_t)(__t27))`.  Full suite `1626 passed, 0
failed`, no regressions.

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
- `docs/m5-residual-straddle-retirement.md` -- root cause C.
- `docs/reported/make-struct-phantom-typeparam-lowering.md` -- origin of the
  `default-of` builtin behind root cause B.
- `CLAUDE.md` -- Fixture Snapshots / "a codegen mismatch is a real failure"
  and the Reporting Bugs rule under which this was filed.
