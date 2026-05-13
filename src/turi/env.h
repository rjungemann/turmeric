#ifndef TURI_ENV_H
#define TURI_ENV_H

#include <stdbool.h>

#include "arena.h"
#include "buf.h"
#include "symbols.h"
#include "value.h"

/* A per-eval-call arena node, kept alive until TuriEnv is freed.
 * Closures may hold Expr* pointers into these arenas. */
typedef struct ArenaNode {
    Arena            arena;
    struct ArenaNode *next;
} ArenaNode;

/* A name→value binding in the global environment. */
typedef struct EnvBinding {
    const char       *name;   /* points into sym_arena (permanent) */
    TuriValue         value;
    struct EnvBinding *next;
} EnvBinding;

/* Persistent evaluation environment.
 * All per-eval arenas are kept alive here; callers must not free TuriEnv
 * while any closures from it are still referenced. */
typedef struct TuriEnv {
    Arena       sym_arena;       /* SymbolTable string storage — permanent */
    SymbolTable st;              /* Persistent symbol table */
    Buf         src_acc;         /* Accumulated prior source text */
    uint32_t    prior_toplevel;  /* Count of top-level exprs from prior evals */
    ArenaNode  *eval_arenas;     /* Linked list of per-call arenas (never freed) */
    EnvBinding *globals;         /* Global name→TuriValue map (linked list) */
    bool        sandboxed;       /* When true, I/O builtins are disabled */
    /* Return-signal state: set by EX_RETURN, cleared by function application */
    bool        returning;
    TuriValue   return_value;
} TuriEnv;

/* Create a new unrestricted environment. */
TuriEnv *turi_env_new(void);

/* Create a sandboxed environment (I/O and FFI builtins disabled). */
TuriEnv *turi_env_new_sandboxed(void);

/* Free all resources owned by env. Any closures captured from it become
 * dangling after this call. */
void turi_env_free(TuriEnv *env);

/* Look up a global binding by name.  Returns TURI_ERROR if not found. */
TuriValue turi_env_get(TuriEnv *env, const char *name);

/* Set (or replace) a global binding by name. */
void turi_env_set(TuriEnv *env, const char *name, TuriValue value);

#endif /* TURI_ENV_H */
