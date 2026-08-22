/* trail.c -- SX1: backtrackable state as a runtime primitive.  See trail.h. */

#include "trail.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- *
 * State
 *
 * Per-thread, and deliberately not shareable: the trail is a stack discipline
 * over one thread's search, and a second thread undoing another's writes is not
 * a feature anyone asked for.  `__thread` matches how the rest of the runtime
 * carries per-thread state (tur_tls.c); front ends without TLS route through
 * there, and this file follows if that ever becomes necessary.
 * ------------------------------------------------------------------------- */

typedef struct TrailEntry {
    TurBtCell *cell;      /* the cell to restore                              */
    intptr_t   old;       /* payload to restore into it                       */
    uint32_t   old_stamp; /* the stamp it carried before this level touched it */
    bool       was_bound; /* write-once: whether it was bound before          */
} TrailEntry;

typedef struct Level {
    uint32_t base;        /* trail depth when this level was pushed */
    uint32_t generation;  /* decision 4: what makes a stale mark detectable */
} Level;

static __thread TrailEntry *g_trail;
static __thread uint32_t    g_trail_n, g_trail_cap;
static __thread Level      *g_levels;
static __thread uint32_t    g_levels_n, g_levels_cap;
static __thread uint32_t    g_pause_depth;

/* Monotone per thread, never reused.  A generation is only ever compared for
 * equality, so wrapping after 2^32 marks would be the first time this could
 * mistake a stale mark for a live one -- at which point the process has taken
 * four billion marks and has other problems. */
static __thread uint32_t    g_generation = 1;

uint32_t tur_trail_level(void) { return g_levels_n; }
uint32_t tur_trail_depth(void) { return g_trail_n; }

void tur_trail_pause(void)  { g_pause_depth++; }
void tur_trail_resume(void) { if (g_pause_depth) g_pause_depth--; }
bool tur_trail_paused(void) { return g_pause_depth != 0; }

/* ------------------------------------------------------------------------- *
 * Growth
 * ------------------------------------------------------------------------- */

static bool trail_grow(void) {
    uint32_t ncap = g_trail_cap ? g_trail_cap * 2 : 64;
    TrailEntry *n = (TrailEntry *)realloc(g_trail, (size_t)ncap * sizeof(TrailEntry));
    if (!n) return false;
    g_trail = n; g_trail_cap = ncap;
    return true;
}

static bool levels_grow(void) {
    uint32_t ncap = g_levels_cap ? g_levels_cap * 2 : 16;
    Level *n = (Level *)realloc(g_levels, (size_t)ncap * sizeof(Level));
    if (!n) return false;
    g_levels = n; g_levels_cap = ncap;
    return true;
}

/* ------------------------------------------------------------------------- *
 * Levels
 * ------------------------------------------------------------------------- */

TurTrailMark tur_trail_mark(void) {
    TurTrailMark m;
    if (g_levels_n == g_levels_cap && !levels_grow()) {
        /* Out of memory taking a mark.  Hand back a mark that can never match a
         * live level, so the undo is refused rather than silently unwinding to
         * whatever level happens to sit at that index. */
        m.level = 0; m.generation = 0;
        return m;
    }
    g_levels[g_levels_n].base       = g_trail_n;
    g_levels[g_levels_n].generation = ++g_generation;
    g_levels_n++;
    m.level = g_levels_n;                    /* 1-based: 0 means "no level" */
    m.generation = g_levels[g_levels_n - 1].generation;
    return m;
}

/* A mark is live when its level is still on the stack AND the generation
 * matches -- the level index alone is not enough, because the same index is
 * reused by the next mark taken after an undo.  That reuse is exactly the case
 * a re-entered continuation walks into. */
static bool mark_is_live(TurTrailMark m) {
    return m.level != 0 && m.level <= g_levels_n &&
           g_levels[m.level - 1].generation == m.generation;
}

bool tur_trail_undo_to(TurTrailMark m) {
    if (!mark_is_live(m)) return false;
    uint32_t base = g_levels[m.level - 1].base;
    while (g_trail_n > base) {
        TrailEntry *e = &g_trail[--g_trail_n];
        TurBtCell *c = e->cell;
        /* Ownership (decision 3) is a TRANSFER, not a copy.  Recording moved
         * the cell's reference into the entry; undoing moves it back.  So undo
         * releases the value being discarded and installs the saved one with no
         * clone at all -- cloning here would abandon the entry's reference and
         * leak exactly one payload per undone write.
         *
         * The `!= e->old` guard covers a cell set back to what it already held:
         * there is one reference and it is going to the cell, so dropping it
         * first would free the object this line is about to install. */
        if (c->drop && c->payload && c->payload != e->old) c->drop(c->payload);
        c->payload = e->old;
        c->stamp   = e->old_stamp;
        c->bound   = e->was_bound;
    }
    g_levels_n = m.level - 1;
    return true;
}

bool tur_trail_commit_to(TurTrailMark m) {
    if (!mark_is_live(m)) return false;
    uint32_t base = g_levels[m.level - 1].base;
    while (g_trail_n > base) {
        TrailEntry *e = &g_trail[--g_trail_n];
        /* The write stays, so the SAVED value is the one nobody will ever hold
         * again -- release it rather than the payload.  Getting this backwards
         * would free the live value and leak the dead one. */
        if (e->cell->drop && e->old) e->cell->drop(e->old);
    }
    g_levels_n = m.level - 1;
    /* The cells keep their current payloads, but their stamps now name a level
     * that no longer exists.  Left as-is deliberately: a stamp is only ever
     * compared against the CURRENT level, and any stamp from a popped level is
     * necessarily >= it, so the next write at an outer level trails correctly.
     * See the comment in tur_bt_set. */
    return true;
}

