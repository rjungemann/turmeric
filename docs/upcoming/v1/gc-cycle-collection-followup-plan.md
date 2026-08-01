# Cycle-Collecting GC -- Follow-up (automatic collection, observability, last replica)

**Status:** open -- CG5, CG6, CG7, DEDUP-5 and PT1/PT2 (pause time) are done;
CG3's residue is closed as a compiler bug (filed, not linted); CG8
(graduation) is deliberately held until `cycle-gc` has baked to its 0.34.0
review point, and two of the three things it said to weigh are now measured.
Successor to
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
| On by default | **no, and never** -- a permanent design decision, not a pending phase; see CG8 |

Five bugs came out of the duplication (CG1 double suspect-removal, CG3 `:heap`
mis-cast, CG4 weak force-free, the `gc_enable` mode gap, and the
`gc_enqueue_grey` colour downgrade) -- **six** counting PT1, where the runtime
copy carried a redundant dedup scan the emitted copy never had and paid for it
with a quadratic. Note the direction: every earlier divergence made the runtime
copy *wrong*, this one made it *slow*, which is why nothing behavioural caught
it. Three fences now hold that line and must
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
payload wide enough for its declared kind. The codegen repoints only via
`rc_set_value`, in the allocation-free window right after `rc_cb_alloc`, where
no AUTO checkpoint can fire.

Pinned by `tests/fixtures/gc-auto-collects-without-gc-call` (end-to-end, no
`(gc!)` in the program) and two assertions in the runtime parity battery, one of
which allocates hard enough to fire many collections mid-construction -- a crash
there is the bug returning.

**Not done, and now superseded:** capping candidate-set size per collection.
That cap was never the pause-time fix -- see PT1/PT2 below, where the actual
term was measured and removed. Left uncapped deliberately.

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

### CG6 -- Observability [DONE 2026-07-25]

**Landed.** Four reader intrinsics -- `(gc-collections)`, `(gc-objects-freed)`,
`(gc-live-blocks)`, `(gc-candidate-high-water)` -- plus `TUR_GC_TRACE=1` for a
line per collection on stderr:

```
[gc] #1 mode=3 candidates=128 freed=128 live=128->0
```

Four readers rather than one `(gc-stats)` record: each is a plain count, so
`:int` is the honest type here, not an `:int` standing in for something
structured. A record would have to be boxed by an inline-C body for no gain,
and a caller who wants one can define it in Turmeric over these. They are
**ungated**, unlike `(gc-auto!)` -- reading a counter changes no behaviour, and
someone deciding whether the experiment is worth enabling needs the numbers.

`gc_candidate_high_water` is new. It is the counter that answers "is the
collector keeping up?": `gc_suspect_count` is instantaneous and sits near zero
right after any collection, so sampling it tells you almost nothing.

**This finally answers the question the whole line of work started from.**
A cycle-churning program under `GC_AUTO`, 50,000 two-node cycles:

| collections | blocks freed | live at exit | candidate high-water |
|---|---|---|---|
| 781 | 99,968 | **32** | **128** |

The high-water pinning at exactly `GC_SUSPECT_THRESHOLD` (128) is the
informative number: the candidate trigger is what fires for this shape, and the
buffer never grows past it, so the collector is keeping up exactly. The
allocation-interval trigger is the safety net for the other shape.

**`may_contain_cycles` is resolved -- wired up, not deleted.** A block with no
rc children cannot be a cycle *root*, so buffering it costs a slot plus a walk
per collection for nothing. `gc_add_suspect` now skips those, and the runtime
marks scalar payloads (`value_type < RC_VT_REF`) the way the emitted copy always
did. Measured on 5000 scalar `rc<int>` clone/drop pairs: candidate occupancy
**5000 -> 0**.

Worth recording how that measurement went, because the first one was wrong. A
probe written in Turmeric reported 0 either way, which read as "scalars never
get buffered, the filter is pointless". That was **last-use elision deleting
the clone/drop pair** -- the collector was never exercised. The C-level probe,
where the clone/drop is real, showed 5000. The same mistake then recurred in the
test: the first version asserted on the high-water mark, which is a running
maximum over the process, so a late +1 could never move it. Instantaneous
occupancy is the right instrument for "did this block get buffered".

