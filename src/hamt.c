/* hamt.c - Persistent Hash Array Mapped Trie implementation (Phase P1)
 *
 * A HAMT is an immutable hash map with structural sharing.
 * Uses 5-bit hash chunks (32 slots per level) for balanced trie depth.
 * 
 * Memory management: reference counting (Phase P1 v1).
 * Collision handling: linked list in v1.
 * Hash function: xxHash64 (speed-optimised, non-cryptographic).
 *
 * See docs/hamt-plan.md for full design and phase breakdown.
 */

#include "hamt.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * xxHash64 - Minimal implementation for HAMT
 * ============================================================================
 */

#define XXH_rotl64(x, r) (((x) << (r)) | ((x) >> (64 - (r))))

#define XXH_PRIME64_1 0x9E3779B185EBCA87ULL
#define XXH_PRIME64_2 0xC2B2AE3D27D4EB4FULL
#define XXH_PRIME64_3 0x27D4EB2F165667C5ULL
#define XXH_PRIME64_4 0x85EBCA77C2B2AE63ULL
#define XXH_PRIME64_5 0x27D4EB2F165667C5ULL

static uint64_t xxh64_round(uint64_t acc, uint64_t input) {
    acc += input * XXH_PRIME64_2;
    acc = XXH_rotl64(acc, 31);
    acc *= XXH_PRIME64_1;
    return acc;
}

static uint64_t xxh64_finalize(uint64_t acc, uint64_t input) {
    acc ^= xxh64_round(0, input);
    acc *= XXH_PRIME64_3;
    acc ^= acc >> 33;
    acc *= XXH_PRIME64_2;
    acc ^= acc >> 29;
    acc *= XXH_PRIME64_3;
    acc ^= acc >> 32;
    return acc;
}

uint64_t hamt_hash_xxh64(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t acc = 0;
    
    if (len >= 32) {
        uint64_t v1 = XXH_PRIME64_1 + XXH_PRIME64_2;
        uint64_t v2 = XXH_PRIME64_2;
        uint64_t v3 = 0;
        uint64_t v4 = XXH_PRIME64_1;
        
        do {
            v1 += *(uint64_t *)(p) * XXH_PRIME64_2;
            v1 = XXH_rotl64(v1, 31);
            v1 *= XXH_PRIME64_1;
            p += 8;
            
            v2 += *(uint64_t *)(p) * XXH_PRIME64_2;
            v2 = XXH_rotl64(v2, 31);
            v2 *= XXH_PRIME64_1;
            p += 8;
            
            v3 += *(uint64_t *)(p) * XXH_PRIME64_2;
            v3 = XXH_rotl64(v3, 31);
            v3 *= XXH_PRIME64_1;
            p += 8;
            
            v4 += *(uint64_t *)(p) * XXH_PRIME64_2;
            v4 = XXH_rotl64(v4, 31);
            v4 *= XXH_PRIME64_1;
            p += 8;
            
            len -= 32;
        } while (len >= 32);
        
        acc = XXH_rotl64(v1, 1) + XXH_rotl64(v2, 7) + XXH_rotl64(v3, 12) + XXH_rotl64(v4, 18);
        acc = xxh64_round(acc, v1);
        acc = xxh64_round(acc, v2);
        acc = xxh64_round(acc, v3);
        acc = xxh64_round(acc, v4);
    } else {
        acc = XXH_PRIME64_5 + len;
        
        while (len >= 8) {
            uint64_t k = *(uint64_t *)p;
            acc = xxh64_round(acc, k);
            p += 8;
            len -= 8;
        }
        
        if (len >= 4) {
            acc = xxh64_round(acc, *(uint32_t *)p);
            p += 4;
            len -= 4;
        }
        
        while (len > 0) {
            acc = xxh64_round(acc, *p);
            p += 1;
            len -= 1;
        }
    }
    
    return xxh64_finalize(acc, len);
}

