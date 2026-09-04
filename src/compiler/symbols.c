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

bool symtab_contains(const SymbolTable *st, StrSlice name) {
    uint32_t h = hash_bytes(name.p, name.len);
    size_t idx = h & (st->nbuckets - 1);
    for (SymbolEntry *e = st->buckets[idx]; e; e = e->next) {
        const Symbol *s = e->sym;
        if (s->hash == h && s->len == name.len &&
            (name.len == 0 || memcmp(s->name, name.p, name.len) == 0)) {
            return true;
        }
    }
    return false;
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

/* Phase 8: Levenshtein distance for "did you mean" suggestions.
   Returns the minimum number of single-character edits (insertions, deletions, or
   substitutions) required to change one symbol into the other. */
int sym_levenshtein_distance(const Symbol *a, const Symbol *b) {
    size_t len_a = a->len;
    size_t len_b = b->len;
    
    /* Use a simple dynamic programming approach with O(n*m) space.
       For short symbol names, this is fine. */
    int *prev_row = (int *)malloc((len_b + 1) * sizeof(int));
    int *curr_row = (int *)malloc((len_b + 1) * sizeof(int));
    
    if (!prev_row || !curr_row) {
        free(prev_row);
        free(curr_row);
        return -1; /* Error, but shouldn't happen */
    }
    
    /* Initialize previous row */
    for (size_t j = 0; j <= len_b; j++) {
        prev_row[j] = (int)j;
    }
    
    /* Fill the matrix */
    for (size_t i = 1; i <= len_a; i++) {
        curr_row[0] = (int)i;
        for (size_t j = 1; j <= len_b; j++) {
            int cost = (a->name[i - 1] == b->name[j - 1]) ? 0 : 1;
            int insertion = prev_row[j] + 1;
            int deletion = curr_row[j - 1] + 1;
            int substitution = prev_row[j - 1] + cost;
            curr_row[j] = insertion < deletion ? 
                         (insertion < substitution ? insertion : substitution) :
                         (deletion < substitution ? deletion : substitution);
        }
        
        /* Swap rows */
        int *temp = prev_row;
        prev_row = curr_row;
        curr_row = temp;
    }
    
    int result = prev_row[len_b];
    free(prev_row);
    free(curr_row);
    return result;
}
