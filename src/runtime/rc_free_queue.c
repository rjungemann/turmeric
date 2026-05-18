/* rc_free_queue.c - Deferred free queue implementation for RC values (Phase 9)
 */

#include "rc_free_queue.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Global deferred free queue - v1 is single-threaded */
static RcFreeQueue rc_free_queue = {0};

void rc_free_queue_init(void) {
    if (rc_free_queue.items) {
        return;  /* Already initialized */
    }
    
    rc_free_queue.capacity = RC_FREE_QUEUE_CAPACITY;
    rc_free_queue.items = (RcControlBlock **)malloc(
        rc_free_queue.capacity * sizeof(RcControlBlock *)
    );
    if (!rc_free_queue.items) {
        fprintf(stderr, "rc_free_queue: out of memory during init\n");
        abort();
    }
    
    rc_free_queue.count = 0;
}

void rc_free_queue_shutdown(void) {
    if (rc_free_queue.items) {
        /* Drain any remaining items before shutting down */
        rc_free_queue_drain();
        
        free(rc_free_queue.items);
        rc_free_queue.items = NULL;
        rc_free_queue.capacity = 0;
        rc_free_queue.count = 0;
    }
}

void rc_free_queue_push(RcControlBlock *cb) {
    if (!cb) return;
    
    /* Initialize queue on first use */
    if (!rc_free_queue.items) {
        rc_free_queue_init();
    }
    
    /* Check capacity */
    if (rc_free_queue.count >= rc_free_queue.capacity) {
        fprintf(stderr, "rc_free_queue: queue full (capacity: %u), draining...\n",
                rc_free_queue.capacity);
        rc_free_queue_drain();
        
        if (rc_free_queue.count >= rc_free_queue.capacity) {
            fprintf(stderr, "rc_free_queue: cannot queue block, skipping\n");
            return;
        }
    }
    
    rc_free_queue.items[rc_free_queue.count++] = cb;
}

uint32_t rc_free_queue_drain(void) {
    if (!rc_free_queue.items || rc_free_queue.count == 0) {
        return 0;
    }
    
    uint32_t freed = 0;
    
    /* Process queue iteratively to avoid stack overflow */
    while (rc_free_queue.count > 0) {
        /* Pop from front (FIFO) */
        RcControlBlock *cb = rc_free_queue.items[0];
        
        /* Shift remaining items */
        memmove(rc_free_queue.items, rc_free_queue.items + 1,
                (rc_free_queue.count - 1) * sizeof(RcControlBlock *));
        rc_free_queue.count--;
        
        /* Free the control block
         * (Any newly queued items from nested drops will be added to the back) */
        rc_cb_free(cb);
        freed++;
    }
    
    return freed;
}

bool rc_free_queue_is_empty(void) {
    return rc_free_queue.count == 0;
}

uint32_t rc_free_queue_size(void) {
    return rc_free_queue.count;
}
