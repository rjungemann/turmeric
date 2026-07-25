# Cycle-Collecting GC -- Follow-up (automatic collection, observability, last replica)

**Status:** open. Successor to
[docs/archive/gc-cycle-collection-plan.md](../../archive/gc-cycle-collection-plan.md),
which shipped CG0--CG4 and the DEDUP-1--4b de-duplication and is now archived.

The collector works and compiled executables run the maintained copy of it.
What is left divides cleanly in three: **it never runs on its own** (CG5), **you
cannot see what it did** (CG6), and **two copies of it still exist** (DEDUP-5).
CG7's corpus and CG8's graduation sit on top of those.

---

## Where the predecessor left things

| capability | state |
|---|---|
| Reclaims live strong `rc<T>` cycles | **yes** -- Bacon-Rajan trial deletion; measured 192 B/cycle -> 0 |
| Live cycles survive collection | **yes** -- pinned by `gc-live-cycle-survives` and the runtime parity battery |
| Weak observers of a collected member | **yes** -- zombie discipline, both copies |
| Compiled executables run the runtime collector | **yes** -- DEDUP-4b, linked from `libturt_runtime.a` |
| `--shared` / bare `emit-c` | still the emitted replica (deliberately -- see DEDUP-5) |
| Runs without being asked | **no** -- CG5 |
| Reports what it did | **no** -- CG6 |
| On by default | **no** -- CG8, and deliberately so |

Five bugs came out of the duplication (CG1 double suspect-removal, CG3 `:heap`
mis-cast, CG4 weak force-free, the `gc_enable` mode gap, and the
`gc_enqueue_grey` colour downgrade). Three fences now hold that line and must
survive everything below: the `RcControlBlock` layout guard (DEDUP-1), the
`RC_VT_*` / `TypeKind` `_Static_assert` pair (DEDUP-4b), and
`tests/turi/gc-runtime-copy-parity.c` (DEDUP-4a). `tools/gc-copy-diff.py`
reports the remaining textual divergence.

---

## Phases

### CG5 -- Opt-in automatic trigger (`GC_AUTO`) [DONE 2026-07-25]

**Landed.** `(gc-auto!)` switches the collector to `GC_AUTO`, where collections
run at allocation checkpoints with no `(gc!)` anywhere in the program. Gated by
`--enable=cycle-gc` -- the registry's first active row since the last
graduation.

Measured, 3000 garbage cycles built with no explicit collection call:

| mode | collections | blocks freed | live blocks at exit |
|---|---|---|---|
| `GC_MANUAL` (`(gc-enable!)`) | 0 | 0 | 6000 |
| `GC_AUTO` (`(gc-auto!)`) | 46 | 5888 | **112** |

Two triggers, because either alone has a blind spot: the candidate buffer
reaching `GC_SUSPECT_THRESHOLD`, **or** `GC_AUTO_ALLOC_INTERVAL` allocations
since the last collection. A candidate-count trigger misses a program that
allocates heavily but buffers few candidates; an allocation-count trigger alone
makes a cycle-churning program wait out the full interval.

**The checkpoint is at ALLOCATION, never on the decrement path** -- the DEDUP-4a
constraint, honoured. At an allocation site no caller is mid-mutation.

**A crash fell out of it, and it is the interesting part of this phase.**
The first version put the checkpoint at the *end* of `gc_register_block`, which
reads naturally: register the block, then consider collecting. But
`rc_cb_alloc_kinded` registers a block **before its caller writes the payload**,
so the walker read uninitialised heap as a child pointer and segfaulted in
`gc_get_color` on `cb = 0x811`. Two fixes, both needed:

1. The checkpoint moved **ahead of** the registry insert, so the block being
   allocated right now is not walkable.
2. `rc_cb_alloc_kinded` **zeroes the payload under `GC_AUTO`**, which covers the
   other half: blocks already registered whose callers have not finished
   writing them either. The walker then sees a NULL child, which every walk
   callback already handles. Confined to AUTO so the always-on RC path keeps its
   `malloc` semantics.

This is worth remembering beyond CG5: **"registered" and "initialised" are not
the same instant**, and anything that can traverse the registry asynchronously
has to assume the gap exists. It is also a constraint on the C API rather than
the codegen -- `rc_set_value` repointing `value` after allocation leaves the
same window, so a block that will be repointed must still be allocated with a
payload wide enough for its declared kind. The codegen never repoints.

