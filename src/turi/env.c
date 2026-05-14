#include "env.h"
#include "fiber.h"

#include <stdlib.h>
#include <string.h>

TuriEnv *turi_env_new(void) {
    TuriEnv *env = (TuriEnv *)calloc(1, sizeof(TuriEnv));
    if (!env) return NULL;
    arena_init(&env->sym_arena, 0);
    symtab_init(&env->st, &env->sym_arena);
    buf_init(&env->src_acc);
    env->max_eval_depth = 4096;
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

    buf_free(&env->src_acc);
    symtab_free(&env->st);
    arena_free(&env->sym_arena);
    free(env);
}

TuriValue turi_env_get(TuriEnv *env, const char *name) {
    for (EnvBinding *b = env->globals; b; b = b->next) {
        if (strcmp(b->name, name) == 0) return b->value;
    }
    return turi_errorf("unbound variable: %s", name);
}

void turi_env_set(TuriEnv *env, const char *name, TuriValue value) {
    /* Update existing binding if present */
    for (EnvBinding *b = env->globals; b; b = b->next) {
        if (strcmp(b->name, name) == 0) {
            b->value = value;
            return;
        }
    }
    /* Create new binding */
    EnvBinding *b = (EnvBinding *)malloc(sizeof(EnvBinding));
    b->name  = name;
    b->value = value;
    b->next  = env->globals;
    env->globals = b;
}
