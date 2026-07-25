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
    return rc_cb_alloc_kinded(value_size, value_type, drop_fn,
                              RCK_OPAQUE, RCEXP_OPAQUE);
}

/* EXG5-4: kinded variant -- as rc_cb_alloc but writes the layout tags
 * into the reserved bytes so the cycle walker and any kind-aware drop
 * paths can recognise the block.  Existing callers continue to flow
 * through the older rc_cb_alloc entry point with implicit (RCK_OPAQUE,
 * RCEXP_OPAQUE). */
RcControlBlock *rc_cb_alloc_kinded(size_t value_size, TypeKind value_type,
                                   RcDropFn drop_fn, uint8_t kind,
                                   uint8_t payload_kind) {
    size_t total_size = sizeof(RcControlBlock) + value_size;
    RcControlBlock *cb = (RcControlBlock *)malloc(total_size);
    if (!cb) {
        fprintf(stderr, "rc: out of memory allocating control block\n");
        abort();
    }

    cb->strong_count = 1;
    cb->weak_count = 0;
    cb->value = (void *)(cb + 1);
    cb->drop_fn = drop_fn ? drop_fn : default_drop_fn_for_type(value_type);
    cb->walk_fn = NULL;
    cb->value_type_kind = value_type;

    cb->color = GC_WHITE;
    cb->may_contain_cycles = true;
    cb->gc_index = RC_GC_INDEX_NONE;   /* CG0: set by gc_register_block below */
    cb->gc_buffered = false;           /* CG1: not in the candidate buffer */
    cb->gc_trial = 0;                  /* CG2: scratch, set per collection */
    cb->gc_collecting = false;         /* CG2: not being collected */
    memset(cb->reserved, 0, sizeof(cb->reserved));
    cb->reserved[0] = kind;
    cb->reserved[1] = payload_kind;

    gc_register_block(cb);

    return cb;
}

/* DS3: as rc_cb_alloc_kinded with RCK_STRUCT + a walker function so the
 * cycle walker can enumerate the struct's rc-typed children. */
RcControlBlock *rc_cb_alloc_struct(size_t value_size, TypeKind value_type,
                                   RcDropFn drop_fn, RcWalkFn walk_fn) {
    RcControlBlock *cb = rc_cb_alloc_kinded(value_size, value_type, drop_fn,
                                            RCK_STRUCT, 0);
    cb->walk_fn = walk_fn;
    return cb;
}

/* Free a control block and its value.
 * Called when both strong_count and weak_count reach 0.
 */
void rc_cb_free(RcControlBlock *cb) {
    if (!cb) return;

    /* Unregister from GC tracking */
    gc_unregister_block(cb);

    /* F1-2-4: smart drop for RCK_EXISTENTIAL blocks whose payload is
     * itself an rc reference (RCEXP_RC).  The packed value's first 8
     * bytes hold the inner RcControlBlock pointer (laid out by
     * emit_expr.c's EX_EXISTS_PACK path; mirrored by the cycle walker
     * in gc.c).  Decrement that inner reference *before* the
     * existential's own drop hook fires so the inner allocation does
     * not leak when the outer existential is reclaimed.
     *
     * F1-2-5: the drop dispatch lives here rather than in
     * tur_existential_drop because rc_cb_free is the single
     * teardown entry point that is guaranteed to run once per
     * block; the per-program drop hook in emit_module.c stays a
     * no-op (preserving the original layout-free interface). */
    if (cb->value && cb->reserved[0] == RCK_EXISTENTIAL &&
            cb->reserved[1] == RCEXP_RC) {
        int64_t raw = *(const int64_t *)cb->value;
        RcControlBlock *inner = (RcControlBlock *)(intptr_t)raw;
        if (inner) {
            (void)rc_strong_decrement(inner);
        }
    }

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

    /* CG2: this block is part of a white set currently being freed. Its
     * drop_fn is running and decrementing rc children that are themselves in
     * that set, so the count must fall without triggering a free (double free)
     * or a candidate buffering (dangling suspect). The collector frees every
     * member itself once all drop_fns have run. */
    if (cb->gc_collecting) {
        if (cb->strong_count > 0) cb->strong_count--;
        return false;
    }

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
    
    /* CG1: the count is still > 0. In classic Bacon-Rajan this is exactly the
     * edge that reveals a possible cycle root -- dropping an external reference
     * to a structure that keeps itself alive through its own back-edges. The
     * pre-CG1 hook only fired at strong->0, which a self-sustaining cycle never
     * reaches, so no cycle member was ever buffered. Gated on gc_mode so the
     * default (collector off) path is a single global compare. */
    if (gc_mode != GC_DISABLED) gc_possible_root(cb);

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
