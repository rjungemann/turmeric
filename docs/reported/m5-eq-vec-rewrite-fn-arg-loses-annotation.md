---
title: Eq Vec instance body cannot pass a typed (fn [A A] bool) arg to a (Vec A)-receiving helper
severity: blocking for the Option D Eq Vec rewrite; latent elaborator gap independent of M5
date: 2026-06-14
---

## Summary

Inside a `(definstance Eq [Vec] [(Eq A)] (eq? [x y] ...))` body, calling
a polymorphic helper `(defn h [A] [v : (Vec A) ... ^fat cmp : (fn [A A]
bool)] ...)` with a lambda argument fails to elaborate: the cmp formal's
recorded type is `(fn [] : ?)` (zero arity, unknown result) at the call
site, never the declared `(fn [A A] bool)`.

The same helper called from a non-instance polymorphic defn elaborates
and compiles cleanly, so the gap is specific to the typeclass-instance
body's elaboration context, not the helper signature.

## Repro

```turmeric
;; in stdlib/vec.tur
(defn vec-eq-loop-byval [A]
  [x : (Vec A) y : (Vec A) i : int len : int
   ^fat cmp : (fn [A A] bool)]
  : bool
  (if (= i len)
    true
    (if (cmp (:: (vec-get-byval x i) A) (:: (vec-get-byval y i) A))
      (vec-eq-loop-byval x y (+ i 1) len cmp)
      false)))

(definstance Eq [Vec]
  [(Eq A)]
  (eq? [x y]
    (let [xv (:: x (Vec A))
          yv (:: y (Vec A))
          lx (.len xv)
          ly (.len yv)]
      (if (= lx ly)
        (vec-eq-loop-byval xv yv 0 lx (fn [a b] (eq? a b)))
        false))))
```

## Observed

```
error [TUR-E0001]: function 'vec-eq-loop-byval' arg 5: expected (fn [] : ?),
got (fn [int int] : bool)
        (vec-eq-loop-byval xv yv 0 lx (fn [a b] (eq? a b)))
                                      ^^^^^^^^^^^^^^^^^^^^
```

The expected type `(fn [] : ?)` says the elaborator lost both vec-eq-loop-
byval's cmp param's arity (`[A A]` → `[]`) and result (`bool` → `?`)
during the call's arg-type check.

## Control

The same helper called from a non-instance, non-typeclass polymorphic
defn elaborates correctly:

```turmeric
(defn use-byval [A] [v1 : (Vec A) v2 : (Vec A) ^fat cmp : (fn [A A] bool)] : bool
  (vec-eq-loop-byval v1 v2 0 (.len v1) cmp))
;; compiles and runs cleanly.
```

So the helper's signature is correctly stored on the binding; the gap
is in how the typeclass-instance body re-reads / unifies that signature
against the call's lambda arg.

## Alternative repro -- constrained-poly helper segfaults

Trying the alternative shape -- make the helper itself constrained-poly
on `(Eq A)` and call `eq?` directly instead of taking a cmp arg --
SEGVs the elaborator:

```turmeric
(defn vec-eq-loop-byval [A]
  [(Eq A)]
  [x : (Vec A) y : (Vec A) i : int len : int]
  : bool
  (if (= i len)
    true
    (if (eq? (:: (vec-get-byval x i) A) (:: (vec-get-byval y i) A))
      (vec-eq-loop-byval x y (+ i 1) len)
      false)))
```

```
SUMMARY: AddressSanitizer: SEGV elab_typeclasses.c:3388 in elab_method_call
```

Two distinct elaborator gaps, both blocking the Option D Eq Vec
rewrite from `docs/upcoming/m5-residual-straddle-retirement.md`.

## Severity

Hard compile error / hard crash; does not affect any current stdlib
shape (Eq Vec retains the carrier-bridging body it had before).
Blocks the M4c-pre-ext straddle retirement.

## Root cause hypothesis

### Diagnostic finding (2026-06-14, post-investigation)

The diagnostic message itself is misleading.  At
`elab_call.c:3006-3014`, the expected-type enrichment whitelist for the
"expected X, got Y" diagnostic is
`{UNION, INTERSECTION, APP, HANDLER, STRUCT, ADT}` -- it omits TY_FN.
For a TY_FN-expected param the message degrades to `type_from_kind(TY_FN)`
which stringifies to `(fn [] : ?)`.  The REAL expected type is the
helper's declared `(fn [A A] bool)`.  One-line fix to the diagnostic.

### The actual elab gap

