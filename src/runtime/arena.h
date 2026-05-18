#ifndef TUR_ARENA_H
#define TUR_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct ArenaSlab ArenaSlab;

typedef struct Arena {
    ArenaSlab *head;
    size_t default_slab;
    size_t total_bytes;
    size_t total_allocs;
} Arena;

void  arena_init(Arena *a, size_t default_slab_size);
void *arena_alloc(Arena *a, size_t size);
void *arena_alloc_aligned(Arena *a, size_t size, size_t align);
char *arena_strdup(Arena *a, const char *s, size_t len);
void  arena_free(Arena *a);

#endif