**The replica got the counters too.** It had none at all, so `(gc-collections)`
would have reported real numbers on the archive path and zeroes on `--shared` /
bare `emit-c`. A statistic that silently lies is worse than one that is absent,
so rather than sequencing CG6 behind DEDUP-5 as this plan originally suggested,
the counters, the high-water and the trace are mirrored into the emitted copy.

Pinned by `tests/fixtures/gc-stats-observability` (shape assertions, so it does
not re-pin collector internals) and three runtime parity assertions.

*Original phase text:*

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

### CG7 -- Test corpus + runtime leak-checking [DONE 2026-07-25]

**Landed, with two of the three items ending somewhere other than planned.**

**1. `exg5-exists-cycle` promoted.** It was written before CG2, when trial
deletion was zombie-only, so all it could assert was that the `RCK_STRUCT`
walker ran without tripping. It now asserts the cycle is *reclaimed*
(`gc-objects-freed > 0`, `gc-live-blocks == 0`) using the CG6 counters, and its
header no longer claims the collector cannot do this.

**2. The `vec`/`map`/closure blind-spot fixtures could not be written -- because
the blind spot is not where the plan thought.** Measured:

| shape | result |
|---|---|
| `vec` holding `rc<T>` | **does not compile** -- `emit: invalid EX_REINTERPRET rc -> int` |
| `map`/`hamt` holding `rc<T>` | **does not compile** -- same error |
| closure capturing `rc<T>` | **compiles, and the capture is released correctly** (live blocks back to 0 across a collection) |

So there is no reachable `RCK_OPAQUE` cycle to assert non-collection *for*: the
collection cases are closed by rejection rather than by tracing, and the closure
case is not a blind spot at all. (An earlier probe that appeared to show a
closure leak was an ordinary refcount leak in the probe itself -- a `rc/clone`
inside the closure body that nothing dropped.)

That is a better outcome than the fixtures would have been, but it comes with a
real expressiveness hole, filed as
[docs/archive/history/collections-cannot-hold-rc-values.md](../../archive/history/collections-cannot-hold-rc-values.md).
The blind spot documented in the GC guide reopens the moment collections accept
`rc<T>`, and the fixtures become writable and necessary at the same instant.

**3. The leak harness became an ASan harness, because LeakSanitizer cannot
express the assertion.** `tests/run-gc-leak-gate.sh`.

LSan reports memory unreachable from roots, and every rc block sits in the
`gc_all_blocks` registry -- a global. An uncollected cycle is therefore
*reachable* by construction. Measured: the collector-off run retains ~1 MB of
cycles and LSan reports **zero leaks**. The planned "collector off must leak"
assertion is not expressible with LSan at all.

What the gate does instead is the part that was actually missing: `tests/run.sh`
compiles fixture programs **without sanitizers**, so nothing in the ordinary
suite ever runs a compiled binary under ASan. A use-after-free in the
collector's sweep is invisible to a suite that only diffs printed output --
CG5's mid-construction walk of a half-built block was exactly that shape. The
gate builds each fixture with `-fsanitize=address` and runs it twice, asserting
ASan-clean both ways, expected output with the collector on, and *different*
output with it off.

A second measurement trap surfaced while building it: **`mallinfo2` reads
glibc's allocator, which ASan replaces**, so a fixture measuring retention that
way reports 0 under ASan no matter what. `gc-collects-strong-cycle` prints a
1,087,232-byte delta with the collector off under normal flags and 0 under ASan
-- its heap probe is simply blind there. The on/off control therefore runs only
on fixtures whose output comes from the CG6 counters, which are allocator-
independent; `gc-collects-strong-cycle` still gets the ASan-clean checks.

**Amended 2026-07-29.** That exclusion was drawn one check too narrowly: the
*expected-output* check kept running on the same blind probe. On glibc it passed
vacuously (0 compared against an expected 0 the collector had no part in
producing); on Darwin the probe is not blind but quarantine-inflated -- ASan's
freed blocks stay accounted in `malloc_zone_statistics`, so the fixture printed
~800000 and the check failed for a reason unrelated to the collector. A
probe-output fixture is now excluded from **both** output checks and prints a
visible SKIP line for each; `tests/run.sh` (unsanitized) remains where that
assertion actually lives. See
[docs/archive/history/gc-leak-gate-darwin-sanitized-probe-drift.md](../../archive/history/gc-leak-gate-darwin-sanitized-probe-drift.md).

