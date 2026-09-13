# A constrained generic that dispatches only indirectly collapses to one instance

**Severity: high** -- a **silent wrong answer**. Two instantiations of one
generic produced one answer, computed by whichever instance happened to be
declared last.

**Status: RESOLVED** 2026-09-12. Found while checking whether
[crdt-spice-plan.md](../upcoming/crdt-spice-plan.md) section 2.3's constrained
`ORMap` instance was viable after the phantom-dispatch fix earlier the same
day. It was not, for this separate reason.

## Repro

```turmeric
(defclass JS [a] (j [x : a y : a] : a))
(defopaque Gmax :int)
(definstance JS [Gmax] (j [x y] (if (< (:: x int) (:: y int)) y x)))   ;; max
(defopaque Gsum :int)
(definstance JS [Gsum] (j [x y] (:: (+ (:: x int) (:: y int)) Gsum)))  ;; sum

(defstruct M [V] [e : int])

(defn direct [V] [(JS V)] [a : (M V) b : (M V)] : int
  (:: (j (:: (.e a) V) (:: (.e b) V)) int))

(defn relay  [V] [(JS V)] [a : (M V) b : (M V)] : int (direct a b))
```

```
(direct ... Gmax) => 9    (direct ... Gsum) => 12     correct
(relay  ... Gmax) => 12   (relay  ... Gsum) => 12     WRONG
```

The same function is right or wrong depending on whether a constrained caller
sits in between, which is what made it hard to see.

## Mechanism

`relay` has no class-method call of its own -- it only calls another
constrained generic. `body_has_dispatch_on_app_tyvar`, which decides
`instance_changes` and therefore whether to mint a specialization, asks only
about **this** body. It answered no, so `relay` was emitted once and called the
**base** `direct`, which bakes the representative (last-declared) instance.

Visible in the emitted C: `direct` has two correct specializations
(`__spec__` -> `__inst_JS_j_Gmax`, `__h1` -> `__inst_JS_j_Gsum`), `relay` has
none, and base `direct` calls `__inst_JS_j_Gsum`.

## Fix

`src/compiler/emit_module.c`: when the body calls another constrained generic,
ask the callee the same question, with this specialization's concrete bindings
substituted into the type arguments the call passes it. Guarded by a depth cap
(mutual recursion would not terminate) and by requiring at least one type
argument to have actually become concrete -- a relay that stays abstract cannot
select an instance and must not mint a spec.

That last condition is what keeps it from over-minting: **the full fixture
suite regenerated zero codegen snapshot changes**, so the new probe fires only
where dispatch was genuinely being lost.

## Fixture

`tests/fixtures/typeclass-constrained-relay-dispatch` covers the direct call
(the control, correct before the fix), one hop, and **two** hops, so a fix that
only looks one level deep is caught. The interpreter is correct on it too, so
it carries no `requires.compiled` marker. Suite: 2963 passed, 0 failed.

## What it did NOT unblock

The constrained `(ORMap V)` instance is still unwritable, for an unrelated
reason recorded in
[phantom-constrained-generic-base-body-picks-aggregate-instance](../reported/phantom-constrained-generic-base-body-picks-aggregate-instance.md):
the generic base body resolves the phantom variable to a representative
instance, and `JoinSemilattice` has by-value aggregate instances, so the base
body is invalid C before specialization is even considered.
