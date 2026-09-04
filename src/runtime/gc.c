/* gc.c - Bacon-Rajan Cycle Collector for Turmeric (Phase 10)
 *
 * This implements the Bacon-Rajan algorithm for cycle collection on top
 * of the Phase 9 rc<T> reference counting machinery.
 *
 * Key insight: when strong count reaches 0, if weak count > 0, the object
 * enters a "suspect" state. It may be part of a cycle. The collector uses
 * trial deletion: scan from strong roots (strong_count > 0), mark reachable
 * objects. Suspects not reachable from strong roots are garbage.
 */

#include "gc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== Global state ========== */

/* Suspect roots buffer - global for v1 (per-thread in future).
 * CG0: grown on demand. The old fixed 4096-entry arrays silently dropped
 * everything past the cap, which made the collector blind to real garbage in
 * any program with more than 4096 live rc blocks. */
#define GC_SUSPECT_INITIAL_CAPACITY 256
RcControlBlock **gc_suspect_roots = NULL;
uint32_t gc_suspect_count = 0;
uint32_t gc_suspect_capacity = 0;

/* Work queues for the collector */
#define GC_WORK_QUEUE_INITIAL_CAPACITY 256
RcControlBlock **gc_grey_queue = NULL;
uint32_t gc_grey_count = 0;
uint32_t gc_grey_capacity = 0;

/* CG0: grow a RcControlBlock* vector to hold at least `needed` entries.
 * Returns false only on allocation failure, in which case the caller keeps the
 * existing (smaller) buffer -- degrading to the old drop behavior rather than
 * crashing, but only under genuine OOM. */
static bool gc_vec_reserve(RcControlBlock ***vec, uint32_t *cap, uint32_t needed) {
    if (*cap >= needed) return true;
    uint32_t ncap = *cap ? *cap : 256u;
    while (ncap < needed) {
        if (ncap > (0xFFFFFFFFu / 2u)) { ncap = needed; break; }
        ncap *= 2u;
    }
    RcControlBlock **grown =
        (RcControlBlock **)realloc(*vec, (size_t)ncap * sizeof(RcControlBlock *));
    if (!grown) return false;
    *vec = grown;
    *cap = ncap;
    return true;
}

/* GC state */
GcMode gc_mode = GC_DISABLED;
bool gc_enabled = false;

/* Statistics */
uint64_t gc_collections = 0;
uint64_t gc_objects_freed = 0;
/* CG6: high-water mark of the candidate-root buffer.  The counter that answers
 * "is the collector keeping up?" -- gc_suspect_count is instantaneous and is
 * near zero right after any collection, so sampling it tells you almost
 * nothing.  Updated in gc_add_suspect, the one place the buffer grows. */
uint64_t gc_candidate_high_water = 0;
/* CG6: TUR_GC_TRACE.  -1 = not yet read from the environment. */
static int gc_trace_enabled = -1;

