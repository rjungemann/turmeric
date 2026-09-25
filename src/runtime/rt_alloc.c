/* rt_alloc.c -- the runtime archive's allocator hook.  See rt_alloc.h. */
#include "rt_alloc.h"

#include <stdlib.h>

/* libc until something installs another.  A plain struct in .data: the
 * collector that installs itself scans this segment, and the table holds
 * only code pointers anyway. */
static tur_rt_allocator g_tur_rt_alloc = { malloc, calloc, realloc, free };

void tur_rt_set_allocator(const tur_rt_allocator *a) {
    if (a && a->malloc && a->calloc && a->realloc && a->free) g_tur_rt_alloc = *a;
    else { g_tur_rt_alloc.malloc = malloc; g_tur_rt_alloc.calloc = calloc;
           g_tur_rt_alloc.realloc = realloc; g_tur_rt_alloc.free = free; }
}

void *tur_rt_malloc(size_t n)           { return g_tur_rt_alloc.malloc(n); }
void *tur_rt_calloc(size_t n, size_t m) { return g_tur_rt_alloc.calloc(n, m); }
void *tur_rt_realloc(void *p, size_t n) { return g_tur_rt_alloc.realloc(p, n); }
void  tur_rt_free(void *p)              { g_tur_rt_alloc.free(p); }
