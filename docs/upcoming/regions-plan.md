---
title: Regions (RM3) -- a declared lifetime for values with no unique owner
category: Plan
description: A scope form over the arena that already ships, so a persistent structure whose nodes have no unique owner is reclaimed by generation instead of per node. The answer RM2 needs and cannot supply itself; conservative by construction, so a shape the escape walk cannot prove means "no saving", never "use-after-reset".
---

# Regions (RM3)

**Status: PROTOTYPE, started 2026-09-04.** Gated behind `--enable=regions`.

RM3 of [reclamation-plan.md](reclamation-plan.md). Read that plan's RM2
section first -- this phase exists because RM2's question has no answer at RM2.

## The problem this solves

A persistent recursive structure has **no unique owner by construction**.
`(SBind v t rest)` shares `rest` with every older chain, and backtracking
depends on that sharing, so "is this the last reference to this node?" is a
runtime fact. RM1's scope-exit rule cannot reach it (the nodes escape their
constructor -- that is what a spine is), and per-node free needs ownership the
emitter does not have.

Measured cost of leaving it (2026-09-04):

- Per-node spine boxes are **~990 of the 1790 bytes** remaining in the RM1
  leak sweep, the largest real category once one fixture's hand-written test
  scaffold is out of it.
- It **grows**: a 64-link `Subst` chain leaks 64 boxes, and 100 rounds of an
  8-link chain leak 800, not 8. A program that builds and discards recursive
  values in a loop grows without bound.

## The shape

Do not ask who owns a node. Ask **when the whole generation dies.**

For a solver that is one query. For a parser it is one parse. The boundary is
not something this phase invents -- `bt-scope` (stdlib/trail.tur) already
brackets exactly it, and the reclamation plan already said so: "not region
inference; it is one `arena_reset()` at a call site that already exists."

## What already ships, and is why this is the tractable phase

- **`arena_reset`** (src/runtime/arena.c) -- O(slabs) rewind that keeps the
  backing memory. In a Debug build it **poisons** the reclaimed bytes, so a
  value that outlives its region crashes loudly under ASan instead of reading
  stale-but-mapped data. That is this phase's worst risk, already
  instrumented, before a line of it is written.
- **`arena_owns`** -- the guard the reclamation plan requires on every free
  path, so a pointer into region memory is never handed to `free()`.
- **A precedent with the same shape, already running.** turi's value-pool
  scratch/permanent split (`src/turi/eval.c`,
  `turi-value-pool-scratch-promotion-plan`) does reset-with-promotion: walk
  the escapees out, then rewind. Its conservatism rule is the one this phase
  adopts verbatim:

  > Correctness never depends on catching every shape: a missed shape means
  > "this eval does not shrink", never "use-after-reset".

  The code is the interpreter's and does not transfer; the discipline does,
  and it has been load-bearing in this tree before.

## The safety rule, stated once

**A region that cannot prove every escaping value safe to relocate does not
rewind.** No partial rewinds, no best-effort. That turns the failure mode from
"silent wrong answer" into "no saving", which is the only trade that makes a
lifetime mechanism shippable behind a flag.

Corollaries:

- Every free path consults `arena_owns` before `free()`.
- The Debug poison stays on. It is the backstop that makes a missed shape
  loud in the suite rather than latent in a user's program.
- A fixture asserts the **value** read back across the boundary, never merely
  that the program builds. That is the SR4 lesson and the reclamation plan
  already says it applies here with more force.

## Increments

**R1 -- the gate and the plumbing (this commit).** `EXPERIMENTS[]` row with
every descriptor field, `g_opt_regions`, `experiment_warn_if_used` at the
elaboration entry point. No behaviour change: with the flag off nothing moves,
and with it on the entry point warns and does nothing yet. Landing the gate
first is what CLAUDE.md requires for a user-visible feature, and it means every
later increment is measurable against a flag rather than a rebuild.

**R2 -- an allocation generation. LANDED 2026-09-04.** Spine-node allocation
routed to the innermost open generation, and to `malloc` outside one. No
escape analysis and no rewind: pure plumbing.

`tur_region_alloc_or_malloc` is the routing point, so the emitted constructor
needs ONE call site rather than a branch -- it allocates by generation inside a
region and exactly as it does today outside one. The consequence a caller must
carry is that the result is not necessarily region memory, which is why
`tur_region_owns` has to gate every free path rather than the allocation being
assumed.

