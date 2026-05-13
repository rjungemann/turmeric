#include "types.h"
#include "typeclass.h"  /* Phase 15 */
#include "kind_check.h"  /* Phase HKT-P1: for kind_of_type_app */
#include "forms.h"      /* Phase HKT-P1: for Span */

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
    /* Phase 17: Exception types */
    if (a.kind == TY_EXCEPTION) {
        return a.as.exn.payload_type == b.as.exn.payload_type;
    }
    /* Phase 18: Continuation types */
    if (a.kind == TY_CONT) {
        return a.as.cont.returns == b.as.cont.returns;
    }
    if (a.kind == TY_CLONEABLE_CONT) {
        return a.as.cont.returns == b.as.cont.returns;
    }
    /* Phase 11: Struct types - identity by StructDef pointer */
    if (a.kind == TY_STRUCT) {
        return a.as.struct_.def == b.as.struct_.def;
    }
    /* Phase HKT-P1: Type application - compare fn and arg */
    if (a.kind == TY_APP) {
        if (!a.as.app.fn || !b.as.app.fn) return a.as.app.fn == b.as.app.fn;
        if (!a.as.app.arg || !b.as.app.arg) return a.as.app.arg == b.as.app.arg;
        return type_eq(*a.as.app.fn, *b.as.app.fn) && type_eq(*a.as.app.arg, *b.as.app.arg);
    }
    /* Phase HKT-P2: Recursive types - identity by name pointer (interned) */
    if (a.kind == TY_REC) {
        return a.as.rec.name == b.as.rec.name;
    }
    return 1;
}

/* Helper to create a Type from TypeKind. */
static Type type_from_kind(TypeKind k) {
    Type t;
    t.kind = k;
    t.copy_kind = typekind_default_copy_kind(k);
    t.as.fn.arity = 0;
    t.n_lifetimes = 0;  /* Phase 13: no lifetimes by default */
    t.typeclass_instances = NULL;
    t.n_typeclass_instances = 0;
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
        case TY_FLOAT:   return "float";
        case TY_CSTR:    return "cstr";
        case TY_PTR_VOID: return "ptr<void>";
        case TY_NEVER:   return "!";
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
            return tur_strdup(tmp.data);
        }
        case TY_REF: {
            /* Build "ref<T>" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "ref<");
            buf_puts(&tmp, type_name(type_from_kind(t.as.ref.inner)));
            buf_puts(&tmp, ">");
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
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
            return tur_strdup(tmp.data);
        }
        case TY_WEAK: {
            /* Build "weak<T>" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "weak<");
            buf_puts(&tmp, type_name(type_from_kind(t.as.rc.inner)));
            buf_puts(&tmp, ">");
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        /* Phase 12: Borrow types */
        case TY_REF_IMMUT: {
            /* Build "&T" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "&");
            buf_puts(&tmp, type_name(type_from_kind(t.as.ref_borrow.target)));
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        case TY_REF_MUT: {
            /* Build "&mut T" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "&mut ");
            buf_puts(&tmp, type_name(type_from_kind(t.as.ref_borrow.target)));
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
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
            return tur_strdup(tmp.data);
        }
        /* Phase 18: Continuation types */
        case TY_CONT: {
            /* Build "cont<T>" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "cont<");
            buf_puts(&tmp, type_name(type_from_kind(t.as.cont.returns)));
            buf_puts(&tmp, ">");
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        case TY_CLONEABLE_CONT: {
            /* Build "cloneable_cont<T>" name */
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "cloneable_cont<");
            buf_puts(&tmp, type_name(type_from_kind(t.as.cont.returns)));
            buf_puts(&tmp, ">");
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        /* Phase 11: Struct types */
        case TY_STRUCT:
            return t.as.struct_.def ? t.as.struct_.def->name : "<struct>";
        /* Phase HKT-P1: Type application */
        case TY_APP: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "(type-app ");
            buf_puts(&tmp, t.as.app.fn ? type_name(*t.as.app.fn) : "?");
            buf_putc(&tmp, ' ');
            buf_puts(&tmp, t.as.app.arg ? type_name(*t.as.app.arg) : "?");
            buf_putc(&tmp, ')');
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        /* Phase HKT-P2: Recursive types */
        case TY_REC:
            return t.as.rec.name ? t.as.rec.name : "<rec>";
    }
    return "?";
}