The lambda `(fn [a b] (eq? a b))` elaborates to TY_FN with
`arg_kinds=[TY_INT, TY_INT]` and `arg_full_types=NULL` in BOTH contexts
(verified by instrumenting `elab_fn`'s final fn_type construction).

But by the time the call site reaches `call_collect_type_bindings`,
the lambda's `actual.as.fn.arg_full_types` is:
- Inside a `(definstance Eq [Vec] [(Eq A)] (eq? [x y] ...))` body:
  **non-NULL**, pointing to TY_TYVAR(A) for both positions.
- Inside a `(defn caller [A] [(Eq A)] ...)` polymorphic defn:
  **NULL**.

The lambda's arg_full_types is populated AFTER `elab_fn` returns but
BEFORE `call_collect_type_bindings` runs, via some pass that fires in
the definstance context but not the plain polymorphic defn context.
The mechanism is not visible via direct grep for `arg_full_types =`
mutations -- it must run through a chain of typeclass-resolution
helpers that have side effects on the lambda's TY_FN structure.

`type_eq(TY_TYVAR(A), TY_INT) = 0` (kind mismatch at `types.c:65`),
so once the lambda DOES get arg_full_types=NULL, the cmp's
`(fn [A A] bool)` expected vs `(fn [int int] :bool)` actual fails:
A was previously bound to TY_TYVAR(A) via args 0/1's `(Vec A)` formals,
then arg 4's actual TY_INT for the lambda doesn't match.

### Why definstance bodies are different

Confirmed by side-by-side trace.  The exact same lambda source, the
exact same `[(Eq A)]` constraint, the exact same helper signature --
only the enclosing form (definstance vs plain defn) differs.  The
gap is a top-down inference channel that fires from one but not the
other.

### Further trace 2026-06-14 (session 2, attempt 3)

Instrumented `elab_fn` at the closure-conversion point.  Side-by-side
between gap1b (definstance body, works) and gap1e (plain defn body,
fails):

- gap1b: only 3 lambdas reach `elab_fn`, all with `e->current_fn_name =
  ?top`.  The "cmp arg" lambda inside the Eq Vec instance method body
  is NOT one of them.
- gap1e: 3 lambdas in `?top` + 1 in `caller` (the cmp arg lambda).

This means the Eq Vec body's `(fn [a b] (eq? a b))` is being
**optimised away at the typeclass-elab level** -- it never reaches
`elab_fn`.  Most likely it's eta-reduced to a direct reference to
`eq?` (the typeclass method itself), which already carries TY_FN
with fully-typed `arg_full_types`.  The cmp arg at the call site is
then the typeclass method reference, not a lambda Expr -- which is
why its TY_FN has aft populated and unification succeeds.

The plain-defn case (`caller`) doesn't trigger the eta-reduction (or
the optimisation gate excludes that context), so the lambda goes
through `elab_fn` and produces a TY_FN with NULL aft.

The optimisation site itself was not localised this session.
Suspects: a typeclass-method-elab early-rewrite in
`elab_typeclasses.c`, or an eta-reduction in `elab_call.c` when an
arg's value is exactly `(fn [a b] (method a b))` and `method` is a
typeclass method.

### AST-diff finding 2026-06-14 (session 2, attempt 4)

Two corrections to earlier analyses:

1. **gap1b doesn't actually work**.  Earlier comparison showed
   "definstance body works, plain defn fails", attributed to a
   typeclass-elab optimisation.  AST dumping showed the user-level
   `(definstance Eq [Vec])` in gap1b was being **silently overridden
   by stdlib's existing Eq Vec** -- the user's `loop-helper` was
   never emitted (`emit-c` output has no `loop_helper` symbol).
   The "working" path was stdlib's Eq Vec body using
   `(vec-eq-loop xi yi 0 lx (fn [a b] (eq? a b)))` where xi/yi are
   typed `:int` -- so the lambda's int args match the helper's A
   (bound only via cmp).
   
   With a fresh class (`defclass MyClass [a]`) that doesn't shadow,
   the definstance body's call to the helper FAILS with the same
   "expected (fn [] : ?)" error.  The gap is universal across
   contexts.

2. **Root cause refined**: in helper signatures with `[x : (Vec A)
   y : (Vec A) ... cmp : (fn [A A] bool)]`, args 0/1 bind helper's
   `A` to the outer caller's abstract tyvar FIRST.  When cmp's
   `(fn [A A] bool)` is then checked against the lambda's
   `(fn [int int] :bool)`, A is already bound to TYVAR; the
   concrete int from the lambda doesn't unify with TYVAR.
   `call_collect_type_bindings`'s TY_TYVAR case fails on
   `type_eq(prior=TYVAR, actual=int)`.

