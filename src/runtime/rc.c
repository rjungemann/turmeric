/* rc.c - Reference counting implementation for Turmeric (Phase 9)
 * 
 * This implements rc<T> (reference-counted ownership) and weak<T> 
 * (non-owning observation) as the v1 GC strategy.
 */

#include <stddef.h>   /* offsetof, for the layout guard below */
#include "rc.h"
/* DEDUP-2 made rc.h standalone (a compiled program must be able to include it)
 * by moving the compiler's types.h in here; DEDUP-4b removes it from here too.
 * types.h is C11 and pulls in the whole type system, which kept this TU out of
 * the C99 runtime archive.  The only thing it was needed for was the three
 * TypeKind ordinals default_drop_fn_for_type switches on, which now live in
 * rc.h as RC_VT_* and are asserted against the real enum by the compiler. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Phase 10: Include GC header for cycle collection integration */
#include "gc.h"

/* Phase 9: Include deferred free queue to avoid deep recursion */
#include "rc_free_queue.h"

/* Default drop function: just call free() on the value.  Correct only for a
 * payload that is its own allocation (tur_rc_from_ref, rc_set_value) -- see
 * inline_scalar_drop_fn for the inline case. */
static void default_drop_fn(void *value) {
    free(value);
}

/* Default drop for a SCALAR payload allocated inline by rc_cb_alloc*: the
 * value lives at (cb + 1), inside the header's own malloc block, so there is
 * nothing separate to free -- rc_cb_free's free(cb) reclaims it.  free(value)
 * here hands free() an interior pointer (glibc SIGABRT / ASan bad-free).
 * A block repointed at a separate allocation via rc_set_value gets its glue
 * re-derived there, so it never keeps this no-op. */
