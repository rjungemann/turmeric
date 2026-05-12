#ifndef TUR_TYPES_H
#define TUR_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "buf.h"
#include "lifetimes.h"  /* Phase 13: Lifetime annotations */

/* Forward declarations for typeclasses (Phase 15) */
typedef struct TypeClass TypeClass;
typedef struct TypeClassInstance TypeClassInstance;

/* Forward declaration for effect rows (Phase 19) */
struct EffectRow;

/* Phase 11: Copy/Move traits */
typedef enum CopyKind {
    CK_MOVE,      /* Move-only: ownership transfer, cannot be copied */
    CK_COPY,      /* Copy: bitwise duplication allowed */
    CK_UNSIZED,   /* Unsized: size unknown at compile time (e.g., slices) */
} CopyKind;
/* Phase HKT (v2, stub): Kind annotations for higher-kinded type support.
 * In v1 all types have kind KIND_STAR; KIND_ARROW is reserved for future use.
 * The hkt_kind field on Type is always KIND_STAR in v1 and ignored by all
 * current elaboration, codegen, and borrow-check passes. */
typedef enum Kind {
    KIND_STAR   = 0,  /* * — a concrete type, e.g. int, bool, vec<int> */
    KIND_ARROW  = 1,  /* * -> * — a unary type constructor, e.g. vec, option */
    KIND_ARROW2 = 2,  /* * -> * -> * — a binary type constructor, e.g. result, either */
} Kind;
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
    TY_FLOAT,         /* Phase 1: double-precision floating point */
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
    /* Phase 18: Delimited continuations */
    TY_CONT,         /* cont<T> - captured continuation that returns T */
    /* Phase B2: Cloneable continuations */
    TY_CLONEABLE_CONT, /* cloneable_cont<T> - multi-shot continuation that returns T */
    /* Phase 11: User-defined struct types */
    TY_STRUCT,       /* user-defined struct type - see as.struct_ for StructDef */
    /* Phase R2: Panic - diverging/never type */
    TY_NEVER,        /* ! - diverging type (never returns; bottom type) */
    /* Phase HKT-P1: Type application (partially-applied type constructor) */
    TY_APP,          /* (type-app F A) — apply type constructor F to argument A */
    /* Phase HKT-P2: Recursive types */
    TY_REC,          /* (defrec Name [params] body) — recursive type binder */
} TypeKind;

/* Phase 11: Struct field descriptor.
 * Stored inline in StructDef.fields[]. */
typedef struct StructField {
    TypeKind kind;               /* field type kind */
    TypeKind inner_kind;         /* for rc/ref/weak, the inner type; TY_UNKNOWN otherwise */
    const char *name;            /* field name (interned string, NUL-terminated) */
    /* Phase 16 v2: effect-row annotation for :fn fields (NULL = no annotation) */
    struct EffectRow *effect_row;
} StructField;

/* Phase 11: Struct type descriptor.
 * Created by elab_defstruct; pointed to from Type.as.struct_.def. */
typedef struct StructDef {
    const char *name;       /* struct name (interned string, NUL-terminated) */
    uint32_t n_fields;
    StructField *fields;    /* field array (malloc'd) */
    bool is_copy;           /* :copy annotation */
    bool needs_drop_glue;   /* true if any field is rc/ref/weak */
    /* Phase HKT-P4: file that defined this struct (for orphan instance check).
     * file_id mirrors Span.file_id; 0 means unknown/builtin. */
    uint16_t origin_file_id;
} StructDef;

/* Phase 11: canonical default copy-kind by kind (typeclass path is primary; this
 * keeps concrete move/copy semantics deterministic for elaboration/codegen). */
