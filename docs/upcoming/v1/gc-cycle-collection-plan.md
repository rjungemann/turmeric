# Cycle-Collecting GC -- Reaching Past Rust (CG0--CG8)

> **Status:** CG0 + CG1 landed 2026-07-25 (dynamic buffers + registry with an
> O(1) unregister; classic PossibleRoot candidate buffering). CG2 -- real
> trial deletion, the phase that actually reclaims cycles -- is next and is what
> makes CG1's buffered candidates observable. CG2-CG8 not started. The Bacon-Rajan *scaffold* exists (`src/runtime/gc.c`,
> `src/runtime/gc.h`, colors + modes + suspect buffer + a global block registry
> + `mark_phase`/`trial_deletion_phase`), but as wired it only reclaims the
> **weak-zombie** case (strong->0 while weak>0). It cannot collect a live
> **strong** reference cycle -- and not merely because of the suspect-buffering
> condition: `gc_mark_phase` treats *every* block with `strong_count > 0` as a
> root, and every member of a self-sustaining cycle has `strong_count > 0`, so
> the cycle is unconditionally marked live. Real cycle collection requires
> trial *decrements* to distinguish internal (in-cycle) references from external
> roots. This plan replaces the mark-sweep-from-strong-roots core with a correct
> synchronous Bacon-Rajan trial-deletion collector, and adds an opt-in
> automatic trigger.
>
> **The reach-past-Rust claim:** Rust's `Rc`/`Arc` never collect cycles -- the
> programmer must break them with `Weak`. Turmeric already matches Rust (rc<T> +
> weak<T>); this plan lets Turmeric go *beyond* Rust with an **optional,
> automatically-triggered** cycle collector, while keeping the zero-overhead
> default (collector off, pure RC) for programs that don't opt in.
>
> **Prerequisites:** none hard. Benefits from the stdlib weak-ref audit
> (`stdlib-weak-ref-audit-plan.md`) landing first so the test corpus reflects
> real cycle shapes, and shares the walker infrastructure with the existential
> GC work (EXG5, already shipped).
>
> **Gate:** the runtime already ships the `(gc!)` / `(gc-enable!)` /
> `(gc-disable!)` intrinsics ungated (they are runtime knobs, off by default, not
> elaboration features). The *new* automatic mode (CG5) and any move toward
> default-on (CG8) are semantics/perf changes and MUST ship behind an
> `EXPERIMENTS[]` row (`--enable=cycle-gc`) per the CLAUDE.md experimental-feature
> rule, with `experiment_warn_if_used` wired at the elaboration site that lowers
> the automatic-mode intrinsic. The manual `(gc!)` path stays ungated.
>
> **Last updated:** 2026-07-25

---

## Motivation

`rc<T>` alone leaks cycles. Turmeric's data-structure defaults (persistent HAMT,
by-value structs, cons lists) make cycles rare in practice, which is why
off-by-default is defensible for v1. But two audiences want more:

- Programs that genuinely model cyclic object graphs (doubly-linked structures,
  observer/back-edge graphs, arbitrary user graphs) currently must hand-manage
  `weak<T>` exactly as in Rust, or leak.
- The project has already invested in a Bacon-Rajan scaffold and walker
  infrastructure. Finishing it -- as an *opt-in, automatic* collector -- is a
  differentiator Rust does not offer.

The single fact that shapes this plan: **the collector is opt-in and the RC
layer is always correct on its own.** That means the collector is allowed to be
conservative (miss a cycle routed through an `RCK_OPAQUE` handle) and stay
sound: a missed cycle is a leak, never a use-after-free. Soundness is
one-directional -- never free something still reachable; leaking is always the
safe failure. This is what makes an incrementally-built collector shippable.

Non-goals for this iteration:

- Concurrent / background collection. The collector is synchronous, driven at
  `(gc!)` or at an allocation/threshold checkpoint. Thread-safety of the global
  buffers for `arc<T>` across threads is explicitly deferred (CG7 notes it).
- A tracing, precise, moving collector. This stays trial-deletion on top of RC.
- Making cycle collection the default. That is CG8, gated and separate.

