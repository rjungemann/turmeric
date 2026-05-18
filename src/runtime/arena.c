#include "arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define DEFAULT_SLAB (64 * 1024)

struct ArenaSlab {
    ArenaSlab *next;
    size_t cap;
    size_t used;
    /* data[cap] follows inline */
    unsigned char data[];
};

static void oom(void) {
    fprintf(stderr, "tur: out of memory\n");
    abort();
}

static ArenaSlab *slab_new(size_t cap) {
    ArenaSlab *s = (ArenaSlab *)malloc(sizeof(ArenaSlab) + cap);
    if (!s) oom();
    s->next = NULL;
    s->cap = cap;
    s->used = 0;
    return s;
}

void arena_init(Arena *a, size_t default_slab_size) {
    a->head = NULL;
    a->default_slab = default_slab_size ? default_slab_size : DEFAULT_SLAB;
    a->total_bytes = 0;
    a->total_allocs = 0;
}

static size_t align_up(size_t n, size_t align) {
    assert(align && (align & (align - 1)) == 0);
    return (n + (align - 1)) & ~(align - 1);
}

void *arena_alloc_aligned(Arena *a, size_t size, size_t align) {
    if (size == 0) size = 1;
    if (align < sizeof(void *)) align = sizeof(void *);

    ArenaSlab *s = a->head;
    if (s) {
        size_t aligned_used = align_up(s->used, align);
        if (aligned_used + size <= s->cap) {
            void *p = s->data + aligned_used;
            s->used = aligned_used + size;
            a->total_bytes += size;
            a->total_allocs++;
            return p;
        }
    }

    /* Need a new slab. Grow if the request is large. */
    size_t cap = a->default_slab;
    if (size + align > cap) cap = size + align;
    ArenaSlab *fresh = slab_new(cap);
    fresh->next = a->head;
    a->head = fresh;

    size_t aligned_used = align_up(0, align);
    void *p = fresh->data + aligned_used;
    fresh->used = aligned_used + size;
    a->total_bytes += size;
    a->total_allocs++;
    return p;
}

void *arena_alloc(Arena *a, size_t size) {
    return arena_alloc_aligned(a, size, sizeof(void *));
}

char *arena_strdup(Arena *a, const char *s, size_t len) {
    char *p = (char *)arena_alloc_aligned(a, len + 1, 1);
    if (len) memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

void arena_free(Arena *a) {
    ArenaSlab *s = a->head;
    while (s) {
        ArenaSlab *next = s->next;
        free(s);
        s = next;
    }
    a->head = NULL;
    a->total_bytes = 0;
    a->total_allocs = 0;
}
