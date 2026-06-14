#include "env.h"
#include "eval.h"   /* TURI_DEFAULT_SANDBOX_FUEL, TURI_DEFAULT_SANDBOX_DEPTH */
#include "fiber.h"
#include "reader_macros.h"  /* RM Q#5: session-scoped reader-macro registry */
#include "spice_loader.h"   /* RP3: env owns the loaded TurSpiceImage */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>  /* getrlimit(RLIMIT_STACK) */
#endif

/* -------------------------------------------------------------------------
 * Recursion-depth guard sizing
 *
 * The depth guard in eval.c (eval_depth >= max_eval_depth) only protects
 * against a SIGSEGV if it fires *before* the native C stack overflows.
 * `eval_depth` increments once per `eval_expr` call, which is exactly the
 * count of nested eval frames on the C stack -- a faithful proxy for stack
 * nesting. Each such frame is large (`eval_expr_impl` reserves several
 * `TuriValue[MAX_EVAL_ARGS]` scratch arrays); measured at ~10 KB/frame under
 * Debug/ASan, so a ~12.5 MB stack overflows at peak `eval_depth` ~1250 --
 * far below a hardcoded 4096, which is why that guard was dead code. Derive
 * the limit from the real stack size instead, with a wide safety margin, so
 * the guard fires as a clean "recursion limit exceeded" rather than a raw
 * crash. (Release builds have smaller frames and sustain more, so sizing for
 * the Debug/ASan frame keeps both safe.)
 * ---------------------------------------------------------------------- */

/* Conservative upper bound on C-stack bytes per `eval_expr` frame.
 *
 * Post-trampoline (turi-eval-trampoline-plan, T1-T3.2b): deep *non-tail*
 * recursion is folded onto the heap work-stack (eval_drive) and no longer
 * touches this guard at all -- it is heap-bounded.  The guard now binds only the
 * *residual* C-recursion that still flows through `eval_expr`: recursion through
 * native HOFs (a callback re-enters via turi_call -> eval_apply -> eval_body_tco
 * -> eval_expr) and the few not-yet-driven expr kinds.  That HOF-re-entry path
 * is the worst case (~8 C frames per eval_depth) and is what this constant must
 * be sized against -- NOT the (now-folded) `sum-to` path the T1 value was tuned
 * for.
 *
 * T3.4 re-tune: measured (Debug+ASan, 12.5 MB stack) HOF re-entry through
 * `option-map` stack-overflows at ~1625 `eval_depth`.  9472 B/level gives
 * max_eval_depth ~811, i.e. the guard trips at a ~2x margin below that crash
 * (restoring Direction A's safety factor, which the 7168 value had eroded to
 * ~1.5x for this path).  Folded non-tail recursion is unaffected (it bypasses
 * the guard); Release builds have smaller frames and sustain more. */
#define TURI_EVAL_FRAME_BYTES   9472u   /* ~9.25 KB (T3.4: sized for HOF re-entry) */
/* Fraction of the stack we let interpreter recursion consume (the rest is
 * headroom for the base call chain and any non-eval frames within a level). */
#define TURI_EVAL_STACK_FRACTION_NUM  3u
#define TURI_EVAL_STACK_FRACTION_DEN  5u  /* 3/5 == 60% */
#define TURI_EVAL_DEPTH_MIN     256u
#define TURI_EVAL_DEPTH_FALLBACK 4096u   /* used when the stack size is unknown */

/* Compute a recursion-depth limit the native C stack can provably sustain. */
static int turi_default_max_eval_depth(void) {
#if defined(__unix__) || defined(__APPLE__)
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 &&
        rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > 0) {
        unsigned long long usable =
            (unsigned long long)rl.rlim_cur *
            TURI_EVAL_STACK_FRACTION_NUM / TURI_EVAL_STACK_FRACTION_DEN;
        unsigned long long depth = usable / TURI_EVAL_FRAME_BYTES;
        if (depth < TURI_EVAL_DEPTH_MIN) depth = TURI_EVAL_DEPTH_MIN;
        if (depth > (unsigned long long)INT_MAX) depth = (unsigned long long)INT_MAX;
        return (int)depth;
    }
#endif
    return (int)TURI_EVAL_DEPTH_FALLBACK;
}

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
    env->max_eval_depth = (uint32_t)turi_default_max_eval_depth();
    env->caps = TURI_CAP_ALL;
    ht_init(&env->globals_ht);
    /* Phase S7: initialise async scheduler state */
    turi_sched_init(env);
    /* Register async native builtins (sleep-async, with-timeout, etc.) */
    turi_async_register_builtins(env);
    /* Register eval-layer native builtins (panic?, etc.) */
    turi_eval_register_builtins(env);
    /* RM Q#5: persistent reader-macro registry for session semantics. */
    env->reader_macros = (ReaderMacroRegistry *)arena_alloc(
        &env->sym_arena, sizeof(ReaderMacroRegistry));
    reader_macros_init(env->reader_macros, &env->sym_arena);
    return env;
}

TuriEnv *turi_env_new_sandboxed(void) {
    TuriEnv *env = turi_env_new();
    if (!env) return NULL;
    env->sandboxed       = true;
    env->caps            = TURI_CAP_NONE;
    env->step_fuel_limit = TURI_DEFAULT_SANDBOX_FUEL;
    env->step_fuel       = TURI_DEFAULT_SANDBOX_FUEL;
    env->max_eval_depth  = TURI_DEFAULT_SANDBOX_DEPTH;
    return env;
}

void turi_env_free(TuriEnv *env) {
    if (!env) return;

    /* RP3: drop the spice image first. dlclose runs C-side destructors
     * (atexit handlers, etc.) so it should happen before the async
     * scheduler tears down its state -- otherwise spice destructors
     * that touch the runtime would see a half-freed env. */
    if (env->spice_image) {
        tur_spice_image_free(env->spice_image);
        env->spice_image = NULL;
    }
    /* RP5: drop any retired images that (reload) accumulated. They
     * outlived the current image so previously-installed FFI binding
     * shims could still see their name strings. Free in LIFO order
     * (most-recent first) for symmetry with how they were pushed. */
    {
        struct TurSpiceImageNode *n = env->retired_spice_images;
        while (n) {
            struct TurSpiceImageNode *next = n->next;
            if (n->image) tur_spice_image_free(n->image);
            free(n);
            n = next;
        }
        env->retired_spice_images = NULL;
    }

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
