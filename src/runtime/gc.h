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
    GC_THRESHOLD   /* Collection when suspect buffer exceeds threshold */
} GcMode;

/* Configuration constants */
#define GC_SUSPECT_THRESHOLD 128  /* Trigger collection when this many suspects */
/* CG0: this is now a "force a collection at this size" trigger, NOT a hard cap.
 * The suspect buffer grows past it if a collection cannot drain it, because
 * silently dropping suspects would make the collector miss real garbage. */
#define GC_MAX_SUSPECTS 4096

/* Suspect roots buffer - global for v1 (per-thread in future).
 * CG0: heap-allocated and grown on demand (was a fixed 4096-entry array whose
 * overflow silently dropped blocks -- see
 * docs/reported/gc-strong-cycles-not-collected.md). */
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
