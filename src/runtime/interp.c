#include "interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Create a new environment */
Env *env_new(Arena *a, Env *parent) {
    Env *e = (Env *)arena_alloc(a, sizeof(Env));
    e->parent = parent;
    e->names = NULL;
    e->values = NULL;
    e->macros = NULL;
    e->count = 0;
    e->cap = 0;
    e->arena = a;
    return e;
}

/* Look up a symbol in the environment (value namespace) */
Form *env_lookup(Env *e, const Symbol *name) {
    for (Env *cur = e; cur != NULL; cur = cur->parent) {
        for (uint32_t i = 0; i < cur->count; i++) {
            if (cur->names[i] == name) {
                return cur->values[i];
            }
        }
    }
    return NULL;
}

/* Look up a macro in the environment */
MacroDef *env_lookup_macro(Env *e, const Symbol *name) {
    for (Env *cur = e; cur != NULL; cur = cur->parent) {
        if (cur->macros) {
            for (uint32_t i = 0; i < cur->count; i++) {
                if (cur->macros[i] && cur->macros[i]->params && cur->macros[i]->params->as.list.items[0]->as.sym == name) {
                    /* Simplified: assume first param name matches (need proper lookup) */
                    /* For now, just return the first macro with matching name */
                    if (cur->macros[i]->params && cur->macros[i]->params->tag == F_LIST && 
                        cur->macros[i]->params->as.list.len > 0) {
                        Form *first_param = cur->macros[i]->params->as.list.items[0];
                        if (first_param->tag == F_SYM && first_param->as.sym == name) {
                            return cur->macros[i];
                        }
                    }
                }
            }
        }
    }
    return NULL;
}

/* Grow the environment if needed */
static void env_grow(Env *e) {
    if (e->count >= e->cap) {
        uint32_t new_cap = e->cap ? e->cap * 2 : 8;
        e->names = (const Symbol **)arena_alloc(e->arena, new_cap * sizeof(const Symbol *));
        e->values = (Form **)arena_alloc(e->arena, new_cap * sizeof(Form *));
        e->macros = (MacroDef **)arena_alloc(e->arena, new_cap * sizeof(MacroDef *));
        for (uint32_t i = 0; i < e->count; i++) {
            e->names[i] = e->names[i];
            e->values[i] = e->values[i];
            e->macros[i] = e->macros[i];
        }
        e->cap = new_cap;
    }
}

/* Bind a symbol to a value in the environment */
void env_bind(Env *e, const Symbol *name, Form *value) {
    env_grow(e);
    e->names[e->count] = name;
    e->values[e->count] = value;
    e->macros[e->count] = NULL;
    e->count++;
}

/* Bind a macro in the environment */
void env_bind_macro(Env *e, const Symbol *name, MacroDef *macro) {
    env_grow(e);
    e->names[e->count] = name;
    e->values[e->count] = NULL;
    e->macros[e->count] = macro;
    e->count++;
}

static Form *quasiquote_expand(Env *macro_env, Form *f);