static void type_name_buf(Buf *b, Type t) {
    switch (t.kind) {
        case TY_UNKNOWN: buf_puts(b, "?"); break;
        case TY_NIL:     buf_puts(b, "nil"); break;
        case TY_BOOL:    buf_puts(b, "bool"); break;
        case TY_INT:     buf_puts(b, "int"); break;
        case TY_FLOAT:   buf_puts(b, "float"); break;
        case TY_CSTR:    buf_puts(b, "cstr"); break;
        case TY_PTR_VOID: buf_puts(b, "ptr<void>"); break;
        case TY_NEVER:   buf_puts(b, "!"); break;
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
        /* Phase 18: Continuation types */
        case TY_CONT: {
            buf_puts(b, "cont<");
            type_name_buf(b, type_from_kind(t.as.cont.returns));
            buf_puts(b, ">");
            break;
        }
        case TY_CLONEABLE_CONT: {
            buf_puts(b, "cloneable_cont<");
            type_name_buf(b, type_from_kind(t.as.cont.returns));
            buf_puts(b, ">");
            break;
        }
        /* Phase 11: Struct types */
        case TY_STRUCT: {
            if (t.as.struct_.def) {
                buf_puts(b, t.as.struct_.def->name);
            } else {
                buf_puts(b, "<struct>");
            }
            break;
        }
        /* Phase HKT-P1: Type application */
        case TY_APP: {
            buf_puts(b, "(type-app ");
            if (t.as.app.fn) type_name_buf(b, *t.as.app.fn); else buf_puts(b, "?");
            buf_putc(b, ' ');
            if (t.as.app.arg) type_name_buf(b, *t.as.app.arg); else buf_puts(b, "?");
            buf_putc(b, ')');
            break;
        }
        /* Phase HKT-P2: Recursive types */
        case TY_REC: {
            buf_puts(b, t.as.rec.name ? t.as.rec.name : "<rec>");
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
        case TY_FLOAT:   return "double";
        case TY_CSTR:    return "const char *";
        case TY_PTR_VOID: return "void *";
        case TY_NEVER:   return "void";  /* never type has no values, use void */
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
        /* Phase 18: Continuation types lower to a struct in C */
        case TY_CONT:
            return "tur_cont *";
        case TY_CLONEABLE_CONT:
            return "tur_cloneable_cont *";
        /* Phase 11: Struct types lower to the struct name in C */
        case TY_STRUCT:
            /* Phase HKT H3: TY_STRUCT without a concrete StructDef represents an
             * opaque HKT type-constructor argument; it is stored as int64_t at
             * runtime (container values are int64_t-sized opaque handles). */
            return t.as.struct_.def ? t.as.struct_.def->name : "int64_t";
        /* Phase HKT-P1: Type application — opaque int64_t handle in v1 */
        case TY_APP:
            return "int64_t";
        /* Phase HKT-P2: Recursive types — opaque int64_t handle in v1 */
        case TY_REC:
            return "int64_t";
    }
    return "void";
}

/* Phase HKT-P1: Type application constructor.
 * Creates a TY_APP type node with the given fn and arg types.
 * The result kind is computed using kind_of_type_app. */
Type type_app(Arena *a, Type fn, Type arg, Span span) {
    Type t;
    memset(&t, 0, sizeof(t));
    t.kind = TY_APP;
    t.copy_kind = CK_COPY;  /* TY_APP is an opaque handle, copy by value */
    t.n_lifetimes = 0;
    t.typeclass_instances = NULL;
    t.n_typeclass_instances = 0;
    
    /* Allocate memory for fn and arg on the arena */
    t.as.app.fn = (Type *)arena_alloc(a, sizeof(Type));
    memcpy(t.as.app.fn, &fn, sizeof(Type));
    t.as.app.arg = (Type *)arena_alloc(a, sizeof(Type));
    memcpy(t.as.app.arg, &arg, sizeof(Type));
    
    /* Compute the result kind */
    t.hkt_kind = kind_of_type_app(fn, arg, span);
    
    return t;
}

/* Phase HKT-P2: One-step unrolling of a TY_REC type.
 * Returns the body of the recursive type (the type with the binder variable in scope).
 * In v1, simply returns the body pointer without substitution.
 * Returns NULL if t is not TY_REC or body is not yet evaluated. */
Type *type_rec_unfold(Type *t) {
    if (!t || t->kind != TY_REC) return NULL;
    return t->as.rec.body;
}

void type_print(Buf *b, Type t) {
    type_name_buf(b, t);
}

/* Phase HKT H0: Kind utilities */

bool kind_eq(Kind a, Kind b) {
    return a == b;
}

const char *kind_to_string(Kind k) {
    switch (k) {
        case KIND_STAR:   return "*";
        case KIND_ARROW:  return "* -> *";
        case KIND_ARROW2: return "* -> * -> *";
    }
    return "*";  /* default */
}

Kind kind_parse(const char *s) {
    if (!s) return KIND_STAR;
    if (s[0] == '*' && s[1] == ' ' && s[2] == '-' && s[3] == '>' &&
        s[4] == ' ' && s[5] == '*' && s[6] == ' ' && s[7] == '-' &&
        s[8] == '>' && s[9] == ' ' && s[10] == '*' && s[11] == '\0') {
        return KIND_ARROW2;
    }
    if (s[0] == '*' && s[1] == ' ' && s[2] == '-' && s[3] == '>' &&
        s[4] == ' ' && s[5] == '*' && s[6] == '\0') {
        return KIND_ARROW;
    }
    return KIND_STAR;
}

const char *typekind_to_string(TypeKind k) {
    switch (k) {
        case TY_UNKNOWN:   return "unknown";
        case TY_NIL:      return "nil";
        case TY_BOOL:     return "bool";
        case TY_INT:      return "int";
        case TY_FLOAT:    return "float";
        case TY_CSTR:     return "cstr";
        case TY_PTR_VOID: return "ptr-void";
        case TY_FN:       return "fn";
        case TY_REF:      return "ref";
        case TY_RC:       return "rc";
        case TY_WEAK:     return "weak";
        case TY_REF_IMMUT: return "&immut";
        case TY_REF_MUT:  return "&mut";
        case TY_TYPECLASS: return "typeclass";
        case TY_TYPECLASS_INST: return "typeclass-inst";
        case TY_EXCEPTION: return "exception";
        case TY_CONT:     return "cont";
        case TY_CLONEABLE_CONT: return "cloneable_cont";
        case TY_STRUCT:   return "struct";
        case TY_NEVER:    return "!";
        default:          return "<?>";
    }
}

TypeKind typekind_from_name(const char *name) {
    if (!name) return TY_UNKNOWN;
    if (strcmp(name, "unknown") == 0) return TY_UNKNOWN;
    if (strcmp(name, "nil") == 0) return TY_NIL;
    if (strcmp(name, "bool") == 0) return TY_BOOL;
    if (strcmp(name, "int") == 0) return TY_INT;
    if (strcmp(name, "float") == 0) return TY_FLOAT;
    if (strcmp(name, "cstr") == 0) return TY_CSTR;
    if (strcmp(name, "ptr-void") == 0 || strcmp(name, "ptr<void>") == 0) return TY_PTR_VOID;
    if (strcmp(name, "fn") == 0) return TY_FN;
    if (strcmp(name, "ref") == 0) return TY_REF;
    if (strcmp(name, "rc") == 0) return TY_RC;
    if (strcmp(name, "weak") == 0) return TY_WEAK;
    if (strcmp(name, "&immut") == 0 || strcmp(name, "&") == 0) return TY_REF_IMMUT;
    if (strcmp(name, "&mut") == 0) return TY_REF_MUT;
    if (strcmp(name, "typeclass") == 0) return TY_TYPECLASS;
    if (strcmp(name, "typeclass-inst") == 0) return TY_TYPECLASS_INST;
    if (strcmp(name, "exception") == 0) return TY_EXCEPTION;
    if (strcmp(name, "cont") == 0) return TY_CONT;
    if (strcmp(name, "struct") == 0) return TY_STRUCT;
    if (strcmp(name, "!") == 0 || strcmp(name, "never") == 0) return TY_NEVER;
    return TY_UNKNOWN;
}
