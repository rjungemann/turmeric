# Cycle-Collecting GC -- Reaching Past Rust (CG0--CG8)

> **Status:** CG0 + CG1 + CG2 landed 2026-07-25. **`(gc!)` now reclaims live
> strong `rc<T>` cycles** -- measured 192 bytes per cycle -> 0, with a
> collector-off control still leaking, so the collector is demonstrably what
> reclaims them. Turmeric can now do what Rust's `Rc`/`Arc` cannot: opt-in, off
> by default, zero-overhead when unused. CG3 is **audited but not complete**
> (see below -- its one live bug, `:heap` + `rc<T>`, is fixed); CG5
> (automatic trigger) and the rest are not started.
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

### CG2 -- Real trial-deletion collection (the core) [DONE 2026-07-25]

**Landed. `(gc!)` now reclaims live strong `rc<T>` cycles** -- the thing the
collector could never do. Measured on the same probe as the original report:
**192 bytes per cycle -> 0**, with a GC-off control still leaking ~192 B/cycle,
so the collector is demonstrably what reclaims it.

`gc_cycle_collect_phase` implements MarkGray / Scan / ScanBlack / CollectWhite
over the candidate roots CG1 buffers, then the pre-existing zombie sweep runs
unchanged. Three deliberate departures from the textbook, all safety-motivated:

1. **Trial decrements land on a scratch counter (`gc_trial`), never the real
   `strong_count`.** The textbook decrements the live count and restores it in
   ScanBlack; if a `walk_fn` were incomplete or asymmetric that would corrupt a
   live refcount. With a scratch copy the worst case degrades to a missed cycle
   (a leak), never a use-after-free.
2. **The white set is freed only after the whole traversal.** Freeing inside
   CollectWhite would let a later sibling read a freed block's color -- the
   classic algorithm frees in-traversal and gets away with it only because
   nothing re-reads a freed node; we do not rely on that.
3. **Members are flagged `gc_collecting` before their `drop_fn`s run.** A
   struct's drop glue decrements its rc children, which for a cycle are also
   being freed; `rc_strong_decrement` honours the flag by decrementing without
   freeing or buffering, so drop glue cannot double-free.

**The bug the fixtures caught:** the first implementation ran CollectWhite over
*every registered block* rather than over the drained candidate roots. Blocks
are born WHITE at registration, so that collected live values the collection had
never examined -- `gc-mixed` returned a garbage refcount for a live binding.
CollectWhite now starts only from the candidates, as the classic algorithm
specifies.

CG4's weak discipline came along for free: a collected block with
`weak_count > 0` keeps its control block as a zombie (value dropped, count
zeroed) so `upgrade` returns none rather than dangling.

Guarded by `tests/fixtures/gc-collects-strong-cycle` (5000 cycles built and
collected, heap growth must be exactly 0). All pre-existing GC fixtures
(`gc-mixed`, `gc-cycle-freed`, `gc-stress`, `gc-dag`, `gc-no-false-positives`,
`gc-deterministic`, `exg5-exists-cycle`) still pass unchanged.

**Still open:** the collector only sees what the walker sees, so a cycle routed
through an `RCK_OPAQUE` block remains uncollectable (CG3), and collection is
still driven manually by `(gc!)` / the suspect threshold (CG5).

*Original phase text:*

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

### CG3 -- Walker completeness (close the blind spots that matter) [AUDITED 2026-07-25]

**Audit result: struct-shaped cycle coverage is better than the plan assumed,
and the audit turned up a real bug instead.**

A walker is attached only when the boxed payload is a by-value product ADT with
drop glue (`emit_expr.c:6832-6845`); everything else boxes `RCK_OPAQUE` with a
NULL walker. Empirically probing which cycle *shapes* that actually covers:

| shape | collected? |
|---|---|
| two-node ring, one `rc` field each | yes (0 B retained) |
| three-node ring | yes |
| one node with **two** `rc` fields | yes |
| two **different** struct types, mutually referencing | yes |
| **`:heap`-annotated struct** with an `rc` field | **crashes** |

So the walker enumerates multiple fields, longer rings, and heterogeneous types
correctly -- the shapes the plan worried about are fine. What it found instead:
an `rc<T>` over a **`:heap`** struct hands `rc_strong_decrement` something that
is not a full `RcControlBlock`. Filed as
`docs/reported/gc-heap-struct-rc-not-a-control-block.md`. That confusion is
pre-existing and silent (a leak) with the collector off; CG1's `PossibleRoot`
hook reads a field far enough into the block to turn it fatal. Every existing GC
fixture uses `:move` structs, which is why nothing caught it.

**Remaining CG3 work, re-scoped by the audit:**

