---
title: Sized worlds cannot ride the parallel scheduler -- `System`/`Stage` type-erase the world through an `int`/`void*` carrier
category: Compiler / library architecture -- world-type polymorphism vs. int64 carrier ABI
severity: Expressiveness wall (blocked feature). No miscompile; the sized scheduler simply cannot be expressed today. A by-value `(GameWorld n)` struct cannot pass through the scheduler's type-erased run-fn carrier -- the same carrier wall as the `world-resize` blocker. Blocks the Slice 8 follow-up "wire sized worlds through the parallel scheduler."
status: OPEN 2026-06-17. Root-caused. Shares the int64 carrier root cause with the (now resolved) world-resize blocker [`docs/archive/world-resize-existential-multifield-struct-payload.md`](../archive/world-resize-existential-multifield-struct-payload.md). The pack/open half of the carrier wall is fixed (struct payloads can now be heap-boxed through an existential), so **direction 1 below (pass the world by heap pointer) is now unblocked** -- a `System`/`Stage` carrying a `ptr<GameWorld>` is implementable today. The remaining blocker is only the *heterogeneous* world-type-polymorphism (direction 2 / gap-H). Single-world sized scheduling can land via direction 1; cross-world scheduling still waits on gap-H.
---

# Sized worlds cannot ride the parallel scheduler

> **Note on scope of evidence.** The ECS `System`/`Stage` scheduler lives in
> the sibling repo `../turmeric-spices/spices/ecs/` (per the spice-repo
> layout rule in `CLAUDE.md`), which is **not checked out** in the reporting
> container, so the exact spice-side `System` struct line numbers are not
> cited here. What *is* verifiable in this repo is the shared compiler root
> cause -- the single-word carrier ABI -- and the identical type-erasure
> pattern in the stdlib coroutine scheduler. The spice-side shape
> (`run-fn : [w : int] : nil`) is taken from the originating Slice 8 report.

## Summary

The parallel scheduler erases the world type. A `System` packs a run-fn
typed `[w : int] : nil`: the world is passed as an `int` (a `void*`
reinterpreted as an `int64` handle), exactly the way the stdlib coroutine
scheduler passes fiber closures as `f : ptr<void>`
([`stdlib/scheduler.tur:61`](../../stdlib/scheduler.tur)):

```turmeric
(defn scheduler-spawn [sched : ptr<void> f : ptr<void>] : nil ...)
```

A `(GameWorld n)` from the sized-world plan is a **by-value struct** (one
`(Dense n T)` field per component plus a state cell), not an `int` handle.
It cannot be reinterpret-cast onto a single-word carrier and ride that
run-fn pointer -- this is the **same carrier wall** documented in the
sibling report
[`world-resize-existential-multifield-struct-payload.md`](world-resize-existential-multifield-struct-payload.md):
the existential/closure ABI assumes every payload fits in 8 bytes
([`src/compiler/emit_module.c:3300`](../../src/compiler/emit_module.c),
`typedef void * tur_exists_t;`). The codebase already flags this in
[`docs/upcoming/ecs-spice-plan.md`](../upcoming/ecs-spice-plan.md) (E2d-P6,
"Out of scope"):

> Today every storage handle rides the `int64` carrier so this is fine; if
> a backend ever wants non-int handles, that compiler work has to land
> first.

A by-value sized world is precisely a "non-int handle," and the scheduler's
run-fn carrier is precisely where it cannot land.

## Where it is queued

[`docs/upcoming/ecs-spice-plan.md`](../upcoming/ecs-spice-plan.md) ships a
parallel scheduler that proves per-`(world, component)` non-conflict
statically (Slice 8 / "Systems and scheduling"). The Slice 8 CHANGELOG
queues "wiring sized worlds through the parallel scheduler" as a follow-up
that generalizes `System`/`Stage` over the world type. The non-conflict
machinery and conflict-graph are already built
([`docs/upcoming/v1/ecs-cross-world-systems-plan.md`](../upcoming/v1/ecs-cross-world-systems-plan.md)
"The scheduler implementation already builds a conflict graph per ...");
what is missing is the ability to thread a *non-int* world type through the
scheduled run-fn.

## Root cause

Two layers, one cause:

1. **The carrier is one machine word.** Closures/run-fns scheduled by the
   parallel scheduler are stored and invoked through a `void*`/`int64`
   environment-and-payload carrier -- the same single-word existential
   carrier audited in
   [`docs/monomorphization-audit.md`](../monomorphization-audit.md) and
   defined at [`src/compiler/emit_module.c:3300`](../../src/compiler/emit_module.c).
   A by-value struct wider than `int64` does not fit (see the sibling
   report's repro: `(tur_exists_t)(intptr_t)((int64_t)(world_struct))` ->
   `cc: aggregate value used where an integer was expected`).

