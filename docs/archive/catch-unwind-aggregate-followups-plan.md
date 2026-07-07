---
title: Stackless catch-unwind -- aggregate+group combo and the panic-unwind box leak -- Plan
category: Planning
description: Two smaller follow-ons to the completed general lowering (archived) -- (1) let a by-value aggregate param ride the cross-function GROUP driver (not just the single-function path), and (2) close the bounded aggregate-box leak that occurs when a self-call resume node is popped during a panic unwind.
---

# Stackless catch-unwind -- aggregate+group combo and the panic-unwind box leak -- Plan

> **Resolved.** Both parts landed. Part A: by-value and by-const-ptr aggregate
> params now ride the shared group driver (the shim boxes the arg by pointer, the
> seed switch copies it into the by-value / re-homed param local and frees the
> transfer box); every member's by-ptr params are registered as pbp so an
> accessor on any member's aggregate param reads the pointer directly instead of
> materializing a struct temp. Part B: a node-carried `uint32_t aggr_mask`
> (recommendation (2)) frees each popped self-call resume node's aggregate boxes
> during a panic unwind. Validated by `stackless-catch-unwind-mutual-aggregate-param`,
> `stackless-catch-unwind-mutual-byref-aggregate-param`, and
> `stackless-catch-unwind-panic-unwind-aggregate-leak` (the last confirmed under
> valgrind: the per-level 16-byte aggregate boxes no longer leak on the panic
> path, leaving only the pre-existing result-box baseline).

Two independent, small follow-ons from the general lowering
([archived plan](./compiled-catch-unwind-general-lowering-plan.md), G6).
Grouped because both are aggregate-heap-box housekeeping and both are narrow.

---

## Part A -- aggregate param + group (cross-function) combo

### Current state

By-value aggregate params ride the **single-function** trampoline (heap-boxed
into a `saved[]` slot). The **group** path (G4, mutual/cross-function recursion)
bails when any member has an aggregate param: `emit_group_member`
(`src/compiler/emit_fns.c`) returns false, falling back to native.

### Why the group path bails

A group emits one shared driver `static int64_t __cu_group_N(int __pc, int64_t
__a0, int64_t __a1, ...)`, and each member's ordinary C function becomes a
**shim** that marshals its args through the `__a` int64 slots:
`__cu_group_N(entry, sc_save(arg0), sc_save(arg1), ...)`. The driver's seed
switch restores each member's params from those `__a` slots. A by-value
aggregate does not fit an int64 `__a` slot, so the shim cannot pass it and the
seed cannot restore it.

### Design

The aggregate travels **by pointer** across the shim/seed boundary (the driver
takes ownership of a heap copy), then behaves like the single-function aggregate
param inside the driver:

- **Shim.** For an aggregate arg, `void *__ab = malloc(sizeof(T)); memcpy(__ab,
  &arg, sizeof(T)); ... __cu_group_N(entry, ..., (int64_t)(intptr_t)__ab, ...)`
  -- pass the box pointer in the `__a` slot.
- **Seed switch.** For that member+param, `memcpy(&<param-local>, (void*)__a_i,
  sizeof(T)); free((void*)__a_i);` -- copy into the driver's by-value param local
  and free the transfer box. From there the param is an ordinary
  `GsVar.is_aggr` local, saved/restored by the existing heap-box path across
  interior descends.
- **Arity/slot accounting.** `max_arity` and the `__a` count are already
  per-group; an aggregate arg still consumes exactly one `__a` slot (the
  pointer), so no widening of the arg vector is needed.

Lift the `emit_group_member` bail once shim+seed handle aggregates. Keep it as a
guard for anything still unsupported (e.g. aggregate returns until that plan
lands).

### Validation

- The mutual-recursion-with-aggregate-param case that currently falls back
  (see the archived plan's group-bail test) now uses `__cu_group_N` and matches
  native; runs deep flat where native SIGSEGVs.
- Scalar/int64 group output byte-identical.

---

## Part B -- panic-unwind aggregate-box leak

### The leak

Every descend heap-boxes each live aggregate param into a `saved[]` slot; the
matching **resume** memcpys it back and frees the box. On a normal return every
box is freed (verified leak-clean). But the panic-unwind loop in the driver
(`src/compiler/emit_fns.c`, `gs_emit_driver`):

```c
while (__k->boundary == 0 && __k->tag != 0) { tur_cont *__pk = __k->next; free(__k); __k = __pk; }
```

pops **self-call resume nodes** (boundary == 0) and `free`s the node **without
running its resume** -- so any aggregate box that node's `saved[]` slots point at
is leaked. It is bounded (by the unwound depth), panic-path only, and catch
nodes (boundary != 0) are fine (the unwind stops at them and their resume frees
their boxes as normal). Native does not box at all, so this is a stackless-only
leak on the panic path.

### Fix options

- **(1) Free aggregate boxes while popping (single-function).** In the unwind
  loop, before `free(__k)`, free the node's aggregate-box slots. The generic
  loop does not know which slots are aggregate; emit a per-function helper (or
  inline the known aggregate slot indices, which are compile-time constants for
  the single-function path) that frees them. For the group path, nodes of
  different members have different aggregate layouts -- would need a per-node
  "which slots are boxes" descriptor (a small bitmask stored on the node, or a
  per-tag table).
- **(2) Node-carried box bitmask.** Give `tur_cont` a `uint32_t aggr_mask`
  set at descend (which `saved[]` slots hold malloc'd boxes). The unwind loop,
  and the DONE/abort paths, free `saved[i]` for each set bit. Uniform across
  single and group; costs one word per node and a mask store per descend.
- **(3) Arena per call-tree.** Allocate aggregate boxes from an arena tied to
  the outermost boundary / driver entry; release the arena when the driver
  returns (normally or via panic). No per-box free, no leak, but changes the
  allocation model and must interact correctly with the propagate/longjmp exits
  (free the arena on every exit path).

Recommendation: **(2)** -- a node-carried bitmask -- is the smallest uniform fix
and also cleans up the abort/propagate exit paths (which today also drop boxes
of any still-live nodes). Reassess against (3) if box churn is a measured cost.

### Validation

- valgrind a panicking aggregate-param recursion (panic escaping past self-call
  frames): `definitely lost` drops to the pre-existing result-box baseline (the
  aggregate boxes no longer leak).
- Normal-path behavior and value unchanged; scalar cases byte-identical.

### Note

This leak is distinct from the always-native result-box + payload leak tracked
in `docs/reported/catch-unwind-result-box-leak.md`; fixing that is separate and
governed by native's drop semantics.
