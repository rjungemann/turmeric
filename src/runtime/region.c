/* region.c -- declared lifetimes (RM3).  See region.h for the design and
 * docs/upcoming/regions-plan.md for the phase.
 *
 * One Arena per generation rather than watermarks into a shared one.  Arena
 * exposes `arena_reset` (whole-arena rewind) and `arena_owns`, not a
 * save/restore watermark, so a per-generation Arena is what the existing API
 * supports without changing it -- and it makes `tur_region_owns` a walk over
 * generations instead of an address-range comparison that would have to know
 * about interleaving.
 *
 * A generation popped WITHOUT reclaim is retired, not freed: its memory stays
 * mapped for the process.  That is deliberately the status quo (the spine
 * leaks today), so the conservative path costs nothing new, and it keeps
 * `tur_region_owns` true for those pointers -- a value that outlived its
 * generation must still never reach `free()`. */
#include "region.h"

#include <stdlib.h>
#include <string.h>

#include "arena.h"

#define TUR_REGION_SLAB (64u * 1024u)

/* Live generations, innermost last.  Depth is 1-based so 0 can mean "none". */
static Arena  **g_live;
static int      g_live_n;
static int      g_live_cap;

/* Generations popped without reclaim.  Never freed before shutdown -- see the
 * file comment for why they are kept rather than released. */
static Arena  **g_retired;
static int      g_retired_n;
static int      g_retired_cap;

static bool push_ptr(Arena ***vec, int *n, int *cap, Arena *a) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 8;
        Arena **nv = (Arena **)realloc(*vec, (size_t)nc * sizeof(Arena *));
        if (!nv) return false;
        *vec = nv;
        *cap = nc;
    }
    (*vec)[(*n)++] = a;
    return true;
}

int tur_region_push(void) {
    Arena *a = (Arena *)calloc(1, sizeof(Arena));
    if (!a) return 0;
    arena_init(a, TUR_REGION_SLAB);
    if (!push_ptr(&g_live, &g_live_n, &g_live_cap, a)) {
        arena_free(a);
        free(a);
        return 0;
    }
    return g_live_n;   /* 1-based depth */
}

/* Both pops take the depth back so a mismatched pair is a no-op rather than a
 * rewind of somebody else's generation.  A region form that unwinds through a
 * panic can leave the stack deeper than the caller thinks; refusing is the
 * conservative answer, and it keeps the failure "no saving" rather than
 * "reclaimed memory still in use". */
static Arena *detach(int depth) {
    if (depth <= 0 || depth != g_live_n) return NULL;
    return g_live[--g_live_n];
}

void tur_region_pop(int depth) {
    Arena *a = detach(depth);
    if (!a) return;
    if (!push_ptr(&g_retired, &g_retired_n, &g_retired_cap, a)) {
        /* Out of memory retiring it: the arena stays mapped and unreferenced,
         * which is the same outcome the retired list produces.  Never free it
         * -- something may still point in. */
        return;
    }
}

void tur_region_pop_reclaim(int depth) {
    Arena *a = detach(depth);
    if (!a) return;
    /* arena_free releases the slabs.  In a Debug build arena_reset poisons
     * first; here the memory is unmapped outright, so a survivor faults rather
     * than reading stale bytes -- the louder of the two backstops. */
    arena_free(a);
    free(a);
}

void *tur_region_alloc(size_t n) {
    if (g_live_n <= 0) return NULL;   /* no region open -- caller uses malloc */
    return arena_alloc(g_live[g_live_n - 1], n);
}

void *tur_region_alloc_or_malloc(size_t n) {
    void *p = tur_region_alloc(n);
    return p ? p : malloc(n);
}

bool tur_region_owns(const void *p) {
    if (!p) return false;
    for (int i = 0; i < g_live_n; i++)
        if (arena_owns(g_live[i], p)) return true;
    for (int i = 0; i < g_retired_n; i++)
        if (arena_owns(g_retired[i], p)) return true;
    return false;
}

bool tur_region_active(void) { return g_live_n > 0; }

void tur_region_shutdown(void) {
    for (int i = 0; i < g_live_n; i++)    { arena_free(g_live[i]);    free(g_live[i]); }
    for (int i = 0; i < g_retired_n; i++) { arena_free(g_retired[i]); free(g_retired[i]); }
    free(g_live);    g_live = NULL;    g_live_n = g_live_cap = 0;
    free(g_retired); g_retired = NULL; g_retired_n = g_retired_cap = 0;
}
