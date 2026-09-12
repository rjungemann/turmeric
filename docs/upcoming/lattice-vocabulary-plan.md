# Lattice vocabulary: `Semigroup`, `Monoid`, and the join/meet family

> **Status:** proposed (2026-09-11). **Track:** post-v1.
> **Type:** stdlib typeclasses, plus **two compiler defects that block it**.
> **Sequencing:** second of three.
> [type-confusion-detection-plan.md](type-confusion-detection-plan.md) ->
> this -> [crdt-spice-plan.md](crdt-spice-plan.md). The detection plan comes
> first because it ratchets the emitted-C check that catches defect 1's failure
> mode; this plan supplies the vocabulary the CRDT spice consumes.

## 0. Summary

Turmeric has no `Semigroup`, no `Monoid`, and no lattice classes. It does not
have a usable `max`/`min` either. (It had *macros* of those names -- see 3.2
for what they could and could not do, and what replaced them.) The absent
vocabulary is not an abstraction nicety: it is the reason `effects-chain` in
tur-signal hand-rolls a fold, and the reason the CRDT plan had to define its own
`ord-max` before it could write a counter's join.

This plan adds that vocabulary. It is deliberately **small**: one new family
(`Semigroup` / `Monoid` / the four lattice classes), two default methods on an
existing class, and a set of selection newtypes. It does not restructure the
existing classes, and it does not propose superclasses.

**The headline finding is not the design -- it is that the design does not
compile today.** Probing the shapes turned up two defects, both filed:

- [nested-class-method-call-picks-the-first-instance](../archive/nested-class-method-call-picks-the-first-instance.md)
  (**high**, silent wrong answer). A nested class-method call inside a
  constrained generic resolves the outer call to the first declared instance.
  `combine : a -> a -> a` nests by construction, so a `Semigroup` family is
  **entirely** exposed to this. A float instance silently truncates.
- [nullary-class-method-unresolvable-over-newtype-tyvar](../archive/nullary-class-method-unresolvable-over-newtype-tyvar.md)
  (**medium**, hard error). `mempty` will not resolve against a `defopaque`
  type variable, which removes the one idiom that lets `int` have more than one
  monoid.

Together they mean Phase L0 of this plan is *fix the compiler*, and nothing
else can usefully ship first.

## 1. Does this pass the gate?

The standing rule is that stdlib and typeclass growth is justified by
tur-signal's actual call surface, not by hierarchy completeness. This plan is
exactly the kind of thing that rule exists to slow down, so the honest answer
is worth writing out rather than assumed.

**What genuinely exists as call surface:**

- `spices/signal/src/signal/compose.tur`'s `effects-chain` applies a `Vec` of
  signal functions left-to-right through a hand-written `__chain-loop`
  recursion. That is `mconcat` over the endomorphism monoid, written out by
  hand because there is no name for it.
- `stdlib/arrow.tur:485` already declares `Category` with `ident` and `comp`.
  `comp` **is** the semigroup operation and `ident` **is** the identity; the
  structure is in the tree under a different name, for one specific shape.
- `spices/signal/src/signal/shaper.tur`'s `mix` / `add` / `multiply` are
  semigroup operations on signals.
- The CRDT plan needs the lattice half and cannot proceed without it.

**What this honestly is not:** none of the above is *blocked*. tur-signal ships
today. This is latent call surface -- code that re-implements the structure --
not code that is stuck. A reasonable reading of the gate says "not yet."

The argument for doing it anyway is the bug report: the two defects above are
real, they are in the typeclass machinery, and they were found only because
someone tried to write a class whose method returns its own class type. Almost
every existing stdlib class returns `bool` (`Eq`, `Ord`), `cstr`/`String`
(`Display`, `Show`, `Debug`), or `int` (`Hash`) -- shapes that cannot nest and
therefore cannot trip defect 1. **The vocabulary's real value right now may be
as the thing that exercises the typeclass system honestly.** That is a defensible
reason to do it, and a different one from completeness.

Decision for the reader, not for this document: if the answer is "not yet,"
**file the two bugs anyway and stop there** -- they are already filed, and they
stand on their own.

## 2. What exists today

### 2.1 The two-tier typeclass layout is deliberate