Result: 14 passed, 2 skipped, 0 failed (was 11 passed when first measured; the
fixture set has grown since). Opt-in (`bash tests/run-gc-leak-gate.sh`) -- a
sanitized compile per fixture is slow, and it is a diagnostic gate rather than
an everyday one.

Thread-safety of the global buffers under `arc<T>` remains **out of scope**.

*Original remaining items:*

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

### DEDUP-5 -- Retire the last replica [DONE 2026-07-25]

**Landed, as two of three: the dead text is gone, `--shared` is now a defended
decision rather than an accident, and bare `emit-c` keeps its replica by
design.**

**1. The `#if 0` text is elided.** DEDUP-4b emitted the whole replica and
excluded it with `#if 0`, so it stayed readable next to its call sites while
both implementations existed and the switch was a one-line revert. That has
baked. `emit_rt_defs_begin` now records `out->len` and `emit_rt_defs_end`
rewinds to it, discarding the run outright -- `Buf` is length-tracked with no
NUL invariant, so truncation is the whole operation and no `Buf` change was
needed. **7428 -> 6915 lines** per generated `.c` (513 lines, ~7%).

**2. `--shared` keeps its replica, and that is now defended.** The question was
whether a `.so` should link the archive. Measured first:

| host | result |
|---|---|
| links `libturt_runtime.a`, dlopens the `.so` | **separate collectors** |
| same, linked `-rdynamic` | **separate collectors** |

Separate is the right answer -- a control block registered in one registry must
never be freed through the other, since `cb->gc_index` would index the wrong
array -- but it was true only because the host's symbols happened not to
interpose. The `.so` was still *exporting* `gc_collect`, `rc_cb_alloc`,
`gc_all_blocks` and friends as global dynamic symbols, so a differently-linked
host, or `RTLD_GLOBAL`, could have partially merged the two.

So the rc/GC block in a `--shared` build is now emitted with
`__attribute__((visibility("hidden")))` (via a `TUR_RT_LOCAL` macro emitted only
in shared mode, so single-file output and its 140 snapshots are untouched).
Result: **zero** exported `gc_*`/`rc_*` dynamic symbols, module exports intact,
and still exactly one collector instance per library -- DEDUP-3's win kept.
Structural now, not lucky.

**3. Bare `emit-c` keeps the replica, deliberately.** Its contract is a
self-contained translation unit; a declare-without-define preamble is not that.
The alternative -- emitting the runtime as a companion `.c` -- turns a
one-file output into a two-file one and would break every caller that pipes
`emit-c` to a single file. `TUR_RCGC_FROM_ARCHIVE=1` remains available for
someone who links `libturt_runtime.a` from their own build system.

**Consequence for the fences.** A replica still exists on two paths, so
`tools/gc-copy-diff.py` (27 divergent, all cosmetic), the DEDUP-1 layout guard,
the DEDUP-4b `RC_VT_*` asserts and the DEDUP-4a parity battery all stay load-
bearing. This phase reduced the replica's blast radius; it did not remove it.

*Original phase text:*

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

### CG3 (residue) -- Walker lint [RESOLVED 2026-07-25 -- do not write it]

**The lint's target shape exists, and it is a compiler bug rather than a lint.**

Before writing "warn when a `defstruct` with rc fields is boxed without a
walker", the question was whether it could ever fire. It can, and what it would
be warning about is a silent leak:

| shape | boxed as | live blocks after `(gc!)` |
|---|---|---|
| by-value product with an `rc` field | `rc_cb_alloc_struct(..., drop_glue, walk_glue)` | 0 |
| **multi-variant sum with an `rc` field** | `rc_cb_alloc(0, 19, NULL)` | **1** |

