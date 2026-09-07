---
title: A float result dispatched through a mode-B dict clone is truncated to an integer, silently
category: Archive
description: RESOLVED 2026-09-07 via fix direction 1 (bit-reinterpret both ends). The dict-clone body returns the int64 carrier via `(int64_t)(intptr_t)__ps`, a numeric conversion -- so a method declared `: float` returns 2.5 as 2 and 7.1 as 7. No warning, no diagnostic, correct-looking code. The same method dispatched statically returns 7.1. The emitter's float-to-carrier crossings elsewhere bit-reinterpret through a union; this one converts.
---

# A float result through a mode-B dict clone is silently truncated

**RESOLVED 2026-09-07 via fix direction 1 -- bit-reinterpret, both ends.**

The report leaned toward direction 2 (give the clone a real `double` return).
Measuring the chain settled it the other way: the clone's result travels
through `tur_poly_fn_t`, whose `fn` field is a shared
`int64_t (*)(void *, int64_t, ...)`. Giving one clone a `double` return would
mean a different carrier type per result type -- a change to an ABI every poly
value uses. Direction 1 turned out to be the *smaller* change, not the larger
one.

Both ends were converting numerically, so the fix is symmetric:

- **Producer** (`emit_fns.c`, the `n_dict_clone` return): `tur_sc_bits_f64` /
  `tur_sc_bits_f32` instead of `(int64_t)(intptr_t)`.
- **Consumer** (`emit_expr.c`, the poly-call result): `tur_sc_f64_from_bits` /
  `tur_sc_f32_from_bits`, in a new float arm beside the existing pointer-class
  arm -- which had handled `cstr`/`ptr`/`rc` all along and simply had no float
  case.

The helpers already existed in the preamble, and their own comment already said
why: *"Float params round-trip through the int64 saved[] slots by BIT
reinterpretation (an intptr_t cast would truncate the value)."* The knowledge
was in the codebase; the dict-clone path just never applied it.

The two halves must stay in lockstep -- packing without unpacking turns a
truncation into garbage, which is worse -- so the fixture asserts the value
coming back out, not just the emission.

**Verified:** `2.5 / 7.1` for `float` and `3.25 / 1.5` for `float32`, byte-
identical compiled and interpreted; static dispatch of the same methods (always
correct) pinned alongside so a future change cannot regress the working path;
`forall-dict-show` (a `cstr` result through the same carrier) unaffected.

**Fixture:** `forall-dict-float-result`. Every literal in it has a non-zero
fractional part, which is the point: this defect is invisible to `7.0`, and it
survived because no fixture in the `forall-dict-*` family declared a float
result at all.

**Suites:** `run.sh` 2840/0, `run-turi.sh` 1933/0, `check-examples` 38/0,
`run-stdlib-checks` 35/0.

**Knock-on:** the `:heap` receiver route mentioned in
[forall-dict-byvalue-receiver-emits-uncompilable-c](forall-dict-byvalue-receiver-emits-uncompilable-c.md)
now returns correct values (19.6349 / 50.41). It still emits a
`-Wint-conversion` warning, because the dispatched signature is still spelled
from the representative instance's pointer type, so that report's diagnostic
still does not recommend it.

---

## The original report

**Severity: high.** A silent wrong answer -- not a crash, not a diagnostic, not
even a warning. Any `forall`-constrained rank-2 call to a typeclass method
declared `: float` returns the value with its fractional part discarded.

Found while guarding
[forall-dict-byvalue-receiver-emits-uncompilable-c](forall-dict-byvalue-receiver-emits-uncompilable-c.md).
It is independent of that defect: it needs no by-value aggregate and no
pointer, only a float **result**.

## Repro (2026-09-07)

```turmeric
(defclass Scale [a] (scale [x : a] : float))
(definstance Scale [int]  (scale [x : int]  : float (* 2.5 1.0)))
(definstance Scale [bool] (scale [x : bool] : float 7.1))

(defn poly-scale [a] [^Scale a x : a] : float (scale x))

(defn use-both [f (forall [a] [(Scale a)] (-> a float))] : float
  (println (f 3))
  (println (f true))
  0.0)

(defn main [] : int (println (use-both poly-scale)) 0)
```

```
$ tur run tc5.tur
2          <- expected 2.5
7          <- expected 7.1
0
```

Both receivers are carrier-shaped primitives, so nothing here is the by-value
problem. Dispatching the same method **statically** is correct:

```turmeric
(definstance Scale [int] (scale [x : int] : float 7.1))
(defn main [] : int (println (scale 3)) 0)    ;; => 7.1
```

## Root cause

The clone body returns the int64 carrier by **numeric conversion**:

```c
static int64_t poly_hyscale_un_undict_un1444(int64_t __dict_1445, int64_t x) {
        double __ps_40 = (((double (*)(int64_t))((void **)(intptr_t)__dict_1445)[0])(x));
        if (tur_panicking) return ((int64_t)0);
        return (int64_t)(intptr_t)__ps_40;      /* <-- 19.6349 becomes 19 */
}
```

`(int64_t)(intptr_t)` on a `double` is a C numeric conversion: it rounds toward
zero and keeps the integral part. The method's real value never reaches the
caller.

Everywhere else the emitter crosses a float through the int64 carrier it
**bit-reinterprets** instead, e.g. at an `any` widen site:

```c
((union { double s; int64_t d; }){.s = 7.1}).d
```

The clone's return path is the one that converts.

## Why it went unnoticed

`(defn scale [x : int] : float 7.0)` would return `7` and look right. Only a
literal with a fractional part exposes it -- which is exactly what CLAUDE.md's
float rule is for ("always pick a literal with a non-zero fractional part as
your first probe"). No fixture in the `forall-dict-*` family declares a float
result; they return `cstr` (`forall-dict-show`) or int.

## Fix directions

1. **Bit-reinterpret on the way out**, matching every other float/carrier
   crossing: emit the union punt rather than a cast when the clone's declared
   result is a float-family type. The receiving side must un-punt symmetrically
   -- check what the *caller* of the poly fn does with the returned carrier
   before landing this, since a one-sided change turns a truncation into
   garbage, which is worse.
2. **Or give the clone a real `double` return** when every instance's method
   returns a float, instead of forcing the int64 carrier. The carrier exists so
   one clone can serve instances with differing result types; a class whose
   result type is fixed by the class signature (`: float` is declared on the
   method, not on the class variable) does not need it.

Direction 2 is probably the right one -- the result type here is not
polymorphic at all, so carrying it through an integer carrier is pure loss --
but it needs a check that `emit_inst_fn_return_carrier` is not relied on to
paper over a genuine per-instance difference elsewhere.

Whichever lands, the fixture must use a fractional literal, and the family
needs one float-returning case per shape (`forall-dict-show` covers `cstr`
only).
