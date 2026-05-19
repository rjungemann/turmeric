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
    /* ET3: Handler types */
    if (a.kind == TY_HANDLER) {
        return a.as.handler_.effect_name == b.as.handler_.effect_name
            && a.as.handler_.value_kind == b.as.handler_.value_kind
            && a.as.handler_.result_kind == b.as.handler_.result_kind;
    }
    /* Phase 11: Struct types - identity by StructDef pointer */
    if (a.kind == TY_STRUCT) {
        return a.as.struct_.def == b.as.struct_.def;
    }
    /* Phase G0: ADT types - identity by AdtDef pointer */
    if (a.kind == TY_ADT) {
        return a.as.adt_.def == b.as.adt_.def;
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
    /* Phase HRT0: Quantified types — structural equality on n_vars and body */
    if (a.kind == TY_FORALL || a.kind == TY_EXISTS) {
        if (a.as.forall_.n_vars != b.as.forall_.n_vars) return 0;
        if (!a.as.forall_.body || !b.as.forall_.body)
            return a.as.forall_.body == b.as.forall_.body;
        return type_eq(*a.as.forall_.body, *b.as.forall_.body);
    }
    /* IT0: Union types — structural equality: same n_members, each member equal */
    if (a.kind == TY_UNION) {
        if (a.as.union_.n_members != b.as.union_.n_members) return 0;
        for (uint8_t i = 0; i < a.as.union_.n_members; i++) {
            if (!a.as.union_.members[i] || !b.as.union_.members[i]) {
                if (a.as.union_.members[i] != b.as.union_.members[i]) return 0;
                continue;
            }
            if (!type_eq(*a.as.union_.members[i], *b.as.union_.members[i])) return 0;
        }
        return 1;
    }
    /* IT2: Intersection types — structural equality: same n_members, each member equal */
    if (a.kind == TY_INTERSECTION) {
        if (a.as.intersection_.n_members != b.as.intersection_.n_members) return 0;
        for (uint8_t i = 0; i < a.as.intersection_.n_members; i++) {
            if (!a.as.intersection_.members[i] || !b.as.intersection_.members[i]) {
                if (a.as.intersection_.members[i] != b.as.intersection_.members[i]) return 0;
                continue;
            }
            if (!type_eq(*a.as.intersection_.members[i], *b.as.intersection_.members[i])) return 0;
        }
        return 1;
    }
    return 1;
}

/* LT2: Check whether function type `actual' is compatible with function type
 * `expected' with respect to arg_linear constraints.  Returns 1 if compatible,
 * 0 if there is a linearity mismatch.
 *
 * Linearity is invariant for function types:
 *   - if expected.arg_linear[i] is true, actual.arg_linear[i] must also be true
 *     (you cannot pass a non-consuming function where a consuming one is required)
 *   - if expected.arg_linear[i] is false, actual.arg_linear[i] must also be false
 *     (you cannot pass a consuming function where a non-consuming one is required,
 *     because the caller would not know to treat the argument as consumed)
 *
 * Called from elab_call_fn when -Xlinear is enabled and higher-order functions
 * are passed as arguments. */
int fn_type_subtype(Type actual, Type expected) {
    if (actual.kind != TY_FN || expected.kind != TY_FN) return 1;
    if (actual.as.fn.arity != expected.as.fn.arity) return 1; /* arity mismatch caught elsewhere */
    for (uint8_t i = 0; i < actual.as.fn.arity; i++) {
        if (actual.as.fn.arg_linear[i] != expected.as.fn.arg_linear[i]) return 0;
    }
    return 1;
}

/* Helper to create a Type from TypeKind.
 * Zero-initialises the entire struct so that compound type fields (union_,
 * intersection_, etc.) are safe to pass to type_name() even for kinds that
 * store no additional data. */
