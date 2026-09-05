---
title: A let-alias of a fn-typed parameter is mishandled when captured into a lambda
category: Reported
description: `(let [g f] ... (fn [] ... g ...))` where `f` is a fn-typed parameter. In callee position the lifted body names `g` directly and the emitted C says "'g' undeclared"; in argument position it compiles and segfaults. Pre-existing (both symptoms reproduce against a compiler without the fat-alias declaration fix), and adjacent to the -Wint-conversion straddle that fix closed.
---

# A let-alias of a fn-typed parameter is mishandled when captured into a lambda

**Severity: medium** -- one shape is a hard C compile error and the other is a
SEGV, both for programs `tur check` accepts. Found 2026-09-05 while writing a
regression fixture for the fat-alias declaration fix (see *Relationship* below);
the fixture was abandoned because every minimal spelling of it lands on one of
these instead.

## The shape

A fn-typed PARAMETER arrives as the fat-closure handle. Aliasing it into a
`let` and then capturing the alias in a lambda goes wrong in two different ways
depending on how the lambda uses it.

### 1. Callee position -- `'g' undeclared`

```turmeric
(defstruct Pair2 :copy [a : int b : int])
(defn mk [n : int] : (fn [int] Pair2)
  (fn [x : int] : Pair2 (make-struct Pair2 :a n :b x)))

(defn deferred [f : (fn [int] Pair2) v : int] : (fn [] Pair2)
  (let [g f
        w v]
    (fn [] : Pair2 (g w))))          ;; g in CALLEE position
```

```
error: 'g' undeclared (first use in this function)
 4412 | tur_adt_Pair2 __ps_41 = (((tur_adt_Pair2 (*)(int64_t))(intptr_t)g)(__env___env_1452->w));
```

`w` is read from the env; `g` is named directly. So the capture analysis put
`w` in the env and not `g` -- a fn-typed local used as a callee is not being
counted as a capture.

### 2. Argument position -- compiles, segfaults

```turmeric
(defn apply2 [f : (fn [int] Pair2) x : int] : Pair2 (f x))

(defn deferred [f : (fn [int] Pair2) v : int] : (fn [] Pair2)
  (let [g f
        w v]
    (fn [] : Pair2 (apply2 g w))))   ;; g in ARGUMENT position
```

Here `g` IS captured (`__env->g = g`), the program builds, and it segfaults when
the returned thunk is called.

## Pre-existing, not introduced

Both symptoms reproduce against a compiler WITHOUT the fat-alias declaration fix
that landed alongside this filing. Without that fix case 2 additionally emits

```
warning: assignment to 'int64_t' from 'int64_t (*)(int64_t)'
         makes integer from pointer without a cast [-Wint-conversion]
 __t189->g = g;
```

-- so the fix strictly improves this shape (the straddle is gone) and the SEGV
is untouched by it. Do not read the two as one defect.

## Relationship to what DOES work

`stdlib/logic.tur`'s `st-bind` uses the same alias-and-capture shape and works:

```turmeric
(let [lth th
      lf  f]
  (StInc (fn [] (let [_ lth] (st-bind (st-force lth) lf)))))
```

`lf` is a fn-typed param alias, captured, used in argument position -- case 2's
shape, running correctly in the suite. So case 2 is NOT "argument position is
broken"; something narrower separates the two, and finding it is the first step.
Candidate differences, none of them checked: the working one passes the alias to
a TOP-LEVEL function while the probe's `apply2` is also top-level; the working
one returns a `:copy` ADT while the probe returns a struct; the working thunk is
stored in an ADT field while the probe's is returned as `(fn [] Pair2)`.

## Fix directions

Start by narrowing case 2 against the working `st-bind` shape -- one of those
differences is the trigger, and the failing/passing pair is a bisection that
costs a few probes. Case 1 is likely the simpler of the two and may be
independent: a fn-typed local in callee position inside a lifted lambda needs to
join the capture set, which is a question about `collect_free_vars` rather than
about representation.
