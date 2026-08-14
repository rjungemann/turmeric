/* gc.h - Bacon-Rajan Cycle Collector for Turmeric (Phase 10)
 *
 * This implements a cycle collector layered on top of the Phase 9 rc<T>
 * reference counting machinery. The Bacon-Rajan algorithm uses trial
 * deletion to collect cycles that would otherwise leak with pure RC.
 *
 * See effects-plan.md §5.1.2 for design details.
 */

#ifndef TUR_GC_H
#define TUR_GC_H

#include <stdint.h>
#include <stdbool.h>

#include "rc.h"

/* GcColor is defined in rc.h to avoid circular dependency */

/* GC mode for the collector */
typedef enum {
    GC_DISABLED,   /* No cycle collection (default for v1) */
    GC_MANUAL,     /* Collection only when explicitly triggered */
    GC_THRESHOLD,  /* Collection when suspect buffer exceeds threshold */
    /* CG5: collection driven automatically from ALLOCATION checkpoints -- see
     * gc_on_alloc_checkpoint.  Appended last so the existing ordinals, which
     * the emitted copy of this enum also encodes, are unchanged. */
    GC_AUTO
} GcMode;

/* Configuration constants */
#define GC_SUSPECT_THRESHOLD 128  /* Trigger collection when this many suspects */
/* CG0 made this a "force a collection at this size" trigger rather than a hard
 * cap.  DEDUP-4a then removed the force-collect entirely -- it reentered the
 * collector from inside a refcount decrement (see gc_add_suspect) -- so this is
 * now UNUSED by the collector itself.  Kept because the emitted copy in
 * emit_module.c still defines the same constant, and the two must not drift
 * while both exist; a pressure-driven trigger belongs to CG5. */
#define GC_MAX_SUSPECTS 4096

/* CG5: allocations between AUTO collections.  The second half of the AUTO
 * heuristic: a program that buffers few candidates but allocates heavily still
 * gets swept, which a candidate-count trigger alone would miss entirely. */
#define GC_AUTO_ALLOC_INTERVAL 4096

/* Suspect roots buffer - global for v1 (per-thread in future).
 * CG0: heap-allocated and grown on demand (was a fixed 4096-entry array whose
 * overflow silently dropped blocks -- see
 * docs/archive/history/gc-strong-cycles-not-collected.md). */
extern RcControlBlock **gc_suspect_roots;
extern uint32_t gc_suspect_count;
extern uint32_t gc_suspect_capacity;

/* GC state */
extern GcMode gc_mode;
extern bool gc_enabled;

/* Initialize the GC subsystem */
void gc_init(void);

/* Shutdown the GC subsystem */
void gc_shutdown(void);

/* Called when strong count reaches 0 on a control block */
void gc_on_strong_decrement(RcControlBlock *cb);

/* CG1: classic Bacon-Rajan PossibleRoot. Called on a strong decrement that
 * leaves the count > 0 -- the edge a self-sustaining cycle actually produces,
 * and the one the old zombie-only hook never saw. Colors the block PURPLE and
 * buffers it as a candidate root (O(1), deduped via cb->gc_buffered).
 *
 * Buffering alone does not reclaim anything: the mark phase still treats every
 * block with strong_count > 0 as a root, so candidates are re-blackened each
 * collection. CG2 replaces that phase with real trial deletion, which is what
 * turns these candidates into collected garbage. */
void gc_possible_root(RcControlBlock *cb);

/* Main collection function - runs the Bacon-Rajan algorithm */
void gc_collect(void);

/* Force a full collection cycle */
void gc_force(void);

/* Enable cycle collection */
void gc_enable(void);

/* Disable cycle collection */
void gc_disable(void);

/* Set GC mode */
void gc_set_mode(GcMode mode);

/* CG5: enable collection in GC_AUTO mode -- what `(gc-auto!)` lowers to. */
void gc_auto(void);

/* ------------------------------------------------------------------------- *
 * CG6: observability.
 *
 * The counters have existed since Phase 10 but nothing surfaced them, so
 * "run the suite and see how much garbage accumulates" was unanswerable.
 * These are the four numbers `(gc-stats ...)` reads, plus TUR_GC_TRACE for a
 * per-collection log.
 *
 * All four are plain counts, so `:int` on the Turmeric side is the honest type
 * -- not a stand-in for something structured.
 * ------------------------------------------------------------------------- */
extern uint64_t gc_collections;         /* collections run */
extern uint64_t gc_objects_freed;       /* control blocks reclaimed */
extern uint64_t gc_candidate_high_water;/* peak candidate-buffer occupancy */

/* Accessors, so the emitted intrinsics call a function rather than naming a
 * global -- the globals are `static` in the emitted replica but external in the
 * archive, and a call works for both. */
uint64_t gc_stat_collections(void);
uint64_t gc_stat_objects_freed(void);
uint64_t gc_stat_live_blocks(void);
uint64_t gc_stat_candidate_high_water(void);

/* CG5: the allocation checkpoint.  Called from gc_register_block, i.e. on every
 * rc block allocation, and collects if GC_AUTO's heuristic says to.
 *
 * ALLOCATION is deliberately the only automatic trigger site.  The obvious
 * alternative -- collecting when the candidate buffer grows, on the
 * rc_strong_decrement path -- reenters the collector from inside a caller's
 * refcount drop, re-marking and potentially freeing blocks mid-mutation.  That
 * is what DEDUP-4a removed from gc_add_suspect and must not come back.  At an
 * allocation site no caller is mid-mutation. */
void gc_on_alloc_checkpoint(void);

/* Check if the value referenced by a weak pointer is still alive
 * (strong count > 0 or reachable from strong roots) */
bool gc_is_alive(RcControlBlock *cb);

/* Mark a control block with a color */
void gc_set_color(RcControlBlock *cb, GcColor color);

/* Get the color of a control block */
GcColor gc_get_color(RcControlBlock *cb);

/* Register a newly allocated control block with the GC */
void gc_register_block(RcControlBlock *cb);

/* Unregister a control block (when freed) */
void gc_unregister_block(RcControlBlock *cb);

#endif /* TUR_GC_H */