static Type type_from_kind(TypeKind k) {
    Type t;
    memset(&t, 0, sizeof(t));
    t.kind = k;
    t.copy_kind = typekind_default_copy_kind(k);
    t.hkt_kind = KIND_STAR;  /* Phase HKT-P6: all types are kind * in v1 */
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
        case TY_INT8:    return "int8";
        case TY_INT16:   return "int16";
        case TY_INT32:   return "int32";
        case TY_INT64:   return "int64";
        case TY_UINT8:   return "uint8";
        case TY_UINT16:  return "uint16";
        case TY_UINT32:  return "uint32";
        case TY_UINT64:  return "uint64";
        case TY_FLOAT32: return "float32";
        case TY_FLOAT64: return "float64";
        case TY_CSTR:    return "cstr";
        case TY_PTR_VOID: return "ptr<void>";
        case TY_NEVER:   return "!";
        case TY_TYVAR:   return "tyvar";
        /* IT4: Top type */
        case TY_ANY:     return "any";
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
        /* LT3: lref<T> — linear ref */
        case TY_LREF: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "lref<");
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
        /* Phase G0: ADT types */
        case TY_ADT:
            return t.as.adt_.def ? t.as.adt_.def->name : "<adt>";
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
        /* Phase X3: Set literal */
        case TY_SET:
            return "set";
        /* Phase HRT0: Quantified types */
        case TY_FORALL:
        case TY_EXISTS: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, t.kind == TY_FORALL ? "(forall [" : "(exists [");
            for (uint8_t i = 0; i < t.as.forall_.n_vars; i++) {
                if (i > 0) buf_putc(&tmp, ' ');
                buf_puts(&tmp, t.as.forall_.var_names && t.as.forall_.var_names[i]
                                ? t.as.forall_.var_names[i] : "?");
            }
            buf_puts(&tmp, "] ");
            if (t.as.forall_.body) {
                buf_puts(&tmp, type_name(*t.as.forall_.body));
            } else {
                buf_puts(&tmp, "?");
            }
            buf_putc(&tmp, ')');
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        /* IT0: Union types — "(T1 | T2 | ...)" */
        case TY_UNION: {
            Buf tmp;
            buf_init(&tmp);
            buf_putc(&tmp, '(');
            for (uint8_t i = 0; i < t.as.union_.n_members; i++) {
                if (i > 0) buf_puts(&tmp, " | ");
                if (t.as.union_.members && t.as.union_.members[i]) {
                    buf_puts(&tmp, type_name(*t.as.union_.members[i]));
                } else {
                    buf_putc(&tmp, '?');
                }
            }
            buf_putc(&tmp, ')');
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        /* IT2: Intersection types — "(T1 & T2 & ...)" */
        case TY_INTERSECTION: {
            Buf tmp;
            buf_init(&tmp);
            buf_putc(&tmp, '(');
            for (uint8_t i = 0; i < t.as.intersection_.n_members; i++) {
                if (i > 0) buf_puts(&tmp, " & ");
                if (t.as.intersection_.members && t.as.intersection_.members[i]) {
                    buf_puts(&tmp, type_name(*t.as.intersection_.members[i]));
                } else {
                    buf_putc(&tmp, '?');
                }
            }
            buf_putc(&tmp, ')');
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        /* ET3: Handler type — "handler<Effect, ValueType, ResultType>" */
        case TY_HANDLER: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "handler<");
            buf_puts(&tmp, t.as.handler_.effect_name ? t.as.handler_.effect_name : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, type_name(type_from_kind(t.as.handler_.value_kind)));
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, type_name(type_from_kind(t.as.handler_.result_kind)));
            buf_putc(&tmp, '>');
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        /* CT0: Contract type — "{ x : T | p }" */
        case TY_CONTRACT: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "{ ");
            buf_puts(&tmp, t.as.contract_.var_name ? t.as.contract_.var_name : "_");
            buf_puts(&tmp, " : ");
            buf_puts(&tmp, t.as.contract_.base_type
                          ? type_name(*t.as.contract_.base_type) : "?");
            buf_puts(&tmp, " | ... }");
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        /* SS0a: Session protocol type names */
        case TY_SESSION: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "Session[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        case TY_SEND: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "Send[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.snd ? type_name(*t.as.session_.snd) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        case TY_RECV: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "Recv[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.snd ? type_name(*t.as.session_.snd) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        case TY_CLOSE:
            return "Close";
        case TY_CHOOSE: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "Choose[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.snd ? type_name(*t.as.session_.snd) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        case TY_BRANCH: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "Branch[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.snd ? type_name(*t.as.session_.snd) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        case TY_SESSION_REC: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "Rec[");
            buf_puts(&tmp, t.as.session_.label ? t.as.session_.label : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        case TY_TIMEOUT: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "Timeout[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.snd ? type_name(*t.as.session_.snd) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        case TY_SESSION_PAIR: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "SessionPair[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.snd ? type_name(*t.as.session_.snd) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        case TY_SESSION_RECV_PAIR: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "RecvPair[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.snd ? type_name(*t.as.session_.snd) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
        }
        case TY_SESSION_OFFER: {
            Buf tmp;
            buf_init(&tmp);
            buf_puts(&tmp, "Offer[");
            buf_puts(&tmp, t.as.session_.fst ? type_name(*t.as.session_.fst) : "?");
            buf_puts(&tmp, ", ");
            buf_puts(&tmp, t.as.session_.snd ? type_name(*t.as.session_.snd) : "?");
            buf_puts(&tmp, "]");
            buf_putc(&tmp, '\0');
            return tur_strdup(tmp.data);
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
        case TY_FLOAT:   buf_puts(b, "float"); break;
        case TY_INT8:    buf_puts(b, "int8"); break;
        case TY_INT16:   buf_puts(b, "int16"); break;
        case TY_INT32:   buf_puts(b, "int32"); break;
        case TY_INT64:   buf_puts(b, "int64"); break;
        case TY_UINT8:   buf_puts(b, "uint8"); break;
        case TY_UINT16:  buf_puts(b, "uint16"); break;
        case TY_UINT32:  buf_puts(b, "uint32"); break;
        case TY_UINT64:  buf_puts(b, "uint64"); break;
        case TY_FLOAT32: buf_puts(b, "float32"); break;
        case TY_FLOAT64: buf_puts(b, "float64"); break;
        case TY_CSTR:    buf_puts(b, "cstr"); break;
        case TY_PTR_VOID: buf_puts(b, "ptr<void>"); break;
        case TY_NEVER:   buf_puts(b, "!"); break;
        case TY_TYVAR:   buf_puts(b, "tyvar"); break;
        /* IT4: Top type */
        case TY_ANY:     buf_puts(b, "any"); break;
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
        /* LT3: lref<T> — linear ref */
        case TY_LREF: {
            buf_puts(b, "lref<");
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
        /* Phase G0: ADT types */
        case TY_ADT: {
            if (t.as.adt_.def) {
                buf_puts(b, t.as.adt_.def->name);
            } else {
                buf_puts(b, "<adt>");
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
        /* Phase X3: Set literal */
        case TY_SET: {
            buf_puts(b, "set");
            break;
        }
        /* Phase HRT0: Quantified types — always print quantifiers explicitly */
        case TY_FORALL:
        case TY_EXISTS: {
            buf_puts(b, t.kind == TY_FORALL ? "(forall [" : "(exists [");
            for (uint8_t i = 0; i < t.as.forall_.n_vars; i++) {
                if (i > 0) buf_putc(b, ' ');
                buf_puts(b, t.as.forall_.var_names && t.as.forall_.var_names[i]
                             ? t.as.forall_.var_names[i] : "?");
            }
            buf_puts(b, "] ");
            if (t.as.forall_.body) type_name_buf(b, *t.as.forall_.body);
            else buf_puts(b, "?");
            buf_putc(b, ')');
            break;
        }
        /* IT0: Union types — "(T1 | T2 | ...)" */
        case TY_UNION: {
            buf_putc(b, '(');
            for (uint8_t i = 0; i < t.as.union_.n_members; i++) {
                if (i > 0) buf_puts(b, " | ");
                if (t.as.union_.members && t.as.union_.members[i]) {
                    type_name_buf(b, *t.as.union_.members[i]);
                } else {
                    buf_putc(b, '?');
                }
            }
            buf_putc(b, ')');
            break;
        }
        /* IT2: Intersection types — "(T1 & T2 & ...)" */
        case TY_INTERSECTION: {
            buf_putc(b, '(');
            for (uint8_t i = 0; i < t.as.intersection_.n_members; i++) {
                if (i > 0) buf_puts(b, " & ");
                if (t.as.intersection_.members && t.as.intersection_.members[i]) {
                    type_name_buf(b, *t.as.intersection_.members[i]);
                } else {
                    buf_putc(b, '?');
                }
            }
            buf_putc(b, ')');
            break;
        }
        /* ET3: Handler type */
        case TY_HANDLER: {
            buf_puts(b, "handler<");
            buf_puts(b, t.as.handler_.effect_name ? t.as.handler_.effect_name : "?");
            buf_puts(b, ", ");
            type_name_buf(b, type_from_kind(t.as.handler_.value_kind));
            buf_puts(b, ", ");
            type_name_buf(b, type_from_kind(t.as.handler_.result_kind));
            buf_putc(b, '>');
            break;
        }
        /* CT0: Contract type */
        case TY_CONTRACT: {
            buf_puts(b, "{ ");
            buf_puts(b, t.as.contract_.var_name ? t.as.contract_.var_name : "_");
            buf_puts(b, " : ");
            if (t.as.contract_.base_type) {
                type_name_buf(b, *t.as.contract_.base_type);
            } else {
                buf_putc(b, '?');
            }
            buf_puts(b, " | ... }");
            break;
        }
        /* SS0a: Session protocol types */
        case TY_SESSION:
            buf_puts(b, "Session[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_SEND:
            buf_puts(b, "Send[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_puts(b, ", ");
            if (t.as.session_.snd) type_name_buf(b, *t.as.session_.snd);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_RECV:
            buf_puts(b, "Recv[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_puts(b, ", ");
            if (t.as.session_.snd) type_name_buf(b, *t.as.session_.snd);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_CLOSE:
            buf_puts(b, "Close");
            break;
        case TY_CHOOSE:
            buf_puts(b, "Choose[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_puts(b, ", ");
            if (t.as.session_.snd) type_name_buf(b, *t.as.session_.snd);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_BRANCH:
            buf_puts(b, "Branch[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_puts(b, ", ");
            if (t.as.session_.snd) type_name_buf(b, *t.as.session_.snd);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_SESSION_REC:
            buf_puts(b, "Rec[");
            buf_puts(b, t.as.session_.label ? t.as.session_.label : "?");
            buf_puts(b, ", ");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_TIMEOUT:
            buf_puts(b, "Timeout[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_puts(b, ", ");
            if (t.as.session_.snd) type_name_buf(b, *t.as.session_.snd);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_SESSION_PAIR:
            buf_puts(b, "SessionPair[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_puts(b, ", ");
            if (t.as.session_.snd) type_name_buf(b, *t.as.session_.snd);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_SESSION_RECV_PAIR:
            buf_puts(b, "RecvPair[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_puts(b, ", ");
            if (t.as.session_.snd) type_name_buf(b, *t.as.session_.snd);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
        case TY_SESSION_OFFER:
            buf_puts(b, "Offer[");
            if (t.as.session_.fst) type_name_buf(b, *t.as.session_.fst);
            else buf_putc(b, '?');
            buf_puts(b, ", ");
            if (t.as.session_.snd) type_name_buf(b, *t.as.session_.snd);
            else buf_putc(b, '?');
            buf_putc(b, ']');
            break;
    }
}

const char *type_c_name(Type t) {
    switch (t.kind) {
        case TY_UNKNOWN: return "/*unknown*/ void";
        case TY_NIL:     return "void";
        case TY_BOOL:    return "bool";
        case TY_INT:     return "int64_t";
        case TY_FLOAT:   return "double";
        case TY_INT8:    return "int8_t";
        case TY_INT16:   return "int16_t";
        case TY_INT32:   return "int32_t";
        case TY_INT64:   return "int64_t";
        case TY_UINT8:   return "uint8_t";
        case TY_UINT16:  return "uint16_t";
        case TY_UINT32:  return "uint32_t";
        case TY_UINT64:  return "uint64_t";
        case TY_FLOAT32: return "float";
        case TY_FLOAT64: return "double";
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
        /* LT3: lref<T> lowers identically to ref<T> in C */
        case TY_LREF:
            return "void *";
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
        /* Phase 17: Exceptions removed. TY_EXCEPTION is orphaned; lower to void* */
        case TY_EXCEPTION:
            return "void *";
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
        /* Phase G0: ADT types are passed as int64_t (opaque heap pointer) */
        case TY_ADT:
            return "int64_t";
        /* Phase G2: unresolved type variable — treated as int64_t at codegen level */
        case TY_TYVAR:
            return "int64_t";
        /* Phase HKT-P1: Type application — opaque int64_t handle in v1 */
        case TY_APP:
            return "int64_t";
        /* Phase HKT-P2: Recursive types — opaque int64_t handle in v1 */
        case TY_REC:
            return "int64_t";
        /* Phase X3: Set literal — sorted int64_t array */
        case TY_SET:
            return "tur_set_t *";
        /* Phase HRT0: Quantified types — no runtime representation in HRT0 */
        case TY_FORALL:
        case TY_EXISTS:
            return "void *";
        /* IT4: Union types — tagged union struct {int64_t tag; int64_t val} */
        case TY_UNION:
            return "tur_tagged_t";
        /* IT2: Intersection types — opaque int64_t placeholder (full codegen in IT4) */
        case TY_INTERSECTION:
            return "int64_t";
        /* IT4: any — tagged union struct (same as TY_UNION; tag is TypeKind of stored value) */
        case TY_ANY:
            return "tur_tagged_t";
        /* ET3: Handler type — struct with env pointer and function pointer */
        case TY_HANDLER:
            return "tur_handler_t";
        /* CT0: Contract type — same C representation as its base type */
        case TY_CONTRACT:
            return t.as.contract_.base_type
                   ? type_c_name(*t.as.contract_.base_type)
                   : "int64_t";
        /* SS0a/SS1: Session channel endpoints lower to void* in C until SS2
         * defines the TurChannel struct. Protocol descriptor types are erased
         * and never appear as C values. */
        case TY_SESSION:
            return "void *";
        case TY_SEND:
        case TY_RECV:
        case TY_CLOSE:
        case TY_CHOOSE:
        case TY_BRANCH:
        case TY_SESSION_REC:
        case TY_TIMEOUT:
            return "/*session-protocol*/ void";
        case TY_SESSION_PAIR:
            return "/*session-pair*/ void";
        case TY_SESSION_RECV_PAIR:
            return "/*session-recv-pair*/ void";
        case TY_SESSION_OFFER:
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

/* Phase HKT-P2: Check if a type body is guarded-recursive.
 * A recursive reference to `rec_name` is guarded if it appears under at least
 * one type constructor (TY_APP or TY_STRUCT). This prevents infinite types
 * like `(defrec X [] X)` while allowing `(defrec Fix [f] (Fix (f Fix)))`.
 *
 * `t`: the type body to check
 * `rec_name`: the name of the recursive type binder
 * `depth`: current nesting depth of type constructors (guard count)
 * Returns true if all occurrences of rec_name are guarded (depth >= 1),
 * or if there are no occurrences of rec_name at all.
 */
static bool type_is_guarded_recursive_helper(const Type *t, const char *rec_name, int depth) {
    if (!t) return true;

    switch (t->kind) {
        case TY_REC: {
            /* A nested TY_REC with a different name is fine.
             * Same name at depth 0 means unguarded recursion -> reject. */
            if (t->as.rec.name && strcmp(t->as.rec.name, rec_name) == 0) {
                return depth > 0;  /* Guarded iff under a type constructor */
            }
            /* Different name or no name - recurse into body */
            return type_is_guarded_recursive_helper(t->as.rec.body, rec_name, depth);
        }

        case TY_APP: {
            /* TY_APP is a type constructor - it guards recursion */
            int new_depth = depth + 1;
            /* Check both the function and argument types */
            if (!type_is_guarded_recursive_helper(t->as.app.fn, rec_name, new_depth))
                return false;
            if (!type_is_guarded_recursive_helper(t->as.app.arg, rec_name, new_depth))
                return false;
            return true;
        }

        case TY_STRUCT: {
            /* TY_STRUCT is a type constructor - it guards recursion */
            /* In v1, StructDef stores TypeKind, not full Type structs.
             * This is a limitation - we can only check TY_APP and TY_REC properly.
             * For v1, we assume struct fields are fine and just return true.
             * The recursion checking only works for TY_APP and TY_REC. */
            (void)depth;  /* Unused in this branch */
            return true;
        }

        case TY_FN: {
            /* Function types: in v1, TY_FN stores TypeKind arrays, not full Type structs */
            /* So we can't recursively check - this is a v1 limitation */
            (void)depth;  /* Unused in this branch */
            return true;
        }

        case TY_REF:
        case TY_LREF:
        case TY_RC:
        case TY_WEAK:
        case TY_REF_IMMUT:
        case TY_REF_MUT: {
            /* In v1, these store TypeKind not Type, so we can't recurse.
             * Just return true and assume no unguarded recursion. */
            (void)depth;  /* Unused in this branch */
            return true;
        }

        case TY_EXCEPTION:
        case TY_CONT:
        case TY_CLONEABLE_CONT: {
            /* In v1, they store TypeKind, not Type */
            (void)depth;  /* Unused in this branch */
            return true;
        }

        /* Phase HRT0: Quantified types guard recursion (the forall/exists is itself a constructor) */
        case TY_FORALL:
        case TY_EXISTS: {
            int new_depth = depth + 1;
            return type_is_guarded_recursive_helper(t->as.forall_.body, rec_name, new_depth);
        }

        /* Leaf types - no recursion possible */
        case TY_UNKNOWN:
        case TY_NIL:
        case TY_BOOL:
        case TY_INT:
        case TY_FLOAT:
        case TY_INT8:
        case TY_INT16:
        case TY_INT32:
        case TY_INT64:
        case TY_UINT8:
        case TY_UINT16:
        case TY_UINT32:
        case TY_UINT64:
        case TY_FLOAT32:
        case TY_FLOAT64:
        case TY_CSTR:
        case TY_PTR_VOID:
        case TY_TYPECLASS:
        case TY_TYPECLASS_INST:
        case TY_NEVER:
        case TY_SET:
        /* Phase G0: ADT types guard recursion like structs */
        case TY_ADT:
        /* Phase G2: unresolved type variable — treated as opaque/guarded */
        case TY_TYVAR:
            return true;
        /* IT0: Union types — guard recursion like other type constructors */
        case TY_UNION: {
            int new_depth = depth + 1;
            for (uint8_t i = 0; i < t->as.union_.n_members; i++) {
                if (!type_is_guarded_recursive_helper(t->as.union_.members[i], rec_name, new_depth))
                    return false;
            }
            return true;
        }
        /* IT2: Intersection types — guard recursion like union types */
        case TY_INTERSECTION: {
            int new_depth = depth + 1;
            for (uint8_t i = 0; i < t->as.intersection_.n_members; i++) {
                if (!type_is_guarded_recursive_helper(t->as.intersection_.members[i], rec_name, new_depth))
                    return false;
            }
            return true;
        }
        /* IT4: any — top type; always safe (no recursive members) */
        case TY_ANY:
            return true;
        /* ET3: Handler type — leaf type; no recursive members */
        case TY_HANDLER:
            return true;
        /* CT0: Contract type — guarded by the base type constructor */
        case TY_CONTRACT:
            return type_is_guarded_recursive_helper(t->as.contract_.base_type, rec_name, depth + 1);
        /* SS0a: Session protocol types — guarded by their constructors.
         * TY_SESSION wraps the protocol (one constructor deep = depth+1 counts).
         * TY_CLOSE is a leaf; others have child protocol types. */
        case TY_CLOSE:
            return true;
        case TY_SESSION:
            return type_is_guarded_recursive_helper(t->as.session_.fst, rec_name, depth + 1);
        case TY_SEND:
        case TY_RECV:
        case TY_CHOOSE:
        case TY_BRANCH:
            return type_is_guarded_recursive_helper(t->as.session_.fst, rec_name, depth + 1)
                && (!t->as.session_.snd
                    || type_is_guarded_recursive_helper(t->as.session_.snd, rec_name, depth + 1));
        case TY_SESSION_REC:
            return type_is_guarded_recursive_helper(t->as.session_.fst, rec_name, depth + 1);
        case TY_TIMEOUT:
            return type_is_guarded_recursive_helper(t->as.session_.fst, rec_name, depth + 1)
                && (!t->as.session_.snd
                    || type_is_guarded_recursive_helper(t->as.session_.snd, rec_name, depth + 1));
        case TY_SESSION_PAIR:
        case TY_SESSION_RECV_PAIR:
        case TY_SESSION_OFFER:
            return type_is_guarded_recursive_helper(t->as.session_.fst, rec_name, depth + 1)
                && (!t->as.session_.snd
                    || type_is_guarded_recursive_helper(t->as.session_.snd, rec_name, depth + 1));
    }

    return true;  /* Unknown type kind - assume safe */
}

bool type_is_guarded_recursive(Type t, const char *rec_name) {
    /* Start with depth 0 - we need at least one type constructor
     * (TY_APP or TY_STRUCT) before we hit the recursive reference */
    return type_is_guarded_recursive_helper(&t, rec_name, 0);
}

const char *kind_to_string(Kind k) {
    switch (k) {
        case KIND_STAR:   return "*";
        case KIND_ARROW:  return "* -> *";
        case KIND_ARROW2: return "* -> * -> *";
        case KIND_ROW:    return "Row";
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
        case TY_INT8:     return "int8";
        case TY_INT16:    return "int16";
        case TY_INT32:    return "int32";
        case TY_INT64:    return "int64";
        case TY_UINT8:    return "uint8";
        case TY_UINT16:   return "uint16";
        case TY_UINT32:   return "uint32";
        case TY_UINT64:   return "uint64";
        case TY_FLOAT32:  return "float32";
        case TY_FLOAT64:  return "float64";
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
        case TY_ADT:      return "adt";
        case TY_NEVER:    return "!";
        case TY_SET:      return "set";
        /* Phase HRT0 */
        case TY_FORALL:   return "forall";
        case TY_EXISTS:   return "exists";
        /* Phase G2 */
        case TY_TYVAR:    return "tyvar";
        /* IT0: Union types */
        case TY_UNION:        return "union";
        /* IT2: Intersection types */
        case TY_INTERSECTION: return "intersection";
        /* IT4: Top type */
        case TY_ANY:          return "any";
        /* ET3: Handler type */
        case TY_HANDLER:      return "handler";
        /* CT0: Contract type */
        case TY_CONTRACT:     return "contract";
        /* Phase HKT-P1/P2 */
        case TY_APP:          return "app";
        case TY_REC:          return "rec";
        /* Phase LT3 */
        case TY_LREF:         return "lref";
        /* SS0a: Session protocol types */
        case TY_SESSION:      return "Session";
        case TY_SEND:         return "Send";
        case TY_RECV:         return "Recv";
        case TY_CLOSE:        return "Close";
        case TY_CHOOSE:       return "Choose";
        case TY_BRANCH:       return "Branch";
        case TY_SESSION_REC:       return "Rec";
        case TY_TIMEOUT:           return "Timeout";
        case TY_SESSION_PAIR:      return "SessionPair";
        case TY_SESSION_RECV_PAIR: return "RecvPair";
        case TY_SESSION_OFFER:     return "Offer";
        default:          return "<?>";
    }
}

/* Phase HRT0: Compute the rank of a type.
 * Rank 0 = monotype (no quantifiers).
 * Rank 1 = forall/exists at the outermost level.
 * Rank N = forall/exists nested N levels deep (each level adds 1).
 * Note: full rank computation requires Type* args in TY_FN (deferred to HRT1).
 */
int type_rank(const Type *t) {
    if (!t) return 0;
    switch (t->kind) {
        case TY_FORALL:
        case TY_EXISTS: {
            int body_rank = type_rank(t->as.forall_.body);
            return body_rank + 1;
        }
        default:
            return 0;
    }
}

/* IT0: Union type constructor.
 * Builds a TY_UNION type from an array of member types.
 * Nested TY_UNION members are flattened: (A | (B | C)) -> (A | B | C).
 * IT4: If any member is TY_ANY, the union simplifies to any.
 * The members array and its contents are allocated on the given arena. */
Type type_union_build(Arena *a, Type **members, uint8_t n_members) {
    /* IT4: If any member is TY_ANY, the whole union is any */
    for (uint8_t i = 0; i < n_members; i++) {
        if (members[i] && members[i]->kind == TY_ANY) {
            Type t;
            memset(&t, 0, sizeof(t));
            t.kind = TY_ANY;
            t.copy_kind = CK_COPY;
            t.hkt_kind = KIND_STAR;
            return t;
        }
    }

    /* First, compute flattened count */
    uint8_t flat_count = 0;
    for (uint8_t i = 0; i < n_members; i++) {
        if (members[i] && members[i]->kind == TY_UNION) {
            flat_count += members[i]->as.union_.n_members;
        } else {
            flat_count++;
        }
    }

    /* Allocate flattened array */
    Type **flat = (Type **)arena_alloc(a, flat_count * sizeof(Type *));
    uint8_t fi = 0;
    for (uint8_t i = 0; i < n_members; i++) {
        if (members[i] && members[i]->kind == TY_UNION) {
            /* Flatten nested union */
            for (uint8_t j = 0; j < members[i]->as.union_.n_members; j++) {
                flat[fi++] = members[i]->as.union_.members[j];
            }
        } else {
            flat[fi++] = members[i];
        }
    }

    Type t;
    memset(&t, 0, sizeof(t));
    t.kind = TY_UNION;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.union_.members = flat;
    t.as.union_.n_members = flat_count;
    return t;
}

/* IT2: Intersection type constructor.
 * Builds a TY_INTERSECTION type from an array of member types.
 * Nested TY_INTERSECTION members are flattened: (A & (B & C)) -> (A & B & C).
 * The members array and its contents are allocated on the given arena. */
Type type_intersection_build(Arena *a, Type **members, uint8_t n_members) {
    /* First, compute flattened count */
    uint8_t flat_count = 0;
    for (uint8_t i = 0; i < n_members; i++) {
        if (members[i] && members[i]->kind == TY_INTERSECTION) {
            flat_count += members[i]->as.intersection_.n_members;
        } else {
            flat_count++;
        }
    }

    /* Allocate flattened array */
    Type **flat = (Type **)arena_alloc(a, flat_count * sizeof(Type *));
    uint8_t fi = 0;
    for (uint8_t i = 0; i < n_members; i++) {
        if (members[i] && members[i]->kind == TY_INTERSECTION) {
            /* Flatten nested intersection */
            for (uint8_t j = 0; j < members[i]->as.intersection_.n_members; j++) {
                flat[fi++] = members[i]->as.intersection_.members[j];
            }
        } else {
            flat[fi++] = members[i];
        }
    }

    Type t;
    memset(&t, 0, sizeof(t));
    t.kind = TY_INTERSECTION;
    t.copy_kind = CK_MOVE;
    t.hkt_kind = KIND_STAR;
    t.as.intersection_.members = flat;
    t.as.intersection_.n_members = flat_count;
    return t;
}

TypeKind typekind_from_name(const char *name) {
    if (!name) return TY_UNKNOWN;
    if (strcmp(name, "unknown") == 0) return TY_UNKNOWN;
    if (strcmp(name, "nil") == 0) return TY_NIL;
    if (strcmp(name, "bool") == 0) return TY_BOOL;
    if (strcmp(name, "int") == 0) return TY_INT;
    if (strcmp(name, "int64") == 0) return TY_INT;      /* alias */
    if (strcmp(name, "float") == 0) return TY_FLOAT;
    if (strcmp(name, "float64") == 0) return TY_FLOAT;  /* alias */
    if (strcmp(name, "int8") == 0) return TY_INT8;
    if (strcmp(name, "int16") == 0) return TY_INT16;
    if (strcmp(name, "int32") == 0) return TY_INT32;
    if (strcmp(name, "uint8") == 0) return TY_UINT8;
    if (strcmp(name, "uint16") == 0) return TY_UINT16;
    if (strcmp(name, "uint32") == 0) return TY_UINT32;
    if (strcmp(name, "uint64") == 0) return TY_UINT64;
    if (strcmp(name, "float32") == 0) return TY_FLOAT32;
    if (strcmp(name, "cstr") == 0) return TY_CSTR;
    if (strcmp(name, "ptr-void") == 0 || strcmp(name, "ptr<void>") == 0) return TY_PTR_VOID;
    if (strcmp(name, "fn") == 0) return TY_FN;
    if (strcmp(name, "ref") == 0) return TY_REF;
    if (strcmp(name, "lref") == 0) return TY_LREF;
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
    /* Phase N: fixed-width numeric types */
    if (strcmp(name, "int8")   == 0) return TY_INT8;
    if (strcmp(name, "int16")  == 0) return TY_INT16;
    if (strcmp(name, "int32")  == 0) return TY_INT32;
    if (strcmp(name, "int64")  == 0) return TY_INT64;
    if (strcmp(name, "uint8")  == 0) return TY_UINT8;
    if (strcmp(name, "uint16") == 0) return TY_UINT16;
    if (strcmp(name, "uint32") == 0) return TY_UINT32;
    if (strcmp(name, "uint64") == 0) return TY_UINT64;
    if (strcmp(name, "float32") == 0) return TY_FLOAT32;
    if (strcmp(name, "float64") == 0) return TY_FLOAT64;
    if (strcmp(name, "set") == 0) return TY_SET;
    if (strcmp(name, "handler") == 0) return TY_HANDLER;
    /* SS0a: Session protocol types */
    if (strcmp(name, "Session") == 0) return TY_SESSION;
    if (strcmp(name, "Send") == 0)    return TY_SEND;
    if (strcmp(name, "Recv") == 0)    return TY_RECV;
    if (strcmp(name, "Close") == 0)   return TY_CLOSE;
    if (strcmp(name, "Choose") == 0)  return TY_CHOOSE;
    if (strcmp(name, "Branch") == 0)  return TY_BRANCH;
    if (strcmp(name, "Rec") == 0)     return TY_SESSION_REC;
    return TY_UNKNOWN;
}

/* ET3-D: type_is_subtype -- check if sub is a subtype of super_.
 * Returns true if sub is assignable where super_ is expected.
 * Rules:
 *   - TY_ANY as super_ accepts any subtype (top type).
 *   - TY_NEVER as sub is a subtype of everything (bottom type).
 *   - TY_CONTRACT as sub is a subtype of its base type (predicate already checked).
 *   - TY_HANDLER: covariant in result, contravariant in value (simplified: equality for now).
 *   - Otherwise: use type_eq.
 */
bool type_is_subtype(Type sub, Type super_) {
    if (super_.kind == TY_ANY) return true;
    if (sub.kind == TY_NEVER) return true;
    /* CT0: Contract type is a subtype of its base type */
    if (sub.kind == TY_CONTRACT && sub.as.contract_.base_type) {
        if (type_is_subtype(*sub.as.contract_.base_type, super_)) return true;
    }
    if (sub.kind == super_.kind) {
        if (sub.kind == TY_HANDLER) {
            if (sub.as.handler_.effect_name != super_.as.handler_.effect_name) return false;
            return sub.as.handler_.value_kind == super_.as.handler_.value_kind
                && sub.as.handler_.result_kind == super_.as.handler_.result_kind;
        }
        return type_eq(sub, super_);
    }
    return false;
}
