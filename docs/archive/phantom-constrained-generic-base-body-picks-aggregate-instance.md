# A constrained generic over a phantom type parameter cannot be written when the class has an aggregate instance

**Severity: medium-high** -- a hard **compile error** on a function that is
never called, with a message naming a type the function does not mention. Not a
wrong answer, but it makes a whole design shape unwritable.

**Status: RESOLVED** 2026-09-12. Found attempting the constrained `(ORMap V)`
instance from [crdt-spice-plan.md](../upcoming/crdt-spice-plan.md) section 2.3,
which it blocked. That instance now ships.

## Repro

```turmeric
(defclass JS [a] (j [x : a y : a] : a))
(defopaque G :int)
(definstance JS [G] (j [x y] x))

(defstruct Agg [p : int q : int])
(definstance JS [Agg] (j [x y] x))        ;; a BY-VALUE AGGREGATE instance

(defstruct P [V] [e : int])               ;; V is PHANTOM -- in no field
(defn f [V] [(JS V)] [w : (P V) x : int] : int (:: (j (:: x V) (:: x V)) int))

(defn main [] : int 0)                    ;; f is NEVER CALLED
```

```
error [TUR-E0295]: cannot reinterpret by-value aggregate 'Agg' as a one-word
carrier (:int / :ptr<void>) ...
```

Delete the `JS [Agg]` instance -- leaving only int-carrier instances -- and the
identical `f` compiles. `f` is never called either way, so this is purely about
emitting its **generic base body**.

## Mechanism

A constrained generic is emitted once as a base body at the carrier, plus a
specialization per instantiation. To emit the base, the phantom `V` has to be
given *some* type, so it resolves to a representative instance. When the
representative happens to be a by-value aggregate, `(:: x V)` becomes a
reinterpret of an aggregate as a one-word carrier -- which is exactly what
TUR-E0295 exists to reject.

The choice of representative is not the author's and not visible from the
function: **adding an unrelated instance elsewhere in the program breaks a
function that compiled yesterday**, and the error points at the new instance
rather than at anything the author wrote.

A non-phantom parameter is unaffected: when `V` appears in a field, each
instantiation has its own layout, and the base body is never asked to stand in
for an aggregate.

## Why it blocks the constrained ORMap

`crdt-spice-plan` 2.3 wants `(ORMap V)` to be a `JoinSemilattice` exactly when
`V` is. An ORMap stores entries in a HAMT of carriers, so `V` is necessarily
phantom -- and `JoinSemilattice` in that spice has several by-value aggregate
instances (`DotContext`, `GCounter`, `ORSet`). The fold helper therefore cannot
be written at all, independent of whether dispatch would specialize correctly
(it now does, after
[the relay fix](../archive/typeclass-constrained-relay-dispatch.md)).

`crdt/ormap` keeps its explicit `ormap-merge-with` parameter for this reason.

## Fix

The second option above, and it turned out to be a one-tier gap rather than a
design question.

The receiver-directed representative search in `elab_typeclasses.c` already had
two tiers -- an `int` instance, then a carrier-compatible **scalar**
(cstr/bool/sym/sized-int), with a comment correctly noting that "floats and
aggregates do not ride the carrier and would make the base clone ill-typed".
What it lacked was a tier for a **`defopaque` newtype over a non-pointer base**,
which is the int64 carrier just as much as `int` is. With none of its tiers
matching, the search fell through to a generic one that landed on the aggregate.

The return-directed twin at the top of the same file has had that tier since
`nullary-class-method-unresolvable-over-newtype-tyvar`; this is the
receiver-directed version, which never grew it.

That gap bit hard here because `JoinSemilattice`'s only carrier-shaped instances
ARE opaque newtypes (`Sum`, `Product`, `MinI`, `MaxI`) while its others are
aggregates -- so the class had a perfectly good representative available and the
search could not see it.

## Fixture

`tests/fixtures/typeclass-opaque-representative-vs-aggregate` -- an aggregate
instance in scope alongside two opaque-newtype instances, asserting that each
instantiation reaches its OWN instance rather than the representative.

## Related, and NOT the same

- [phantom-type-param-does-not-drive-monomorphization](../archive/phantom-type-param-does-not-drive-monomorphization.md)
  (fixed) -- dispatch through an ascription was not detected. That was a wrong
  answer; this is a compile error, and it survives that fix.
- The relay defect (fixed, same session) -- one constrained generic calling
  another lost the binding. Also a wrong answer, also distinct.

## Fixtures owed

- The repro above as an `errors/` fixture, plus its aggregate-free sibling as a
  passing one, so the pair pins exactly which ingredient is fatal.