---

## Current state (what's really there)

Grounded in the source so the plan starts honest:

- **Colors / modes / buffers.** `GcColor` = WHITE/GREY/BLACK/PURPLE (`rc.h:19-24`).
  `GcMode` = DISABLED/MANUAL/THRESHOLD (`gc.h:21-25`), default DISABLED
  (`gc.c:33`). Suspect buffer, grey queue, and global block registry were all
  static 4096-entry arrays; **CG0 replaced them with growable vectors.**
- **Suspect entry is zombie-only.** `rc_strong_decrement` calls
  `gc_on_strong_decrement` only on strong->0 with weak>0 (`rc.c:180-184`), and
  that function again guards on `weak_count > 0` (`gc.c:369`). A strong cycle
  never triggers it.
- **Mark treats all live blocks as roots.** `gc_mark_phase` marks every
  registered block with `strong_count > 0` BLACK (`gc.c:249-255`), then
  propagates through `RCK_STRUCT` walkers (`gc.c:283-288`) and
  `RCK_EXISTENTIAL`/`RCEXP_RC` payloads (`gc.c:271-282`). Because a cycle member
  has `strong_count > 0`, it is a root -- structurally uncollectable.
- **Trial deletion is really "sweep zombies not reachable from strong roots"**
  (`gc.c:296-343`), not Bacon-Rajan trial deletion.
- **Two latent bugs** (filed separately this session): a registry overflow past
  4096 silently dropped blocks (**fixed by CG0**), and trial deletion force-frees a
  block with `weak_count > 0` by zeroing the weak count (`gc.c:335-340`),
  dangling any live `weak<T>` -- contradicting the zombie contract the rest of
  RC upholds.

The walker infrastructure (EXG5 + DS3) is the reusable, correct part. The
collection *algorithm* is what CG1--CG3 replace.

---

## Phases

### CG0 -- Dynamic buffers + registry (unblocks everything) [DONE 2026-07-25]

Replace the three static 4096 arrays (`gc_all_blocks`, `gc_suspect_roots`,
`gc_grey_queue`) with growable vectors. Fix the silent-drop cliff at
`gc_register_block` (`gc.c:207`). This is a prerequisite: a real collector must
see every rc block, and real programs exceed 4096 live blocks. Keep the static
fast-path capacity as the initial allocation.

**Landed.** All three vectors now grow on demand via a shared `gc_vec_reserve`
helper; the only remaining failure mode is genuine OOM. Three silent-drop sites
were fixed, not one:

- `gc_register_block` -- blocks past 4096 were invisible to the collector.
- `gc_add_suspect` -- suspects past the cap were discarded, losing real garbage.
  `GC_MAX_SUSPECTS` is now a *force-a-collection* watermark rather than a hard
  cap: collection is attempted first, and the buffer grows if it cannot drain.
- the grey queue (three enqueue sites) -- a dropped grey entry makes the mark
  phase miss a subgraph, which would let the collector free something **live**.
  This was the most dangerous of the three.

**Also fixed: `gc_unregister_block` was O(live blocks).** It linear-scanned the
whole registry on every rc free -- and register/unregister run on *every* rc
alloc/free even with the collector disabled, so making the registry unbounded
would have turned that into O(N^2). Each `RcControlBlock` now stores its own
registry slot (`gc_index`, sentinel `RC_GC_INDEX_NONE`), making unregister an
O(1) swap-remove. This is a straight improvement over the pre-CG0 behavior.

**Both copies of the GC had to change.** The compiler emits its *own* copy of
the registry/suspect/grey machinery into every compiled program
(`emit_module.c`), separate from `src/runtime/gc.c`. Both were updated in step,
including the `RcControlBlock` layout (the new `gc_index` field), and all 140
`expected.c` snapshots regenerated in the same change.

Guarded by `tests/fixtures/gc-registry-growth`: 20000 simultaneously-live rc
blocks with the collector enabled must yield a peak registry size of 20000
(pre-CG0 it saturated at 4096).

