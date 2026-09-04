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

**R4 -- wire `bt-scope`. LANDED 2026-09-04.** Not a new surface form: the
solver's existing bracket became the region boundary, so a caller opts in by
using the bracket it would use anyway. `emit_value` opens a generation around a
call to `bt-scope` and closes it after the call's hoist temp.

### The static lock, which is the half R3 could not have

`region_type_reaches_node` (emit_expr.c) walks the call's RESULT type and
refuses unless it can prove the type cannot transitively reach a
region-allocated node. Scalars and `cstr` pass; a `:heap` ADT fails on sight; a
non-heap ADT is walked through its type arguments and every constructor field,
with a **self-recursive field's NULL `full_type` read as "reaches"** -- which is
exactly the spine, so a recursive result is refused. Pointers, refs, closures,
structs, containers and type variables are refused outright: R4 needs none of
them to say no, and refusing costs a saving rather than correctness.

An unproven result emits `tur_region_pop` (retire), which is byte-for-byte what
a flag-off build does.

### Both locks are load-bearing, and that was checked rather than argued

R3's header claims the runtime note is a second lock, not decoration. R4 has the
case that proves it: a carrier-ERASED `:heap` pointer is spelled `:int`, so the
static walk sees a scalar and clears the rewind. Only
`tur_region_note_escape` catches it. Deleting the note from
`tests/fixtures/region-scope-escape-refused`'s emitted C turns it into

```
ERROR: AddressSanitizer: use-after-poison ... READ of size 8 ... #0 in chain_hysum
```

so the fixture is a live test of the lock and not a shape that happens to work.

### R2's "spine-node allocation is routed" did not cover the workload

The single most valuable thing R4 did was measure the saving instead of
re-reading the claim. Two allocation sites had to be found by looking at emitted
C and at valgrind block counts:

- **A THIRD ctor emitter.** `emit_program` emits base ctors at its own site
  (emit_module.c, near the SR3 slice-A note that records this exact miss one
  representation change earlier), so a NON-parametric `:heap` ADT -- `(defdata
  Link :heap ...)`, the plainest spine there is -- still called `malloc` after
  R2. Symptom: valgrind reported 913 live blocks with the flag on against 909
  with it off. The region was pushed, popped, and never allocated into.
- **The SR4 recursive-field box, which is not a ctor at all.** `Subst` is
  `:copy`, not `:heap`, so its per-link box is the `tur_adt_Subst *__t =
  malloc(...)` that emit_expr.c writes when a by-value aggregate flows into a
  recursive constructor field. That is the allocation the RM1 leak sweep
  actually blames, and none of the three ctor emitters reach it. Until it was
  routed, R4 on `logic.tur` saved nothing at all.

Four allocation sites now, then, and they do not resemble each other. The habit
this keeps re-teaching is not "grep for the other emitter" -- that was already
written down twice and still missed both of these. It is: **a routing change is
verified by measuring the saving, never by reading the diff.**

### The free side had to move with it

From R2 onward a node allocated inside a region is arena memory while the same
emitted drop glue still ended in `free(ptr)`. That is an allocator mismatch --
glibc aborts -- not a leak. `tur_region_free` (free unless `tur_region_owns`) is
the mirror of `tur_region_alloc_or_malloc`, spelled by `region_free_fn()` at the
node free paths and resolving to plain `free` in a default build.

### What it is worth

`benchmarks/bench-regions-subst.tur`, same source both arms, flag as the only
variable (full numbers in `benchmarks/regions-subst-results.md`):

| | flag off | flag on |
|---|---|---|
| leaked at exit | 2,668,800 B in 55,600 blocks | **0** |
| allocations | 64,645 | 9,050 |
| ns/op, n <= 16 | -- | **0.75-0.80x** |
| ns/op, n >= 64 | -- | ~parity |

Checksums identical in every row, which is the column that would catch a rewind
of something still live.

### Known gaps R5 has to price

- **A `:void` bracket is not wrapped at all.** Such a call is not hoisted into a
  temp, so `emit_value` has no statement seam to close after it. A missed
  saving, conservative, but `(bt-scope (fn [] ... ))` for effect is a normal
  spelling.
- **The pooled slab is never returned.** 64 KiB stays reachable at exit because
  no emitted program calls `tur_region_shutdown`. O(1), reported as reachable
  rather than lost, and worth closing before graduation.
- **Argument-position allocation lands in the generation**, because the push has
  to precede the argument emissions -- the call text embeds their temps. Sound
  for this form's thunk; the shapes it is not sound for are the transitive ones
  the static walk already refuses.
- **A `bt-scope` in a non-main defn whose thunk calls another user function
  miscompiles**, independently of this flag --
  `docs/reported/cps-direct-bt-scope-closure-temp-undeclared.md`. Both R4
  fixtures and the benchmark carry a workaround for it.

**R5 -- decide.** Graduate, shelve, or bump, against the leak sweep and the
`bench-regions-subst` A/B. Shelving was a real outcome and R4 has now retired
the reason it was most likely: the solver's region DOES rewind, and takes the
whole per-link spine with it (2,668,800 leaked bytes to zero). What R5 weighs is
therefore no longer "does this ever pay" but the four gaps listed under R4 --
the `:void` bracket, the unreturned slab, argument-position allocation, and how
much of the language the static walk has to accept before the flag is worth
turning on by default.

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
