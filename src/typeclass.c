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

/* Look up instances for a typeclass and type arguments */
TypeClassInstance *typeclass_env_lookup_instance(const TypeClassEnv *env,
                                                  TypeClass *typeclass,
                                                  Type *type_args, uint8_t n_type_args) {
    /* For now, just return the first matching instance */
    /* A proper implementation would need to match type_args */
    for (TypeClassInstance *inst = env->instances; inst != NULL; inst = inst->next) {
        if (inst->typeclass == typeclass) {
            return inst;
        }
    }
    return NULL;
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
