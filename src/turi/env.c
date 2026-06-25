#include "env.h"
#include "eval.h"   /* TURI_DEFAULT_SANDBOX_FUEL, TURI_DEFAULT_SANDBOX_DEPTH */
#include "fiber.h"
#include "reader_macros.h"  /* RM Q#5: session-scoped reader-macro registry */
#include "spice_loader.h"   /* RP3: env owns the loaded TurSpiceImage */
#include "../runtime/globals.h"  /* g_interpret_mode (libturi-embed-interpret-mode-flag) */

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

/* Conservative upper bound on C-stack bytes per recursion level.
 *
 * Post-trampoline (turi-eval-trampoline-plan, T1-T3.2b): deep *non-tail*
 * recursion is folded onto the heap work-stack (eval_drive) and *tail* recursion
 * reuses a single work-stack slot (turi-cek-frame-reuse-tco-plan, F1-F4); neither
 * touches this guard -- both are heap-/O(1)-bounded.  The guard binds only the
 * *residual* C-recursion: a native HOF whose callback re-enters evaluation via
 * turi_call.  After the F4 unification that cycle is
 * turi_call -> eval_apply -> eval_apply_driven -> eval_drive_ex -> (leaf native)
 * -> turi_call -> ..., and it bumps eval_depth once per `eval_apply` (where the
 * guard now lives -- the driver no longer routes per-level through eval_expr, so
 * the guard moved off eval_expr onto eval_apply; see eval.c).
 *
 * The driver-based re-entry frame is cheaper than the old
 * eval_body_tco -> eval_expr chain (measured Debug+ASan, 12.5 MB stack: HOF
 * re-entry through `option-map` now stack-overflows far deeper than the ~1625
 * eval_depth the old path hit), so 9472 B/level -- giving max_eval_depth ~811 --
 * is comfortably conservative here: the guard trips with a large margin below
 * the crash.  Keeping the value pins the user-visible recursion ceiling where it
 * was.  Release builds have smaller frames and sustain more. */
#define TURI_EVAL_FRAME_BYTES   9472u   /* ~9.25 KB (sized for native HOF re-entry) */
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
 * Default-natives registry (libturi-per-embed-env-and-peripherals Gap 1)
 *
 * An embedder seeds a fixed set of natives once; every subsequently-created
 * env installs them automatically, so a host with N per-script envs does not
 * re-register the same table N times (and cannot forget one).
 * ---------------------------------------------------------------------- */

typedef struct DefaultNative {
    char        *name;   /* owned (strdup) */
    TuriNativeFn fn;
    void        *ud;
} DefaultNative;

static DefaultNative *g_default_natives;
static size_t         g_default_natives_count;
static size_t         g_default_natives_cap;

void turi_register_default_native(const char *name, TuriNativeFn fn, void *ud) {
    if (!name || !fn) return;
    for (size_t i = 0; i < g_default_natives_count; i++) {
        if (strcmp(g_default_natives[i].name, name) == 0) {
            g_default_natives[i].fn = fn;
            g_default_natives[i].ud = ud;
            return;
        }
    }
    if (g_default_natives_count == g_default_natives_cap) {
        size_t nc = g_default_natives_cap ? g_default_natives_cap * 2u : 8u;
        DefaultNative *grown =
            (DefaultNative *)realloc(g_default_natives, nc * sizeof(DefaultNative));
        if (!grown) return;
        g_default_natives     = grown;
        g_default_natives_cap = nc;
    }
    DefaultNative *e = &g_default_natives[g_default_natives_count++];
    e->name = strdup(name);
    e->fn   = fn;
    e->ud   = ud;
}

void turi_clear_default_natives(void) {
    for (size_t i = 0; i < g_default_natives_count; i++) free(g_default_natives[i].name);
    free(g_default_natives);
    g_default_natives       = NULL;
    g_default_natives_count = 0;
    g_default_natives_cap   = 0;
}

static void install_default_natives(TuriEnv *env) {
    for (size_t i = 0; i < g_default_natives_count; i++)
        turi_env_register_native(env, g_default_natives[i].name,
                                 g_default_natives[i].fn, g_default_natives[i].ud);
}