uint64_t hamt_hash_str(const char *str) {
    return hamt_hash_xxh64(str, strlen(str));
}

/* ============================================================================
 * HAMT Constants
 * ============================================================================
 */

#define HAMT_BITS_PER_LEVEL 5
#define HAMT_SLOTS_PER_LEVEL (1 << HAMT_BITS_PER_LEVEL)
#define HAMT_MASK_PER_LEVEL ((1 << HAMT_BITS_PER_LEVEL) - 1)

/* ============================================================================
 * Memory Allocation
 * ============================================================================
 */

static void *hamt_malloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "hamt: out of memory\n");
        abort();
    }
    return ptr;
}

/* Allocate a HAMT node with initial ref_count=1 */
HamtNode *hamt_node_alloc(size_t size) {
    HamtNode *n = (HamtNode *)hamt_malloc(size);
    n->ref_count = 1;
    return n;
}

/* Retain a node (increment ref count) */
void hamt_node_retain(HamtNode *n) {
    if (n) {
        n->ref_count++;
    }
}

/* Release a node (decrement ref count; free if reaches zero) */
void hamt_node_release(HamtNode *n);

/* Allocate a Hamt root with initial ref_count=1 */
static Hamt *hamt_alloc_empty(void) {
    Hamt *m = (Hamt *)hamt_malloc(sizeof(Hamt));
    m->root = NULL;
    m->count = 0;
    m->ref_count = 1;
    return m;
}

/* ============================================================================
 * Bitmap Node Helpers
 * ============================================================================
 */

static uint32_t popcount32(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcount(x);
#else
    x = x - ((x >> 1) & 0x55555555);
    x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
    x = (x + (x >> 4)) & 0x0F0F0F0F;
    x = x + (x >> 8);
    x = x + (x >> 16);
    return x & 0x3F;
#endif
}

static HamtNode *bitmap_node_create(uint32_t bitmap) {
    size_t child_count = popcount32(bitmap);
    size_t size = sizeof(HamtNode) + sizeof(HamtNode *) * (child_count - 1);
    HamtNode *n = hamt_node_alloc(size);
    n->type = HAMT_NODE_BITMAP;
    n->as.bitmap.bitmap = bitmap;
    return n;
}

static HamtNode *array_node_create(void) {
    HamtNode *n = hamt_node_alloc(sizeof(HamtNode));
    n->type = HAMT_NODE_ARRAY;
    for (uint32_t i = 0; i < HAMT_SLOTS_PER_LEVEL; i++) {
        n->as.array.children[i] = NULL;
    }
    return n;
}

static HamtNode *collision_node_create(uint32_t hash_prefix, 
                                       uint64_t hash, void *key, void *val) {
    HamtNode *n = hamt_node_alloc(sizeof(HamtNode));
    n->type = HAMT_NODE_COLLISION;
    n->as.collision.hash_prefix = hash_prefix;
    n->as.collision.entries = (HamtEntry *)hamt_malloc(sizeof(HamtEntry));
    n->as.collision.entries->hash = hash;
    n->as.collision.entries->key = key;
    n->as.collision.entries->val = val;
    n->as.collision.entries->next = NULL;
    return n;
}

/* ============================================================================
 * Hash utilities
 * ============================================================================
 */

static uint32_t hash_chunk(uint64_t hash, uint32_t level) {
    return (uint32_t)((hash >> (level * HAMT_BITS_PER_LEVEL)) & HAMT_MASK_PER_LEVEL);
}

/* ============================================================================
 * Entry lookup in collision nodes
 * ============================================================================
 */

static HamtEntry *find_entry(HamtEntry *entries, uint64_t hash, void *key) {
    for (HamtEntry *e = entries; e; e = e->next) {
        if (e->hash == hash && e->key == key) {
            return e;
        }
    }
    return NULL;
}

