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
have `max`/`min` either -- `Ord` (`stdlib/typeclass.tur:24`) declares only
`lt?` / `lte?` / `gt?` / `gte?`. The absent vocabulary is not an abstraction
nicety: it is the reason `effects-chain` in tur-signal hand-rolls a fold, and
the reason the CRDT plan has to define its own `ord-max` before it can write a
counter's join.

This plan adds that vocabulary. It is deliberately **small**: one new family
(`Semigroup` / `Monoid` / the four lattice classes), two default methods on an
existing class, and a set of selection newtypes. It does not restructure the
existing classes, and it does not propose superclasses.

**The headline finding is not the design -- it is that the design does not
compile today.** Probing the shapes turned up two defects, both filed:

- [nested-class-method-call-picks-the-first-instance](../reported/nested-class-method-call-picks-the-first-instance.md)
  (**high**, silent wrong answer). A nested class-method call inside a
  constrained generic resolves the outer call to the first declared instance.
  `combine : a -> a -> a` nests by construction, so a `Semigroup` family is
  **entirely** exposed to this. A float instance silently truncates.
- [nullary-class-method-unresolvable-over-newtype-tyvar](../reported/nullary-class-method-unresolvable-over-newtype-tyvar.md)
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

### 3.2 `max` / `min` on `Ord`, as defaults

The single highest-value, lowest-risk item in the plan, and the only one that
touches an existing class:

```turmeric
(defclass Ord [a]
  (lt?  [x y] : bool)
  (lte? [x y] : bool)
  (gt?  [x y] : bool)
  (gte? [x y] : bool)
  (max  [x : a y : a] : a (if (gte? x y) x y))
  (min  [x : a y : a] : a (if (gte? x y) y x)))
```

Zero blast radius (2.2), and it removes the `ord-max` stub the CRDT plan
currently has to define. Worth landing **on its own**, before any of the rest,
and worth landing even if the reader decides section 1's gate says "not yet"
for the family.

### 3.3 Selection newtypes -- blocked by defect 2

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

The `Monoid` half does not compile: `mempty` will not resolve against a
newtype type variable
([defect 2](../reported/nullary-class-method-unresolvable-over-newtype-tyvar.md)).
Until that is fixed the newtypes can ship `Semigroup` only, which is a
genuinely useful half -- `combine` is what folds need -- but it means no
`mconcat` over an empty vector, and that is exactly where an identity earns
its keep.

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

**Not** added to the autoload list initially -- `load`-on-demand, like
`typeclass.tur`. Autoloading is a per-TU cost in every build in the tree, and
nothing in stdlib will depend on these classes at first. Revisit only when a
stdlib module (not a spice) needs them without an import.

## 4. Phases

- **L0 -- unblock.** Fix
  [defect 1](../reported/nested-class-method-call-picks-the-first-instance.md)
  (nested call picks the first instance) and
  [defect 2](../reported/nullary-class-method-unresolvable-over-newtype-tyvar.md)
  (nullary method over a newtype tyvar). The static half of this -- ratcheting
  `-Wfloat-conversion` on emitted C, which flags defect 1's line with zero
  noise -- is **F0 of
  [type-confusion-detection-plan.md](type-confusion-detection-plan.md)** and is
  measured there (corpus sweeps clean at 0 across 2250 cc-invoking fixtures);
  do not duplicate it here. **Nothing below is worth starting first.**
- **L1 -- `Ord` gains `max`/`min`.** Independent of everything else, zero blast
  radius, removes a stub from the CRDT plan. Land it even if L2+ is deferred.
- **L2 -- `Semigroup` + `Monoid`** with primitive instances, the law functions,
  and the selection newtypes' `Semigroup` half.
- **L3 -- the lattice four** (`JoinSemilattice`, `MeetSemilattice`,
  `BoundedJoin`, `BoundedMeet`) with their laws. This is what
  [crdt-spice-plan.md](crdt-spice-plan.md) C1 consumes.
- **L4 -- retire a hand-rolled fold.** Rewrite tur-signal's `effects-chain` in
  terms of the vocabulary, or conclude it cannot be and record why. This is the
  phase that converts section 1's "latent call surface" into real call surface,
  and it is the honest test of whether any of this earned its place.

## 5. Risks and open questions

- **The gate may say no** (section 1). L1 and L0 survive that verdict; L2-L4
  do not. Do not smuggle the family in under the bug fixes.
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

- Defects: [nested-class-method-call-picks-the-first-instance](../reported/nested-class-method-call-picks-the-first-instance.md),
  [nullary-class-method-unresolvable-over-newtype-tyvar](../reported/nullary-class-method-unresolvable-over-newtype-tyvar.md),
  and the sibling
  [typeclass-method-resolution-ignores-the-class](../reported/typeclass-method-resolution-ignores-the-class.md).
- Consumer: [crdt-spice-plan.md](crdt-spice-plan.md).
- In-tree: `docs/guides/typeclass-guide.md` (default methods, associated types,
  constrained instances), `src/compiler/stdlib_autoload.c` (the autoload list).
