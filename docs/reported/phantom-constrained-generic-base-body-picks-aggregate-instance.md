# A constrained generic over a phantom type parameter cannot be written when the class has an aggregate instance

**Severity: medium-high** -- a hard **compile error** on a function that is
never called, with a message naming a type the function does not mention. Not a
wrong answer, but it makes a whole design shape unwritable.

**Status:** open. Found 2026-09-12 attempting the constrained `(ORMap V)`
instance from [crdt-spice-plan.md](../upcoming/crdt-spice-plan.md) section 2.3,
which it blocks.

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

## Fix direction

Do not emit a carrier base body for a constrained generic whose constrained
variable is phantom -- there is no correct representative, and every real call
site is specialized anyway. Emitting only the specializations would sidestep the
question. Failing that, choosing the representative from the **carrier-shaped**
instances rather than the first/arbitrary one would at least make the base body
valid C, though it would still be an arbitrary choice.

A diagnostic naming the real problem would beat TUR-E0295 here: the author's
mistake, if any, is the phantom parameter, not an aggregate they never mentioned.

## Related, and NOT the same

- [phantom-type-param-does-not-drive-monomorphization](../archive/phantom-type-param-does-not-drive-monomorphization.md)
  (fixed) -- dispatch through an ascription was not detected. That was a wrong
  answer; this is a compile error, and it survives that fix.
- The relay defect (fixed, same session) -- one constrained generic calling
  another lost the binding. Also a wrong answer, also distinct.

## Fixtures owed

- The repro above as an `errors/` fixture, plus its aggregate-free sibling as a
  passing one, so the pair pins exactly which ingredient is fatal.