static bool collision_has(HamtEntry *entries, uint64_t hash, void *key) {
    return find_entry(entries, hash, key) != NULL;
}

static void *collision_get(HamtEntry *entries, uint64_t hash, void *key) {
    HamtEntry *e = find_entry(entries, hash, key);
    return e ? e->val : NULL;
}

/* ============================================================================
 * Node deep free (for cleanup)
 * ============================================================================
 */

static void node_free_recursive(HamtNode *n) {
    if (!n) return;
    
    switch (n->type) {
        case HAMT_NODE_BITMAP: {
            uint32_t bitmap = n->as.bitmap.bitmap;
            for (uint32_t i = 0; i < 32; i++) {
                if (bitmap & (1 << i)) {
                    uint32_t child_idx = 0;
                    uint32_t mask = 1;
                    for (uint32_t j = 0; j < i; j++, mask <<= 1) {
                        if (bitmap & mask) child_idx++;
                    }
                    hamt_node_release(n->as.bitmap.children[child_idx]);
                }
            }
            break;
        }
        case HAMT_NODE_ARRAY: {
            for (uint32_t i = 0; i < HAMT_SLOTS_PER_LEVEL; i++) {
                hamt_node_release(n->as.array.children[i]);
            }
            break;
        }
        case HAMT_NODE_COLLISION: {
            HamtEntry *e = n->as.collision.entries;
            while (e) {
                HamtEntry *next = e->next;
                free(e);
                e = next;
            }
            break;
        }
    }
}

void hamt_node_release(HamtNode *n) {
    if (n && --n->ref_count == 0) {
        node_free_recursive(n);
        free(n);
    }
}

/* ============================================================================
 * Node copy functions (for structural sharing)
 * ============================================================================
 */

static HamtNode *bitmap_node_copy(HamtNode *src) {
    uint32_t bitmap = src->as.bitmap.bitmap;
    uint32_t child_count = popcount32(bitmap);
    size_t size = sizeof(HamtNode) + sizeof(HamtNode *) * (child_count - 1);
    
    HamtNode *dst = hamt_node_alloc(size);
    dst->type = HAMT_NODE_BITMAP;
    dst->as.bitmap.bitmap = bitmap;
    
    for (uint32_t i = 0; i < child_count; i++) {
        dst->as.bitmap.children[i] = src->as.bitmap.children[i];
        if (dst->as.bitmap.children[i]) {
            hamt_node_retain(dst->as.bitmap.children[i]);
        }
    }
    
    return dst;
}

static HamtNode *array_node_copy(HamtNode *src) {
    HamtNode *dst = hamt_node_alloc(sizeof(HamtNode));
    dst->type = HAMT_NODE_ARRAY;
    
    for (uint32_t i = 0; i < HAMT_SLOTS_PER_LEVEL; i++) {
        dst->as.array.children[i] = src->as.array.children[i];
        if (dst->as.array.children[i]) {
            hamt_node_retain(dst->as.array.children[i]);
        }
    }
    
    return dst;
}

static HamtNode *collision_node_copy(HamtNode *src) {
    HamtNode *dst = hamt_node_alloc(sizeof(HamtNode));
    dst->type = HAMT_NODE_COLLISION;
    dst->as.collision.hash_prefix = src->as.collision.hash_prefix;
    
    HamtEntry **tail = &dst->as.collision.entries;
    for (HamtEntry *e = src->as.collision.entries; e; e = e->next) {
        *tail = (HamtEntry *)hamt_malloc(sizeof(HamtEntry));
        (*tail)->hash = e->hash;
        (*tail)->key = e->key;
        (*tail)->val = e->val;
        (*tail)->next = NULL;
        tail = &(*tail)->next;
    }
    
    return dst;
}

/* ============================================================================
 * Insert operation
 * ============================================================================
 */

static HamtNode *node_insert(HamtNode *n, uint64_t hash, void *key, void *val, 
                             uint32_t level);

