#ifndef TUR_TYPECLASS_H
#define TUR_TYPECLASS_H

#include <stdint.h>
#include <stdbool.h>
#include "arena.h"
#include "symbols.h"
#include "types.h"

/* Forward declaration for EffectRow (defined in effect.h) */
struct EffectRow;

/* Forward declarations */
typedef struct TypeClassMethod TypeClassMethod;
typedef struct TypeConstraint TypeConstraint;

/* A method in a typeclass */
struct TypeClassMethod {
    const Symbol *name;           /* Method name */
    const Symbol **param_names;   /* Parameter names */
    Type *param_types;           /* Parameter types */
    /* Phase CCL: per-parameter callable flag.  param_is_fn[j] = true means the
     * j-th parameter is a single-argument callable and should be represented as
     * tur_poly_fn_t at call sites so that capturing closures can be passed.
     * Set by parse_typeclass_method when the defclass uses [param :fn] syntax.
     * NULL means no parameters are callable (backwards-compatible default). */
    bool *param_is_fn;
    uint8_t n_params;
    Type return_type;             /* Return type */
    /* ER3: Effect-row annotation from the defclass method signature.
     * NULL if not annotated; ERK_UNRESOLVED until PASS_EFFECT_ROW_INFER resolves it. */
    struct EffectRow *effect_row;
    /* ER3: Elaborated default method body as a file-level EX_FN_DEF expression.
     * NULL if the defclass method has no default body.  Registered in the program
     * by elab_defclass so effect_check_pass validates it automatically. */
    struct Expr *default_fn_expr;
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
    /* Phase HKT-P4: file that defined this typeclass (for orphan instance check).
     * file_id mirrors Span.file_id; 0 means unknown/builtin. */
    uint16_t origin_file_id;
    /* For linking */
    TypeClass *next;             /* Next typeclass in global registry */
};

/* A typeclass instance (e.g., Eq for int) */
struct TypeClassInstance {
    TypeClass *typeclass;         /* The typeclass this is an instance of */
    Type *type_args;              /* Concrete types for type parameters */
    /* Phase HKT §1: original symbol name for each type arg (e.g. "option",
     * "vec").  Parallel to type_args; entries may be NULL for primitive types
     * that have an unambiguous C name from their TypeKind.  Used by emit.c to
     * produce distinct dictionary struct names (dict_Functor_option vs
     * dict_Functor_vec) when multiple HKT instances exist. */
    const struct Symbol **type_arg_syms;
    uint8_t n_type_args;
    /* Method implementations - these are FnDef pointers */
    struct FnDef **method_impls;   /* Function definitions for each method */
    uint8_t n_method_impls;
    /* Constraints on type parameters (e.g., Eq a => Eq (List a)) */
    TypeClassInstance **constraints;
    uint8_t n_constraints;
    /* Phase PTC1: Type parameter constraints (e.g., [Clone a, Clone b] in Clone [Pair a b])
     * These are constraints that the type parameters must satisfy.
     * Stored as TypeConstraint array for better type info tracking. */
    TypeConstraint *type_param_constraints;
    uint8_t n_type_param_constraints;
    /* Phase HKT-P4: file that defined this instance (for orphan instance check).
     * file_id mirrors Span.file_id; 0 means unknown. */
    uint16_t origin_file_id;
    /* For linking */
    TypeClassInstance *next;      /* Next instance in global registry */
};

/* Typeclass environment (global registry) */
typedef struct TypeClassEnv {
    Arena *arena;
    TypeClass *typeclasses;
    TypeClassInstance *instances;
} TypeClassEnv;

/* Initialize typeclass environment */
void typeclass_env_init(TypeClassEnv *env, Arena *arena);

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

/* Phase PTC3: Check if an instance's type parameter constraints are satisfied
 * for the given type arguments. */
bool typeclass_instance_constraints_satisfied(const TypeClassInstance *inst,
                                              Type *lookup_type_args, uint8_t n_lookup_args,
                                              const TypeClassEnv *env);

/* Phase HKT H2: Structured dispatch-table key.
 * Generalized lookup key carrying the constructor kind for two-level dispatch.
 * KIND_STAR keys fall through to typeclass_env_lookup_instance (no regression). */
typedef struct TypeClassDispatchKey {
    TypeClass       *typeclass;
    Type            *type_args;
    uint8_t          n_type_args;
    /* HKT extension: KIND_STAR for concrete types, KIND_ARROW for type constructors. */
    Kind             constructor_kind;
} TypeClassDispatchKey;

/* Phase HKT H2: Look up an instance using a structured TypeClassDispatchKey.
 * Two-level dispatch: constructor kind (level 1) then type_args (level 2).
 * For KIND_STAR keys this delegates to typeclass_env_lookup_instance(). */
TypeClassInstance *typeclass_env_lookup_instance_by_key(const TypeClassEnv *env,
                                                         const TypeClassDispatchKey *key);

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
