---
title: Constrained-poly defn with (Eq A) constraint dispatches eq? to wrong instance when receiver type is TY_TYVAR (silent miscompile / SIGSEGV)
severity: silent miscompile, SIGSEGV at runtime
date: 2026-06-14
---

## Summary

A `(defn h [A] [(Eq A)] [v : int] : bool (eq? (:: v A) (:: v A)))` defn
elaborates without error, but the emitted helper body dispatches to
`__inst_Eq_eq_qu_MutableMap(v, v)` — the wrong instance.  At runtime
the helper reads garbage and SIGSEGVs (or worse, silently returns
wrong results).

The receiver of `(eq? ...)` is `(:: v A)` whose static type is `A`,
a TY_TYVAR.  The enclosing fn has `(Eq A)` constraint, so the dispatch
SHOULD route through the constraint dict.  Instead the typeclass
solver falls through to a "first non-primitive instance" pick that
just happens to grab `MutableMap` (alphabetically near the head of
the instance list).

## Minimal repro

```turmeric
(defn helper [A]
  [(Eq A)]
  [v : int]
  : bool
  (eq? (:: v A) (:: v A)))

(defn main [] : int
  (if (helper 1) 0 1))
```

```
$ ./build/tur build /tmp/gap2d.tur -o /tmp/gap2d
$ /tmp/gap2d; echo "exit=$?"
exit=139   # SIGSEGV
```

## Observed emission

```c
static bool helper(int64_t v) {
    return __inst_Eq_eq_qu_MutableMap(v, v);
}
```

The helper has no spec — just the carrier body — and bakes
MutableMap as the dispatch target.

## Expected emission

```c
static bool helper(int64_t v) {
    /* dispatch via the (Eq A) constraint dict, or via a per-A spec */
    return __inst_Eq_eq_qu_int(v, v);  /* when specialized for A=int */
}
```

Or: the helper takes a dict pointer and dispatches through it; the
caller passes the int dict.

## Control: works for some receiver shapes

Bare-A receiver works correctly:

```turmeric
(defn loop-byval [A] [(Eq A)] [a : A b : A] : bool
  (eq? a b))
(defn main [] : int (if (loop-byval 1 1) 0 1))   ; exit=0
```

The difference is whether the receiver's static type ARRIVES as
TY_TYVAR via an EX_VAR (works) vs via an EX_ASCRIBE to A (broken).

## Root cause

`elab_typeclasses.c:3653-3671` computes `obj_ck` from the receiver
TypeKind.  For TY_TYVAR it falls into the KIND_ARROW branch (TY_TYVAR
isn't in the primitive list).  The instance search loop at L3674+
then iterates all instances and picks the first non-primitive one
(L3719: `type_ok = !inst_is_primitive`).  Alphabetical instance
ordering happens to put `Eq MutableMap` near the head.

The missing logic: when the receiver is TY_TYVAR AND the enclosing
fn has a class-constraint `(Eq A)` where A matches the receiver tyvar,
defer dispatch via the constraint dict instead of baking a concrete
instance.  This is the classic Haskell-style constraint dispatch
that the rest of the typeclass system relies on but the EX_ASCRIBE
path misses.

## Severity

**Silent miscompile** — the program builds without warning and may
appear to work for small inputs.  At runtime the wrong-instance call
reads garbage from struct field offsets that don't exist for int data
and SIGSEGVs (or worse, silently returns plausible-but-wrong bool).

## Why the pattern was rare before M5

Existing constrained-poly defns in stdlib (e.g. `vec-eq-loop`) accept
the comparator via a `^fat cmp : (fn [A A] bool)` parameter and let
the call site close over the actual `eq?` dispatch.  That sidesteps
this bug by NEVER calling a typeclass method on a tyvar receiver
inside the constrained body.

Option D's `vec-eq-loop-byval` rewrite explored direct `(eq? ...)`
dispatch from the body (cleaner API, no `^fat cmp` plumbing) and
surfaced the gap.

## Workaround

Use the `^fat cmp : (fn [A A] bool)` parameter pattern instead.

## Proposed fix directions

1. **Constraint-dict dispatch in elab_typeclasses.c** for TY_TYVAR
   receivers when an enclosing constraint matches.  The dict is
   already threaded through the call ABI (see other constrained-poly
   call sites); the missing piece is making `(eq? ...)` use it
   instead of falling through to instance search.
2. **Bail to a diagnostic** when no concrete instance match exists
   for a TY_TYVAR receiver and no constraint provides the dispatch.
   Better than silent miscompile while a full fix is designed.

## Validation under fix

After fix, `/tmp/gap2d.tur` should either:
- compile and `exit=0` (constraint-dispatched, correct);
- emit a clean elab diagnostic (no constraint matches).

The existing `vec-eq-loop` ^fat-cmp pattern keeps working as before.

## Related

- `docs/reported/m5-eq-vec-rewrite-fn-arg-loses-annotation.md` —
  gap 1 (lambda annotation loss in plain polymorphic defn).
- `docs/archive/history/m5-constrained-poly-spec-wrong-dispatch-for-parametric-receiver.md` —
  FIXED earlier; addressed parametric receiver (Vec A), not
  TY_TYVAR-via-ascription.
- `docs/upcoming/m5-residual-straddle-retirement.md` — Option D
  plan that this gap blocks.
