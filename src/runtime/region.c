/* region.c -- declared lifetimes (RM3).  See region.h for the design and
 * docs/archive/regions-plan.md for the phase.
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

/* Reclaimed generations, reset and available for the next push.  Reclaiming
 * REWINDS rather than releases: arena_reset keeps the slabs (and poisons them
 * in a Debug build), so a per-query region inside a loop pays for its slabs
 * once instead of per iteration.  A pooled arena is dead memory -- deliberately
 * NOT reported by tur_region_owns, which answers about live and retired
 * generations only. */
static Arena  **g_pool;
static int      g_pool_n;
static int      g_pool_cap;

/* R3: set on the innermost generation when a noted escape points into it.
 * Parallel to g_live, one flag per open generation.  Sticky: once a generation
 * has leaked a pointer it can never be reclaimed, however many safe escapes
 * follow. */
static bool    *g_escaped;
static int      g_escaped_cap;

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

TUR_RT_API int tur_region_push(void) {
    Arena *a;
    if (g_pool_n > 0) {
        a = g_pool[--g_pool_n];    /* already reset by the reclaim that pooled it */
    } else {
        a = (Arena *)calloc(1, sizeof(Arena));
        if (!a) return 0;
        arena_init(a, TUR_REGION_SLAB);
    }
    if (!push_ptr(&g_live, &g_live_n, &g_live_cap, a)) {
        arena_free(a);
        free(a);
        return 0;
    }
    /* A fresh generation has escaped nothing.  Grow the flag vector alongside
     * the live stack; a failure to grow it is treated as "escaped", so the
     * generation is never reclaimed on the strength of a flag we could not
     * store -- the conservative direction. */
    if (g_live_n > g_escaped_cap) {
        int nc = g_escaped_cap ? g_escaped_cap * 2 : 8;
        if (nc < g_live_n) nc = g_live_n;
        bool *nf = (bool *)realloc(g_escaped, (size_t)nc * sizeof(bool));
        if (!nf) return g_live_n;   /* flag missing -> read as escaped below */
        g_escaped = nf;
        g_escaped_cap = nc;
    }
    g_escaped[g_live_n - 1] = false;
    return g_live_n;   /* 1-based depth */
}

TUR_RT_API void tur_region_note_escape(const void *p) {
    if (g_live_n <= 0 || !p) return;
    /* Only an escape that IS region memory matters.  A malloc'd or static
     * pointer crossing the boundary is ordinary and blocks nothing -- which is
     * what makes this check worth having rather than a blanket refusal. */
    if (arena_owns(g_live[g_live_n - 1], p)) {
        if (g_live_n <= g_escaped_cap) g_escaped[g_live_n - 1] = true;
    }
}

/* Read the flag conservatively: a generation whose flag we could not store is
 * treated as having escaped. */
static bool generation_escaped(int idx0) {
    if (idx0 < 0) return true;
    if (idx0 >= g_escaped_cap) return true;
    return g_escaped[idx0];
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

TUR_RT_API void tur_region_pop(int depth) {
    Arena *a = detach(depth);
    if (!a) return;
    if (!push_ptr(&g_retired, &g_retired_n, &g_retired_cap, a)) {
        /* Out of memory retiring it: the arena stays mapped and unreferenced,
         * which is the same outcome the retired list produces.  Never free it
         * -- something may still point in. */
        return;
    }
}

TUR_RT_API void tur_region_pop_reclaim(int depth) {
    Arena *a = detach(depth);
    if (!a) return;
    /* Rewind, do not release.  arena_reset keeps the slabs and, in a Debug
     * build, poisons them -- so a straggler traps at the deref under ASan
     * instead of reading stale bytes, and the next push reuses the memory
     * rather than asking the allocator again. */
    arena_reset(a);
    if (!push_ptr(&g_pool, &g_pool_n, &g_pool_cap, a)) {
        arena_free(a);
        free(a);
    }
}

TUR_RT_API bool tur_region_pop_checked(int depth) {
    if (depth <= 0 || depth != g_live_n) return false;   /* mismatched: refuse */
    if (generation_escaped(depth - 1)) {
        tur_region_pop(depth);       /* retire: correctness over saving */
        return false;
    }
    tur_region_pop_reclaim(depth);
    return true;
}

TUR_RT_API void *tur_region_alloc(size_t n) {
    if (g_live_n <= 0) return NULL;   /* no region open -- caller uses malloc */
    return arena_alloc(g_live[g_live_n - 1], n);
}

TUR_RT_API void *tur_region_alloc_or_malloc(size_t n) {
    void *p = tur_region_alloc(n);
    return p ? p : malloc(n);
}

TUR_RT_API bool tur_region_owns(const void *p) {
    if (!p) return false;
    for (int i = 0; i < g_live_n; i++)
        if (arena_owns(g_live[i], p)) return true;
    for (int i = 0; i < g_retired_n; i++)
        if (arena_owns(g_retired[i], p)) return true;
    return false;
}

TUR_RT_API void tur_region_free(void *p) {
    if (!p) return;
    /* Region memory is owned by the generation, not by this pointer: handing it
     * to free() is an allocator mismatch (glibc aborts), so the guard is not a
     * leak-avoidance nicety but the thing that keeps a region-allocated node
     * survivable at all once a drop path runs over it. */
    if (tur_region_owns(p)) return;
    free(p);
}

TUR_RT_API bool tur_region_active(void) { return g_live_n > 0; }

TUR_RT_API void tur_region_shutdown(void) {
    for (int i = 0; i < g_live_n; i++)    { arena_free(g_live[i]);    free(g_live[i]); }
    for (int i = 0; i < g_retired_n; i++) { arena_free(g_retired[i]); free(g_retired[i]); }
    for (int i = 0; i < g_pool_n; i++)    { arena_free(g_pool[i]);    free(g_pool[i]); }
    free(g_live);    g_live = NULL;    g_live_n = g_live_cap = 0;
    free(g_retired); g_retired = NULL; g_retired_n = g_retired_cap = 0;
    free(g_pool);    g_pool = NULL;    g_pool_n = g_pool_cap = 0;
    free(g_escaped); g_escaped = NULL; g_escaped_cap = 0;
}
