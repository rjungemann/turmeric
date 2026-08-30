---
title: Reclamation Plan (RM)
category: Planning
description: Freeing what the emitted program allocates -- the mechanism the sum-representation plan has routed three separate decisions through since 2026-08-25 without a plan existing to route them to. Scope-exit drop glue for the boxed residue, then the recursive spine, then declared regions over an arena that already ships inside every emitted binary. The first phase builds nothing: SR1, SR2a and SR3 slice A have moved the baseline out from under every row of the measurement this plan inherits.
---

# Reclamation (RM)

**Status: proposal. Nothing here is built.**

[sum-representation-plan.md](sum-representation-plan.md) has named reclamation
the largest measured win on its board since 2026-08-25 and has routed three
separate decisions through it -- SR4's default flip ("waiting on either a
workload that wants memory over speed, or reclamation landing first"), the
slab's shelving ("reclamation (row B) is the unblocked half"), and its own
ordering rationale ("do the arena first and then re-ask whether SR4 is worth
starting"). No plan existed to route them to. This is that plan.

**The first phase builds nothing, and that is the point.** The 7.64x that
justifies this work was measured on 2026-08-25 against a tree where every
multi-variant ADT boxed. SR1 (2026-08-26), SR2a/SR2b (2026-08-27) and SR3
slice A (2026-08-27) have since removed the allocation outright for most of
that population. The rows still price a real population -- the recursive
sums, which are still carrier-boxed by default -- but "7.64x" stopped being
a claim about the language and became a claim about one corner of it, and
nobody has re-measured. **RM0 exists to re-base the number before anything is
built on it.**

That is the same discipline SR0 was specified under, and the same trap
section 5 of the SR plan records: a phase gate is only as good as the workload
it is measured on. Here the workload did not change -- the compiler did, out
from under it.

## 0. Provenance

- [multi-variant-adts-always-heap-allocate](../archive/multi-variant-adts-always-heap-allocate.md)
  -- "Cause 2 -- nothing ever frees them", and the
  [slab-shelving decision](../archive/multi-variant-adts-always-heap-allocate.md#decision----the-slab-allocator-is-shelved-2026-08-25)
  with its two-part reopen condition.
- [benchmarks/adt-alloc/RESULTS.md](../../benchmarks/adt-alloc/RESULTS.md) --
  the seven representations, and the caveat on rows F and G that is the whole
  design problem of RM3.
- [sum-representation-plan.md](sum-representation-plan.md) sections 2 and 3.
- Three open reports this plan is the fix for:
  [carrier-sum-option-boxes-have-no-owner](../reported/carrier-sum-option-boxes-have-no-owner.md),
  [inline-c-option-carrier-box-leaks](../reported/inline-c-option-carrier-box-leaks.md),
  and the container-element box that
  [container-element-form-plan.md](container-element-form-plan.md) removes for
  niche Options only.

## 1. What is actually true today

### Nothing frees a sum box, and one line says so

```c
/* elab_effects.c:30 */
return def->needs_drop_glue && !def->is_heap && def->n_ctors == 1;
```

`needs_drop_glue` (`elab_structs.c`) is set for `TY_RC` / `TY_REF` / `TY_WEAK`
and boxed-fn fields, and transitively for a nested owning by-value aggregate
field -- so it releases a box's *contents*, never the box itself, and only
for a single-variant def; `elab_forms.c:30` gates the same way. A `:heap` sum yields a typed
pointer instead of an `int64_t` carrier and still never frees. Every
construction that still boxes, still leaks.

### The population that still allocates has shrunk, and the benchmark has not noticed

| construction | allocated 2026-08-25 | allocates today |
|---|---|---|
| non-recursive multi-variant `defdata` | yes | **no** -- SR1, default on |
| concrete `(Option T)` / `(Result T E)` monomorph | yes | **no** -- SR2a, graduated |
| `(none)` | yes | **no** -- SR3 slice A, default on |
| **self-recursive sum spine, per node** | yes | **yes** -- SR4 not defaulted |
| **erased-path `some` / `ok` / `err`** | yes | **yes** -- generic bases |
| **`:heap` ADTs** | yes | **yes** |
| **wide by-value aggregates at a fat/container boundary** | yes | **yes** -- b4box |
| **container elements crossing the erased boundary** | yes | **yes** -- CE's subject |

The top three rows are gone, and they were most of what a corpus constructs.
The bottom five are what reclamation now has for a constituency. `logic.tur`'s
`Term` and `Subst` are self-recursive, so **row A of the ceiling table is still
literally today for them** and the benchmark has not rotted -- it has narrowed.
RM0's job is to say by how much, in the corpus rather than in one benchmark.

### The mechanism to build RM1 out of already exists, three times over

The emitter carries three independent "bind a heap value to a local, prove the
name does not escape, free it at scope exit or at its sole use" families:

| client | escape walk | firing |
|---|---|---|
| closure env | `closure_binding_escapes` (emit_expr.c) | `let_binding_env_freeable` |
| catch box | `catch_box_binding_escapes` | trailing drop |
| `any` widen box | `any_box_binding_escapes` | pending-drop stack + scope-drop stack, fired at `return` and at tail-call back-edges (`emit_any_scope_drops`, emit_expr.c:4061) |

The third landed 2026-08-29 over five passes and is the most complete: it
handles the temporary case (`any_pending_*`, drained per node so an inner call
never drops an outer call's argument), the scope case, and early exits. **A
carrier sum box is the fourth instance of that shape, not a new mechanism.**
That is what makes RM1 the cheap phase and why it goes first.

### And the arena already ships inside every emitted program

`src/runtime/arena.c` is in `TUR_CORE_SOURCES`, so it is in `libturi`, which
every fixture binary already links. Its API is `arena_init` /
`arena_alloc_aligned` / `arena_reset` / `arena_free` / **`arena_owns`**, and it
carries ASan-aware poisoning plus a guard mode that turns a stale pointer into
a hard SIGSEGV at the deref (`arena-debug-poisoning-plan`). The emitted
preamble already declares libturi externs directly (`extern uint64_t
tur_atomic_load_u64(...)`, emit_module.c:8601).

**So RM3's runtime is three extern lines, not an implementation.** And
`arena_owns` is specifically the thing whose absence killed the slab: the slab
died on a `free()` of slab memory reached through `rc/of`, and being safe
needed a whole-program escape pass. A free path that asks `arena_owns(p)`
first is safe *locally*, which is the difference between this and the
representation that was shelved.

## 2. The two mechanisms, and the one honest caveat

| | representation | vs today |
|---|---|---:|
| B | boxed, **freed** per node | 2.49x |
| G | boxed, **arena**-reclaimed | 7.64x |
| F | by value **and** arena-reclaimed | 10.95x |

Reclamation is also nearly *flat* across a 64x heap range where leaking climbs
-- this is a footprint problem before it is a speed problem, which is the
report's own corrected finding.

The caveat on F and G, restated because it is the entire design problem of
RM3 and quoting it is cheaper than rediscovering it:

> An arena needs a region whose end is known, and that is the hard part, not
> the bump pointer. This harness resets at the end of each pass because the
> pass boundary is obvious; real code has no such boundary handed to it, and
> inferring one is region inference.

**RM does not do region inference.** RM3 takes the boundary as *declared*.
That is a smaller feature than the 7.64x implies, and pretending otherwise is
how this plan would end up promising a number it cannot deliver.

## 3. Phases

### RM0 -- re-base the measurement, and find the constituency

Builds nothing. Two deliverables, and the second is the one the slab decision
already asked for and nobody has produced.

**(a) Re-base the ceiling.** `benchmarks/adt-alloc/ceiling.c` models
`logic.tur`'s `Term`/`Subst` shapes by hand in C. Re-run it unchanged (it
still prices the recursive population honestly), then add the measurement it
cannot make: what a corpus program actually still allocates. The instrument
exists -- SR0(a)'s per-constructor counters injected into emitted C
(`benchmarks/sum-census/`), which deliberately are not a codegen flag so the
measurement does not perturb what it measures. Re-run that census against
today's compiler and report allocations, not constructions: SR0(a) counted
constructions when every construction allocated, and those are now different
questions.

**(b) Find a real workload, or record that there is not one.** The slab
decision's reopen condition names two halves and requires both: a real
workload -- not a benchmark -- that constructs enough to care, *and* a reason
it cannot take the cheaper fix. SR0(a) went looking for the first half and did
not find it: 20 `defdata` across 727 spice files, `minikanren` constructs
nothing and does not import the module it is named for, and `datalog` is the
one sum-heavy program. **The honest possible outcome of RM0 is that RM2 and
RM3 have no constituency and only RM1 should be built.** That outcome must be
reportable, or this phase is not a gate.

*Files:* benchmarks only. *Gate:* RM2 and RM3 do not start until (a) reports
the residual allocating population and (b) either names a workload or
concludes there is not one. *Validation:* zero behavior change; the corpus
untouched.

### RM1 -- drop the boxed residue at scope exit

The row B mechanism, local, no whole-program analysis, as the fourth client of
the `any`-box machinery. A carrier sum box bound to a local that provably does
not escape is freed at scope exit and at every early exit, through
`any_scope_drops_push` / `emit_any_scope_drops` and the pending-drop stack for
the temporary case.

Closes [carrier-sum-option-boxes-have-no-owner](../reported/carrier-sum-option-boxes-have-no-owner.md)
for the erased path, which is what it has narrowed to after SR2a. Whether it
closes [inline-c-option-carrier-box-leaks](../reported/inline-c-option-carrier-box-leaks.md)
is an open question RM1 must answer explicitly rather than assume: that box is
built inside a C body the emitter did not write, so the drop has to attach at
the call site from the declared return type, and the report says the obvious
workaround does not transfer.

**Take the `any` family's hard-won rules with it, do not re-derive them.** Two
in particular, both of which cost a pass each: a narrowing cast on a bare name
is not an escape when the cast emits a copy (but that admission is a flag the
`any` rules set, and the closure-env and catch-box callers still refuse a
cast); and `emit_tail` emits a tail-position `let` inline rather than through
`emit_let_value`, so bookkeeping added only to the latter silently never runs
for exactly the loop shape this kind of phase exists to fix.

*Files:* emit_expr.c, emit_stmt.c, emit_fns.c. *Gate:* none -- this is the
phase that runs whatever RM0 says. *Validation:* `requires.leak-check`
fixtures with real teeth (the SR1 pattern: bytes leaked without the change,
zero with it), the full corpus green at its current baseline, and snapshots
regenerated in the same commit.

### RM2 -- the recursive spine

The per-node spine box of a self-recursive sum, which is what `logic.tur`
allocates and what SR4 leaves boxed even under `TUR_SR4_RECURSIVE_BYVALUE=1`
(by value halves the mallocs; the spine box per node remains).

**This is where RM1's local rule stops working.** A tree's nodes escape their
constructor by construction -- that is what a spine is -- so scope-exit drop
does not reach them and per-node free needs ownership the emitter does not
have. Row B's 2.49x is measured with the frees written by hand in C, not
inferred.

*Gate:* RM0(b). If there is no workload, this phase does not start, and the
reason is recorded rather than the phase being left implicitly pending.

### RM3 -- declared regions

An explicit scope form over the existing arena. `arena_owns` guards every free
path so a pointer into region memory is never handed to `free()` -- the
slab's exact death, made a local check instead of a whole-program pass.

Applied first to the workload that has a natural boundary: the SR plan already
identifies it, and is careful about what it is claiming --

> a per-query arena there is not region inference; it is one `arena_reset()`
> at a call site that already exists.

**This one is user-visible, so per CLAUDE.md it wants an `EXPERIMENTS[]` row**
with every descriptor field populated, `experiment_warn_if_used` at the
elaboration entry point, and `plan_path` pointing here. It is also the phase
most able to produce a silent wrong answer -- a value outliving its region is
a use-after-reset, and the arena's guard mode exists precisely to turn that
into a SIGSEGV at the deref rather than a plausible-looking read. Any
regression fixture asserts the VALUE, not merely that it builds; that is the
SR4 lesson and it applies here with more force.

*Gate:* RM0(b), and RM1 landed (a region is not a substitute for knowing what
owns a box; it is a different answer to the same question, and shipping both
orderings at once is how two sites end up disagreeing about who frees).

### RM4 -- re-measure, and settle SR4

SR4's default flip is explicitly parked on this: the trade today is 7-13%
time for 2.2x memory, and an arena "makes the carrier's mallocs cheap AND
keeps one-word copies, at which point by-value recursive sums may have no
constituency at all." The seam (`TUR_SR4_RECURSIVE_BYVALUE=1`,
`tests/run-sr4-seam.sh`, ctest `tur_sr4_seam`) is CI-armed, so this
re-measurement is a re-run, not a re-excavation.

Deliverable: the SR plan's SR4 section updated with a decision, either
direction. RM4 is not entitled to leave it open a second time.

## 4. What this plan does not do

- **Region inference.** Named as the hard part by the benchmark's own caveat.
  RM3 takes the boundary as declared.
- **Turning on the cycle collector.** `src/runtime/gc.c` is a Bacon-Rajan
  trial-deletion collector, `GC_DISABLED` by default for v1. It collects
  `rc<T>` cycles, which is a different problem from a box nobody owns. RM does
  not touch it.
- **The slab.** Shelved 2026-08-25, and re-measurement strengthened the
  decision rather than weakening it (E 1.72x against B 2.49x, and E is the
  only proposed *fix* that still climbs with heap size). The reopen condition
  stands as written and requires both halves.
- **Ownership or affine types.** The dead-arm write is gone with SR2b, so the
  expressiveness argument that would motivate them is already banked.
- **Container element boxes.** That is [CE](container-element-form-plan.md),
  which removes them for niche Options by making storage per-monomorph rather
  than by freeing them.

## 5. Risks, named

- **The escape walk's admissions are the whole soundness argument.** Every
  false negative is a use-after-free, and the `any` family took five passes to
  get its walk right. RM1 inherits the walk rather than writing one; a new
  admission is a new pass, priced as such.
- **Free of arena memory.** The slab's exact failure mode. `arena_owns` is the
  answer and it must be on every free path RM3 can reach, not most of them.
- **ASan sees a bump arena at the wrong granularity.** Already solved --
  `arena.c` carries poisoning and a guard mode with the rationale in
  `arena-debug-poisoning-plan`. Reuse it; do not re-derive it, and do not
  suppress a leak report to make a phase look done.
- **Do not price RM on `logic.tur` alone.** It is the workload the SR plan
  fell into twice, in both directions -- once under-selling SR1 because
  `logic.tur` is structurally blind to it, once over-selling the ceiling
  because `logic.tur` is all of the population it prices. RM0 is specified
  over the corpus for exactly that reason.
- **Fixture churn.** RM1 changes emitted C, so snapshots regenerate in the
  same commit as the codegen change, never in a follow-up.

## 6. If only one phase gets built

**RM1.** It is local, it reuses a mechanism that has been proven three times,
it closes at least one open report outright, and it is the only phase that
does not depend on RM0 finding a constituency. Row B is 2.49x and flat with
heap size, which is the property that matters for a footprint problem.

RM3 is the phase with the big number attached, and it is also the phase whose
number is conditional on a boundary the language does not currently have a way
to say. Build it when RM0(b) names a workload that wants one -- not before,
and not because 7.64x is the largest figure in the table.
