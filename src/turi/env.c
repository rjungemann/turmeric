#include "env.h"
#include "fiber.h"

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Global-binding hash table (open addressing, linear probing)
 * ---------------------------------------------------------------------- */

#define HT_INIT_CAP 64u

static uint32_t ht_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void ht_init(EnvHashTable *ht) {
    ht->cap   = HT_INIT_CAP;
    ht->count = 0;
    ht->slots = (EnvBinding **)calloc(HT_INIT_CAP, sizeof(EnvBinding *));
}

static void ht_insert_raw(EnvHashTable *ht, EnvBinding *b) {
    uint32_t mask = ht->cap - 1u;
    uint32_t idx  = ht_hash(b->name) & mask;
    while (ht->slots[idx]) {
        if (strcmp(ht->slots[idx]->name, b->name) == 0) {
            ht->slots[idx] = b;  /* replace existing slot pointer */
            return;
        }
        idx = (idx + 1u) & mask;
    }
    ht->slots[idx] = b;
    ht->count++;
}

static void ht_grow(EnvHashTable *ht) {
    uint32_t old_cap   = ht->cap;
    EnvBinding **old   = ht->slots;
    ht->cap   = old_cap * 2u;
    ht->count = 0;
    ht->slots = (EnvBinding **)calloc(ht->cap, sizeof(EnvBinding *));
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old[i]) ht_insert_raw(ht, old[i]);
    }
    free(old);
}

/* Insert or update the slot for b->name to point to b. */
static void ht_insert(EnvHashTable *ht, EnvBinding *b) {
    if (ht->count * 3u >= ht->cap * 2u) ht_grow(ht);
    ht_insert_raw(ht, b);
}

/* Find an EnvBinding* for name, or NULL if not present. */
static EnvBinding *ht_find(const EnvHashTable *ht, const char *name) {
    if (!ht->slots) return NULL;
    uint32_t mask = ht->cap - 1u;
    uint32_t idx  = ht_hash(name) & mask;
    for (;;) {
        EnvBinding *b = ht->slots[idx];
        if (!b) return NULL;
        if (strcmp(b->name, name) == 0) return b;
        idx = (idx + 1u) & mask;
    }
}

/* -------------------------------------------------------------------------
 * TuriEnv lifecycle
 * ---------------------------------------------------------------------- */

TuriEnv *turi_env_new(void) {
    TuriEnv *env = (TuriEnv *)calloc(1, sizeof(TuriEnv));
    if (!env) return NULL;
    arena_init(&env->sym_arena, 0);
    symtab_init(&env->st, &env->sym_arena);
    buf_init(&env->src_acc);
    env->max_eval_depth = 4096;
    ht_init(&env->globals_ht);
    /* Phase S7: initialise async scheduler state */
    turi_sched_init(env);
    /* Register async native builtins (sleep-async, with-timeout, etc.) */
    turi_async_register_builtins(env);
    return env;
}

TuriEnv *turi_env_new_sandboxed(void) {
    TuriEnv *env = turi_env_new();
    if (env) env->sandboxed = true;
    return env;
}

void turi_env_free(TuriEnv *env) {
    if (!env) return;

    /* Phase S7: free async scheduler state */
    turi_sched_free(env);

    /* Free all per-call arenas */
    ArenaNode *node = env->eval_arenas;
    while (node) {
        ArenaNode *next = node->next;
        arena_free(&node->arena);
        free(node);
        node = next;
    }

    /* Free global bindings */
    EnvBinding *b = env->globals;
    while (b) {
        EnvBinding *next = b->next;
        /* b->name points into sym_arena — do not free separately */
        free(b);
        b = next;
    }

    free(env->globals_ht.slots);

    buf_free(&env->src_acc);
    symtab_free(&env->st);
    arena_free(&env->sym_arena);
    free(env);
}

/* -------------------------------------------------------------------------
 * Global binding access
 * ---------------------------------------------------------------------- */

TuriValue turi_env_get(TuriEnv *env, const char *name) {
    EnvBinding *b = ht_find(&env->globals_ht, name);
    if (b) return b->value;
    return turi_errorf("unbound variable: %s", name);
}

void turi_env_set(TuriEnv *env, const char *name, TuriValue value) {
    /* Update existing binding in linked list if present. */
    EnvBinding *b = ht_find(&env->globals_ht, name);
    if (b) {
        b->value = value;
        return;
    }
    /* Create new binding, prepend to linked list, insert into hash table. */
    b         = (EnvBinding *)malloc(sizeof(EnvBinding));
    b->name   = name;
    b->value  = value;
    b->next   = env->globals;
    env->globals = b;
    ht_insert(&env->globals_ht, b);
}
