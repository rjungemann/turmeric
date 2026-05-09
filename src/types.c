#include "types.h"

int type_eq(Type a, Type b) {
    return a.kind == b.kind;
}

const char *type_name(Type t) {
    switch (t.kind) {
        case TY_UNKNOWN: return "?";
        case TY_NIL:     return "nil";
        case TY_BOOL:    return "bool";
        case TY_INT:     return "int";
        case TY_CSTR:    return "cstr";
    }
    return "?";
}

const char *type_c_name(Type t) {
    switch (t.kind) {
        case TY_UNKNOWN: return "/*unknown*/ void";
        case TY_NIL:     return "void";
        case TY_BOOL:    return "bool";
        case TY_INT:     return "int64_t";
        case TY_CSTR:    return "const char *";
    }
    return "void";
}

void type_print(Buf *b, Type t) {
    buf_puts(b, type_name(t));
}
