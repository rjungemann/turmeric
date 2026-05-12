#include "effect.h"

#include <string.h>
#include "buf.h"

/* Create an empty effect row */
EffectRow *effect_row_empty(Arena *a) {
    EffectRow *row = arena_alloc(a, sizeof(EffectRow));
    row->kind = ERK_EMPTY;
    return row;
}

/* Create a concrete effect row with a single effect */
EffectRow *effect_row_single(Arena *a, Effect *effect) {
    EffectRow *row = arena_alloc(a, sizeof(EffectRow));
    row->kind = ERK_CONCRETE;
    row->as.concrete.effects = arena_alloc(a, sizeof(Effect *));
    row->as.concrete.effects[0] = effect;
    row->as.concrete.n_effects = 1;
    return row;
}

/* Create a concrete effect row from multiple effects */
EffectRow *effect_row_concrete(Arena *a, Effect **effects, uint8_t n_effects) {
    EffectRow *row = arena_alloc(a, sizeof(EffectRow));
    row->kind = ERK_CONCRETE;
    row->as.concrete.effects = arena_alloc(a, n_effects * sizeof(Effect *));
    memcpy(row->as.concrete.effects, effects, n_effects * sizeof(Effect *));
    row->as.concrete.n_effects = n_effects;
    return row;
}

/* Create an effect row union */
EffectRow *effect_row_union(Arena *a, EffectRow *left, EffectRow *right) {
    EffectRow *row = arena_alloc(a, sizeof(EffectRow));
    row->kind = ERK_UNION;
    row->as.union_.left = left;
    row->as.union_.right = right;
    return row;
}

/* Check if an effect row is empty */
bool effect_row_is_empty(EffectRow *row) {
    if (!row) return true;
    return row->kind == ERK_EMPTY;
}

/* Helper to compare two effects by pointer (they're arena-allocated) */
static bool effect_ptr_eq(Effect *a, Effect *b) {
    return a == b;
}

/* Check if an effect row contains a specific effect */
bool effect_row_contains(EffectRow *row, Effect *effect) {
    if (!row) return false;
    
    switch (row->kind) {
        case ERK_EMPTY:
            return false;
        case ERK_CONCRETE: {
            for (uint8_t i = 0; i < row->as.concrete.n_effects; i++) {
                if (effect_ptr_eq(row->as.concrete.effects[i], effect)) {
                    return true;
                }
            }
            return false;
        }
        case ERK_VAR:
            /* Effect variables are polymorphic - we can't determine at this level */
            return false;
        case ERK_UNION:
            return effect_row_contains(row->as.union_.left, effect) ||
                   effect_row_contains(row->as.union_.right, effect);
    }
    return false;
}

/* Check if effect row r1 is a subset of r2 (r1 ⊆ r2) */
bool effect_row_is_subset(EffectRow *r1, EffectRow *r2) {
    if (!r1) return true;  /* Empty is subset of everything */
    if (!r2) return effect_row_is_empty(r1);
    
    if (r1->kind == ERK_EMPTY) return true;
    if (r2->kind == ERK_EMPTY) return false;
    
    if (r1->kind == ERK_VAR || r2->kind == ERK_VAR) {
        /* For now, we treat effect variables as unknown - can't determine subset */
        return true; /* Conservative: allow it */
    }
    
    if (r1->kind == ERK_CONCRETE && r2->kind == ERK_CONCRETE) {
        /* Check each effect in r1 is in r2 */
        for (uint8_t i = 0; i < r1->as.concrete.n_effects; i++) {
            Effect *e = r1->as.concrete.effects[i];
            if (!effect_row_contains(r2, e)) {
                return false;
            }
        }
        return true;
    }
    
    if (r1->kind == ERK_UNION) {
        return effect_row_is_subset(r1->as.union_.left, r2) &&
               effect_row_is_subset(r1->as.union_.right, r2);
    }
    
    /* Other cases: conservative */
    return true;
}

