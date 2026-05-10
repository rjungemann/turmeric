#include "types.h"
#include "typeclass.h"  /* Phase 15 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Phase 13: Helper to compare lifetimes */
static bool lifetimes_eq(LifetimeId a_lifetimes[], uint8_t a_n, 
                        LifetimeId b_lifetimes[], uint8_t b_n) {
    if (a_n != b_n) return false;
    for (uint8_t i = 0; i < a_n; i++) {
        if (a_lifetimes[i] != b_lifetimes[i]) {
            return false;
        }
    }
    return true;
}

int type_eq(Type a, Type b) {
    if (a.kind != b.kind) return 0;
    /* Phase 13: Check lifetime annotations - only if either has lifetimes */
    if (a.n_lifetimes > 0 || b.n_lifetimes > 0) {
        if (!lifetimes_eq(a.lifetimes, a.n_lifetimes, b.lifetimes, b.n_lifetimes)) {
            return 0;
        }
    }
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
    /* Phase 9: rc<T> and weak<T> */
    if (a.kind == TY_RC || a.kind == TY_WEAK) {
        return a.as.rc.inner == b.as.rc.inner;
    }
    /* Phase 12: Borrow types */
    if (a.kind == TY_REF_IMMUT || a.kind == TY_REF_MUT) {
        return a.as.ref_borrow.target == b.as.ref_borrow.target;
    }
    /* Phase 15: Typeclass types */
    if (a.kind == TY_TYPECLASS) {
        return a.as.typeclass.typeclass == b.as.typeclass.typeclass;
    }
    if (a.kind == TY_TYPECLASS_INST) {
        return a.as.typeclass_inst.instance == b.as.typeclass_inst.instance;
    }
    return 1;
}

/* Helper to create a Type from TypeKind. */
static Type type_from_kind(TypeKind k) {
    Type t;
    t.kind = k;
    t.as.fn.arity = 0;
    t.n_lifetimes = 0;  /* Phase 13: no lifetimes by default */
    return t;
}

/* Phase 13: Helper to format lifetime annotations */
static void lifetime_format(Buf *b, LifetimeId id) {
    /* Format lifetime as 'a, 'b, 'c, etc. based on ID */
    buf_putc(b, '\'');
    buf_putc(b, 'a' + (id - 1));  /* ID 1 -> 'a, ID 2 -> 'b, etc. */
}