`(rc/of v)` selects glue on `needs_drop_glue && adt_is_byvalue_product(adef)`.
A sum satisfies the first half and fails the second, so it falls to the `else`
branch with `drop_fn_name` still at its initial `"NULL"` -- no drop glue *and*
no walker. The `rc` payload is never released (a leak with the collector off)
and the walker cannot trace through the box (a blind spot with it on).
`option<T>` and `result<T,E>` are multi-variant, so this is not an exotic
corner.

**Fixed 2026-07-25**, archived at
[docs/archive/rc-of-sum-type-drops-no-glue.md](../../archive/rc-of-sum-type-drops-no-glue.md).
The glue now dispatches on the tag, and -- the part that was easy to miss --
the boxing had the same double-indirection as the `:heap` bug, so emitting glue
alone did not fix the leak. Both halves verified: the field is released, and a
cycle routed *through* a boxed sum is collected.

A lint is the wrong response: this is not a questionable-but-valid pattern for
the user to reconsider, it is an ordinary program compiled into a leak. Emit
glue for sums; the lint then has nothing to warn about. **This item is closed --
do not implement the lint.**

*Original item:*

The one unstarted item from the original CG3: warn when a `defstruct` with `rc`
fields is boxed without a walker. Low priority -- the audit found the by-value
product path already covers every reachable struct shape, so the lint would fire
rarely. Its value is keeping the `RCK_OPAQUE` blind spot *visible* rather than
catching live bugs.

The blind spot itself (item 2 of the original CG3) stays **accepted**: a cycle
routed only through a raw C handle is not collected, exactly as in Rust with
`*mut`. Making collections walkable means teaching the walker about C-backed
storage -- worth doing only if a real consumer needs it.

### PT1/PT2 -- Pause time [DONE 2026-07-26]

**Landed, and the premise this plan carried for three phases was wrong.**

CG5 left "cap candidate-set size per collection" as the pause-time fix, and the
risks list repeated it. Before writing the cap, the two variables were
separated and measured -- one collection, `GC_MANUAL`, timed directly:

| held fixed | swept | pause |
|---|---|---|
| live heap 2,000 | candidates 128 -> 16,384 | 0.78 ms -> 2.16 ms |
| candidates 512 | live heap 1,512 -> 128,512 | 0.22 ms -> **2977 ms** |

Capping candidates would have done essentially nothing. A collection's cost was
**quadratic in the live heap**, and a 128k-block heap stalled for three seconds
on a collection that freed 512 blocks.

**PT1 -- the quadratic.** `gc_enqueue_grey` did a linear "already queued?" scan
of the grey queue on every enqueue, and `gc_mark_phase` enqueues *every*
strong-rooted block -- so an O(queue) scan per enqueue over O(live) enqueues.
The scan could never fire: every call site colours a block `GC_BLACK` before
enqueuing and refuses to enqueue one already black, so a duplicate is never
offered. The colour was always the dedup. Removed.

The emitted replica in `emit_module.c` has **never** had that scan, so this was
a live DEDUP divergence in which the runtime copy was the slow one -- the fix
brings the runtime in line with the emitted copy, not the reverse.

| live blocks | with scan | without |
|---|---|---|
| 8,512 | 14.2 ms | 0.17 ms |
| 32,512 | 177.7 ms | 1.14 ms |
| 128,512 | **2977.5 ms** | **11.11 ms** |

**PT2 -- the remaining whole-heap walk.** With PT1 in, a phase breakdown showed
where the rest went: at 256k live blocks a collection spent **34.0 ms in
`gc_mark_phase` and 0.000 ms in `gc_trial_deletion_phase`**. That pair is the
zombie sweep, and `gc_trial_deletion_phase` reads the candidate buffer and
nothing else -- while `gc_cycle_collect_phase` has just *drained* every
candidate with `strong_count > 0`, so what remains is exactly the zombie set.
With no zombies the sweep is a no-op that still walks the whole registry to
produce colours for it to read. `gc_collect` now skips the pair when that drain
leaves the buffer empty, in both copies.

| | 128k live, 512 candidates | 256k live |
|---|---|---|
| before | 2977 ms | -- |
| PT1 | 11.1 ms | 37.3 ms |
| PT1+PT2 | **2.09 ms** | **4.28 ms** |