static inline CopyKind typekind_default_copy_kind(TypeKind k) {
    switch (k) {
        case TY_NIL:
        case TY_BOOL:
        case TY_INT:
        case TY_FLOAT:
        case TY_CSTR:
        case TY_PTR_VOID:
        case TY_FN:
        case TY_REF_IMMUT:
        case TY_TYPECLASS:
        case TY_TYPECLASS_INST:
            return CK_COPY;
        case TY_REF:
        case TY_RC:
        case TY_WEAK:
        case TY_REF_MUT:
        case TY_EXCEPTION:
        case TY_CONT:
        case TY_STRUCT:   /* default move; actual copy_kind set via type_struct() */
        case TY_NEVER:     /* never type is move-only (no values exist) */
        case TY_REC:       /* recursive type is move-only in v1 */
            return CK_MOVE;
        case TY_APP:       /* type application — opaque int64_t handle, copy by value */
            return CK_COPY;
        case TY_UNKNOWN:
        default:
            return CK_MOVE;
    }
}
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
    /* Phase HKT (v2, stub): kind annotation — always KIND_STAR in v1, reserved for HKT. */
    Kind hkt_kind;
    union {
        struct {
            TypeKind arg_kinds[MAX_FN_ARITY];
            TypeKind result_kind;
            uint8_t arity;
            /* Future-proofing for v3 effects: effect row slot.
             * NULL in v0/v1; treated as empty effect set. */
            struct EffectRow *effect_row;
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
        /* Phase 18: Continuation types */
        struct {
            TypeKind returns;  /* The type T that cont<T> returns */
        } cont;
        /* Phase 11: Struct types */
        struct {
            StructDef *def;    /* The struct type descriptor */
        } struct_;
        /* Phase HKT-P1: Type application — (type-app F A) */
        struct {
            struct Type *fn;   /* The type constructor being applied (kind * -> * or * -> * -> *) */
            struct Type *arg;  /* The type argument (kind *) */
        } app;
        /* Phase HKT-P2: Recursive type binder — (defrec Name [params] body) */
        struct {
            const char *name;  /* Interned binder name (e.g. "Fix"); compare by pointer */
            struct Type *body; /* Body type (NULL in v1 when body is not yet evaluated) */
        } rec;
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

/* Phase T19-B: Thread-safety marker helpers.
 *
 * type_is_send(t): true iff a value of type `t` can be transferred to another
 *   thread (ownership move across thread boundary is safe).
 * type_is_sync(t): true iff a value of type `t` can be shared across threads
 *   (immutable or internally synchronized).
 *
 * Rules (conservative defaults derived from TypeKind):
 *   - Primitives, TY_NIL, TY_BOOL, TY_INT, TY_FLOAT, TY_CSTR, TY_PTR_VOID,
 *     TY_FN, TY_TYPECLASS*, TY_EXCEPTION, TY_STRUCT (default): Send + Sync.
 *   - TY_REF, TY_RC, TY_WEAK:   neither Send nor Sync (single-threaded RC).
 *   - TY_CONT:                   neither Send nor Sync (captures C stack).
 *   - TY_REF_IMMUT, TY_REF_MUT: neither Send nor Sync (borrows; not owned).
 *
 * Struct field propagation and Arc<T>/Mutex<T>/Chan<T> Send+Sync derivation
 * are handled at call sites (T19-C) when those types are introduced.
 */
static inline bool type_is_send(Type t) {
    switch (t.kind) {
        case TY_REF:
        case TY_RC:
        case TY_WEAK:
        case TY_CONT:
        case TY_CLONEABLE_CONT:
        case TY_REF_IMMUT:
        case TY_REF_MUT:
            return false;
        case TY_NEVER:
            /* Never type has no values, so vacuously Send */
            return true;
        default:
            return true;
    }
}

static inline bool type_is_sync(Type t) {
    /* Sync implies Send; use the same conservative rule set in v1. */
    return type_is_send(t);
}

/* Convert TypeKind to string representation for debugging */
const char *typekind_to_string(TypeKind k);

/* Convert type name string to TypeKind */
TypeKind typekind_from_name(const char *name);

static inline Type type_simple(TypeKind kind, CopyKind copy_kind) {
    Type t = {0};
    t.kind = kind;
    t.copy_kind = copy_kind;
    t.hkt_kind = KIND_STAR;
    return t;
}

#define TYPE_UNKNOWN  (type_simple(TY_UNKNOWN, CK_MOVE))
#define TYPE_NIL      (type_simple(TY_NIL, CK_COPY))
#define TYPE_BOOL     (type_simple(TY_BOOL, CK_COPY))
#define TYPE_INT      (type_simple(TY_INT, CK_COPY))
#define TYPE_FLOAT    (type_simple(TY_FLOAT, CK_COPY))
#define TYPE_CSTR     (type_simple(TY_CSTR, CK_COPY))
#define TYPE_PTR_VOID (type_simple(TY_PTR_VOID, CK_COPY))
#define TYPE_NEVER    (type_simple(TY_NEVER, CK_MOVE))

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
    t.copy_kind = typekind_default_copy_kind(TY_FN);
    t.n_lifetimes = 0;
    t.typeclass_instances = NULL;
    t.n_typeclass_instances = 0;
    t.as.fn.arity = arity;
    for (uint8_t i = 0; i < arity; i++) {
        t.as.fn.arg_kinds[i] = arg_kinds[i];
    }
    t.as.fn.result_kind = result_kind;
    t.as.fn.effect_row = NULL;
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

/* Phase 18: Continuation type constructor */
/* Create a continuation type cont<T> that returns T */
static inline Type type_cont(TypeKind returns) {
    Type t;
    t.kind = TY_CONT;
    t.copy_kind = CK_MOVE;  /* Continuations are move-only (one-shot) */
    t.n_lifetimes = 0;
    t.as.cont.returns = returns;
    return t;
}

/* Phase B2: Cloneable continuation type constructor */
/* Create a cloneable continuation type cloneable_cont<T> that returns T */
static inline Type type_cloneable_cont(TypeKind returns) {
    Type t;
    t.kind = TY_CLONEABLE_CONT;
    t.copy_kind = CK_COPY;  /* Cloneable continuations can be copied (multi-shot) */
    t.n_lifetimes = 0;
    t.as.cont.returns = returns;
    return t;
}

/* Phase 11: Struct type constructor */
/* Create a TY_STRUCT type referencing the given StructDef.
 * copy_kind is taken from def->is_copy. */
static inline Type type_struct(StructDef *def) {
    Type t;
    t.kind = TY_STRUCT;
    t.copy_kind = def->is_copy ? CK_COPY : CK_MOVE;
    t.n_lifetimes = 0;
    t.typeclass_instances = NULL;
    t.n_typeclass_instances = 0;
    t.as.struct_.def = def;
    return t;
}

int          type_eq(Type a, Type b);
const char  *type_name(Type t);                   /* "int", "bool", … */
const char  *type_c_name(Type t);                 /* "int64_t", "bool", … */

/* Phase HKT-P2: One-step unrolling of a TY_REC type.
 * Returns t->as.rec.body (the body with the recursive variable bound).
 * In v1, the body is not substituted; returns the raw body pointer.
 * Returns NULL if t is not TY_REC or body is not yet evaluated. */
Type *type_rec_unfold(Type *t);
void         type_print(Buf *b, Type t);

/* Phase HKT H0: Kind utilities */
bool         kind_eq(Kind a, Kind b);             /* true if a == b */
const char  *kind_to_string(Kind k);              /* "*", "* -> *", … */
Kind         kind_parse(const char *s);           /* parse "*" / "* -> *"; default KIND_STAR */

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