void tur_trail_reset(void) {
    free(g_trail);  g_trail = NULL;  g_trail_n = g_trail_cap = 0;
    free(g_levels); g_levels = NULL; g_levels_n = g_levels_cap = 0;
    g_pause_depth = 0;
}

/* ------------------------------------------------------------------------- *
 * Cells
 * ------------------------------------------------------------------------- */

void tur_bt_cell_init(TurBtCell *c, intptr_t init,
                      TurTrailClone clone, TurTrailDrop drop) {
    c->payload    = init;
    /* Born stamped at the current level: a cell allocated inside the level that
     * is about to write it has nothing worth restoring, because undoing past
     * its birth destroys the cell too.  This is the WAM's address comparison,
     * expressed as a stamp. */
    c->stamp      = tur_trail_level();
    c->bound      = true;
    c->write_once = false;
    c->clone      = clone;
    c->drop       = drop;
}

void tur_bt_lvar_init(TurBtCell *c, intptr_t unbound,
                      TurTrailClone clone, TurTrailDrop drop) {
    tur_bt_cell_init(c, unbound, clone, drop);
    c->bound      = false;
    c->write_once = true;
}

static bool record(TurBtCell *c) {
    if (g_trail_n == g_trail_cap && !trail_grow()) return false;
    TrailEntry *e = &g_trail[g_trail_n++];
    e->cell      = c;
    e->old       = c->payload;
    e->old_stamp = c->stamp;
    e->was_bound = c->bound;
    /* No clone: the cell's reference MOVES to the entry.  The caller's write is
     * about to overwrite `c->payload`, and because this entry now owns it, that
     * write must not drop it -- see the branch in tur_bt_set. */
    return true;
}

bool tur_bt_set(TurBtCell *c, intptr_t v) {
    if (c->write_once && c->bound) return false;

    uint32_t level = tur_trail_level();
    /* Trail on the first write per level, and only then (decision 2).
     *
     * `<` rather than `!=` is load-bearing after a commit: a committed level
     * leaves cells stamped above the current level, and `!=` would trail them
     * again on the next outer write -- recording an entry whose undo would
     * revert a write the caller explicitly asked to keep. */
    if (level > 0 && !g_pause_depth && c->stamp < level) {
        if (!record(c)) return false;
        c->stamp = level;
        /* Recorded: the entry owns the old payload now, so this write must NOT
         * release it.  Undo will hand it back; commit will release it then. */
    } else if (c->drop && c->payload && c->payload != v) {
        /* Not recorded -- either there is no level to come back to, trailing is
         * paused, or this cell was already trailed at this level.  Nothing will
         * ever restore the old payload, so it is released here or never. */
        c->drop(c->payload);
    }
    c->payload = v;
    c->bound   = true;
    return true;
}

/* A reference the caller may keep past the next write.  `tur_bt_get` hands back
 * a BORROWED word -- valid until the cell is written or undone -- which is what
 * a union-find `find` wants and what most reads want.  A caller that needs the
 * value to outlive the cell's next write needs its own reference, and only the
 * cell knows how to make one.  This is the clone hook's caller. */
intptr_t tur_bt_get_owned(const TurBtCell *c) {
    if (c->clone && c->payload) return c->clone(c->payload);
    return c->payload;
}

/* ------------------------------------------------------------------------- *
 * Scalar ABI shims (see trail.h)
 * ------------------------------------------------------------------------- */

/* A packed mark is never arithmetic -- it is compared and unpacked, nothing
 * else -- so the layout only has to be reversible, not ordered. */
static int64_t mark_pack(TurTrailMark m) {
    return ((int64_t)m.level << 32) | (int64_t)m.generation;
}
static TurTrailMark mark_unpack(int64_t p) {
    TurTrailMark m;
    m.level      = (uint32_t)((uint64_t)p >> 32);
    m.generation = (uint32_t)((uint64_t)p & 0xFFFFFFFFu);
    return m;
}

int64_t tur_trail_mark_packed(void)             { return mark_pack(tur_trail_mark()); }
bool    tur_trail_undo_to_packed(int64_t p)     { return tur_trail_undo_to(mark_unpack(p)); }
bool    tur_trail_commit_to_packed(int64_t p)   { return tur_trail_commit_to(mark_unpack(p)); }

void *tur_bt_cell_new(int64_t init) {
    TurBtCell *c = (TurBtCell *)malloc(sizeof(TurBtCell));
    if (!c) return NULL;
    tur_bt_cell_init(c, (intptr_t)init, NULL, NULL);
    return c;
}

void *tur_bt_lvar_new(int64_t unbound) {
    TurBtCell *c = (TurBtCell *)malloc(sizeof(TurBtCell));
    if (!c) return NULL;
    tur_bt_lvar_init(c, (intptr_t)unbound, NULL, NULL);
    return c;
}

/* Freeing a cell that still has trail entries pointing at it would leave those
 * entries dangling until the next undo.  The caller owns that ordering -- free
 * cells outside the scope that wrote them -- and this is the one place worth
 * saying so, because the failure is a use-after-free inside undo rather than
 * at the free itself. */
void     tur_bt_cell_free(void *c)              { free(c); }
int64_t  tur_bt_cell_get(void *c)               { return (int64_t)tur_bt_get((TurBtCell *)c); }
bool     tur_bt_cell_bound(void *c)             { return tur_bt_bound((TurBtCell *)c); }
bool     tur_bt_cell_set(void *c, int64_t v)    { return tur_bt_set((TurBtCell *)c, (intptr_t)v); }
