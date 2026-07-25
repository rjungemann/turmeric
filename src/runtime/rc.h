/* rc.h - Reference counting for Turmeric (Phase 9)
 * 
 * This implements rc<T> (reference-counted ownership) and weak<T> 
 * (non-owning observation) as the v1 GC strategy.
 * 
 * See effects-plan.md §5.1.2 for design details.
 */

#ifndef TUR_RC_H
#define TUR_RC_H

#include <stdint.h>
#include <stdbool.h>

#include "arena.h"
#include "types.h"

/* Phase 10: GC color enum for Bacon-Rajan cycle collector */
typedef enum {
    GC_WHITE,   /* Not yet scanned */
    GC_GREY,    /* In work queue, being scanned */
    GC_BLACK,   /* Reachable from strong roots */
    GC_PURPLE   /* Suspect (strong count == 0, weak count > 0) */
} GcColor;

/* Phase 9: Reference counting control block layout */

/* Forward declaration */
typedef struct RcControlBlock RcControlBlock;

/* Custom drop function type for rc<T> where T has a destructor */
typedef void (*RcDropFn)(void *value);

/* DS3: walker callback invoked once per rc-typed child of `value`.
 * Used by gc_mark_phase for RCK_STRUCT blocks to enumerate children. */
typedef void (*RcWalkChildFn)(RcControlBlock *child, void *ctx);
typedef void (*RcWalkFn)(void *value, RcWalkChildFn cb, void *ctx);

/* The control block layout for rc<T> and weak<T>.
 * 
 * Memory layout (for cache efficiency):
 * - strong_count and weak_count on the same cache line
 * - value immediately follows for small T (to take advantage of same cache line)
 * - drop_fn pointer for types with custom destructors
 * 
 * For rc<T>:
 *   - strong_count >= 1
 *   - value points to the actual T value
 * 
 * For weak<T>:
 *   - strong_count is the count from the corresponding rc control block
 *   - weak_count >= 1
 *   - value is NULL (only the control block exists)
 * 
 * When strong_count reaches 0:
 *   - If weak_count > 0: object enters "zombie" state (memory still valid)
 *   - If weak_count == 0: immediately free value and control block
 */
struct RcControlBlock {
    /* Reference counts */
    uint64_t strong_count;   /* Number of rc<T> pointers to the value */
    uint64_t weak_count;     /* Number of weak<T> pointers to the control block */

    /* Pointer to the actual value (allocated right after this struct) */
    void *value;

    /* Custom drop function (NULL for types that use free()) */
    RcDropFn drop_fn;

    /* DS3: walker function -- enumerates rc-typed children for the cycle
     * collector.  NULL when the value has no rc children to follow
     * (the default for RCK_OPAQUE / RCK_EXISTENTIAL blocks). */
    RcWalkFn walk_fn;

    /* Type information for debugging.
     *
     * DEDUP-1: stored as a fixed-width byte, NOT the `TypeKind` enum. The
     * compiler emits its own copy of this struct into every compiled program
     * (emit_module.c) where these two fields have always been `uint8_t`; an
     * enum is implementation-defined width (4 bytes here), so the two layouts
     * silently disagreed from `value_type_kind` onward -- every field after it,
     * including all the GC bookkeeping, sat at a different offset in the two
     * copies. That blocked ever linking one against the other and is the same
     * class of bug as CG3's `:heap` mis-cast, but process-wide. The widths are
     * now pinned by _Static_assert in rc.c and in the emitted preamble. */
    uint8_t value_type_kind;

    /* Phase 10: Bacon-Rajan cycle collector fields */
    uint8_t color;           /* GC color (GcColor), fixed-width -- see above */
    bool may_contain_cycles;  /* Hint: true if this could be part of a cycle */
    /* CG0: index of this block in the global gc_all_blocks registry, or
     * RC_GC_INDEX_NONE when not registered.  Storing it here makes
     * gc_unregister_block an O(1) swap-remove instead of an O(live-blocks)
     * linear scan -- which matters because EVERY rc free unregisters, even
     * with the collector disabled. */
    uint32_t gc_index;
    /* CG1: true while this block sits in the suspect/candidate-root buffer.
     * Classic Bacon-Rajan `buffered` flag -- gives O(1) dedup on the
     * PossibleRoot path, which now fires on EVERY strong decrement that leaves
     * the count > 0. The old linear scan over the buffer would have made that
     * quadratic. */
    bool gc_buffered;
    /* CG2: scratch trial refcount, recomputed at the start of every collection.
     * Bacon-Rajan's trial deletion subtracts internal (in-cycle) edges to find
     * blocks referenced only from within the candidate subgraph. The textbook
     * algorithm decrements the REAL refcount and restores it; we use a scratch
     * copy instead, so an incomplete or asymmetric walk_fn can never corrupt a
     * live count -- the worst case becomes a missed cycle (a leak), never a
     * use-after-free. */
    uint64_t gc_trial;
    /* CG2: set while this block is in the white set being freed. A struct's
     * drop_fn decrements its rc children; for a cycle those children are also
     * being freed, so rc_strong_decrement must neither queue them for free nor
     * buffer them as candidates while this is set. */
    bool gc_collecting;
    /* EXG5: layout tag bytes -- reserved[0] is the high-level kind
     * (one of RCK_*), reserved[1] is the payload descriptor for
     * RCK_EXISTENTIAL blocks (one of RCEXP_*).  The remaining bytes
     * are still reserved for future use. */
    uint8_t reserved[6];
};