static HamtNode *node_insert(HamtNode *n, uint64_t hash, void *key, void *val, 
                             uint32_t level) {
    uint32_t chunk = hash_chunk(hash, level);
    
    if (!n) {
        return collision_node_create(chunk, hash, key, val);
    }
    
    switch (n->type) {
        case HAMT_NODE_BITMAP: {
            uint32_t bitmap = n->as.bitmap.bitmap;
            
            bool has_child = (bitmap & (1 << chunk)) != 0;
            
            if (has_child) {
                uint32_t idx = 0;
                uint32_t mask = 1;
                for (uint32_t i = 0; i < chunk; i++, mask <<= 1) {
                    if (bitmap & mask) idx++;
                }
                
                HamtNode *new_child = node_insert(n->as.bitmap.children[idx], hash, key, val, level + 1);
                
                if (new_child == n->as.bitmap.children[idx]) {
                    return n;
                }
                
                HamtNode *new_node = bitmap_node_copy(n);
                new_node->as.bitmap.children[idx] = new_child;
                return new_node;
            } else {
                uint32_t new_bitmap = bitmap | (1 << chunk);
                HamtNode *new_node = bitmap_node_create(new_bitmap);
                
                uint32_t new_idx = 0;
                uint32_t pos = 0;
                for (uint32_t i = 0; i < 32; i++) {
                    if (bitmap & (1 << i)) {
                        new_node->as.bitmap.children[new_idx] = n->as.bitmap.children[pos];
                        hamt_node_retain(new_node->as.bitmap.children[new_idx]);
                        new_idx++;
                        pos++;
                    } else if (i == chunk) {
                        new_node->as.bitmap.children[new_idx] = collision_node_create(chunk, hash, key, val);
                        new_idx++;
                    }
                }
                
                return new_node;
            }
        }
        
        case HAMT_NODE_ARRAY: {
            HamtNode *new_child = node_insert(n->as.array.children[chunk], hash, key, val, level + 1);
            
            if (new_child == n->as.array.children[chunk]) {
                return n;
            }
            
            HamtNode *new_node = array_node_copy(n);
            new_node->as.array.children[chunk] = new_child;
            hamt_node_retain(new_child);
            
            return new_node;
        }
        
        case HAMT_NODE_COLLISION: {
            if (n->as.collision.hash_prefix != chunk) {
                uint32_t existing_chunk = n->as.collision.hash_prefix;
                uint32_t new_bitmap = (1 << existing_chunk) | (1 << chunk);
                
                HamtNode *new_node = bitmap_node_create(new_bitmap);
                
                uint32_t idx = 0;
                uint32_t mask = 1;
                for (uint32_t i = 0; i < existing_chunk; i++, mask <<= 1) {
                    if (new_bitmap & mask) idx++;
                }
                new_node->as.bitmap.children[idx] = n;
                hamt_node_retain(n);
                
                idx = 0;
                mask = 1;
                for (uint32_t i = 0; i < chunk; i++, mask <<= 1) {
                    if (new_bitmap & mask) idx++;
                }
                new_node->as.bitmap.children[idx] = collision_node_create(chunk, hash, key, val);
                hamt_node_retain(new_node->as.bitmap.children[idx]);
                
                return new_node;
            }
            
            for (HamtEntry **e = &n->as.collision.entries; *e; e = &(*e)->next) {
                if ((*e)->hash == hash && (*e)->key == key) {
                    HamtNode *new_node = collision_node_copy(n);
                    for (HamtEntry *f = new_node->as.collision.entries; f; f = f->next) {
                        if (f->hash == hash && f->key == key) {
                            f->val = val;
                            break;
                        }
                    }
                    return new_node;
                }
            }
            
            HamtNode *new_node = collision_node_copy(n);
            HamtEntry *new_entry = (HamtEntry *)hamt_malloc(sizeof(HamtEntry));
            new_entry->hash = hash;
            new_entry->key = key;
            new_entry->val = val;
            new_entry->next = new_node->as.collision.entries;
            new_node->as.collision.entries = new_entry;
            
            return new_node;
        }
    }
    
    return n;
}