`src/compiler/stdlib_autoload.c` auto-loads a fixed list into **both**
single-file and project-mode builds. Eight of the `typeclass-*.tur` files are
on it:

```
typeclass-eq, typeclass-functor, typeclass-clone, typeclass-drop,
typeclass-hash, typeclass-applicative, typeclass-alternative,
typeclass-monad, typeclass-monaderror, typeclass-bifunctor
```

`typeclass.tur` -- the monolith carrying `Ord`, `Num`, `Foldable`,
`Traversable`, `Display`, `Debug`, `Error`, `From`, `Into` -- is **not** on the
list; it is `load`ed on demand. The overlap between the two is intentional:
`defclass` redefinition is idempotent, and the split files exist so
`stdlib/list.tur` and friends can declare instances without dragging in the
monolith.

Consequence for this plan: a new class is available everywhere with no import
only if it joins the autoload list, and every addition there is paid by every
TU in every build. That is the cost to weigh, and it argues for **one** new
file, not six.

### 2.2 Default methods work, and are the cheapest lever available

Verified against v0.46.1:

```turmeric
(defclass Ord3 [a]
  (lt3? [x y] : bool)
  (max3 [x : a y : a] : a (if (lt3? x y) y x))
  (min3 [x : a y : a] : a (if (lt3? x y) x y)))

(definstance Ord3 [int]   (lt3? [x y] (< x y)))
(definstance Ord3 [float] (lt3? [x y] (< x y)))
```

`(max3 2.5 7.1)` answers `7.1`; both instances implement only `lt3?`. So
`max`/`min` can be added to the real `Ord` **without touching any of its 13
existing `definstance` blocks**.

One trap, worth writing down because it cost a probe: the default's parameters
must be **annotated** as the class variable. Writing `(max3 [x y] : a ...)` --
matching the style of `Ord`'s existing `bool`-returning methods, which never
needed it -- fails at the call site with:

```
error: cannot infer type for return-directed method 'max3'; add a type
ascription, e.g. (:: (max3 ...) T)
```

### 2.3 No superclasses, no constraint aliases

`defclass` has no `extends`; nothing in `src/compiler/` implements superclass
constraints and the typeclass guide has no section for them. `defalias`
(`elab_call.c:3224`) exists but is a **type** alias (TA1/TA2), not a constraint
alias.

So a function needing two classes lists two constraints. Verified working:

```turmeric
(defn join3 [^JoinSemilattice A ^BoundedJoinSemilattice A] [x : A y : A z : A] : A
  (join (join x y) (join z (bottom))))
```

This compiles and runs. It is also the exact shape that triggers defect 1 once
a second instance exists, which is how the bug was found.

One ordering constraint this plan was written under is **gone as of
2026-09-11**: a `definstance` no longer has to appear above the code that
dispatches on it
([typeclass-method-resolution-ignores-the-class](../archive/typeclass-method-resolution-ignores-the-class.md),
resolved -- a defn that cannot resolve a class method is elaborated
speculatively and retried once its unit is fully processed). So
`stdlib/typeclass-lattice.tur` can order its classes, newtypes, instances and
law functions for readability rather than to satisfy the elaborator.

## 3. Design

### 3.1 The family

Six classes, one file, all flat:

```turmeric
;;; An associative binary operation.
(defclass Semigroup [a] (combine [x : a y : a] : a))

;;; `combine`'s identity element.
(defclass Monoid [a] (mempty [] : a))

;;; Associative, commutative, idempotent. The CRDT join.
(defclass JoinSemilattice [a] (join [x : a y : a] : a))
(defclass MeetSemilattice [a] (meet [x : a y : a] : a))

;;; Least and greatest elements.
(defclass BoundedJoin [a] (bottom [] : a))
(defclass BoundedMeet [a] (top    [] : a))
```

`JoinSemilattice` is not a newtype of `Semigroup` with extra prose: the two
differ only in their **laws**, and the laws are the whole content. That is
uncomfortable in a system with no law checking, and section 3.4 is the answer.

Deliberately **excluded**: `Group` (no caller), `Lattice` as a combined class
(absorption laws with nothing to check them), and anything indexed by a higher
kind (`Foldable`-style `fold-map`) until the flat case is proven.

