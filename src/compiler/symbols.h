#ifndef TUR_SYMBOLS_H
#define TUR_SYMBOLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime/arena.h"

typedef struct StrSlice {
    const char *p;
    uint32_t    len;
} StrSlice;

static inline StrSlice strslice(const char *p, uint32_t len) {
    StrSlice s = { p, len };
    return s;
}

int strslice_eq(StrSlice a, StrSlice b);

typedef struct Symbol {
    const char *name;   /* arena-allocated, NUL-terminated */
    uint32_t    len;
    uint32_t    hash;
} Symbol;

typedef struct SymbolEntry {
    struct SymbolEntry *next;
    Symbol             *sym;
} SymbolEntry;

typedef struct SymbolTable {
    Arena        *arena;
    SymbolEntry **buckets;
    size_t        nbuckets;
    size_t        count;
} SymbolTable;

void          symtab_init(SymbolTable *st, Arena *arena);
void          symtab_free(SymbolTable *st);
const Symbol *symtab_intern(SymbolTable *st, StrSlice name);
/* True when `name` is already interned, without interning it.  Used by
 * gensym to guarantee a generated name is fresh with respect to every
 * symbol the reader has seen so far. */
bool          symtab_contains(const SymbolTable *st, StrSlice name);

/* Phase 8: Levenshtein distance for "did you mean" suggestions */
int sym_levenshtein_distance(const Symbol *a, const Symbol *b);

#endif