/* ============================================================================
 * Delete operation
 * ============================================================================
 */

static HamtNode *node_delete(HamtNode *n, uint64_t hash, void *key, uint32_t level) {
    uint32_t chunk = hash_chunk(hash, level);
    
    if (!n) {
        return NULL;
    }
    
    switch (n->type) {
        case HAMT_NODE_BITMAP: {
            uint32_t bitmap = n->as.bitmap.bitmap;
            
            if (!(bitmap & (1 << chunk))) {
                return n;
            }
            
            uint32_t idx = 0;
            uint32_t mask = 1;
            for (uint32_t i = 0; i < chunk; i++, mask <<= 1) {
                if (bitmap & mask) idx++;
            }
            
            HamtNode *new_child = node_delete(n->as.bitmap.children[idx], hash, key, level + 1);
            
            if (new_child == n->as.bitmap.children[idx]) {
                return n;
            }
            
            if (!new_child) {
                hamt_node_release(n->as.bitmap.children[idx]);
                uint32_t new_bitmap = bitmap & ~(1 << chunk);
                uint32_t new_child_count = popcount32(new_bitmap);
                
                if (new_child_count == 0) {
                    return NULL;
                }
                
                HamtNode *new_node = bitmap_node_create(new_bitmap);
                uint32_t old_idx = 0;
                uint32_t new_idx = 0;
                for (uint32_t i = 0; i < 32; i++) {
                    if (bitmap & (1 << i)) {
                        if (i == chunk) {
                            old_idx++;
                        } else {
                            new_node->as.bitmap.children[new_idx] = n->as.bitmap.children[old_idx];
                            hamt_node_retain(new_node->as.bitmap.children[new_idx]);
                            new_idx++;
                            old_idx++;
                        }
                    }
                }
                return new_node;
            } else {
                HamtNode *new_node = bitmap_node_copy(n);
                hamt_node_release(new_node->as.bitmap.children[idx]);
                new_node->as.bitmap.children[idx] = new_child;
                hamt_node_retain(new_child);
                return new_node;
            }
        }
        
        case HAMT_NODE_ARRAY: {
            HamtNode *new_child = node_delete(n->as.array.children[chunk], hash, key, level + 1);
            
            if (new_child == n->as.array.children[chunk]) {
                return n;
            }
            
            if (!new_child) {
                hamt_node_release(n->as.array.children[chunk]);
                HamtNode *new_node = array_node_create();
                for (uint32_t i = 0; i < HAMT_SLOTS_PER_LEVEL; i++) {
                    if (i == chunk) {
                        new_node->as.array.children[i] = NULL;
                    } else {
                        new_node->as.array.children[i] = n->as.array.children[i];
                        if (new_node->as.array.children[i]) {
                            hamt_node_retain(new_node->as.array.children[i]);
                        }
                    }
                }
                return new_node;
            } else {
                HamtNode *new_node = array_node_copy(n);
                hamt_node_release(new_node->as.array.children[chunk]);
                new_node->as.array.children[chunk] = new_child;
                hamt_node_retain(new_child);
                return new_node;
            }
        }
        
        case HAMT_NODE_COLLISION: {
            for (HamtEntry *e = n->as.collision.entries; e; e = e->next) {
                if (e->hash == hash && e->key == key) {
                    HamtNode *new_node = collision_node_copy(n);
                    HamtEntry *new_prev = NULL;
                    for (HamtEntry *new_e = new_node->as.collision.entries; new_e; new_e = new_e->next) {
                        if (new_e->hash == hash && new_e->key == key) {
                            if (new_prev) {
                                new_prev->next = new_e->next;
                            } else {
                                new_node->as.collision.entries = new_e->next;
                            }
                            free(new_e);
                            break;
                        }
                        new_prev = new_e;
                    }
                    
                    if (new_node->as.collision.entries == NULL) {
                        free(new_node);
                        return NULL;
                    }
                    
                    return new_node;
                }
            }
            
            return n;
        }
    }
    
    return n;
}