### 3.2 `max` / `min` over `Ord` -- DONE 2026-09-11, but not as written

**Two premises in this section were wrong, and the design changed twice.**

**`max`/`min` already existed**, as macros in `stdlib/macros.tur`, which *is*
auto-loaded (indented inside its `defmodule`, which is why the greps behind
section 0 missed them):

```turmeric
(defmacro min [a b] (if (<= a b) a b))
(defmacro max [a b] (if (>= a b) a b))
```

They expanded to the builtin `>=` / `<=`, so they worked for concrete `int` and
`float` and nowhere else: `(max "a" "b")` was `operator lookup failed for '>='
... cstr`, and inside a constrained generic it was the same failure at
`type tyvar` -- the case a `max` earns its keep. Macro expansion precedes
typeclass dispatch, so a macro named `max` makes any method of that name
unreachable: the two spellings could not coexist. **The macros are retired**
(their old site carries a note). This is BREAKING -- `max`/`min` now require
`(load "stdlib/typeclass.tur")`. Exactly one genuine call site existed
tree-wide, in `spices/stats`, and it was inlined to what the macro expanded to.

**`Ord` did not cover the primitives.** It had `int`, the sized ints, `float32`,
`Rational`, `String`, `StringSlice`, `Bound` -- but **not `float` and not
`cstr`**, so `(gte? 2.5 7.1)` had no instance at all. Retiring the macro
without adding those two would have been a net loss. Both are added, `cstr`
over a local `strcmp` helper (`typeclass.tur` cannot load `cstr.tur` without
adding an edge to the Show/string load cycle).

**They are constrained generic DEFNS, not defaulted methods:**

```turmeric
(defn max [^Ord A] [x : A y : A] : A (if (gte? x y) x y))
(defn min [^Ord A] [x : A y : A] : A (if (gte? x y) y x))
```

