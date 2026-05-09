#ifndef TUR_TYPES_H
#define TUR_TYPES_H

#include "buf.h"

/* Phase 1 type system: a small enum. Compound types (functions, structs,
 * generics) get added in later phases; the API is designed so callers don't
 * peek at the underlying enum directly. */
typedef enum TypeKind {
    TY_UNKNOWN = 0,   /* not-yet-resolved (elaboration in progress) */
    TY_NIL,           /* unit / void; (do) with no body, (println …) result */
    TY_BOOL,
    TY_INT,           /* int64_t */
    TY_CSTR,          /* const char* — string literal type for now */
} TypeKind;

typedef struct Type {
    TypeKind kind;
} Type;

#define TYPE_UNKNOWN  ((Type){TY_UNKNOWN})
#define TYPE_NIL      ((Type){TY_NIL})
#define TYPE_BOOL     ((Type){TY_BOOL})
#define TYPE_INT      ((Type){TY_INT})
#define TYPE_CSTR     ((Type){TY_CSTR})

int          type_eq(Type a, Type b);
const char  *type_name(Type t);                   /* "int", "bool", … */
const char  *type_c_name(Type t);                 /* "int64_t", "bool", … */
void         type_print(Buf *b, Type t);

#endif
