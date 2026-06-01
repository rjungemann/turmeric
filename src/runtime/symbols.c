/* symbols.c -- runtime intern table for first-class symbols (SYM5).
 *
 * A small open-addressed hash table keyed by name, guarded by a single mutex.
 * Entries are never removed and live for the process lifetime, so the pointer a
 * caller receives is stable forever -- the same identity contract the static
 * literal records have.  This file is auto-linked into -Xsymbols programs via
 * the `__tur_autolink__` marker in stdlib/sym.tur (mirroring hamt.c). */
#include "symbols.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "hamt.h"   /* tur_hamt_hash_str -- match the codegen hash exactly */

static pthread_mutex_t        g_sym_mu  = PTHREAD_MUTEX_INITIALIZER;
static const struct __tur_sym **g_sym_tab = NULL;  /* open-addressed slots */
static size_t                 g_sym_cap = 0;        /* always a power of two */
static size_t                 g_sym_len = 0;        /* live entries */

static int sym_name_eq(const struct __tur_sym *r, const char *s, uint32_t len) {
    return r->len == len && memcmp(r->name, s, len) == 0;
}

/* Insert `rec` into the table (caller holds the lock; capacity guaranteed). */
static void sym_tab_put(const struct __tur_sym *rec) {
    size_t mask = g_sym_cap - 1;
    size_t i = (size_t)rec->hash & mask;
    while (g_sym_tab[i] != NULL) i = (i + 1) & mask;
    g_sym_tab[i] = rec;
    g_sym_len++;
}

/* Grow + rehash (caller holds the lock). */
static void sym_tab_grow(void) {
    size_t new_cap = g_sym_cap ? g_sym_cap * 2 : 64;
    const struct __tur_sym **old = g_sym_tab;
    size_t old_cap = g_sym_cap;
    g_sym_tab = (const struct __tur_sym **)calloc(new_cap, sizeof(*g_sym_tab));
    if (!g_sym_tab) { g_sym_tab = old; return; }  /* OOM: keep old table */
    g_sym_cap = new_cap;
    g_sym_len = 0;
    for (size_t i = 0; i < old_cap; i++)
        if (old[i]) sym_tab_put(old[i]);
    free(old);
}

/* Find an existing record for (s,len) (caller holds the lock), or NULL. */
static const struct __tur_sym *sym_tab_find(const char *s, uint32_t len, uint64_t hash) {
    if (g_sym_cap == 0) return NULL;
    size_t mask = g_sym_cap - 1;
    size_t i = (size_t)hash & mask;
    while (g_sym_tab[i] != NULL) {
        if (g_sym_tab[i]->hash == hash && sym_name_eq(g_sym_tab[i], s, len))
            return g_sym_tab[i];
        i = (i + 1) & mask;
    }
    return NULL;
}

void tur_sym_register(const struct __tur_sym *rec) {
    if (!rec) return;
    pthread_mutex_lock(&g_sym_mu);
    if ((g_sym_len + 1) * 4 >= g_sym_cap * 3)  /* keep load factor < 0.75 */
        sym_tab_grow();
    if (!sym_tab_find(rec->name, rec->len, rec->hash))
        sym_tab_put(rec);   /* first registration of this name wins */
    pthread_mutex_unlock(&g_sym_mu);
}

const struct __tur_sym *tur_sym_intern(const char *s, uint32_t len) {
    if (!s) return NULL;
    uint64_t hash = tur_hamt_hash_str(s);
    pthread_mutex_lock(&g_sym_mu);
    if ((g_sym_len + 1) * 4 >= g_sym_cap * 3)
        sym_tab_grow();
    const struct __tur_sym *found = sym_tab_find(s, len, hash);
    if (found) {
        pthread_mutex_unlock(&g_sym_mu);
        return found;
    }
    /* Allocate a fresh, process-lifetime record (header + name + NUL). */
    struct __tur_sym *rec = (struct __tur_sym *)malloc(sizeof(struct __tur_sym) + len + 1);
    if (!rec) { pthread_mutex_unlock(&g_sym_mu); return NULL; }
    rec->hash = hash;
    rec->len  = len;
    rec->_pad = 0;
    memcpy(rec->name, s, len);
    rec->name[len] = '\0';
    sym_tab_put(rec);
    pthread_mutex_unlock(&g_sym_mu);
    return rec;
}