static void inline_scalar_drop_fn(void *value) {
    (void)value;
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

static RcDropFn default_drop_fn_for_type(uint8_t value_type) {
    switch (value_type) {
        case RC_VT_REF:
            return drop_ref_payload;
        case RC_VT_RC:
            return drop_rc_payload;
        case RC_VT_WEAK:
            return drop_weak_payload;
        default:
            return default_drop_fn;
    }
}

/* As default_drop_fn_for_type, but for the rc_cb_alloc* entry points, whose
 * scalar payload is inline rather than a separate allocation.  The three
 * non-scalar glues stay: their payload cell is always separately allocated
 * (installed via rc_set_value), so their free(value) is correct. */
static RcDropFn inline_default_drop_fn_for_type(uint8_t value_type) {
    switch (value_type) {
        case RC_VT_REF:
            return drop_ref_payload;
        case RC_VT_RC:
            return drop_rc_payload;
        case RC_VT_WEAK:
            return drop_weak_payload;
        default:
            return inline_scalar_drop_fn;
    }
}

/* Allocate a control block with space for a value of size `value_size`.
 * The value storage comes right after the control block header.
 * Initializes strong_count to 1, weak_count to 0.
 */
RcControlBlock *rc_cb_alloc(size_t value_size, uint8_t value_type, RcDropFn drop_fn) {
    return rc_cb_alloc_kinded(value_size, value_type, drop_fn,
                              RCK_OPAQUE, RCEXP_OPAQUE);
}

/* EXG5-4: kinded variant -- as rc_cb_alloc but writes the layout tags
 * into the reserved bytes so the cycle walker and any kind-aware drop
 * paths can recognise the block.  Existing callers continue to flow
 * through the older rc_cb_alloc entry point with implicit (RCK_OPAQUE,
 * RCEXP_OPAQUE). */
RcControlBlock *rc_cb_alloc_kinded(size_t value_size, uint8_t value_type,
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
    cb->drop_fn = drop_fn ? drop_fn : inline_default_drop_fn_for_type(value_type);
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

    /* CG5: under GC_AUTO a collection can fire from any later allocation, and
     * the walker traverses every registered block -- including ones whose
     * caller has allocated but not yet written the payload.  Reading an
     * uninitialised payload as a child pointer is a segfault (it was, before
     * this).  Zeroing makes that window harmless: the walker sees a NULL child
     * and skips it, which every walk callback already handles.
     *
     * Confined to AUTO so the always-on RC path keeps its malloc semantics: in
     * every other mode collection only happens where the program asked for it,
     * by which point the caller has finished writing. */
    if (gc_mode == GC_AUTO && value_size) memset(cb->value, 0, value_size);

    gc_register_block(cb);

    /* CG6: a scalar payload (TypeKind <= 7: int/float/bool/...) has no rc
     * children, so it can never be a cycle ROOT.  Marking it lets
     * gc_add_suspect skip it -- see the filter there.  The emitted copy has
     * always set this; the runtime never did, which is half of why the field
     * was dead. */
    if (value_type < RC_VT_REF) cb->may_contain_cycles = false;

    return cb;
}

/* DS3: as rc_cb_alloc_kinded with RCK_STRUCT + a walker function so the
 * cycle walker can enumerate the struct's rc-typed children. */
RcControlBlock *rc_cb_alloc_struct(size_t value_size, uint8_t value_type,
                                   RcDropFn drop_fn, RcWalkFn walk_fn) {
    RcControlBlock *cb = rc_cb_alloc_kinded(value_size, value_type, drop_fn,
                                            RCK_STRUCT, 0);
    cb->walk_fn = walk_fn;
    return cb;
}

/* DEDUP-4b: `(rc/from-ref r)` -- adopt an existing ref<T> payload into a fresh
 * control block, taking ownership of it.  Unlike rc_cb_alloc* the value is NOT
 * the inline (cb + 1) region: it is the caller's already-allocated payload, so
 * the block is header-only and the drop glue is derived from the value type.
 *
 * Ported from the hand-written copy in emit_module.c, which is the only place
 * it existed.  One difference, deliberate: that copy leaves gc_index,
 * gc_buffered, gc_trial and gc_collecting to gc_register_block, which zeroes
 * all four in the EMITTED collector but only the first two here (this runtime
 * zeroes gc_trial/gc_collecting in rc_cb_alloc_kinded instead, which this path
 * bypasses).  Initialising them explicitly is what keeps the port from handing
 * the collector a block with a garbage trial count. */
RcControlBlock *tur_rc_from_ref(void *ref_value, uint8_t value_type) {
    if (!ref_value) return NULL;

    RcControlBlock *cb = (RcControlBlock *)malloc(sizeof(RcControlBlock));
    if (!cb) {
        fprintf(stderr, "rc/from-ref: out of memory\n");
        abort();
    }

    cb->strong_count = 1;
    cb->weak_count = 0;
    cb->value = ref_value;
    cb->drop_fn = default_drop_fn_for_type(value_type);
    cb->walk_fn = NULL;
    cb->value_type_kind = value_type;

    cb->color = GC_WHITE;
    cb->may_contain_cycles = true;
    cb->gc_index = RC_GC_INDEX_NONE;
    cb->gc_buffered = false;
    cb->gc_trial = 0;
    cb->gc_collecting = false;
    memset(cb->reserved, 0, sizeof(cb->reserved));

    gc_register_block(cb);
    return cb;
}

/* DEDUP-4b: `(ref/from-rc r)` -- the inverse.  Extracts the payload and
 * destroys the control block, which is only sound when the rc is UNIQUE: any
 * other strong owner would be left holding a freed block, and any weak
 * observer would lose the liveness flag it polls.  Both are checked. */
void *tur_ref_from_rc(RcControlBlock *cb) {
    if (!cb) return NULL;
    if (cb->strong_count != 1 || cb->weak_count != 0) {
        fprintf(stderr,
                "ref/from-rc requires unique rc (strong_count==1 and "
                "weak_count==0), got strong=%llu weak=%llu\n",
                (unsigned long long)cb->strong_count,
                (unsigned long long)cb->weak_count);
        abort();
    }
    void *value = cb->value;
    cb->value = NULL;
    gc_unregister_block(cb);
    free(cb);
    return value;
}

/* Free a control block and its value.
 * Called when both strong_count and weak_count reach 0.
 */
/* Run the block's value teardown exactly once and mark it done.
 *
 * Split out of rc_cb_free (which used to be the only caller) so the zombie
 * transition in rc_strong_decrement can reach it too: a block whose strong
 * count hits 0 while a weak<T> still observes it must release its VALUE
 * immediately and keep only the control block alive.  Every caller nulls
 * `value` afterwards, so a later rc_cb_free / gc sweep skips it rather than
 * double-dropping -- the same discipline gc.c already uses (gc.c:476, 655). */
static void rc_release_value(RcControlBlock *cb) {
    if (!cb || !cb->value) return;

    /* F1-2-4: smart drop for RCK_EXISTENTIAL blocks whose payload is
     * itself an rc reference (RCEXP_RC).  The packed value's first 8
     * bytes hold the inner RcControlBlock pointer (laid out by
     * emit_expr.c's EX_EXISTS_PACK path; mirrored by the cycle walker
     * in gc.c).  Decrement that inner reference *before* the
     * existential's own drop hook fires so the inner allocation does
     * not leak when the outer existential is reclaimed.
     *
     * F1-2-5: the drop dispatch lives here rather than in
     * tur_existential_drop because this is the single value-teardown
     * entry point that is guaranteed to run once per block; the
     * per-program drop hook in emit_module.c stays a no-op (preserving
     * the original layout-free interface). */
    if (cb->reserved[0] == RCK_EXISTENTIAL && cb->reserved[1] == RCEXP_RC) {
        int64_t raw = *(const int64_t *)cb->value;
        RcControlBlock *inner = (RcControlBlock *)(intptr_t)raw;
        if (inner) {
            (void)rc_strong_decrement(inner);
        }
    }

    if (cb->drop_fn) cb->drop_fn(cb->value);
    cb->value = NULL;
}

void rc_cb_free(RcControlBlock *cb) {
    if (!cb) return;

    /* Unregister from GC tracking */
    gc_unregister_block(cb);

    /* Call the drop function on the value (no-op if already released by the
     * zombie transition in rc_strong_decrement, or by a gc sweep). */
    rc_release_value(cb);

    /* Free the entire block (header + value) */
    free(cb);
}

/* ---------------------------------------------------------------------------
 * DEDUP-1: RcControlBlock layout guard.
 *
 * This struct exists TWICE: here, and hand-written into every compiled program
 * by the compiler (emit_module.c). The two copies must stay layout-compatible,
 * because linking one against the other -- the end goal of de-duplicating them
 * -- silently mis-reads every field past the first divergence otherwise.
 *
 * They HAD diverged: `value_type_kind` and `color` were enums here (4 bytes)
 * and `uint8_t` in the emitted copy, shifting every GC field after them. Both
 * are fixed-width now, and these assertions pin the widths and the field order
 * so any future drift is a COMPILE error in whichever copy changed, instead of
 * a runtime mis-read. Three bugs this session (CG1, CG3, CG4) came from these
 * copies drifting apart; each was invisible to half the test suite.
 *
 * Deliberately no assertion on the total sizeof or on absolute offsets: those
 * are padding-dependent and would break on a different ABI without indicating
 * a real divergence. Field widths and relative order are what must match.
 *
 * Written with the typedef trick rather than _Static_assert because the emitted
 * programs are compiled as C99, and both copies carry the identical text.
 * --------------------------------------------------------------------------- */
#define RC_LAYOUT_ASSERT(name, cond) typedef char rc_layout_##name[(cond) ? 1 : -1]

RC_LAYOUT_ASSERT(strong_w,  sizeof(((RcControlBlock *)0)->strong_count)      == 8);
RC_LAYOUT_ASSERT(weak_w,    sizeof(((RcControlBlock *)0)->weak_count)        == 8);
RC_LAYOUT_ASSERT(vtk_w,     sizeof(((RcControlBlock *)0)->value_type_kind)   == 1);
RC_LAYOUT_ASSERT(color_w,   sizeof(((RcControlBlock *)0)->color)             == 1);
RC_LAYOUT_ASSERT(mcc_w,     sizeof(((RcControlBlock *)0)->may_contain_cycles) == 1);
RC_LAYOUT_ASSERT(index_w,   sizeof(((RcControlBlock *)0)->gc_index)          == 4);
RC_LAYOUT_ASSERT(buffered_w,sizeof(((RcControlBlock *)0)->gc_buffered)       == 1);
RC_LAYOUT_ASSERT(trial_w,   sizeof(((RcControlBlock *)0)->gc_trial)          == 8);
RC_LAYOUT_ASSERT(collect_w, sizeof(((RcControlBlock *)0)->gc_collecting)     == 1);
RC_LAYOUT_ASSERT(ord_1, offsetof(RcControlBlock, strong_count)  < offsetof(RcControlBlock, weak_count));
RC_LAYOUT_ASSERT(ord_2, offsetof(RcControlBlock, weak_count)    < offsetof(RcControlBlock, value));
RC_LAYOUT_ASSERT(ord_3, offsetof(RcControlBlock, value)         < offsetof(RcControlBlock, drop_fn));
RC_LAYOUT_ASSERT(ord_4, offsetof(RcControlBlock, drop_fn)       < offsetof(RcControlBlock, walk_fn));
RC_LAYOUT_ASSERT(ord_5, offsetof(RcControlBlock, walk_fn)       < offsetof(RcControlBlock, value_type_kind));
RC_LAYOUT_ASSERT(ord_6, offsetof(RcControlBlock, value_type_kind) < offsetof(RcControlBlock, color));
RC_LAYOUT_ASSERT(ord_7, offsetof(RcControlBlock, color)         < offsetof(RcControlBlock, may_contain_cycles));
RC_LAYOUT_ASSERT(ord_8, offsetof(RcControlBlock, may_contain_cycles) < offsetof(RcControlBlock, gc_index));
RC_LAYOUT_ASSERT(ord_9, offsetof(RcControlBlock, gc_index)      < offsetof(RcControlBlock, gc_buffered));
RC_LAYOUT_ASSERT(ord_10, offsetof(RcControlBlock, gc_buffered)  < offsetof(RcControlBlock, gc_trial));
RC_LAYOUT_ASSERT(ord_11, offsetof(RcControlBlock, gc_trial)     < offsetof(RcControlBlock, gc_collecting));
RC_LAYOUT_ASSERT(ord_12, offsetof(RcControlBlock, gc_collecting) < offsetof(RcControlBlock, reserved));

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
            /* Zombie state: the VALUE dies now; only the control block lives
             * on, so a weak observer can be told "gone" instead of dangling.
             *
             * stdlib-weak-ref-audit WR1: this used to skip the value teardown
             * entirely and defer it to rc_weak_decrement, which deadlocks the
             * one shape weak<T> exists for.  In Rust's parent/child break the
             * surviving weak lives INSIDE the parent's own value (the child
             * holds weak<Parent>, and the parent strongly owns the child), so
             * "wait for the last weak before dropping the value" waits on a
             * weak that only the value's own drop glue can release.  Neither
             * ever runs: parent's value is never dropped, so its rc<Child> is
             * never released, so the child's weak<Parent> is never dropped.
             * Measured at 217 bytes leaked per parent/child pair, with the
             * collector off and nothing for it to collect.  Dropping the value
             * here -- exactly what Rust's Rc does at strong 0 -- is what makes
             * the pattern reclaim promptly and completely. */
            rc_release_value(cb);
            /* Phase 10: Notify GC for cycle collection */
            gc_on_strong_decrement(cb);
            return false;  /* Control block not yet freed */
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
