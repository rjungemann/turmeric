# An associated type cannot be a nullary method's only class mention, or named at a type variable

**Severity: medium** -- both halves are hard errors at `tur check`, so nothing
miscompiles. What they cost is the ability to write a class over an associated
type the way the guide's own shape suggests: one method spelling is
unreachable and generic code over such a class cannot name the projection, so
the class is usable only one concrete instance at a time.

**Status: OPEN.** Found 2026-09-29 implementing `DeltaCRDT` for
[crdt-spice-plan](../upcoming/crdt-spice-plan.md) C4, on `tur` v0.56.3
(`origin/main` at `dc95b2fdc`). Both are worked around in the spice and the
workarounds are recorded at the class definition in
`spices/crdt/src/crdt/lattice.tur`.

## Half 1 -- a nullary method returning only the associated type is unreachable

```turmeric
(defclass Box [a]
  (type Inner : Type)
  (empty [] : Inner)                 ;; declared fine
  (unwrap [x : a] : Inner))
(defopaque W :int)
(definstance Box [W] (type Inner = int) (empty [] 0) (unwrap [x] (:: x int)))
```

The class and the instance both compile. The method cannot be CALLED:

```turmeric
(empty)                              ;; error: unknown function or operator 'empty'
(let [z : (Inner W) (empty)] ...)    ;; error: unknown function or operator 'empty'
(:: (empty) int)                     ;; error: ascribed type does not match the result shape of 'empty'
```

Dispatch reads the first parameter's type, and `empty` has none -- the class
variable `a` appears nowhere in its signature, only `Inner` does. So there is
nothing to resolve the instance from, and the method is never registered.
Annotating the binding with the projection does not rescue it: the third
spelling proves the name IS known to the ascription path, so the two halves
disagree about whether the method exists.

`bottom` in `stdlib/typeclass-lattice.tur` is the same nullary shape and works
-- because its return type is the class variable itself, which the
return-directed path can resolve from an ascription. It is specifically the
**associated type in return position with no other mention** that has no path.

A two-parameter class with a functional dependency is not an alternative:

```turmeric
(defclass Box [a d] | (a -> d)
  (combine [p : d q : d] : d))
;; TUR-E0015: no instance of typeclass 'Box' for type 'D' (method '.combine')
```

Dispatch reads the FIRST class variable only, so a method mentioning only `d`
looks for an instance at the delta type and finds none. The fundep does not
run backwards.

**Workaround:** give the method a witness parameter of the class type that it
does not read -- `(empty [^borrow x : a] : Inner)`. That works, and it is what
`DeltaCRDT`'s `delta-bottom` and `delta-join` do. It is visible in the API and
a caller has to hold a value of the state type to ask for the empty delta,
which is exactly the wrong requirement for a relay that buffers deltas without
holding a state.

## Half 2 -- the projection is rejected at a type variable

```turmeric
(defn take-concrete [d : (Inner W)] : int d)                 ;; fine
(defn take-generic [A] [(Box A)] [x : A d : (Inner A)] : int ;; rejected
  (unwrap x))
```

```
error: no instance binding for associated type 'Inner' at this type
```

Projection at a CONCRETE type reduces as
[the guide](../guides/typeclass-guide.md) documents. At a type VARIABLE, inside a
generic constrained by the very class that declares the associated type, it is
refused -- which is the only place a projection is load-bearing, since a
concrete type could always have been spelled directly.

The consequence is that no generic function over the class can take or return
a `Delta`. C4 therefore ships no generic delta helpers at all: every caller of
`apply-delta` names a concrete state type, and a `(defn apply-all [A]
[(DeltaCRDT A)] [x : A ds : (Vec (Delta A))] : A ...)` cannot be written.

A generic that does NOT name the projection in its signature is also blocked,
for a third reason:

```turmeric
(defn neutral? [A] [(DeltaCRDT A) (Eq A)] [x : A y : A ^borrow w : A] : bool
  (eq? (apply-delta x (delta-bottom w)) y))
```

```
TUR-E0015: 'apply-delta' is a method of typeclass 'DeltaCRDT', but this
program declares no 'DeltaCRDT' instance at all
```

-- when the generic and the class live in a module that holds no instance
itself, which is the normal layout (the class beside the vocabulary, the
instances beside the types). The instances exist in the program; the error
fires anyway.

## Why the class still shipped

The witness parameter makes half 1 survivable, and half 2 is survivable
because a CRDT caller knows its concrete state type. The associated type is
still earning its keep: it is what lets `ORSet`'s delta be a different type
from `ORSet` (a dot-store fragment plus an exact `DotSet`) while `GCounter`'s
delta is a `GCounter`. Forcing `Delta = a` for every instance would have cost
the causal types their compression, and erasing the delta to bytes would have
cost the checking.

## Fix directions

Half 1: dispatch needs a path for "the class variable appears only through an
associated-type projection in the return", which is what the concrete
projection machinery already resolves at annotation sites. The ascription
error message ("does not match the result shape") suggests the ascription path
computes the projection and then compares against something that was never
built for a nullary method.

Half 2: the projection reducer bails when the argument is a `TY_TYVAR` rather
than deferring the reduction into the instantiation, which is what the
constrained-generic monomorphizer would later supply. This is the same place
the archived
[typeclass-associated-types-missing](../archive/typeclass-associated-types-missing.md)
work landed.

Fixtures worth having: a nullary associated-type method called through an
ascription; a constrained generic taking `(Assoc A)`; and the
class-in-one-module, instances-in-another layout, which is the shape every
spice will hit.
