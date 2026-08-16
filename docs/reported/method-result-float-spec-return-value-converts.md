# A float typeclass-method result crossing into a generic call is value-converted, not reinterpreted

**Severity:** silent wrong answer. No crash, no diagnostic -- `7.1` becomes
`3.45846e-323`.
**Status:** OPEN. Pre-existing (reproduced on the compiler before the
2026-08-16 argcast routing change, byte-identical emitted C both sides).
**Found by:** `tests/type-fuzz-src.py --n 250 --seed 9201`, case 98.

## Minimal repro

```turmeric
(defclass FzT10 [a] (l1p1 [self : a] : a))
(definstance FzT10 [float] (l1p1 [self : float] : float self))
(defn l1g2 [A] [x : A] : A x)
(defn main [] : int
  (println (l1g2 (l1p1 7.1)))   ; expect 7.1, prints 3.45846e-323
  0)
```

Neither half fails alone -- it takes the composition:

| expression | result |
| --- | --- |
| `(= (l1p1 7.1) 7.1)` -- method alone | `true` |
| `(= (l1g2 7.1) 7.1)` -- generic identity alone | `true` |
| `(= (l1g2 (l1g2 7.1)) 7.1)` -- two generic identities, no method | `true` |
| **`(= (l1g2 (l1p1 7.1)) 7.1)`** -- one generic identity over a method result | **`false`** |

So the defect is specifically **a float typeclass-method result crossing into
a generic (carrier) call argument**.

## Root cause: producer converts by VALUE, consumer reinterprets by BITS

The emitted C makes it plain.  The specialization clone of the instance
method is declared to return the int64 carrier while its body returns the
`double` parameter:

```c
static int64_t __inst_FzT10_l1p1_float__spec__int64_t_double(double self) {
        return self;          /* C converts 7.1 -> 7 here */
}
```

`return self;` from a `double` into an `int64_t` return type is an implicit
**floating-to-integer conversion** in C, not a reinterpret.  The value is
already destroyed before it leaves the callee.

The caller then does the opposite -- a correct **bit** reinterpret:

```c
int64_t __ps_159 = (__inst_FzT10_l1p1_float__spec__int64_t_double(7.1));
double  __ps_160 = (l1g2__spec__double_double(
                      ((union { int64_t s; double d; }){.s = (__ps_159)}).d));
```

So the round trip is `7.1` --value-> `7` --bits-> `3.45846e-323`.  The
un-specialized instance is fine (`static double __inst_FzT10_l1p1_float(double
self)`); only the carrier-returning `__spec__int64_t_double` clone is wrong.

This is the campaign's canonical shape -- two sites disagreeing about a
carrier convention -- with the twist that the disagreement is
**value-vs-bits** rather than boxed-vs-unboxed, so it produces a wrong number
instead of a compile error or a crash.

## Fix direction

The spec clone's return needs the same bit reinterpret the caller applies,
rather than C's implicit conversion:

```c
return ((union { double d; int64_t s; }){.d = self}).s;
```

The caller side already has this union-bitcast idiom (see `__ps_160` above),
so the fix is to emit its mirror on the producing side whenever a
carrier-returning specialization's body tail is a float-typed value.  Worth
checking the same clone family for `float32` and for a float **argument**
crossing the other way before fixing, since the value/bits confusion is not
inherently specific to the return slot.

## Why the fixtures never caught it

Two one-sided coverage gaps compounding, which is the pattern
`docs/upcoming/representation-consolidation-meta-plan.md` warns about:

- the method-result carrier crossing is well covered at `int`, where value
  conversion and bit reinterpretation are the same operation, so the bug is
  invisible;
- the float carrier round trip is covered for plain generic calls, which do
  not go through a carrier-returning instance clone.

Only the composition of the two exposes it, and it needs a literal with a
non-zero fractional part -- CLAUDE.md's float-probe rule exists for exactly
this class, and `7.0` would have printed `7` and looked fine.

## Guide upkeep

`docs/guides/value-representations-guide.md` -- this is an open cell:
**typeclass method result (float) -> generic carrier call argument**.  Add it
to the open-cells table when filing work on it.