/* Phase 13: Helper to format lifetimes for a type */
static void lifetimes_format(Buf *b, Type t) {
    if (t.n_lifetimes == 0) return;
    buf_putc(b, '<');
    for (uint8_t i = 0; i < t.n_lifetimes; i++) {
        if (i > 0) buf_putc(b, ',');
        lifetime_format(b, t.lifetimes[i]);
    }
    buf_putc(b, '>');
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
        /* Phase 9: rc<T> and weak<T> */
        case TY_RC: {
            /* Build "rc<T>" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "rc<");
            buf_puts(&tmp, type_name(type_from_kind(t.as.rc.inner)));
            buf_puts(&tmp, ">");
            buf_putc(&tmp, '\0');
            return strdup(tmp.data);
        }
        case TY_WEAK: {
            /* Build "weak<T>" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "weak<");
            buf_puts(&tmp, type_name(type_from_kind(t.as.rc.inner)));
            buf_puts(&tmp, ">");
            buf_putc(&tmp, '\0');
            return strdup(tmp.data);
        }
        /* Phase 12: Borrow types */
        case TY_REF_IMMUT: {
            /* Build "&T" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "&");
            buf_puts(&tmp, type_name(type_from_kind(t.as.ref_borrow.target)));
            buf_putc(&tmp, '\0');
            return strdup(tmp.data);
        }
        case TY_REF_MUT: {
            /* Build "&mut T" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "&mut ");
            buf_puts(&tmp, type_name(type_from_kind(t.as.ref_borrow.target)));
            buf_putc(&tmp, '\0');
            return strdup(tmp.data);
        }
        /* Phase 15: Typeclass types */
        case TY_TYPECLASS:
            return t.as.typeclass.typeclass ? t.as.typeclass.typeclass->name->name : "<typeclass>";
        case TY_TYPECLASS_INST:
            return t.as.typeclass_inst.instance && t.as.typeclass_inst.instance->typeclass ?
                   t.as.typeclass_inst.instance->typeclass->name->name : "<typeclass-inst>";
        /* Phase 17: Exception types */
        case TY_EXCEPTION: {
            /* Build "exception<T>" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "exception<");
            buf_puts(&tmp, type_name(type_from_kind(t.as.exn.payload_type)));
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
        /* Phase 9: rc<T> and weak<T> */
        case TY_RC: {
            buf_puts(b, "rc<");
            type_name_buf(b, type_from_kind(t.as.rc.inner));
            buf_puts(b, ">");
            break;
        }
        case TY_WEAK: {
            buf_puts(b, "weak<");
            type_name_buf(b, type_from_kind(t.as.rc.inner));
            buf_puts(b, ">");
            break;
        }
        /* Phase 12: Borrow types */
        case TY_REF_IMMUT: {
            buf_puts(b, "&");
            type_name_buf(b, type_from_kind(t.as.ref_borrow.target));
            /* Phase 13: Add lifetime annotation if present */
            if (t.n_lifetimes > 0) {
                buf_putc(b, ' ');
                lifetimes_format(b, t);
            }
            break;
        }
        case TY_REF_MUT: {
            buf_puts(b, "&mut ");
            type_name_buf(b, type_from_kind(t.as.ref_borrow.target));
            /* Phase 13: Add lifetime annotation if present */
            if (t.n_lifetimes > 0) {
                buf_putc(b, ' ');
                lifetimes_format(b, t);
            }
            break;
        }
        /* Phase 15: Typeclass types */
        case TY_TYPECLASS: {
            if (t.as.typeclass.typeclass) {
                buf_puts(b, t.as.typeclass.typeclass->name->name);
            } else {
                buf_puts(b, "<typeclass>");
            }
            break;
        }
        case TY_TYPECLASS_INST: {
            if (t.as.typeclass_inst.instance && t.as.typeclass_inst.instance->typeclass) {
                buf_puts(b, t.as.typeclass_inst.instance->typeclass->name->name);
                buf_puts(b, "<");
                for (uint8_t i = 0; i < t.as.typeclass_inst.instance->n_type_args; i++) {
                    if (i > 0) buf_puts(b, ", ");
                    type_name_buf(b, t.as.typeclass_inst.instance->type_args[i]);
                }
                buf_puts(b, ">");
            } else {
                buf_puts(b, "<typeclass-inst>");
            }
            break;
        }
        /* Phase 17: Exception types */
        case TY_EXCEPTION: {
            buf_puts(b, "exception<");
            type_name_buf(b, type_from_kind(t.as.exn.payload_type));
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
        /* Phase 9: rc<T> and weak<T> both lower to RcControlBlock* in C */
        case TY_RC:
        case TY_WEAK:
            return "RcControlBlock *";
        /* Phase 12: Borrow types lower to pointers in C */
        case TY_REF_IMMUT: {
            /* &T lowers to const T* in C (immutable borrow) */
            /* For now, use void* since we don't have full type info */
            return "const void *";
        }
        case TY_REF_MUT: {
            /* &mut T lowers to T* in C (mutable borrow) */
            /* For now, use void* since we don't have full type info */
            return "void *";
        }
        /* Phase 15: Typeclass types don't have a C representation - they're compile-time only */
        case TY_TYPECLASS:
        case TY_TYPECLASS_INST:
            return "void";  /* Typeclasses don't exist at runtime */
        /* Phase 17: Exception types lower to a struct in C */
        case TY_EXCEPTION:
            return "tur_exception *";
    }
    return "void";
}

void type_print(Buf *b, Type t) {
    type_name_buf(b, t);
}
