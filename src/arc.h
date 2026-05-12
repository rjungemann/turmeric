/* arc.h - Atomic reference counting for Turmeric (Phase T19)
 *
 * Arc<T> is the thread-safe counterpart to rc<T>.  Where rc<T> uses plain
 * uint64_t counts (single-threaded), Arc<T> uses _Atomic uint64_t counts so
 * that multiple OS threads can safely clone/drop shared values.
 *
 * Design decisions (from deferred-tasks-phase15-phase19.md § T19):
 *   - New file src/arc.{c,h} — not reusing rc.c because the atomic operations
 *     and the drop semantics differ.
 *   - Control block layout mirrors RcControlBlock with _Atomic fields.
 *   - arc_strong_decrement returns true when the value should be dropped, so
 *     the caller is responsible for calling the drop_fn and freeing memory;
 *     this avoids recursive drops inside the locked decrement.
 *   - No Bacon-Rajan cycle collector for Arc<T> in v1 — cycles must be broken
 *     manually via arc_weak (ArcWeakControlBlock).
 *   - Drop function type is the same RcDropFn to allow sharing between rc/arc.
 */

#ifndef TUR_ARC_H
#define TUR_ARC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include "types.h"

/* Drop function type — identical to RcDropFn for interoperability */
typedef void (*RcDropFn)(void *value);

/* Atomic control block for Arc<T>.
 *
 * strong_count and weak_count are plain uint64_t accessed exclusively through
 * the __atomic_* GCC/Clang builtins (memory_order_* constants from <stdatomic.h>
 * are not used; we pass the raw __ATOMIC_* macros directly).  This avoids the
 * C11 _Atomic keyword while remaining correct under -std=c99 -Werror.
 *
 * strong_count  — number of Arc<T> clones pointing to this block.
 * weak_count    — number of ArcWeak<T> references (+ 1 sentinel while strong>0).
 *                 The sentinel prevents the block from being freed while strong
 *                 references still exist.
 */
typedef struct ArcControlBlock {
    uint64_t strong_count; /* accessed only via __atomic_* builtins */
    uint64_t weak_count;   /* accessed only via __atomic_* builtins */
    void            *value;
    RcDropFn         drop_fn;
    TypeKind         value_type_kind;
} ArcControlBlock;

/* ── Strong (Arc) operations ─────────────────────────────────────────────── */

/* Allocate a new ArcControlBlock wrapping `value_size` bytes.
 * Initialises strong_count = 1, weak_count = 1 (sentinel).
 * Returns NULL on allocation failure. */
ArcControlBlock *arc_cb_alloc(size_t value_size, TypeKind kind, RcDropFn drop_fn);

/* Increment strong_count.  Call when cloning an Arc<T>.
 * Returns the same block for convenience. */
ArcControlBlock *arc_strong_increment(ArcControlBlock *cb);

/* Decrement strong_count.  Returns true if the count reached 0 — the caller
 * must then call arc_drop_value(cb) and arc_weak_decrement(cb) (for sentinel).
 * Never frees memory itself. */
bool arc_strong_decrement(ArcControlBlock *cb);

/* Drop the wrapped value: call drop_fn (or free) then set cb->value = NULL.
 * Must only be called once, when strong_count has reached 0. */
void arc_drop_value(ArcControlBlock *cb);

/* ── Weak operations ─────────────────────────────────────────────────────── */

/* Increment weak_count.  Returns the same block. */
ArcControlBlock *arc_weak_increment(ArcControlBlock *cb);

/* Decrement weak_count.  Returns true if both counts have reached 0 —
 * the caller must then free the ArcControlBlock itself. */
bool arc_weak_decrement(ArcControlBlock *cb);

/* Try to upgrade a weak reference to a strong one.
 * Returns true and increments strong_count if the value is still alive.
 * Returns false if strong_count is already 0 (value has been dropped). */
bool arc_weak_upgrade(ArcControlBlock *cb);

/* ── Convenience ─────────────────────────────────────────────────────────── */

/* Full clone: increment strong_count, return cb. */
static inline ArcControlBlock *arc_clone(ArcControlBlock *cb) {
    return arc_strong_increment(cb);
}

/* Full drop: decrement strong_count; if it reaches 0, drop value + sentinel. */
static inline void arc_drop(ArcControlBlock *cb) {
    if (!cb) return;
    if (arc_strong_decrement(cb)) {
        arc_drop_value(cb);
        if (arc_weak_decrement(cb)) {
            /* Both counts zero: free the control block itself */
            free(cb);
        }
    }
}

#endif /* TUR_ARC_H */