**PT2 has one visible consequence, and a fixture caught it.** The claim "nothing
reads those colours" is true of the *runtime* -- `gc_is_alive` is the only
reader outside the collector and has no callers; `rc_is_alive`/`rc_upgrade` test
`strong_count`. But colours are observable to inline C, and
`tests/fixtures/exg5-walker-rc-payload` reads one as a white-box probe of the
EXG5 walker. That program buffers no candidates at all (its `rc<int>` is scalar,
so `gc_add_suspect` filters it), so the skip left it reading WHITE and the
fixture went red. The walker is unchanged; what changed is whether a program
with nothing to sweep runs one. The fixture now leaves a real zombie
(`strong_count 0`, `weak_count 1`) in the buffer first, which restores the
precondition its assertion was always assuming.

**Still O(live), and left that way:** `gc_cycle_collect_phase` reseeds
`cb->gc_trial` over the whole registry (3.26 ms of the 4.28 ms above). Linear,
not quadratic, and removing it needs lazy per-block seeding behind an epoch
stamp -- a new field, in a struct pinned by the DEDUP-1 layout guard, in both
copies. Not worth it at these numbers; the note is here if it ever is.

### CG8 -- Ungating `(gc-auto!)` [NOT YET -- deliberately]

**Scope, decided 2026-07-30 -- read this before anything below.** This phase
was originally titled "graduation toward default-on", which conflated two
decisions that are not the same and do not travel together:

1. **Ungating `(gc-auto!)`** -- deleting the `cycle-gc` row so the call form
   works without `--enable`. This is what the registry row actually controls
   (`src/compiler/elab_memory.c:605`), and it is the only thing CG8 decides.
2. **Making `GC_AUTO` the default collection mode.** **REJECTED, permanently.**
   Not deferred, not "behind a longer bake" -- not happening, before or after
   v1. Automatic GC is opt-in in this language, full stop. A program that never
   calls `(gc-auto!)` gets the pure-RC path with no collector overhead, and
   that stays true after CG8 lands.

The two are independent because the gate is on the *call form*, not on a
default: `(gc!)`, `(gc-enable!)` and the CG6 stat readers are always available
regardless of the row, and the `rc_cb_alloc_kinded` payload zeroing is
conditional on `GC_AUTO` mode **at run time**. So ungating costs a non-calling
program exactly nothing, and no amount of bake time on (1) ever adds up to (2).

Read the measurements below accordingly: they are inputs to "is this call form
ready to be unflagged", never to "should everyone get it".

That settled, what remains for CG8 is timing. `cycle-gc` was introduced at
**0.30.8**, the same day CG5 landed, and graduating an experiment on its
introduction day defeats the point of having gated it. Its `expires_at` is
**0.34.0**, which the release-cut skills surface as a review point (advisory --
per CLAUDE.md it never blocks a release), and that is the right moment to
decide, with several releases of bake behind it.

What ungating should weigh when it comes up:

- **Pause time** -- **measured and fixed, see PT1/PT2 above.** No longer the
  open risk it was: the quadratic is gone and a collection no longer walks the
  live heap when there is nothing to sweep. What remains is one linear
  whole-registry pass (the `gc_trial` reseed), still uncapped. That is a real
  number to weigh at graduation rather than an unknown.
- **The `rc_cb_alloc_kinded` payload zeroing** CG5 added under AUTO --
  **measured.** 1M allocations, alloc path only, best-of-5:

  | payload | with zeroing | without | delta |
  |---|---|---|---|
  | 8 B | 101.0 ms | 88.3 ms | +12.7 ns/alloc |
  | 24 B | 102.0 ms | 93.7 ms | +8.4 ns/alloc |
  | 64 B | 117.8 ms | 111.5 ms | +6.2 ns/alloc |
  | 256 B | 216.2 ms | 211.8 ms | +4.4 ns/alloc |

  ~10% of the rc allocation path. The informative part is that the delta
  *shrinks* as the payload grows: this is fixed overhead, not memset bytes. So
  the alternative CG8 named -- a per-block "under construction" flag -- would
  trade one fixed per-allocation cost for another (a byte write at alloc, a
  branch per walk visit) and is not obviously a win. Worth re-measuring against
  a real implementation before assuming it helps.
