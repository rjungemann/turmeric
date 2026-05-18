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
Hamt *tur_hamt_new(void);

/* Free a HAMT. Decrements the root's ref count; if it reaches zero,
 * recursively frees all nodes. Does NOT free keys or values. */
void tur_hamt_free(Hamt *m);

/* Retain a HAMT (increment ref count). Returns the same pointer. */
Hamt *tur_hamt_retain(Hamt *m);

/* Public API - Core operations */

/* Insert or update a key/value pair. Returns a new HAMT (structural sharing).
 * hash: full 64-bit hash of the key ( caller must compute this)
 * key: key pointer (HAMT stores this pointer; caller owns lifetime)
 * val: value pointer (HAMT stores this pointer; caller owns lifetime)
 * Returns: new Hamt with ref_count=1, or same Hamt if no change.
 * Note: If key already exists, value is updated (last writer wins). */
Hamt *tur_hamt_set(Hamt *m, uint64_t hash, void *key, void *val);

/* Delete a key. Returns a new HAMT (structural sharing).
 * hash: full 64-bit hash of the key
 * key: key pointer to delete
 * Returns: new Hamt with ref_count=1, or same Hamt if key was not present. */
Hamt *tur_hamt_del(Hamt *m, uint64_t hash, void *key);

/* Check if a key exists.
 * hash: full 64-bit hash of the key
 * key: key pointer to check
 * Returns: true if key exists in the map */
bool tur_hamt_has(Hamt *m, uint64_t hash, void *key);

/* Get the value for a key.
 * hash: full 64-bit hash of the key
 * key: key pointer to lookup
 * Returns: value pointer, or NULL if not found */
void *tur_hamt_get(Hamt *m, uint64_t hash, void *key);

/* Get the number of key/value pairs in the map. O(1). */
uint32_t tur_hamt_count(Hamt *m);

/* Merge two HAMTs. b wins on collision.
 * Returns: new Hamt with ref_count=1 containing all entries from both maps.
 *         If a key exists in both, the value from b is used. */
Hamt *tur_hamt_merge(Hamt *a, Hamt *b);

/* Public API - Iteration */

/* Iterator state for in-order traversal */
typedef struct {
    Hamt *map;
    HamtNode **stack;       /* Stack of nodes to visit */
    uint32_t *cidx;         /* Per-node child indices, parallel to stack */
    size_t stack_cap;      /* Allocated capacity of stack */
    size_t stack_len;      /* Current length of stack */
    HamtEntry *coll_entry; /* Current position in collision chain */
    bool done;             /* Iteration complete */
} HamtIter;

/* Initialize an iterator. Must be paired with tur_hamt_iter_free. */
void tur_hamt_iter_init(HamtIter *iter, Hamt *m);

/* Free iterator resources. Must be called when done iterating. */
void tur_hamt_iter_free(HamtIter *iter);

/* Advance iterator to next key/value pair.
 * Returns: true if a pair was found, false if iteration is complete.
 * Output parameters (only valid if return is true):
 *   - hash_out: receives the full 64-bit hash of the key
 *   - key_out: receives the key pointer
 *   - val_out: receives the value pointer */
bool tur_hamt_iter_next(HamtIter *iter, uint64_t *hash_out, void **key_out, void **val_out);

/* Public API - Debugging */

/* Dump the HAMT structure to a file for debugging.
 * Produces a human-readable representation of the node tree. */
void tur_hamt_dump(Hamt *m, FILE *out);

/* Dump the HAMT structure in DOT format for Graphviz visualization. */
void tur_hamt_dump_dot(Hamt *m, FILE *out);

/* Dump the HAMT in DOT format to stderr (convenience wrapper for Lisp bindings). */
void tur_hamt_dump_dot_stderr(Hamt *m);

/* Hash utilities - use xxHash64 as the default hash function */

/* Compute xxHash64 of a memory region. */
uint64_t tur_hamt_hash_xxh64(const void *data, size_t len);

/* Compute xxHash64 of a string (NUL-terminated). */
uint64_t tur_hamt_hash_str(const char *str);

/* Pointer hash - hash a pointer value directly. */
uint64_t tur_hamt_hash_ptr(void *ptr);

/* Higher-order operations */

/* Map: return a new HAMT with each value replaced by fn(val). */
Hamt *tur_hamt_map(Hamt *m, void *(*fn)(void *val, void *ctx), void *ctx);

/* Filter: return a new HAMT with only entries where fn(key, val, ctx) is true. */
Hamt *tur_hamt_filter(Hamt *m, bool (*fn)(void *key, void *val, void *ctx), void *ctx);

/* Reduce: fold all entries (order unspecified). Returns final accumulator. */
void *tur_hamt_reduce(Hamt *m, void *(*fn)(void *acc, void *key, void *val, void *ctx), void *init, void *ctx);

/* Merge-with: merge a and b; for duplicate keys call fn(val_a, val_b, ctx) to
 * resolve the conflict. Keys present in only one map keep their value. */
Hamt *tur_hamt_merge_with(Hamt *a, Hamt *b, void *(*fn)(void *va, void *vb, void *ctx), void *ctx);

/* Show: return heap-allocated string "{k->v, ...}" — caller must free. */
char *tur_hamt_show(Hamt *m);

/* Transient mode — mutable batch-construction wrapper.
 * A transient is created from an immutable map, allows in-place mutation
 * for bulk construction, then is sealed back into an immutable map.
 * A transient MUST NOT be used after calling tur_hamt_persistent(). */

typedef struct HamtTransient {
    HamtNode *root;      /* Mutable root node (may be NULL for empty) */
    uint32_t count;      /* Current entry count */
    uint64_t token;      /* Owner token — unique per transient to prevent aliasing */
} HamtTransient;

/* Fork a transient from an immutable map.
 * The original map is unchanged; the transient starts with the same data. */
HamtTransient *tur_hamt_transient(Hamt *m);

/* Mutate a transient: insert or update key with given hash. */
void tur_hamt_transient_set(HamtTransient *t, uint64_t hash, void *key, void *val);

/* Mutate a transient: delete key with given hash. */
void tur_hamt_transient_del(HamtTransient *t, uint64_t hash, void *key);

/* Seal transient into an immutable map.
 * Returns a new Hamt; the transient is freed and must not be used again. */
Hamt *tur_hamt_persistent(HamtTransient *t);

/* Internal allocation functions (exposed for testing) */

/* Allocate a HAMT node. Returns pointer with ref_count=1. */
HamtNode *tur_hamt_node_alloc(size_t size);

/* Retain a node (increment ref count). */
void tur_hamt_node_retain(HamtNode *n);

/* Release a node (decrement ref count; free if reaches zero). */
void tur_hamt_node_release(HamtNode *n);

#endif /* TUR_HAMT_H */