/* ============================================================================
 * Lookup operation
 * ============================================================================
 */

static void *node_get(HamtNode *n, uint64_t hash, void *key, uint32_t level) {
    uint32_t chunk = hash_chunk(hash, level);
    
    if (!n) {
        return NULL;
    }
    
    switch (n->type) {
        case HAMT_NODE_BITMAP: {
            uint32_t bitmap = n->as.bitmap.bitmap;
            
            if (!(bitmap & (1 << chunk))) {
                return NULL;
            }
            
            uint32_t idx = 0;
            uint32_t mask = 1;
            for (uint32_t i = 0; i < chunk; i++, mask <<= 1) {
                if (bitmap & mask) idx++;
            }
            
            return node_get(n->as.bitmap.children[idx], hash, key, level + 1);
        }
        
        case HAMT_NODE_ARRAY: {
            return node_get(n->as.array.children[chunk], hash, key, level + 1);
        }
        
        case HAMT_NODE_COLLISION: {
            return collision_get(n->as.collision.entries, hash, key);
        }
    }
    
    return NULL;
}

/* ============================================================================
 * Has operation
 * ============================================================================
 */

static bool node_has(HamtNode *n, uint64_t hash, void *key, uint32_t level) {
    uint32_t chunk = hash_chunk(hash, level);
    
    if (!n) {
        return false;
    }
    
    switch (n->type) {
        case HAMT_NODE_BITMAP: {
            uint32_t bitmap = n->as.bitmap.bitmap;
            
            if (!(bitmap & (1 << chunk))) {
                return false;
            }
            
            uint32_t idx = 0;
            uint32_t mask = 1;
            for (uint32_t i = 0; i < chunk; i++, mask <<= 1) {
                if (bitmap & mask) idx++;
            }
            
            return node_has(n->as.bitmap.children[idx], hash, key, level + 1);
        }
        
        case HAMT_NODE_ARRAY: {
            return node_has(n->as.array.children[chunk], hash, key, level + 1);
        }
        
        case HAMT_NODE_COLLISION: {
            return collision_has(n->as.collision.entries, hash, key);
        }
    }
    
    return false;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================
 */

Hamt *hamt_new(void) {
    return hamt_alloc_empty();
}

void hamt_free(Hamt *m) {
    if (!m || --m->ref_count != 0) {
        return;
    }
    
    if (m->root) {
        hamt_node_release(m->root);
    }
    free(m);
}

Hamt *hamt_retain(Hamt *m) {
    if (m) {
        m->ref_count++;
    }
    return m;
}

uint32_t hamt_count(Hamt *m) {
    if (!m) return 0;
    return m->count;
}

Hamt *hamt_set(Hamt *m, uint64_t hash, void *key, void *val) {
    if (!m) {
        m = hamt_alloc_empty();
    }
    
    bool key_exists = hamt_has(m, hash, key);
    HamtNode *new_root = node_insert(m->root, hash, key, val, 0);
    
    if (new_root == m->root) {
        return m;
    }
    
    Hamt *new_map = hamt_alloc_empty();
    new_map->root = new_root;
    hamt_node_retain(new_root);
    
    if (key_exists) {
        new_map->count = m->count;
    } else {
        new_map->count = m->count + 1;
    }
    
    return new_map;
}

Hamt *hamt_del(Hamt *m, uint64_t hash, void *key) {
    if (!m) {
        return m;
    }
    
    if (!hamt_has(m, hash, key)) {
        return m;
    }
    
    HamtNode *new_root = node_delete(m->root, hash, key, 0);
    
    if (new_root == m->root) {
        return m;
    }
    
    Hamt *new_map = hamt_alloc_empty();
    new_map->root = new_root;
    hamt_node_retain(new_root);
    new_map->count = m->count - 1;
    
    return new_map;
}

bool hamt_has(Hamt *m, uint64_t hash, void *key) {
    if (!m) return false;
    return node_has(m->root, hash, key, 0);
}

void *hamt_get(Hamt *m, uint64_t hash, void *key) {
    if (!m) return NULL;
    return node_get(m->root, hash, key, 0);
}

Hamt *hamt_merge(Hamt *a, Hamt *b) {
    if (!a) return hamt_retain(b);
    if (!b) return hamt_retain(a);
    
    Hamt *result = hamt_retain(a);
    
    HamtIter iter;
    hamt_iter_init(&iter, b);
    
    uint64_t hash;
    void *key, *val;
    while (hamt_iter_next(&iter, &hash, &key, &val)) {
        Hamt *old = result;
        result = hamt_set(result, hash, key, val);
        if (old != result) {
            hamt_free(old);
        }
    }
    
    hamt_iter_free(&iter);
    return result;
}

/* ============================================================================
 * Iteration Implementation
 * ============================================================================
 */

void hamt_iter_init(HamtIter *iter, Hamt *m) {
    iter->map = m;
    iter->stack = NULL;
    iter->stack_cap = 0;
    iter->stack_len = 0;
    iter->coll_entry = NULL;
    iter->child_idx = 0;
    iter->done = (m == NULL || m->root == NULL);
    
    if (!iter->done) {
        iter->stack_cap = 16;
        iter->stack = (HamtNode **)hamt_malloc(sizeof(HamtNode *) * iter->stack_cap);
        iter->stack[0] = m->root;
        iter->stack_len = 1;
    }
}

void hamt_iter_free(HamtIter *iter) {
    if (iter->stack) {
        free(iter->stack);
        iter->stack = NULL;
    }
    iter->stack_cap = 0;
    iter->stack_len = 0;
    iter->done = true;
}

bool hamt_iter_next(HamtIter *iter, uint64_t *hash_out, void **key_out, void **val_out) {
    while (!iter->done) {
        if (iter->coll_entry) {
            *hash_out = iter->coll_entry->hash;
            *key_out = iter->coll_entry->key;
            *val_out = iter->coll_entry->val;
            iter->coll_entry = iter->coll_entry->next;
            if (iter->coll_entry) {
                return true;
            }
            iter->coll_entry = NULL;
        }
        
        if (iter->stack_len == 0) {
            iter->done = true;
            break;
        }
        
        HamtNode *n = iter->stack[iter->stack_len - 1];
        
        switch (n->type) {
            case HAMT_NODE_COLLISION: {
                iter->coll_entry = n->as.collision.entries;
                iter->stack_len--;
                if (iter->coll_entry) {
                    *hash_out = iter->coll_entry->hash;
                    *key_out = iter->coll_entry->key;
                    *val_out = iter->coll_entry->val;
                    iter->coll_entry = iter->coll_entry->next;
                    return true;
                }
                break;
            }
            case HAMT_NODE_BITMAP: {
                uint32_t bitmap = n->as.bitmap.bitmap;
                uint32_t child_count = popcount32(bitmap);
                
                if (iter->child_idx < child_count) {
                    HamtNode *child = n->as.bitmap.children[iter->child_idx++];
                    if (iter->stack_len >= iter->stack_cap) {
                        iter->stack_cap *= 2;
                        iter->stack = (HamtNode **)realloc(iter->stack, 
                            sizeof(HamtNode *) * iter->stack_cap);
                    }
                    iter->stack[iter->stack_len++] = child;
                } else {
                    iter->stack_len--;
                    iter->child_idx = 0;
                }
                break;
            }
            case HAMT_NODE_ARRAY: {
                if (iter->child_idx < HAMT_SLOTS_PER_LEVEL) {
                    HamtNode *child = n->as.array.children[iter->child_idx++];
                    if (child) {
                        if (iter->stack_len >= iter->stack_cap) {
                            iter->stack_cap *= 2;
                            iter->stack = (HamtNode **)realloc(iter->stack, 
                                sizeof(HamtNode *) * iter->stack_cap);
                        }
                        iter->stack[iter->stack_len++] = child;
                    }
                } else {
                    iter->stack_len--;
                    iter->child_idx = 0;
                }
                break;
            }
        }
    }
    
    return false;
}

/* ============================================================================
 * Debugging: Dump functions
 * ============================================================================
 */

static void dump_node(HamtNode *n, FILE *out, int indent) {
    if (!n) return;
    
    for (int i = 0; i < indent; i++) fprintf(out, "  ");
    
    switch (n->type) {
        case HAMT_NODE_BITMAP: {
            uint32_t bitmap = n->as.bitmap.bitmap;
            uint32_t child_count = popcount32(bitmap);
            fprintf(out, "BitmapNode (bitmap=0x%08X, children=%u, refs=%u)\n", 
                    bitmap, child_count, n->ref_count);
            for (uint32_t i = 0; i < 32; i++) {
                if (bitmap & (1 << i)) {
                    uint32_t child_idx = 0;
                    uint32_t mask = 1;
                    for (uint32_t j = 0; j < i; j++, mask <<= 1) {
                        if (bitmap & mask) child_idx++;
                    }
                    for (int j = 0; j < indent + 1; j++) fprintf(out, "  ");
                    fprintf(out, "  slot %u:\n", i);
                    dump_node(n->as.bitmap.children[child_idx], out, indent + 2);
                }
            }
            break;
        }
        case HAMT_NODE_ARRAY: {
            fprintf(out, "ArrayNode (refs=%u)\n", n->ref_count);
            for (uint32_t i = 0; i < HAMT_SLOTS_PER_LEVEL; i++) {
                if (n->as.array.children[i]) {
                    for (int j = 0; j < indent + 1; j++) fprintf(out, "  ");
                    fprintf(out, "  slot %u:\n", i);
                    dump_node(n->as.array.children[i], out, indent + 2);
                }
            }
            break;
        }
        case HAMT_NODE_COLLISION: {
            fprintf(out, "CollisionNode (prefix=0x%02X, refs=%u)\n", 
                    n->as.collision.hash_prefix, n->ref_count);
            for (HamtEntry *e = n->as.collision.entries; e; e = e->next) {
                for (int j = 0; j < indent + 1; j++) fprintf(out, "  ");
                fprintf(out, "  {hash=0x%016llX, key=%p, val=%p}\n", 
                        (unsigned long long)e->hash, e->key, e->val);
            }
            break;
        }
    }
}

void hamt_dump(Hamt *m, FILE *out) {
    if (!m) {
        fprintf(out, "(null)\n");
        return;
    }
    fprintf(out, "HAMT (count=%u, refs=%u)\n", m->count, m->ref_count);
    if (m->root) {
        dump_node(m->root, out, 1);
    } else {
        fprintf(out, "  (empty)\n");
    }
}

void hamt_dump_dot(Hamt *m, FILE *out) {
    if (!m) {
        fprintf(out, "digraph HAMT {\n  rankdir=TB;\n}\n");
        return;
    }
    
    fprintf(out, "digraph HAMT {\n");
    fprintf(out, "  rankdir=TB;\n");
    fprintf(out, "  node [shape=record];\n\n");
    
    fprintf(out, "  root [label=\"HAMT\\ncount=%u\\nrefs=%u\"];\n", m->count, m->ref_count);
    
    if (m->root) {
        fprintf(out, "}\n");
    } else {
        fprintf(out, "}\n");
    }
}