**Two ctor emitters, and only finding one is how this nearly shipped wrong.**
A `:heap` ADT's node is emitted by the base ctor (`emit_module.c`,
`emit_adt_typedef_and_ctors`) AND by the monomorph ctor (`types.c`). The first
pass changed only the base emitter, and `ctor_Cons_Cons__int` -- the ctor the
leak sweep actually blames -- kept its plain `malloc`. Caught because the
verification asked the emitted C rather than trusting the edit. This is
verbatim the standing habit RM1 wrote down after the null-None mirror ("when a
representation change lands in a ctor or temp emitter, grep for the other
emitter before calling it done"), and it cost a round anyway; the two sites now
cross-reference each other by name.

`region.c` and `arena.c` also had to join `TURT_RUNTIME_SOURCES`, not just the
compiler's own object list: an emitted program LINKS the routing helper, so a
definition only the compiler can see is an undefined reference at the fixture's
link step. With the flag off nothing references it and the linker never
extracts the member -- the same reasoning the rc/gc members carry.

*Validation:* `tests/run-regions-seam.sh` (ctest `tur_regions_seam`) asserts
that `--enable=regions` with no region open changes nothing OBSERVABLE across
six spine-carrying fixtures -- output equality, not that both arms build, since
a routing bug returning uninitialised memory would still link. That is the
property R2 is built to have and the one a later increment is most likely to
break by accident.

**R3 -- the escape check and the rewind. LANDED 2026-09-04 (runtime half).**

`tur_region_note_escape(p)` records a value crossing out of the innermost
generation; `tur_region_pop_checked(depth)` reclaims only when no noted escape
pointed INTO it, and retires otherwise. It returns which it did, so a caller
can report whether a region paid for itself.

**Reclaiming rewinds rather than releases.** `arena_reset` keeps the slabs and
the arena is pooled for the next push, so a per-query region inside a loop pays
for its slabs once -- and the reset is what gets the Debug poison, which
`arena_free` would not. The unit test pins the reuse (`second == first`), not
just that reclaim happened.

### What the runtime check does and does not establish

This is the part to read before extending it. The check proves the escaping
pointer ITSELF does not point into the generation. **It proves nothing about
what that pointer transitively reaches** -- a malloc'd struct whose field
points at a region node passes and would dangle after a rewind.

So it is the SECOND lock. The first is static, and lands with R4 where there
is a form to attach it to: a region reclaims only when its result TYPE cannot
transitively reach a region-allocated node, which is a compile-time question
with a decidable conservative answer. The runtime check then catches the direct
case cheaply and makes a static mistake loud rather than silent. Neither alone
is the safety argument, and neither should be removed on the strength of the
other.

Erring the right way is what the tests assert: a region-owned escape must BLOCK
the rewind, and a heap escape must NOT -- one direction is unsafe, the other
makes the check useless.

### The backstop is verified, not asserted

The claim this phase rests on is that a value outliving its generation crashes
loudly rather than reading stale-but-mapped data. Checked directly: a canary
that reads reclaimed region memory reports

```
ERROR: AddressSanitizer: use-after-poison on address 0x531000000818
READ of size 1 ... #0 in main canary.c:10
```

`tests/check-region-poison.sh` (ctest `tur_region_poison`) is that canary, kept
because a backstop that silently stops firing -- an ASan flag dropped from the
Debug build, `TUR_DEBUG_ARENA_POISON` turned off, `arena_reset` changed to skip
the poison -- looks exactly like a program with no stragglers. Same reasoning
as `check-cc-warn-ratchet.sh`, and the same failure it was written for.

**R4 -- wire `bt-scope`.** Not a new surface form: the solver's existing
bracket becomes a region boundary. Measure on `logic.tur`, which is the
workload RM0(b) asked for and this plan's sibling report has already
benchmarked at n=1..512.

**R5 -- decide.** Graduate, shelve, or bump, against the leak sweep and the
`bench-logic-subst` A/B. Shelving is a real outcome: if R3's conservatism
means the solver's region never actually rewinds, the phase says so and stops.

## What this does NOT do

- It does not free a value whose lifetime is not a scope. Those stay RM2's,
  and RM2 gets smaller for it -- what remains is "spines that escape their
  region", which is also the set R3's walk has to identify anyway.
- It does not change any default. The flag is off; `logic.tur` and every other
  program allocate exactly as they do today until R4 opts a call site in.
- It is not region *inference*. Every region is declared.

## Guides to update when this graduates

- `docs/guides/gc-guide.md` -- it documents the arena and the rc/cycle paths;
  a third reclamation mode belongs beside them.
- `docs/guides/experimental-flags-guide.md` -- the row while it is gated.
