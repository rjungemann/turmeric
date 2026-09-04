#ifndef TUR_REGION_H
#define TUR_REGION_H
/* region.h -- declared lifetimes (RM3), over the Arena that already ships.
 *
 * The reclamation plan's third phase.  RM1 frees a box whose owner is a scope;
 * RM2 would free a node whose owner is a value, and cannot, because a
 * persistent structure's nodes have no unique owner -- `(SBind v t rest)`
 * shares `rest` with every older chain, so "is this the last reference?" is a
 * runtime fact no static rule answers.
 *
 * A region does not ask who owns a node.  It asks WHEN THE GENERATION DIES.
 * Every node allocated between a push and its matching pop dies at the pop, in
 * one O(slabs) rewind, with no per-node bookkeeping at all.
 *
 * See docs/upcoming/regions-plan.md.  Gated behind `--enable=regions`
 * (g_opt_regions); nothing here runs in a default build.
 *
 * THE SAFETY RULE, and it is the whole design:
 *
 *   A region that cannot prove every escaping value relocatable does not
 *   rewind.
 *
 * There are no partial rewinds and no best-effort.  A shape the escape check
 * cannot handle costs a SAVING, never correctness -- the same discipline
 * turi's value-pool promotion walk runs on ("a missed shape means this eval
 * does not shrink, never use-after-reset").  R1 ships the mechanism with the
 * rule enforced the only way it can be before the escape check exists: a pop
 * rewinds only when the caller has vouched for the generation
 * (`tur_region_pop_reclaim`), and the plain `tur_region_pop` does not rewind
 * at all.  R3 replaces the caller's word with a proof.
 *
 * The Debug arena poisons reclaimed bytes, so a value that outlives its region
 * crashes loudly under ASan rather than reading stale-but-mapped data.  That
 * backstop is why this is shippable behind a flag at all; do not disable it. */
#include <stdbool.h>
#include <stddef.h>

/* Push a new generation.  Returns the depth, which `tur_region_pop*` takes
 * back so a mismatched pair is caught rather than silently rewinding someone
 * else's generation. */
int  tur_region_push(void);

/* Pop WITHOUT reclaiming: the generation's memory stays live for the process.
 * The conservative default, and what R1 uses everywhere -- correctness with no
 * saving, which is the safe half of the rule above. */
void tur_region_pop(int depth);

/* Pop AND rewind.  The caller asserts that nothing allocated in this
 * generation is still reachable.  R3 makes that assertion checkable; until
 * then this is only reached from code that owns both ends of the scope. */
void tur_region_pop_reclaim(int depth);

/* Allocate `n` bytes in the innermost live generation.  Returns NULL when no
 * region is open, which every caller must treat as "use malloc instead" -- a
 * region is an optimisation, never a requirement. */
void *tur_region_alloc(size_t n);

/* True when `p` points into any live generation.  The guard the reclamation
 * plan requires on every free path: a pointer into region memory must never
 * reach `free()`, because the slab, not the pointer, owns it. */
bool tur_region_owns(const void *p);

/* True when at least one generation is open. */
bool tur_region_active(void);

/* Release every generation and the backing arena.  Process teardown only. */
void tur_region_shutdown(void);

#endif
