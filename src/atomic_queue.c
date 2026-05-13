#include "atomic_queue.h"
#include <stdlib.h>
#include <string.h>

/* Phase T23: Lock-free MPSC queue implementation
 * 
 * Uses a ring buffer with atomic head/tail for lock-free push.
 * Pop is only called by the scheduler thread (single consumer).
 * 
 * Capacity must be a power of 2 for efficient modulo operations.
 */

#if defined(__clang__) || defined(__GNUC__)
#define ATOMIC_T _Atomic
#else
#define ATOMIC_T volatile
#endif

struct AtomicQueue {
    AtomicQueueItem *buffer;
    size_t capacity;
    size_t mask;  /* capacity - 1, for fast modulo */
    ATOMIC_T size_t head;
    ATOMIC_T size_t tail;
};

AtomicQueue *aq_new(size_t capacity) {
    /* Round up to next power of 2 */
    if (capacity < 2) capacity = 2;
    capacity--;
    capacity |= capacity >> 1;
    capacity |= capacity >> 2;
    capacity |= capacity >> 4;
    capacity |= capacity >> 8;
    capacity |= capacity >> 16;
#if SIZE_MAX > UINT32_MAX
    capacity |= capacity >> 32;
#endif
    capacity++;

    AtomicQueue *q = (AtomicQueue *)calloc(1, sizeof(AtomicQueue));
    if (!q) return NULL;

    q->buffer = (AtomicQueueItem *)calloc(capacity, sizeof(AtomicQueueItem));
    if (!q->buffer) { free(q); return NULL; }

    q->capacity = capacity;
    q->mask = capacity - 1;
    q->head = 0;
    q->tail = 0;

    return q;
}

void aq_free(AtomicQueue *q) {
    if (!q) return;
    free(q->buffer);
    free(q);
}

/* Push an item to the queue using atomic compare-and-swap on tail */
bool aq_push(AtomicQueue *q, AtomicQueueItem item) {
    size_t tail = q->tail;
    size_t new_tail = tail + 1;
    size_t head = q->head;

    /* Check if queue is full */
    if (new_tail - head >= q->capacity) {
        return false;  /* Full */
    }

    /* Try to atomically reserve a slot */
    if (!__atomic_compare_exchange_n((size_t *)&q->tail, &tail, new_tail, 0, 
                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        return false;  /* CAS failed, retry */
    }

    /* Store the item */
    __atomic_store_n(&q->buffer[tail & q->mask], item, __ATOMIC_RELEASE);

    return true;
}

/* Pop an item from the queue (single-threaded consumer) */
AtomicQueueItem aq_pop(AtomicQueue *q, bool *success) {
    size_t head = __atomic_load_n((size_t *)&q->head, __ATOMIC_RELAXED);
    size_t tail = __atomic_load_n((size_t *)&q->tail, __ATOMIC_RELAXED);

    if (head >= tail) {
        *success = false;
        return NULL;
    }

    AtomicQueueItem item = __atomic_load_n(&q->buffer[head & q->mask], __ATOMIC_ACQUIRE);
    __atomic_store_n((size_t *)&q->head, head + 1, __ATOMIC_RELEASE);

    *success = true;
    return item;
}

bool aq_empty(AtomicQueue *q) {
    size_t head = __atomic_load_n((size_t *)&q->head, __ATOMIC_RELAXED);
    size_t tail = __atomic_load_n((size_t *)&q->tail, __ATOMIC_RELAXED);
    return head >= tail;
}
