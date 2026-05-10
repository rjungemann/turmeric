#ifndef TUR_TYPES_H
#define TUR_TYPES_H

#include <stdint.h>
#include "buf.h"

/* Forward declaration for effect rows (future-proofing for v3 effects) */
typedef struct EffectRow EffectRow;

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
} TypeKind;

/* Max arity for function types in phase 2. */
#define MAX_FN_ARITY 8

/* Type uses a union to store either a simple kind or function info. */
typedef struct Type {
    TypeKind kind;
    union {
        struct {
            TypeKind arg_kinds[MAX_FN_ARITY];
            TypeKind result_kind;
            uint8_t arity;
            /* Future-proofing for v3 effects: effect row slot.
             * NULL in v0/v1; treated as empty effect set. */
            EffectRow *effect_row;
        } fn;
    } as;
} Type;

#define TYPE_UNKNOWN  ((Type){TY_UNKNOWN, .as={0}})
#define TYPE_NIL      ((Type){TY_NIL, .as={0}})
#define TYPE_BOOL     ((Type){TY_BOOL, .as={0}})
#define TYPE_INT      ((Type){TY_INT, .as={0}})
#define TYPE_CSTR     ((Type){TY_CSTR, .as={0}})
#define TYPE_PTR_VOID ((Type){TY_PTR_VOID, .as={0}})

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

#endif