Pinned by `tests/fixtures/gc-auto-collects-without-gc-call` (end-to-end, no
`(gc!)` in the program) and two assertions in the runtime parity battery, one of
which allocates hard enough to fire many collections mid-construction -- a crash
there is the bug returning.

**Not done:** capping candidate-set size per collection. AUTO fires often enough
that each collection sees a small set in practice, but that is an emergent
property, not a bound. Pause time stays an open risk (below).

*Original phase text:*

Add a `GC_AUTO` mode that collects at allocation checkpoints on a heuristic:
candidate-buffer high-water mark, **plus** an allocation-count/bytes counter so
a program that buffers few candidates but allocates heavily still gets swept.
Expose it as `(gc-auto!)`, or extend `(gc-enable!)` to select AUTO vs THRESHOLD.

**Gated** behind `--enable=cycle-gc`: one fully-populated `EXPERIMENTS[]` row in
`src/runtime/experiments.c` (`name`, `summary`, `plan_path` -> this file,
`introduced`, `expires_at`, `lifecycle`, `opt_global`), plus
`experiment_warn_if_used("cycle-gc")` at the elaboration entry point. It changes
runtime timing and behaviour, which is exactly what the experiment gate is for.
Default stays `GC_DISABLED`.

Two constraints inherited from DEDUP-4a, both non-negotiable:

- **A refcount decrement must never trigger a collection.** `gc_add_suspect`'s
  watermark force-collect was removed precisely because it reentered the
  collector from inside `rc_strong_decrement`. AUTO's checkpoints belong at
  *allocation* sites, where no caller is mid-mutation.
- **Bounding peak suspect-buffer size is CG5's job**, not the decrement path's.
  That bound was given up when the force-collect went; this is where it returns.

Pause time is the open risk: synchronous collection over a large candidate set
stalls. The heuristic should cap candidate-set size per cycle. Incremental and
generational refinements stay out of scope.

### CG6 -- Observability

*Carried over unstarted, and now with a second reason to exist.*

A `gc-stats` intrinsic returning `gc_collections`, `gc_objects_freed`, live
block count and candidate high-water; an optional `TUR_GC_TRACE` that logs each
collection (count in, freed, survived). This is what makes "run the suite and
see how much garbage accumulates" -- the question that started this whole line
of work -- actually answerable.

The counters already exist in `src/runtime/gc.c`; nothing surfaces them. Two
wrinkles discovered since the original phase text:

- **The emitted replica has no counters at all.** So `gc-stats` reports real
  numbers on the archive path and zeroes on the `--shared` / `emit-c` path,
  unless DEDUP-5 retires the replica first or the counters are added to it.
  Sequencing CG6 after DEDUP-5 avoids the problem entirely.
- **`may_contain_cycles` is written but never read** -- by either copy. It is
  currently pure overhead. Either wire it into `gc_add_suspect` as a real filter
  (keeping scalar `rc<int>` out of the candidate machinery, which is a genuine
  optimisation and would show up directly in the CG6 numbers) or delete the
  field. Wiring it up changes what gets buffered, so it needs its own evidence;
  do it here, where the instrumentation to measure it exists.

### CG7 -- Test corpus + runtime leak-checking

*Partially done. `gc-live-cycle-survives` (the critical negative test) and the
DEDUP-4a runtime parity battery both landed.*

Remaining:

1. Promote `tests/fixtures/exg5-exists-cycle` from "documents non-collection" to
   a **collection** assertion. Its header already describes the topology.
2. Fixtures for cycles routed through `vec` / `map` / closure payloads. These
   assert the *documented non-collection* of the `RCK_OPAQUE` blind spot -- they
   exist so the blind spot's boundary is pinned rather than assumed, and so it
   is obvious if CG3's item 2 ever changes it.
3. An opt-in `ASAN_OPTIONS=detect_leaks=1` harness for the compiled GC binaries,
   with a **collector-off companion run asserting the cycle does leak**. The
   companion is the point: it proves the collector is what reclaims the cycle,
   rather than some incidental teardown. The compiled program *runtime* is not
   leak-checked today; only `emit-c`/build is.

