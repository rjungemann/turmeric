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
    rc_free_queue.head = 0;
    rc_free_queue.draining = false;
}

/* rc-free-queue-drain-quadratic: reclaim the drained prefix [0, head).
 *
 * The one memmove the cursor design still needs, and it runs only when the
 * queue would otherwise overflow -- not once per freed block.  Each compaction
 * moves the UNdrained tail only, and it can only happen after `head` slots have
 * been consumed, so the moved-pointer total stays linear in the number of
 * blocks freed rather than quadratic. */
static void rc_free_queue_compact(void) {
    if (rc_free_queue.head == 0) return;
    uint32_t live = rc_free_queue.count - rc_free_queue.head;
    if (live > 0) {
        memmove(rc_free_queue.items,
                rc_free_queue.items + rc_free_queue.head,
                (size_t)live * sizeof(RcControlBlock *));
    }
    rc_free_queue.count = live;
    rc_free_queue.head = 0;
}

void rc_free_queue_shutdown(void) {
    if (rc_free_queue.items) {
        /* Drain any remaining items before shutting down */
        rc_free_queue_drain();
        
        free(rc_free_queue.items);
        rc_free_queue.items = NULL;
        rc_free_queue.capacity = 0;
        rc_free_queue.count = 0;
        rc_free_queue.head = 0;
        rc_free_queue.draining = false;
    }
}

void rc_free_queue_push(RcControlBlock *cb) {
    if (!cb) return;
    
    /* Initialize queue on first use */
    if (!rc_free_queue.items) {
        rc_free_queue_init();
    }
    
    if (rc_free_queue.count >= rc_free_queue.capacity) {
        /* 1. Reclaim any drained prefix first -- inside a drain this is usually
         *    the whole array, so the queue rarely has to grow at all. */
        rc_free_queue_compact();

        /* 2. Outside a drain, freeing everything pending is the cheapest way to
         *    make room AND is what bounds the queue's memory.  Never from
         *    inside one: that is the reentrancy the `draining` flag exists to
         *    prevent (and the same shape DEDUP-4a removed from the collector's
         *    suspect path -- a free must not re-enter the freer). */
        if (!rc_free_queue.draining && rc_free_queue.count >= rc_free_queue.capacity)
            rc_free_queue_drain();

        /* 3. Still no room -- we are mid-drain and the drop glue is pushing
         *    children faster than the cursor consumes them.  Grow.
         *
         *    The old code printed to stderr and then SKIPPED the block, which
         *    leaked it outright; the emitted replica aborted instead.  Growing
         *    is what the GC's own vectors settled on (CG0) for exactly this. */
        if (rc_free_queue.count >= rc_free_queue.capacity) {
            uint32_t ncap = rc_free_queue.capacity ? rc_free_queue.capacity * 2u
                                                   : RC_FREE_QUEUE_CAPACITY;
            RcControlBlock **grown = (RcControlBlock **)realloc(
                rc_free_queue.items, (size_t)ncap * sizeof(RcControlBlock *));
            if (!grown) {
                /* Genuine OOM: leak this block rather than abort.  A leak is
                 * never unsafe; the process is already out of memory. */
                fprintf(stderr, "rc_free_queue: out of memory, leaking a block\n");
                return;
            }
            rc_free_queue.items = grown;
            rc_free_queue.capacity = ncap;
        }
    }

    rc_free_queue.items[rc_free_queue.count++] = cb;
}

uint32_t rc_free_queue_drain(void) {
    if (!rc_free_queue.items || rc_free_queue.count == 0) {
        return 0;
    }
    /* Reentrancy: an outer drain already owns the walk, and every block this
     * call would free is either already freed or still ahead of its cursor.
     * Returning 0 is honest -- THIS call freed nothing. */
    if (rc_free_queue.draining) {
        return 0;
    }
    rc_free_queue.draining = true;

    uint32_t freed = 0;

    /* Process iteratively to keep stack depth constant regardless of nesting.
     *
     * Both `head` and `count` are re-read every iteration and deliberately not
     * cached: rc_cb_free runs drop glue, which pushes children onto the back
     * (growing `count`, and possibly reallocating or compacting `items`, which
     * rewrites `head`).  FIFO order is preserved -- children queued by this
     * pass are freed after everything already pending, in the same pass. */
    while (rc_free_queue.head < rc_free_queue.count) {
        RcControlBlock *cb = rc_free_queue.items[rc_free_queue.head++];
        rc_cb_free(cb);
        freed++;
    }

    rc_free_queue.count = 0;
    rc_free_queue.head = 0;
    rc_free_queue.draining = false;
    return freed;
}

bool rc_free_queue_is_empty(void) {
    return rc_free_queue_size() == 0;
}

void rc_free_queue_reset_drain_state(void) {
    /* Only the flag: `head` and `count` still describe a consistent queue --
     * everything before `head` is freed, everything from `head` is pending --
     * so the next drain resumes correctly rather than re-freeing. */
    rc_free_queue.draining = false;
}

uint32_t rc_free_queue_size(void) {
    /* Blocks still PENDING -- the drained prefix does not count.  Reported this
     * way so a caller sampling the queue mid-drain sees what is left to do. */
    return rc_free_queue.count - rc_free_queue.head;
}
