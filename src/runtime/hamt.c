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

uint64_t tur_hamt_hash_xxh64(const void *data, size_t len) {
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

uint64_t tur_hamt_hash_str(const char *str) {
    return tur_hamt_hash_xxh64(str, strlen(str));
}

uint64_t tur_hamt_hash_ptr(void *ptr) {
    return (uint64_t)(uintptr_t)ptr;
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
HamtNode *tur_hamt_node_alloc(size_t size) {
    HamtNode *n = (HamtNode *)hamt_malloc(size);
    n->ref_count = 1;
    return n;
}

/* Retain a node (increment ref count) */
void tur_hamt_node_retain(HamtNode *n) {
    if (n) {
        n->ref_count++;
    }
}

/* Release a node (decrement ref count; free if reaches zero) */
void tur_hamt_node_release(HamtNode *n);

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
    HamtNode *n = tur_hamt_node_alloc(size);
    n->type = HAMT_NODE_BITMAP;
    n->as.bitmap.bitmap = bitmap;
    return n;
}

static HamtNode *array_node_create(void) {
    HamtNode *n = tur_hamt_node_alloc(sizeof(HamtNode));
    n->type = HAMT_NODE_ARRAY;
    for (uint32_t i = 0; i < HAMT_SLOTS_PER_LEVEL; i++) {
        n->as.array.children[i] = NULL;
    }
    return n;
}

static HamtNode *collision_node_create(uint32_t hash_prefix,
                                       uint64_t hash, void *key, void *val) {
    HamtNode *n = tur_hamt_node_alloc(sizeof(HamtNode));
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
                if (bitmap & (UINT32_C(1) << i)) {
                    uint32_t child_idx = 0;
                    uint32_t mask = 1;
                    for (uint32_t j = 0; j < i; j++, mask <<= 1) {
                        if (bitmap & mask) child_idx++;
                    }
                    tur_hamt_node_release(n->as.bitmap.children[child_idx]);
                }
            }
            break;
        }
        case HAMT_NODE_ARRAY: {
            for (uint32_t i = 0; i < HAMT_SLOTS_PER_LEVEL; i++) {
                tur_hamt_node_release(n->as.array.children[i]);
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

void tur_hamt_node_release(HamtNode *n) {
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

    HamtNode *dst = tur_hamt_node_alloc(size);
    dst->type = HAMT_NODE_BITMAP;
    dst->as.bitmap.bitmap = bitmap;

    for (uint32_t i = 0; i < child_count; i++) {
        dst->as.bitmap.children[i] = src->as.bitmap.children[i];
        if (dst->as.bitmap.children[i]) {
            tur_hamt_node_retain(dst->as.bitmap.children[i]);
        }
    }

    return dst;
}

static HamtNode *array_node_copy(HamtNode *src) {
    HamtNode *dst = tur_hamt_node_alloc(sizeof(HamtNode));
    dst->type = HAMT_NODE_ARRAY;

    for (uint32_t i = 0; i < HAMT_SLOTS_PER_LEVEL; i++) {
        dst->as.array.children[i] = src->as.array.children[i];
        if (dst->as.array.children[i]) {
            tur_hamt_node_retain(dst->as.array.children[i]);
        }
    }

    return dst;
}

static HamtNode *collision_node_copy(HamtNode *src) {
    HamtNode *dst = tur_hamt_node_alloc(sizeof(HamtNode));
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

            bool has_child = (bitmap & (UINT32_C(1) << chunk)) != 0;

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
                /* bitmap_node_copy retained children[idx]; release it before overwriting. */
                tur_hamt_node_release(new_node->as.bitmap.children[idx]);
                /* new_child has ref_count=1 from node_insert; new_node is the sole owner. */
                new_node->as.bitmap.children[idx] = new_child;
                return new_node;
            } else {
                uint32_t new_bitmap = bitmap | (UINT32_C(1) << chunk);
                HamtNode *new_node = bitmap_node_create(new_bitmap);

                uint32_t new_idx = 0;
                uint32_t pos = 0;
                for (uint32_t i = 0; i < 32; i++) {
                    if (bitmap & (UINT32_C(1) << i)) {
                        new_node->as.bitmap.children[new_idx] = n->as.bitmap.children[pos];
                        tur_hamt_node_retain(new_node->as.bitmap.children[new_idx]);
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
            /* array_node_copy retained children[chunk]; release it before overwriting. */
            tur_hamt_node_release(new_node->as.array.children[chunk]);
            /* new_child has ref_count=1 from node_insert; new_node is the sole owner. */
            new_node->as.array.children[chunk] = new_child;

            return new_node;
        }

        case HAMT_NODE_COLLISION: {
            /* Use the actual hash chunk of existing entries at the CURRENT level,
             * not the stored hash_prefix (which was set at the creation level). */
            uint32_t existing_chunk = hash_chunk(n->as.collision.entries->hash, level);

            if (existing_chunk != chunk) {
                /* Different slot at this level — expand into a bitmap node. */
                uint32_t new_bitmap = (UINT32_C(1) << existing_chunk) | (UINT32_C(1) << chunk);

                HamtNode *new_node = bitmap_node_create(new_bitmap);

                uint32_t idx = 0;
                uint32_t mask = 1;
                for (uint32_t i = 0; i < existing_chunk; i++, mask <<= 1) {
                    if (new_bitmap & mask) idx++;
                }
                new_node->as.bitmap.children[idx] = n;
                tur_hamt_node_retain(n);

                idx = 0;
                mask = 1;
                for (uint32_t i = 0; i < chunk; i++, mask <<= 1) {
                    if (new_bitmap & mask) idx++;
                }
                /* collision_node_create returns ref_count=1; new_node is the sole owner. */
                new_node->as.bitmap.children[idx] = collision_node_create(chunk, hash, key, val);

                return new_node;
            }

            /* Same chunk at this level — check for exact key match (update). */
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

            /* Same chunk, no key match.  Check whether this is a true hash collision
             * (all existing entries share the same full 64-bit hash) or just a
             * chunk collision at this level that must be split deeper. */
            if (n->as.collision.entries->hash == hash) {
                /* True hash collision — add new entry to the chain. */
                HamtNode *new_node = collision_node_copy(n);
                HamtEntry *new_entry = (HamtEntry *)hamt_malloc(sizeof(HamtEntry));
                new_entry->hash = hash;
                new_entry->key = key;
                new_entry->val = val;
                new_entry->next = new_node->as.collision.entries;
                new_node->as.collision.entries = new_entry;
                return new_node;
            }

            /* Different full hash but same chunk at this level — must diverge at
             * level+1.  Recurse into this collision node one level deeper and wrap
             * the result in a single-slot bitmap so the level invariant is maintained. */
            HamtNode *inner = node_insert(n, hash, key, val, level + 1);
            uint32_t wrapper_bitmap = (1u << chunk);
            HamtNode *wrapper = bitmap_node_create(wrapper_bitmap);
            /* inner has ref_count=1 from the recursive call; wrapper is the sole owner. */
            wrapper->as.bitmap.children[0] = inner;
            return wrapper;
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

            if (!(bitmap & (UINT32_C(1) << chunk))) {
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
                tur_hamt_node_release(n->as.bitmap.children[idx]);
                uint32_t new_bitmap = bitmap & ~(UINT32_C(1) << chunk);
                uint32_t new_child_count = popcount32(new_bitmap);

                if (new_child_count == 0) {
                    return NULL;
                }

                HamtNode *new_node = bitmap_node_create(new_bitmap);
                uint32_t old_idx = 0;
                uint32_t new_idx = 0;
                for (uint32_t i = 0; i < 32; i++) {
                    if (bitmap & (UINT32_C(1) << i)) {
                        if (i == chunk) {
                            old_idx++;
                        } else {
                            new_node->as.bitmap.children[new_idx] = n->as.bitmap.children[old_idx];
                            tur_hamt_node_retain(new_node->as.bitmap.children[new_idx]);
                            new_idx++;
                            old_idx++;
                        }
                    }
                }
                return new_node;
            } else {
                HamtNode *new_node = bitmap_node_copy(n);
                /* bitmap_node_copy retained children[idx]; release before overwriting. */
                tur_hamt_node_release(new_node->as.bitmap.children[idx]);
                /* new_child has ref_count=1 from node_delete; new_node is the sole owner. */
                new_node->as.bitmap.children[idx] = new_child;
                return new_node;
            }
        }

        case HAMT_NODE_ARRAY: {
            HamtNode *new_child = node_delete(n->as.array.children[chunk], hash, key, level + 1);

            if (new_child == n->as.array.children[chunk]) {
                return n;
            }

            if (!new_child) {
                tur_hamt_node_release(n->as.array.children[chunk]);
                HamtNode *new_node = array_node_create();
                for (uint32_t i = 0; i < HAMT_SLOTS_PER_LEVEL; i++) {
                    if (i == chunk) {
                        new_node->as.array.children[i] = NULL;
                    } else {
                        new_node->as.array.children[i] = n->as.array.children[i];
                        if (new_node->as.array.children[i]) {
                            tur_hamt_node_retain(new_node->as.array.children[i]);
                        }
                    }
                }
                return new_node;
            } else {
                HamtNode *new_node = array_node_copy(n);
                /* array_node_copy retained children[chunk]; release before overwriting. */
                tur_hamt_node_release(new_node->as.array.children[chunk]);
                /* new_child has ref_count=1 from node_delete; new_node is the sole owner. */
                new_node->as.array.children[chunk] = new_child;
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

            if (!(bitmap & (UINT32_C(1) << chunk))) {
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

            if (!(bitmap & (UINT32_C(1) << chunk))) {
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

Hamt *tur_hamt_new(void) {
    return hamt_alloc_empty();
}

void tur_hamt_free(Hamt *m) {
    if (!m || --m->ref_count != 0) {
        return;
    }

    if (m->root) {
        tur_hamt_node_release(m->root);
    }
    free(m);
}

Hamt *tur_hamt_retain(Hamt *m) {
    if (m) {
        m->ref_count++;
    }
    return m;
}

uint32_t tur_hamt_count(Hamt *m) {
    if (!m) return 0;
    return m->count;
}

Hamt *tur_hamt_set(Hamt *m, uint64_t hash, void *key, void *val) {
    if (!m) {
        m = hamt_alloc_empty();
    }

    bool key_exists = tur_hamt_has(m, hash, key);
    HamtNode *new_root = node_insert(m->root, hash, key, val, 0);

    if (new_root == m->root) {
        return m;
    }

    Hamt *new_map = hamt_alloc_empty();
    /* new_root has ref_count=1 from node_insert; new_map is the sole owner. */
    new_map->root = new_root;

    if (key_exists) {
        new_map->count = m->count;
    } else {
        new_map->count = m->count + 1;
    }

    return new_map;
}

Hamt *tur_hamt_del(Hamt *m, uint64_t hash, void *key) {
    if (!m) {
        return m;
    }

    if (!tur_hamt_has(m, hash, key)) {
        return m;
    }

    HamtNode *new_root = node_delete(m->root, hash, key, 0);

    if (new_root == m->root) {
        return m;
    }

    Hamt *new_map = hamt_alloc_empty();
    new_map->root = new_root;
    tur_hamt_node_retain(new_root);
    new_map->count = m->count - 1;

    return new_map;
}

bool tur_hamt_has(Hamt *m, uint64_t hash, void *key) {
    if (!m) return false;
    return node_has(m->root, hash, key, 0);
}

void *tur_hamt_get(Hamt *m, uint64_t hash, void *key) {
    if (!m) return NULL;
    return node_get(m->root, hash, key, 0);
}

Hamt *tur_hamt_merge(Hamt *a, Hamt *b) {
    if (!a) return tur_hamt_retain(b);
    if (!b) return tur_hamt_retain(a);

    Hamt *result = tur_hamt_retain(a);

    HamtIter iter;
    tur_hamt_iter_init(&iter, b);

    uint64_t hash;
    void *key, *val;
    while (tur_hamt_iter_next(&iter, &hash, &key, &val)) {
        Hamt *old = result;
        result = tur_hamt_set(result, hash, key, val);
        if (old != result) {
            tur_hamt_free(old);
        }
    }

    tur_hamt_iter_free(&iter);
    return result;
}

Hamt *tur_hamt_map(Hamt *m, void *(*fn)(void *val, void *ctx), void *ctx) {
    Hamt *result = tur_hamt_new();

    HamtIter iter;
    tur_hamt_iter_init(&iter, m);

    uint64_t hash;
    void *key, *val;
    while (tur_hamt_iter_next(&iter, &hash, &key, &val)) {
        void *new_val = fn(val, ctx);
        Hamt *old = result;
        result = tur_hamt_set(result, hash, key, new_val);
        if (old != result) tur_hamt_free(old);
    }

    tur_hamt_iter_free(&iter);
    return result;
}

Hamt *tur_hamt_filter(Hamt *m, bool (*fn)(void *key, void *val, void *ctx), void *ctx) {
    Hamt *result = tur_hamt_new();

    HamtIter iter;
    tur_hamt_iter_init(&iter, m);

    uint64_t hash;
    void *key, *val;
    while (tur_hamt_iter_next(&iter, &hash, &key, &val)) {
        if (fn(key, val, ctx)) {
            Hamt *old = result;
            result = tur_hamt_set(result, hash, key, val);
            if (old != result) tur_hamt_free(old);
        }
    }

    tur_hamt_iter_free(&iter);
    return result;
}

void *tur_hamt_reduce(Hamt *m, void *(*fn)(void *acc, void *key, void *val, void *ctx), void *init, void *ctx) {
    void *acc = init;

    HamtIter iter;
    tur_hamt_iter_init(&iter, m);

    uint64_t hash;
    void *key, *val;
    while (tur_hamt_iter_next(&iter, &hash, &key, &val)) {
        acc = fn(acc, key, val, ctx);
    }

    tur_hamt_iter_free(&iter);
    return acc;
}

Hamt *tur_hamt_merge_with(Hamt *a, Hamt *b, void *(*fn)(void *va, void *vb, void *ctx), void *ctx) {
    if (!a) return tur_hamt_retain(b);
    if (!b) return tur_hamt_retain(a);

    Hamt *result = tur_hamt_retain(a);

    HamtIter iter;
    tur_hamt_iter_init(&iter, b);

    uint64_t hash;
    void *key, *val_b;
    while (tur_hamt_iter_next(&iter, &hash, &key, &val_b)) {
        void *val_a = tur_hamt_get(result, hash, key);
        void *new_val = (val_a != NULL) ? fn(val_a, val_b, ctx) : val_b;
        Hamt *old = result;
        result = tur_hamt_set(result, hash, key, new_val);
        if (old != result) tur_hamt_free(old);
    }

    tur_hamt_iter_free(&iter);
    return result;
}


/* ============================================================================
 * Iteration Implementation
 * ============================================================================
 */

void tur_hamt_iter_init(HamtIter *iter, Hamt *m) {
    iter->map = m;
    iter->stack = NULL;
    iter->cidx = NULL;
    iter->stack_cap = 0;
    iter->stack_len = 0;
    iter->coll_entry = NULL;
    iter->done = (m == NULL || m->root == NULL);

    if (!iter->done) {
        iter->stack_cap = 16;
        iter->stack = (HamtNode **)hamt_malloc(sizeof(HamtNode *) * iter->stack_cap);
        iter->cidx  = (uint32_t *)hamt_malloc(sizeof(uint32_t) * iter->stack_cap);
        iter->stack[0] = m->root;
        iter->cidx[0]  = 0;
        iter->stack_len = 1;
    }
}

void tur_hamt_iter_free(HamtIter *iter) {
    if (iter->stack) {
        free(iter->stack);
        iter->stack = NULL;
    }
    if (iter->cidx) {
        free(iter->cidx);
        iter->cidx = NULL;
    }
    iter->stack_cap = 0;
    iter->stack_len = 0;
    iter->done = true;
}

bool tur_hamt_iter_next(HamtIter *iter, uint64_t *hash_out, void **key_out, void **val_out) {
    while (!iter->done) {
        /* Resume a collision chain from a previous call. */
        if (iter->coll_entry) {
            *hash_out = iter->coll_entry->hash;
            *key_out = iter->coll_entry->key;
            *val_out = iter->coll_entry->val;
            iter->coll_entry = iter->coll_entry->next;
            return true;
        }

        if (iter->stack_len == 0) {
            iter->done = true;
            break;
        }

        size_t top = iter->stack_len - 1;
        HamtNode *n = iter->stack[top];

        switch (n->type) {
            case HAMT_NODE_COLLISION: {
                /* Pop the collision node and start (or resume) its chain. */
                iter->stack_len--;
                iter->coll_entry = n->as.collision.entries;
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
                uint32_t child_count = popcount32(n->as.bitmap.bitmap);
                if (iter->cidx[top] < child_count) {
                    /* Push next child of this bitmap node. */
                    HamtNode *child = n->as.bitmap.children[iter->cidx[top]++];
                    if (iter->stack_len >= iter->stack_cap) {
                        iter->stack_cap *= 2;
                        iter->stack = (HamtNode **)realloc(iter->stack,
                            sizeof(HamtNode *) * iter->stack_cap);
                        iter->cidx  = (uint32_t *)realloc(iter->cidx,
                            sizeof(uint32_t) * iter->stack_cap);
                    }
                    iter->cidx[iter->stack_len] = 0;
                    iter->stack[iter->stack_len++] = child;
                } else {
                    /* All children visited — pop this bitmap node. */
                    iter->stack_len--;
                }
                break;
            }
            case HAMT_NODE_ARRAY: {
                if (iter->cidx[top] < HAMT_SLOTS_PER_LEVEL) {
                    HamtNode *child = n->as.array.children[iter->cidx[top]++];
                    if (child) {
                        if (iter->stack_len >= iter->stack_cap) {
                            iter->stack_cap *= 2;
                            iter->stack = (HamtNode **)realloc(iter->stack,
                                sizeof(HamtNode *) * iter->stack_cap);
                            iter->cidx  = (uint32_t *)realloc(iter->cidx,
                                sizeof(uint32_t) * iter->stack_cap);
                        }
                        iter->cidx[iter->stack_len] = 0;
                        iter->stack[iter->stack_len++] = child;
                    }
                } else {
                    /* All slots visited — pop this array node. */
                    iter->stack_len--;
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
                if (bitmap & (UINT32_C(1) << i)) {
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

/* Returns a heap-allocated string representing the HAMT as "{k1->v1, k2->v2, ...}".
 * Key and value are shown as pointer addresses. Caller must free. */
char *tur_hamt_show(Hamt *m) {
    if (!m || m->count == 0) {
        char *s = (char *)malloc(3);
        s[0] = '{'; s[1] = '}'; s[2] = '\0';
        return s;
    }
    /* Each entry: "0x%016llx->0x%016llx" = ~40 chars; plus ", " and {}. */
    size_t cap = m->count * 48 + 4;
    char *buf = (char *)malloc(cap);
    size_t pos = 0;
    buf[pos++] = '{';
    bool first = true;

    HamtIter iter;
    tur_hamt_iter_init(&iter, m);
    uint64_t hash;
    void *key, *val;
    while (tur_hamt_iter_next(&iter, &hash, &key, &val)) {
        if (!first) { buf[pos++] = ','; buf[pos++] = ' '; }
        first = false;
        int written = snprintf(buf + pos, cap - pos, "%p->%p", key, val);
        pos += (size_t)written;
    }
    tur_hamt_iter_free(&iter);
    buf[pos++] = '}';
    buf[pos] = '\0';
    return buf;
}

void tur_hamt_dump(Hamt *m, FILE *out) {
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

void tur_hamt_dump_dot(Hamt *m, FILE *out) {
    if (!m) {
        fprintf(out, "digraph HAMT {\n  rankdir=TB;\n}\n");
        return;
    }

    fprintf(out, "digraph HAMT {\n");
    fprintf(out, "  rankdir=TB;\n");
    fprintf(out, "  node [shape=record];\n\n");
    fprintf(out, "  root [label=\"HAMT|count=%u|refs=%u\"];\n", m->count, m->ref_count);

    if (!m->root) {
        fprintf(out, "}\n");
        return;
    }

    /* Emit nodes and edges with pointer-based IDs */
    /* We use a simple iterative worklist via the iter for leaf entries,
     * but for structural nodes we emit them inline during traversal. */

    /* Use a recursive helper via a local static to keep things simple */
    struct DotWork { HamtNode *node; };
    /* We'll emit via DFS using the stack manually. For simplicity, emit
     * all leaf entries by walking the iterator and emit structure note. */

    fprintf(out, "  n%p [label=\"", (void *)m->root);
    switch (m->root->type) {
        case HAMT_NODE_BITMAP:
            fprintf(out, "Bitmap|bitmap=0x%08X|children=%u",
                    m->root->as.bitmap.bitmap,
                    popcount32(m->root->as.bitmap.bitmap));
            break;
        case HAMT_NODE_ARRAY:
            fprintf(out, "Array|slots=32");
            break;
        case HAMT_NODE_COLLISION:
            fprintf(out, "Collision|prefix=0x%02X",
                    m->root->as.collision.hash_prefix);
            break;
    }
    fprintf(out, "\"];\n");
    fprintf(out, "  root -> n%p;\n", (void *)m->root);

    /* Emit entries as leaf nodes connected to root */
    HamtIter iter;
    tur_hamt_iter_init(&iter, m);
    uint64_t hash;
    void *key, *val;
    while (tur_hamt_iter_next(&iter, &hash, &key, &val)) {
        fprintf(out, "  e%016llx [label=\"{hash=0x%016llx|key=%p|val=%p}\" shape=box];\n",
                (unsigned long long)hash, (unsigned long long)hash, key, val);
        fprintf(out, "  n%p -> e%016llx;\n", (void *)m->root, (unsigned long long)hash);
    }
    tur_hamt_iter_free(&iter);

    fprintf(out, "}\n");
}

void tur_hamt_dump_dot_stderr(Hamt *m) {
    tur_hamt_dump_dot(m, stderr);
}

/* ---------------------------------------------------------------------------
 * Transient mode — mutable batch-construction wrapper
 * ---------------------------------------------------------------------------
 *
 * A transient is a mutable view over HAMT nodes. For Phase P4 v1 we use
 * a simple strategy: the transient wraps the immutable root and delegates
 * to the existing functional set/del operations but avoids allocating a
 * new Hamt shell on every operation.  The token field guards against use
 * after tur_hamt_persistent() invalidates the transient.
 */

static uint64_t g_transient_token = 1;  /* monotonically increasing */

HamtTransient *tur_hamt_transient(Hamt *m) {
    HamtTransient *t = (HamtTransient *)malloc(sizeof(HamtTransient));
    if (!t) return NULL;

    if (m && m->root) {
        t->root = m->root;
        tur_hamt_node_retain(t->root);
        t->count = m->count;
    } else {
        t->root = NULL;
        t->count = 0;
    }
    t->token = g_transient_token++;
    return t;
}

void tur_hamt_transient_set(HamtTransient *t, uint64_t hash, void *key, void *val) {
    if (!t) return;
    if (t->token == 0) {
        fprintf(stderr, "tur: transient used after persistent! (invalidated)\n");
        abort();
    }

    /* Check if key already exists in the current tree */
    bool exists = node_has(t->root, hash, key, 0);

    HamtNode *new_root = node_insert(t->root, hash, key, val, 0);
    if (new_root == t->root) return;

    /* Release old root; node_insert returned a new root with ref_count=1 */
    if (t->root) {
        tur_hamt_node_release(t->root);
    }
    t->root = new_root;
    if (!exists) {
        t->count++;
    }
}

void tur_hamt_transient_del(HamtTransient *t, uint64_t hash, void *key) {
    if (!t) return;
    if (t->token == 0) {
        fprintf(stderr, "tur: transient used after persistent! (invalidated)\n");
        abort();
    }

    if (!node_has(t->root, hash, key, 0)) return;

    HamtNode *new_root = node_delete(t->root, hash, key, 0);
    if (new_root == t->root) return;

    if (t->root) {
        tur_hamt_node_release(t->root);
    }
    t->root = new_root;
    if (new_root) {
        tur_hamt_node_retain(new_root);
    }
    t->count--;
}

Hamt *tur_hamt_persistent(HamtTransient *t) {
    if (!t) return tur_hamt_new();

    Hamt *m = hamt_alloc_empty();
    m->root = t->root;
    m->count = t->count;
    /* t already retained root; transfer ownership to m */

    /* Invalidate t so any subsequent use is detectable.  The struct is
     * deliberately left allocated (not freed) -- tur_hamt_transient_set/del
     * read t->token to abort on use-after-persistent!, which would be a
     * use-after-free if the struct were freed here. */
    t->root = NULL;
    t->count = 0;
    t->token = 0;

    return m;
}