/* -------------------------------------------------------------------------
 * Native ud finalizers (libturi-per-embed-env-and-peripherals Gap 5)
 *
 * turi_env_register_native_ex records a (free_fn, ud) pair here; turi_env_free
 * fires them in LIFO order so a native whose ud is owned by the env (e.g. a
 * per-script object) is torn down with the env.  turi_env_reset deliberately
 * leaves them intact -- natives survive a reset, so their ud must too.
 * ---------------------------------------------------------------------- */

typedef struct NativeFinalizer {
    TuriNativeFreeFn        free_fn;
    void                   *ud;
    struct NativeFinalizer *next;
} NativeFinalizer;

/* -------------------------------------------------------------------------
 * TuriEnv lifecycle
 * ---------------------------------------------------------------------- */

TuriEnv *turi_env_new(void) {
    /* libturi-embed-interpret-mode-flag: anybody calling turi_env_new() is by
     * definition an interpreter embedder; the compiler path never builds an env
     * this way. Flip the process-global elaborator flag so registered natives
     * resolve at runtime instead of failing as "unknown function". */
    g_interpret_mode = true;
    TuriEnv *env = (TuriEnv *)calloc(1, sizeof(TuriEnv));
    if (!env) return NULL;
    arena_init(&env->sym_arena, 0);
    /* turi-env-owned-value-arena-pool-plan: value-payload pool. Init before any
     * builtin registration, which allocates native closures from it. */
    arena_init(&env->value_arena, 0);
    symtab_init(&env->st, &env->sym_arena);
    buf_init(&env->src_acc);
    env->max_eval_depth = (uint32_t)turi_default_max_eval_depth();
    env->caps = TURI_CAP_ALL;
    /* Gap 7: default to interpret mode (every turi_env_new caller is an
     * interpreter embedder; mirrors the g_interpret_mode = true above).  An
     * embedder driving compile-mode elaboration flips this with
     * turi_env_set_interpret_mode. */
    env->interpret_mode = true;
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
    /* Gap 1: install any embedder-seeded default natives last, so they can
     * override a builtin of the same name if the embedder intends to. */
    install_default_natives(env);
    return env;
}