Thread-safety of the global buffers under `arc<T>` remains **explicitly out of
scope** -- and note DEDUP-4a's removal of the reentrant force-collect makes the
buffers meaningfully easier to reason about if that scope ever opens.

### DEDUP-5 -- Retire the last replica

*New phase. DEDUP-4b took compiled executables off the hand-written collector;
two paths still carry it.*

Current state after DEDUP-4b:

| path | collector | why |
|---|---|---|
| `tur build` (executable) | archive | `tur` owns the link line |
| `tur build --shared` (.so) | replica | the archive is never injected into a shared library's link line; declaring without defining left the `.so` with undefined `rc_cb_alloc`, which a shared object tolerates at link time and only fails at dlopen |
| bare `tur emit-c` | replica | its contract is a self-contained translation unit |

Three pieces, in increasing order of difficulty:

1. **Elide the `#if 0` text.** The archive path still *generates* the whole
   replica and excludes it with `#if 0` -- roughly 500 dead lines in every such
   `.c`. Deliberate, so the text stayed readable next to the call sites while
   both implementations existed and the switch stayed a one-line revert. That
   has now baked; skip the emission instead.
2. **`--shared`.** Decide whether a `.so` should link the archive at all. It is
   not obviously right: a dlopened library carrying its own collector must not
   half-share one with its host, and a static archive linked into each `.so`
   gives every library its own GC state anyway. "Keep the replica here forever"
   is a legitimate answer -- but it should be a decision, not an accident.
3. **Bare `emit-c`.** Hardest, and possibly not worth it: its whole contract is
   standalone C. Options are to leave it, or to emit the runtime as a companion
   `.c` alongside the program.

Whatever happens, the three fences stay. As long as any replica exists,
`tools/gc-copy-diff.py` is the drift alarm and the parity battery is the
behavioural one.

### CG3 (residue) -- Walker lint

The one unstarted item from the original CG3: warn when a `defstruct` with `rc`
fields is boxed without a walker. Low priority -- the audit found the by-value
product path already covers every reachable struct shape, so the lint would fire
rarely. Its value is keeping the `RCK_OPAQUE` blind spot *visible* rather than
catching live bugs.

The blind spot itself (item 2 of the original CG3) stays **accepted**: a cycle
routed only through a raw C handle is not collected, exactly as in Rust with
`*mut`. Making collections walkable means teaching the walker about C-backed
storage -- worth doing only if a real consumer needs it.

### CG8 -- (Stretch) graduation toward default-on

Only after CG5--CG7 are solid and measured: consider a default `GC_AUTO` behind
a longer bake. This is where the `cycle-gc` experiment graduates or is shelved
per its `expires_at`.

Keep the zero-overhead pure-RC path available (`--disable` / `(gc-disable!)`)
regardless. "Reach past Rust" must never mean "lose Rust's predictable no-GC
baseline."

---

## Risks / open questions

- **Pause time** (CG5). Synchronous collection over a large candidate set
  stalls; the heuristic must cap candidate-set size per cycle.
- **`RCK_OPAQUE` blind spot is permanent** without runtime type reflection --
  and that is fine (documented, matches Rust). The CG3 lint keeps it visible.
- **Interaction with the substructural path** (`^linear` / `^unique`): those
  values are single-owner and cannot form rc cycles, so they are orthogonal --
  but confirm no double-drop when a linear value is captured behind an `rc`.
  Still unverified.
- **Two collectors in one process.** A Turmeric host that dlopens a Turmeric
  `.so` has two independent collectors with separate registries. Believed fine
  today because values do not cross the boundary; DEDUP-5 item 2 should confirm
  rather than assume it.

## Related plans

- [stdlib-weak-ref-audit-plan.md](stdlib-weak-ref-audit-plan.md) -- WR1/WR3/WR4
  outstanding; its cycle inventory seeds CG7's corpus.
- [turi-interp-incremental-reclamation-plan.md](turi-interp-incremental-reclamation-plan.md)
  -- TR1/TR3/TR5 outstanding.
- [docs/guides/gc-guide.md](../../guides/gc-guide.md) -- user-facing behaviour,
  including which copy of the collector each build path runs.
