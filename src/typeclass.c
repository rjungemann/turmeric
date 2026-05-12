#include "typeclass.h"

#include <stdlib.h>
#include <string.h>

/* Initialize typeclass environment */
void typeclass_env_init(TypeClassEnv *env, Arena *arena) {
    env->arena = arena;
    env->typeclasses = NULL;
    env->instances = NULL;
}

/* Register a typeclass */
TypeClass *typeclass_env_register_typeclass(TypeClassEnv *env, const Symbol *name) {
    /* Check if already registered */
    TypeClass *existing = typeclass_env_lookup_typeclass(env, name);
    if (existing) {
        return existing;
    }
    
    TypeClass *tc = (TypeClass *)arena_alloc(env->arena, sizeof(TypeClass));
    if (!tc) return NULL;
    
    tc->name = name;
    tc->type_params = NULL;
    tc->n_type_params = 0;
    tc->methods = NULL;
    tc->n_methods = 0;
    tc->next = env->typeclasses;
    env->typeclasses = tc;
    
    return tc;
}

/* Register a typeclass instance */
TypeClassInstance *typeclass_env_register_instance(TypeClassEnv *env, TypeClass *typeclass) {
    TypeClassInstance *inst = (TypeClassInstance *)arena_alloc(env->arena, sizeof(TypeClassInstance));
    if (!inst) return NULL;
    
    inst->typeclass = typeclass;
    inst->type_args = NULL;
    inst->type_arg_syms = NULL;
    inst->n_type_args = 0;
    inst->method_impls = NULL;
    inst->n_method_impls = 0;
    inst->constraints = NULL;
    inst->n_constraints = 0;
    inst->next = env->instances;
    env->instances = inst;
    
    return inst;
}

/* Look up a typeclass by name */
TypeClass *typeclass_env_lookup_typeclass(const TypeClassEnv *env, const Symbol *name) {
    for (TypeClass *tc = env->typeclasses; tc != NULL; tc = tc->next) {
        if (tc->name == name) {
            return tc;
        }
    }
    return NULL;
}

/* Look up instances for a typeclass and type arguments.
 *
 * Phase HKT H2: Properly matches type_args by TypeKind (was "first match wins").
 * For KIND_STAR parameters, compares type_args[i].kind element-wise.
 * Falls back to the first matching typeclass if all type_args are unknown (TY_UNKNOWN).
 * This ensures no performance regression for existing KIND_STAR code paths. */
TypeClassInstance *typeclass_env_lookup_instance(const TypeClassEnv *env,
                                                  TypeClass *typeclass,
                                                  Type *type_args, uint8_t n_type_args) {
    TypeClassInstance *fallback = NULL;

    for (TypeClassInstance *inst = env->instances; inst != NULL; inst = inst->next) {
        if (inst->typeclass != typeclass) continue;

        /* Arity must match. */
        if (inst->n_type_args != n_type_args) continue;

        /* Match element-wise by TypeKind (kind erasure: only the top-level kind
         * is compared; inner types are not inspected in H2). */
        bool match = true;
        bool all_unknown = (n_type_args == 0);
        for (uint8_t i = 0; i < n_type_args; i++) {
            if (type_args[i].kind == TY_UNKNOWN) {
                /* Caller does not know the type yet; defer to fallback. */
                all_unknown = true;
                break;
            }
            if (inst->type_args[i].kind != type_args[i].kind) {
                match = false;
                break;
            }
        }
        if (all_unknown) {
            /* Record as fallback — used only when no typed match is found. */
            if (!fallback) fallback = inst;
            continue;
        }
        if (match) return inst;
    }

    return fallback;
}

/* Initialize a constraint set */
void constraint_set_init(ConstraintSet *cs) {
    cs->constraints = NULL;
    cs->n_constraints = 0;
    cs->cap_constraints = 0;
}

/* Add a constraint to a set */
void constraint_set_add(ConstraintSet *cs, TypeClass *typeclass, Type type_arg) {
    if (cs->n_constraints >= cs->cap_constraints) {
        cs->cap_constraints = cs->cap_constraints ? cs->cap_constraints * 2 : 8;
        cs->constraints = (TypeConstraint *)realloc(cs->constraints,
                                                   cs->cap_constraints * sizeof(TypeConstraint));
        if (!cs->constraints) return;
    }
    cs->constraints[cs->n_constraints].typeclass = typeclass;
    cs->constraints[cs->n_constraints].type_arg = type_arg;
    cs->n_constraints++;
}

/* Free a constraint set */
void constraint_set_free(ConstraintSet *cs) {
    free(cs->constraints);
    cs->constraints = NULL;
    cs->n_constraints = 0;
    cs->cap_constraints = 0;
}

/* Check if a type satisfies all constraints in a set */
bool constraint_set_satisfied(const ConstraintSet *cs, Type type, const TypeClassEnv *env) {
    for (uint8_t i = 0; i < cs->n_constraints; i++) {
        TypeClassInstance *inst = typeclass_env_lookup_instance(
            env, cs->constraints[i].typeclass, &type, 1);
        if (!inst) {
            return false;
        }
    }
    return true;
}

/* Solve constraints for a function call */
bool typeclass_solve_constraints(ConstraintSet *constraints, Type *arg_types, uint8_t n_args,
                                  const TypeClassEnv *env) {
    /* For each constraint, check if there's a matching instance */
    for (uint8_t i = 0; i < constraints->n_constraints; i++) {
        TypeClass *tc = constraints->constraints[i].typeclass;
        Type type_arg = constraints->constraints[i].type_arg;
        
        /* Try to find an instance for this type argument */
        TypeClassInstance *inst = typeclass_env_lookup_instance(env, tc, &type_arg, 1);
        if (!inst) {
            return false; /* No instance found */
        }
    }
    return true;
}

/* Look up an instance using a structured TypeClassDispatchKey (Phase HKT H2).
 *
 * Two-level lookup:
 *   Level 1 — constructor kind (KIND_STAR vs KIND_ARROW).
 *   Level 2 — for KIND_STAR: exact TypeKind match on type_args.
 *             for KIND_ARROW: match by typeclass only (constructor-name keying
 *             is reserved for Phase H3+).
 *
 * No performance regression for the all-KIND_STAR fast path: KIND_STAR simply
 * delegates to typeclass_env_lookup_instance().
 */
TypeClassInstance *typeclass_env_lookup_instance_by_key(const TypeClassEnv *env,
                                                         const TypeClassDispatchKey *key) {
    if (!key || !key->typeclass) return NULL;

    /* Fast path: KIND_STAR — delegate to the typed lookup. */
    if (key->constructor_kind == KIND_STAR) {
        return typeclass_env_lookup_instance(env, key->typeclass,
                                             key->type_args, key->n_type_args);
    }

    /* KIND_ARROW path: find the first instance whose first type_arg is a
     * non-primitive (struct or user-defined type, effective kind * -> *).
     * Phase H3 will add constructor-name keying for Functor, Monad, etc. */
    for (TypeClassInstance *inst = env->instances; inst != NULL; inst = inst->next) {
        if (inst->typeclass != key->typeclass) continue;
        if (inst->n_type_args != key->n_type_args) continue;
        if (inst->n_type_args > 0) {
            TypeKind tk = inst->type_args[0].kind;
            bool is_primitive = (tk == TY_INT  || tk == TY_BOOL  || tk == TY_CSTR ||
                                 tk == TY_NIL  || tk == TY_FLOAT || tk == TY_PTR_VOID);
            if (!is_primitive) return inst;
        } else {
            return inst;
        }
    }
    return NULL;
}