2. **`System`/`Stage` are monomorphic in the world type.** The run-fn is
   typed `[w : int] : nil` -- the world is fixed to the `int` handle so the
   scheduler can store a homogeneous list of systems without knowing each
   one's world type. Making it `[w : (World n) ...]` requires `System` /
   `Stage` to be polymorphic over the world type *and* for the scheduler's
   heterogeneous system list to survive monomorphization -- the **gap-H**
   limitation noted for `StorageOps`'s bounded wrappers
   ([`docs/upcoming/ecs-spice-plan.md`](../upcoming/ecs-spice-plan.md)
   E2d-P6: "methods whose *C* signature depends on `(Storage T)`" are
   out of scope today for the same carrier reason).

## Observed vs. expected

- **Observed:** the scheduler can only carry an `int`-handle world. A sized
  `(GameWorld n)` value has nowhere to ride; the surface cannot even be
  written without falling back to erasing the world to `int` (which throws
  away the entire point of the sized-world plan -- the static
  cross-parameter `n` unification).
- **Expected:** a `System`/`Stage` parameterised over the world type so a
  scheduled system runs against `(GameWorld n)` by reference, with the
  conflict-graph non-conflict proof intact and the size index `n` still
  statically threaded.

## Proposed fix directions

1. **Pass the world by heap pointer (smallest change, recommended first
   cut).** Keep `System`/`Stage` monomorphic in *representation* but change
   the world calling convention from by-value struct to `ptr<World>` (a real
   `:ptr<GameWorld>`, **not** `:int` -- see the "No Lazy `:int` Stand-Ins"
   rule). The scheduler carries a `void*`-to-world plus a monomorphic run-fn;
   each system's run-fn casts the pointer back to its concrete
   `ptr<(GameWorld n)>`. This sidesteps the carrier-width problem (a pointer
   is one word) without solving the harder polymorphism problem, and pairs
   naturally with fix-direction 1 of the sibling `world-resize` report
   (heap-boxed pack).
   - Caveat: this fixes representation but not *heterogeneity* -- a single
     scheduler still cannot mix systems over differently-typed worlds in one
     list unless their run-fn pointer types are unified (erased) at the
     storage boundary. For a single-world game that is fine; cross-world
     scheduling needs direction 2.

2. **World-type-polymorphic `System`/`Stage` (the real refactor).** Make
   `System`/`Stage` generic over the world type and monomorphize the
   scheduler per world type, or thread the world type through a typeclass so
   the scheduled run-fn dispatches with the concrete world ABI. This is the
   gap-H work: it carries monomorphization/linking risk (a heterogeneous
   system list whose elements have different world ABIs) and should not be
   attempted before the end-to-end monomorphization phases that retire the
   carrier bridge
   ([`docs/monomorphization-audit.md`](../monomorphization-audit.md),
   M2-M7) make a direct-ABI run-fn representable.

Recommendation: do **not** widen `System` to carry a by-value world struct
on the existing carrier -- that is the same dead end as the `world-resize`
blocker. Land the heap-boxed pack fix (sibling report, direction 1) first;
then direction 1 here (by-pointer world) unblocks single-world sized
scheduling; defer direction 2 to the monomorphization track.

## How to validate a fix

1. A fixture/spice test scheduling at least one `System` over a sized
   `(GameWorld n)` (by pointer), running a `Stage`, and asserting a
   component value mutated by the system survives -- with the size index
   still statically unified across the system's storage accesses (no
   runtime `__fe-min-cap` probe).
2. The existing parallel non-conflict tests (`stage-pair.tur`,
   `stage-wave.tur` per
   [`docs/upcoming/ecs-spice-plan.md`](../upcoming/ecs-spice-plan.md)
   "Validation -- what shipped") must still pass against the
   sized-world-typed scheduler -- the static non-conflict proof must not
   regress.
3. `bash tests/run.sh` (leak detection ON) green; a by-pointer world must
   not introduce a leak in the scheduled path.

## Related

- [`docs/archive/world-resize-existential-multifield-struct-payload.md`](../archive/world-resize-existential-multifield-struct-payload.md)
  -- the sibling blocker, **now resolved** (same int64 carrier root cause).
  Its heap-boxing pack/open fix landed, which is what unblocks direction 1
  here.
- [`docs/upcoming/ecs-sized-world-plan.md`](../upcoming/ecs-sized-world-plan.md)
  -- the sized-world surface the scheduler must accept.
- [`docs/upcoming/v1/ecs-cross-world-systems-plan.md`](../upcoming/v1/ecs-cross-world-systems-plan.md)
  -- the scheduler conflict-graph / non-conflict machinery to preserve.
- [`docs/monomorphization-audit.md`](../monomorphization-audit.md) -- the
  carrier-ABI audit and the M2-M7 path that direction 2 depends on.
- [`stdlib/scheduler.tur:61`](../../stdlib/scheduler.tur) -- the analogous
  `f : ptr<void>` type-erasure pattern in the stdlib coroutine scheduler.