1. ~~Fix the `:heap` + `rc<T>` lowering and add a `:heap`-struct cycle
   fixture.~~ **DONE 2026-07-25.** Root cause was a double-indirection mismatch
   at the `rc/of` boxing site: a `:heap` ADT is already a pointer to its
   payload, so the generic "malloc a cell and store the value" boxing left
   `cb->value` a `T **` while every consumer cast it to `T *`. One fix
   (`emit_expr.c`, adopt the ctor's pointer when the payload is a heap ADT)
   resolved all four symptoms -- crash, mis-traced walker, mis-read fields, and
   a leaked struct per allocation. Archived at
   `docs/archive/gc-heap-struct-rc-not-a-control-block.md`; guarded by
   `tests/fixtures/gc-heap-struct-rc`.
2. Cycles routed through `RCK_OPAQUE` payloads (C handles, and collection
   buffers such as a `vec` of `rc<T>`) remain uncollectable. This is the
   **accepted** blind spot -- exactly Rust's situation with raw pointers -- and
   is documented as such in `docs/guides/gc-guide.md`. Making collections
   walkable would mean teaching the walker about C-backed storage; worth doing
   only if a real consumer needs it.
3. The lint the original phase called for -- warn when a `defstruct` with `rc`
   fields is boxed without a walker -- is still unwritten. Note the audit
   suggests it would fire rarely, since the by-value product path already covers
   the reachable struct shapes.

*Original phase text:*

Audit every `rc_cb_alloc_*` site and ensure a `walk_fn` is attached wherever the
payload can hold rc children. Known gaps to check: boxed closures with captured
`rc<T>` children, `rc<Cons>` cells, boxed `rc<Result>`/`rc<Option>`. Blocks that
genuinely hold no rc children (scalars, C handles) stay `RCK_OPAQUE` with a NULL
walker. Document `RCK_OPAQUE` as the accepted blind spot (a cycle routed only
through a raw C handle is not collected -- exactly Rust's `*mut` situation).
Emit a lint/warning when a `defstruct` with rc fields is boxed without a walker.

### CG4 -- Correct weak handling in collection [DONE 2026-07-25]

**Landed.** The legacy zombie sweep in `src/runtime/gc.c` force-freed a block
that still had live `weak<T>` observers -- it zeroed `weak_count` and freed the
control block, so the next `upgrade()` (or the final `rc_weak_decrement`) read
freed memory. It now drops the *value* but keeps the control block alive while
any weak reference can still observe it; `rc_weak_decrement` frees it when the
last one goes.

Two things worth recording:

- **CG2's resolution note overclaimed this.** CG2 gave the zombie discipline to
  its new cycle-collection phase only; this legacy path was untouched. The
  archived report has been corrected rather than left to imply a fix that had
  not happened.
- **The two GC copies had diverged again, with the runtime being the wrong
  one.** The emitted preamble in `emit_module.c` had always kept the block for
  weak refs, so no compiled fixture could ever catch this -- the defect was
  reachable only through the runtime library (the interpreter and libturi
  embedders). CG1's divergence ran the other way. Divergence between the two
  copies has now caused three separate bugs; a shared-source or generated-from-
  one-source arrangement is worth considering (noted for CG7).

Verified with a runtime-level ASan test: after collection `rc_is_alive` is
false, `rc_upgrade` returns NULL rather than dangling, the block survives with
`weak_count == 1`, and the final `rc_weak_decrement` frees it cleanly. No
snapshot churn -- this copy is not emitted.

*Original phase text:*

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

### CG7 -- Test corpus + runtime leak-checking [IN PROGRESS 2026-07-25]

**Done so far: the missing negative test.** `tests/fixtures/gc-live-cycle-survives`
covers the direction that matters most and was previously untested -- a cycle
still reachable from a **live external root** must NOT be collected. This is the
failure mode that is far worse than a leak: MarkGray subtracts the in-cycle
edges, and if an external reference is not correctly accounted for, the cycle
falls to a zero trial count and is freed while the program still holds it -- a
use-after-free on live data. The pre-existing `gc-no-false-positives` only
covered a plain live `rc` with no cycle at all.

The fixture asserts both halves of the correctness statement: with roots live
the cycle survives collection *and stays readable afterwards* (reading through
it is what would expose a wrongly-freed block), and with roots gone the same
shape is reclaimed. Verified clean under ASan as well as in the suite.

**Assessment: de-duplicating the two GC copies (added to this phase).**
Divergence between `src/runtime/gc.c` and the copy emitted by
`emit_module.c` has now produced **three** bugs -- CG1's double suspect-removal
(emitted copy wrong), CG3's `:heap` mis-cast (surfaced differently per copy), and
CG4's weak force-free (runtime copy wrong). Each was invisible to half the test
suite *by construction*: compiled fixtures exercise only the emitted copy, and
the interpreter/libturi embedders exercise only the runtime copy.

Scale of the duplication: ~949 lines of `gc.c` + `rc.c` against ~436 `buf_puts`
lines emitting equivalent logic. Every GC change so far has had to be written
twice, by hand, in two different notations.

A concrete de-dup path exists and is worth costing out: `libturi.a` already
exports `gc_collect`, and `--runtime=lib` already replaces emitted runtime
sources with an archive link. What blocks it today is that AUTO mode links only
the **lean** `libturt_runtime.a`, which does not contain the GC (verified: zero
GC symbols). So the shape of the fix is *"move rc/gc into the lean runtime
archive and have the preamble link it instead of inlining a copy"* -- not a new
mechanism, just extending one that exists. That would also delete the
`RcControlBlock`-layout-must-match-in-two-places hazard that CG0/CG1/CG2 each had
to navigate.

**Still to do in CG7:** promote `exg5-exists-cycle` to a collection assertion;
fixtures for cycles through `vec`/`map`/closure payloads (currently the
`RCK_OPAQUE` blind spot -- these would assert the *documented* non-collection);
the opt-in `detect_leaks=1` harness for compiled binaries with a collector-off
companion run. Thread-safety of the global buffers under `arc<T>` remains
explicitly out of scope.

*Original phase text:*

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
