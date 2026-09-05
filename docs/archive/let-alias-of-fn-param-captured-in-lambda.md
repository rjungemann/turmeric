---
title: A let-alias of a fn-typed parameter is mishandled when captured into a lambda
category: Archive
description: RESOLVED 2026-09-05, same day, in three lines. Both symptoms were one defect and the lambda was not part of it -- the minimal repro has no lambda at all. A fn-typed param that is fat by NORMALIZATION rather than by the ^fat ANNOTATION did not carry that fact through a let alias, so every guard keyed on is_fat re-shimmed the alias into a second fatshim box.
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


---

# Resolution, 2026-09-05

Fixed the same day it was filed, in three lines, and **the filing's framing was
wrong in two ways**. Both are worth keeping because the bisection that found
them is the cheap part anyone could have run.

## The lambda is not part of it

The report presents two cases, both involving a capture into a lambda. The
minimal repro has **no lambda and no capture**:

```turmeric
(defn use3 [f : (fn [int] Pair2)] : int
  (let [g f]                     ;; alias a fn-typed PARAM
    (.a (apply2 g 5))))          ;; forward the alias
```

-- SEGV, while `(apply2 f 5)` without the alias is fine. Four probes got there:
passing the param directly works, aliasing a LOCAL works, forwarding the param
without an alias works, aliasing the PARAM does not. The lambda in the filed
repro was incidental scaffolding.

## The two cases are one defect

They are not "callee position" and "argument position" behaving differently.
Both are the same missing fact, and the same three-line change fixes both.

## What it was

A fn-typed parameter can be fat two ways: by the `^fat` ANNOTATION (which sets
`Binding.is_fat`) or by NORMALIZATION (`fn_param_type_is_fat_normalized`, keyed
on the type -- the callee's invoke dispatches fat either way). The let-alias
propagation in `elab_forms.c` carried only the first:

```c
if (init_b->is_fat) { b->is_fat = true; }
```

So an alias of a normalized nominal param lost the fact, and the call-site
pass-through re-shimmed it into a second `__tur_fatshim` box:

```c
int64_t g = (int64_t)(intptr_t)(f);
__t185[0] = (int64_t)(intptr_t)__tur_fatshim_tur_adt_Pair2_int64_t;
__t185[1] = (int64_t)(intptr_t)g;        /* g is ALREADY a fat handle */
apply2((int64_t)(intptr_t)(__t187), 5);
```

The consumer then reads the inner handle's first word as code.

The pass-through's own comment says it covers "a `^fat` parameter (or a
let-alias of one)", and it does test both kinds -- but its normalized arm
requires `is_param`, which an alias is not. Carrying the fact on the ALIAS fixes
it for every guard keyed on `is_fat` rather than for that one call site.

## The result type is why this hid

An `int`-returning callback never failed: the double-box only misbehaves once
the result is a by-value aggregate. `defstruct` and `:copy defdata` results both
failed, so it is not a defstruct-lowering question. `tests/fixtures/let-alias-of-fat-fn-param`
carries all of it -- both repro positions, the no-alias control, the
int-result control, and both aggregate flavours -- and fails on the pre-fix
compiler.