3. **Attempted fix**: in `call_collect_type_bindings` TY_TYVAR
   case, allow a CONCRETE actual to upgrade a prior abstract-
   placeholder binding (where the placeholder is TY_TYVAR OR
   TY_STRUCT-NULL-def).  The principle: an abstract placeholder
   was "any type"; a concrete is a valid instantiation; this is
   exactly what unification should do.
   
   Result: the simple plain-defn case (gap1e) now compiles and
   runs correctly.  But the suite saw a NET ZERO change: one
   test (emit-abi-trace) started passing, one test (hamt-delete)
   started SIGSEGV'ing at runtime.  The hamt-delete regression
   suggests the upgrade is too permissive in some context --
   probably interacts with how HAMT specs are interned across
   multiple call sites with different concrete types.
   
   Reverted.  The fix is on the right track but needs a tighter
   gate that excludes whatever HAMT pattern conflicts.

### Two-step fix

1. **Diagnostic** (one-liner): add TY_FN to the enrichment whitelist
   at `elab_call.c:3007` so the error message shows the real expected
   type instead of "(fn [] : ?)".  Independent of the deeper fix and
   strictly improves observability.  **LANDED 2026-06-14.**  Probe:
   `(defn h [A] [^fat cmp : (fn [A A] bool)] : bool (cmp 1 1))`
   called with `(h 42)` now reports `expected (fn [int int] : bool),
   got int` instead of `(fn [] : ?)`.

2. **Lambda inference channel**: identify what populates
   arg_full_types in the definstance body case and replicate it for
   plain polymorphic defn bodies.

   **Session-3 trace finding (2026-06-14)**: there is NO existing
   mechanism populating arg_full_types on lambdas from caller context.
   `elab_fn` (elab_fns.c:3165) reads only the lambda's own explicit
   annotations; `elab_call_fn`'s arg loop does not pass expected types
   through `elab_form`.  The earlier "definstance body works" claim was
   misattribution (stdlib Eq Vec shadowing per the AST-diff finding).
   The gap is universal across enclosing contexts.  All three suspects
   above are ruled out by direct grep of `arg_full_types` writers.

   **Session-3 fix attempt (REVERTED)**: pushed expected_type at
   elab_call.c:2630 and consumed it in elab_fn after param parsing
   (conservative split: only set `param_full_types[i]`, leave
   `param_kinds[i]` and `params[i]->type` as TY_INT default so the body
   keeps int-operator dispatch).  Added compensating logic in
   `call_collect_type_bindings` TY_TYVAR case: same-name self-binding
   skip + narrow "prior self-tyvar can be upgraded by concrete actual"
   path.

   Probes (gap1-probe plain defn; gap1-instance definstance body) BOTH
   compiled and exited 0.  Suite: +75 unique FAILs vs diagnostic-only
   baseline:
   - 72 codegen-snapshot drift (deterministic; regen-able).
   - 1 stdout mismatch: **hamt-delete** (same regression as the prior
     attempt at TY_TYVAR upgrade; the narrower self-tyvar gate did NOT
     prevent it -- the upgrade still interacts with HAMT spec interning
     in a way the gate doesn't capture).
   - 2 build failures including `m5-byval-marker-spec-emit` (fixture
     pinning prior M5 work; conservative split breaks its spec-emit
     path).

   Reverted.  For the next attempt, isolate hamt-delete's specific
   call_collect_type_bindings path FIRST (likely a HAMT-of-HAMT spec
   that registers two A bindings for the same param shape).  Then trace
   what m5-byval-marker-spec-emit's spec lookup does differently when
   the helper's arg_full_types now carries TY_TYVAR vs NULL.

### Workaround (works today)

Typed lambda: `(fn [a : A b : A] : bool (eq? a b))` succeeds in plain
polymorphic defns.  Inside a definstance method body, the typed lambda
hits a SEPARATE bug (the SEGV at elab_typeclasses.c:3388 in the
constrained-poly helper alternative form -- unverified for the
definstance-body case).

## Workaround

None.  Keep the existing Eq Vec body (`(:: x :int)` ascription +
carrier vec-eq-loop).  The M5 residual-straddle retirement plan stays
blocked on this gap.

## Validation under fix

The fixture `tests/fixtures/m5-byval-marker-spec-emit/` already covers
the working case (helper invoked from a non-instance polymorphic defn);
a fix to either form should make the analogous instance-body shape
build and pass.

## Related

- `docs/upcoming/m5-residual-straddle-retirement.md` -- Option D plan
  that this gap blocks.
- `docs/upcoming/end-to-end-monomorphization-plan.md` -- M5 phase.
- `docs/archive/history/m5-constrained-poly-spec-wrong-dispatch-for-parametric-receiver.md`
  -- the elab fix for the dispatch side; this is a different elab gap
  on the typed-fn-arg side.