- **CG6's numbers on real programs** -- **first real-shape run done, and it
  found a leak that is not the collector's.** A workload with program shape
  rather than synthetic churn (parent/child back-references at uneven fanout,
  mixed with acyclic chains and long-lived data), 4000 rounds under `GC_AUTO`
  with no `(gc!)`:

  | half of the workload | collections | freed | live at exit |
  |---|---|---|---|
  | cyclic families | 63 | 7940 | **60** |
  | acyclic chains | 31 | **0** | **16000** |

  The collector is doing its job -- every cycle reclaimed, 60 blocks live at
  exit. The entire 16000-block residue was `set!` on an `^mut` binding holding
  `rc<T>` never releasing the overwritten value: 4000 rounds x 4 assignments,
  exactly. Acyclic and `strong_count > 0`, so no collector could reclaim it.
  **Since FIXED** -- archived at
  [docs/archive/history/set-bang-does-not-release-old-rc-value.md](../../archive/history/set-bang-does-not-release-old-rc-value.md).
  Re-measured after the fix, same workload:

  | half of the workload | collections | freed | live at exit |
  |---|---|---|---|
  | acyclic chains | 4 | 0 | **0** (was 16000) |
  | full mixed workload | 63 | 7938 | **62** (was 16050) |

  So the collector's own steady-state residue on this shape is ~60 blocks, and
  it is no longer masked by an unrelated refcount leak. That is the number
  graduation should weigh.

*Original phase text (SUPERSEDED by the scope decision at the top of CG8 --
retained only to show what changed):*

> Only after CG5--CG7 are solid and measured: consider a default `GC_AUTO`
> behind a longer bake. This is where the `cycle-gc` experiment graduates or is
> shelved per its `expires_at`.

The "consider a default `GC_AUTO`" clause is **withdrawn**, per (2) above --
the default is never coming, so there is nothing to consider behind any length
of bake. What survives from the original text, and is the reason (2) was
rejected rather than merely postponed:

Keep the zero-overhead pure-RC path available (`--disable` / `(gc-disable!)`)
regardless. "Reach past Rust" must never mean "lose Rust's predictable no-GC
baseline."

---

## Risks / open questions

- ~~**Pause time** (CG5). Synchronous collection over a large candidate set
  stalls; the heuristic must cap candidate-set size per cycle.~~
  **RESOLVED 2026-07-26 -- and the diagnosis was wrong.** The candidate set was
  never the term that mattered (128 -> 16,384 candidates moved one collection
  0.78 ms -> 2.16 ms). Cost was quadratic in the *live heap*: 2977 ms at 128k
  blocks, from a redundant linear dedup scan in `gc_enqueue_grey`. See PT1/PT2
  -- now 2.09 ms on the same shape, with no cap on candidates anywhere. One
  linear whole-registry pass remains, documented there.
- **`RCK_OPAQUE` blind spot is permanent** without runtime type reflection --
  and that is fine (documented, matches Rust). The CG3 lint keeps it visible.
- **Interaction with the substructural path** (`^linear` / `^unique`):
  **VERIFIED 2026-07-26 -- and it was not safe.** Those values are single-owner
  and cannot form rc cycles, so they are orthogonal to *cycle collection* as
  expected. But the double-drop this asked about is real, and broader than the
  `rc` framing: a closure that consumes a captured linear value escapes the
  linearity checker entirely, and calling it twice double-frees (confirmed under
  ASan). `rc` is a vector -- `(rc/of closure)` is accepted where
  `(rc/of linear-value)` is rejected -- but the minimal repro needs no `rc`.
  **FIXED 2026-07-26** -- a closure that CONSUMES a captured linear/unique value
  now inherits its `copy_kind`, so the double call is TUR-E0101, `(rc/of f)` is
  TUR-E0103, and dropping it is TUR-E0100. Archived at
  [docs/archive/history/closure-capture-escapes-linearity.md](../../archive/history/closure-capture-escapes-linearity.md).
  The fix was in the substructural checker, not the collector, so it never gated
  CG8.