The defaulted-method design in this section's original text was implemented
first and is **miscompiled**: the class's method form is spliced into each
instance with its `: a` annotations intact and elaborated literally, so
`Ord [float]`'s copy emits as `int64_t __inst_Ord_max_float(int64_t, int64_t)`
and converts its own arguments -- `(generic-max 2.5 7.1)` answered 2. Dropping
the annotations is not an option either (section 2.2's trap). Filed as
[default-method-spliced-at-carrier-type](../reported/default-method-spliced-at-carrier-type.md).

The defn form is better regardless: it is correct at every instance, works
inside another generic, and costs a user-written `Ord` instance nothing, where
a method -- defaulted or not -- is one more thing every instance must satisfy.

**How it was caught matters.** The truncation was found by the
`-Wfloat-conversion` ratchet from
[type-confusion-detection-plan.md](type-confusion-detection-plan.md) F0, not by
the fixture's stdout diff: the concrete `(max 2.5 7.1)` was correct and only
the call through a generic was wrong, so the obvious assertion passed. That
ratchet was built two steps earlier in this same chain.

Pinned by `tests/fixtures/ord-max-min`.

### 3.3 Selection newtypes

`int` is a monoid four different ways, and only a wrapper can choose:

```turmeric
(defopaque Sum     :int)   (defopaque Product :int)
(defopaque MinI    :int)   (defopaque MaxI    :int)
(defopaque Any     :bool)  (defopaque All     :bool)
```

Construction is by ascription, not a constructor call: `(:: 3 Sum)`, and
`(:: x int)` to read the carrier back. **The `Semigroup` half of this is
verified working** -- two newtypes over the same carrier, each selecting its own
instance through a constrained generic, answering `10` and `7` respectively.

The `Monoid` half works too as of 2026-09-11
([defect 2](../archive/nullary-class-method-unresolvable-over-newtype-tyvar.md),
resolved): `(fold2 (:: 3 Sum) (:: 7 Sum))` answers 10 and the `Product` pair
answers 21, each selecting its own instance through one constrained generic.

One shape stays refused, by design: a generic whose class tyvar reaches no
**parameter**. Specializations split on argument types -- `Sum` and `Product`
are distinct types even though both render `int64_t` -- so a generic with no
`A`-typed argument interns a single spec for every instantiation and a
representative chosen for it would be baked in silently. **`mconcat` over an
empty container is exactly that shape**, so it needs an explicit witness
argument (or an ascribed element type) rather than relying on return-only
inference. Worth knowing before L2 writes it.

### 3.4 Laws

The classes differ only by laws, so the laws ship with them, as ordinary
constrained functions in the same file:

```turmeric
(defn law-associative? [^Semigroup A ^Eq A] [x : A y : A z : A] : bool
  (eq? (combine (combine x y) z) (combine x (combine y z))))

(defn law-commutative? [^JoinSemilattice A ^Eq A] [x : A y : A] : bool
  (eq? (join x y) (join y x)))

(defn law-idempotent?  [^JoinSemilattice A ^Eq A] [x : A] : bool
  (eq? (join x x) x))

(defn law-identity?    [^Semigroup A ^Monoid A ^Eq A] [x : A] : bool
  (and (eq? (combine (mempty) x) x) (eq? (combine x (mempty)) x)))
```

These are not decoration. `law-associative?` is *literally the shape that
triggers defect 1* -- a nested `combine` inside a constrained generic -- so the
law suite is also the regression suite for the bug that blocks the plan. Ship
the laws with the classes, not after.

Note `law-associative?` cannot be written for `JoinSemilattice` without
restating it, since there is no superclass relating `join` to `combine`. Two
copies is the honest cost of 2.3; a constraint-alias feature would remove it,
and is worth *considering* only if this duplication is felt more than twice.

### 3.5 File layout

One new file, `stdlib/typeclass-lattice.tur`, carrying all six classes, the
selection newtypes, the law functions, and instances for the primitives
(`int`, `float`, `bool`, `cstr` where meaningful).

**SETTLED 2026-09-11: ship NO `Semigroup`/`Monoid` instance for a bare
primitive.** This was an open question when the plan was written; the language
decision it depended on has since landed. A colliding `definstance` from
outside `stdlib/` is now a hard `TUR-E0373` error, not a silent drop and not a
warning
([duplicate-instance-silently-drops-a-user-definstance](../archive/duplicate-instance-silently-drops-a-user-definstance.md),
resolved: reject, not replace).

That makes the constraint concrete rather than advisory. If this file ships
`Semigroup [int]`, a user who wants product instead of sum cannot have it --
their instance is now an **error**, where before it was merely ignored. For
`Eq [int]` that is correct and desirable; there is one right answer. For
`Semigroup [int]` there are four, which is the entire reason 3.3 exists.

So: the six classes ship with **no bare-primitive instances at all**, and the
selection newtypes (`Sum`, `Product`, `MinI`, `MaxI`, `Any`, `All`) carry every
one of them. `Semigroup [int]` stays free for a user to define unopposed. The
lattice four may still instance at `bool` (`&&`/`||` under `Any`/`All` are the
newtypes' business, but `JoinSemilattice [bool]` has a genuinely canonical
reading) -- decide each on whether the algebra is unique, not by default.

`Ord`'s `max`/`min` (3.2) are unaffected: they are defaults on an existing
class, not a new instance, so nothing can collide with them.

Reject also removed a hazard this section previously carried. Under the old
first-wins rule, "first" meant *load order* for a load-on-demand module: the
same two files gave `7` (stdlib sum) or `12` (user product) depending on which
was elaborated first. A hard error cannot be order-dependent.

**Not** added to the autoload list initially -- `load`-on-demand, like
`typeclass.tur`. Autoloading is a per-TU cost in every build in the tree, and
nothing in stdlib will depend on these classes at first. Revisit only when a
stdlib module (not a spice) needs them without an import.

## 4. Phases

- **L0 -- DONE 2026-09-11.**
  [Defect 1](../archive/nested-class-method-call-picks-the-first-instance.md)
  (nested call picks the first instance) is **FIXED and archived** -- it was
  `emit_reresolve_disp_type` refusing to look through a receiver that is itself
  a re-resolved class-method call. `combine`/`join`/`meet` nest safely now, so
  the law functions in 3.4 and the lattice four in L3 are unblocked.
  [Defect 2](../archive/nullary-class-method-unresolvable-over-newtype-tyvar.md)
  (nullary method over a newtype tyvar) is **FIXED and archived** -- the
  return-directed representative search accepts a carrier-compatible opaque
  newtype, gated on the class tyvar reaching a parameter (specs split on
  argument types, so that is exactly the condition under which the choice can
  be re-resolved). `mempty` over `Sum`/`Product` works through a constrained
  generic; the return-only shape keeps its hard error and is pinned by
  `errors/typeclass-nullary-return-only-newtype`.
  The static half of this -- ratcheting
  `-Wfloat-conversion` on emitted C, which flags defect 1's line with zero
  noise -- is **F0 of
  [type-confusion-detection-plan.md](type-confusion-detection-plan.md)** and is
  measured there (corpus sweeps clean at 0 across 2250 cc-invoking fixtures);
  do not duplicate it here. **Nothing below is worth starting first.**
- **L1 -- DONE 2026-09-11.** `max`/`min` are constrained generics over `Ord`
  (not defaulted methods -- see 3.2), the auto-loaded macros they replace are
  retired, and `Ord [float]` / `Ord [cstr]` are added. Removes the `ord-max`
  stub from the CRDT plan. Suite: 2951 passed, 0 failed.
- **L2 -- DONE 2026-09-11.** `stdlib/typeclass-lattice.tur`: `Semigroup`,
  `Monoid`, the six selection newtypes (`Sum` `Product` `MinI` `MaxI` `Any`
  `All`) with their `Eq`/`Semigroup`/`Monoid` instances, and
  `law-associative?` / `law-identity?`. No bare-primitive instances (3.5); not
  auto-loaded. Pinned by `tests/fixtures/typeclass-lattice-semigroup-monoid`,
  which carries deliberately non-associative and wrong-identity instances so
  the laws are shown to DISCRIMINATE -- section 5's requirement.
  Two things learned writing it:
  - **`(combine (mempty) x)` does not elaborate.** Argument inference runs
    left-to-right, so in first position `mempty` has nothing to fix its type
    and the call is "cannot infer type for return-directed method". Second
    position is fine; the laws bind `(let [unit : A (mempty)] ...)`, which also
    reads better.
  - **The MinI/MaxI identities are int64 literals**, not `<stdint.h>`
    constants. The file is deliberately inline-C free: `run-turi.sh`
    PASS-skips any fixture whose program contains a user inline-C block, so one
    block here would cost interpreter coverage for everything that loads it.
- **L3 -- DONE 2026-09-11.** The lattice four (`JoinSemilattice`,
  `MeetSemilattice`, `BoundedJoin`, `BoundedMeet`) in the same file, with
  `law-join-associative?` / `law-join-commutative?` / `law-join-idempotent?` /
  `law-bottom-identity?` / `law-meet-idempotent?` / `law-top-identity?` and
  `lattice-leq?` (the order that comes free from the operation). Each newtype
  carries the one lattice its name commits it to -- `MaxI` join, `MinI` meet,
  `Any` join, `All` meet -- so nothing picks an algebra arbitrarily; `MaxI` is
  deliberately given no `meet`. Pinned by
  `tests/fixtures/typeclass-lattice-join-meet`, whose `BadJoin` is associative
  and commutative but NOT idempotent and must fail exactly one law. This is
  what [crdt-spice-plan.md](crdt-spice-plan.md) C1 consumes.

  **Interpreter caveat.** Both L2/L3 fixtures carry `requires.compiled`: the
  law functions nest a class-method call inside a constrained generic, which
  `--interpret` resolves to the wrong instance, so a law that must answer
  `false` answers `true`. Compiled is correct. This raised
  [turi-nested-class-method-call-picks-first-instance](../reported/turi-nested-class-method-call-picks-first-instance.md)
  from medium to **high** -- over newtypes it is a silent wrong answer, not the
  crash its original repro produced. The markers name it, and the assertions
  are already written.
- **L4 -- INVESTIGATED 2026-09-11. Verdict: yes, it is expressible -- and the
  shipped function should NOT be rewritten anyway.**

  **It works.** `effects-chain` threads a signal through a `Vec` of signal
  functions, and a signal function is an endomorphism on Signal, so the loop is
  a fold over the endomorphism monoid. Built end to end on the real shape --
  the carrier-spelled `(fn [ptr<void>] ptr<void>)` the fat-dispatch ABI forces,
  a `Vec` of them, closures and affine values included -- and it answers
  correctly. So the vocabulary does reach the case section 1 offered as its
  justification. `mconcat` and `mconcat-from` are in
  `stdlib/typeclass-lattice.tur` as a result; they were the missing piece, and
  the empty-`Vec` case (where the identity finally earns its keep) resolves
  because `(Vec A)` carries the class variable into a parameter.

  **But rewriting `effects-chain` itself would make it worse, not better.**
  `definstance` heads must be plain type names, so a bare
  `(fn [ptr<void>] ptr<void>)` cannot carry a `Semigroup` instance -- the
  endomorphism needs a `defstruct` wrapper. `effects-chain`'s public signature
  takes an untyped `Vec` of raw SF carriers, so using `mconcat` means wrapping
  every element first. That is a pass added, not a recursion removed, for no
  behavioural gain. The win here is conceptual (composition separated from
  application), and it belongs in code written fresh against the vocabulary --
  the CRDT spice -- not retrofitted into a shipped module whose signature
  predates it.

  **A separate, real finding about that module.**
  `spices/signal/src/signal/compose.tur` hand-writes `__vec-get-i` and
  `__vec-len-i` in inline C, justified by a comment saying "Project-mode
  compilation auto-loads only stdlib/macros.tur, so stdlib/vec.tur's `vec-get`
  is not in scope here." That is **stale**: `src/compiler/stdlib_autoload.c`'s
  list is shared by single-file and project mode and has carried `vec.tur` for
  some time, and `spices/plot` and `spices/linalg` both call stdlib `vec-get`
  in project mode today. Those two helpers -- and the interpreter coverage
  their inline C costs every fixture that loads the module -- can go. That is a
  worthwhile change to make to `signal/compose.tur`; swapping its fold for
  `mconcat` is not.

  So section 1's "latent call surface" stays latent, honestly: the vocabulary
  is demonstrably able to express it, and the existing caller is not improved
  by adopting it. The case for the vocabulary rests on new code (the CRDT
  spice) and on what building it exposed -- four compiler defects, three of
  them silent wrong answers.

## 5. Risks and open questions

- **The gate may say no** (section 1). L1 and L0 survive that verdict; L2-L4
  do not. Do not smuggle the family in under the bug fixes.
- ~~**A stdlib instance cannot be overridden, and which one wins depends on
  load order.**~~ **Settled 2026-09-11** (3.5): a colliding instance is now
  `TUR-E0373`, so the order-dependence is gone -- and L2 ships no
  bare-primitive `Semigroup`/`Monoid` instance, leaving those types to the
  selection newtypes.
- **`Eq` must be structural.** Every law function is an `eq?` call. A law suite
  wired to pointer identity passes on everything. Pin it with a
  deliberately-failing instance in L2.
- **Defect 1 may not be cheap.** The sibling report
  (`typeclass-method-resolution-ignores-the-class`) already measured its
  structural fix for symptom A as "not cheap." If defect 1 lands in the same
  machinery, L0 could be a large piece of work -- in which case the detection
  plan's F0 (cheap, independent, catches the class) is the part to land
  regardless.
- **Autoload pressure.** If L3 makes the lattice classes feel like they belong
  everywhere, the temptation will be to autoload them. That cost is paid by
  every TU in every build; measure before adding.
- **No higher-kinded members yet.** `fold-map` over `Foldable` is the obvious
  next thing and is deliberately out of scope; `Foldable` is currently declared
  twice (`stdlib/typeclass.tur:317`, `stdlib/rc.tur:80`), which is its own
  question and should not be entangled with this one.

## 6. References

- Defects: [nested-class-method-call-picks-the-first-instance](../archive/nested-class-method-call-picks-the-first-instance.md),
  [nullary-class-method-unresolvable-over-newtype-tyvar](../archive/nullary-class-method-unresolvable-over-newtype-tyvar.md),
  and the sibling
  [typeclass-method-resolution-ignores-the-class](../archive/typeclass-method-resolution-ignores-the-class.md).
- Consumer: [crdt-spice-plan.md](crdt-spice-plan.md).
- In-tree: `docs/guides/typeclass-guide.md` (default methods, associated types,
  constrained instances), `src/compiler/stdlib_autoload.c` (the autoload list).
