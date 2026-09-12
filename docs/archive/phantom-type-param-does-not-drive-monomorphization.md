# A phantom type parameter does not drive monomorphization

**Severity: high** -- a **silent wrong answer**. A constrained generic whose
type variable appears only in a *phantom* position gets no specialization at
all, so every instantiation shares one body and dispatches to whichever
instance the representative resolves to. `tur check` is clean, cc is clean,
and the program prints a plausible number computed by the wrong instance.

**Status: RESOLVED** 2026-09-12, same day. Found executing
[crdt-spice-plan.md](../upcoming/crdt-spice-plan.md) C3, whose `ORMap` design
it blocked (see "What it blocked" -- now unblocked).

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

## Root cause -- narrower than "the specialization key"

The "Fix direction" first drafted here (extend the specialization key with
declared type arguments) was aimed at the wrong layer. The key was never
consulted, because **no specialization was requested at all**.

Traced by instrumenting every early return in `emit_abi_register_call`:
`fromph` exits at the `if (!abi_changes && !instance_changes)` gate with both
flags false. `instance_changes` comes from `body_has_dispatch_on_app_tyvar`,
which asks whether the call's receiver is a type variable bound to something
concrete -- and it does this:

```c
const Expr *recv = e->as.call_.args[0];
while (recv && recv->kind == EX_ASCRIBE)
    recv = recv->as.ascribe_.inner;          /* <-- discards the ascribed type */
if (recv && recv->type.kind == TY_TYVAR && ...)
```

`fromph`'s receiver is `(:: (.v x) V)`. Stripping the ascription leaves
`(.v x)`, whose declared type is `int` -- so the dispatch looked concrete. The
**ascription was the only thing that said `V`**, and it was thrown away before
the question was asked.

`fromreal`'s receiver is `(.v x)` where the field is declared `: V`, so its
type *is* a tyvar with no ascription involved. That is why the field-typed
sibling was correct all along and the defect looked like it was about
phantomness: phantomness is what forces the ascription.

## Fix

`src/compiler/emit_module.c`: keep the outermost ascribed type before
stripping, and use it for the tyvar test when it is one. Two lines of intent,
and the existing `match_bindings` / Gap H `__h<n>` machinery then does the rest
-- the specialization was always able to be minted, nothing was asking for it.

## Fixture

`tests/fixtures/typeclass-phantom-tyvar-dispatch` asserts both instantiations
of **both** shapes (ascribed-out-of-carrier and field-typed), so a fix that
repairs one path while breaking the other is caught. The interpreter was
verified correct on the same fixture, so it carries no `requires.compiled`
marker. Suite: 2963 passed, 0 failed.

## What it blocked

crdt-spice-plan section 2.3's `ORMap K V`, whose whole claim is that its join
recurses at the *value's* instance so "an `ORMap` of `PNCounter`s merges
correctly with no code specific to that pairing". An `ORMap` stores its entries
in a HAMT of int carriers, so `V` is necessarily phantom -- which is exactly
the broken case. The C3 implementation shipped with the value-join as an explicit parameter,
which was the right call at the time and remains correct and more general (two
ORMaps over one value type can merge differently). With this fixed, the
constrained form is viable -- verified on the original ORMap-shaped probe --
and migrating to it is now a design choice rather than a workaround.

## Fixtures owed

- The repro above, asserting `12` twice.
- A sibling where the phantom parameter is instantiated at two *non*-newtype
  types, so a fix that works only for `defopaque` is not mistaken for a real one.
