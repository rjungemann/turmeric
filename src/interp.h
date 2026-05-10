#ifndef TUR_INTERP_H
#define TUR_INTERP_H

#include "forms.h"
#include "symbols.h"
#include "arena.h"

/* Forward declaration */
typedef struct Env Env;

/* The interpreter evaluates Form* to Form* at compile time.
 * Used for macro expansion (Phase 6). */

/* Macro environment - maps symbols to macro definitions */
typedef struct MacroDef {
    Form *params;      /* Parameter list (F_LIST of F_SYM) */
    Form *body;        /* Macro body (can be any Form) */
    Env *env;          /* Environment where macro was defined (for lexical scoping) */
} MacroDef;

/* Environment - maps symbols to values (for macro evaluation) */
struct Env {
    Env *parent;
    const Symbol **names;
    Form **values;
    MacroDef **macros;  /* Macros in this scope */
    uint32_t count;
    uint32_t cap;
    Arena *arena;
};

/* Create a new environment */
Env *env_new(Arena *a, Env *parent);

/* Look up a symbol in the environment */
Form *env_lookup(Env *e, const Symbol *name);

/* Look up a macro in the environment */
MacroDef *env_lookup_macro(Env *e, const Symbol *name);

/* Bind a symbol to a value in the environment */
void env_bind(Env *e, const Symbol *name, Form *value);

/* Bind a macro in the environment */
void env_bind_macro(Env *e, const Symbol *name, MacroDef *macro);

/* Evaluate a Form in an environment */
Form *interp_eval(Env *e, Form *f);

/* Expand macros in a Form (recursively until fixpoint) */
Form *macro_expand(Env *macro_env, Form *f, int *depth);

/* Maximum macro expansion depth */
#define MAX_MACRO_DEPTH 256

#endif /* TUR_INTERP_H */
