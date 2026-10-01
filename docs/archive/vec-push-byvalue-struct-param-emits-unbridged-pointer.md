# `vec-push!` of a by-value struct PARAMETER emits an unbridged pointer

**Severity: medium** -- a compile failure, not a wrong answer: `tur check` is
clean, `emit-c` succeeds, and cc rejects the emitted C. Loud, but it makes an
obvious API shape (`(defn push! [v : (Vec T) x : T] ...)`) unwritable for any
struct wider than two words, with no diagnostic pointing at the cause.

**Status: OPEN.** Found 2026-09-29 building `crdt/rga` for
[crdt-spice-plan](../upcoming/crdt-spice-plan.md) C5, on `tur` v0.56.3
(`origin/main` at `dc95b2fdc`). Worked around in the spice by passing the
struct's FIELDS and constructing at the push site, which is noted at
`__ins-at!` in `spices/crdt/src/crdt/rga.tur`.

## Repro

Three `int` fields is the smallest failing case:

```turmeric
(defstruct Node [a : int b : int c : int])
(defn push-it! [v : (Vec Node) x : Node] : int (vec-push! v x) 0)
(defn main [] : int
  (let [v (:: (vec-of) (Vec Node))]
    (push-it! v (Node 1 2 3))
    (println (.a (vec-get-byval v 0)))
    0))
```

```
error: assigning to 'tur_adt_Node' (aka 'struct tur_adt_Node') from
       incompatible type 'const tur_adt_Node *' (aka 'const struct tur_adt_Node *')
  *__t182 = x;
          ^ ~
error: indirection requires pointer operand ('int64_t' invalid)
  vec_hypush_ex(..., (*((int64_t)(intptr_t)(__t182))));
```

Two errors, one cause: the parameter `x` arrives as a `const tur_adt_Node *`
(the by-value ABI passes a wide aggregate by pointer), the carrier bridge
allocates a `tur_adt_Node` temp and assigns `x` to it without a dereference,
and then dereferences the temp's ADDRESS as if it were a pointer.

## What separates it, measured

| shape | result |
| --- | --- |
| `[a : int b : int]` -- two words | **compiles**, prints 1 |
| `[a : int b : int c : int]` -- three words | fails as above |
| `[ctr : int rep : Sym]` -- two words | **compiles** |
| `[ctr : int rep : Sym val : int dead : bool]` | fails as above |
| the same struct built AT the push site, not passed in | **compiles** |

So the trigger is (a) the value is a **parameter**, not a locally constructed
expression, and (b) the struct is **wider than two words**, which is where the
by-value ABI switches from passing in registers to passing a pointer. Field
types are irrelevant; word count is not.

The workaround is to pass the fields separately and construct inside the
callee, which is what `crdt/rga` does -- at the cost of an eight-parameter
internal helper for a six-field node.

## Not a duplicate of

- [generic-loop-vec-push-byvalue-struct-element-from-result](../archive/generic-loop-vec-push-byvalue-struct-element-from-result.md)
  (resolved 2026-06-22) -- that needed a constrained generic and a
  Result-decoded element; this has no generic, no typeclass and no Result.
- [vec-push-byvalue-aggregate-escapes-frame-regression](../archive/vec-push-byvalue-aggregate-escapes-frame-regression.md)
  (resolved 2026-07-23) -- that was a dangling pointer at RUNTIME after a
  successful compile; this never compiles.

Both of those are about the carrier bridge choosing the wrong promotion. This
one is the bridge not dereferencing an argument it was handed by pointer, and
it is a much simpler shape than either.

## Fix directions

The bridge that spills a by-value aggregate argument into a temp before the
`vec_hypush_ex` carrier call needs to know whether the source expression is
already an lvalue-by-pointer (a wide by-value parameter) or a value. The
`*__t = x` / `*(&__t)` pair reads like the two halves were written for the
value case and the parameter case was never routed through. `emit_expr.c`'s
`expr_emits_byvalue_carrier_abi` / `emit_carrier_bridge_*` seam is where the
two prior reports were fixed and is the place to look.

A fixture belongs next to the existing ones: a non-generic three-word
`defstruct` pushed from a parameter, plus the two-word control that already
passes, so the width boundary is pinned rather than rediscovered.

## Resolution (2026-09-30)

Two arms of the argument-bridging code in `emit_call` (`src/compiler/emit_expr.c`)
heap-promote a by-value aggregate into an inline-C `val : A` carrier slot
through `emit_carrier_bridge_escaping`. Both handed that bridge the raw
argument text. For a wide struct PARAMETER that text names a `const T *`, so
the heap cell was assigned a pointer (`*__t = x`). Then the later pass-by-ptr
`(*(...))` deref wrapped the already-bridged carrier word (`*((int64_t)...)`).
Those are the two cc errors in the report.

Both arms now check `expr_is_pbp_param` first. They hand the bridge the
pointee `(*(x))` and set `pbp_carrier_cast`, which suppresses the second
deref. This is what the neighbouring TY_APP arm already did for its
pointer-cast crossing. The arms are:

- the non-parametric seam-4 arm (`(defstruct Node [a b c])`, the report's
  repro), and
- the first `expr_emits_byvalue_carrier_abi` arm, reached by a PARAMETRIC
  by-value struct parameter (`x : (Tri int)`). Fixing only the first arm left
  this shape failing the same way.

The element is still a heap COPY. A copy is what a store needs, because the
element outlives the caller's aggregate the parameter pointer borrows.

Pinned by `tests/fixtures/vec-push-byvalue-struct-param`: the two-word
control, a three-word struct pushed twice and read field by field, the
four-field `Wide` shape (the one with a `bool` field) from the table above,
and the parametric `(Tri int)`. It runs under both `run.sh` and `run-turi.sh`.
The `crdt/rga` workaround in turmeric-spices (`__ins-at!` passes fields) can
now be reverted to take the node.