/* Evaluate a Form in an environment */
Form *interp_eval(Env *e, Form *f) {
    switch (f->tag) {
        case F_NIL:
        case F_BOOL:
        case F_INT:
        case F_FLOAT:
        case F_STR:
        case F_KEYWORD:
            /* Literals evaluate to themselves */
            return f;
        case F_QUOTE: {
            /* (quote x) evaluates to x without evaluating x */
            if (f->as.list.len == 1) {
                return f->as.list.items[0];
            }
            /* Invalid quote form - return as-is */
            return f;
        }
        case F_QUASIQUOTE:
            /* Phase 6: expand the quasiquoted form and evaluate the result */
            return interp_eval(e, quasiquote_expand(e, f->as.list.items[0]));
        case F_UNQUOTE:
        case F_UNQUOTE_SPLICING:
            /* unquote/unquote-splicing outside quasiquote: evaluate inner form */
            return interp_eval(e, f->as.list.items[0]);
        case F_SYM: {
            /* Look up symbol in environment */
            Form *value = env_lookup(e, f->as.sym);
            if (value) {
                return value;
            }
            /* Symbol not found - return as-is (will be resolved later) */
            return f;
        }
        case F_LIST: {
            /* Evaluate list: first element is function, rest are args */
            if (f->as.list.len == 0) {
                return form_nil(e->arena, f->span);
            }
            /* Check if first element is a macro */
            Form *first = f->as.list.items[0];
            if (first->tag == F_SYM) {
                MacroDef *macro = env_lookup_macro(e, first->as.sym);
                if (macro) {
                    /* This is a macro call - expand it */
                    /* For now, return the macro body as-is (simplified) */
                    return macro->body;
                }
            }
            /* Evaluate each element */
            Form **items = (Form **)arena_alloc(e->arena, f->as.list.len * sizeof(Form *));
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                items[i] = interp_eval(e, f->as.list.items[i]);
            }
            return form_list(e->arena, f->span, items, f->as.list.len);
        }
        case F_VEC:
        case F_MAP:
        case F_SET:
        case F_MAP_LITERAL:
        case F_SET_LITERAL:
        case F_CBLOCK:
        case F_TYPE_ANN:
        /* CT0: Contract type annotations evaluate to themselves */
        case F_CONTRACT_TYPE:
        /* INT-1: Reader conditionals are resolved at elab time */
        case F_READER_COND:
            /* Vectors, maps, sets, C blocks, and type annotations evaluate to themselves */
            return f;
        /* RR3: Range literal variable annotation -- eval inner form (shadowing warned by elaborator) */
        case F_RANGE_VAR:
            return interp_eval(e, f->as.list.items[1]);
    }
    return f;
}

/* Phase 6: Expand quasiquote forms recursively */
static Form *quasiquote_expand(Env *macro_env, Form *f) {
    switch (f->tag) {
        case F_NIL:
        case F_BOOL:
        case F_INT:
        case F_FLOAT:
        case F_STR:
        case F_KEYWORD:
        case F_SYM:
            /* Literals and symbols: (quasiquote x) -> (quote x) */
            return form_quote(macro_env->arena, f->span, f);
        case F_QUOTE:
            /* (quasiquote (quote x)) -> (quote (quasiquote x)) */
            /* This handles nested quote inside quasiquote */
            {
                Form *quoted_form = f->as.list.items[0];
                Form *expanded_quoted = quasiquote_expand(macro_env, quoted_form);
                return form_quote(macro_env->arena, f->span, expanded_quoted);
            }
        case F_UNQUOTE:
            /* (quasiquote (unquote x)) -> x (just return the inner form) */
            return f->as.list.items[0];
        case F_UNQUOTE_SPLICING:
            /* (quasiquote (unquote-splicing x)) -> x (for phase 6, same as unquote) */
            return f->as.list.items[0];
        case F_LIST:
            /* Check if this is a quasiquote form: (quasiquote expr) */
            if (f->as.list.len == 2 && f->as.list.items[0]->tag == F_SYM) {
                /* This might be (quasiquote expr) as a list form */
                /* But quasiquote is already a F_QUASIQUOTE tag, not a list */
                /* So we shouldn't get here for direct quasiquote forms */
            }
            
            /* Process list items - expand each element */
            Form **new_items = (Form **)arena_alloc(macro_env->arena, f->as.list.len * sizeof(Form *));
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                new_items[i] = quasiquote_expand(macro_env, f->as.list.items[i]);
            }
            return form_list(macro_env->arena, f->span, new_items, f->as.list.len);
        case F_VEC:
        case F_MAP:
        case F_SET:
        case F_MAP_LITERAL:
        case F_SET_LITERAL:
            {
                Form **new_items = (Form **)arena_alloc(macro_env->arena, f->as.list.len * sizeof(Form *));
                for (uint32_t i = 0; i < f->as.list.len; i++) {
                    new_items[i] = quasiquote_expand(macro_env, f->as.list.items[i]);
                }
                if (f->tag == F_MAP) return form_map(macro_env->arena, f->span, new_items, f->as.list.len);
                if (f->tag == F_SET) return form_set(macro_env->arena, f->span, new_items, f->as.list.len);
                if (f->tag == F_MAP_LITERAL) return form_map_literal(macro_env->arena, f->span, new_items, f->as.list.len);
                if (f->tag == F_SET_LITERAL) return form_set_literal(macro_env->arena, f->span, new_items, f->as.list.len);
                return form_vec(macro_env->arena, f->span, new_items, f->as.list.len);
            }
        case F_CBLOCK:
        case F_TYPE_ANN:
        /* CT0: Contract type annotations are passed through in quasiquote */
        case F_CONTRACT_TYPE:
        /* INT-1: Reader conditionals are passed through in quasiquote */
        case F_READER_COND:
        /* RR3: Range literal variable annotation -- passed through in quasiquote */
        case F_RANGE_VAR:
            return f;
        case F_QUASIQUOTE:
            /* This is the main case: expand the quasiquoted form */
            return quasiquote_expand(macro_env, f->as.list.items[0]);
    }
    return f;
}

