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

/* Phase 9: Reference counting control block layout */

/* Forward declaration */
typedef struct RcControlBlock RcControlBlock;

/* Custom drop function type for rc<T> where T has a destructor */
typedef void (*RcDropFn)(void *value);

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
    
    /* Type information for debugging */
    TypeKind value_type_kind;
    
    /* Future-proofing: reserved fields for GC integration (Phase 10) */
    /* enum gc_color { GC_WHITE, GC_GREY, GC_BLACK, GC_PURPLE } color; */
    uint8_t reserved[8];
};

/* Size of the control block header (without the value) */
#define RC_CB_HEADER_SIZE (sizeof(RcControlBlock))

/* Allocate a control block with space for a value of size `value_size`.
 * Initializes strong_count to 1, weak_count to 0.
 * Returns pointer to the control block.
 */
RcControlBlock *rc_cb_alloc(size_t value_size, TypeKind value_type, RcDropFn drop_fn);

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