- **Two collectors in one process.** **VERIFIED 2026-07-26 -- sound, but for a
  different reason than this assumed.** Measured: the DEDUP-5 visibility
  hardening holds (0 exported `gc_*`/`rc_*` dynamic symbols), the registries are
  genuinely separate, a foreign block reads correctly, and releasing one does not
  corrupt the host registry. The safety does **not** come from "values do not
  cross the boundary" -- that premise is unenforced, and a `.so` can export a
  `defn` returning `rc<T>` today. It comes from `gc_unregister_block` validating
  `gc_all_blocks[idx] != cb` before mutating its array. One hazard found by
  inspection on the path *before* that guard (`gc_remove_suspect` clearing
  `gc_buffered` on a foreign block) has been hardened in both collector copies.
  Cross-boundary *cycles* remain uncollectable by either collector -- a leak, same
  class as the `Vec`/HAMT blind spot. Written up for users in
  [docs/guides/gc-guide.md](../../guides/gc-guide.md) ("Two collectors in one
  process") and archived at
  [docs/archive/history/two-collectors-dlopen-boundary.md](../../archive/history/two-collectors-dlopen-boundary.md).

## Filed on the way (not collector bugs)

Three defects surfaced while measuring the above. None is in the collector; all
three were hit *because* the measurements drove the rc path harder than the
fixtures do.

- [set-bang-does-not-release-old-rc-value.md](../../archive/history/set-bang-does-not-release-old-rc-value.md)
  -- was **high**, now **FIXED** (archived). `set!` on an `^mut` binding holding
  `rc<T>` never released the overwritten value. Acyclic, so no collector could
  reclaim it -- it was the whole of the residue in CG8's real-workload run
  above. Investigating it turned up two further defects on the same seam (a
  missing `rc_strong_increment` on rc field-read values, and a read-after-release
  ordering bug in the field write), so the fix is an ownership normalization
  rather than a decrement. Pinned by `tests/fixtures/set-bang-releases-old-rc`
  and `tests/set-bang-rc-release-check.sh`.

- [rc-free-queue-drain-is-quadratic.md](../../archive/history/rc-free-queue-drain-is-quadratic.md)
  -- was **medium**, now **FIXED** (archived). `rc_free_queue_drain` memmoved
  the whole queue per pop, in both copies; it dominated the payload-zeroing
  measurement so completely that the memset was invisible until the queue was
  taken out of the loop. Filed as a performance cliff, but the quadratic turned
  out to be **masking a correctness bug**: drop glue calls the drain from inside
  the drain, so a deep cascade recursed one stack frame per link and died with
  an ASan stack-overflow at 200k depth -- in exactly the case the queue exists
  to prevent. A reentrancy guard closes it. 378 ms -> 0.91 ms to free 65,000
  blocks; 2M alloc/drop pairs 11,563 ms -> 70 ms. Pinned by
  `tests/fixtures/rc-free-queue-deep-cascade`.

- [rc-scalar-default-glue-invalid-free.md](../../archive/history/rc-scalar-default-glue-invalid-free.md)
  -- was **medium**, now **FIXED** (archived). `rc_cb_alloc(size, <scalar>,
  NULL)` got default drop glue that `free()`d its own inline payload. Scalars
  now default to a no-op inline glue; the fix flushed out that "C-API-only" was
  wrong -- `EX_RC_OF` repointed `cb->value` by raw assignment and relied on the
  defaulted `free()`, and now repoints through `rc_set_value` (added to the
  emitted replica). Pinned by `test_scalar_default_glue_drop` in the runtime
  parity battery.

## Related plans

- [stdlib-weak-ref-audit-plan.md](../../archive/history/stdlib-weak-ref-audit-plan.md)
  -- COMPLETE (WR0--WR4 landed 2026-07-26) and archived; its cycle inventory
  seeded CG7's corpus.
- [turi-interp-incremental-reclamation-plan.md](../../archive/turi-interp-incremental-reclamation-plan.md)
  -- COMPLETE (2026-07-27) and archived: incremental elaboration default-on,
  scratch promotion in the REPL, and the TR3 eval-boundary collection sweep;
  only TR1 (carrier relocation) stays shelved as demand-driven.
- [docs/guides/gc-guide.md](../../guides/gc-guide.md) -- user-facing behaviour,
  including which copy of the collector each build path runs.