/* Expand macros in a Form recursively */
Form *macro_expand(Env *macro_env, Form *f, int *depth) {
    if (*depth >= MAX_MACRO_DEPTH) {
        fprintf(stderr, "tur: maximum macro expansion depth exceeded\n");
        return f;
    }
    (*depth)++;
    
    switch (f->tag) {
        case F_SYM:
        case F_NIL:
        case F_BOOL:
        case F_INT:
        case F_FLOAT:
        case F_STR:
        case F_KEYWORD:
        case F_CBLOCK:
        case F_QUOTE:
        case F_TYPE_ANN:
        /* CT0: Contract type annotations don't expand */
        case F_CONTRACT_TYPE:
        /* INT-1: Reader conditionals don't expand */
        case F_READER_COND:
        /* RR3: Range literal variable annotation -- don't expand, eval handles it */
        case F_RANGE_VAR:
            /* Atoms, quote forms, and type annotations don't expand */
            return f;
        case F_QUASIQUOTE:
        case F_UNQUOTE:
        case F_UNQUOTE_SPLICING:
            /* Expand quasiquote forms */
            return quasiquote_expand(macro_env, f);
        case F_LIST: {
            if (f->as.list.len == 0) {
                return f;
            }
            /* Check if first element is a macro */
            Form *first = f->as.list.items[0];
            if (first->tag == F_SYM) {
                MacroDef *macro = env_lookup_macro(macro_env, first->as.sym);
                if (macro) {
                    /* Expand the macro */
                    /* Bind parameters to arguments */
                    Env *macro_scope = env_new(macro_env->arena, macro->env);
                    
                    /* Extract parameter names */
                    FormList *params = &macro->params->as.list;
                    /* Extract arguments (rest of list) */
                    uint32_t n_args = f->as.list.len - 1;
                    
                    /* Bind parameters to arguments */
                    for (uint32_t i = 0; i < params->len && i < n_args; i++) {
                        Form *param_name = params->items[i];
                        Form *arg = f->as.list.items[1 + i];
                        if (param_name->tag == F_SYM) {
                            env_bind(macro_scope, param_name->as.sym, arg);
                        }
                    }
                    
                    /* Evaluate macro body in the macro scope */
                    Form *expanded = interp_eval(macro_scope, macro->body);
                    
                    /* Recursively expand the result */
                    Form *result = macro_expand(macro_env, expanded, depth);
                    (*depth)--;
                    return result;
                }
            }
            /* Not a macro - expand children */
            Form **items = (Form **)arena_alloc(macro_env->arena, f->as.list.len * sizeof(Form *));
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                items[i] = macro_expand(macro_env, f->as.list.items[i], depth);
            }
            if (f->as.list.items == items) {
                return f; /* No changes */
            }
            return form_list(macro_env->arena, f->span, items, f->as.list.len);
        }
        case F_VEC:
        case F_MAP:
        case F_SET:
        case F_MAP_LITERAL:
        case F_SET_LITERAL: {
            Form **items = (Form **)arena_alloc(macro_env->arena, f->as.list.len * sizeof(Form *));
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                items[i] = macro_expand(macro_env, f->as.list.items[i], depth);
            }
            if (f->as.list.items == items) {
                return f;
            }
            if (f->tag == F_MAP) return form_map(macro_env->arena, f->span, items, f->as.list.len);
            if (f->tag == F_SET) return form_set(macro_env->arena, f->span, items, f->as.list.len);
            if (f->tag == F_MAP_LITERAL) return form_map_literal(macro_env->arena, f->span, items, f->as.list.len);
            if (f->tag == F_SET_LITERAL) return form_set_literal(macro_env->arena, f->span, items, f->as.list.len);
            return form_vec(macro_env->arena, f->span, items, f->as.list.len);
        }
    }
    return f;
}
