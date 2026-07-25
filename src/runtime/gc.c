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

    /* Check if already in suspect buffer */
    for (uint32_t i = 0; i < gc_suspect_count; i++) {
        if (gc_suspect_roots[i] == cb) {
            return true;  /* Already tracked */
        }
    }

    /* CG0: at the force-collection watermark, try to drain the buffer first;
     * if collection cannot shrink it, GROW rather than drop the suspect (the
     * old code silently discarded it, losing real garbage). */
    if (gc_suspect_count >= GC_MAX_SUSPECTS) {
        uint32_t before = gc_suspect_count;
        gc_collect();
        if (gc_suspect_count < before) return gc_add_suspect(cb);
    }
    if (gc_suspect_count >= gc_suspect_capacity) {
        if (!gc_vec_reserve(&gc_suspect_roots, &gc_suspect_capacity,
                            gc_suspect_count + 1u))
            return false;   /* OOM: cannot track this suspect */
    }

    /* Add to suspect buffer */
    gc_suspect_roots[gc_suspect_count++] = cb;
    gc_set_color(cb, GC_PURPLE);

    /* Trigger collection if in threshold mode and threshold exceeded */
    if (gc_mode == GC_THRESHOLD && gc_suspect_count >= GC_SUSPECT_THRESHOLD) {
        gc_collect();
    }

    return true;
}

static void gc_remove_suspect(RcControlBlock *cb) {
    if (!cb) return;

    for (uint32_t i = 0; i < gc_suspect_count; i++) {
        if (gc_suspect_roots[i] == cb) {
            /* Swap with last element and shrink */
            gc_suspect_roots[i] = gc_suspect_roots[gc_suspect_count - 1];
            gc_suspect_count--;
            break;
        }
    }
}

/* ========== Work queue management ========== */

static void gc_enqueue_grey(RcControlBlock *cb) {
    if (!cb) return;

    /* Check if already in queue */
    for (uint32_t i = 0; i < gc_grey_count; i++) {
        if (gc_grey_queue[i] == cb) {
            return;
        }
    }

    /* CG0: grow instead of silently skipping -- a dropped grey entry means the
     * mark phase misses a subgraph and the collector frees something live. */
    if (gc_grey_count >= gc_grey_capacity) {
        if (!gc_vec_reserve(&gc_grey_queue, &gc_grey_capacity, gc_grey_count + 1u))
            return;   /* OOM only */
    }

    gc_grey_queue[gc_grey_count++] = cb;
    gc_set_color(cb, GC_GREY);
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
 * (docs/reported/gc-strong-cycles-not-collected.md). */
#define GC_GLOBAL_REGISTRY_INITIAL_CAPACITY 4096
RcControlBlock **gc_all_blocks = NULL;
uint32_t gc_all_blocks_count = 0;
static uint32_t gc_all_blocks_capacity = 0;

/* Register a newly allocated control block */
void gc_register_block(RcControlBlock *cb) {
    if (!cb) return;
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

        /* Decrement weak count and free if both counts are 0 */
        /* Note: we need to be careful here - weak pointers might still exist */
        /* For v1, we just free immediately since we're removing from suspect */
        if (cb->weak_count == 0) {
            /* No weak references - free immediately */
            if (cb->value) {
                cb->drop_fn(cb->value);
            }
            free(cb);
            gc_objects_freed++;
        } else {
            /* Has weak references - decrement weak count for each
             * For v1, we don't track individual weak pointers,
             * so we just set weak_count to 0 to force free */
            cb->weak_count = 0;
            if (cb->value) {
                cb->drop_fn(cb->value);
            }
            free(cb);
            gc_objects_freed++;
        }
    }
}

/* Main collection function */
void gc_collect(void) {
    if (!gc_enabled || gc_mode == GC_DISABLED) {
        return;
    }

    gc_collections++;

    /* Clear grey queue */
    gc_grey_count = 0;

    /* Mark phase */
    gc_mark_phase();

    /* Trial deletion phase */
    gc_trial_deletion_phase();
}

/* Called when strong count reaches 0 */
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
}

/* Disable cycle collection */
void gc_disable(void) {
    gc_enabled = false;
}

/* Set GC mode */
void gc_set_mode(GcMode mode) {
    gc_mode = mode;
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
