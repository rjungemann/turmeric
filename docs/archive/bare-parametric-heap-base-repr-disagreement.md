# repr_of and the merge-temp emitter disagree on a bare parametric :heap base

**Severity: medium** -- a compiler **ICE** (`repr-shadow merge-temp ... want=heap-ptr
got=carrier-i64`), not a wrong answer. It is the representation-decision defect
family that [repr-decision-function-plan.md](repr-decision-function-plan.md)
exists to close.

**Status: RESOLVED** 2026-09-12. Found making `crdt/ormap` a `:heap` parametric
struct so it would carry as a pointer per instantiation.

## Mechanism

Inside a generic body a parametric container appears two ways: applied with
erased arguments (`(Map K V)`, a `TY_APP` whose args are tyvars) and as the
**bare base** with its args dropped entirely (`ORMap`, a `TY_ADT` whose def has
`n_type_params > 0`). Both mean the same thing -- the erased container, declared
as the int64 carrier.

`emit_repr_concrete_heap_ptr_c_name` excluded both spellings, with a comment
saying why. `repr_of` carved out only the `TY_APP` one:

```c
if (t->kind == TY_APP && repr_app_mentions_erased_arg(t) &&
    (pos == REPR_POS_LET_BIND || pos == REPR_POS_RESULT))
    return REPR_CARRIER_I64;
return REPR_HEAP_PTR;          /* <- bare parametric base fell through here */
```

The `TY_APP` test cannot see the bare-base case: there are no arguments left to
find a tyvar in. So the shadow said heap-ptr, the emitter declared the carrier,
and the two disagreed.

## Fix

Extend the carve-out to a bare parametric ADT base in the same declaring
positions, matching the predicate it is shadowing.

## Fixture

`tests/fixtures/heap-parametric-struct-constrained-join` -- a `:heap` parametric
struct with a by-value struct field, merged through a constrained instance that
recurses at its element type, with two element lattices whose joins differ.

It also covers a second defect found in the same emission: the by-value field's
**forward** typedef (`TUR_FWD_<name>`) and its **full layout**
(`TUR_TD_<name>`) used different include guards for one typedef name, so each
let the other through and cc warned `-Wtypedef-redefinition` on emitted code --
a hard error for anyone compiling the output as C99. The forward emission now
checks both guards.