static bool gc_trace_on(void) {
    if (gc_trace_enabled < 0) {
        const char *v = getenv("TUR_GC_TRACE");
        gc_trace_enabled = (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
    }
    return gc_trace_enabled == 1;
}

/* ========== Initialization ========== */

void gc_init(void) {
    gc_vec_reserve(&gc_suspect_roots, &gc_suspect_capacity,
                   GC_SUSPECT_INITIAL_CAPACITY);
    gc_vec_reserve(&gc_grey_queue, &gc_grey_capacity,
                   GC_WORK_QUEUE_INITIAL_CAPACITY);
    gc_suspect_count = 0;
    gc_grey_count = 0;
    gc_collections = 0;
    gc_objects_freed = 0;
    gc_candidate_high_water = 0;
    gc_enabled = true;
}

void gc_shutdown(void) {
    /* CG0: release the grown vectors as well as resetting the counters. The
     * registry itself is intentionally NOT freed here: live control blocks may
     * still reference their slots via cb->gc_index. */
    free(gc_suspect_roots);
    gc_suspect_roots   = NULL;
    gc_suspect_capacity = 0;
    gc_suspect_count   = 0;
    free(gc_grey_queue);
    gc_grey_queue    = NULL;
    gc_grey_capacity = 0;
    gc_grey_count    = 0;
    gc_enabled = false;
}

/* ========== Color accessors ========== */

void gc_set_color(RcControlBlock *cb, GcColor color) {
    if (cb) {
        cb->color = color;
    }
}

GcColor gc_get_color(RcControlBlock *cb) {
    if (cb) {
        return cb->color;
    }
    return GC_WHITE;
}

/* ========== Suspect buffer management ========== */

static bool gc_add_suspect(RcControlBlock *cb) {
    if (!cb || !gc_enabled || gc_mode == GC_DISABLED) {
        return false;
    }

    /* CG1: O(1) dedup via the classic `buffered` flag. This used to be a linear
     * scan of the whole buffer, which was tolerable when only zombies were
     * buffered but would be quadratic now that every non-zero strong decrement
     * offers a candidate root. */
    if (cb->gc_buffered) return true;

    /* CG6: a block that cannot hold rc children cannot be a cycle root, so
     * buffering it as a candidate is pure cost -- a slot, plus a walk on every
     * collection until it is freed.
     *
     * This is what finally makes may_contain_cycles mean something: it was
     * written by both copies and read by neither.  Measured on 5000 scalar
     * rc<int> clone/drop pairs, the candidate high-water went 5000 -> 0.
     *
     * (A first measurement through Turmeric source showed 0 either way and
     * looked like "scalars never get buffered".  That was last-use elision
     * removing the clone/drop pair, not the collector's behaviour -- the C-level
     * probe is what showed the real number.) */
    if (!cb->may_contain_cycles) return false;

    /* DEDUP-4a: no force-collect at a watermark here.
     *
     * This used to call gc_collect() once gc_suspect_count reached
     * GC_MAX_SUSPECTS, and recurse.  But gc_add_suspect runs on the
     * rc_strong_decrement path, so that reentered the whole collector from
     * inside a refcount drop -- re-marking, re-sweeping and potentially
     * freeing blocks while a caller was mid-decrement.  The emitted copy in
     * emit_module.c has never done this; it simply grows the buffer, which is
     * the contract we are standardising on (and the one that stays sane once
     * arc<T> puts more than one thread on these buffers -- their
     * thread-safety is separately out of scope, see the plan).
     *
     * The bound this gave up is peak suspect-buffer size under GC_MANUAL.
     * That belongs to CG5's automatic trigger -- "collect when there is
     * memory pressure" is a policy decision, not something a refcount
     * decrement should make on its own. */
    if (gc_suspect_count >= gc_suspect_capacity) {
        if (!gc_vec_reserve(&gc_suspect_roots, &gc_suspect_capacity,
                            gc_suspect_count + 1u))
            return false;   /* OOM: cannot track this suspect */
    }

    /* Add to suspect buffer */
    cb->gc_buffered = true;
    gc_suspect_roots[gc_suspect_count++] = cb;
    if (gc_suspect_count > gc_candidate_high_water)
        gc_candidate_high_water = gc_suspect_count;   /* CG6 */
    gc_set_color(cb, GC_PURPLE);

    /* Trigger collection if in threshold mode and threshold exceeded */
    if (gc_mode == GC_THRESHOLD && gc_suspect_count >= GC_SUSPECT_THRESHOLD) {
        gc_collect();
    }

    return true;
}

static void gc_remove_suspect(RcControlBlock *cb) {
    if (!cb) return;
    if (!cb->gc_buffered) return;   /* CG1: not in the buffer, nothing to scan */

    for (uint32_t i = 0; i < gc_suspect_count; i++) {
        if (gc_suspect_roots[i] == cb) {
            /* Swap with last element and shrink */
            gc_suspect_roots[i] = gc_suspect_roots[gc_suspect_count - 1];
            gc_suspect_count--;
            /* Clear the flag only when THIS collector actually held it.  A
             * process with two collectors (a Turmeric host that dlopens a
             * Turmeric .so) can call this on a block the OTHER collector
             * buffered: the scan above finds nothing, and clearing the flag
             * anyway would desynchronize the owner -- its gc_suspect_roots
             * would keep a pointer to a block about to be freed, while its own
             * gc_remove_suspect early-returns on the cleared flag and never
             * removes it.  Its next collection would then read freed memory,
             * which is the very failure the gc_unregister_block call site was
             * added to prevent.  Leaving a stale `true` on an owned-but-absent
             * block is harmless by comparison: it only costs a later rescan
             * that finds nothing. */
            cb->gc_buffered = false;
            break;
        }
    }
}

/* ========== Work queue management ========== */

static void gc_enqueue_grey(RcControlBlock *cb) {
    if (!cb) return;

    /* PT1: NO linear "already queued?" scan here -- the COLOR is the dedup.
     *
     * Every call site colors the block GC_BLACK before enqueuing and refuses to
     * enqueue a block that is already GC_BLACK (gc_mark_phase's strong-root
     * sweep visits each registry slot exactly once; gc_mark_struct_child and
     * the RCEXP_RC walker both guard on `color != GC_BLACK`).  A duplicate can
     * therefore never be offered, so the scan never found one -- it was pure
     * overhead, and quadratic overhead at that: gc_mark_phase enqueues EVERY
     * strong-rooted block, so an O(queue) scan per enqueue made a collection
     * O(live_blocks^2).
     *
     * Measured, 512 candidates against a growing live heap (one collection):
     *
     *      live blocks |  with scan  |  without
     *      ------------+-------------+----------
     *           8,512  |     14.2 ms |   0.17 ms
     *          32,512  |    177.7 ms |   1.14 ms
     *         128,512  |   2977.5 ms |  11.11 ms
     *
     * Three seconds of stall on a heap that is not especially large, on a
     * collection that freed 512 blocks.  This was the pause-time risk the plan
     * carried as "cap the candidate set" -- the candidate set was never the
     * term that mattered (128 -> 16384 candidates moved the same collection
     * only 0.78 ms -> 2.16 ms).
     *
     * DEDUP note: the emitted replica in emit_module.c has NEVER had this scan.
     * This brings the runtime copy in line with it rather than the reverse. */

    /* CG0: grow instead of silently skipping -- a dropped grey entry means the
     * mark phase misses a subgraph and the collector frees something live. */
    if (gc_grey_count >= gc_grey_capacity) {
        if (!gc_vec_reserve(&gc_grey_queue, &gc_grey_capacity, gc_grey_count + 1u))
            return;   /* OOM only */
    }

    gc_grey_queue[gc_grey_count++] = cb;
    /* DEDUP-4b: the caller owns the color; enqueuing must not change it.
     *
     * This used to set GC_GREY, which every one of the three call sites
     * immediately contradicted -- each sets GC_BLACK and then enqueues, so the
     * enqueue silently DOWNGRADED a just-marked-reachable block. The runtime
     * stayed self-consistent (gc_trial_deletion_phase and gc_is_alive both
     * accept BLACK or GREY), but it left the mark phase with a different
     * observable end state than the emitted collector, which leaves reachable
     * blocks BLACK.
     *
     * tests/fixtures/exg5-walker-rc-payload reads the inner block's color
     * through inline C after `(gc!)` and expects BLACK; under the archive link
     * it read GREY.  "Everything reachable is BLACK after the mark phase" is
     * both the cleaner invariant and the one already asserted, so the runtime
     * converges on it.  GREY remains what it should be: the trial-deletion
     * traversal's own color, set by gc_mark_gray. */
}

static RcControlBlock *gc_dequeue_grey(void) {
    if (gc_grey_count == 0) {
        return NULL;
    }
    RcControlBlock *cb = gc_grey_queue[0];
    /* Swap with last and shrink */
    gc_grey_queue[0] = gc_grey_queue[gc_grey_count - 1];
    gc_grey_count--;
    return cb;
}

/* ========== Scanner - scans fields of a value for RC pointers ========== */

/* For Phase 10 v1, we can only scan structs that we know about.
 * For primitive types (int, bool, etc.), there are no RC pointers to scan.
 * For pointer types, we need type information to know what they point to.
 * 
 * This is a simplified version that only handles known types.
 * A proper implementation would need reflection or type metadata.
 */

/* For v1, we use a conservative approach: we don't scan the value
 * because we don't have type information at runtime. Instead, we rely
 * on the fact that RC pointers are stored as RcControlBlock* in the
 * generated C code. We can scan the control block's value field if it's
 * itself an RC type.
 * 
 * However, without type metadata, we can't generally scan arbitrary values.
 * For Phase 10 v1, we'll implement a simplified version that:
 * 1. Marks all strong-count>0 objects as black (strong roots)
 * 2. Marks all weak-count>0 objects with strong-count==0 as purple (suspects)
 * 3. Suspects that remain purple (not reachable from strong roots) are garbage
 * 
 * The key insight: we don't need to scan the value contents if we track
 * all RC allocations globally. But that requires registration.
 * 
 * Alternative approach for v1: use a global set of all RC allocations.
 * When an RC is created, register it. When collected, unregister it.
 * Then the collector can scan all registered RCs.
 */

/* For v1, we use a simpler approach: maintain a global list of all
 * RC control blocks. When collecting, we:
 * 1. Reset all colors to WHITE
 * 2. Mark all strong_count>0 blocks as BLACK (strong roots)
 * 3. For each strong root, scan its value (if it contains RC pointers)
 *    - For v1, we can't scan arbitrary values, so we skip this
 * 4. All PURPLE (suspect) blocks that are still WHITE are garbage
 */

/* Global registry of all RC control blocks.
 * CG0: heap-allocated and grown on demand. The old fixed 4096-entry array made
 * gc_register_block a silent no-op past the cap, so every block beyond 4096 was
 * invisible to the collector -- a correctness cliff, not just a capacity limit
 * (docs/archive/history/gc-strong-cycles-not-collected.md). */
#define GC_GLOBAL_REGISTRY_INITIAL_CAPACITY 4096
RcControlBlock **gc_all_blocks = NULL;
uint32_t gc_all_blocks_count = 0;
static uint32_t gc_all_blocks_capacity = 0;

/* Register a newly allocated control block */
void gc_register_block(RcControlBlock *cb) {
    if (!cb) return;

    /* CG5: the allocation checkpoint, and it must run BEFORE `cb` joins the
     * registry.
     *
     * A collection walks every registered block, and a block is registered by
     * rc_cb_alloc_kinded *before its caller has written the payload*.  Running
     * the checkpoint after the insert therefore hands the walker a block whose
     * RCK_EXISTENTIAL payload is uninitialised heap: the first version of this
     * did exactly that and segfaulted in gc_get_color on a garbage child
     * pointer.  Checking in first leaves the half-built block invisible.
     *
     * That alone is not sufficient -- see the AUTO-mode payload zeroing in
     * rc_cb_alloc_kinded, which covers blocks already registered whose callers
     * have not finished filling them either. */
    gc_on_alloc_checkpoint();

    cb->gc_buffered = false;   /* CG1: fresh block is not in the candidate buffer */
    if (gc_all_blocks_count >= gc_all_blocks_capacity) {
        uint32_t want = gc_all_blocks_capacity ? gc_all_blocks_count + 1u
                                               : GC_GLOBAL_REGISTRY_INITIAL_CAPACITY;
        if (!gc_vec_reserve(&gc_all_blocks, &gc_all_blocks_capacity, want)) {
            cb->gc_index = RC_GC_INDEX_NONE;   /* OOM: untracked */
            gc_set_color(cb, GC_WHITE);
            cb->may_contain_cycles = true;
            return;
        }
    }
    cb->gc_index = gc_all_blocks_count;
    gc_all_blocks[gc_all_blocks_count++] = cb;
    gc_set_color(cb, GC_WHITE);
    cb->may_contain_cycles = true;  /* Default: assume it might contain cycles */
}

/* Unregister a control block (when freed).
 * CG0: O(1) swap-remove via the block's stored index. This runs on EVERY rc
 * free -- including with the collector disabled -- so the old linear scan over
 * all live blocks was quadratic in a long-running program. */
void gc_unregister_block(RcControlBlock *cb) {
    if (!cb) return;
    /* CG1 (safety): drop it from the candidate-root buffer too. Before CG1 only
     * zombies were buffered and they were freed via a path that had already
     * removed them; now ordinary blocks are buffered and are freed the moment
     * their strong count hits 0, so leaving a stale pointer here would make the
     * next collection read freed memory. */
    gc_remove_suspect(cb);
    uint32_t idx = cb->gc_index;
    if (idx == RC_GC_INDEX_NONE || idx >= gc_all_blocks_count ||
        gc_all_blocks[idx] != cb) {
        cb->gc_index = RC_GC_INDEX_NONE;
        return;   /* not registered (or already removed) */
    }
    RcControlBlock *last = gc_all_blocks[gc_all_blocks_count - 1u];
    gc_all_blocks[idx] = last;
    if (last) last->gc_index = idx;
    gc_all_blocks_count--;
    cb->gc_index = RC_GC_INDEX_NONE;
}

/* ========== The Bacon-Rajan Algorithm ========== */

/* DS3: callback passed to RCK_STRUCT walk_fns during gc_mark_phase.
 * Colors and enqueues each enumerated rc child. */
static void gc_mark_struct_child(RcControlBlock *child, void *ctx) {
    (void)ctx;
    if (child && gc_get_color(child) != GC_BLACK) {
        gc_set_color(child, GC_BLACK);
        gc_enqueue_grey(child);
    }
}

/* Mark phase: traverse from strong roots and mark reachable objects */
static void gc_mark_phase(void) {
    /* Reset all colors to WHITE first */
    for (uint32_t i = 0; i < gc_all_blocks_count; i++) {
        gc_set_color(gc_all_blocks[i], GC_WHITE);
    }

    /* Step 1: Mark all strong roots (strong_count > 0) as BLACK */
    for (uint32_t i = 0; i < gc_all_blocks_count; i++) {
        RcControlBlock *cb = gc_all_blocks[i];
        if (cb->strong_count > 0) {
            gc_set_color(cb, GC_BLACK);
            gc_enqueue_grey(cb);
        }
    }

    /* Step 2: Propagate from strong roots.
     *
     * For most blocks we still cannot scan the value contents (no type
     * metadata at runtime), so they are processed as-is.  EXG5 widens
     * the walker for blocks tagged RCK_EXISTENTIAL: their value field
     * is a known layout (`tur_existential_t`) whose `value` slot, when
     * tagged RCEXP_RC, is an RcControlBlock pointer that the walker
     * follows just like any other strong field.  This makes the inner
     * rc reachable through the existential, which is what fixes the
     * cycle-detection blind spot called out in the EXG5 plan. */
    while (gc_grey_count > 0) {
        RcControlBlock *cb = gc_dequeue_grey();
        if (!cb) break;

        if (cb->value && cb->reserved[0] == RCK_EXISTENTIAL &&
                cb->reserved[1] == RCEXP_RC) {
            /* `value` points at a tur_existential_t whose first field
             * is an int64_t holding the inner RcControlBlock pointer
             * (cast through intptr_t).  Mirror that layout here so we
             * do not need to depend on the codegen-generated struct. */
            int64_t raw = *(const int64_t *)cb->value;
            RcControlBlock *inner = (RcControlBlock *)(intptr_t)raw;
            if (inner && gc_get_color(inner) != GC_BLACK) {
                gc_set_color(inner, GC_BLACK);
                gc_enqueue_grey(inner);
            }
        } else if (cb->value && cb->reserved[0] == RCK_STRUCT && cb->walk_fn) {
            /* DS3: walk_fn enumerates each rc-typed child of the struct
             * payload via gc_mark_struct_child below, which colors and
             * enqueues children just like the existential path. */
            cb->walk_fn(cb->value, gc_mark_struct_child, NULL);
        }
    }

    /* Step 3: All BLACK objects are reachable from strong roots */
    /* All WHITE objects with strong_count == 0 are not reachable */
}

/* Trial deletion: identify and collect garbage cycles */
static void gc_trial_deletion_phase(void) {
    uint32_t i = 0;
    while (i < gc_suspect_count) {
        RcControlBlock *cb = gc_suspect_roots[i];

        /* Check if this suspect is reachable from strong roots */
        if (gc_get_color(cb) == GC_BLACK || gc_get_color(cb) == GC_GREY) {
            /* Still reachable - not garbage, keep as suspect */
            i++;
            continue;
        }

        /* Check if strong count is now > 0 (was revived) */
        if (cb->strong_count > 0) {
            /* Was revived - remove from suspect list */
            gc_remove_suspect(cb);
            gc_set_color(cb, GC_BLACK);
            continue;
        }

        /* WHITE and strong_count == 0: this is garbage */
        /* Remove from suspect list and free */
        gc_remove_suspect(cb);
        gc_unregister_block(cb);

        /* CG4: honour the zombie contract the rest of RC upholds.
         *
         * This used to force the block away even with live weak<T> observers:
         * it zeroed weak_count and freed the control block, so the next
         * upgrade() (or the last rc_weak_decrement) touched freed memory. The
         * emitted preamble in emit_module.c already did the right thing here --
         * this copy was the one that diverged.
         *
         * Correct behavior: drop the VALUE (it is genuinely dead) but keep the
         * control block alive while any weak reference can still observe it, so
         * upgrade() reports "gone" instead of dangling. rc_weak_decrement frees
         * the block once the last weak reference goes away. */
        if (cb->value) {
            cb->drop_fn(cb->value);
            cb->value = NULL;
        }
        if (cb->weak_count == 0) {
            free(cb);
            gc_objects_freed++;
        }
        /* weak_count > 0: leave the zombie block for rc_weak_decrement. */
    }
}

/* ========== CG2: Bacon-Rajan trial deletion ==========
 *
 * The pre-CG2 "trial deletion phase" was really a mark-sweep from strong roots:
 * every block with strong_count > 0 was treated as a root, so a self-sustaining
 * cycle -- whose members all have strong_count > 0 via their own back-edges --
 * was unconditionally marked live and could never be collected. Real trial
 * deletion subtracts the internal edges first, which is what distinguishes an
 * in-cycle reference from an external one.
 *
 * Three deliberate departures from the textbook, all safety-motivated:
 *   1. Trial decrements land on a SCRATCH counter (gc_trial), not the real
 *      strong_count. A bad walker can then only cause a missed cycle, never a
 *      corrupted live count.
 *   2. The white set is collected into a pending list and freed only after the
 *      whole traversal. Freeing during traversal would let a later sibling read
 *      a freed block's color.
 *   3. Members are flagged gc_collecting before their drop_fns run, so the
 *      child decrements inside drop glue cannot double-free.
 */

static RcControlBlock **gc_pending_free = NULL;
static uint32_t gc_pending_count = 0;
static uint32_t gc_pending_capacity = 0;
/* Candidate roots drained from the suspect buffer this collection. */
static RcControlBlock **gc_cand_roots = NULL;
static uint32_t gc_cand_count = 0;
static uint32_t gc_cand_capacity = 0;

/* Enumerate the rc children of a block. Only layouts we can describe are
 * walkable: RCK_STRUCT via its registered walk_fn, and an RCK_EXISTENTIAL whose
 * payload is itself an rc. RCK_OPAQUE blocks have no enumerable children -- a
 * cycle routed through one is invisible to the collector, which is the
 * documented blind spot (and exactly Rust's situation with raw pointers). */
static void gc_each_child(RcControlBlock *cb, RcWalkChildFn fn, void *ctx) {
    if (!cb || !cb->value) return;
    if (cb->reserved[0] == RCK_EXISTENTIAL && cb->reserved[1] == RCEXP_RC) {
        int64_t raw = *(const int64_t *)cb->value;
        RcControlBlock *inner = (RcControlBlock *)(intptr_t)raw;
        if (inner) fn(inner, ctx);
    } else if (cb->reserved[0] == RCK_STRUCT && cb->walk_fn) {
        cb->walk_fn(cb->value, fn, ctx);
    }
}

static void gc_mark_gray(RcControlBlock *s);
static void gc_scan(RcControlBlock *s);
static void gc_scan_black(RcControlBlock *s);
static void gc_collect_white(RcControlBlock *s);

/* MarkGray: subtract one trial reference for each internal edge. */
static void gc_mark_gray_child(RcControlBlock *t, void *ctx) {
    (void)ctx;
    if (!t) return;
    if (t->gc_trial > 0) t->gc_trial--;
    gc_mark_gray(t);
}
static void gc_mark_gray(RcControlBlock *s) {
    if (!s || s->color == GC_GREY) return;
    s->color = GC_GREY;
    gc_each_child(s, gc_mark_gray_child, NULL);
}

/* ScanBlack: this subgraph is externally reachable after all -- restore the
 * trial counts we subtracted and re-blacken. */
static void gc_scan_black_child(RcControlBlock *t, void *ctx) {
    (void)ctx;
    if (!t) return;
    t->gc_trial++;
    if (t->color != GC_BLACK) gc_scan_black(t);
}
static void gc_scan_black(RcControlBlock *s) {
    if (!s) return;
    s->color = GC_BLACK;
    gc_each_child(s, gc_scan_black_child, NULL);
}

/* Scan: a grey block with trial refs left is reachable from outside the
 * candidate subgraph; one with none is provisionally garbage (white). */
static void gc_scan_child(RcControlBlock *t, void *ctx) { (void)ctx; gc_scan(t); }
static void gc_scan(RcControlBlock *s) {
    if (!s || s->color != GC_GREY) return;
    if (s->gc_trial > 0) {
        gc_scan_black(s);
    } else {
        s->color = GC_WHITE;
        gc_each_child(s, gc_scan_child, NULL);
    }
}

/* CollectWhite: gather the garbage cycle. Nothing is freed here (see note 2). */
static void gc_collect_white_child(RcControlBlock *t, void *ctx) {
    (void)ctx;
    gc_collect_white(t);
}
static void gc_collect_white(RcControlBlock *s) {
    if (!s || s->color != GC_WHITE || s->gc_buffered) return;
    s->color = GC_BLACK;   /* visited marker; prevents re-entry */
    if (gc_pending_count >= gc_pending_capacity) {
        if (!gc_vec_reserve(&gc_pending_free, &gc_pending_capacity,
                            gc_pending_count + 1u))
            return;   /* OOM: leave it uncollected (a leak, never unsafe) */
    }
    gc_pending_free[gc_pending_count++] = s;
    gc_each_child(s, gc_collect_white_child, NULL);
}

/* Run one trial-deletion collection over the buffered candidate roots.
 * Only roots with strong_count > 0 are candidates; strong==0 roots are zombies
 * and are left for the pre-existing zombie sweep. */
static void gc_cycle_collect_phase(void) {
    if (gc_suspect_count == 0) return;

    /* Reset scratch state over every registered block. Same order of work as
     * the existing color reset, so no new asymptotic cost. */
    for (uint32_t i = 0; i < gc_all_blocks_count; i++) {
        RcControlBlock *cb = gc_all_blocks[i];
        if (!cb) continue;
        cb->gc_trial = cb->strong_count;
    }

    /* MarkRoots */
    for (uint32_t i = 0; i < gc_suspect_count; i++) {
        RcControlBlock *s = gc_suspect_roots[i];
        if (s && s->color == GC_PURPLE && s->strong_count > 0) gc_mark_gray(s);
    }
    /* ScanRoots */
    for (uint32_t i = 0; i < gc_suspect_count; i++) {
        RcControlBlock *s = gc_suspect_roots[i];
        if (s && s->strong_count > 0) gc_scan(s);
    }
    /* CollectRoots: drain the candidate roots (zombies stay), then gather. */
    /* CollectRoots: drain the candidate roots (zombies stay in the buffer for
     * the sweep below), then CollectWhite from EACH DRAINED CANDIDATE.
     *
     * It must be the candidates, not every registered block: blocks are born
     * WHITE at registration, so sweeping all-WHITE would collect live values
     * that this collection never even looked at. */
    gc_pending_count = 0;
    gc_cand_count = 0;
    {
        uint32_t w = 0;
        for (uint32_t i = 0; i < gc_suspect_count; i++) {
            RcControlBlock *s = gc_suspect_roots[i];
            if (s && s->strong_count > 0) {
                s->gc_buffered = false;      /* required before gc_collect_white */
                if (gc_cand_count < gc_cand_capacity ||
                    gc_vec_reserve(&gc_cand_roots, &gc_cand_capacity,
                                   gc_cand_count + 1u))
                    gc_cand_roots[gc_cand_count++] = s;
                continue;                    /* dropped from the buffer */
            }
            gc_suspect_roots[w++] = s;       /* keep zombies for the sweep below */
        }
        gc_suspect_count = w;
    }
    for (uint32_t i = 0; i < gc_cand_count; i++)
        gc_collect_white(gc_cand_roots[i]);
    gc_cand_count = 0;

    if (gc_pending_count == 0) return;

    /* Free the white set. Flag first so drop glue cannot double-free (note 3),
     * then run every drop_fn, then release the blocks. */
    for (uint32_t i = 0; i < gc_pending_count; i++)
        gc_pending_free[i]->gc_collecting = true;
    for (uint32_t i = 0; i < gc_pending_count; i++) {
        RcControlBlock *cb = gc_pending_free[i];
        if (cb->value && cb->drop_fn) { cb->drop_fn(cb->value); cb->value = NULL; }
    }
    for (uint32_t i = 0; i < gc_pending_count; i++) {
        RcControlBlock *cb = gc_pending_free[i];
        gc_unregister_block(cb);
        cb->strong_count = 0;
        if (cb->weak_count > 0) {
            /* CG4 discipline: a live weak<T> still observes this block, so keep
             * the control block as a zombie -- upgrade() must return none, not
             * dangle. It is freed by the last rc_weak_decrement. */
            cb->gc_collecting = false;
            continue;
        }
        free(cb);
        gc_objects_freed++;
    }
    gc_pending_count = 0;
}

/* Main collection function */
/* CG5 forward decls: the AUTO counters live below, next to gc_auto. */
static void gc_collection_window_reset(void);
static bool gc_collection_reentered(void);
static void gc_collection_enter(void);
static void gc_collection_leave(void);

void gc_collect(void) {
    if (!gc_enabled || gc_mode == GC_DISABLED) {
        return;
    }
    /* CG5: drop glue run during a collection may allocate, which re-enters the
     * allocation checkpoint.  Refuse to nest rather than sweep a registry that
     * is mid-collection. */
    if (gc_collection_reentered()) return;
    gc_collection_enter();
    gc_collection_window_reset();

    gc_collections++;

    /* CG6: TUR_GC_TRACE.  Sampled before the phases so the "in" figures
     * describe what this collection was handed. */
    uint32_t trace_candidates_in = gc_suspect_count;
    uint32_t trace_live_in       = gc_all_blocks_count;
    uint64_t trace_freed_before  = gc_objects_freed;

    /* CG2: real trial deletion over the buffered candidate roots -- this is the
     * phase that reclaims genuine strong cycles. */
    gc_cycle_collect_phase();

    /* Clear grey queue */
    gc_grey_count = 0;

    /* Pre-existing zombie sweep (strong==0 with live weak refs).
     *
     * PT2: skipped outright when the candidate buffer is empty.  This is what
     * takes the remaining O(live_blocks) term off the common path.
     *
     * gc_cycle_collect_phase has just DRAINED every candidate with
     * strong_count > 0, so whatever is left in gc_suspect_roots is exactly the
     * zombie set -- and gc_trial_deletion_phase iterates that buffer and
     * nothing else.  With the buffer empty it is a no-op by construction, which
     * leaves gc_mark_phase's whole-registry walk existing only to produce
     * colors for it to read.
     *
     * No RUNTIME consumer reads the colors this skips.  gc_is_alive is the only
     * reader of the color field outside the collector and it has no callers in
     * the tree; the liveness path that actually runs (rc_is_alive, rc_upgrade)
     * tests strong_count and never the color.  The next collection
     * re-establishes everything it needs regardless: gc_mark_phase resets every
     * color to WHITE before marking, gc_cycle_collect_phase reseeds gc_trial
     * from strong_count for every block, and a candidate is re-colored
     * GC_PURPLE by gc_add_suspect when it is (re-)buffered.
     *
     * The colors ARE observable to inline C, though, and one fixture reads them
     * as a white-box probe of the walker: tests/fixtures/exg5-walker-rc-payload
     * asserts that gc_mark_phase colors an existential's inner rc BLACK.  That
     * program buffers no candidates at all, so this skip left it reading WHITE.
     * The walker itself is unchanged -- what changed is whether a program with
     * nothing to sweep runs one -- so the fixture now leaves a real zombie
     * (strong_count 0, weak_count 1) in the buffer first, restoring the
     * precondition its assertion was always assuming.
     *
     * Measured, 256 garbage cycles against a 256k-block live heap: the
     * collection spent 34.0 ms of its 37.3 ms in gc_mark_phase and 0.000 ms in
     * gc_trial_deletion_phase.  Skipping the pair when there are no zombies
     * makes a collection cost scale with the garbage being collected rather
     * than with the size of the live heap it is sitting in. */
    if (gc_suspect_count > 0) {
        gc_mark_phase();
        gc_trial_deletion_phase();
    }

    if (gc_trace_on()) {
        /* One line per collection, to stderr so it never contaminates a
         * program's stdout (which the fixture harness diffs). */
        fprintf(stderr,
                "[gc] #%llu mode=%d candidates=%u freed=%llu live=%u->%u\n",
                (unsigned long long)gc_collections, (int)gc_mode,
                trace_candidates_in,
                (unsigned long long)(gc_objects_freed - trace_freed_before),
                trace_live_in, gc_all_blocks_count);
    }

    gc_collection_leave();
}

/* Called when strong count reaches 0 */
/* CG1: classic Bacon-Rajan PossibleRoot -- see gc.h for why this is the edge
 * that matters for strong cycles. */
void gc_possible_root(RcControlBlock *cb) {
    if (!cb || !gc_enabled || gc_mode == GC_DISABLED) return;
    gc_add_suspect(cb);   /* colors PURPLE and buffers, O(1)-deduped */
}

void gc_on_strong_decrement(RcControlBlock *cb) {
    if (!gc_enabled || gc_mode == GC_DISABLED || !cb) {
        return;
    }

    if (cb->weak_count > 0) {
        /* Enter suspect state */
        gc_add_suspect(cb);
        gc_set_color(cb, GC_PURPLE);
    }
}

/* Force collection */
void gc_force(void) {
    gc_collect();
}

/* Enable cycle collection */
void gc_enable(void) {
    gc_enabled = true;
    /* DEDUP-4: gc_collect() gates on BOTH gc_enabled and gc_mode, and gc_mode
     * starts at GC_DISABLED.  Setting only the flag left every collection a
     * silent no-op for the interpreter/libturi path -- `(gc-enable!)` lowers to
     * this function (src/turi/eval.c) and nothing else ever called
     * gc_set_mode, so `(gc!)` returned without collecting.  The emitted copy in
     * emit_module.c has always defaulted to GC_MANUAL here, which is why the
     * compiled path collected and the interpreted path did not. */
    if (gc_mode == GC_DISABLED) gc_mode = GC_MANUAL;
}

/* Disable cycle collection */
void gc_disable(void) {
    gc_enabled = false;
    gc_mode = GC_DISABLED;   /* DEDUP-4: match the emitted copy */
}

/* Set GC mode */
void gc_set_mode(GcMode mode) {
    gc_mode = mode;
}

/* ========== CG6: statistics accessors ========== */

uint64_t gc_stat_collections(void)          { return gc_collections; }
uint64_t gc_stat_objects_freed(void)        { return gc_objects_freed; }
uint64_t gc_stat_live_blocks(void)          { return (uint64_t)gc_all_blocks_count; }
uint64_t gc_stat_candidate_high_water(void) { return gc_candidate_high_water; }

/* ========== CG5: automatic collection (GC_AUTO) ========== */

/* Allocations since the last collection.  Reset by gc_on_alloc_checkpoint when
 * it fires and by gc_collect, so an explicit `(gc!)` also restarts the window
 * rather than leaving AUTO to fire again immediately after. */
static uint32_t gc_allocs_since_collect = 0;

/* Reentrancy guard.  A collection runs drop glue, and drop glue is arbitrary
 * user code that may allocate -- which lands right back in
 * gc_on_alloc_checkpoint.  Without this, freeing a large cycle could recurse
 * into a fresh collection over a half-swept registry. */
static bool gc_in_collection = false;

static void gc_collection_window_reset(void) { gc_allocs_since_collect = 0; }
static bool gc_collection_reentered(void)    { return gc_in_collection; }
static void gc_collection_enter(void)        { gc_in_collection = true; }
static void gc_collection_leave(void)        { gc_in_collection = false; }

void gc_auto(void) {
    gc_enabled = true;
    gc_mode = GC_AUTO;
    gc_allocs_since_collect = 0;
}

void gc_on_alloc_checkpoint(void) {
    if (gc_mode != GC_AUTO || !gc_enabled) return;
    if (gc_in_collection) return;

    gc_allocs_since_collect++;

    /* Two triggers, because either alone has a blind spot.  A candidate-count
     * trigger misses a program that allocates heavily but buffers few
     * candidates (nothing ever reaches the watermark); an allocation-count
     * trigger alone makes a cycle-churning program wait out the full interval
     * even while candidates pile up. */
    bool by_candidates = gc_suspect_count >= GC_SUSPECT_THRESHOLD;
    bool by_allocations = gc_allocs_since_collect >= GC_AUTO_ALLOC_INTERVAL;
    if (!by_candidates && !by_allocations) return;

    gc_collect();
}

/* Check if a weak pointer's target is still alive */
bool gc_is_alive(RcControlBlock *cb) {
    if (!cb) return false;
    /* Alive if strong count > 0 */
    if (cb->strong_count > 0) return true;
    /* Or if it's reachable from strong roots (was marked BLACK/GREY in last collection) */
    GcColor color = gc_get_color(cb);
    return (color == GC_BLACK || color == GC_GREY);
}