/* CG0: sentinel for RcControlBlock.gc_index -- block not in the registry. */
#define RC_GC_INDEX_NONE ((uint32_t)0xFFFFFFFFu)

/* EXG5-1: High-level layout tag for an RcControlBlock's value field.
 * Stored in cb->reserved[0].  The default (zero) is RCK_OPAQUE so
 * existing call sites do not need to change. */
#define RCK_OPAQUE        0  /* value is a scalar / bit pattern (default) */
#define RCK_EXISTENTIAL   1  /* value points at a tur_existential_t record */
/* DS3: value points at a struct with rc-typed fields.  cb->walk_fn (if
 * non-NULL) enumerates them so the cycle walker can trace through. */
#define RCK_STRUCT        2

/* EXG5-1: Payload descriptor for RCK_EXISTENTIAL blocks.
 * Describes the type of bits stored in tur_existential_t::value, so the
 * cycle walker (gc_mark_phase) knows whether to follow it as a pointer.
 * Stored in cb->reserved[1]; meaningless when kind != RCK_EXISTENTIAL. */
#define RCEXP_OPAQUE      0  /* scalar / bit pattern (no recursion) */
#define RCEXP_RC          1  /* RcControlBlock pointer (follow it) */

/* Size of the control block header (without the value) */
#define RC_CB_HEADER_SIZE (sizeof(RcControlBlock))

/* Allocate a control block with space for a value of size `value_size`.
 * Initializes strong_count to 1, weak_count to 0.  The block is tagged
 * RCK_OPAQUE (cycle walker treats `value` as a scalar / bit pattern).
 * Returns pointer to the control block.
 */
RcControlBlock *rc_cb_alloc(size_t value_size, TypeKind value_type, RcDropFn drop_fn);

/* EXG5-4: Allocate a control block with explicit layout tags.
 * `kind` is one of RCK_* and describes how the value field is laid out.
 * `payload_kind` is one of RCEXP_* and (when kind == RCK_EXISTENTIAL)
 * describes whether the existential's `value` slot is a scalar or an
 * RcControlBlock pointer the cycle walker must follow.  For other
 * kinds, payload_kind is ignored.
 *
 * Otherwise identical to rc_cb_alloc; the older entry point now just
 * calls this with (RCK_OPAQUE, RCEXP_OPAQUE).
 */
RcControlBlock *rc_cb_alloc_kinded(size_t value_size, TypeKind value_type,
                                   RcDropFn drop_fn, uint8_t kind,
                                   uint8_t payload_kind);

/* DS3: allocate a control block for a struct payload with an attached
 * walker function.  Tags the block as RCK_STRUCT so the cycle walker
 * invokes `walk_fn` to enumerate rc-typed children.  `drop_fn` runs
 * normally on the final strong decrement. */
RcControlBlock *rc_cb_alloc_struct(size_t value_size, TypeKind value_type,
                                   RcDropFn drop_fn, RcWalkFn walk_fn);

/* Free a control block and its value.
 * Called when both strong_count and weak_count reach 0.
 */
void rc_cb_free(RcControlBlock *cb);

/* Increment the strong count. Returns the new count. */
uint64_t rc_strong_increment(RcControlBlock *cb);

/* Decrement the strong count. If count reaches 0, may free the value.
 * Returns true if the value was freed, false otherwise.
 */
bool rc_strong_decrement(RcControlBlock *cb);

/* Increment the weak count. Returns the new count. */
uint64_t rc_weak_increment(RcControlBlock *cb);

/* Decrement the weak count. If both strong and weak counts are 0, frees the control block.
 * Returns true if the control block was freed, false otherwise.
 */
bool rc_weak_decrement(RcControlBlock *cb);

/* Get the strong count (for debugging/testing). */
uint64_t rc_strong_count(RcControlBlock *cb);

/* Get the weak count (for debugging/testing). */
uint64_t rc_weak_count(RcControlBlock *cb);

/* Check if the value is still alive (strong count > 0). */
bool rc_is_alive(RcControlBlock *cb);

/* Upgrade a weak pointer to an rc pointer.
 * Returns NULL if the strong count is 0 (value has been freed).
 * If successful, increments the strong count.
 */
RcControlBlock *rc_upgrade(RcControlBlock *cb);

/* Future-proofing for Phase 10 (Bacon-Rajan cycle collector) */
/* void gc_on_strong_decrement(RcControlBlock *cb); */
/* void gc_collect(void); */

#endif /* TUR_RC_H */
