# Cycle-Collecting GC -- Reaching Past Rust (CG0--CG8)

> **ARCHIVED 2026-07-25 -- superseded by
> [docs/upcoming/v1/gc-cycle-collection-followup-plan.md](../upcoming/v1/gc-cycle-collection-followup-plan.md).**
>
> This plan shipped **CG0--CG4** (dynamic buffers and registry, candidate
> buffering, real Bacon-Rajan trial deletion, walker audit plus the `:heap`
> fix, weak/zombie handling) and the whole **DEDUP-1--4b** de-duplication,
> which ends with compiled executables linking the maintained collector
> instead of a hand-written replica of it.
>
> Five bugs were found and fixed along the way, all of them consequences of
> the two GC copies drifting: CG1's double suspect-removal, CG3's `:heap`
> mis-cast, CG4's weak force-free, `gc_enable` never setting `gc_mode` (which
> made the collector a silent no-op for the interpreter and every embedder),
> and `gc_enqueue_grey` downgrading a just-marked-reachable block.
>
> **Carried forward** to the follow-up plan: CG5 (automatic trigger), CG6
> (observability), CG7's remaining corpus, CG8 (graduation), CG3's unwritten
> walker lint, and a new DEDUP-5 for the replica that `--shared` and bare
> `emit-c` still carry.
>
> Kept intact below as the paper trail -- the per-phase notes record what was
> measured, what was wrong about the plan, and why.

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

**De-dup investigated 2026-07-25 -- there is a hard blocker underneath it, and
it explains why the duplication exists at all.**

The obvious path looked easy: `--runtime=lib` already swaps emitted runtime
sources for an archive link, and the lean `libturt_runtime.a` just needs
`rc.c`/`gc.c` added to `TURT_RUNTIME_SOURCES` (currently only `hamt.c`,
`symbols.c`, `tur_string.c`). `emit_rt_global` already knows how to emit
`extern` declarations instead of definitions under `TUR_RT_OWNER`, so the
globals half is solved.

**But the two `RcControlBlock` layouts are not ABI-identical**, which makes
linking one against the other actively dangerous rather than merely tedious:

| field | emitted (`emit_module.c`) | runtime (`rc.h`) |
|---|---|---|
| `value_type_kind` | `uint8_t` (1 byte) | `TypeKind` (enum, 4 bytes) |
| `color` | `uint8_t` (1 byte) | `GcColor` (enum, 4 bytes) |

Every field after those sits at a different offset in the two versions. Linking
the runtime `rc.c`/`gc.c` into a compiled program today would silently mis-read
`gc_index`, `gc_buffered`, `gc_trial` and `gc_collecting` -- the exact class of
bug that CG3's `:heap` mis-cast turned out to be, but process-wide.

Compounding it, `rc.h` includes `types.h` (the compiler's type system, which
transitively pulls in `lifetimes.h` and friends), so a standalone compiled
program cannot simply include the real header. That dependency is almost
certainly *why* the emitted copy was hand-written with narrowed types in the
first place.

**Revised sequence** -- the de-dup is a prerequisite chain, not a one-step change:

1. **Reconcile the ABI first.** Give `RcControlBlock` fixed-width fields in both
   copies (`uint8_t` for the two enum-typed fields, or widen the emitted ones)
   and assert the layout with `_Static_assert` on `sizeof`/`offsetof` in both,
   so any future drift fails at compile time instead of at runtime.
2. **Make `rc.h`/`gc.h` standalone** -- no `types.h` dependency -- so an emitted
   program can include them.
3. Split the 31 emitted `static` GC/RC functions into declare-vs-define, reusing
   the `TUR_RT_OWNER` pattern already used for globals.
4. Add `rc.c`/`gc.c`/`rc_free_queue.c` to the lean archive and have AUTO link it.
5. Regenerate the 140 snapshots; full suite as the gate.

**Step 1 landed 2026-07-25.** `RcControlBlock.value_type_kind` and `.color` are
now `uint8_t` in `src/runtime/rc.h`, matching what the emitted copy always used,
so the two layouts finally agree. Both copies carry an identical 22-assertion
guard (`RC_LAYOUT_ASSERT`, in `rc.c` and emitted into the preamble) pinning each
field's width and the field ORDER via `offsetof` comparisons.

