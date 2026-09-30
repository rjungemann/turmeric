# Let-binding a phantom-parametric `:heap` value ICEs; without the `let` it compiles

**Severity: medium** -- an ICE (`repr-shadow binding let-bind ... want=heap-ptr
got=scalar-bits`), so nothing miscompiles, but the workaround is to delete a
`let` and inline the expression, which is the opposite of what readable code
wants and is not discoverable from the message.

**Status: OPEN.** Found 2026-09-29 building `crdt/ormap`'s delta support for
[crdt-spice-plan](../upcoming/crdt-spice-plan.md) C4, on `tur` v0.56.3
(`origin/main` at `dc95b2fdc`). Same defect FAMILY as the resolved
[bare-parametric-heap-base-repr-disagreement](../archive/bare-parametric-heap-base-repr-disagreement.md)
(also found in `crdt/ormap`, also a repr-shadow ICE) -- but a different
trigger and a different site: that one fired on a **merge temp** inside a
generic body, this one on an ordinary **`let` binding at a concrete call
site**, and that one's fix is in the tree.

## Repro

```turmeric
(defstruct Holder :heap [V] [payload : int])       ;; V is PHANTOM
(defn holder-new [V] [] : (Holder V) (Holder 0))
(defn holder-put [V] [^borrow h : (Holder V) v : V] : (Holder V) (Holder (:: v int)))
(defn holder-get [V] [^borrow h : (Holder V)] : int (.payload h))

(defn main [] : int
  (let [a (holder-put (:: (holder-new) (Holder int)) 7)]
    (println (holder-get a)))
  0)
```

```
tur: internal error (ICE): a representation decision disagrees with repr_of at binding.
  repr-shadow binding let-bind type=(Holder int) want=heap-ptr got=scalar-bits
  cty=int64_t own=int64_t
```

## Three controls, each isolating one ingredient

| variant | result |
| --- | --- |
| the repro above | **ICE** |
| ascribe the binding: `(let [a (:: (holder-put ...) (Holder int))] ...)` | **ICE** -- the ascription does not help |
| drop the `let`, consume the call inline | **compiles**, prints 7 |
| make `V` non-phantom (`[payload : V]`) | **compiles**, prints 7 |

So it needs **both** a `let` binding and a **phantom** type parameter. A
phantom-only parametric ADT has one layout for every instantiation, which is
what makes its base constructor real rather than an abort trap -- and it looks
like the binding site then takes the scalar-carrier decision that the erased
base would take, while `repr_of` correctly says heap-ptr for the `:heap`
struct.

The ascription control is the one worth noting: the usual remedy for a
carrier/pointer disagreement in this codebase is to pin the type at the
binding, and here it does nothing.

## The same family, seen as bad C rather than an ICE

`spices/crdt/tests/crdt/test_ormap.tur` (turmeric-spices) had been red since
`ORMap` became parametric, for what looks like the other face of this:
un-ascribed `let` bindings holding an `(ORMap int)` were declared `int64_t`
while the monomorphized constructor returns `tur_adt_ORMap__int *`, so cc
rejected the assignment:

```
error: incompatible pointer to integer conversion assigning to 'int64_t'
       from 'tur_adt_ORMap__int *'
```

`tur check` was clean. That one IS fixed by ascribing the binding (done in
turmeric-spices#76), and it only failed the build on clang >= 21, where
`-Wint-conversion` is an error -- so it was a macOS-only red that Linux
reported as a warning nobody read. Worth knowing that this family can surface
either way depending on which side wins.

## Also in the same family, opposite directions in one spice

Two more spellings from the same C4 work, both in `spices/crdt`, recorded
here because together they show the rule is not "always let-bind" or "always
inline":

- **Non-generic defn, `:heap` struct as a constructor argument:**
  `(ORSet (:: (map-new) int) (__ctx-of-elem ...))` fails (`-Wint-conversion`,
  `DotContext *` into an `int64_t` slot); **let-binding it first compiles.**
- **Generic defn, same shape:** `(let [rms (__rms-for-key ...)] (OrmapDelta
  ... rms))` fails (`int64_t` into a `DotSet *` slot); **inlining it into the
  constructor call compiles.**

Exactly opposite remedies for the same-looking code, separated only by
whether the enclosing `defn` is generic. Both are worked around at their
sites with comments saying which way and why.

## Fix directions

This is the representation-decision defect family that
[repr-decision-function-plan](../archive/repr-decision-function-plan.md)
exists to close, and the ICE message says so itself. The specific gap is the
`let-bind` decision site not consulting the same `repr_of` answer for a
`:heap` struct whose type parameters are all phantom -- the case where the
concrete monomorph and the erased base have the same layout, which is
precisely when it is tempting to treat the value as the carrier.

`TUR_REPR_NO_SHADOW_ICE=1` downgrades the ICE to a warning, which is how to
see what the rest of the pipeline then does with it.

Fixtures worth having: the four-row control table above, since each row
isolates one ingredient and three of them pass.

## Resolution (2026-09-30)

The trigger was narrower than "phantom". It was **a phantom parameter plus a
single concrete `:int` field**, which is exactly SC7's *transparent int
newtype* shape (`type_is_transparent_int_newtype`, `src/compiler/types.c`).
SC7 makes such a record its int64 payload everywhere: an identity
constructor, identity field access, `int64_t` as its C name. It never looked
at `:heap`. `repr_of` ranks `:heap` first and answers heap-ptr, so the
`let-bind` shadow saw `int64_t` against heap-ptr and ICE'd. A two-field
phantom `:heap` struct (`[payload : int extra : int]`) was never affected,
and neither is the non-phantom control, because neither is the newtype shape.

`:heap` now opts out of the collapse, in both arms of the predicate. The
reason is semantic, not only ICE-avoidance. `:heap` asks for reference
semantics, a node mutated through one handle and seen through every other,
and an int64 identity cannot give that. With the collapse, a
`(set! (.payload h) 42)` through a generic callee would have written a copy.
Because the one predicate steers every site SC7 touches (16 call sites), all
of them now agree with `repr_of`.

**A second defect in the same shape, found while measuring the controls.**
Dropping `:heap` from the repro, the genuine transparent newtype, compiled
and then **segfaulted** at scope exit. `call_returns_fresh_sum_box`
(`src/compiler/elab_fns.c`) treats every constructor application as a freshly
minted box. For a transparent newtype that constructor is an identity, so
RM1's scope drop emitted `tur_region_free((void *)7)`. A transparent-newtype
constructor is no longer fresh.

Pinned by:

- `tests/fixtures/phantom-heap-let-binding`: the report's control table
  (let, ascribed let, inline, non-phantom), plus a write through a generic
  callee seen through the caller's handle (`42`).
- `tests/fixtures/transparent-int-newtype-let-no-free`: the non-`:heap`
  shape, let-bound and read.

Both pass under `run.sh` and `run-turi.sh`, and the full suite stays green.
The crdt spice's opposite-remedy spellings recorded above are multi-field
structs. Those are a different path, not re-measured here.
