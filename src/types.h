#ifndef TUR_TYPES_H
#define TUR_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "buf.h"
#include "lifetimes.h"  /* Phase 13: Lifetime annotations */

/* Forward declarations for typeclasses (Phase 15) */
typedef struct TypeClass TypeClass;
typedef struct TypeClassInstance TypeClassInstance;

/* Forward declaration for effect rows (future-proofing for v3 effects) */
typedef struct EffectRow EffectRow;

/* Phase 11: Copy/Move traits */
typedef enum CopyKind {
    CK_MOVE,      /* Move-only: ownership transfer, cannot be copied */
    CK_COPY,      /* Copy: bitwise duplication allowed */
    CK_UNSIZED,   /* Unsized: size unknown at compile time (e.g., slices) */
} CopyKind;

/* Phase 13: Lifetime annotations */
/* Lifetimes are purely an elaborator construct - no runtime representation */

/* Phase 2 type system: function types are stored inline without recursion
 * by using a simple array of TypeKind values. Compound types (structs, 
 * generics) get added in later phases. */
typedef enum TypeKind {
    TY_UNKNOWN = 0,   /* not-yet-resolved (elaboration in progress) */
    TY_NIL,           /* unit / void; (do) with no body, (println …) result */
    TY_BOOL,
    TY_INT,           /* int64_t */
    TY_CSTR,          /* const char* — string literal type for now */
    TY_PTR_VOID,      /* void* — for extern-c and raw pointers */
    TY_FN,            /* function type — requires checking as.fn */
    /* Phase 5: ref<T> — owning pointer with move semantics */
    TY_REF,           /* ref<T> — owning handle, auto-defer drop at scope end */
    /* Phase 9: rc<T> + weak<T> — reference-counted shared ownership */
    TY_RC,            /* rc<T> — reference-counted owning pointer */
    TY_WEAK,          /* weak<T> — non-owning observer for rc<T> */
    /* Phase 12: Borrow traits */
    TY_REF_IMMUT,    /* &T — immutable borrow (non-owning reference) */
    TY_REF_MUT,       /* &mut T — mutable borrow (exclusive non-owning reference) */
    /* Phase 15: Typeclasses */
    TY_TYPECLASS,    /* Typeclass type (e.g., Eq, Ord) */
    TY_TYPECLASS_INST, /* Typeclass instance type */
    /* Phase 17: Exceptions */
    TY_EXCEPTION,    /* Exception type - wraps any value for throw/catch */
} TypeKind;

/* Max arity for function types in phase 2. */
#define MAX_FN_ARITY 8

/* Phase 13: Maximum lifetime parameters per type */
#define MAX_TYPE_LIFETIMES 4

/* Type uses a union to store either a simple kind or function info. */
typedef struct Type {
    TypeKind kind;
    /* Phase 11: Copy/Move trait */
    CopyKind copy_kind;
    /* Phase 13: Lifetime annotations */
    /* Lifetimes attached to this type (for &T, &mut T, function types with lifetime params) */
    LifetimeId lifetimes[MAX_TYPE_LIFETIMES];
    uint8_t   n_lifetimes;
    /* Phase 15: Typeclasses */
    /* For concrete types, the typeclass instances they implement (e.g., int has Eq, Show) */
    TypeClassInstance **typeclass_instances;
    uint8_t n_typeclass_instances;
    union {
        struct {
            TypeKind arg_kinds[MAX_FN_ARITY];
            TypeKind result_kind;
            uint8_t arity;
            /* Future-proofing for v3 effects: effect row slot.
             * NULL in v0/v1; treated as empty effect set. */
            EffectRow *effect_row;
        } fn;
        /* Phase 5: ref<T> stores the inner type T */
        struct {
            TypeKind inner;   /* The type T that ref<T> owns */
        } ref;
        /* Phase 9: rc<T> and weak<T> store the inner type T */
        struct {
            TypeKind inner;   /* The type T that rc<T> or weak<T> points to */
        } rc;
        /* Phase 12: Borrow types store the referenced type T */
        struct {
            TypeKind target;  /* The type T being referenced by &T or &mut T */
        } ref_borrow;
        /* Phase 15: Typeclass types */
        struct {
            TypeClass *typeclass;  /* For TY_TYPECLASS - the typeclass itself */
        } typeclass;
        struct {
            TypeClassInstance *instance;  /* For TY_TYPECLASS_INST - a specific instance */
        } typeclass_inst;
        /* Phase 17: Exception types */
        struct {
            TypeKind payload_type;  /* The type of the exception payload */
        } exn;
    } as;
} Type;

/* Helper to check if a type has lifetime annotations */
static inline bool type_has_lifetime(Type t) {
    return t.n_lifetimes > 0;
}

/* Helper to get the first lifetime from a type */
static inline LifetimeId type_first_lifetime(Type t) {
    return t.n_lifetimes > 0 ? t.lifetimes[0] : LIFETIME_NONE;
}

#define TYPE_UNKNOWN  ((Type){TY_UNKNOWN, .copy_kind=CK_MOVE, .n_lifetimes=0, .as={0}})
#define TYPE_NIL      ((Type){TY_NIL, .copy_kind=CK_COPY, .n_lifetimes=0, .as={0}})
#define TYPE_BOOL     ((Type){TY_BOOL, .copy_kind=CK_COPY, .n_lifetimes=0, .as={0}})
#define TYPE_INT      ((Type){TY_INT, .copy_kind=CK_COPY, .n_lifetimes=0, .as={0}})
#define TYPE_CSTR     ((Type){TY_CSTR, .copy_kind=CK_COPY, .n_lifetimes=0, .as={0}})
#define TYPE_PTR_VOID ((Type){TY_PTR_VOID, .copy_kind=CK_COPY, .n_lifetimes=0, .as={0}})