/* Check if two concrete effect rows are equal (same effects, order-independent) */
static bool effect_row_concrete_equal(EffectRow *r1, EffectRow *r2) {
    if (r1->as.concrete.n_effects != r2->as.concrete.n_effects) return false;
    
    for (uint8_t i = 0; i < r1->as.concrete.n_effects; i++) {
        Effect *e = r1->as.concrete.effects[i];
        bool found = false;
        for (uint8_t j = 0; j < r2->as.concrete.n_effects; j++) {
            if (effect_ptr_eq(e, r2->as.concrete.effects[j])) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

/* Create a union of two effect rows - merges them into a single concrete row */
EffectRow *effect_row_merge(Arena *a, EffectRow *r1, EffectRow *r2) {
    if (effect_row_is_empty(r1)) return r2;
    if (effect_row_is_empty(r2)) return r1;
    
    if (r1->kind == ERK_EMPTY) return r2;
    if (r2->kind == ERK_EMPTY) return r1;
    
    /* For now, handle the simple case: both are concrete */
    if (r1->kind == ERK_CONCRETE && r2->kind == ERK_CONCRETE) {
        /* Check if they're the same */
        if (effect_row_concrete_equal(r1, r2)) {
            return r1; /* or r2, they're equal */
        }
        
        /* Merge: collect all unique effects from both */
        Effect *all_effects[32]; /* Max effects in a row for now */
        uint8_t n_all = 0;
        
        /* Add from r1 */
        for (uint8_t i = 0; i < r1->as.concrete.n_effects; i++) {
            bool already = false;
            for (uint8_t j = 0; j < n_all; j++) {
                if (effect_ptr_eq(all_effects[j], r1->as.concrete.effects[i])) {
                    already = true;
                    break;
                }
            }
            if (!already) {
                all_effects[n_all++] = r1->as.concrete.effects[i];
            }
        }
        
        /* Add from r2 */
        for (uint8_t i = 0; i < r2->as.concrete.n_effects; i++) {
            bool already = false;
            for (uint8_t j = 0; j < n_all; j++) {
                if (effect_ptr_eq(all_effects[j], r2->as.concrete.effects[i])) {
                    already = true;
                    break;
                }
            }
            if (!already) {
                all_effects[n_all++] = r2->as.concrete.effects[i];
            }
        }
        
        return effect_row_concrete(a, all_effects, n_all);
    }
    
    /* For unions and variables, just create a union node */
    return effect_row_union(a, r1, r2);
}

/* Print an effect row for debugging */
void effect_row_print(Buf *b, EffectRow *row) {
    if (!row) {
        buf_puts(b, "<null>");
        return;
    }
    
    switch (row->kind) {
        case ERK_EMPTY:
            buf_puts(b, "{}");
            break;
        case ERK_CONCRETE:
            buf_puts(b, "{");
            for (uint8_t i = 0; i < row->as.concrete.n_effects; i++) {
                if (i > 0) buf_puts(b, ", ");
                buf_printf(b, "%s", row->as.concrete.effects[i]->name->name);
            }
            buf_puts(b, "}");
            break;
        case ERK_VAR:
            buf_printf(b, "{%s}", row->as.var.var_name->name);
            break;
        case ERK_UNION:
            buf_puts(b, "(");
            effect_row_print(b, row->as.union_.left);
            buf_puts(b, " | ");
            effect_row_print(b, row->as.union_.right);
            buf_puts(b, ")");
            break;
    }
}

/* Effect environment - global registry of effects */
EffectEnv *effect_env_new(Arena *a) {
    EffectEnv *env = arena_alloc(a, sizeof(EffectEnv));
    env->effects = NULL;
    env->n_effects = 0;
    env->cap_effects = 0;
    return env;
}

/* Register a new effect in the environment */
Effect *effect_env_register(EffectEnv *env, Arena *a, const Symbol *name,
                            const Symbol **param_names, TypeKind *param_types,
                            uint8_t n_params, TypeKind result_type) {
    /* Check if already exists */
    for (uint32_t i = 0; i < env->n_effects; i++) {
        if (env->effects[i]->name == name) {
            return env->effects[i]; /* Already registered */
        }
    }
    
    /* Allocate space if needed */
    if (env->n_effects >= env->cap_effects) {
        uint32_t new_cap = env->cap_effects ? env->cap_effects * 2 : 8;
        Effect **new_array = arena_alloc(a, new_cap * sizeof(Effect *));
        if (env->effects) {
            memcpy(new_array, env->effects, env->n_effects * sizeof(Effect *));
        }
        env->effects = new_array;
        env->cap_effects = new_cap;
    }
    
    /* Create the effect */
    Effect *effect = arena_alloc(a, sizeof(Effect));
    effect->name = name;
    effect->is_polymorphic = false; /* For now, all effects are monomorphic */
    
    /* Create the constructor (1:1 for now) */
    EffectConstructor *ctor = arena_alloc(a, sizeof(EffectConstructor));
    ctor->name = name;
    ctor->param_names = param_names;
    ctor->param_types = param_types;
    ctor->n_params = n_params;
    ctor->result_type = result_type;
    ctor->effect = effect;
    
    effect->constructor = ctor;
    
    env->effects[env->n_effects++] = effect;
    return effect;
}

/* Look up an effect by name */
Effect *effect_env_lookup(EffectEnv *env, const Symbol *name) {
    for (uint32_t i = 0; i < env->n_effects; i++) {
        if (env->effects[i]->name == name) {
            return env->effects[i];
        }
    }
    return NULL;
}

/* Check if an effect is valid (exists in environment) */
bool effect_env_contains(EffectEnv *env, const Symbol *name) {
    return effect_env_lookup(env, name) != NULL;
}

/* ---------------------------------------------------------------------------
 * Phase P19-4: EffectRowSubst — effect-row variable unification
 * --------------------------------------------------------------------------- */

EffectRowSubst *effect_row_subst_new(Arena *a) {
    EffectRowSubst *s = arena_alloc(a, sizeof(EffectRowSubst));
    s->n = 0;
    return s;
}

void effect_row_subst_bind(EffectRowSubst *subst,
                            const Symbol *var, EffectRow *row) {
    if (!subst || !var || !row) return;
    /* First-wins: do not overwrite an existing binding. */
    for (uint32_t i = 0; i < subst->n; i++) {
        if (subst->entries[i].var == var) return;
    }
    if (subst->n >= EFFECT_ROW_SUBST_CAP) return; /* silently drop when full */
    subst->entries[subst->n].var = var;
    subst->entries[subst->n].row = row;
    subst->n++;
}

EffectRow *effect_row_subst_lookup(const EffectRowSubst *subst,
                                    const Symbol *var) {
    if (!subst || !var) return NULL;
    for (uint32_t i = 0; i < subst->n; i++) {
        if (subst->entries[i].var == var)
            return subst->entries[i].row;
    }
    return NULL;
}

bool effect_row_unify(EffectRow *r1, EffectRow *r2,
                      EffectRowSubst *subst, Arena *a) {
    if (!r1 || !r2) return true; /* NULL rows unify trivially */

    /* Simplify: apply existing substitutions first. */
    if (r1->kind == ERK_VAR) {
        EffectRow *bound = effect_row_subst_lookup(subst, r1->as.var.var_name);
        if (bound) return effect_row_unify(bound, r2, subst, a);
        /* r1 is an unbound variable: bind it to r2 */
        effect_row_subst_bind(subst, r1->as.var.var_name, r2);
        return true;
    }
    if (r2->kind == ERK_VAR) {
        EffectRow *bound = effect_row_subst_lookup(subst, r2->as.var.var_name);
        if (bound) return effect_row_unify(r1, bound, subst, a);
        /* r2 is an unbound variable: bind it to r1 */
        effect_row_subst_bind(subst, r2->as.var.var_name, r1);
        return true;
    }

    /* Both concrete: succeed (we allow concrete rows to be supersets; exact
     * subset checking is done separately in effect_row_check_declared). */
    if (r1->kind == ERK_EMPTY && r2->kind == ERK_EMPTY) return true;
    if (r1->kind == ERK_EMPTY || r2->kind == ERK_EMPTY) {
        /* One empty, one non-empty: succeed — the non-empty row just means
         * the callee uses those effects at the call site. */
        return true;
    }
    /* Both concrete (or union): succeed permissively in v1. */
    (void)a;
    return true;
}

EffectRow *effect_row_apply_subst(EffectRow *row,
                                   const EffectRowSubst *subst, Arena *a) {
    if (!row) return NULL;
    switch (row->kind) {
    case ERK_EMPTY:
    case ERK_CONCRETE:
        return row; /* No variables — return unchanged. */

    case ERK_VAR: {
        EffectRow *bound = effect_row_subst_lookup(subst, row->as.var.var_name);
        if (bound) {
            /* Recursively apply substitution to the bound value. */
            return effect_row_apply_subst(bound, subst, a);
        }
        return row; /* Unbound — keep as-is. */
    }

    case ERK_UNION: {
        EffectRow *left  = effect_row_apply_subst(row->as.union_.left,  subst, a);
        EffectRow *right = effect_row_apply_subst(row->as.union_.right, subst, a);
        return effect_row_union(a, left, right);
    }
    }
    return row; /* unreachable */
}
