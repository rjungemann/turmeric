/* hamt.h - Persistent Hash Array Mapped Trie (Phase P1)
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

#ifndef TUR_HAMT_H
#define TUR_HAMT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* Forward declaration */
typedef struct HamtNode HamtNode;

/* HAMT root structure. Owned via reference counting. */
typedef struct Hamt {
    HamtNode *root;      /* Root node (NULL for empty map) */
    uint32_t count;      /* Number of key/value pairs */
    uint32_t ref_count;   /* Reference count for the root struct */
} Hamt;

/* Node types for the tagged union */
typedef enum {
    HAMT_NODE_BITMAP,   /* Sparse node: <= 32 entries, bitmap tracks populated slots */
    HAMT_NODE_ARRAY,    /* Dense node: exactly 32 entries (all slots populated) */
    HAMT_NODE_COLLISION, /* Collision node: multiple keys with same hash prefix */
} HamtNodeType;

/* A key/value entry in collision nodes */
typedef struct HamtEntry {
    uint64_t hash;        /* Full 64-bit hash of the key */
    void *key;           /* Key pointer (owned by caller; HAMT does not free) */
    void *val;           /* Value pointer (owned by caller; HAMT does not free) */
    struct HamtEntry *next; /* Next entry in collision chain */
} HamtEntry;

/* Bitmap node: sparse, <= 32 entries
 * Uses a 32-bit bitmap where bit i indicates slot i is populated.
 * Popcount(bitmap) gives the number of children. */
typedef struct {
    uint32_t bitmap;      /* Bitmask: bit i = 1 means slot i has a child */
    HamtNode *children[1]; /* Variable-length array of child pointers (size = popcount(bitmap)) */
} HamtNodeBitmap;

/* Array node: dense, exactly 32 entries
 * All 32 slots are populated. */
typedef struct {
    HamtNode *children[32]; /* All 32 slots populated */
} HamtNodeArray;

/* Collision node: keys with same 5-bit hash chunk
 * Stores a linked list of entries that all share the same hash prefix. */
typedef struct {
    uint32_t hash_prefix; /* The 5-bit hash chunk that collided */
    HamtEntry *entries;   /* Linked list of key/value entries */
} HamtNodeCollision;

/* Tagged union for node types */
struct HamtNode {
    HamtNodeType type;
    uint32_t ref_count;   /* Reference count for this node */
    union {
        HamtNodeBitmap bitmap;
        HamtNodeArray array;
        HamtNodeCollision collision;
    } as;
};

/* Public API - Lifecycle */

/* Create a new empty HAMT. Returns a heap-allocated Hamt with ref_count=1. */
Hamt *hamt_new(void);

/* Free a HAMT. Decrements the root's ref count; if it reaches zero,
 * recursively frees all nodes. Does NOT free keys or values. */
void hamt_free(Hamt *m);

/* Retain a HAMT (increment ref count). Returns the same pointer. */
Hamt *hamt_retain(Hamt *m);

/* Public API - Core operations */

/* Insert or update a key/value pair. Returns a new HAMT (structural sharing).
 * hash: full 64-bit hash of the key ( caller must compute this)
 * key: key pointer (HAMT stores this pointer; caller owns lifetime)
 * val: value pointer (HAMT stores this pointer; caller owns lifetime)
 * Returns: new Hamt with ref_count=1, or same Hamt if no change.
 * Note: If key already exists, value is updated (last writer wins). */
Hamt *hamt_set(Hamt *m, uint64_t hash, void *key, void *val);

/* Delete a key. Returns a new HAMT (structural sharing).
 * hash: full 64-bit hash of the key
 * key: key pointer to delete
 * Returns: new Hamt with ref_count=1, or same Hamt if key was not present. */
Hamt *hamt_del(Hamt *m, uint64_t hash, void *key);

/* Check if a key exists.
 * hash: full 64-bit hash of the key
 * key: key pointer to check
 * Returns: true if key exists in the map */
bool hamt_has(Hamt *m, uint64_t hash, void *key);

/* Get the value for a key.
 * hash: full 64-bit hash of the key
 * key: key pointer to lookup
 * Returns: value pointer, or NULL if not found */
void *hamt_get(Hamt *m, uint64_t hash, void *key);

/* Get the number of key/value pairs in the map. O(1). */
uint32_t hamt_count(Hamt *m);

/* Merge two HAMTs. b wins on collision.
 * Returns: new Hamt with ref_count=1 containing all entries from both maps.
 *         If a key exists in both, the value from b is used. */
Hamt *hamt_merge(Hamt *a, Hamt *b);

/* Public API - Iteration */

/* Iterator state for in-order traversal */
typedef struct {
    Hamt *map;
    HamtNode **stack;       /* Stack of nodes to visit */
    size_t stack_cap;      /* Allocated capacity of stack */
    size_t stack_len;      /* Current length of stack */
    HamtEntry *coll_entry; /* Current position in collision chain */
    uint32_t child_idx;    /* Current child index within a node */
    bool done;             /* Iteration complete */
} HamtIter;

/* Initialize an iterator. Must be paired with hamt_iter_free. */
void hamt_iter_init(HamtIter *iter, Hamt *m);

/* Free iterator resources. Must be called when done iterating. */
void hamt_iter_free(HamtIter *iter);

/* Advance iterator to next key/value pair.
 * Returns: true if a pair was found, false if iteration is complete.
 * Output parameters (only valid if return is true):
 *   - hash_out: receives the full 64-bit hash of the key
 *   - key_out: receives the key pointer
 *   - val_out: receives the value pointer */
bool hamt_iter_next(HamtIter *iter, uint64_t *hash_out, void **key_out, void **val_out);

/* Public API - Debugging */

/* Dump the HAMT structure to a file for debugging.
 * Produces a human-readable representation of the node tree. */
void hamt_dump(Hamt *m, FILE *out);

/* Dump the HAMT structure in DOT format for Graphviz visualization. */
void hamt_dump_dot(Hamt *m, FILE *out);

/* Hash utilities - use xxHash64 as the default hash function */

/* Compute xxHash64 of a memory region. */
uint64_t hamt_hash_xxh64(const void *data, size_t len);

/* Compute xxHash64 of a string (NUL-terminated). */
uint64_t hamt_hash_str(const char *str);

/* Pointer hash - hash a pointer value directly. */
static inline uint64_t hamt_hash_ptr(void *ptr) {
    return (uint64_t)(uintptr_t)ptr;
}

/* Internal allocation functions (exposed for testing) */

/* Allocate a HAMT node. Returns pointer with ref_count=1. */
HamtNode *hamt_node_alloc(size_t size);

/* Retain a node (increment ref count). */
void hamt_node_retain(HamtNode *n);

/* Release a node (decrement ref count; free if reaches zero). */
void hamt_node_release(HamtNode *n);

#endif /* TUR_HAMT_H */
