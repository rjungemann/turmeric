/* rc.c - Reference counting implementation for Turmeric (Phase 9)
 * 
 * This implements rc<T> (reference-counted ownership) and weak<T> 
 * (non-owning observation) as the v1 GC strategy.
 */

#include "rc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Phase 10: Include GC header for cycle collection integration */
#include "gc.h"

/* Phase 9: Include deferred free queue to avoid deep recursion */
#include "rc_free_queue.h"

/* Default drop function: just call free() on the value */
static void default_drop_fn(void *value) {
    free(value);
}

/* Drop hook for ref<T> payloads stored in rc<ref<T>>.
 * rc/of stores payload as a heap-allocated cell containing the inner pointer,
 * so we must free both the pointee and the payload cell. */
static void drop_ref_payload(void *value) {
    if (!value) return;
    void *inner = *((void **)value);
    if (inner) {
        free(inner);
    }
    free(value);
}

/* Drop hook for rc<T> payloads stored in rc<rc<T>>.
 * Payload is a heap-allocated cell containing RcControlBlock*. */
static void drop_rc_payload(void *value) {
    if (!value) return;
    RcControlBlock *inner = *((RcControlBlock **)value);
    if (inner) {
        (void)rc_strong_decrement(inner);
        rc_free_queue_drain();
    }
    free(value);
}

/* Drop hook for weak<T> payloads stored in rc<weak<T>>.
 * Payload is a heap-allocated cell containing RcControlBlock*. */
static void drop_weak_payload(void *value) {
    if (!value) return;
    RcControlBlock *inner = *((RcControlBlock **)value);
    if (inner) {
        (void)rc_weak_decrement(inner);
    }
    free(value);
}

static RcDropFn default_drop_fn_for_type(TypeKind value_type) {
    switch (value_type) {
        case TY_REF:
            return drop_ref_payload;
        case TY_RC:
            return drop_rc_payload;
        case TY_WEAK:
            return drop_weak_payload;
        default:
            return default_drop_fn;
    }
}

/* Allocate a control block with space for a value of size `value_size`.
 * The value storage comes right after the control block header.
 * Initializes strong_count to 1, weak_count to 0.
 */
RcControlBlock *rc_cb_alloc(size_t value_size, TypeKind value_type, RcDropFn drop_fn) {
    /* Allocate header + value in one block for cache efficiency */
    size_t total_size = sizeof(RcControlBlock) + value_size;
    RcControlBlock *cb = (RcControlBlock *)malloc(total_size);
    if (!cb) {
        fprintf(stderr, "rc: out of memory allocating control block\n");
        abort();
    }
    
    /* Initialize the control block */
    cb->strong_count = 1;  /* First rc<T> reference */
    cb->weak_count = 0;
    cb->value = (void *)(cb + 1);  /* Value starts right after header */
    cb->drop_fn = drop_fn ? drop_fn : default_drop_fn_for_type(value_type);
    cb->value_type_kind = value_type;
    
    /* Phase 10: Initialize GC fields */
    cb->color = GC_WHITE;
    cb->may_contain_cycles = true;  /* Default: assume it might contain cycles */
    memset(cb->reserved, 0, sizeof(cb->reserved));
    
    /* Register with GC for cycle collection tracking */
    gc_register_block(cb);
    
    return cb;
}

/* Free a control block and its value.
 * Called when both strong_count and weak_count reach 0.
 */
void rc_cb_free(RcControlBlock *cb) {
    if (!cb) return;
    
    /* Unregister from GC tracking */
    gc_unregister_block(cb);
    
    /* Call the drop function on the value */
    if (cb->value) {
        cb->drop_fn(cb->value);
    }
    
    /* Free the entire block (header + value) */
    free(cb);
}

/* Increment the strong count. Returns the new count. */
uint64_t rc_strong_increment(RcControlBlock *cb) {
    if (!cb) return 0;
    return ++cb->strong_count;
}

/* Decrement the strong count. If count reaches 0, may free the value.
 * Returns true if the value was freed, false otherwise.
 */
bool rc_strong_decrement(RcControlBlock *cb) {
    if (!cb) return false;
    
    cb->strong_count--;
    
    if (cb->strong_count == 0) {
        /* Strong count reached 0 */
        if (cb->weak_count > 0) {
            /* Zombie state: value is logically freed but memory still valid
             * for weak pointers to check. Don't free yet. */
            /* Phase 10: Notify GC for cycle collection */
            gc_on_strong_decrement(cb);
            return false;  /* Value not yet freed */
        } else {
            /* Phase 9: Queue for deferred freeing to avoid deep recursion */
            rc_free_queue_push(cb);
            return true;  /* Value was (or will be) freed */
        }
    }
    
    return false;  /* Value not freed */
}

/* Increment the weak count. Returns the new count. */
uint64_t rc_weak_increment(RcControlBlock *cb) {
    if (!cb) return 0;
    return ++cb->weak_count;
}

/* Decrement the weak count. If both strong and weak counts are 0, frees the control block.
 * Returns true if the control block was freed, false otherwise.
 */
bool rc_weak_decrement(RcControlBlock *cb) {
    if (!cb) return false;
    
    cb->weak_count--;
    
    if (cb->weak_count == 0 && cb->strong_count == 0) {
        /* Both counts are 0 - free the control block */
        rc_cb_free(cb);
        return true;  /* Control block was freed */
    }
    
    return false;  /* Control block not freed */
}

/* Get the strong count (for debugging/testing). */
uint64_t rc_strong_count(RcControlBlock *cb) {
    if (!cb) return 0;
    return cb->strong_count;
}

/* Get the weak count (for debugging/testing). */
uint64_t rc_weak_count(RcControlBlock *cb) {
    if (!cb) return 0;
    return cb->weak_count;
}

/* Check if the value is still alive (strong count > 0). */
bool rc_is_alive(RcControlBlock *cb) {
    if (!cb) return false;
    return cb->strong_count > 0;
}

/* Upgrade a weak pointer to an rc pointer.
 * Returns the same control block if strong count > 0 (value is alive).
 * Returns NULL if the strong count is 0 (value has been freed).
 * If successful, increments the strong count.
 */
RcControlBlock *rc_upgrade(RcControlBlock *cb) {
    if (!cb) return NULL;
    
    if (cb->strong_count > 0) {
        /* Value is still alive - increment strong count and return */
        rc_strong_increment(cb);
        return cb;
    } else {
        /* Value has been freed - return NULL */
        return NULL;
    }
}

/* Additional helper functions for codegen integration */

/* Get the value pointer from a control block */
void *rc_get_value(RcControlBlock *cb) {
    if (!cb) return NULL;
    return cb->value;
}

/* Set the value pointer (used during rc/from-ref) */
void rc_set_value(RcControlBlock *cb, void *value, RcDropFn drop_fn) {
    if (!cb) return;
    cb->value = value;
    cb->drop_fn = drop_fn ? drop_fn : default_drop_fn_for_type(cb->value_type_kind);
}