*Note for anyone re-running the earlier leak probe:* with the registry now
unbounded, **LeakSanitizer no longer reports leaked cycle blocks at all** --
every block stays reachable from the registry, so the 4096-cliff artifact that
made leaks visible is gone. Measure cycle garbage with heap growth (or the
registry count), not LSan. See the Measured section of
`docs/reported/gc-strong-cycles-not-collected.md`.

### CG1 -- Candidate buffering (classic `PossibleRoot`) [DONE 2026-07-25]

**Landed.** `rc_strong_decrement` now calls `gc_possible_root` on the branch
where the count stays **> 0** -- the edge a self-sustaining cycle actually
produces, and the one the old zombie-only hook could never see. Gated on
`gc_mode` so the default (collector off) path costs a single global compare.

Two things this forced, both of which would have been bugs on their own:

- **O(1) dedup.** `gc_add_suspect` deduped with a linear scan of the whole
  buffer. That was tolerable when only zombies were buffered, but CG1 offers a
  candidate on *every* non-zero decrement, which would have made it quadratic.
  Each block now carries the classic Bacon-Rajan `buffered` flag
  (`RcControlBlock.gc_buffered`).
- **Freed blocks must leave the buffer.** Before CG1 only zombies were buffered,
  and they were freed through a path that had already removed them. Now ordinary
  blocks are buffered and are freed the moment their strong count hits 0, so
  `gc_unregister_block` drops the block from the candidate buffer -- otherwise
  the next collection would dereference freed memory.

That second change collided with the **emitted** trial-deletion loop, which
open-coded its own swap-remove instead of calling `gc_remove_suspect`: both ran,
`gc_suspect_count` decremented twice, and the loop walked off the buffer --
a segfault in three zombie fixtures (`gc-mixed`, `gc-cycle-freed`, `gc-stress`).
The emitted loop now uses `gc_remove_suspect` like the runtime copy does. Worth
noting the runtime `gc.c` was correct throughout; only the emitted copy diverged.

**This changes no collection outcome yet, by design.** The mark phase still
treats every `strong_count > 0` block as a root, so buffered candidates are
re-blackened each cycle and survive. CG2 (real trial deletion) is what turns
these candidates into reclaimed garbage; CG1 is the edge that makes them
*visible* to it.

Guarded by `tests/fixtures/gc-candidate-buffering` (suspect count 0 before a
strong cycle is built, > 0 after), validated by removing the hook and confirming
the fixture fails.

*Original phase text:*

Add the textbook Bacon-Rajan hook: on a strong *decrement that leaves the count
> 0*, color the block PURPLE and buffer it as a candidate root (dedup against
the buffer). This is the edge that a strong cycle actually produces. Keep the
existing zombie path for weak-referenced blocks. New entry point
`gc_on_strong_decrement_nonzero` (or fold both into one `gc_possible_root`)
called from `rc_strong_decrement` on the `strong_count > 0` branch.

### CG2 -- Real trial-deletion collection (the core)

Replace `gc_mark_phase` + `gc_trial_deletion_phase` with the three-sub-phase
Bacon-Rajan `MarkGray` / `Scan` / `CollectWhite` over the candidate set:

1. **MarkGray(root):** for each candidate, if WHITE-able, color GREY and, via
   the walker, *decrement a trial refcount* for each rc child, recursing. This
   subtracts internal (in-cycle) edges so that a block whose count reaches 0 is
   referenced only from within the candidate subgraph.
2. **Scan(root):** if trial count > 0, the block is externally reachable ->
   `ScanBlack` (restore counts, color BLACK). Else color WHITE (provisionally
   garbage) and recurse.
