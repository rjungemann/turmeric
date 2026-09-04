/* trail_c.h -- a value trail over uint32 array slots, for compiler-side
 * incremental theory state (SX3; the C sibling of src/runtime/trail.c).
 *
 * Discipline is mark / mutate-through-the-trail / undo-to-mark, same as the
 * runtime trail, with the same stamp suppression: a slot written twice
 * between one mark and the next is trailed once.  Two lessons from the
 * runtime trail are baked in rather than relearned:
 *
 *   - Entries record slot INDICES, not addresses.  The arrays being trailed
 *     are arena-grown (realloc-and-copy), so a trailed pointer can dangle
 *     into the superseded allocation by the time undo runs.  The caller
 *     resolves index -> current array at undo time.
 *
 *   - The level counter is MONOTONIC.  Undo does not roll `level` back;
 *     the next mark takes a fresh level.  Rolling back would leave
 *     stale-high stamps equal to a reused level, silently suppressing
 *     trailing -- the exact bug the runtime trail's commit path shipped and
 *     had to fix (test_commit_then_reused_level).  Monotonic levels make
 *     that state unrepresentable at the cost of one uint32 that only wraps
 *     after 2^32 marks per solver state.
 *
 * Allocation: entries live in the caller's arena, grown by doubling and
 * REUSED across undo (undo just truncates the count) -- deleting the
 * per-cube rebuild churn is half of SX3's point.
 */
#ifndef TUR_TRAIL_C_H
#define TUR_TRAIL_C_H

#include <stdint.h>
#include <string.h>
#include "../runtime/arena.h"

typedef struct {
    uint32_t idx;   /* which slot in the caller's array */
    uint32_t old;   /* value to restore */
} TrailCEntry;

typedef struct {
    TrailCEntry *e;
    uint32_t     n, cap;
    uint32_t     level;   /* monotonic; bumped by trailc_mark */
    Arena       *a;
} TrailC;

typedef struct {
    uint32_t len;    /* trail length at the mark */
    uint32_t level;  /* level in force after the mark (for callers that log) */
} TrailCMark;

static inline void trailc_init(TrailC *t, Arena *a) {
    memset(t, 0, sizeof *t);
    t->a = a;
    t->level = 1;   /* 0 is the "never stamped" resting value in stamp arrays */
}

static inline TrailCMark trailc_mark(TrailC *t) {
    t->level++;
    TrailCMark m = { t->n, t->level };
    return m;
}

/* Record `old` for slot `idx`, once per level: `stamp` is the caller's
 * per-slot stamp word (parallel to the trailed array).  Returns true when an
 * entry was appended -- callers only care in tests. */
static inline bool trailc_note(TrailC *t, uint32_t idx, uint32_t old,
                               uint32_t *stamp) {
    if (*stamp == t->level) return false;
    *stamp = t->level;
    if (t->n == t->cap) {
        uint32_t ncap = t->cap ? t->cap * 2 : 64;
        TrailCEntry *ne = (TrailCEntry *)arena_alloc(t->a, ncap * sizeof *ne);
        if (t->n) memcpy(ne, t->e, t->n * sizeof *ne);
        t->e = ne; t->cap = ncap;
    }
    t->e[t->n].idx = idx;
    t->e[t->n].old = old;
    t->n++;
    return true;
}

/* Replay entries above the mark in reverse into `slots`, then truncate.
 * `n_slots` guards against entries for slots the caller has since truncated
 * away -- restoring those would write past the caller's logical size (the
 * physical array is still allocated, but skipping is cheaper than relying
 * on re-initialization). */
static inline void trailc_undo_to(TrailC *t, TrailCMark m, uint32_t *slots,
                                  uint32_t n_slots) {
    for (uint32_t i = t->n; i > m.len; i--) {
        const TrailCEntry *e = &t->e[i - 1];
        if (e->idx < n_slots) slots[e->idx] = e->old;
    }
    t->n = m.len;
}

#endif /* TUR_TRAIL_C_H */
