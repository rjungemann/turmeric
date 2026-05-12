#ifndef TUR_ATOMIC_QUEUE_H
#define TUR_ATOMIC_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Phase T23: Lock-free MPSC (multi-producer, single-consumer) queue
 * for cross-thread fiber submission.
 * 
 * Uses a ring buffer with atomic head/tail indices.
 * This is a simplified v1 implementation that assumes a single consumer
 * (the scheduler thread) and multiple producers (worker threads).
 */

typedef struct AtomicQueue AtomicQueue;

/* Opaque type for queue elements */
typedef void *AtomicQueueItem;

/* Create a new atomic queue with the given capacity (must be power of 2) */
AtomicQueue *aq_new(size_t capacity);

/* Free the atomic queue */
void aq_free(AtomicQueue *q);

/* Push an item to the queue (thread-safe, lock-free) */
bool aq_push(AtomicQueue *q, AtomicQueueItem item);

/* Pop an item from the queue (single-threaded consumer only) */
AtomicQueueItem aq_pop(AtomicQueue *q, bool *success);

/* Check if queue is empty */
bool aq_empty(AtomicQueue *q);

#endif /* TUR_ATOMIC_QUEUE_H */
