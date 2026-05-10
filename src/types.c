#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int type_eq(Type a, Type b) {
    if (a.kind != b.kind) return 0;
    if (a.kind == TY_FN) {
        if (a.as.fn.arity != b.as.fn.arity) return 0;
        for (uint8_t i = 0; i < a.as.fn.arity; i++) {
            if (a.as.fn.arg_kinds[i] != b.as.fn.arg_kinds[i])
                return 0;
        }
        return a.as.fn.result_kind == b.as.fn.result_kind;
    }
    if (a.kind == TY_REF) {
        return a.as.ref.inner == b.as.ref.inner;
    }
    return 1;
}

/* Helper to create a Type from TypeKind. */
static Type type_from_kind(TypeKind k) {
    Type t;
    t.kind = k;
    t.as.fn.arity = 0;
    return t;
}

static void type_name_buf(Buf *b, Type t);

const char *type_name(Type t) {
    switch (t.kind) {
        case TY_UNKNOWN: return "?";
        case TY_NIL:     return "nil";
        case TY_BOOL:    return "bool";
        case TY_INT:     return "int";
        case TY_CSTR:    return "cstr";
        case TY_PTR_VOID: return "ptr<void>";
        case TY_FN: {
            /* Build into a buf, then strdup. */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "(fn [");
            for (uint8_t i = 0; i < t.as.fn.arity; i++) {
                if (i > 0) buf_puts(&tmp, " ");
                buf_puts(&tmp, type_name(type_from_kind(t.as.fn.arg_kinds[i])));
            }
            buf_puts(&tmp, "] : ");
            type_name_buf(&tmp, type_from_kind(t.as.fn.result_kind));
            buf_puts(&tmp, ")");
            buf_putc(&tmp, '\0');
            /* This leaks but it's only used for diagnostics. */
            return strdup(tmp.data);
        }
        case TY_REF: {
            /* Build "ref<T>" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "ref<");
            buf_puts(&tmp, type_name(type_from_kind(t.as.ref.inner)));
            buf_puts(&tmp, ">");
            buf_putc(&tmp, '\0');
            return strdup(tmp.data);
        }
    }
    return "?";
}

static void type_name_buf(Buf *b, Type t) {
    switch (t.kind) {
        case TY_UNKNOWN: buf_puts(b, "?"); break;
        case TY_NIL:     buf_puts(b, "nil"); break;
        case TY_BOOL:    buf_puts(b, "bool"); break;
        case TY_INT:     buf_puts(b, "int"); break;
        case TY_CSTR:    buf_puts(b, "cstr"); break;
        case TY_PTR_VOID: buf_puts(b, "ptr<void>"); break;
        case TY_FN: {
            buf_puts(b, "(fn [");
            for (uint8_t i = 0; i < t.as.fn.arity; i++) {
                if (i > 0) buf_puts(b, " ");
                type_name_buf(b, type_from_kind(t.as.fn.arg_kinds[i]));
            }
            buf_puts(b, "] : ");
            type_name_buf(b, type_from_kind(t.as.fn.result_kind));
            buf_puts(b, ")");
            break;
        }
        case TY_REF: {
            buf_puts(b, "ref<");
            type_name_buf(b, type_from_kind(t.as.ref.inner));
            buf_puts(b, ">");
            break;
        }
    }
}

const char *type_c_name(Type t) {
    switch (t.kind) {
        case TY_UNKNOWN: return "/*unknown*/ void";
        case TY_NIL:     return "void";
        case TY_BOOL:    return "bool";
        case TY_INT:     return "int64_t";
        case TY_CSTR:    return "const char *";
        case TY_PTR_VOID: return "void *";
        case TY_FN: {
            /* For function types, return the result type's C name. */
            return type_c_name(type_from_kind(t.as.fn.result_kind));
        }
        case TY_REF: {
            /* ref<T> lowers to a pointer to T in C */
            /* For now, we use void* for all refs (simple approach) */
            /* TODO: could use T* directly but void* is simpler for v1 */
            return "void *";
        }
    }
    return "void";
}

void type_print(Buf *b, Type t) {
    type_name_buf(b, t);
}
