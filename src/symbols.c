#include "symbols.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_BUCKETS 64

int strslice_eq(StrSlice a, StrSlice b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, a.len) == 0);
}

/* FNV-1a 32-bit. */
static uint32_t hash_bytes(const char *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 16777619u;
    }
    return h;
}

void symtab_init(SymbolTable *st, Arena *arena) {
    st->arena = arena;
    st->nbuckets = INITIAL_BUCKETS;
    st->buckets = (SymbolEntry **)calloc(st->nbuckets, sizeof(SymbolEntry *));
    if (!st->buckets) {
        fprintf(stderr, "tur: out of memory\n");
        abort();
    }
    st->count = 0;
}

void symtab_free(SymbolTable *st) {
    free(st->buckets);
    st->buckets = NULL;
    st->nbuckets = 0;
    st->count = 0;
    /* Symbol structs and name strings live in the arena; nothing to free here. */
}

static void rehash(SymbolTable *st) {
    size_t new_n = st->nbuckets * 2;
    SymbolEntry **nb = (SymbolEntry **)calloc(new_n, sizeof(SymbolEntry *));
    if (!nb) {
        fprintf(stderr, "tur: out of memory\n");
        abort();
    }
    for (size_t i = 0; i < st->nbuckets; i++) {
        SymbolEntry *e = st->buckets[i];
        while (e) {
            SymbolEntry *next = e->next;
            size_t idx = e->sym->hash & (new_n - 1);
            e->next = nb[idx];
            nb[idx] = e;
            e = next;
        }
    }
    free(st->buckets);
    st->buckets = nb;
    st->nbuckets = new_n;
}

const Symbol *symtab_intern(SymbolTable *st, StrSlice name) {
    uint32_t h = hash_bytes(name.p, name.len);
    size_t idx = h & (st->nbuckets - 1);

    for (SymbolEntry *e = st->buckets[idx]; e; e = e->next) {
        Symbol *s = e->sym;
        if (s->hash == h && s->len == name.len &&
            (name.len == 0 || memcmp(s->name, name.p, name.len) == 0)) {
            return s;
        }
    }

    Symbol *s = (Symbol *)arena_alloc(st->arena, sizeof(Symbol));
    s->name = arena_strdup(st->arena, name.p, name.len);
    s->len = name.len;
    s->hash = h;

    SymbolEntry *e = (SymbolEntry *)arena_alloc(st->arena, sizeof(SymbolEntry));
    e->sym = s;
    e->next = st->buckets[idx];
    st->buckets[idx] = e;
    st->count++;

    if (st->count * 2 > st->nbuckets) rehash(st);
    return s;
}
