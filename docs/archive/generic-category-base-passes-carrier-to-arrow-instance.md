# A generic over `Category` emits a pointer/integer warning at the function arrow

> **RESOLVED 2026-09-26.** See [Resolution](#resolution) at the end. The
> analysis below is the original filing.

**Severity:** low -- the program prints the right answer, but the emitted C
carries `-Wint-conversion`, which `tests/run.sh`'s representation check
rejects, so a fixture exercising generic arrow code cannot pass. Found
2026-09-25 writing the SC8b step-4 fixture for typeclass-superclasses-plan;
reproduces on the compiler before that work with a plain `^Category`
constraint.

## Repro

```turmeric
(load "stdlib/arrow.tur")
(defn add1 [x : int] : int (+ x 1))
(defn twice [^Category A] [f : A] : A (comp f f))
(defn main [] : int (let [h (twice add1)] (println (h 5))) 0)
```

```
warning: passing argument 1 of '__inst_Category_comp_arrow' makes pointer
from integer without a cast [-Wint-conversion]
```

It prints `7`, which is right.

## Cause

Two clones of `twice` are emitted. The specialization the program calls,
`twice__spec__void___void__(void *f)`, is clean. The unspecialized base,
`static int64_t twice(int64_t f)`, dispatches `comp` to the representative
instance `__inst_Category_comp_arrow(void *, void *)` -- the function arrow's
`Category` instance, whose parameters are `void *` fat-closure handles -- and
passes its `int64_t` carrier arguments with no cast.

## Fix directions

Cast carrier arguments to the representative instance's parameter C types in
the base clone, the way the other carrier-to-instance crossings do, or skip
emitting a base clone that no call site reaches. Once fixed, a generic
`[^Arrow A]` fixture calling `comp` (Arrow entails Category) can be added to
cover the arrow superclasses' entailment, which today is covered only by the
obligation (`errors/stdlib-arrowchoice-requires-arrow`) and the general
`class-superclass-*` fixtures.

## Resolution

The forward straddle bridge at an ordinary call -- an argument that emits as
the int64 carrier passed to a parameter recorded as a concrete pointer --
skipped `void *` parameters. The function arrow's `Category` instance takes
`void *` fat-closure handles, and a generic's unspecialized base holds
`f : A` as the carrier, so its `comp` call passed an integer to a pointer.
A `void *` parameter is now bridged too, limited to a variable argument that
emits as the carrier (a literal 0 is a null pointer constant).

Pinned by `tests/fixtures/stdlib-arrow-generic-entails-category`: a
`[^Arrow A]` generic calls `comp` (Arrow entails Category), and a
`[^Category A]` one nests it, with a plain and a capturing function. It fails
the suite's representation check on the previous compiler. It loads
`stdlib/arrow.tur`, whose inline C keeps it out of `run-turi.sh`.
