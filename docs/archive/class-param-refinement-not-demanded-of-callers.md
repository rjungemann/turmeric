# A class parameter refinement is never demanded of callers (RESOLVED)

**Severity:** medium -- not a soundness hole. Nothing false is proved and the
runtime check is retained in every case (gate-off and gate-on abort
identically). What is lost is a compile-time error on a violation the class
signature documents, and the guide previously described that error as though it
always fired.

## Summary

For a typeclass method, the argument obligation raised at a call site comes
from the **resolved instance**, not from the **class**. Since an instance may
legally demand *less* than its class signature, and an unannotated instance
parameter demands nothing at all, the class's parameter predicate is enforced
only in the one case where an instance happens to restate it.

This is the mirror image of the result direction, which is fully enforced: an
instance either inherits the class result promise (and is checked against it)
or restates one that must imply it, and the class promise then propagates to
callers even at a dynamic site.

| direction | instance checked against class? | enforced on callers? |
|---|---|---|
| result | yes -- inherits or must imply (`TUR-E0374`) | yes -- propagates, even dynamically |
| parameter | yes, but only as an UPPER bound (`TUR-E0374` rejects demanding more) | **no** |

## Repro

All three cases below pass `0` where the class demands `(> v 0)`.

```turmeric
(defclass Scaler [a]
  (scale-by [self : a, k : #refine{ v : int | (> v 0) }] : int))
```

**1. Instance demands less (legal) -- static site, silent:**

```turmeric
(definstance Scaler [int]
  (scale-by [self : int, k : int] : int (* self k)))

(defn use-static [] : int (.scale-by 3 0))     ; no diagnostic
```

**2. Instance parameters unannotated -- silent, and no runtime check either:**

```turmeric
(definstance Scaler [int]
  (scale-by [self k] (* self k)))

(defn use-static [] : int (.scale-by 3 0))     ; compiles, runs, prints 0
```

Note this differs from results, where omitting the annotation *inherits* the
class promise and is checked.

**3. Genuinely dynamic dispatch -- silent even under `--strict-refine`:**

```turmeric
(definstance Scaler [int]                       ; weak
  (scale-by [self : int, k : int] : int (* self k)))
(definstance Scaler [float]                     ; restates the class demand
  (scale-by [self : float, k : #refine{ v : int | (> v 0) }] : int k))

(defn use-dynamic [^Scaler a x : a] : int (.scale-by x 0))

(defn main [] : int (println (use-dynamic 3.5)) 0)
```

`tur check --enable=refined --strict-refine` reports nothing. At runtime the
float instance is selected, its entry check fires, and the program aborts --
identically with the gate off. So the violation is real and reachable; it is
simply not reported at compile time.

Contrast: make the sole instance restate the predicate and the same call *is*
reported (`TUR-E0371`), which is what makes the obligation's source visible.

## Root cause

The call-site crossing is built against the resolved instance's parameter
predicates. When resolution yields no instance, no obligation is constructed;
when it yields an instance that demands less, the obligation that is
constructed is correspondingly weaker. The class signature is consulted only
for the variance check on the instance itself (`TUR-E0374`).

## Fix direction

Raise the obligation against the **class** parameter predicate at a class-method
call site, in addition to (not instead of) the instance's when one is resolved.

This is sound by the same variance argument that already licenses result
propagation, run in the other direction: `TUR-E0374` guarantees no instance
demands *more* than its class, so `class_pred` is the strongest demand true of
every instance, and an argument satisfying it is acceptable to whichever
instance runs. Verified that the E0374 half holds -- an instance demanding more
is rejected today.

One design question has to be answered first, and it is a behaviour change
rather than a bug fix:

> Is a class parameter refinement a **contract callers must honour**, or only an
> upper bound on what instances may demand?

Under the first reading, case 1 above becomes an error even though that program
runs correctly with its lenient instance -- the argument being that the caller
is programming against the class signature and a stricter instance added later
would break it. Under the second, only the dynamic case (3) should be reported,
since there the violation is genuinely reachable. Case 3 alone is the
conservative slice and produces no false positives; case 1 is the coherent one.
Worth deciding deliberately, because the first reading can reject existing code.

## Resolution

Both halves landed, and the design question was answered as **"only bounds
instances", plus a lint**.

**1. The inheritance asymmetry was a defect and is fixed.** An instance
parameter with no annotation now inherits the class's refinement, exactly as an
unannotated result inherits the class's promise. The inherited predicate is
published on the instance method's binding, which is what both the call-site
crossing and the entry-check injection read -- so case 2 above is now an error
statically AND guarded at run time. Writing an explicit annotation (including a
bare `: int`, which carries no predicate) is how an instance opts out and
demands less, which stays legal.

That shrank the design question considerably: once omission inherits, most
instances demand exactly the class predicate, and the two readings stop
differing for them.

**2. For what remained -- an instance that EXPLICITLY demands less -- the
resolved instance's predicate governs.** A statically-resolved dispatch knows
which implementation runs, so it is checked against that implementation's
contract, the more precise of the two. Case 1 above stays legal.

But it is linted (`TUR-W0377`): the leniency is not part of the interface, and
the same argument fails the moment dispatch goes dynamic or a stricter instance
appears. Only a DEFINITE violation warns -- an argument the class predicate
rejects outright, not one it merely cannot prove -- so an unconstrained
argument is silent.

Case 3 (dynamic dispatch) was fixed earlier by crossing into the class
signature; see the plan entry.

The alternative reading -- the class signature binding on callers, making case
1 an error -- was rejected because it turns correct programs into failures.
Under Liskov-style contract variance, a caller that statically knows the
implementation may use its weaker precondition; that is the same reasoning that
makes the instance the right contract here. The lint recovers the evolution
argument (a stricter instance added later would break the caller) without the
rejection.

## Coverage

- `tests/fixtures/refine-class-param-inheritance` -- inheritance, explicit
  opt-out, the lint firing once, and the lint NOT firing on an argument the
  class admits.
- `tests/fixtures/errors/refine-class-param-inherited-violated` -- the
  inherited demand is a hard error, not the lint.
- `tests/fixtures/refine-class-param-dynamic-dispatch` and
  `tests/fixtures/errors/refine-class-param-dynamic-violated` -- the dynamic
  half.

Negative controls checked by hand, all behaving: satisfying argument (no lint),
unknown argument (no lint), instance restating the class predicate (E0371, no
lint), class with no refinement at all (no lint), unannotated instance (E0371,
no lint).

## Demand

Measured, and it is low: repo-wide there are 13 `defclass` methods carrying a
parameter refinement, **all of them in fixtures and none in `stdlib/`**. 49
files use `^Class` constrained generics. The intersection -- a constrained
generic calling a refined class method, which is the shape case 3 needs -- is
**zero**.

## Found

While investigating "dynamic typeclass dispatch" as a next slice for
`docs/upcoming/v1/refinement-types-plan.md`. The plan and guide both described
this as specific to *dynamic* dispatch; the measurement above shows the static
case has the same hole whenever the instance does not restate the predicate,
which is the more common way to write an instance. Guide corrected in the same
change.