TuriEnv *turi_env_new_with_natives(const TuriNativeSpec *specs, size_t n) {
    TuriEnv *env = turi_env_new();
    if (!env) return NULL;
    for (size_t i = 0; specs && i < n; i++) {
        if (specs[i].name && specs[i].fn)
            /* Gap 5: honour each spec's optional ud finalizer. */
            turi_env_register_native_ex(env, specs[i].name, specs[i].fn,
                                        specs[i].ud, specs[i].free_ud);
    }
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
        /* Gap 8: a borrowed (shared) image is owned by the prototype env -- do
         * not free it here, just drop the reference. */
        if (!env->spice_image_borrowed)
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

    /* Debugger Phase 2: drop any attached debugger state. */
    if (env->debugger) {
        turi_debug_disable(env);
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

    /* turi-env-owned-value-arena-pool-plan: reclaim all escaping value payloads
     * (closures, structs, captured frames/bindings, cons cells, ...) in one
     * shot, plus the process-global fallback pool the error/rejection
     * constructors use (the env adopts it on free; single-env pattern). */
    arena_free(&env->value_arena);
    turi_val_global_pool_free();

    /* Free global bindings */
    EnvBinding *b = env->globals;
    while (b) {
        EnvBinding *next = b->next;
        /* b->name points into sym_arena — do not free separately */
        free(b);
        b = next;
    }

    free(env->globals_ht.slots);

    /* Gap 4: free a module base dir we own (set via the setter). A directly
     * assigned (borrowed) path leaves module_base_dir_owned false. */
    if (env->module_base_dir_owned) {
        free((void *)env->module_base_dir);
        env->module_base_dir = NULL;
    }

    /* Gap 5: fire native ud finalizers in LIFO order, then free the nodes. */
    {
        NativeFinalizer *fin = (NativeFinalizer *)env->native_finalizers;
        while (fin) {
            NativeFinalizer *next = fin->next;
            if (fin->free_fn) fin->free_fn(fin->ud);
            free(fin);
            fin = next;
        }
        env->native_finalizers = NULL;
    }

    buf_free(&env->src_acc);
    symtab_free(&env->st);
    arena_free(&env->sym_arena);
    free(env);
}

/* -------------------------------------------------------------------------
 * Per-embed-env peripherals (libturi-per-embed-env-and-peripherals)
 * ---------------------------------------------------------------------- */

void turi_env_reset(TuriEnv *env) {
    if (!env) return;

    /* Drop accumulated REPL/eval source so the next turi_eval starts fresh. */
    env->src_acc.len    = 0;
    env->prior_toplevel = 0;

    /* Rebuild the globals list, keeping only native-closure bindings (the
     * builtins from turi_env_new plus the embedder's natives); free every
     * binding turi_eval installed (user defns/defs). The kept EnvBinding nodes
     * are reused as-is; their value/name pointers stay valid. */
    EnvBinding *keep = NULL;
    EnvBinding *b    = env->globals;
    while (b) {
        EnvBinding *next = b->next;
        if (turi_value_is_native(b->value)) {
            b->next = keep;
            keep    = b;
        } else {
            free(b);
        }
        b = next;
    }
    env->globals = keep;

    /* Rebuild the hash table over the surviving bindings. */
    free(env->globals_ht.slots);
    ht_init(&env->globals_ht);
    for (EnvBinding *k = keep; k; k = k->next) ht_insert(&env->globals_ht, k);

    /* Clear transient interpreter control / unwind state. */
    env->returning    = false; env->return_value = turi_nil();
    env->throwing     = false; env->throw_value  = turi_nil();
    env->aborting     = false; env->abort_value  = turi_nil();
    env->abort_target = NULL;  env->abort_prompt_kind = 0;
    env->panicking    = false;
    env->in_no_unwind = false;
    env->handler_stack = NULL;
    env->defer_stack   = NULL;
    env->eval_depth    = 0;
    env->catch_jmp     = NULL;
    env->last_tc_env   = NULL;
    env->defining_mod    = NULL;
    env->current_module  = NULL;
    env->reader_type   = READER_TURMERIC;

    /* Restore step fuel to the configured limit (0 == unlimited). */
    env->step_fuel = env->step_fuel_limit;
}

void turi_env_set_diag_sink(TuriEnv *env, TuriDiagSinkFn cb, void *ud) {
    if (!env) return;
    env->diag_sink    = cb;
    env->diag_sink_ud = ud;
}

void turi_env_set_module_base_dir(TuriEnv *env, const char *path) {
    if (!env) return;
    if (env->module_base_dir_owned) {
        free((void *)env->module_base_dir);
        env->module_base_dir       = NULL;
        env->module_base_dir_owned = false;
    }
    if (path) {
        char *copy = strdup(path);
        if (!copy) return;  /* leave base dir unset on OOM */
        env->module_base_dir       = copy;
        env->module_base_dir_owned = true;
    } else {
        env->module_base_dir = NULL;  /* default (".") */
    }
}

void turi_env_register_native_ex(TuriEnv *env, const char *name,
                                  TuriNativeFn fn, void *ud,
                                  TuriNativeFreeFn free_ud) {
    if (!env) return;
    /* Install the binding via the eval-layer registrar (TuriClosure is internal
     * to eval.c). */
    turi_env_register_native(env, name, fn, ud);
    /* Gap 5: record the finalizer (if any) so turi_env_free can run it. */
    if (free_ud) {
        NativeFinalizer *fin = (NativeFinalizer *)malloc(sizeof(NativeFinalizer));
        if (!fin) return;  /* the native is still registered; only the finalizer is lost on OOM */
        fin->free_fn = free_ud;
        fin->ud      = ud;
        fin->next    = (NativeFinalizer *)env->native_finalizers;
        env->native_finalizers = fin;
    }
}

void turi_env_set_interpret_mode(TuriEnv *env, bool interpret) {
    if (!env) return;
    env->interpret_mode = interpret;
}

void turi_env_set_shared_spice_image(TuriEnv *env, struct TurSpiceImage *image) {
    if (!env) return;
    /* Replace any image we currently OWN (a borrowed one is the prototype's to
     * free; just drop our reference to it). */
    if (env->spice_image && !env->spice_image_borrowed)
        tur_spice_image_free(env->spice_image);
    env->spice_image          = image;
    env->spice_image_borrowed = (image != NULL);
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