/* Phase 5: ref<T> type constructor */
static inline Type type_ref(TypeKind inner) {
    Type t;
    t.kind = TY_REF;
    t.copy_kind = CK_MOVE;  /* ref<T> is move-only */
    t.as.ref.inner = inner;
    t.n_lifetimes = 0;
    return t;
}

/* Phase 9: rc<T> type constructor */
static inline Type type_rc(TypeKind inner) {
    Type t;
    t.kind = TY_RC;
    t.copy_kind = CK_MOVE;  /* rc<T> is move-only by default */
    t.as.rc.inner = inner;
    t.n_lifetimes = 0;
    return t;
}

/* Phase 9: weak<T> type constructor */
static inline Type type_weak(TypeKind inner) {
    Type t;
    t.kind = TY_WEAK;
    t.copy_kind = CK_MOVE;  /* weak<T> is move-only */
    t.as.rc.inner = inner;  /* Reuse the same field as rc */
    t.n_lifetimes = 0;
    return t;
}

/* Construct a function type from TypeKinds. */
static inline Type type_fn(TypeKind arg_kinds[], uint8_t arity, TypeKind result_kind) {
    Type t;
    t.kind = TY_FN;
    t.as.fn.arity = arity;
    for (uint8_t i = 0; i < arity; i++) {
        t.as.fn.arg_kinds[i] = arg_kinds[i];
    }
    t.as.fn.result_kind = result_kind;
    return t;
}

/* Phase 12: Borrow type constructors */
/* &T - immutable borrow */
static inline Type type_ref_immut(TypeKind target) {
    Type t;
    t.kind = TY_REF_IMMUT;
    t.copy_kind = CK_COPY;  /* Borrows are copyable (they're just pointers) */
    t.as.ref_borrow.target = target;
    t.n_lifetimes = 0;
    return t;
}

/* &mut T - mutable borrow */
static inline Type type_ref_mut(TypeKind target) {
    Type t;
    t.kind = TY_REF_MUT;
    t.copy_kind = CK_MOVE;  /* Mutable borrows are move-only (exclusive access) */
    t.as.ref_borrow.target = target;
    t.n_lifetimes = 0;
    return t;
}

/* Phase 13: Lifetime-annotated borrow type constructors */
/* &'a T - immutable borrow with lifetime */
static inline Type type_ref_immut_lifetime(TypeKind target, LifetimeId lifetime) {
    Type t = type_ref_immut(target);
    if (lifetime != LIFETIME_NONE) {
        t.lifetimes[0] = lifetime;
        t.n_lifetimes = 1;
    }
    return t;
}

/* &'a mut T - mutable borrow with lifetime */
static inline Type type_ref_mut_lifetime(TypeKind target, LifetimeId lifetime) {
    Type t = type_ref_mut(target);
    if (lifetime != LIFETIME_NONE) {
        t.lifetimes[0] = lifetime;
        t.n_lifetimes = 1;
    }
    return t;
}

/* Phase 15: Typeclass type constructors */
/* Create a typeclass type (e.g., Eq, Ord) */
static inline Type type_typeclass(TypeClass *tc) {
    Type t;
    t.kind = TY_TYPECLASS;
    t.copy_kind = CK_COPY;  /* Typeclasses are copyable */
    t.n_lifetimes = 0;
    t.typeclass_instances = NULL;
    t.n_typeclass_instances = 0;
    t.as.typeclass.typeclass = tc;
    return t;
}

/* Create a typeclass instance type */
static inline Type type_typeclass_inst(TypeClassInstance *inst) {
    Type t;
    t.kind = TY_TYPECLASS_INST;
    t.copy_kind = CK_COPY;  /* Typeclass instances are copyable */
    t.n_lifetimes = 0;
    t.typeclass_instances = NULL;
    t.n_typeclass_instances = 0;
    t.as.typeclass_inst.instance = inst;
    return t;
}

/* Phase 17: Exception type constructor */
/* Create an exception type wrapping a payload of the given type */
static inline Type type_exception(TypeKind payload_type) {
    Type t;
    t.kind = TY_EXCEPTION;
    t.copy_kind = CK_MOVE;  /* Exceptions are move-only (one-shot) */
    t.n_lifetimes = 0;
    t.as.exn.payload_type = payload_type;
    return t;
}

int          type_eq(Type a, Type b);
const char  *type_name(Type t);                   /* "int", "bool", … */
const char  *type_c_name(Type t);                 /* "int64_t", "bool", … */
void         type_print(Buf *b, Type t);

/* Phase 11: Copy/Move trait helpers */
/* Returns true if the type is Copy (bitwise duplication allowed) */
static inline bool type_is_copy(Type t) {
    return t.copy_kind == CK_COPY;
}

/* Returns true if the type is Move-only (ownership transfer) */
static inline bool type_is_move(Type t) {
    return t.copy_kind == CK_MOVE;
}

/* Returns true if the type is Unsized (size unknown at compile time) */
static inline bool type_is_unsized(Type t) {
    return t.copy_kind == CK_UNSIZED;
}

#endif