3. **CollectWhite(root):** free every still-WHITE block, running `drop_fn` and
   unregistering, iteratively (reuse the `rc_free_queue` flattening so deep
   cycles don't blow the C stack).

The trial refcount must not mutate the real `strong_count` observable by the
program mid-collection; use a scratch field (a spare `reserved[]` slot or a
parallel map). The walker already enumerates children for `RCK_STRUCT` and
`RCK_EXISTENTIAL`; CG3 widens that coverage.

### CG3 -- Walker completeness (close the blind spots that matter)

Audit every `rc_cb_alloc_*` site and ensure a `walk_fn` is attached wherever the
payload can hold rc children. Known gaps to check: boxed closures with captured
`rc<T>` children, `rc<Cons>` cells, boxed `rc<Result>`/`rc<Option>`. Blocks that
genuinely hold no rc children (scalars, C handles) stay `RCK_OPAQUE` with a NULL
walker. Document `RCK_OPAQUE` as the accepted blind spot (a cycle routed only
through a raw C handle is not collected -- exactly Rust's `*mut` situation).
Emit a lint/warning when a `defstruct` with rc fields is boxed without a walker.

### CG4 -- Correct weak handling in collection

When CollectWhite frees a cycle member that still has `weak_count > 0`, follow
the RC zombie contract instead of the current force-zero (`gc.c:335`): run the
value `drop_fn` and mark the control block a zombie (value logically dead) but
keep the block alive until the last `weak_decrement`, so `upgrade` returns none
rather than dangling. Aligns the collector with `rc_strong_decrement`'s existing
zombie discipline.

### CG5 -- Opt-in automatic trigger (`GC_AUTO`)

Add a `GC_AUTO` mode: run a collection at allocation checkpoints driven by a
heuristic (candidate-buffer high-water mark, plus an allocation-count/bytes
counter so a program that buffers few candidates but allocates heavily still
gets swept). Expose `(gc-auto!)` (or extend `(gc-enable!)` to select AUTO vs
THRESHOLD). This is the "automatically run" capability. **Gated** behind
`--enable=cycle-gc` with an `EXPERIMENTS[]` row and `experiment_warn_if_used`,
since it changes runtime timing/behavior. Default stays DISABLED.

### CG6 -- Observability

`gc-stats` intrinsic returning `gc_collections`, `gc_objects_freed`, live block
count, candidate high-water. An optional `TUR_GC_TRACE` env that logs each
collection (count in, freed, survived). This is what makes "run the suite and
see how much garbage accumulates" actually answerable -- today the counters
exist (`gc.c:37-38`) but nothing surfaces them.

### CG7 -- Test corpus + runtime leak-checking

- Promote `tests/fixtures/exg5-exists-cycle` from "documents non-collection" to
  a **collection** assertion (its own header already describes the topology).
- New fixtures: self-cycle, mutual `A<->B`, longer ring, cycle-plus-live-external
  root (must NOT collect), cycle through `vec`/`map`/closure payloads, cycle with
  a live `weak<T>` observer (CG4).
- The compiled program *runtime* is not leak-checked today (only `emit-c`/build
  is). Add a small opt-in harness that runs these specific binaries under
  `ASAN_OPTIONS=detect_leaks=1` with `--enable=cycle-gc` and asserts zero leaks,
  plus a companion run with the collector *off* asserting the cycle *does* leak
  (proving the collector is what reclaims it). Thread-safety of the global
  buffers under `arc<T>` is called out here as explicitly out of scope.

### CG8 -- (Stretch) graduation toward default-on

Only after CG1--CG7 are solid and measured: consider a default `GC_AUTO` behind a
longer bake. This is where the experiment graduates or is shelved per its
`expires_at`. Keep the zero-overhead pure-RC path available (`--disable` /
`(gc-disable!)`) regardless -- "reach past Rust" must never mean "lose Rust's
predictable no-GC baseline."

---

## Risks / open questions

- **Trial-count storage.** Where to stash the scratch refcount without growing
  every control block. Candidate: repurpose a `reserved[]` slot, or a
  side-table keyed by block pointer used only during a collection.
- **Pause time.** Synchronous collection over a large candidate set can stall.
  Incremental/generational refinements are out of scope but the heuristic in CG5
  should cap candidate-set size per cycle.
- **`RCK_OPAQUE` blind spot is permanent** without runtime type reflection -- and
  that's fine (documented, matches Rust). The lint in CG3 keeps it visible.
- **Interaction with the substructural path** (`^linear`/`^unique`): those values
  are single-owner and cannot form rc cycles, so they're orthogonal -- confirm no
  double-drop when a linear value is captured behind an rc.
