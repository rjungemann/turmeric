# A phantom type parameter does not drive monomorphization

**Severity: high** -- a **silent wrong answer**. A constrained generic whose
type variable appears only in a *phantom* position gets no specialization at
all, so every instantiation shares one body and dispatches to whichever
instance the representative resolves to. `tur check` is clean, cc is clean,
and the program prints a plausible number computed by the wrong instance.

**Status:** open. Found 2026-09-12 executing
[crdt-spice-plan.md](../upcoming/crdt-spice-plan.md) C3, whose `ORMap` design
it blocks outright (see "What it blocks").

## Repro

```turmeric
(defclass JS [a] (j [x : a y : a] : a))
(definstance JS [int] (j [x y] (if (< x y) y x)))          ;; max
(defopaque Sum :int)
(definstance JS [Sum] (j [x y] (:: (+ (:: x int) (:: y int)) Sum)))  ;; add

(defstruct Real [V] [v : V])     ;; V appears in a FIELD
(defstruct Ph   [V] [v : int])   ;; V is PHANTOM -- named in the head, in no field

(defn fromreal [V] [(JS V)] [x : (Real V) y : (Real V)] : V (j (.v x) (.v y)))
(defn fromph   [V] [(JS V)] [x : (Ph V)   y : (Ph V)]   : V
  (j (:: (.v x) V) (:: (.v y) V)))

(defn main [] : int
  (println (:: (fromreal (Real (:: 3 Sum)) (Real (:: 9 Sum))) int))        ;; 12  correct
  (println (:: (fromph   (:: (Ph 3) (Ph Sum)) (:: (Ph 9) (Ph Sum))) int))  ;; 9   WRONG
  0)
```

`12` then `9`. Both lines ask for `Sum`'s instance, which ADDS; the second gets
`int`'s, which takes a max.

## Mechanism -- visible in one grep of the emitted C

```c
static int64_t fromreal__spec__int64_t_tur_adt_Real__Sum_tur_adt_Real__Sum(...)
        __ps_286 = (__inst_JS_j_Sum((int64_t)(x).v, (int64_t)(y).v));   /* right */

static int64_t fromph(int64_t, int64_t);
        __ps_276 = (__inst_JS_j_int(x, y));                             /* wrong */
```

`fromreal` gets a `__spec__` specialization keyed on the argument type and
reaches `__inst_JS_j_Sum`. `fromph` gets **no `__spec__` suffix at all** -- it
is emitted once, at the carrier, and the constraint resolves to the
first/representative instance.

So the specialization key is derived from the *lowered argument types*. A
phantom parameter contributes nothing to them: `(Ph int)` and `(Ph Sum)` both
lower to the same `int64_t`, the two instantiations are indistinguishable at
that layer, and they collapse.

Both instances ARE emitted (`__inst_JS_j_int` and `__inst_JS_j_Sum`); only one
is ever called. That is the signature of this defect and a fast way to confirm
it: `grep -c '__inst_<Class>_<method>_'` shows two definitions, one call site.

## What is NOT the cause (each ruled out by measurement)

Narrowed by bisection from a much larger repro; every one of these is correct:

| Shape | Result |
| --- | --- |
| `(defn f [V] [(JS V)] [x : V y : V] : V ...)` -- type var direct | correct |
| type var through a parametric struct with a **typed field** | correct |
| value laundered through an int carrier (`(:: (:: x int) V)`) | correct |
| `defopaque` newtype over the same carrier, called directly | correct |
| the same newtype through a constrained generic | correct |
| **type var in a phantom position only** | **WRONG** |

In particular this is *not* the same-carrier-newtype collapse that
[turi-nested-class-method-call-picks-first-instance](turi-nested-class-method-call-picks-first-instance.md)
describes: that one is interpreter-only and is about nesting. This is the
compiled path, needs no nesting, and turns on phantomness alone.

## Fix direction

The specialization key has to include type arguments that do not appear in any
lowered parameter type. Either extend the key with the *declared* type
arguments of a parametric struct parameter (not just its lowered form), or
refuse the collapse: when a constrained generic's type variable is
unconstrained by the lowered signature, the dictionary must be passed rather
than resolved at emit time.

A diagnostic would be a defensible interim step -- "type parameter `V` is
phantom, so the `(JS V)` constraint cannot be resolved by specialization" is a
much better outcome than a wrong number, and the condition is cheap to detect.

## What it blocks

crdt-spice-plan section 2.3's `ORMap K V`, whose whole claim is that its join
recurses at the *value's* instance so "an `ORMap` of `PNCounter`s merges
correctly with no code specific to that pairing". An `ORMap` stores its entries
in a HAMT of int carriers, so `V` is necessarily phantom -- which is exactly
the broken case. The C3 implementation therefore takes the value-join as an
explicit parameter instead; see the plan's C3 note.

## Fixtures owed

- The repro above, asserting `12` twice.
- A sibling where the phantom parameter is instantiated at two *non*-newtype
  types, so a fix that works only for `defopaque` is not mistaken for a real one.
