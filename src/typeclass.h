#ifndef TUR_TYPECLASS_H
#define TUR_TYPECLASS_H

#include <stdint.h>
#include <stdbool.h>
#include "arena.h"
#include "symbols.h"
#include "types.h"

/* Forward declarations */
typedef struct TypeClassMethod TypeClassMethod;

/* A method in a typeclass */
struct TypeClassMethod {
    const Symbol *name;           /* Method name */
    const Symbol **param_names;   /* Parameter names */
    Type *param_types;           /* Parameter types */
    uint8_t n_params;
    Type return_type;             /* Return type */
};

/* A typeclass definition (e.g., Eq, Ord, Show) */
struct TypeClass {
    const Symbol *name;           /* Typeclass name */
    const Symbol **type_params;   /* Type parameters (e.g., [a] for Eq[a]) */
    /* Phase HKT (v2, stub): kind annotation per type parameter.
     * Always KIND_STAR in v1; NULL treated as all-KIND_STAR. */
    Kind         *type_param_kinds; /* Parallel to type_params; may be NULL in v1 */
    uint8_t n_type_params;
    TypeClassMethod *methods;    /* Methods in this typeclass */
    uint8_t n_methods;
    /* For linking */
    TypeClass *next;             /* Next typeclass in global registry */
};

/* A typeclass instance (e.g., Eq for int) */
struct TypeClassInstance {
    TypeClass *typeclass;         /* The typeclass this is an instance of */
    Type *type_args;              /* Concrete types for type parameters */
    uint8_t n_type_args;
    /* Method implementations - these are FnDef pointers */
    struct FnDef **method_impls;   /* Function definitions for each method */
    uint8_t n_method_impls;
    /* Constraints on type parameters (e.g., Eq a => Eq (List a)) */
    TypeClassInstance **constraints;
    uint8_t n_constraints;
    /* For linking */
    TypeClassInstance *next;      /* Next instance in global registry */
};

/* Typeclass environment (global registry) */
typedef struct TypeClassEnv {
    TypeClass *typeclasses;
    TypeClassInstance *instances;
} TypeClassEnv;

/* Initialize typeclass environment */
void typeclass_env_init(TypeClassEnv *env);

/* Register a typeclass */
TypeClass *typeclass_env_register_typeclass(TypeClassEnv *env, const Symbol *name);

/* Register a typeclass instance */
TypeClassInstance *typeclass_env_register_instance(TypeClassEnv *env, TypeClass *typeclass);

/* Look up a typeclass by name */
TypeClass *typeclass_env_lookup_typeclass(const TypeClassEnv *env, const Symbol *name);

/* Look up instances for a typeclass and type arguments */
TypeClassInstance *typeclass_env_lookup_instance(const TypeClassEnv *env,
                                                  TypeClass *typeclass,
                                                  Type *type_args, uint8_t n_type_args);

/* Phase HKT (v2, stub): Structured dispatch-table key.
 * In v1 the lookup is performed directly via (TypeClass*, type_args[], n_type_args).
 * This struct reserves the shape for HKT keying (constructor kind + arg types)
 * so the key representation can be promoted without changing call sites.
 * Unused in v1 — all lookups continue to go through typeclass_env_lookup_instance(). */
typedef struct TypeClassDispatchKey {
    TypeClass       *typeclass;
    Type            *type_args;
    uint8_t          n_type_args;
    /* HKT extension (reserved, always KIND_STAR in v1): */
    Kind             constructor_kind;
} TypeClassDispatchKey;

/* Constraint on a type variable */
typedef struct TypeConstraint {
    TypeClass *typeclass;
    Type type_arg;  /* The type that must satisfy the constraint */
} TypeConstraint;

/* Constraint set for a function */
typedef struct ConstraintSet {
    TypeConstraint *constraints;
    uint8_t n_constraints;
    uint8_t cap_constraints;
} ConstraintSet;

/* Initialize a constraint set */
void constraint_set_init(ConstraintSet *cs);

/* Add a constraint to a set */
void constraint_set_add(ConstraintSet *cs, TypeClass *typeclass, Type type_arg);

/* Free a constraint set */
void constraint_set_free(ConstraintSet *cs);

/* Check if a type satisfies all constraints in a set */
bool constraint_set_satisfied(const ConstraintSet *cs, Type type, const TypeClassEnv *env);

/* Solve constraints for a function call */
bool typeclass_solve_constraints(ConstraintSet *constraints, Type *arg_types, uint8_t n_args,
                                  const TypeClassEnv *env);

#endif