Deliberately no assertion on total `sizeof` or absolute offsets -- those are
padding/ABI-dependent and would fire on a different platform without indicating
real divergence. Written with the typedef trick rather than `_Static_assert`
because emitted programs compile as C99, and both copies carry the same text.

Validated by reintroducing the historical divergence (`value_type_kind` back to
4 bytes in the emitted copy only): the build now fails with
`size of array 'rc_layout_vtk_w' is negative` instead of silently mis-reading
every GC field. That is exactly the CG1/CG4 failure mode, now caught at compile
time.

**Step 2 landed 2026-07-25.** `rc.h` (and `gc.h`, which only includes it) are
now **standalone**: they no longer pull in `types.h` (which transitively drags in
`lifetimes.h` and the rest of the compiler's type system) or the unused
`arena.h`. Verified concretely -- a translation unit that includes both compiles
with `-I src/runtime` alone, no compiler headers on the path.

What made this possible: DEDUP-1 had already narrowed the stored field to
`uint8_t`, so the only remaining `TypeKind` uses were the three `rc_cb_alloc*`
parameters, which are now plain `uint8_t` (the emitted copy always declared them
`int`, so this also moves the two signatures closer). `TypeKind` has 64 members,
comfortably inside a byte -- checked, since DEDUP-1's narrowing would otherwise
have truncated silently.

The `TY_REF`/`TY_RC`/`TY_WEAK` switch in `default_drop_fn_for_type` still needs
the compiler's enum, so `types.h` moved into `rc.c`: the *header* is standalone,
the *implementation* may use whatever it likes. That separation is the whole
point -- a compiled program includes the header, never the .c.

This removes the structural reason the emitted copy had to be hand-written.

**Step 3 landed 2026-07-25.** The emitted rc<T>/GC functions now use the same
declare-vs-define split `emit_rt_global` has always used for state. Two new
helpers (`emit_rt_defs_begin` / `emit_rt_defs_end`) bracket the five runs of
definitions with `#ifdef TUR_RT_OWNER`, and `emit_rcgc_prototypes` emits the 48
prototypes every module TU needs. Guard runs bracket *definitions only* --
typedefs, the `RCK_*`/`RCEXP_*` `#define`s and the `emit_rt_global` state stay
outside, because every TU has to see those.

Linkage needed two prefixes rather than one, because the two families were
never uniform: `rcgc_helper` (`shared ? "" : "static "`) for the internal
helpers and `rcgc_api` (always `""`) for the `rc_*` / `tur_*_rc` API surface,
which already had external linkage in single-file mode. Inside the owner guard
both must be external so the other module TUs can reach them.

Measured on a synthetic 3-module `--shared` spice (each module returning
`rc<int>`):

| | before | after |
|---|---|---|
| copies of each GC/RC function in the `.so` | 3 (`t`, one per module TU) | 1 (`T`) |
| `.so` size | 35,680 B | 28,464 B (-20%) |

Single-file mode is untouched by construction (no guard is emitted at all):
**all 140 `expected.c` snapshots are byte-identical**, so this step needed no
regen. Full suite green at 2283 passed, 0 failed.

One incidental fix on the way: `tests/run-build-shared.sh`'s `nm | grep -q`
symbol check was flaky *by construction* -- the script runs under
`set -o pipefail`, and `grep -q` exiting on first match hands `nm` a SIGPIPE
(141), failing the pipeline even on a match. It reproduced 4 runs in 5 once the
`.so` layout shifted. Now captures `nm`'s output first.

Steps 4-5 (adding `rc.c`/`gc.c`/`rc_free_queue.c` to the lean archive, making
AUTO link it, regenerating snapshots) remain. Step 3 is what makes them
tractable: suppressing the emitted definitions entirely is now one more state
on the same switch, rather than a second surgery on 500 lines of `buf_puts`.

**Step 4 investigated 2026-07-25 -- it is not a build-system change, and the
sequence needs revising again.**

Adding the sources to the archive and flipping AUTO is two lines of CMake. But
what it actually does is **swap which implementation every compiled program
runs**: today compiled fixtures exercise the emitted copy exclusively and the
interpreter exercises the runtime copy exclusively. Linking the archive moves
1442 fixtures onto code they have never run. So the first thing Step 4 needs is
an answer to "how far apart are they?", and the answer is: **far**.

`tools/gc-copy-diff.py` (added here) extracts every top-level function from both
copies, normalises away comments, `static`, brace style and line wrapping, and
compares. Current state:

| | count |
|---|---|
| identical after normalisation | 19 |
| **divergent** | **23** |
| emitted only | 4 (`rc_cb_alloc_kinded`, `rc_cb_alloc_struct`, `tur_rc_from_ref`, `tur_ref_from_rc`) |
| runtime only | 12 (`gc_init`, `gc_shutdown`, `gc_possible_root`, `rc_cb_free`, the grey-queue and free-queue helpers, ...) |

Run `tools/gc-copy-diff.py --diff` for the deltas, `--count` for the number
alone. It is a *syntactic* comparison after normalisation -- it proves a
difference exists, not that the difference matters. Triage of the 23:

**Equivalent-but-rewritten (11)** -- no behavior change, would disappear if
either side were reflowed to match: `gc_get_color`, `gc_set_color`,
`gc_remove_suspect`, `gc_register_block` (the emitted copy redundantly re-zeroes
`gc_collecting`/`gc_trial`, which `rc_cb_alloc_kinded` already did; the capacity
constant differs in name only), `rc_upgrade`, `drop_ref_payload`,
`drop_rc_payload`, `drop_weak_payload`, `default_rc_drop_fn` (name only:
runtime calls it `default_drop_fn`), `__gc_mark_struct_child` (name only, plus
accessor helpers), `gc_on_strong_decrement` (the runtime's extra
enabled-guard and PURPLE set are both already done inside `gc_add_suspect`),
`rc_strong_decrement` (emitted calls `gc_add_suspect`, runtime calls
`gc_possible_root`, which is a two-line wrapper around `gc_add_suspect` with
the same guards).

**Unguarded coincidence (1)** -- `default_drop_fn_for_type`. The emitted copy
hardcodes `case 8/9/10`; `rc.c` spells them `TY_REF`/`TY_RC`/`TY_WEAK`. They
agree *today* (verified: 8, 9, 10) but nothing enforced it, and a TypeKind
reorder would have silently given every `rc<T>`/`weak<T>` in a compiled program
the wrong drop glue. Now fenced with a `_Static_assert` at the emission site,
validated by flipping the expected ordinal.

**Genuinely divergent behavior (8)** -- these are the real remaining work, and
each needs a verdict on which copy is right before any archive link:

| function | delta |
|---|---|
| `gc_enable` / `gc_disable` | **FIXED here** -- see below |
| `gc_add_suspect` | runtime force-collects (recursively!) at `GC_MAX_SUSPECTS` from inside a decrement; emitted just grows. Emitted's is the safer contract -- no reentrancy out of a refcount drop |
| `gc_mark_phase` | runtime's `gc_enqueue_grey` overwrites the caller's BLACK with GREY and dedups O(n); emitted keeps BLACK and appends. Each copy is internally consistent with its own `gc_trial_deletion_phase` -- they are two different algorithms, not one algorithm with a bug |
| `gc_trial_deletion_phase` | substantially different sweeps; the runtime frees the block when `weak_count == 0`, the emitted copy never frees `cb` in this legacy path |
| `rc_free_queue_push` | emitted: fixed 65536 array, `abort()` when full. runtime: dynamic, drains on full, then skips. Different data structures |
| `rc_free_queue_drain` | follows from the above; emitted open-codes the existential-payload release the runtime delegates to `rc_cb_free` |
| `rc_weak_decrement` | emitted open-codes the free; runtime calls `rc_cb_free` |
| `gc_collect` / `gc_cycle_collect_phase` | runtime maintains `gc_collections` / `gc_objects_freed`; the emitted copy has no counters at all, so **compiled programs have no GC statistics**. Feeds CG6 |
| `rc_cb_alloc` | signature differs: `int value_type_kind` vs `uint8_t value_type`. Harmless while the copies never link together; a genuine prototype mismatch the moment they do |

**A real bug fell out of this, fixed here.** `gc_collect()` gates on *both*
`gc_enabled` and `gc_mode`, and `gc_mode` starts at `GC_DISABLED`. The emitted
copy's `gc_enable()` has always defaulted the mode to `GC_MANUAL`; the runtime
copy set only the flag. Nothing else ever called `gc_set_mode`. So in the
interpreter / libturi / embedder path, `(gc-enable!)` followed by `(gc!)`
**collected nothing** -- `gc_enable()` lowers to the runtime function
(`src/turi/eval.c`), `(gc!)` to `gc_force()` -> `gc_collect()`, which returned
immediately. Confirmed directly: `gc_collections` stayed 0 after
`gc_enable(); gc_collect();` and became 1 only after an explicit
`gc_set_mode(GC_MANUAL)`.

This is the fourth bug from this duplication, and the first to have been
*silent on both sides* -- the compiled fixtures could not see it (they run the
emitted copy) and nothing exercised the runtime copy's mode contract. Fixed by
making the runtime match the emitted semantics, with
`tests/turi/gc-runtime-copy-parity.c` (ctest `tur_gc_runtime_copy_parity`)
pinning it; validated by reverting the fix and watching two assertions fail.

**Revised remaining sequence:**

- **4a. Reconcile the 8 behavioral deltas**, one verdict at a time, keeping
  `tools/gc-copy-diff.py --count` monotonically decreasing. The cosmetic 11 can
  be swept in whichever direction is convenient once the behavioral set is
  empty (it costs a 140-snapshot regen, so batch it).
- **4b. Then** add `rc.c`/`gc.c`/`rc_free_queue.c` to `TURT_RUNTIME_SOURCES`,
  extend the DEDUP-3 switch with a third "declare only, archive supplies the
  definitions" state, and make AUTO link it.
- **5.** Regenerate the 140 snapshots; full suite as the gate.

Doing 4b before 4a would swap all eight behavioral deltas into 1442 fixtures in
a single commit, which is precisely the failure mode the DEDUP series exists to
prevent.

**Step 4a landed 2026-07-25 -- and the triage above was wrong on its central
point. Correcting it here.**

The triage argued "adopt the emitted algorithm for `gc_mark_phase` /
`gc_trial_deletion_phase`, because the runtime's has never run." The premise
was right -- verified: nothing in the tree ever called `gc_init` or
`gc_set_mode`, so with the `gc_enable` bug in place `gc_mode` was never
anything but `GC_DISABLED` on that path. But the conclusion was an argument
from absence of evidence, so the first thing 4a did was *generate* the
evidence, and it went the other way.

`tests/turi/gc-runtime-copy-parity.c` now runs a real behavioral battery
against the runtime collector -- the first code ever to execute its mark and
trial-deletion phases:

| case | result |
|---|---|
| mutual `rc<T>` cycle, both external handles dropped | **collected** (`gc_objects_freed` +2) |
| same cycle, one external root retained | **survives**, edge intact |
| cycle member with a live `weak<T>` observer | **zombie** -- retained, `rc_is_alive` false, `rc_upgrade` NULL |

The runtime's algorithm satisfies the entire contract the emitted copy does,
including CG4's zombie discipline. It was never broken -- only unreachable.
So there is nothing to adopt: the two are different implementations of the
same contract, and since 4b's end state is "the archive supplies the
definitions", the runtime copy is the survivor by construction.

**That reframes 4a entirely.** It is not "make the two texts agree" -- it is
"prove the runtime copy satisfies the contract compiled programs depend on."
Re-triaging the eight deltas under that framing, seven need no code at all:

| delta | resolution |
|---|---|
| `gc_mark_phase`, `gc_trial_deletion_phase` | **no change** -- both correct; battery above is the proof |
| `rc_free_queue_push` / `_drain` | **no change** -- the runtime's dynamic, drain-then-skip queue is strictly better than the emitted copy's fixed 65536 array with `abort()` on overflow, and it is the survivor |
| `rc_weak_decrement` | **no change** -- `rc_cb_free` factoring; the zombie-observer case above exercises it |
| `rc_cb_alloc` / `rc_cb_alloc_kinded` signature | **no change** -- the runtime already uses `uint8_t`, matching the field width DEDUP-1 settled; the emitted `int` dies with the emitted copy |
| GC stats counters | **no change** -- the runtime already has `gc_collections` / `gc_objects_freed`; the *surface* stays CG6's |
| `gc_add_suspect` watermark force-collect | **FIXED, see below** |

Only one needed changing, and it is the one that was a genuine judgment call
rather than a matter of evidence: the runtime's `gc_add_suspect` called
`gc_collect()` at `GC_MAX_SUSPECTS` and recursed -- **reentering the whole
collector from inside `rc_strong_decrement`**, re-marking and potentially
freeing blocks while a caller was mid-decrement. The emitted copy has never
done this; it grows the buffer. Removed, per the emitted contract: a refcount
drop must never make a collection-policy decision on its own, which also keeps
it sane once `arc<T>` puts a second thread on these buffers.

What that gives up is a bound on peak suspect-buffer size under `GC_MANUAL`.
That bound belongs to CG5's automatic trigger -- "collect under memory
pressure" is policy, and policy does not belong on the decrement path.
`GC_MAX_SUSPECTS` is now unused by the collector; kept in `gc.h` only so it
does not drift from the emitted copy's identical constant while both exist.

Pinned by `decrement-past-watermark-never-collects` (pushes 5000 candidates,
asserts `gc_collections` never moves and the buffer grows past 4096, then that
one explicit collect still works). Validated by restoring the force-collect and
watching three assertions fail.

**Two smaller findings from the same pass:**

- `rc_set_value` has been defined in `rc.c` since the beginning but was never
  declared in `rc.h`, so every caller outside `rc.c` got an implicit
  declaration. Declared now.
- `may_contain_cycles` is **written but never read** -- by either copy. The
  emitted copy's `if (value_type_kind <= 7) cb->may_contain_cycles = false;`
  scalar optimisation is therefore inert, which is why it needed no porting.
  Wiring it into `gc_add_suspect` as a real filter (keeping scalar `rc<int>`
  out of the candidate machinery entirely) is a genuine optimisation, but it
  changes what gets buffered and needs its own evidence -- CG5/CG6 material,
  not 4a.

`tools/gc-copy-diff.py` also had a real flaw, found while using it: `FUNC_RE`
only matched single-line signatures, so every definition `rc.c` wraps at 80
columns was misfiled as "emitted only" -- including `rc_cb_alloc_kinded`, which
plainly exists in both. Fixed to join up to four lines when matching a header.
Corrected census: **21 identical, 25 divergent, 2 emitted-only, 11
runtime-only** (the two newly-visible divergences, `rc_cb_alloc_kinded` and
`rc_cb_alloc_struct`, are the `int` vs `uint8_t` signature and the inert
`may_contain_cycles` line).

The count does not drop much from 4a, and that is the honest outcome: the
remaining 25 are overwhelmingly *cosmetic* (brace style, accessor helpers,
`default_rc_drop_fn` vs `default_drop_fn`), and they disappear when the emitted
copy does -- not by being rewritten to match. **4a is complete.**

### 4b -- link the archive

**4b step 1 landed 2026-07-25 (and 4b is not "purely mechanical" -- correcting
that too).**

`rc.c`, `gc.c` and `rc_free_queue.c` are now in `TURT_RUNTIME_SOURCES`, so
`libturt_runtime.a` carries the reference counter and collector. Safe to land
ahead of the emit-side switch: a static archive member is only extracted to
resolve an *undefined* symbol, and the emitted copy still defines all of these
itself, so no existing link changes. Verified rather than assumed -- full suite
green, all 140 snapshots unchanged, and rc-heavy fixtures
(`byval-adt-local-owning-field-drop`, `backtrack-clone-rc`) built and run under
an explicit `--runtime=lib` against the new archive.

Getting there needed the last piece of DEDUP-2, which had been left half-done:
`rc.c` still `#include`d the compiler's `types.h` for the three `TypeKind`
ordinals its drop-glue switch dispatches on. That header is C11
(`static_assert`) and drags in the whole type system, so `rc.c` **would not
compile into the C99 archive at all**. Fixed by giving `rc.h` its own
`RC_VT_REF` / `RC_VT_RC` / `RC_VT_WEAK`, so the header *and* the
implementation are now standalone.

That leaves three spellings of the same three numbers -- the enum, `RC_VT_*`,
and the literals in the emitted switch -- so `emit_module.c` (the one place
that sees both the enum and `rc.h`) now carries a two-part `_Static_assert`
pinning enum-to-`RC_VT_*` and `RC_VT_*`-to-literals. Both validated by
perturbing each side and watching the right assertion fire.

**Gap analysis for step 2 (the emit-side declare-only switch).** Comparing
what the archive exports against what emitted code references, the swap is
*not* mechanical -- four things are missing or mismatched:

| gap | detail |
|---|---|
| `tur_rc_from_ref`, `tur_ref_from_rc` | **do not exist in the runtime at all** -- emitted-only. Must be added to `rc.c` |
| `default_rc_drop_fn` | emitted code names it this; the runtime's is `default_drop_fn` *and* `static`, so unexported. Needs renaming plus external linkage |
| `__gc_mark_struct_child` | emitted struct `walk_fn`s call this; the runtime's `gc_mark_struct_child` is `static` |
| `rc_free_queue` | the two copies give this name **incompatible types** -- a fixed `RcControlBlock *[65536]` array in the emitted copy, a struct in the runtime. So in archive mode the rc/gc globals must be *omitted entirely*, not `extern`-declared: a wrong `extern` here is worse than a missing one |

Plus every internal collector helper (`gc_vec_reserve`, `gc_add_suspect`,
`gc_remove_suspect`, `gc_enqueue_grey`, `gc_dequeue_grey`, `gc_mark_phase`,
`gc_trial_deletion_phase`, `gc_each_child`, the scan/mark-gray/collect-white
family) is `static` in `gc.c`. That is fine *if* no emitted module code calls
them -- which needs to be established from the linker's undefined-symbol list,
not from reading, since the preamble's own call sites are indistinguishable
from module call sites by grep.

Step 2 is therefore: export/add the four above, teach
`emit_rt_defs_begin`/`_end` and `emit_rt_global` a third "archive supplies
this" state, emit the prototypes unconditionally, resolve the archive at
*emit* time (the probe is a pure filesystem check, so it can move ahead of
codegen), and iterate against real link errors.

**4b step 2 landed 2026-07-25 -- archive mode works, behind `TUR_RCGC_FROM_ARCHIVE=1`.**

Two of the four "gaps" above turned out to be phantom, and measurement is what
showed it. Emitting every fixture and scanning what module code references
*after* the preamble ends -- 1859 fixtures -- gives the definitive set:

```
gc_all_blocks_count gc_disable gc_enable gc_force gc_suspect_count
rc_cb_alloc rc_cb_alloc_kinded rc_cb_alloc_struct rc_free_queue_drain
rc_get_value rc_strong_count rc_strong_decrement rc_strong_increment
rc_upgrade rc_weak_increment tur_rc_from_ref tur_ref_from_rc
```

`default_rc_drop_fn` and `__gc_mark_struct_child` never escape the preamble,
so their name/linkage mismatches with the runtime do not matter; nor does any
`static` collector helper. Both globals module code touches
(`gc_all_blocks_count`, `gc_suspect_count`) are already exported. That left one
real gap -- `tur_rc_from_ref` / `tur_ref_from_rc`, which existed *only* in the
emitted copy -- now ported to `rc.c`.

The port fixes a latent hazard on the way: the emitted `tur_rc_from_ref`
leaves `gc_trial` / `gc_collecting` to `gc_register_block`, which zeroes them
in the *emitted* collector but not in the runtime (which zeroes them in
`rc_cb_alloc_kinded`, a path `from_ref` bypasses). The port initialises all
four GC fields explicitly rather than inheriting that assumption.

Mechanism: `emit_set_rcgc_from_archive()` adds the third state to the DEDUP-3
switch. Definitions become `#if 0` blocks -- excluded, not deleted, so the text
stays readable next to the call sites while both implementations exist and the
switch is a one-line revert. `emit_rcgc_global` **omits** the rc/GC globals
rather than externing them, because `rc_free_queue` names incompatible *types*
in the two copies and a wrong `extern` links silently. The prototypes switch to
the archive's `uint8_t value_type` signatures, since a prototype that disagrees
with the real definition is a live ABI bug.

**Result: the full suite is behaviorally green under the archive link.**

| | |
|---|---|
| archive mode (`TUR_RCGC_FROM_ARCHIVE=1`) | 2143 passed, 140 failed |
| ... of which non-`codegen mismatch` | **0** |
| default mode | 2283 passed, 0 failed; 140 snapshots byte-identical |

The 140 are exactly the snapshot fixtures: snapshots track *default* emission,
so they mismatch whenever the opt-in flag is set. That regen is step 5, and it
happens when archive mode graduates to the default -- not before.

**One real divergence surfaced, and a fixture caught it.**
`exg5-walker-rc-payload` reads an inner block's GC color through inline C after
`(gc!)` and expects `GC_BLACK`; under the archive link it read `GC_GREY`. Root
cause: the runtime's `gc_enqueue_grey` set `GC_GREY` on enqueue, which **every
one of its three call sites immediately contradicted** -- each sets `GC_BLACK`
and then enqueues, so the enqueue silently downgraded a just-marked-reachable
block. The runtime stayed self-consistent (`gc_trial_deletion_phase` and
`gc_is_alive` both accept BLACK or GREY), which is why nothing caught it
before. Fixed by letting the caller own the color: "everything reachable is
BLACK after the mark phase" is the cleaner invariant and the one already
asserted. GREY goes back to being what it should be -- the trial-deletion
traversal's own color, set by `gc_mark_gray`.

This is the fifth bug from the duplication, and the first found by *running*
one copy against the other's test suite rather than by reading.

**4b graduated 2026-07-25 -- archive mode is the default for `tur build`.**

The flip is scoped by *who links*, which turned out to be the design decision
that mattered and made step 5's snapshot regen unnecessary:

| path | rc<T>/GC runtime | why |
|---|---|---|
| `tur build <file>`, `tur build <dir>` (executable) | **archive** | `tur` owns the link line |
| `tur build --shared` (.so) | emitted (DEDUP-3 owner-TU) | see below |
| bare `tur emit-c` | emitted | its whole contract is "here is a translation unit you will build yourself"; a declare-without-define preamble is not self-contained C |
| no archive locatable | emitted | degrades safely -- a toolchain without the archive still builds |
| `--runtime=source` | emitted | explicit opt-out |

Because bare `emit-c` keeps its definitions, **all 140 snapshots stayed
byte-identical** and step 5's regen never happened. Snapshots track `emit-c`,
and `emit-c` is exactly the path that must not change.

**`--shared` had to be excluded, and an existing test caught why.** The archive
is never injected into a shared library's link line, so declaring without
defining left the `.so` with undefined `rc_cb_alloc` / `rc_cb_alloc_struct` --
and a shared object *tolerates* unresolved symbols at link time, so this does
not fail the build. It fails at dlopen, or worse, silently binds to whatever
the host exports. `build-shared-rc-runtime` in `run-build-project.sh` asserts
exactly one owning definition of `gc_all_blocks` in the `.so` and reported 0.
Excluding `--shared` restores the previous, correct shape: a `.so` stays
self-contained on its owner-TU replica, which is also right on the merits --
a dlopened library carrying its own collector must not half-share one with its
host.

`locate_runtime_lib` also grew an installed-layout probe
(`<prefix>/bin/tur` -> `<prefix>/lib/libturt_runtime.a`), checked after the dev
layout so a build tree still wins over a stale system install. Without it the
new install rule was inert: the archive was only ever findable in a dev build
tree, so an installed toolchain both recompiled the runtime on every build
*and* kept running the emitted replica.

Verified with the flip on by default:

| | |
|---|---|
| `bash tests/run.sh` | 2283 passed, 0 failed |
| 140 `expected.c` snapshots | byte-identical |
| `run-build-project` / `run-build-shared` / `run-install` / `run-flags` | 27/0, 11/0, 29/0, 78/0 |
| interpreter ctests (incl. `tur_gc_runtime_copy_parity`) | 6/6 |
| executable links the runtime collector | confirmed via `nm` -- `gc_possible_root` and `rc_cb_free` exist **only** in the runtime copy |
| `TUR_RCGC_FROM_ARCHIVE=0` | replica restored, program still correct |

**The de-dup's goal is met for compiled executables: they now run the same
collector the interpreter does.** What remains is cleanup rather than
correctness:

- The emitted copy is still *generated* (as `#if 0` text) for the archive path
  -- ~500 dead lines in every such `.c`. Physically eliding it is a follow-up
  once this has baked.
- `--shared` and bare `emit-c` still carry a real replica, so
  `tools/gc-copy-diff.py` stays meaningful and the two copies must still not
  drift. The layout guard (DEDUP-1), the `RC_VT_*` fences (4b) and the parity
  battery (4a) are what hold that line.

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
