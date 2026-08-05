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
    uint32_t count;           /* Number of slots used (drained prefix included) */
    uint32_t capacity;        /* Allocated capacity */
    /* rc-free-queue-drain-quadratic: cursor of the next block to free.  The
     * drain used to pop from the front and memmove the remainder down one slot
     * on EVERY pop, which made draining n blocks move ~n^2/2 pointers -- 378 ms
     * to free 65,000 blocks.  Advancing a cursor instead makes it O(n); the
     * drained prefix [0, head) is reclaimed by compaction only when the queue
     * would otherwise overflow. */
    uint32_t head;
    /* True while rc_free_queue_drain is walking the queue.
     *
     * Drop glue pushes children (nested drops) and CALLS DRAIN AGAIN -- the
     * emitted struct-field glue does exactly that -- and that glue runs from
     * inside the walk.  Without this flag the nested call carries on freeing
     * from the shared cursor, so it does not double-free; what it does instead
     * is add a C stack frame per link, which defeats the entire reason this
     * queue exists ("constant stack depth regardless of nesting depth", top of
     * this file).  Measured on a 200,000-deep chain: ASan stack-overflow, on
     * the pre-fix code as well as on a cursor drain without the guard.  The
     * quadratic memmove was what kept anyone from driving a cascade deep enough
     * to hit it.
     *
     * So this is load-bearing, not defensive: with the flag, the outer walk
     * frees the children the glue queued, in the same pass, still FIFO. */
    bool draining;
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

/* rc-free-queue-drain-quadratic: clear the in-drain flag after a panic unwound
 * THROUGH a drain.
 *
 * Drop glue is user-reachable code, so a panic can longjmp out of the middle of
 * rc_free_queue_drain, skipping the assignment that clears `draining`.  The
 * flag would then be stuck on for the rest of the process and every later drain
 * would no-op -- a silent, unbounded leak.  Called from the panic-caught branch
 * of tur_catch_unwind, alongside the other runtime state reset there.
 *
 * Safe to call at any time: it only clears the flag, and the blocks that were
 * pending stay queued for the next drain. */
void rc_free_queue_reset_drain_state(void);

#endif /* TUR_RC_FREE_QUEUE_H */
