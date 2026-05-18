/* rc_free_queue.h - Deferred free queue for RC values (Phase 9)
 *
 * This implements a deferred free queue to avoid deep recursive free chains
 * when rc<T> values contain nested rc<T> values.
 *
 * Problem: When rc_strong_decrement reduces strong_count to 0, if the value
 * itself contains rc<T> pointers, those decrments trigger recursively,
 * potentially causing stack overflow for deeply nested structures.
 *
 * Solution: Queue drops instead of executing immediately, then drain the queue
 * iteratively to maintain constant stack depth regardless of nesting depth.
 */

#ifndef TUR_RC_FREE_QUEUE_H
#define TUR_RC_FREE_QUEUE_H

#include <stdint.h>
#include <stdbool.h>

#include "rc.h"

/* Deferred free queue - global for v1 (per-thread in future) */
#define RC_FREE_QUEUE_CAPACITY 65536  /* Max pending frees */

typedef struct {
    RcControlBlock **items;   /* Queue of control blocks to free */
    uint32_t count;           /* Current queue size */
    uint32_t capacity;        /* Allocated capacity */
} RcFreeQueue;

/* Initialize the deferred free queue */
void rc_free_queue_init(void);

/* Shutdown the deferred free queue (must drain it first) */
void rc_free_queue_shutdown(void);

/* Add a control block to the deferred free queue.
 * This prevents immediate recursive rc_strong_decrement calls.
 * Called by rc_strong_decrement when strong_count reaches 0 and weak_count is also 0.
 */
void rc_free_queue_push(RcControlBlock *cb);

/* Drain the deferred free queue, freeing all pending blocks iteratively.
 * This ensures constant stack depth regardless of nesting depth.
 * Returns the number of blocks freed.
 */
uint32_t rc_free_queue_drain(void);

/* Check if the queue is empty */
bool rc_free_queue_is_empty(void);

/* Get the current queue size (for debugging/testing) */
uint32_t rc_free_queue_size(void);

#endif /* TUR_RC_FREE_QUEUE_H */
