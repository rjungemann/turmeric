/* arc.c - Atomic reference counting implementation for Turmeric (Phase T19) */

#include "arc.h"

#include <stdlib.h>
#include <string.h>

/* ── Allocation ─────────────────────────────────────────────────────────── */

ArcControlBlock *arc_cb_alloc(size_t value_size, TypeKind kind, RcDropFn drop_fn) {
    ArcControlBlock *cb = (ArcControlBlock *)malloc(sizeof(ArcControlBlock));
    if (!cb) return NULL;

    void *value = malloc(value_size);
    if (!value) { free(cb); return NULL; }
    memset(value, 0, value_size);

    __atomic_store_n(&cb->strong_count, (uint64_t)1, __ATOMIC_RELAXED);
    __atomic_store_n(&cb->weak_count,   (uint64_t)1, __ATOMIC_RELAXED); /* sentinel */
    cb->value           = value;
    cb->drop_fn         = drop_fn;
    cb->value_type_kind = kind;
    return cb;
}

/* ── Strong (Arc) operations ────────────────────────────────────────────── */

ArcControlBlock *arc_strong_increment(ArcControlBlock *cb) {
    __atomic_fetch_add(&cb->strong_count, (uint64_t)1, __ATOMIC_RELAXED);
    return cb;
}

bool arc_strong_decrement(ArcControlBlock *cb) {
    uint64_t prev = __atomic_fetch_sub(&cb->strong_count, (uint64_t)1,
                                       __ATOMIC_ACQ_REL);
    return prev == 1;
}

void arc_drop_value(ArcControlBlock *cb) {
    if (!cb->value) return;
    if (cb->drop_fn) {
        cb->drop_fn(cb->value);
    } else {
        free(cb->value);
    }
    cb->value = NULL;
}

/* ── Weak operations ────────────────────────────────────────────────────── */

ArcControlBlock *arc_weak_increment(ArcControlBlock *cb) {
    __atomic_fetch_add(&cb->weak_count, (uint64_t)1, __ATOMIC_RELAXED);
    return cb;
}

bool arc_weak_decrement(ArcControlBlock *cb) {
    uint64_t prev = __atomic_fetch_sub(&cb->weak_count, (uint64_t)1,
                                       __ATOMIC_ACQ_REL);
    return prev == 1;
}

bool arc_weak_upgrade(ArcControlBlock *cb) {
    uint64_t cur = __atomic_load_n(&cb->strong_count, __ATOMIC_ACQUIRE);
    while (cur != 0) {
        if (__atomic_compare_exchange_n(&cb->strong_count, &cur, cur + 1,
                                        /*weak=*/1,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            return true;
        }
    }
    return false;
}
