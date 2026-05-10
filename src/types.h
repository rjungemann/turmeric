#ifndef TUR_TYPES_H
#define TUR_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "buf.h"

/* Forward declaration for effect rows (future-proofing for v3 effects) */
typedef struct EffectRow EffectRow;

/* Phase 11: Copy/Move traits */
typedef enum CopyKind {
    CK_MOVE,      /* Move-only: ownership transfer, cannot be copied */
    CK_COPY,      /* Copy: bitwise duplication allowed */
    CK_UNSIZED,   /* Unsized: size unknown at compile time (e.g., slices) */
} CopyKind;

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
} TypeKind;

/* Max arity for function types in phase 2. */
#define MAX_FN_ARITY 8

/* Type uses a union to store either a simple kind or function info. */
typedef struct Type {
    TypeKind kind;
    /* Phase 11: Copy/Move trait */
    CopyKind copy_kind;
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
    } as;
} Type;

#define TYPE_UNKNOWN  ((Type){TY_UNKNOWN, .copy_kind=CK_MOVE, .as={0}})
#define TYPE_NIL      ((Type){TY_NIL, .copy_kind=CK_COPY, .as={0}})
#define TYPE_BOOL     ((Type){TY_BOOL, .copy_kind=CK_COPY, .as={0}})
#define TYPE_INT      ((Type){TY_INT, .copy_kind=CK_COPY, .as={0}})
#define TYPE_CSTR     ((Type){TY_CSTR, .copy_kind=CK_COPY, .as={0}})
#define TYPE_PTR_VOID ((Type){TY_PTR_VOID, .copy_kind=CK_COPY, .as={0}})

/* Phase 5: ref<T> type constructor */
static inline Type type_ref(TypeKind inner) {
    Type t;
    t.kind = TY_REF;
    t.copy_kind = CK_MOVE;  /* ref<T> is move-only */
    t.as.ref.inner = inner;
    return t;
}

/* Phase 9: rc<T> type constructor */
static inline Type type_rc(TypeKind inner) {
    Type t;
    t.kind = TY_RC;
    t.copy_kind = CK_MOVE;  /* rc<T> is move-only by default */
    t.as.rc.inner = inner;
    return t;
}

/* Phase 9: weak<T> type constructor */
static inline Type type_weak(TypeKind inner) {
    Type t;
    t.kind = TY_WEAK;
    t.copy_kind = CK_MOVE;  /* weak<T> is move-only */
    t.as.rc.inner = inner;  /* Reuse the same field as rc */
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
