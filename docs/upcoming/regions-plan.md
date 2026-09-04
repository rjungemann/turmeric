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

**R2 -- an allocation generation.** A region handle over the existing `Arena`,
with spine-node allocation routed to it inside a region and to `malloc`
outside. `arena_owns` on the free paths. No escape analysis yet: R2's region
never rewinds, so it is pure plumbing that can be measured for allocation-path
cost in isolation.

**R3 -- the escape check and the rewind.** The conservative walk: prove every
value crossing the boundary is relocatable or leave the generation intact.
Rewind only when proven. This is where the turi precedent's shape is copied.

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
