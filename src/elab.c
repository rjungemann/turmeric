#include "elab.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtins.h"
#include "diag.h"
#include "reader.h"    /* Phase M2: read_all for module loading */
#include "typeclass.h"  /* Phase 15 */
#include "types.h"
#include "effect.h"    /* Phase 19 */
#include "globals.h"   /* ET4: g_effect_types_enabled */

/* Phase U5: External declarations for global unsafe linting configuration */
extern uint32_t g_unsafe_max_lines;
extern bool g_unsafe_warn_nested;
extern bool g_unsafe_require_safety;
extern bool g_unsafe_stats_enabled;
extern bool g_lint_unsafe_enabled;
extern uint32_t g_unsafe_block_count;
extern uint32_t g_unsafe_total_lines;

/* Phase R6: Result/panic linting flags */
extern bool g_warn_unused_result;
extern bool g_lint_panic;

/* Phase G1: GADT feature flag */
extern bool g_gadt_enabled;

/* LT0: Linear types feature flag */
extern bool g_linear_enabled;

/* UT0: Uniqueness types feature flag */
extern bool g_unique_enabled;

/* ST0: Substructural types feature flag */
extern bool g_substructural_enabled;

/* IT0: Union types feature flag */
extern bool g_union_types_enabled;
extern bool g_intersection_types_enabled;

#define ELAB_MAX_MACRO_EXPANSION_DEPTH 256

/* Helper to create a Type from TypeKind. */
static Type type_from_kind(TypeKind k) {
    Type t;
    t.kind = k;
    t.copy_kind = typekind_default_copy_kind(k);
    t.as.fn.arity = 0;
    t.n_lifetimes = 0;  /* Phase 13: no lifetimes by default */
    t.typeclass_instances = NULL;
    t.n_typeclass_instances = 0;
    t.hkt_kind = KIND_STAR;  /* Phase HKT-P6: all types are kind * in v1 */
    return t;
}

/* Helper to convert a type name string to TypeKind (Phase 19). */
static TypeKind typekind_from_symbol(const char *name) {
    if (strcmp(name, "int") == 0) return TY_INT;
    if (strcmp(name, "int64") == 0) return TY_INT;       /* alias */
    if (strcmp(name, "bool") == 0) return TY_BOOL;
    if (strcmp(name, "float") == 0) return TY_FLOAT;
    if (strcmp(name, "float64") == 0) return TY_FLOAT;   /* alias */
    if (strcmp(name, "cstr") == 0) return TY_CSTR;
    if (strcmp(name, "nil") == 0) return TY_NIL;
    if (strcmp(name, "ptr-void") == 0) return TY_PTR_VOID;
    if (strcmp(name, "ref") == 0) return TY_REF;
    if (strcmp(name, "lref") == 0) return TY_LREF;
    if (strcmp(name, "rc") == 0) return TY_RC;
    if (strcmp(name, "weak") == 0) return TY_WEAK;
    if (strcmp(name, "exception") == 0) return TY_EXCEPTION;
    if (strcmp(name, "cont") == 0) return TY_CONT;
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
    /* IT4: Top type — available with -Xunion-types or -Xintersection-types */
    if (strcmp(name, "any") == 0) return TY_ANY;
    return TY_UNKNOWN;
}

/* Phase N: returns true if kind is any numeric type */
static bool typekind_is_numeric(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_FLOAT:
        case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32: case TY_FLOAT64:
            return true;
        default:
            return false;
    }
}

/* Helper to get the compile-time size of a type in bytes (0 = unknown). */
static int type_size_bytes(TypeKind kind) {
    switch (kind) {
        case TY_BOOL:     return 1;   /* bool → 1 byte in C */
        case TY_INT:      return 8;   /* int64_t → 8 bytes */
        case TY_FLOAT:    return 8;   /* double → 8 bytes */
        case TY_CSTR:     return 8;   /* const char* → pointer size */
        case TY_PTR_VOID: return 8;   /* void* → pointer size */
        case TY_NIL:      return 0;   /* unit / void — no size */
        case TY_REF:      return 8;   /* heap pointer */
        case TY_RC:       return 8;   /* rc pointer */
        case TY_WEAK:     return 8;   /* weak pointer */
        /* Phase N: fixed-width numeric types */
        case TY_INT8:   return 1;
        case TY_INT16:  return 2;
        case TY_INT32:  return 4;
        case TY_INT64:  return 8;
        case TY_UINT8:  return 1;
        case TY_UINT16: return 2;
        case TY_UINT32: return 4;
        case TY_UINT64: return 8;
        case TY_FLOAT32: return 4;
        case TY_FLOAT64: return 8;
        default:          return 0;   /* unknown / composite */
    }
}

/* IT3: Return true if t is a concrete (closed) type that can participate in
 * provable-disjointness checks.  TY_STRUCT/TY_ADT with a NULL def are type
 * variables and are therefore open -- not concrete. */
static bool type_is_concrete_for_disjoint(Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_CSTR:
        case TY_NIL: case TY_PTR_VOID:
        case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32: case TY_FLOAT64:
            return true;
        case TY_STRUCT:
            /* NULL def means it's a type variable -- not a concrete named struct */
            return t->as.struct_.def != NULL;
        case TY_ADT:
            return t->as.adt_.def != NULL;
        default:
            return false;
    }
}

/* IT3: Return true if TypeKind k (for a primitive) is a concrete base kind.
 * Used from the intersection-member-mismatch check where we only have a kind. */
static bool typekind_is_concrete_for_disjoint(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_CSTR:
        case TY_NIL: case TY_PTR_VOID:
        case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32: case TY_FLOAT64:
        /* For struct/ADT we cannot check concreteness without the full Type* here;
         * the caller should use type_is_concrete_for_disjoint() instead when possible. */
        case TY_STRUCT: case TY_ADT:
            return true;
        default:
            return false;
    }
}

/* IT3: Check whether two concrete types are provably disjoint.
 * Returns true if it is impossible for a single value to satisfy both types.
 * Only catches the obvious cases (distinct primitives, distinct named structs,
 * distinct named ADTs). Returns false for any pair involving a typeclass, forall,
 * type variable, or other open type. */
static bool types_provably_disjoint(Type *a, Type *b) {
    if (!a || !b) return false;
    /* A type is never disjoint from itself */
    if (type_eq(*a, *b)) return false;
    /* Only check pairs where both sides are fully concrete */
    if (!type_is_concrete_for_disjoint(a) ||
        !type_is_concrete_for_disjoint(b)) return false;
    TypeKind ka = a->kind, kb = b->kind;
    /* Two different primitive kinds are disjoint */
    if (ka != kb) return true;
    /* Same kind -- check identity for nominal types */
    if (ka == TY_STRUCT) {
        StructDef *da = a->as.struct_.def;
        StructDef *db = b->as.struct_.def;
        if (da && db && da->name && db->name && da->name != db->name) return true;
    }
    if (ka == TY_ADT) {
        AdtDef *da = a->as.adt_.def;
        AdtDef *db = b->as.adt_.def;
        if (da && db && da->name && db->name && da->name != db->name) return true;
    }
    return false;
}

/* ---- scope ---- */

/* Phase 12: Borrow tracking in Scope */
typedef enum BorrowKind {
    BK_IMMUT,   /* &T - immutable borrow */
    BK_MUT,     /* &mut T - mutable borrow */
} BorrowKind;

typedef struct ScopeBorrow {
    Binding *binding;       /* The binding being borrowed */
    BorrowKind kind;        /* BK_IMMUT or BK_MUT */
    struct ScopeBorrow *next; /* Next in the list */
} ScopeBorrow;

typedef struct Scope {
    struct Scope *parent;
    Binding     **bindings;
    uint32_t      n;
    uint32_t      cap;
    /* Phase 12: Active borrows in this scope */
    ScopeBorrow  *borrows;
} Scope;

static void scope_init(Scope *s, Scope *parent) {
    s->parent = parent;
    s->bindings = NULL;
    s->n = 0;
    s->cap = 0;
    s->borrows = NULL;
}

static void scope_free(Scope *s) {
    free(s->bindings);
    /* Free borrow list */
    ScopeBorrow *b = s->borrows;
    while (b) {
        ScopeBorrow *next = b->next;
        free(b);
        b = next;
    }
    s->bindings = NULL;
    s->n = s->cap = 0;
    s->borrows = NULL;
}

/* Phase 12: Check if a binding has an active borrow that conflicts with the requested kind */
static bool scope_borrow_conflicts(const Scope *s, Binding *binding, BorrowKind kind) {
    for (const Scope *cur = s; cur; cur = cur->parent) {
        for (ScopeBorrow *b = cur->borrows; b; b = b->next) {
            if (b->binding == binding) {
                /* Same binding is borrowed - check for conflict */
                if (kind == BK_MUT) {
                    /* &mut T cannot coexist with any other borrow of T */
                    return true;
                }
                if (b->kind == BK_MUT) {
                    /* Existing &mut T conflicts with new &T */
                    return true;
                }
                /* Both are &T - allowed (multiple immutable borrows) */
            }
        }
    }
    return false;
}

/* Phase 12: Add a borrow to the current scope */
static bool scope_add_borrow(Scope *s, Binding *binding, BorrowKind kind, Span span) {
    if (scope_borrow_conflicts(s, binding, kind)) {
        /* Conflict - emit error */
        if (kind == BK_MUT) {
            diag_emit(DIAG_ERROR, span,
                      "cannot borrow `%s` as mutable while it is already borrowed",
                      binding->name->name);
        } else {
            diag_emit(DIAG_ERROR, span,
                      "cannot borrow `%s` as immutable while it is mutably borrowed",
                      binding->name->name);
        }
        return false;
    }
    
    /* Add to this scope's borrow list */
    ScopeBorrow *b = (ScopeBorrow *)malloc(sizeof(ScopeBorrow));
    if (!b) { fprintf(stderr, "tur: oom\n"); abort(); }
    b->binding = binding;
    b->kind = kind;
    b->next = s->borrows;
    s->borrows = b;
    return true;
}

static void scope_add(Scope *s, Binding *b) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 4;
        s->bindings = (Binding **)realloc(s->bindings, s->cap * sizeof(Binding *));
        if (!s->bindings) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    s->bindings[s->n++] = b;
}

static Binding *scope_lookup(Scope *s, const Symbol *name) {
    for (Scope *cur = s; cur; cur = cur->parent) {
        for (uint32_t i = cur->n; i > 0; i--) {
            Binding *b = cur->bindings[i - 1];
            if (b->name == name) return b;
        }
    }
    return NULL;
}

/* Phase 3: Collect free variables in an expression that are not in the given
 * param bindings. Returns a malloc'd list of captured Binding pointers. */
static Binding **collect_free_vars(const Expr *e, Binding **params, uint8_t n_params,
                                  uint32_t *n_out) {
    /* Pre-pass: collect all bindings introduced by `let` forms anywhere within
     * this expression.  These are locally defined — they must never be treated
     * as free variables even when referenced inside a nested `let` body.
     * Binding comparison is by pointer identity, so two `let` forms that bind
     * the same name but produce distinct Binding* objects don't interfere. */
    Binding **local_defs = NULL;
    uint32_t  n_local = 0, cap_local = 0;
    {
        const Expr **ls = (const Expr **)malloc(256 * sizeof(const Expr *));
        int lsp = 0;
        ls[lsp++] = e;
        while (lsp > 0) {
            const Expr *cur = ls[--lsp];
            if (!cur) continue;
            switch (cur->kind) {
                case EX_LET:
                    for (uint32_t i = 0; i < cur->as.let_.n; i++) {
                        Binding *b = cur->as.let_.bindings[i].binding;
                        if (b) {
                            if (n_local >= cap_local) {
                                cap_local = cap_local ? cap_local * 2 : 8;
                                local_defs = (Binding **)realloc(local_defs,
                                    cap_local * sizeof(Binding *));
                            }
                            local_defs[n_local++] = b;
                        }
                        ls[lsp++] = cur->as.let_.bindings[i].init;
                    }
                    ls[lsp++] = cur->as.let_.body;
                    break;
                case EX_IF:
                    if (cur->as.if_.else_or_null) ls[lsp++] = cur->as.if_.else_or_null;
                    ls[lsp++] = cur->as.if_.then_;
                    ls[lsp++] = cur->as.if_.cond;
                    break;
                case EX_DO:
                    for (uint32_t i = cur->as.do_.n; i > 0; i--)
                        ls[lsp++] = cur->as.do_.items[i-1];
                    break;
                case EX_WHILE:
                    ls[lsp++] = cur->as.while_.body;
                    ls[lsp++] = cur->as.while_.cond;
                    break;
                case EX_SET:   ls[lsp++] = cur->as.set_.value;      break;
                case EX_DEF:   ls[lsp++] = cur->as.def_.init;       break;
                case EX_BUILTIN:
                    for (uint32_t i = cur->as.builtin.n; i > 0; i--)
                        ls[lsp++] = cur->as.builtin.args[i-1];
                    break;
                case EX_CALL:
                    for (uint32_t i = cur->as.call_.n_args; i > 0; i--)
                        ls[lsp++] = cur->as.call_.args[i-1];
                    break;
                case EX_FN_DEF:  ls[lsp++] = cur->as.fn_def_.fn->body; break;
                case EX_RETURN:
                    if (cur->as.return_.value) ls[lsp++] = cur->as.return_.value;
                    break;
                case EX_PANIC:   ls[lsp++] = cur->as.panic_.payload;   break;
                case EX_RC_DROP: ls[lsp++] = cur->as.rc_drop_.expr;    break;
                case EX_DEFER:   ls[lsp++] = cur->as.defer_.body;      break;
                case EX_RC_OF:   ls[lsp++] = cur->as.rc_of_.expr;      break;
                case EX_GET_FIELD: ls[lsp++] = cur->as.get_field_.struct_expr; break;
                case EX_MAKE_STRUCT:
                    for (uint32_t i = cur->as.make_struct_.n_fields; i > 0; i--)
                        ls[lsp++] = cur->as.make_struct_.field_values[i-1];
                    break;
                case EX_SET_LIT:
                    for (uint32_t i = cur->as.set_lit_.n; i > 0; i--)
                        ls[lsp++] = cur->as.set_lit_.items[i-1];
                    break;
                case EX_HANDLE: {
                    /* Handle case params (effect args + k) are locally defined.
                     * Register them so collect_free_vars doesn't treat them as
                     * free variables when the handler body is inside an outer
                     * closure (e.g. an async block). */
                    HandleExpr *handle = cur->as.handle_.handle;
                    ls[lsp++] = handle->body;
                    for (uint8_t ci = 0; ci < handle->n_cases; ci++) {
                        HandleCase *hc = &handle->cases[ci];
                        /* Register k binding as local */
                        if (hc->k_binding) {
                            if (n_local >= cap_local) {
                                cap_local = cap_local ? cap_local * 2 : 8;
                                local_defs = (Binding **)realloc(local_defs,
                                    cap_local * sizeof(Binding *));
                            }
                            local_defs[n_local++] = hc->k_binding;
                        }
                        /* Register effect param bindings as local */
                        for (uint8_t pi = 0; pi < hc->n_params; pi++) {
                            if (hc->param_bindings && hc->param_bindings[pi]) {
                                if (n_local >= cap_local) {
                                    cap_local = cap_local ? cap_local * 2 : 8;
                                    local_defs = (Binding **)realloc(local_defs,
                                        cap_local * sizeof(Binding *));
                                }
                                local_defs[n_local++] = hc->param_bindings[pi];
                            }
                        }
                        ls[lsp++] = hc->body;
                    }
                    break;
                }
                default: break;
            }
        }
        free(ls);
    }

    Binding **result = NULL;
    uint32_t cap = 0;
    uint32_t n = 0;

    /* Simple recursive traversal using a stack */
    const Expr **stack = (const Expr **)malloc(256 * sizeof(const Expr *));
    int sp = 0;
    stack[sp++] = e;

    while (sp > 0) {
        const Expr *cur = stack[--sp];

        if (cur->kind == EX_VAR) {
            /* Check if this is a param or global */
            bool is_param = false;
            for (uint8_t i = 0; i < n_params; i++) {
                if (params[i] == cur->as.var.binding) {
                    is_param = true;
                    break;
                }
            }
            if (!is_param && !cur->as.var.binding->is_global) {
                /* Exclude bindings introduced by `let` within this expression —
                 * they are locally defined, not free variables. */
                bool is_local = false;
                for (uint32_t i = 0; i < n_local; i++) {
                    if (local_defs[i] == cur->as.var.binding) {
                        is_local = true;
                        break;
                    }
                }
                if (!is_local) {
                    /* This is a free variable - check if it's already in result */
                    bool found = false;
                    for (uint32_t i = 0; i < n; i++) {
                        if (result[i] == cur->as.var.binding) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        if (n >= cap) {
                            cap = cap ? cap * 2 : 8;
                            result = (Binding **)realloc(result, cap * sizeof(Binding *));
                        }
                        result[n++] = cur->as.var.binding;
                    }
                }
            }
            continue;
        }

        /* Traverse children (in reverse order for depth-first) */
        switch (cur->kind) {
            case EX_LET:
                for (uint32_t i = cur->as.let_.n; i > 0; i--) {
                    stack[sp++] = cur->as.let_.bindings[i-1].init;
                }
                stack[sp++] = cur->as.let_.body;
                break;
            case EX_IF:
                if (cur->as.if_.else_or_null) stack[sp++] = cur->as.if_.else_or_null;
                stack[sp++] = cur->as.if_.then_;
                stack[sp++] = cur->as.if_.cond;
                break;
            case EX_DO:
                for (uint32_t i = cur->as.do_.n; i > 0; i--) {
                    stack[sp++] = cur->as.do_.items[i-1];
                }
                break;
            case EX_WHILE:
                stack[sp++] = cur->as.while_.body;
                stack[sp++] = cur->as.while_.cond;
                break;
            case EX_SET:
                stack[sp++] = cur->as.set_.value;
                break;
            case EX_DEF:
                stack[sp++] = cur->as.def_.init;
                break;
            case EX_BUILTIN:
                for (uint32_t i = cur->as.builtin.n; i > 0; i--) {
                    stack[sp++] = cur->as.builtin.args[i-1];
                }
                break;
            case EX_CALL:
                for (uint32_t i = cur->as.call_.n_args; i > 0; i--) {
                    stack[sp++] = cur->as.call_.args[i-1];
                }
                break;
            case EX_FN_DEF:
                stack[sp++] = cur->as.fn_def_.fn->body;
                break;
            case EX_RC_DROP:
                stack[sp++] = cur->as.rc_drop_.expr;
                break;
            case EX_DEFER:
                stack[sp++] = cur->as.defer_.body;
                break;
            case EX_RC_OF:
                stack[sp++] = cur->as.rc_of_.expr;
                break;
            case EX_RC_FROM_REF:
                stack[sp++] = cur->as.rc_from_ref_.expr;
                break;
            case EX_REF_FROM_RC:
                stack[sp++] = cur->as.ref_from_rc_.expr;
                break;
            case EX_WEAK:
                stack[sp++] = cur->as.weak_.expr;
                break;
            case EX_WEAK_UPGRADE:
                stack[sp++] = cur->as.weak_upgrade_.expr;
                break;
            case EX_RC_CLONE:
                stack[sp++] = cur->as.rc_clone_.expr;
                break;
            case EX_RC_PTR:
                stack[sp++] = cur->as.rc_ptr_.expr;
                break;
            case EX_RC_COUNT:
                stack[sp++] = cur->as.rc_count_.expr;
                break;
            case EX_WEAK_PRED:
                stack[sp++] = cur->as.weak_pred_.expr;
                break;
            case EX_REF_PRED:
                stack[sp++] = cur->as.ref_pred_.expr;
                break;
            /* Phase 5: ref/deref */
            case EX_REF:
                stack[sp++] = cur->as.ref_.expr;
                break;
            case EX_DEREF:
                stack[sp++] = cur->as.deref_.expr;
                break;
            /* Phase 12: Borrow traits */
            case EX_BORROW_IMMUT:
                stack[sp++] = cur->as.borrow_immut_.expr;
                break;
            case EX_BORROW_MUT:
                stack[sp++] = cur->as.borrow_mut_.expr;
                break;
            case EX_SET_DEREF:
                stack[sp++] = cur->as.set_deref_.ref;
                stack[sp++] = cur->as.set_deref_.value;
                break;
            case EX_MAKE_STRUCT:
                for (uint32_t i = cur->as.make_struct_.n_fields; i > 0; i--) {
                    stack[sp++] = cur->as.make_struct_.field_values[i-1];
                }
                break;
            case EX_SET_LIT:
                for (uint32_t i = cur->as.set_lit_.n; i > 0; i--) {
                    stack[sp++] = cur->as.set_lit_.items[i-1];
                }
                break;
            case EX_GET_FIELD:
                stack[sp++] = cur->as.get_field_.struct_expr;
                break;
            /* Phase 19: Algebraic effects */
            case EX_PERFORM:
                for (uint8_t i = cur->as.perform_.perform->n_args; i > 0; i--) {
                    stack[sp++] = cur->as.perform_.perform->args[i-1];
                }
                break;
            case EX_HANDLE:
                stack[sp++] = cur->as.handle_.handle->body;
                for (uint8_t i = cur->as.handle_.handle->n_cases; i > 0; i--) {
                    stack[sp++] = cur->as.handle_.handle->cases[i-1].body;
                }
                break;
            case EX_RESUME:
                stack[sp++] = cur->as.resume_.resume->k;
                stack[sp++] = cur->as.resume_.resume->value;
                break;
            case EX_DISCONTINUE:
                stack[sp++] = cur->as.discontinue_.discontinue->k;
                stack[sp++] = cur->as.discontinue_.discontinue->exception;
                break;
            /* Phase T21: Async/await */
            case EX_ASYNC:
                stack[sp++] = cur->as.async_.fn_expr;
                break;
            case EX_AWAIT:
                stack[sp++] = cur->as.await_.fut_expr;
                break;
            /* Phase 3/4: Return */
            case EX_RETURN:
                if (cur->as.return_.value) stack[sp++] = cur->as.return_.value;
                break;
            /* Phase R2: Panic */
            case EX_PANIC:
                stack[sp++] = cur->as.panic_.payload;
                break;
            default:
                break;
        }
    }
    free(stack);
    free(local_defs);
    *n_out = n;
    return result;
}

/* ---- elaborator state ---- */

/* Phase M2: A loaded module's exported bindings. */
typedef struct ElabModule {
    const Symbol *name;          /* module name (interned) */
    Binding     **exports;       /* exported bindings (each has .name for lookup) */
    uint32_t      n_exports;
    /* Phase M4: exported macros from this module */
    struct MacroDef **exported_macros;
    uint32_t         n_exported_macros;
    /* PR5-3-D: Effects exported by this module */
    Effect      **exported_effects;
    uint32_t      n_exported_effects;
    bool          is_loading;    /* circular import detection */
} ElabModule;

typedef struct Elab {
    Arena       *arena;
    SymbolTable *st;
    Scope       *scope;     /* current */
    Scope        global;
    uint32_t     next_id;
    uint32_t     next_gensym_id;  /* Phase 6: for generating unique symbol names */

    /* Phase 3: Collect file-scope definitions (FN_DEF) from nested contexts */
    Expr       **file_scope_defs;
    uint32_t    n_file_scope_defs;
    uint32_t    cap_file_scope_defs;
    /* Phase 15: Typeclass environment */
    TypeClassEnv typeclass_env;

    /* Cached symbols for special-form dispatch. */
    const Symbol *sym_def;
    const Symbol *sym_let;
    const Symbol *sym_if;
    const Symbol *sym_do;
    const Symbol *sym_unsafe;
    const Symbol *sym_when;
    const Symbol *sym_unless;
    const Symbol *sym_case;
    const Symbol *sym_set;
    const Symbol *sym_while;
    const Symbol *sym_defn;     /* Phase 2 */
    const Symbol *sym_fn;       /* Phase 2 */
    const Symbol *sym_lambda;   /* λ — Unicode alias for fn */
    const Symbol *sym_extern_c; /* Phase 2 */
    const Symbol *sym_caret_mut;     /* ^mut */
    const Symbol *sym_caret_private; /* ^private — module-private visibility annotation */
    /* Phase P3: HAMT lowering */
    const Symbol *sym_caret_persistent; /* ^persistent — immutable map annotation */
    /* LT0: Linear types */
    const Symbol *sym_caret_linear;     /* ^linear — linear value annotation */
    /* UT0: Uniqueness types */
    const Symbol *sym_caret_unique;     /* ^unique -- unique value annotation */
    /* ST0: Substructural types */
    const Symbol *sym_caret_affine;     /* ^affine -- affine value annotation */
    const Symbol *sym_caret_relevant;   /* ^relevant -- relevant value annotation */
    const Symbol *sym_caret_extends;    /* ^extends -- effect hierarchy parent annotation (ET4) */
    const Symbol *sym_map_new;    /* map-new - create new map */
    const Symbol *sym_assoc;      /* assoc - insert/update key-value */
    const Symbol *sym_dissoc;     /* dissoc - delete key */
    const Symbol *sym_map_get;    /* get - get value by key */
    const Symbol *sym_map_has;    /* has? - check key exists */
    const Symbol *sym_map_count;  /* count - number of entries */
    const Symbol *sym_map_merge;  /* merge - merge two maps */
    /* HAMT function symbols for lowering */
    const Symbol *sym_hamt_new;   /* hamt/new */
    const Symbol *sym_hamt_set;   /* hamt/set */
    const Symbol *sym_hamt_del;   /* hamt/del */
    const Symbol *sym_hamt_get;   /* hamt/get */
    const Symbol *sym_hamt_has;   /* hamt/has? */
    const Symbol *sym_hamt_count; /* hamt/count */
    const Symbol *sym_hamt_merge; /* hamt/merge */
    const Symbol *sym_hamt_hash_ptr; /* hamt_hash_ptr */
    const Symbol *sym_defer;      /* Phase 4 */
    const Symbol *sym_return;     /* return - early return with defer firing */
    /* Phase 5 */
    const Symbol *sym_ref;        /* ref */
    const Symbol *sym_deref;      /* @ (deref operator) - stored as symbol for parsing */
    const Symbol *sym_drop;       /* drop! - explicit drop for ref<T> */
    const Symbol *kw_else;         /* :else (the symbol named "else") */
    const Symbol *kw_derive;       /* :as (the symbol named "as") - for inline-C */
    /* LT3: lref<T> — linear owning pointer */
    const Symbol *sym_lref;        /* lref (type name in type expressions) */
    const Symbol *sym_lref_new;    /* lref/new */
    /* Phase 9: rc<T> + weak<T> */
    const Symbol *sym_rc_of;       /* rc/of */
    const Symbol *sym_rc_clone;    /* rc/clone */
    const Symbol *sym_rc_drop;     /* rc/drop */
    const Symbol *sym_rc_ptr;      /* rc->ptr */
    const Symbol *sym_rc_strong_count; /* rc/strong-count */
    const Symbol *sym_rc_from_ref; /* rc/from-ref */
    const Symbol *sym_ref_from_rc; /* ref/from-rc */
    const Symbol *sym_weak;        /* weak */
    const Symbol *sym_upgrade;     /* upgrade */
    const Symbol *sym_weak_pred;   /* weak? */
    const Symbol *sym_ref_pred;    /* ref? */
    /* Phase 18: Delimited continuations */
    const Symbol *sym_reset;      /* reset */
    const Symbol *sym_shift;      /* shift */
    const Symbol *sym_shift0;     /* shift0 */
    const Symbol *sym_call_cc;    /* call/cc (sugar: (reset (shift k (f k)))) */
    const Symbol *sym_escape;     /* escape (sugar: (reset (shift k (f (fn [_] k))))) */
    /* Phase B2: Cloneable continuations */
    const Symbol *sym_cloneable_reset;   /* cloneable-reset */
    const Symbol *sym_cloneable_shift;   /* cloneable-shift */
    const Symbol *sym_call_cc_star;       /* call/cc* (sugar for cloneable call/cc) */
    /* Phase 21: Serializable continuations */
    const Symbol *sym_serial_reset;      /* serial-reset */
    const Symbol *sym_serial_shift;      /* serial-shift */
    /* Phase 19: Algebraic effects */
    const Symbol *sym_defeffect;  /* defeffect */
    const Symbol *sym_perform;    /* perform */
    const Symbol *sym_handle;     /* handle */
    const Symbol *sym_try_with;   /* try-with (sugar for handle) */
    const Symbol *sym_with_handler; /* with-handler (sugar for handle; async-friendly alias) */
    const Symbol *sym_resume;     /* resume */
    const Symbol *sym_discontinue;/* discontinue */
    const Symbol *sym_k;          /* continuation parameter name k */
    const Symbol *sym_effect_unsafe; /* built-in effect name: Unsafe */
    /* ET3: handler type expression and compose-handlers */
    const Symbol *sym_handler_type;     /* "handler" type expression keyword */
    const Symbol *sym_compose_handlers; /* "compose-handlers" call form */
    /* Phase U3: Unsafe primitives - pointer operations */
    const Symbol *sym_ptr_deref;   /* ptr-deref */
    const Symbol *sym_ptr_write;  /* ptr-write */
    const Symbol *sym_ptr_add;     /* ptr-add */
    const Symbol *sym_ptr_sub;     /* ptr-sub */
    const Symbol *sym_ptr_nullq;   /* ptr-null? */
    const Symbol *sym_ptr_of;      /* ptr-of */
    /* Phase U3: Unsafe primitives - type casting */
    const Symbol *sym_unsafe_cast; /* unsafe-cast */
    const Symbol *sym_reinterpret; /* reinterpret */
    const Symbol *sym_transmute;   /* transmute */
    /* Phase U3: Unsafe primitives - unchecked array ops */
    const Symbol *sym_array_get_unchecked;  /* array-get-unchecked */
    const Symbol *sym_array_set_unchecked;  /* array-set-unchecked */
    /* Phase U3: Unsafe primitives - raw memory */
    const Symbol *sym_raw_malloc;  /* raw-malloc */
    const Symbol *sym_raw_free;    /* raw-free */
    const Symbol *sym_raw_realloc; /* raw-realloc */
    const Symbol *sym_raw_memcpy;  /* raw-memcpy */
    const Symbol *sym_raw_memset;  /* raw-memset */
    /* Phase U3: Unsafe primitives - FFI */
    const Symbol *sym_c_call;      /* c-call */
    const Symbol *sym_dlopen;      /* dlopen */
    const Symbol *sym_dlsym;       /* dlsym */
    const Symbol *sym_dlclose;     /* dlclose */
    /* Phase 10: GC */
    const Symbol *sym_gc_force;    /* gc! */
    const Symbol *sym_gc_enable;   /* gc-enable! */
    const Symbol *sym_gc_disable;  /* gc-disable! */
    /* Phase 6: Macro system */
    const Symbol *sym_defmacro;   /* defmacro */
    const Symbol *sym_quote;      /* quote */
    const Symbol *sym_quasiquote; /* quasiquote */
    const Symbol *sym_unquote;    /* unquote */
    const Symbol *sym_unquote_splicing; /* unquote-splicing */
    const Symbol *sym_gensym;     /* gensym */
    const Symbol *sym_thread;    /* -> threading macro */
    const Symbol *sym_thread_last; /* ->> threading macro */
    /* Phase 11: defstruct */
    const Symbol *sym_defstruct;   /* defstruct */
    const Symbol *sym_make_struct; /* make-struct */
    const Symbol *kw_copy;        /* :copy keyword for defstruct */
    const Symbol *kw_move;        /* :move keyword for defstruct */
    const Symbol *kw_linear;      /* LT4: :linear keyword for defstruct (exactly-once) */
    /* Phase 12: Borrow traits */
    const Symbol *sym_borrow;      /* & symbol for immutable borrow */
    const Symbol *sym_borrow_mut;  /* &mut for mutable borrow */
    /* Phase 15: Typeclasses */
    const Symbol *sym_defclass;    /* defclass */
    const Symbol *sym_definstance; /* definstance */
    /* Phase HKT H5: defkind — kind alias declarations */
    const Symbol *sym_defkind;     /* defkind */
    /* Phase HKT-P2: defrec — recursive type binders */
    const Symbol *sym_defrec;      /* defrec */
    const Symbol *sym_deftype;      /* deftype */
    /* Phase HKT-P1: type-app — type-level application */
    const Symbol *sym_type_app;    /* type-app */
    /* Phase HRT0: Higher-ranked type quantifiers (type-level only; reject in expression position) */
    const Symbol *sym_forall;      /* forall */
    const Symbol *sym_exists;      /* exists */
    const Symbol *sym_forall_u;    /* ∀ — Unicode alias for forall */
    const Symbol *sym_exists_u;    /* ∃ — Unicode alias for exists */
    /* Phase HRT1: Rank-2 type expression forms */
    const Symbol *sym_arrow;       /* -> (function type constructor in type expressions) */
    const Symbol *sym_ascribe;     /* :: (type ascription operator) */
    /* Phase HRT2: Existential type intro/elim */
    const Symbol *sym_pack;        /* pack (existential introduction) */
    const Symbol *sym_open;        /* open (existential elimination) */
    /* Phase HKT (v2): reserved typeclass names — user definitions rejected with diagnostic */
    const Symbol *sym_hkt_Functor;
    const Symbol *sym_hkt_Applicative;
    const Symbol *sym_hkt_Monad;
    const Symbol *sym_hkt_Traversable;
    const Symbol *sym_hkt_Foldable;
    /* Phase R2: Panic */
    const Symbol *sym_panic;
    const Symbol *sym_panic_with;
    const Symbol *sym_catch_unwind;
    const Symbol *sym_catch_panic_of;
    const Symbol *sym_throw;         /* (throw expr) */
    const Symbol *sym_try;           /* (try body (catch ...) ...) */
    /* Phase R5: no-unwind attribute */
    const Symbol *sym_no_unwind_attr;
    /* Phase M6: (export-as "c_name") attribute for explicit C symbol naming */
    const Symbol *sym_export_as_attr;
    const Symbol *sym_panic_payload_type;
    const Symbol *sym_panic_payload_value;
    const Symbol *sym_panic_payload_file;
    const Symbol *sym_panic_payload_line;
    const Symbol *sym_panic_payload_downcast;
    /* Phase R1: ? operator (reserved; not yet implemented) */
    const Symbol *sym_question;
    /* Phase T19-B: thread-spawn form — (thread-spawn closure) */
    const Symbol *sym_thread_spawn;
    /* Phase T21-F: async/await forms */
    const Symbol *sym_async;
    const Symbol *sym_await;
    /* Phase SEL1: fair multi-channel select */
    const Symbol *sym_select;
    const Symbol *sym_recv;   /* :recv keyword */
    const Symbol *sym_send;   /* :send keyword */
    /* Phase 20: Software Transactional Memory */
    const Symbol *sym_stm;           /* stm */
    const Symbol *sym_atomically;    /* atomically */
    const Symbol *sym_retry;         /* retry */
    const Symbol *sym_check;         /* check */
    const Symbol *sym_or_else;       /* or-else */
    const Symbol *sym_tvar;          /* tvar (type name) */
    const Symbol *sym_new;           /* new (for tvar/new, etc.) */
    const Symbol *sym_read;          /* read (for tvar/read, etc.) */
    const Symbol *sym_write;         /* write (for tvar/write, etc.) */
    const Symbol *sym_modify;        /* modify (for tvar/modify, etc.) */
    const Symbol *sym_swap;          /* swap (for tvar/swap, etc.) */
    const Symbol *sym_cas;           /* cas (for tvar/cas, etc.) */
    const Symbol *sym_tmvar;         /* tmvar (type name) */
    const Symbol *sym_tchan;         /* tchan (type name) */
    const Symbol *sym_tsem;          /* tsem (type name) */
    const Symbol *sym_dosync;        /* dosync (macro) */
    const Symbol *sym_with_tvar;     /* with-tvar (macro) */
    /* Legacy symbols for TVar:: syntax (kept for compatibility) */
    const Symbol *sym_tvar_new;      /* tvar/new */
    const Symbol *sym_tvar_read;     /* tvar/read */
    const Symbol *sym_tvar_write;    /* tvar/write */
    const Symbol *sym_tvar_modify;   /* tvar/modify */
    const Symbol *sym_tvar_swap;     /* tvar/swap */
    const Symbol *sym_tvar_cas;      /* tvar/cas */
    /* Phase N: (as TargetType expr) numeric cast */
    const Symbol *sym_as;
    /* IT4 gradual typing */
    const Symbol *sym_type_of;  /* (type-of x) — cstr name of an any-typed value's type */
    const Symbol *sym_cast;     /* (cast x T) — unsafe downcast from any to T */
    /* Phase 13: Lifetime annotations */
    /* We recognize lifetime annotations as symbols starting with '\'' */
    /* No specific symbol needed - we check the symbol name at runtime */
    /* Macro storage: symbol -> MacroDef */
    /* For now, use a simple approach: store macros in a list */
    struct MacroDef **macros;
    uint32_t n_macros;
    uint32_t cap_macros;
    /* Phase 19: Algebraic effects */
    EffectEnv *effect_env;  /* Global effect registry */
    /* Phase 19 TUR-E0008: Effect-scope tracking.
     * handled_effect_names: stack of effect names covered by enclosing handle.
     * fn_body_depth: depth inside defn/fn bodies; TUR-E0008 only fires at 0. */
    const Symbol **handled_effect_names;
    uint32_t n_handled_effects;
    uint32_t cap_handled_effects;
    uint32_t fn_body_depth;
    const Symbol *current_fn_name;  /* Phase R6: track current function name for linting */
    uint32_t unsafe_depth;
    uint32_t macro_expand_depth;
    /* Phase U5: Unsafe linting configuration */
    uint32_t unsafe_max_lines;      /* max lines in unsafe block before warning (0 = disabled) */
    bool     unsafe_warn_nested;     /* warn on nested unsafe blocks */
    bool     unsafe_require_safety; /* require ;;; SAFETY: comments in unsafe blocks */
    bool     unsafe_stats_enabled;  /* track unsafe block statistics */
    uint32_t unsafe_block_count;    /* count of unsafe blocks seen */
    uint32_t unsafe_total_lines;    /* total lines in unsafe blocks */
    /* Phase 19: cont? predicate */
    const Symbol *sym_cont_pred; /* cont? */
    /* Phase 11: Struct registry - maps struct names to StructDef */
    StructDef **struct_defs;
    uint32_t n_struct_defs;
    uint32_t cap_struct_defs;
    /* Phase G0: ADT registry - maps ADT names to AdtDef */
    AdtDef **adt_defs;
    uint32_t n_adt_defs;
    uint32_t cap_adt_defs;
    /* Phase G0: ADT special symbols */
    const Symbol *sym_defdata;
    const Symbol *sym_match;
    /* Phase G1: GADT special symbols */
    const Symbol *sym_defgadt;
    const Symbol *sym_colon; /* bare ":" used as annotation separator in defgadt */
    /* IT0: Union type pipe separator "|" */
    const Symbol *sym_pipe;        /* "|" -- used in (A | B | C) union type expressions */
    /* IT2: Intersection type ampersand separator "&" */
    const Symbol *sym_ampersand;   /* "&" -- used in (A & B & C) intersection type expressions */
    /* Phase G2: current per-arm skolem environment (NULL outside GADT match arms) */
    SkolemEnv *g2_skolem_env;
    /* Phase G2: GADT constructor whose arm is currently being elaborated.
     * NULL outside GADT match arms.  Used by diagnostics to name which
     * constructor's return-type annotation caused a type refinement. */
    const CtorDef *g2_current_ctor;
    /* Phase G3: coerce special form */
    const Symbol *sym_coerce;
    /* Phase G3: (~ a b) equality constraint notation */
    const Symbol *sym_tilde;
    /* Phase M0: Module system */
    const Symbol *sym_defmodule;  /* defmodule */
    const Symbol *sym_export;     /* export */
    const Symbol *sym_effect;     /* effect — used to parse (effect Name) in export/refer lists */
    const Symbol *sym_import;     /* import */
    const Symbol *sym_load;       /* load */
    const Symbol *kw_as;          /* :as */
    const Symbol *kw_refer;       /* :refer */
    bool has_defmodule;           /* whether defmodule has been seen in this file */
    /* Phase M1: Module namespace system */
    const Symbol    *current_module_name; /* name of module being elaborated, or NULL */
    const DefModule *current_module;      /* DefModule being elaborated, or NULL */
    /* Phase M2: Module registry */
    const char      *module_base_dir;     /* base dir for resolving module file paths */
    const char      *module_stdlib_dir;   /* stdlib fallback dir (e.g. "stdlib") */
    const char     **module_include_dirs; /* extra search dirs from -I flags / spices */
    int              n_module_include_dirs;
    struct ElabModule *loaded_modules;    /* registry of loaded modules */
    uint32_t         n_loaded_modules;
    uint32_t         cap_loaded_modules;
    uint16_t         next_import_file_id; /* file_id counter for imported source files */
    /* Phase M3: Separate compilation — skip inlining imported modules */
    bool             separate_compilation;
    /* Phase M4: During macro expansion, the defining module of the currently
     * expanding macro (so private helper macros from that module are visible). */
    const Symbol    *macro_expansion_module;
    /* Phase B2 CPS-CL7: tracks nesting depth of cloneable-reset for
     * detecting cloneable-shift outside any reset boundary. */
    int              cloneable_reset_depth;
    /* Phase 21: tracks nesting depth of serial-reset for detecting
     * serial-shift outside any serial-reset boundary. */
    int              serial_reset_depth;
    /* Phase P3: HAMT lowering - track if HAMT functions are used */
    bool             needs_hamt;
    /* PR5-3-D: Effects brought into scope via :refer [(effect Name)] imports */
    Effect      **referred_effects;
    uint32_t      n_referred_effects;
    uint32_t      cap_referred_effects;
    /* Phase RF0: forward-declared type symbols for mutual recursion support.
     * Names added here during the type pre-pass are allowed to be re-elaborated
     * by defstruct/defdata without triggering "already defined" errors. */
    const Symbol   **forward_type_syms;
    uint32_t         n_forward_type_syms;
    uint32_t         cap_forward_type_syms;
} Elab;

/* Phase 6: Macro definition */
typedef struct MacroDef {
    const Symbol *name;
    Form **params;
    uint32_t n_params;       /* number of fixed params (excludes rest param) */
    bool is_variadic;        /* true if [params & rest] syntax used */
    const Symbol *rest_param; /* rest-arg symbol name, or NULL */
    Form *body;
    Span span;
    /* Phase M4: module that defined this macro (NULL = stdlib/pre-module) */
    const Symbol *defining_module_name;
    /* Phase M4: true when injected via :refer — visible in any module context
     * but defining_module_name still holds the original module so private
     * helpers of that module remain accessible during expansion. */
    bool is_referred;
} MacroDef;

typedef enum CtValueTag {
    CT_VAL_FORM = 0,
    CT_VAL_FN,
} CtValueTag;

typedef struct CtEnv CtEnv;
typedef struct CtFn CtFn;

typedef struct CtValue {
    CtValueTag tag;
    union {
        Form *form;
        CtFn *fn;
    } as;
} CtValue;

typedef struct CtBinding {
    const Symbol *name;
    CtValue value;
    struct CtBinding *next;
} CtBinding;

struct CtEnv {
    CtEnv *parent;
    CtBinding *bindings;
    Elab *elab;
    bool *ok;
};

struct CtFn {
    Form **params;
    uint32_t n_params;
    bool is_variadic;
    const Symbol *rest_param;
    Form *body;
    CtEnv *env;
    Span span;
};

/* Phase 3: Register a file-scope definition to be emitted later */
static void elab_register_file_def(Elab *e, Expr *def_expr) {
    if (!def_expr) return;
    if (e->n_file_scope_defs >= e->cap_file_scope_defs) {
        e->cap_file_scope_defs = e->cap_file_scope_defs ? e->cap_file_scope_defs * 2 : 16;
        e->file_scope_defs = (Expr **)realloc(e->file_scope_defs, 
            e->cap_file_scope_defs * sizeof(Expr *));
    }
    e->file_scope_defs[e->n_file_scope_defs++] = def_expr;
}

static const Symbol *intern_cstr(SymbolTable *st, const char *s) {
    return symtab_intern(st, strslice(s, (uint32_t)strlen(s)));
}

/* Phase 11: Copy/Move trait tracking */

/* Mark a binding as moved (poisoned). Returns true if successfully marked,
 * false if it was already moved (use-after-move). */
static bool binding_mark_moved(Binding *b, Span use_span) {
    if (b->is_moved) {
        return false; /* Already moved - use-after-move */
    }
    b->is_moved = true;
    b->moved_at = use_span;
    return true;
}

/* Check if a binding has been moved. Emits use-after-move diagnostic if so. */
static bool binding_check_not_moved(Binding *b, Span use_span, const char *use_desc) {
    if (b->is_moved) {
        /* Emit use-after-move error */
        diag_emit_with_code(DIAG_ERROR, use_span, TUR_E0005_USE_AFTER_MOVE,
                            "use-after-move: %s '%s' was moved and cannot be used again",
                            use_desc, b->name->name);
        if (!span_is_unknown(b->moved_at)) {
            diag_emit(DIAG_NOTE, b->moved_at, "moved here");
        }
        return false;
    }
    return true;
}

/* Snapshot/restore move-state for all currently visible bindings.
 * Used to make branch elaboration path-sensitive for move tracking. */
static uint32_t move_state_snapshot_bindings(const Scope *scope,
                                             Binding ***out_bindings,
                                             bool **out_states) {
    uint32_t n = 0;
    uint32_t cap = 16;
    Binding **bindings = (Binding **)malloc(cap * sizeof(Binding *));
    bool *states = (bool *)malloc(cap * sizeof(bool));
    if (!bindings || !states) {
        fprintf(stderr, "tur: oom\n");
        abort();
    }

    for (const Scope *cur = scope; cur; cur = cur->parent) {
        for (uint32_t i = 0; i < cur->n; i++) {
            if (n == cap) {
                cap *= 2;
                bindings = (Binding **)realloc(bindings, cap * sizeof(Binding *));
                states = (bool *)realloc(states, cap * sizeof(bool));
                if (!bindings || !states) {
                    fprintf(stderr, "tur: oom\n");
                    abort();
                }
            }
            bindings[n] = cur->bindings[i];
            states[n] = cur->bindings[i]->is_moved;
            n++;
        }
    }

    *out_bindings = bindings;
    *out_states = states;
    return n;
}

static bool *move_state_capture_current(Binding **bindings, uint32_t n) {
    bool *states = (bool *)malloc((n == 0 ? 1 : n) * sizeof(bool));
    if (!states) {
        fprintf(stderr, "tur: oom\n");
        abort();
    }
    for (uint32_t i = 0; i < n; i++) {
        states[i] = bindings[i]->is_moved;
    }
    return states;
}

static void move_state_restore(Binding **bindings, const bool *states, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        bindings[i]->is_moved = states[i];
    }
}

/* LT1: Parallel helpers for is_linear_consumed state across branches.
 * Only linear bindings (is_linear == true) are included in the snapshot. */

static uint32_t linear_state_snapshot_bindings(const Scope *scope,
                                                Binding ***out_bindings,
                                                bool **out_states) {
    uint32_t n = 0;
    uint32_t cap = 16;
    Binding **bindings = (Binding **)malloc(cap * sizeof(Binding *));
    bool *states = (bool *)malloc(cap * sizeof(bool));
    if (!bindings || !states) {
        fprintf(stderr, "tur: oom\n");
        abort();
    }

    for (const Scope *cur = scope; cur; cur = cur->parent) {
        for (uint32_t i = 0; i < cur->n; i++) {
            Binding *b = cur->bindings[i];
            if (!b->is_linear) continue;
            if (n == cap) {
                cap *= 2;
                bindings = (Binding **)realloc(bindings, cap * sizeof(Binding *));
                states = (bool *)realloc(states, cap * sizeof(bool));
                if (!bindings || !states) {
                    fprintf(stderr, "tur: oom\n");
                    abort();
                }
            }
            bindings[n] = b;
            states[n] = b->is_linear_consumed;
            n++;
        }
    }

    *out_bindings = bindings;
    *out_states = states;
    return n;
}

static bool *linear_state_capture_current(Binding **bindings, uint32_t n) {
    bool *states = (bool *)malloc((n == 0 ? 1 : n) * sizeof(bool));
    if (!states) {
        fprintf(stderr, "tur: oom\n");
        abort();
    }
    for (uint32_t i = 0; i < n; i++) {
        states[i] = bindings[i]->is_linear_consumed;
    }
    return states;
}

static void linear_state_restore(Binding **bindings, const bool *states, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        bindings[i]->is_linear_consumed = states[i];
    }
}

/* Check if an RC binding is consumed by ref/from-rc or explicitly dropped via rc/drop.
 * Returns true if the binding is consumed (should skip auto-drop to avoid double-free). */
static bool is_rc_binding_consumed(const Expr *body, Binding *binding) {
    if (!body) return false;
    
    const Expr **stack = (const Expr **)malloc(256 * sizeof(const Expr *));
    if (!stack) {
        fprintf(stderr, "tur: oom\n");
        abort();
    }
    int sp = 0;
    
    /* Start by pushing body onto stack */
    stack[sp++] = body;
    
    while (sp > 0) {
        const Expr *cur = stack[--sp];
        if (!cur) continue;
        
        /* Check if this expression consumes the binding via ref/from-rc */
        if (cur->kind == EX_REF_FROM_RC &&
            cur->as.ref_from_rc_.expr &&
            cur->as.ref_from_rc_.expr->kind == EX_VAR &&
            cur->as.ref_from_rc_.expr->as.var.binding == binding) {
            free(stack);
            return true;
        }
        
        /* Check if this expression consumes the binding via rc/drop */
        if (cur->kind == EX_RC_DROP &&
            cur->as.rc_drop_.expr &&
            cur->as.rc_drop_.expr->kind == EX_VAR &&
            cur->as.rc_drop_.expr->as.var.binding == binding) {
            free(stack);
            return true;
        }
        
        /* Recursively traverse sub-expressions */
        switch (cur->kind) {
            case EX_LET:
                for (uint32_t i = cur->as.let_.n; i > 0; i--) {
                    stack[sp++] = cur->as.let_.bindings[i-1].init;
                }
                stack[sp++] = cur->as.let_.body;
                break;
            case EX_IF:
                if (cur->as.if_.else_or_null) stack[sp++] = cur->as.if_.else_or_null;
                stack[sp++] = cur->as.if_.then_;
                stack[sp++] = cur->as.if_.cond;
                break;
            case EX_DO:
                for (uint32_t i = cur->as.do_.n; i > 0; i--) {
                    stack[sp++] = cur->as.do_.items[i-1];
                }
                break;
            case EX_CALL:
                for (uint32_t i = cur->as.call_.n_args; i > 0; i--) {
                    stack[sp++] = cur->as.call_.args[i-1];
                }
                break;
            case EX_BUILTIN:
                for (uint32_t i = cur->as.builtin.n; i > 0; i--) {
                    stack[sp++] = cur->as.builtin.args[i-1];
                }
                break;
            case EX_CLOSURE:
                if (cur->as.closure_.closure && cur->as.closure_.closure->fn) {
                    stack[sp++] = cur->as.closure_.closure->fn->body;
                }
                break;
            case EX_DEFER:
                stack[sp++] = cur->as.defer_.body;
                break;
            case EX_WHILE:
                stack[sp++] = cur->as.while_.body;
                stack[sp++] = cur->as.while_.cond;
                break;
            case EX_PANIC:
                stack[sp++] = cur->as.panic_.payload;
                break;
            case EX_SET:
                stack[sp++] = cur->as.set_.value;
                break;
            case EX_SET_DEREF:
                stack[sp++] = cur->as.set_deref_.ref;
                stack[sp++] = cur->as.set_deref_.value;
                break;
            case EX_MAKE_STRUCT:
                for (uint32_t i = cur->as.make_struct_.n_fields; i > 0; i--) {
                    stack[sp++] = cur->as.make_struct_.field_values[i-1];
                }
                break;
            case EX_SET_LIT:
                for (uint32_t i = cur->as.set_lit_.n; i > 0; i--) {
                    stack[sp++] = cur->as.set_lit_.items[i-1];
                }
                break;
            case EX_GET_FIELD:
                stack[sp++] = cur->as.get_field_.struct_expr;
                break;
            default:
                break;
        }
    }

    free(stack);
    return false;
}

static void elab_init_state(Elab *e, Arena *arena, SymbolTable *st) {
    e->arena = arena;
    e->st = st;
    scope_init(&e->global, NULL);
    e->scope = &e->global;
    e->next_id = 0;
    e->next_gensym_id = 0;  /* Phase 6 */
    /* Phase 3: file-scope defs collection */
    e->file_scope_defs = NULL;
    e->n_file_scope_defs = 0;
    e->cap_file_scope_defs = 0;
    /* Phase 15: Typeclass environment */
    typeclass_env_init(&e->typeclass_env, arena);
    /* Phase 19: Effect environment */
    e->effect_env = effect_env_new(arena);

    e->sym_def       = intern_cstr(st, "def");
    e->sym_let       = intern_cstr(st, "let");
    e->sym_if        = intern_cstr(st, "if");
    e->sym_do        = intern_cstr(st, "do");
    e->sym_unsafe    = intern_cstr(st, "unsafe");
    e->sym_when      = intern_cstr(st, "when");
    e->sym_unless    = intern_cstr(st, "unless");
    e->sym_case      = intern_cstr(st, "case");
    e->sym_set       = intern_cstr(st, "set!");
    e->sym_while     = intern_cstr(st, "while");
    e->sym_defn      = intern_cstr(st, "defn");
    e->sym_fn        = intern_cstr(st, "fn");
    e->sym_lambda    = intern_cstr(st, "\xce\xbb"); /* λ */
    e->sym_extern_c  = intern_cstr(st, "extern-c");
    e->sym_caret_mut     = intern_cstr(st, "^mut");
    e->sym_caret_private = intern_cstr(st, "^private");
    /* Phase P3: HAMT lowering */
    e->sym_caret_persistent = intern_cstr(st, "^persistent");
    /* LT0: Linear types */
    e->sym_caret_linear = intern_cstr(st, "^linear");
    /* UT0: Uniqueness types */
    e->sym_caret_unique = intern_cstr(st, "^unique");
    /* ST0: Substructural types */
    e->sym_caret_affine    = intern_cstr(st, "^affine");
    e->sym_caret_relevant  = intern_cstr(st, "^relevant");
    e->sym_caret_extends   = intern_cstr(st, "^extends");  /* ET4: effect hierarchy */
    e->sym_map_new = intern_cstr(st, "map-new");
    e->sym_assoc = intern_cstr(st, "assoc");
    e->sym_dissoc = intern_cstr(st, "dissoc");
    e->sym_map_get = intern_cstr(st, "get");
    e->sym_map_has = intern_cstr(st, "has?");
    e->sym_map_count = intern_cstr(st, "count");
    e->sym_map_merge = intern_cstr(st, "merge");
    /* HAMT function symbols for lowering */
    e->sym_hamt_new = intern_cstr(st, "hamt/new");
    e->sym_hamt_set = intern_cstr(st, "hamt/set");
    e->sym_hamt_del = intern_cstr(st, "hamt/del");
    e->sym_hamt_get = intern_cstr(st, "hamt/get");
    e->sym_hamt_has = intern_cstr(st, "hamt/has?");
    e->sym_hamt_count = intern_cstr(st, "hamt/count");
    e->sym_hamt_merge = intern_cstr(st, "hamt/merge");
    e->sym_hamt_hash_ptr = intern_cstr(st, "hamt/hash-ptr");
    e->sym_defer     = intern_cstr(st, "defer");
    e->sym_return    = intern_cstr(st, "return");
    /* Phase 5 */
    e->sym_ref       = intern_cstr(st, "ref");
    e->sym_deref     = intern_cstr(st, "deref");
    e->sym_drop      = intern_cstr(st, "drop!");
    e->kw_else       = intern_cstr(st, "else");
    e->kw_derive     = intern_cstr(st, "as");
    /* LT3: lref<T> — linear owning pointer */
    e->sym_lref     = intern_cstr(st, "lref");
    e->sym_lref_new = intern_cstr(st, "lref/new");
    /* Phase 9: rc<T> + weak<T> */
    e->sym_rc_of = intern_cstr(st, "rc/of");
    e->sym_rc_clone = intern_cstr(st, "rc/clone");
    e->sym_rc_drop = intern_cstr(st, "rc/drop");
    e->sym_rc_ptr = intern_cstr(st, "rc->ptr");
    e->sym_rc_strong_count = intern_cstr(st, "rc/strong-count");
    e->sym_rc_from_ref = intern_cstr(st, "rc/from-ref");
    e->sym_ref_from_rc = intern_cstr(st, "ref/from-rc");
    e->sym_weak = intern_cstr(st, "weak");
    e->sym_upgrade = intern_cstr(st, "upgrade");
    e->sym_weak_pred = intern_cstr(st, "weak?");
    e->sym_ref_pred  = intern_cstr(st, "ref?");
    /* Phase 18: Delimited continuations */
    e->sym_reset = intern_cstr(st, "reset");
    e->sym_shift = intern_cstr(st, "shift");
    e->sym_shift0 = intern_cstr(st, "shift0");
    e->sym_call_cc = intern_cstr(st, "call/cc");
    e->sym_escape = intern_cstr(st, "escape");
    /* Phase B2: Cloneable continuations */
    e->sym_cloneable_reset = intern_cstr(st, "cloneable-reset");
    e->sym_cloneable_shift = intern_cstr(st, "cloneable-shift");
    e->sym_call_cc_star = intern_cstr(st, "call/cc*");
    /* Phase 21: Serializable continuations */
    e->sym_serial_reset = intern_cstr(st, "serial-reset");
    e->sym_serial_shift = intern_cstr(st, "serial-shift");
    /* Phase 19: Algebraic effects */
    e->sym_defeffect = intern_cstr(st, "defeffect");
    e->sym_perform = intern_cstr(st, "perform");
    e->sym_handle = intern_cstr(st, "handle");
    e->sym_try_with = intern_cstr(st, "try-with");
    e->sym_with_handler = intern_cstr(st, "with-handler");
    e->sym_resume = intern_cstr(st, "resume");
    e->sym_discontinue = intern_cstr(st, "discontinue");
    e->sym_k = intern_cstr(st, "k");
    e->sym_cont_pred = intern_cstr(st, "cont?");
    e->sym_effect_unsafe = intern_cstr(st, EFFECT_NAME_UNSAFE);
    effect_env_register_builtin_unsafe(e->effect_env, e->arena, e->sym_effect_unsafe);
    /* ET3: handler type expression and compose-handlers */
    e->sym_handler_type     = intern_cstr(st, "handler");
    e->sym_compose_handlers = intern_cstr(st, "compose-handlers");
    /* Phase U3: Unsafe primitives - pointer operations */
    e->sym_ptr_deref = intern_cstr(st, "ptr-deref");
    e->sym_ptr_write = intern_cstr(st, "ptr-write");
    e->sym_ptr_add = intern_cstr(st, "ptr-add");
    e->sym_ptr_sub = intern_cstr(st, "ptr-sub");
    e->sym_ptr_nullq = intern_cstr(st, "ptr-null?");
    e->sym_ptr_of = intern_cstr(st, "ptr-of");
    /* Phase U3: Unsafe primitives - type casting */
    e->sym_unsafe_cast = intern_cstr(st, "unsafe-cast");
    e->sym_reinterpret = intern_cstr(st, "reinterpret");
    e->sym_transmute = intern_cstr(st, "transmute");
    /* Phase U3: Unsafe primitives - unchecked array ops */
    e->sym_array_get_unchecked = intern_cstr(st, "array-get-unchecked");
    e->sym_array_set_unchecked = intern_cstr(st, "array-set-unchecked");
    /* Phase U3: Unsafe primitives - raw memory */
    e->sym_raw_malloc = intern_cstr(st, "raw-malloc");
    e->sym_raw_free = intern_cstr(st, "raw-free");
    e->sym_raw_realloc = intern_cstr(st, "raw-realloc");
    e->sym_raw_memcpy = intern_cstr(st, "raw-memcpy");
    e->sym_raw_memset = intern_cstr(st, "raw-memset");
    /* Phase U3: Unsafe primitives - FFI */
    e->sym_c_call = intern_cstr(st, "c-call");
    e->sym_dlopen = intern_cstr(st, "dlopen");
    e->sym_dlsym = intern_cstr(st, "dlsym");
    e->sym_dlclose = intern_cstr(st, "dlclose");
    /* Phase 19 TUR-E0008: Effect scope tracking */
    e->handled_effect_names = NULL;
    e->n_handled_effects = 0;
    e->cap_handled_effects = 0;
    e->fn_body_depth = 0;
    e->unsafe_depth = 0;
    e->macro_expand_depth = 0;
    /* Phase U5: Unsafe linting configuration */
    e->unsafe_max_lines = 20;      /* default threshold */
    e->unsafe_warn_nested = false;  /* disabled by default */
    e->unsafe_require_safety = false;/* disabled by default */
    e->unsafe_stats_enabled = false; /* disabled by default */
    e->unsafe_block_count = 0;
    e->unsafe_total_lines = 0;
    /* Phase 10: GC */
    e->sym_gc_force = intern_cstr(st, "gc!");
    e->sym_gc_enable = intern_cstr(st, "gc-enable!");
    e->sym_gc_disable = intern_cstr(st, "gc-disable!");
    /* Phase 6 */
    e->sym_defmacro = intern_cstr(st, "defmacro");
    e->sym_quote = intern_cstr(st, "quote");
    e->sym_quasiquote = intern_cstr(st, "quasiquote");
    e->sym_unquote = intern_cstr(st, "unquote");
    e->sym_unquote_splicing = intern_cstr(st, "unquote-splicing");
    e->sym_gensym = intern_cstr(st, "gensym");
    e->sym_thread = intern_cstr(st, "->");
    e->sym_thread_last = intern_cstr(st, "->>");
    e->next_gensym_id = 0;  /* Phase 6 */
    /* Phase 11: defstruct */
    e->sym_defstruct = intern_cstr(st, "defstruct");
    e->sym_make_struct = intern_cstr(st, "make-struct");
    e->kw_copy = intern_cstr(st, "copy");
    e->kw_move = intern_cstr(st, "move");
    e->kw_linear = intern_cstr(st, "linear"); /* LT4 */
    /* Phase 11: struct registry */
    e->struct_defs = NULL;
    e->n_struct_defs = 0;
    e->cap_struct_defs = 0;
    /* Phase G0: ADT registry */
    e->adt_defs = NULL;
    e->n_adt_defs = 0;
    e->cap_adt_defs = 0;
    e->sym_defdata = intern_cstr(st, "defdata");
    e->sym_match = intern_cstr(st, "match");
    /* Phase G1: GADT */
    e->sym_defgadt = intern_cstr(st, "defgadt");
    e->sym_colon = intern_cstr(st, ":");
    /* IT0: Union type pipe separator */
    e->sym_pipe = intern_cstr(st, "|");
    /* IT2: Intersection type ampersand separator */
    e->sym_ampersand = intern_cstr(st, "&");
    /* Phase G2: per-arm skolem environment (NULL until inside a GADT match arm) */
    e->g2_skolem_env = NULL;
    /* Phase G3: coerce */
    e->sym_coerce = intern_cstr(st, "coerce");
    /* Phase G3: (~ a b) equality constraint */
    e->sym_tilde = intern_cstr(st, "~");
    /* Phase 12: Borrow traits */
    e->sym_borrow = intern_cstr(st, "&");
    e->sym_borrow_mut = intern_cstr(st, "&mut");
    /* Phase 15: Typeclasses */
    e->sym_defclass = intern_cstr(st, "defclass");
    e->sym_definstance = intern_cstr(st, "definstance");
    /* Phase HKT H5: defkind */
    e->sym_defkind = intern_cstr(st, "defkind");
    /* Phase HKT-P2: defrec */
    e->sym_defrec = intern_cstr(st, "defrec");
    e->sym_deftype = intern_cstr(st, "deftype");
    /* Phase HKT-P1: type-app */
    e->sym_type_app = intern_cstr(st, "type-app");
    /* Phase HRT0: forall/exists */
    e->sym_forall = intern_cstr(st, "forall");
    e->sym_exists = intern_cstr(st, "exists");
    e->sym_forall_u = intern_cstr(st, "\xe2\x88\x80"); /* ∀ */
    e->sym_exists_u = intern_cstr(st, "\xe2\x88\x83"); /* ∃ */
    /* Phase HRT1: -> and :: */
    e->sym_arrow   = intern_cstr(st, "->");
    e->sym_ascribe = intern_cstr(st, "::");
    /* Phase HRT2: pack and open */
    e->sym_pack = intern_cstr(st, "pack");
    e->sym_open = intern_cstr(st, "open");
    /* Phase HKT (v2): reserved typeclass names */
    e->sym_hkt_Functor      = intern_cstr(st, "Functor");
    e->sym_hkt_Applicative  = intern_cstr(st, "Applicative");
    e->sym_hkt_Monad        = intern_cstr(st, "Monad");
    e->sym_hkt_Traversable  = intern_cstr(st, "Traversable");
    e->sym_hkt_Foldable     = intern_cstr(st, "Foldable");
    /* Phase R2: Panic */
    e->sym_panic = intern_cstr(st, "panic");
    e->sym_panic_with = intern_cstr(st, "panic-with");
    e->sym_catch_unwind = intern_cstr(st, "catch-unwind");
    e->sym_catch_panic_of = intern_cstr(st, "catch-panic-of");
    e->sym_throw = intern_cstr(st, "throw");
    e->sym_try   = intern_cstr(st, "try");
    e->sym_panic_payload_type = intern_cstr(st, "panic-payload-type");
    /* Phase R5: no-unwind attribute */
    e->sym_no_unwind_attr = intern_cstr(st, "#no-unwind");
    /* Phase M6: (export-as "c_name") attribute head symbol */
    e->sym_export_as_attr = intern_cstr(st, "export-as");
    e->sym_panic_payload_value = intern_cstr(st, "panic-payload-value");
    e->sym_panic_payload_file = intern_cstr(st, "panic-payload-file");
    e->sym_panic_payload_line = intern_cstr(st, "panic-payload-line");
    e->sym_panic_payload_downcast = intern_cstr(st, "panic-payload-downcast");
    /* Phase R1: ? operator */
    e->sym_question = intern_cstr(st, "?");
    /* Phase T19-B: thread-spawn */
    e->sym_thread_spawn = intern_cstr(st, "thread-spawn");
    /* Phase T21-F: async/await */
    e->sym_async = intern_cstr(st, "async");
    e->sym_await = intern_cstr(st, "await");
    /* Phase SEL1: fair multi-channel select */
    e->sym_select = intern_cstr(st, "select");
    e->sym_recv   = intern_cstr(st, "recv");
    e->sym_send   = intern_cstr(st, "send");
    /* Phase 20: Software Transactional Memory */
    e->sym_stm = intern_cstr(st, "stm");
    e->sym_atomically = intern_cstr(st, "atomically");
    e->sym_retry = intern_cstr(st, "retry");
    e->sym_check = intern_cstr(st, "check");
    e->sym_or_else = intern_cstr(st, "or-else");
    e->sym_tvar = intern_cstr(st, "tvar");
    e->sym_new = intern_cstr(st, "new");
    e->sym_read = intern_cstr(st, "read");
    e->sym_write = intern_cstr(st, "write");
    e->sym_modify = intern_cstr(st, "modify");
    e->sym_swap = intern_cstr(st, "swap");
    e->sym_cas = intern_cstr(st, "cas");
    e->sym_tmvar = intern_cstr(st, "tmvar");
    e->sym_tchan = intern_cstr(st, "tchan");
    e->sym_tsem = intern_cstr(st, "tsem");
    e->sym_dosync = intern_cstr(st, "dosync");
    e->sym_with_tvar = intern_cstr(st, "with-tvar");
    /* Legacy symbols for tvar/ syntax */
    e->sym_tvar_new = intern_cstr(st, "tvar/new");
    e->sym_tvar_read = intern_cstr(st, "tvar/read");
    e->sym_tvar_write = intern_cstr(st, "tvar/write");
    e->sym_tvar_modify = intern_cstr(st, "tvar/modify");
    e->sym_tvar_swap = intern_cstr(st, "tvar/swap");
    e->sym_tvar_cas = intern_cstr(st, "tvar/cas");
    /* Macro storage */
    e->macros = NULL;
    e->n_macros = 0;
    e->cap_macros = 0;
    /* Phase M0: Module system */
    e->sym_defmodule = intern_cstr(st, "defmodule");
    e->sym_export = intern_cstr(st, "export");
    e->sym_effect = intern_cstr(st, "effect");
    e->sym_import = intern_cstr(st, "import");
    e->sym_load = intern_cstr(st, "load");
    e->sym_as = intern_cstr(st, "as");   /* Phase N: (as Type expr) cast */
    e->sym_type_of = intern_cstr(st, "type-of");  /* IT4: (type-of x) */
    e->sym_cast    = intern_cstr(st, "cast");      /* IT4: (cast x T) */
    e->kw_as = intern_cstr(st, "as");   /* same interned symbol, used as :as keyword */
    e->kw_refer = intern_cstr(st, "refer");
    e->has_defmodule = false;
    e->current_module_name = NULL;
    e->current_module = NULL;
    /* Phase M2: Module registry */
    e->module_base_dir = ".";
    e->loaded_modules = NULL;
    e->n_loaded_modules = 0;
    e->cap_loaded_modules = 0;
    e->next_import_file_id = 10; /* 0-9 reserved for main + stdlib files */
    e->separate_compilation = false;
    e->macro_expansion_module = NULL;
    e->cloneable_reset_depth = 0;
    e->serial_reset_depth = 0;
    /* Phase P3: HAMT lowering */
    e->needs_hamt = false;
    /* PR5-3-D: referred effects */
    e->referred_effects     = NULL;
    e->n_referred_effects   = 0;
    e->cap_referred_effects = 0;
    /* Phase PKG-1: extra module search dirs (-I) */
    e->module_include_dirs = NULL;
    e->n_module_include_dirs = 0;
    /* Phase RF0: forward type declaration tracking */
    e->forward_type_syms = NULL;
    e->n_forward_type_syms = 0;
    e->cap_forward_type_syms = 0;
}

/* Phase 13: Lifetime annotation helpers (deferred - infrastructure in place) */
/* Phase 15: Typeclass cached symbols */

/* Phase 6: Macro lookup */
static MacroDef *elab_lookup_macro(Elab *e, const Symbol *name) {
    for (uint32_t i = 0; i < e->n_macros; i++) {
        MacroDef *m = e->macros[i];
        if (m->name != name) continue;
        /* Phase M4: visibility rules.
         * - is_referred: injected via :refer — always visible.
         * - defining_module_name == NULL: stdlib/pre-module — always visible.
         * - defining_module_name == current module: visible within the defining module.
         * - defining_module_name == macro_expansion_module: private helper called from
         *   an exported macro of the same module that is currently being expanded. */
        if (m->is_referred) return m;
        if (m->defining_module_name == NULL) return m;
        if (m->defining_module_name == e->current_module_name) return m;
        if (e->macro_expansion_module != NULL &&
            m->defining_module_name == e->macro_expansion_module) return m;
    }
    return NULL;
}

static CtValue ct_value_form(Form *f) {
    CtValue v;
    v.tag = CT_VAL_FORM;
    v.as.form = f;
    return v;
}

static CtValue ct_value_fn(CtFn *fn) {
    CtValue v;
    v.tag = CT_VAL_FN;
    v.as.fn = fn;
    return v;
}

static CtEnv *ct_env_new(Elab *e, CtEnv *parent, bool *ok) {
    CtEnv *env = (CtEnv *)arena_alloc(e->arena, sizeof(CtEnv));
    env->parent = parent;
    env->bindings = NULL;
    env->elab = e;
    env->ok = ok;
    return env;
}

static void ct_env_bind(CtEnv *env, const Symbol *name, CtValue v) {
    CtBinding *b = (CtBinding *)arena_alloc(env->elab->arena, sizeof(CtBinding));
    b->name = name;
    b->value = v;
    b->next = env->bindings;
    env->bindings = b;
}

static bool ct_env_lookup(CtEnv *env, const Symbol *name, CtValue *out) {
    for (CtEnv *cur = env; cur; cur = cur->parent) {
        for (CtBinding *b = cur->bindings; b; b = b->next) {
            if (b->name == name) {
                *out = b->value;
                return true;
            }
        }
    }
    return false;
}

static bool compile_time_truthy(Form *f) {
    return !(f->tag == F_NIL || (f->tag == F_BOOL && !f->as.b));
}

static bool ct_form_equal(const Form *a, const Form *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->tag != b->tag) return false;
    switch (a->tag) {
        case F_NIL: return true;
        case F_BOOL: return a->as.b == b->as.b;
        case F_INT: return a->as.i == b->as.i;
        case F_FLOAT: return a->as.f == b->as.f;
        case F_STR:
            return a->as.s.len == b->as.s.len &&
                   strncmp(a->as.s.p, b->as.s.p, a->as.s.len) == 0;
        case F_SYM:
        case F_KEYWORD:
            return a->as.sym == b->as.sym;
        case F_QUOTE:
        case F_QUASIQUOTE:
        case F_UNQUOTE:
        case F_UNQUOTE_SPLICING:
        case F_TYPE_ANN:
        case F_LIST:
        case F_VEC:
        case F_MAP:
        case F_SET:
            if (a->as.list.len != b->as.list.len) return false;
            for (uint32_t i = 0; i < a->as.list.len; i++) {
                if (!ct_form_equal(a->as.list.items[i], b->as.list.items[i])) return false;
            }
            return true;
        case F_CBLOCK:
            return a->as.cblock.len == b->as.cblock.len &&
                   strncmp(a->as.cblock.p, b->as.cblock.p, a->as.cblock.len) == 0;
    }
    return false;
}

static CtValue ct_eval_form(CtEnv *env, Form *f);

static Form *ct_value_to_form(CtEnv *env, CtValue v, Span span) {
    if (v.tag == CT_VAL_FORM) return v.as.form;
    *env->ok = false;
    diag_emit(DIAG_ERROR, span, "compile-time macro evaluation expected a form value");
    return form_nil(env->elab->arena, span);
}

static Form *ct_eval_quasiquote(CtEnv *env, Form *f) {
    switch (f->tag) {
        case F_UNQUOTE: {
            CtValue v = ct_eval_form(env, f->as.list.items[0]);
            return ct_value_to_form(env, v, f->span);
        }
        case F_UNQUOTE_SPLICING: {
            CtValue v = ct_eval_form(env, f->as.list.items[0]);
            return ct_value_to_form(env, v, f->span);
        }
        case F_LIST: {
            uint32_t n_in = f->as.list.len;
            Form **tmp = (Form **)arena_alloc(env->elab->arena, n_in * sizeof(Form *));
            bool *splice = (bool *)arena_alloc(env->elab->arena, n_in * sizeof(bool));
            uint32_t n_out = 0;
            for (uint32_t i = 0; i < n_in; i++) {
                Form *it = f->as.list.items[i];
                if (it->tag == F_UNQUOTE_SPLICING) {
                    CtValue v = ct_eval_form(env, it->as.list.items[0]);
                    Form *inner = ct_value_to_form(env, v, it->span);
                    if (!*env->ok) return form_nil(env->elab->arena, f->span);
                    tmp[i] = inner;
                    if (inner->tag == F_LIST) {
                        splice[i] = true;
                        n_out += inner->as.list.len;
                    } else {
                        splice[i] = false;
                        n_out++;
                    }
                } else {
                    tmp[i] = ct_eval_quasiquote(env, it);
                    splice[i] = false;
                    n_out++;
                }
            }
            Form **items = (Form **)arena_alloc(env->elab->arena, n_out * sizeof(Form *));
            uint32_t out_idx = 0;
            for (uint32_t i = 0; i < n_in; i++) {
                if (splice[i]) {
                    Form *lst = tmp[i];
                    for (uint32_t j = 0; j < lst->as.list.len; j++) items[out_idx++] = lst->as.list.items[j];
                } else {
                    items[out_idx++] = tmp[i];
                }
            }
            return form_list(env->elab->arena, f->span, items, n_out);
        }
        case F_VEC:
        case F_MAP:
        case F_SET: {
            uint32_t n_in = f->as.list.len;
            Form **items = (Form **)arena_alloc(env->elab->arena, n_in * sizeof(Form *));
            for (uint32_t i = 0; i < n_in; i++) items[i] = ct_eval_quasiquote(env, f->as.list.items[i]);
            if (f->tag == F_MAP) return form_map(env->elab->arena, f->span, items, n_in);
            if (f->tag == F_SET) return form_set(env->elab->arena, f->span, items, n_in);
            return form_vec(env->elab->arena, f->span, items, n_in);
        }
        case F_QUASIQUOTE:
            return ct_eval_quasiquote(env, f->as.list.items[0]);
        default:
            return f;
    }
}

static bool ct_symbol_name(const Symbol *sym, const char *name) {
    size_t n = strlen(name);
    return sym->len == n && strncmp(sym->name, name, n) == 0;
}

static bool form_contains_ct_builtins(Form *f) {
    switch (f->tag) {
        case F_LIST:
            if (f->as.list.len > 0 && f->as.list.items[0]->tag == F_SYM) {
                const Symbol *head = f->as.list.items[0]->as.sym;
                if (ct_symbol_name(head, "first") ||
                    ct_symbol_name(head, "rest") ||
                    ct_symbol_name(head, "second") ||
                    ct_symbol_name(head, "nil?") ||
                    ct_symbol_name(head, "empty?") ||
                    ct_symbol_name(head, "list?") ||
                    ct_symbol_name(head, "cons") ||
                    ct_symbol_name(head, "list") ||
                    ct_symbol_name(head, "vec")) {
                    return true;
                }
            }
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (form_contains_ct_builtins(f->as.list.items[i])) return true;
            }
            return false;
        case F_VEC:
        case F_MAP:
        case F_SET:
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (form_contains_ct_builtins(f->as.list.items[i])) return true;
            }
            return false;
        case F_UNQUOTE:
        case F_UNQUOTE_SPLICING:
            return form_contains_ct_builtins(f->as.list.items[0]);
        case F_QUOTE:
        case F_QUASIQUOTE:
        case F_SYM:
        case F_NIL:
        case F_BOOL:
        case F_INT:
        case F_FLOAT:
        case F_STR:
        case F_KEYWORD:
        case F_CBLOCK:
        case F_TYPE_ANN:
            return false;
    }
    return false;
}

static CtValue ct_eval_builtin(CtEnv *env, const Symbol *name, Form **args, uint32_t n_args, Span span) {
    if (ct_symbol_name(name, "first")) {
        if (n_args != 1) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, span, "compile-time first expects 1 argument");
            return ct_value_form(form_nil(env->elab->arena, span));
        }
        if ((args[0]->tag != F_LIST && args[0]->tag != F_VEC) || args[0]->as.list.len == 0) return ct_value_form(form_nil(env->elab->arena, span));
        return ct_value_form(args[0]->as.list.items[0]);
    }
    if (ct_symbol_name(name, "rest")) {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time rest expects 1 argument"); return ct_value_form(form_nil(env->elab->arena, span)); }
        if ((args[0]->tag != F_LIST && args[0]->tag != F_VEC) || args[0]->as.list.len <= 1) return ct_value_form(form_list(env->elab->arena, span, NULL, 0));
        uint32_t len = args[0]->as.list.len - 1;
        Form **items = (Form **)arena_alloc(env->elab->arena, len * sizeof(Form *));
        for (uint32_t i = 0; i < len; i++) items[i] = args[0]->as.list.items[i + 1];
        return ct_value_form(form_list(env->elab->arena, span, items, len));
    }
    if (ct_symbol_name(name, "second")) {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time second expects 1 argument"); return ct_value_form(form_nil(env->elab->arena, span)); }
        if ((args[0]->tag != F_LIST && args[0]->tag != F_VEC) || args[0]->as.list.len < 2) return ct_value_form(form_nil(env->elab->arena, span));
        return ct_value_form(args[0]->as.list.items[1]);
    }
    if (ct_symbol_name(name, "nil?")) {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time nil? expects 1 argument"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        return ct_value_form(form_bool(env->elab->arena, span, args[0]->tag == F_NIL));
    }
    if (ct_symbol_name(name, "empty?")) {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time empty? expects 1 argument"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        bool empty = args[0]->tag == F_NIL ||
                     ((args[0]->tag == F_LIST || args[0]->tag == F_VEC || args[0]->tag == F_MAP || args[0]->tag == F_SET) && args[0]->as.list.len == 0);
        return ct_value_form(form_bool(env->elab->arena, span, empty));
    }
    if (ct_symbol_name(name, "list?")) {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time list? expects 1 argument"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        return ct_value_form(form_bool(env->elab->arena, span, args[0]->tag == F_LIST));
    }
    if (ct_symbol_name(name, "cons")) {
        if (n_args != 2) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time cons expects 2 arguments"); return ct_value_form(form_nil(env->elab->arena, span)); }
        Form *tail = args[1];
        uint32_t tail_len = 0;
        if (tail->tag == F_LIST) {
            tail_len = tail->as.list.len;
        } else if (tail->tag != F_NIL) {
            tail_len = 1;
        }
        Form **items = (Form **)arena_alloc(env->elab->arena, (tail_len + 1) * sizeof(Form *));
        items[0] = args[0];
        if (tail->tag == F_LIST) {
            for (uint32_t i = 0; i < tail_len; i++) items[i + 1] = tail->as.list.items[i];
        } else if (tail->tag != F_NIL) {
            items[1] = tail;
        }
        return ct_value_form(form_list(env->elab->arena, span, items, tail_len + 1));
    }
    if (ct_symbol_name(name, "list")) {
        Form **items = (n_args == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, n_args * sizeof(Form *));
        for (uint32_t i = 0; i < n_args; i++) items[i] = args[i];
        return ct_value_form(form_list(env->elab->arena, span, items, n_args));
    }
    if (ct_symbol_name(name, "vec")) {
        Form **items = (n_args == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, n_args * sizeof(Form *));
        for (uint32_t i = 0; i < n_args; i++) items[i] = args[i];
        return ct_value_form(form_vec(env->elab->arena, span, items, n_args));
    }
    if (ct_symbol_name(name, "=")) {
        if (n_args != 2) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time = expects 2 arguments"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        return ct_value_form(form_bool(env->elab->arena, span, ct_form_equal(args[0], args[1])));
    }
    if (ct_symbol_name(name, "not")) {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time not expects 1 argument"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        return ct_value_form(form_bool(env->elab->arena, span, !compile_time_truthy(args[0])));
    }
    return ct_value_form(NULL);
}

static CtValue ct_eval_call(CtEnv *env, Form *f) {
    if (f->as.list.len == 0) return ct_value_form(form_list(env->elab->arena, f->span, NULL, 0));
    Form *head = f->as.list.items[0];
    /* Non-symbol head: cannot be a CT special form or builtin.
     * Return the form as-is so it can be elaborated later.
     * This preserves unevaluated clause lists inside variadic macros. */
    if (head->tag != F_SYM) {
        return ct_value_form(f);
    }
    /* At this point head->tag == F_SYM is guaranteed by the early return above. */
    if (head->as.sym == env->elab->sym_quote && f->as.list.len == 2) {
        return ct_value_form(f->as.list.items[1]);
    }
    if (head->as.sym == env->elab->sym_quasiquote && f->as.list.len == 2) {
        return ct_value_form(ct_eval_quasiquote(env, f->as.list.items[1]));
    }
    if (head->as.sym == env->elab->sym_if) {
        if (f->as.list.len < 3 || f->as.list.len > 4) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, f->span, "compile-time if requires 2 or 3 arguments");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        CtValue cond_v = ct_eval_form(env, f->as.list.items[1]);
        Form *cond = ct_value_to_form(env, cond_v, f->as.list.items[1]->span);
        if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
        if (cond->tag == F_BOOL || cond->tag == F_NIL) {
            if (compile_time_truthy(cond)) return ct_eval_form(env, f->as.list.items[2]);
            if (f->as.list.len == 4) return ct_eval_form(env, f->as.list.items[3]);
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        Form **items = (Form **)arena_alloc(env->elab->arena, f->as.list.len * sizeof(Form *));
        items[0] = head;
        items[1] = cond;
        CtValue then_v = ct_eval_form(env, f->as.list.items[2]);
        items[2] = ct_value_to_form(env, then_v, f->as.list.items[2]->span);
        if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
        if (f->as.list.len == 4) {
            CtValue else_v = ct_eval_form(env, f->as.list.items[3]);
            items[3] = ct_value_to_form(env, else_v, f->as.list.items[3]->span);
            if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        return ct_value_form(form_list(env->elab->arena, f->span, items, f->as.list.len));
    }
    if (head->as.sym == env->elab->sym_do) {
        CtValue out = ct_value_form(form_nil(env->elab->arena, f->span));
        for (uint32_t i = 1; i < f->as.list.len; i++) {
            out = ct_eval_form(env, f->as.list.items[i]);
            if (!*env->ok) return out;
        }
        return out;
    }
    if (head->as.sym == env->elab->sym_let) {
        if (f->as.list.len < 3) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, f->span, "compile-time let requires bindings and body");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        Form *bindings = f->as.list.items[1];
        if (bindings->tag != F_VEC && bindings->tag != F_LIST) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, bindings->span, "compile-time let bindings must be a vector or list");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        if ((bindings->as.list.len % 2) != 0) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, bindings->span, "compile-time let requires an even number of binding forms");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        CtEnv *inner = ct_env_new(env->elab, env, env->ok);
        for (uint32_t i = 0; i < bindings->as.list.len; i += 2) {
            Form *name_f = bindings->as.list.items[i];
            if (name_f->tag != F_SYM) {
                *env->ok = false;
                diag_emit(DIAG_ERROR, name_f->span, "compile-time let binding name must be a symbol");
                return ct_value_form(form_nil(env->elab->arena, f->span));
            }
            CtValue init_v = ct_eval_form(inner, bindings->as.list.items[i + 1]);
            if (!*env->ok) return init_v;
            ct_env_bind(inner, name_f->as.sym, init_v);
        }
        CtValue out = ct_value_form(form_nil(env->elab->arena, f->span));
        for (uint32_t i = 2; i < f->as.list.len; i++) {
            out = ct_eval_form(inner, f->as.list.items[i]);
            if (!*env->ok) return out;
        }
        return out;
    }
    if (head->as.sym == env->elab->sym_fn) {
        if (f->as.list.len < 3) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, f->span, "compile-time fn requires params and body");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        Form *params_f = f->as.list.items[1];
        if (params_f->tag != F_LIST && params_f->tag != F_VEC) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, params_f->span, "compile-time fn params must be a vector or list");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        uint32_t n_total = params_f->as.list.len;
        Form **params = (Form **)arena_alloc(env->elab->arena, n_total * sizeof(Form *));
        uint32_t n_fixed = 0;
        bool is_variadic = false;
        const Symbol *rest_param = NULL;
        for (uint32_t i = 0; i < n_total; i++) {
            Form *p = params_f->as.list.items[i];
            if (p->tag != F_SYM) {
                *env->ok = false;
                diag_emit(DIAG_ERROR, p->span, "compile-time fn parameter must be a symbol");
                return ct_value_form(form_nil(env->elab->arena, f->span));
            }
            if (p->as.sym == env->elab->sym_borrow) {
                if (i + 1 >= n_total) {
                    *env->ok = false;
                    diag_emit(DIAG_ERROR, p->span, "compile-time fn '&' must be followed by a rest parameter");
                    return ct_value_form(form_nil(env->elab->arena, f->span));
                }
                if (i + 2 < n_total) {
                    *env->ok = false;
                    diag_emit(DIAG_ERROR, params_f->as.list.items[i + 2]->span, "compile-time fn allows only one rest parameter");
                    return ct_value_form(form_nil(env->elab->arena, f->span));
                }
                Form *rest_f = params_f->as.list.items[i + 1];
                if (rest_f->tag != F_SYM) {
                    *env->ok = false;
                    diag_emit(DIAG_ERROR, rest_f->span, "compile-time fn rest parameter must be a symbol");
                    return ct_value_form(form_nil(env->elab->arena, f->span));
                }
                is_variadic = true;
                rest_param = rest_f->as.sym;
                break;
            }
            params[n_fixed++] = p;
        }
        Form *body = NULL;
        if (f->as.list.len == 3) {
            body = f->as.list.items[2];
        } else {
            uint32_t n_body = f->as.list.len - 2;
            Form **body_items = (Form **)arena_alloc(env->elab->arena, (n_body + 1) * sizeof(Form *));
            body_items[0] = form_sym(env->elab->arena, f->span, env->elab->sym_do);
            for (uint32_t i = 0; i < n_body; i++) body_items[i + 1] = f->as.list.items[i + 2];
            body = form_list(env->elab->arena, f->span, body_items, n_body + 1);
        }
        CtFn *fn = (CtFn *)arena_alloc(env->elab->arena, sizeof(CtFn));
        fn->params = params;
        fn->n_params = n_fixed;
        fn->is_variadic = is_variadic;
        fn->rest_param = rest_param;
        fn->body = body;
        fn->env = env;
        fn->span = f->span;
        return ct_value_fn(fn);
    }

    CtValue head_v = ct_eval_form(env, head);
    if (!*env->ok) return head_v;
    uint32_t n_in_args = f->as.list.len - 1;
    Form **tmp_args = (n_in_args == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, n_in_args * sizeof(Form *));
    bool *splice = (n_in_args == 0) ? NULL : (bool *)arena_alloc(env->elab->arena, n_in_args * sizeof(bool));
    uint32_t n_args = 0;
    for (uint32_t i = 0; i < n_in_args; i++) {
        Form *arg_form = f->as.list.items[i + 1];
        if (arg_form->tag == F_UNQUOTE_SPLICING) {
            CtValue v = ct_eval_form(env, arg_form->as.list.items[0]);
            Form *inner = ct_value_to_form(env, v, arg_form->span);
            if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
            tmp_args[i] = inner;
            if (inner->tag == F_LIST) {
                splice[i] = true;
                n_args += inner->as.list.len;
            } else {
                splice[i] = false;
                n_args += 1;
            }
        } else {
            CtValue v = ct_eval_form(env, arg_form);
            Form *evaluated = ct_value_to_form(env, v, arg_form->span);
            if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
            tmp_args[i] = evaluated;
            splice[i] = false;
            n_args += 1;
        }
    }
    Form **args = (n_args == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, n_args * sizeof(Form *));
    uint32_t out_idx = 0;
    for (uint32_t i = 0; i < n_in_args; i++) {
        if (splice && splice[i]) {
            Form *lst = tmp_args[i];
            for (uint32_t j = 0; j < lst->as.list.len; j++) args[out_idx++] = lst->as.list.items[j];
        } else if (n_args > 0) {
            args[out_idx++] = tmp_args ? tmp_args[i] : NULL;
        }
    }

    /* head->tag == F_SYM is guaranteed here by the early return above. */
    {
        CtValue b = ct_eval_builtin(env, head->as.sym, args, n_args, f->span);
        if (b.tag == CT_VAL_FORM && b.as.form != NULL) return b;
    }

    if (head_v.tag == CT_VAL_FN) {
        CtFn *fn = head_v.as.fn;
        if (!fn->is_variadic && n_args != fn->n_params) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, f->span, "compile-time fn expected %u arguments, got %u", fn->n_params, n_args);
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        if (fn->is_variadic && n_args < fn->n_params) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, f->span, "compile-time fn expected at least %u arguments, got %u", fn->n_params, n_args);
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        CtEnv *call_env = ct_env_new(env->elab, fn->env, env->ok);
        for (uint32_t i = 0; i < fn->n_params; i++) ct_env_bind(call_env, fn->params[i]->as.sym, ct_value_form(args[i]));
        if (fn->is_variadic) {
            uint32_t n_rest = n_args - fn->n_params;
            Form **rest = (n_rest == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, n_rest * sizeof(Form *));
            for (uint32_t i = 0; i < n_rest; i++) rest[i] = args[fn->n_params + i];
            ct_env_bind(call_env, fn->rest_param, ct_value_form(form_list(env->elab->arena, f->span, rest, n_rest)));
        }
        return ct_eval_form(call_env, fn->body);
    }

    Form *head_form = ct_value_to_form(env, head_v, head->span);
    if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
    Form **items = (Form **)arena_alloc(env->elab->arena, (n_args + 1) * sizeof(Form *));
    items[0] = head_form;
    for (uint32_t i = 0; i < n_args; i++) items[i + 1] = args[i];
    return ct_value_form(form_list(env->elab->arena, f->span, items, n_args + 1));
}

static CtValue ct_eval_form(CtEnv *env, Form *f) {
    switch (f->tag) {
        case F_NIL:
        case F_BOOL:
        case F_INT:
        case F_FLOAT:
        case F_STR:
        case F_KEYWORD:
        case F_QUOTE:
        case F_CBLOCK:
            return ct_value_form(f);
        case F_QUASIQUOTE:
            return ct_value_form(ct_eval_quasiquote(env, f->as.list.items[0]));
        case F_UNQUOTE:
        case F_UNQUOTE_SPLICING:
            return ct_value_form(f->as.list.items[0]);
        case F_TYPE_ANN:
            /* Type annotations are compile-time literals */
            return ct_value_form(f);
        case F_SYM: {
            CtValue v;
            if (ct_env_lookup(env, f->as.sym, &v)) return v;
            return ct_value_form(f);
        }
        case F_LIST:
            return ct_eval_call(env, f);
        case F_VEC:
        case F_MAP:
        case F_SET: {
            Form **items = (f->as.list.len == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, f->as.list.len * sizeof(Form *));
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                CtValue v = ct_eval_form(env, f->as.list.items[i]);
                items[i] = ct_value_to_form(env, v, f->as.list.items[i]->span);
                if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
            }
            if (f->tag == F_MAP) return ct_value_form(form_map(env->elab->arena, f->span, items, f->as.list.len));
            if (f->tag == F_SET) return ct_value_form(form_set(env->elab->arena, f->span, items, f->as.list.len));
            return ct_value_form(form_vec(env->elab->arena, f->span, items, f->as.list.len));
        }
    }
    return ct_value_form(f);
}

static Form *elab_eval_macro_form(Elab *e, Form *f) {
    bool ok = true;
    CtEnv *env = ct_env_new(e, NULL, &ok);
    CtValue v = ct_eval_form(env, f);
    if (!ok) return NULL;
    if (v.tag != CT_VAL_FORM) {
        diag_emit(DIAG_ERROR, f->span, "compile-time macro body did not evaluate to a form");
        return NULL;
    }
    return v.as.form;
}

/* Phase 6: Register a macro */
static void elab_register_macro(Elab *e, MacroDef *macro) {
    if (e->n_macros >= e->cap_macros) {
        e->cap_macros = e->cap_macros ? e->cap_macros * 2 : 8;
        e->macros = (MacroDef **)realloc(e->macros, e->cap_macros * sizeof(MacroDef *));
    }
    e->macros[e->n_macros++] = macro;
}

/* Phase 6: Expand quasiquote forms recursively */
static Form *quasiquote_expand_form(Elab *e, Form *f) {
    switch (f->tag) {
        case F_NIL:
        case F_BOOL:
        case F_INT:
        case F_FLOAT:
        case F_STR:
        case F_KEYWORD:
        case F_SYM:
            /* Literals and symbols: (quasiquote x) -> (quote x) */
            return form_quote(e->arena, f->span, f);
        case F_QUOTE:
            /* (quasiquote (quote x)) -> (quote (quasiquote x)) */
            {
                Form *quoted_form = f->as.list.items[0];
                Form *expanded_quoted = quasiquote_expand_form(e, quoted_form);
                return form_quote(e->arena, f->span, expanded_quoted);
            }
        case F_UNQUOTE:
            /* (quasiquote (unquote x)) -> x */
            return f->as.list.items[0];
        case F_UNQUOTE_SPLICING:
            /* (quasiquote (unquote-splicing x)) -> x */
            return f->as.list.items[0];
        case F_LIST: {
            /* Check for gensym call: (gensym ...) */
            if (f->as.list.len >= 1 && f->as.list.items[0]->tag == F_SYM) {
                const Symbol *name = f->as.list.items[0]->as.sym;
                if (name == e->sym_gensym) {
                    /* Generate a fresh symbol for gensym inside quasiquote */
                    const char *prefix = "g";
                    if (f->as.list.len >= 2) {
                        Form *prefix_f = f->as.list.items[1];
                        if (prefix_f->tag == F_SYM) {
                            prefix = prefix_f->as.sym->name;
                        } else if (prefix_f->tag == F_STR) {
                            prefix = prefix_f->as.s.p;
                        }
                    }
                    char name_buf[64];
                    snprintf(name_buf, sizeof(name_buf), "%s_%u", prefix, e->next_gensym_id++);
                    const Symbol *fresh_sym = symtab_intern(e->st, strslice(name_buf, (uint32_t)strlen(name_buf)));
                    return form_sym(e->arena, f->span, fresh_sym);
                }
            }
            /* Lists inside quasiquote: process each element */
            /* (quasiquote (a ~b c)) -> (a b c) with b unbound from macro params */
            /* For phase 6, we build a list with expanded elements */
            {
                Form **new_items = (Form **)arena_alloc(e->arena, f->as.list.len * sizeof(Form *));
                for (uint32_t i = 0; i < f->as.list.len; i++) {
                    Form *item = f->as.list.items[i];
                    if (item->tag == F_UNQUOTE || item->tag == F_UNQUOTE_SPLICING) {
                        /* Unwrap unquote/unquote-splicing - just return the inner form */
                        /* Parameter substitution will happen in substitute_params */
                        new_items[i] = item->as.list.items[0];
                    } else {
                        /* Recursively expand other items */
                        new_items[i] = quasiquote_expand_form(e, item);
                    }
                }
                return form_list(e->arena, f->span, new_items, f->as.list.len);
            }
        }
        case F_VEC:
        case F_MAP:
        case F_SET:
            /* Vectors/maps/sets inside quasiquote: process each element */
            {
                Form **new_items = (Form **)arena_alloc(e->arena, f->as.list.len * sizeof(Form *));
                for (uint32_t i = 0; i < f->as.list.len; i++) {
                    Form *item = f->as.list.items[i];
                    if (item->tag == F_UNQUOTE || item->tag == F_UNQUOTE_SPLICING) {
                        new_items[i] = item->as.list.items[0];
                    } else {
                        new_items[i] = quasiquote_expand_form(e, item);
                    }
                }
                if (f->tag == F_MAP) return form_map(e->arena, f->span, new_items, f->as.list.len);
                if (f->tag == F_SET) return form_set(e->arena, f->span, new_items, f->as.list.len);
                return form_vec(e->arena, f->span, new_items, f->as.list.len);
            }
        case F_CBLOCK:
        case F_TYPE_ANN:
            return f;
        case F_QUASIQUOTE:
            /* Expand the quasiquoted form */
            return quasiquote_expand_form(e, f->as.list.items[0]);
    }
    return f;
}

/* Phase 6: Helper to substitute parameters in a form */
static Form *substitute_params(Elab *e, Form *f, MacroDef *macro, Form **args) {
    switch (f->tag) {
        case F_NIL:
        case F_BOOL:
        case F_INT:
        case F_FLOAT:
        case F_STR:
        case F_KEYWORD:
        case F_CBLOCK:
        case F_QUOTE:
        case F_TYPE_ANN:
            /* Literals, quote forms, and type annotations are returned as-is */
            return f;
        case F_QUASIQUOTE:
        case F_UNQUOTE:
        case F_UNQUOTE_SPLICING:
            /* Expand quasiquote forms first, then continue substitution */
            return substitute_params(e, quasiquote_expand_form(e, f), macro, args);
        case F_LIST: {
            /* Check for gensym call: (gensym) or (gensym prefix) */
            if (f->as.list.len >= 1 && f->as.list.items[0]->tag == F_SYM) {
                const Symbol *name = f->as.list.items[0]->as.sym;
                if (name == e->sym_gensym) {
                    /* This is a gensym call - generate a fresh symbol */
                    const char *prefix = "g";
                    if (f->as.list.len >= 2) {
                        Form *prefix_f = f->as.list.items[1];
                        if (prefix_f->tag == F_SYM) {
                            prefix = prefix_f->as.sym->name;
                        } else if (prefix_f->tag == F_STR) {
                            prefix = prefix_f->as.s.p;
                        }
                        /* For other cases, use default prefix */
                    }
                    char name_buf[64];
                    snprintf(name_buf, sizeof(name_buf), "%s_%u", prefix, e->next_gensym_id++);
                    const Symbol *fresh_sym = symtab_intern(e->st, strslice(name_buf, (uint32_t)strlen(name_buf)));
                    return form_sym(e->arena, f->span, fresh_sym);
                }
            }
            /* Not a gensym call - continue with normal processing */
            /* Check if any item is ~@ (unquote-splicing) — use splice path if so */
            bool has_splice = false;
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (f->as.list.items[i]->tag == F_UNQUOTE_SPLICING) {
                    has_splice = true; break;
                }
            }
            if (has_splice) {
                /* Two-pass splice: substitute each item, splicing F_UNQUOTE_SPLICING */
                uint32_t n_in = f->as.list.len;
                Form **temp = (Form **)arena_alloc(e->arena, n_in * sizeof(Form *));
                bool *is_splice = (bool *)arena_alloc(e->arena, n_in * sizeof(bool));
                bool *keep_splice_wrapper = (bool *)arena_alloc(e->arena, n_in * sizeof(bool));
                uint32_t n_out = 0;
                for (uint32_t i = 0; i < n_in; i++) {
                    Form *item = f->as.list.items[i];
                    if (item->tag == F_UNQUOTE_SPLICING) {
                        /* ~@ inner: substitute the inner form, then splice if it's a list */
                        Form *inner = item->as.list.items[0];
                        Form *subst = substitute_params(e, inner, macro, args);
                        temp[i] = subst;
                        if (subst->tag == F_LIST) {
                            is_splice[i] = true;
                            keep_splice_wrapper[i] = false;
                            n_out += subst->as.list.len;
                        } else {
                            is_splice[i] = false;
                            keep_splice_wrapper[i] = true;
                            n_out++;
                        }
                    } else {
                        temp[i] = substitute_params(e, item, macro, args);
                        is_splice[i] = false;
                        keep_splice_wrapper[i] = false;
                        n_out++;
                    }
                }
                Form **items = (Form **)arena_alloc(e->arena, n_out * sizeof(Form *));
                uint32_t out_idx = 0;
                for (uint32_t i = 0; i < n_in; i++) {
                    if (is_splice[i]) {
                        Form *lst = temp[i];
                        for (uint32_t j = 0; j < lst->as.list.len; j++) {
                            items[out_idx++] = lst->as.list.items[j];
                        }
                    } else {
                        if (keep_splice_wrapper[i]) {
                            items[out_idx++] = form_unquote_splicing(e->arena, f->as.list.items[i]->span, temp[i]);
                        } else {
                            items[out_idx++] = temp[i];
                        }
                    }
                }
                return form_list(e->arena, f->span, items, n_out);
            }
            Form **items = (Form **)arena_alloc(e->arena, f->as.list.len * sizeof(Form *));
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                items[i] = substitute_params(e, f->as.list.items[i], macro, args);
            }
            return form_list(e->arena, f->span, items, f->as.list.len);
        }
        case F_SYM: {
            /* Check if this symbol is a fixed parameter - if so, replace with corresponding arg */
            for (uint32_t i = 0; i < macro->n_params; i++) {
                Form *param = macro->params[i];
                if (param->tag == F_SYM && param->as.sym == f->as.sym) {
                    return args[i];
                }
            }
            /* Check if this symbol is the rest parameter */
            if (macro->rest_param && macro->rest_param == f->as.sym) {
                return args[macro->n_params]; /* rest list packed at n_params index */
            }
            /* Not a parameter - return as-is */
            return f;
        }
        case F_VEC:
        case F_MAP:
        case F_SET: {
            /* Recursively substitute in vector/map/set items */
            Form **items = (Form **)arena_alloc(e->arena, f->as.list.len * sizeof(Form *));
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                items[i] = substitute_params(e, f->as.list.items[i], macro, args);
            }
            if (f->tag == F_MAP) return form_map(e->arena, f->span, items, f->as.list.len);
            if (f->tag == F_SET) return form_set(e->arena, f->span, items, f->as.list.len);
            return form_vec(e->arena, f->span, items, f->as.list.len);
        }
    }
    return f;
}

/* Phase 6: Expand a macro call with arguments */
static Form *elab_expand_macro(Elab *e, MacroDef *macro, Form **args, uint32_t n_args) {
    /* Phase M4: set expansion-origin module so private helper macros of the
     * same module are accessible during this expansion. Restore on every exit. */
    const Symbol *saved_expansion_mod = e->macro_expansion_module;
    e->macro_expansion_module = macro->defining_module_name;

#define EXPAND_RESTORE() (e->macro_expansion_module = saved_expansion_mod)

    /* Check arity */
    if (macro->is_variadic) {
        if (n_args < macro->n_params) {
            diag_emit(DIAG_ERROR, macro->span,
                      "macro '%s' expects at least %u arguments, got %u",
                      macro->name->name, macro->n_params, n_args);
            EXPAND_RESTORE(); return NULL;
        }
        /* Build augmented args: fixed args + packed rest list at index n_params */
        Form **aug_args = (Form **)arena_alloc(e->arena, (macro->n_params + 1) * sizeof(Form *));
        for (uint32_t i = 0; i < macro->n_params; i++) {
            aug_args[i] = args[i];
        }
        /* Pack trailing args into an F_LIST for the rest param */
        uint32_t n_rest = n_args - macro->n_params;
        Form **rest_items = (Form **)arena_alloc(e->arena, n_rest * sizeof(Form *));
        for (uint32_t i = 0; i < n_rest; i++) {
            rest_items[i] = args[macro->n_params + i];
        }
        Span rest_span = n_rest > 0 ? args[macro->n_params]->span : macro->span;
        aug_args[macro->n_params] = form_list(e->arena, rest_span, rest_items, n_rest);
        Form *templ = substitute_params(e, macro->body, macro, aug_args);
        if (!templ) { EXPAND_RESTORE(); return NULL; }
        Form *result = form_contains_ct_builtins(templ) ? elab_eval_macro_form(e, templ) : templ;
        EXPAND_RESTORE(); return result;
    }
    if (n_args != macro->n_params) {
        diag_emit(DIAG_ERROR, macro->span,
                  "macro '%s' expects %u arguments, got %u",
                  macro->name->name, macro->n_params, n_args);
        EXPAND_RESTORE(); return NULL;
    }

    /* Substitute parameters in the macro body */
    Form *templ = substitute_params(e, macro->body, macro, args);
    if (!templ) { EXPAND_RESTORE(); return NULL; }
    Form *result = form_contains_ct_builtins(templ) ? elab_eval_macro_form(e, templ) : templ;
    EXPAND_RESTORE(); return result;

#undef EXPAND_RESTORE
}

static Binding *binding_new(Elab *e, const Symbol *name, Type type,
                            bool is_mut, bool is_global, Span span) {
    Binding *b = (Binding *)arena_alloc(e->arena, sizeof(Binding));
    memset(b, 0, sizeof(Binding));
    b->name = name;
    b->type = type;
    b->is_mut = is_mut;
    b->is_global = is_global;
    b->id = e->next_id++;
    b->span = span;
    b->closure_fn_binding = NULL;
    b->is_moved = false;  /* Phase 5: move semantics */
    b->moved_at = SPAN_UNKNOWN;
    b->no_unwind = false;  /* Phase R5: #[no-unwind] attribute */
    b->is_exported = false;
    b->defining_module_name = e->current_module_name;
    b->c_export_name = NULL;  /* Phase M6: ^:export-as C name */
    return b;
}

/* Forward declarations. */
static Binding *elab_lookup_sym(Elab *e, const Symbol *sym, Span span, bool *had_error); /* Phase M1 */
static ElabModule *elab_load_module(Elab *e, const Symbol *name, Span span);             /* Phase M2 */
static Expr *elab_form(Elab *e, Form *f);
static bool typekind_is_numeric(TypeKind k);             /* Phase N */
static Expr *elab_as_cast(Elab *e, const Form *call);   /* Phase N */
static Expr *elab_any_type_of(Elab *e, const Form *call); /* IT4 */
static Expr *elab_any_cast(Elab *e, const Form *call);    /* IT4 */
static Expr *elab_unsafe(Elab *e, const Form *call);
/* Phase HRT1 */
static Type *type_expr_from_form(Elab *e, const Form *form, const Symbol *rec_name,
                                  const Symbol **type_params, Kind *type_param_kinds,
                                  uint8_t n_type_params);
static Expr *elab_ascribe(Elab *e, const Form *call);
static Expr *elab_poly_call(Elab *e, const Form *call, Binding *fn_binding);
static Binding *make_poly_wrapper(Elab *e, Binding *inner_b, uint8_t inner_arity, Span span);
static Binding *poly_arg_fn_binding(Expr *arg);
/* Phase HRT2 */
static Expr *elab_pack(Elab *e, const Form *call);
static Expr *elab_open(Elab *e, const Form *call);
/* Phase 5 */
static Expr *elab_ref(Elab *e, const Form *call);
static Expr *elab_deref(Elab *e, const Form *call);
static Expr *elab_drop(Elab *e, const Form *call);
/* LT3 */
static Expr *elab_lref_new(Elab *e, const Form *call);
/* Phase 9: rc<T> + weak<T> */
static Expr *elab_rc_of(Elab *e, const Form *call);
static Expr *elab_rc_clone(Elab *e, const Form *call);
static Expr *elab_rc_drop(Elab *e, const Form *call);
static Expr *elab_rc_ptr(Elab *e, const Form *call);
static Expr *elab_rc_strong_count(Elab *e, const Form *call);
static Expr *elab_rc_from_ref(Elab *e, const Form *call);
static Expr *elab_ref_from_rc(Elab *e, const Form *call);
static Expr *elab_weak(Elab *e, const Form *call);
static Expr *elab_weak_upgrade(Elab *e, const Form *call);
static Expr *elab_weak_pred(Elab *e, const Form *call);
static Expr *elab_ref_pred(Elab *e, const Form *call);
/* Phase 10: GC */
static Expr *elab_gc_force(Elab *e, const Form *call);
static Expr *elab_gc_enable(Elab *e, const Form *call);
static Expr *elab_gc_disable(Elab *e, const Form *call);
/* Phase 12: Borrow deref assignment */
static Expr *elab_set_deref(Elab *e, const Form *call, const Form *deref_form);
/* Phase U3: Unsafe primitives - pointer operations */
static Expr *elab_ptr_deref(Elab *e, const Form *call);
static Expr *elab_ptr_write(Elab *e, const Form *call);
static Expr *elab_ptr_add(Elab *e, const Form *call);
static Expr *elab_ptr_sub(Elab *e, const Form *call);
static Expr *elab_ptr_nullq(Elab *e, const Form *call);
static Expr *elab_ptr_of(Elab *e, const Form *call);
/* Phase U3: Unsafe primitives - type casting */
static Expr *elab_unsafe_cast(Elab *e, const Form *call);
static Expr *elab_reinterpret(Elab *e, const Form *call);
static Expr *elab_transmute(Elab *e, const Form *call);
/* Phase U3: Unsafe primitives - unchecked array ops */
static Expr *elab_array_get_unchecked(Elab *e, const Form *call);
static Expr *elab_array_set_unchecked(Elab *e, const Form *call);
/* Phase U3: Unsafe primitives - raw memory */
static Expr *elab_raw_malloc(Elab *e, const Form *call);
static Expr *elab_raw_free(Elab *e, const Form *call);
static Expr *elab_raw_realloc(Elab *e, const Form *call);
static Expr *elab_raw_memcpy(Elab *e, const Form *call);
static Expr *elab_raw_memset(Elab *e, const Form *call);
/* Phase U3: Unsafe primitives - FFI */
static Expr *elab_c_call(Elab *e, const Form *call);
static Expr *elab_dlopen(Elab *e, const Form *call);
static Expr *elab_dlsym(Elab *e, const Form *call);
static Expr *elab_dlclose(Elab *e, const Form *call);
/* Phase R2: Panic */
static Expr *elab_panic(Elab *e, const Form *call);
static Expr *elab_panic_with(Elab *e, const Form *call);
static Expr *elab_catch_unwind(Elab *e, const Form *call);
static Expr *elab_catch_panic_of(Elab *e, const Form *call);
/* Phase S4: throw / try-catch */
static Expr *elab_throw(Elab *e, const Form *call);
static Expr *elab_try_catch(Elab *e, const Form *call);
static Expr *elab_panic_payload_type(Elab *e, const Form *call);
static Expr *elab_panic_payload_value(Elab *e, const Form *call);
static Expr *elab_panic_payload_file(Elab *e, const Form *call);
static Expr *elab_panic_payload_line(Elab *e, const Form *call);
static Expr *elab_panic_payload_downcast(Elab *e, const Form *call);

/* Phase M2: File reading helper — returns 0 on success, -1 on failure. */
static int elab_read_file(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return -1; }
    buf[size] = '\0';
    *out = buf;
    *out_len = (size_t)size;
    return 0;
}

/* Phase M2: Find a loaded module in the registry by interned name. */
static ElabModule *elab_find_loaded_module(Elab *e, const Symbol *name) {
    for (uint32_t i = 0; i < e->n_loaded_modules; i++) {
        if (e->loaded_modules[i].name == name)
            return &e->loaded_modules[i];
    }
    return NULL;
}

/* Phase U3: Unsafe primitives implementations */

static Expr *elab_ptr_deref(Elab *e, const Form *call) {
    /* (ptr-deref ptr) - dereference a pointer, return the value at that address */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-deref requires exactly 1 argument: (ptr-deref ptr)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-deref requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-deref requires ptr<void>, got %s", type_name(ptr->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_deref, ptr->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-deref: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = ptr;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

static Expr *elab_ptr_write(Elab *e, const Form *call) {
    /* (ptr-write ptr value) - write a value to a pointer address */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-write requires exactly 2 arguments: (ptr-write ptr value)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-write requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *value = elab_form(e, call->as.list.items[2]);
    if (!value) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-write requires ptr<void> as first argument, got %s", type_name(ptr->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_write, ptr->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-write: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = value;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

static Expr *elab_ptr_add(Elab *e, const Form *call) {
    /* (ptr-add ptr offset) - pointer arithmetic: add offset to pointer */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-add requires exactly 2 arguments: (ptr-add ptr offset)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-add requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *offset = elab_form(e, call->as.list.items[2]);
    if (!offset) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-add requires ptr<void> as first argument, got %s", type_name(ptr->type));
        return NULL;
    }
    if (offset->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-add requires int as second argument, got %s", type_name(offset->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_add, ptr->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-add: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = offset;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

static Expr *elab_ptr_sub(Elab *e, const Form *call) {
    /* (ptr-sub ptr offset) - pointer arithmetic: subtract offset from pointer */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-sub requires exactly 2 arguments: (ptr-sub ptr offset)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-sub requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *offset = elab_form(e, call->as.list.items[2]);
    if (!offset) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-sub requires ptr<void> as first argument, got %s", type_name(ptr->type));
        return NULL;
    }
    if (offset->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-sub requires int as second argument, got %s", type_name(offset->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_sub, ptr->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-sub: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = offset;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

static Expr *elab_ptr_nullq(Elab *e, const Form *call) {
    /* (ptr-null? ptr) - check if pointer is null */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-null? requires exactly 1 argument: (ptr-null? ptr)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-null? requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-null? requires ptr<void>, got %s", type_name(ptr->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_nullq, ptr->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-null?: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = ptr;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

static Expr *elab_ptr_of(Elab *e, const Form *call) {
    /* (ptr-of value) - get pointer to a value (address-of) */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-of requires exactly 1 argument: (ptr-of value)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "ptr-of requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *value = elab_form(e, call->as.list.items[1]);
    if (!value) return NULL;
    const BuiltinSpec *spec = builtin_lookup(e->sym_ptr_of, value->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "ptr-of: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = value;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

static Expr *elab_unsafe_cast(Elab *e, const Form *call) {
    /* (unsafe-cast value to-type) - C-style cast
     * For v1, we just check we're in an unsafe block and emit a cast to int64_t
     * This is a simplified implementation; full type checking comes later */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "unsafe-cast requires exactly 2 arguments: (unsafe-cast value to-type)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "unsafe-cast requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *value = elab_form(e, call->as.list.items[1]);
    if (!value) return NULL;
    /* For v1, we ignore the target type and just cast to int64_t */
    Form *to_type_form = call->as.list.items[2];
    /* We don't validate the target type for v1; just require it exists */
    if (!to_type_form) {
        diag_emit(DIAG_ERROR, call->span, "unsafe-cast requires a target type");
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_unsafe_cast, value->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "unsafe-cast: builtin lookup failed");
        return NULL;
    }
    /* Result type is int for v1 */
    Type result_type = TYPE_INT;
    Expr *out = expr_new(e->arena, EX_BUILTIN, result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = value;
    args[1] = value; /* Placeholder for target type */
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

static Expr *elab_reinterpret(Elab *e, const Form *call) {
    /* (reinterpret value as-type) - bitwise reinterpretation
     * Similar to unsafe-cast but with compile-time size check requirement
     * For v1, we just check we're in an unsafe block */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "reinterpret requires exactly 2 arguments: (reinterpret value as-type)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "reinterpret requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *value = elab_form(e, call->as.list.items[1]);
    if (!value) return NULL;
    /* Compile-time size check: source and target types must have the same size */
    Form *to_type_form = call->as.list.items[2];
    TypeKind target_kind = TY_UNKNOWN;
    if (to_type_form && to_type_form->tag == F_KEYWORD) {
        target_kind = typekind_from_symbol(to_type_form->as.sym->name);
    } else if (to_type_form && to_type_form->tag == F_SYM) {
        target_kind = typekind_from_symbol(to_type_form->as.sym->name);
    }
    int src_size = type_size_bytes(value->type.kind);
    int dst_size = (target_kind != TY_UNKNOWN) ? type_size_bytes(target_kind) : 0;
    if (src_size > 0 && dst_size > 0 && src_size != dst_size) {
        diag_emit(DIAG_ERROR, call->span,
                  "reinterpret: size mismatch — source type '%s' (%d bytes) vs target type '%s' (%d bytes)",
                  type_name(value->type), src_size,
                  to_type_form->as.sym->name, dst_size);
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_reinterpret, value->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "reinterpret: builtin lookup failed");
        return NULL;
    }
    Type result_type = TYPE_INT;
    Expr *out = expr_new(e->arena, EX_BUILTIN, result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = value;
    args[1] = value; /* Placeholder */
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

static Expr *elab_transmute(Elab *e, const Form *call) {
    /* (transmute value to-type) - type-punning with compile-time size check
     * For v1, we just check we're in an unsafe block */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "transmute requires exactly 2 arguments: (transmute value to-type)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "transmute requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *value = elab_form(e, call->as.list.items[1]);
    if (!value) return NULL;
    /* For v1, compile-time size check is not implemented */
    const BuiltinSpec *spec = builtin_lookup(e->sym_transmute, value->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "transmute: builtin lookup failed");
        return NULL;
    }
    Type result_type = TYPE_INT;
    Expr *out = expr_new(e->arena, EX_BUILTIN, result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = value;
    args[1] = value; /* Placeholder */
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

static Expr *elab_array_get_unchecked(Elab *e, const Form *call) {
    /* (array-get-unchecked ptr index) - get element at index without bounds check */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-get-unchecked requires exactly 2 arguments: (array-get-unchecked ptr index)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-get-unchecked requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *index = elab_form(e, call->as.list.items[2]);
    if (!index) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-get-unchecked requires ptr<void> as first argument, got %s",
                  type_name(ptr->type));
        return NULL;
    }
    if (index->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-get-unchecked requires int as second argument, got %s",
                  type_name(index->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_array_get_unchecked, ptr->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "array-get-unchecked: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = index;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

static Expr *elab_array_set_unchecked(Elab *e, const Form *call) {
    /* (array-set-unchecked ptr index value) - set element at index without bounds check */
    if (call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-set-unchecked requires exactly 3 arguments: (array-set-unchecked ptr index value)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-set-unchecked requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *index = elab_form(e, call->as.list.items[2]);
    if (!index) return NULL;
    Expr *value = elab_form(e, call->as.list.items[3]);
    if (!value) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-set-unchecked requires ptr<void> as first argument, got %s",
                  type_name(ptr->type));
        return NULL;
    }
    if (index->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "array-set-unchecked requires int as second argument, got %s",
                  type_name(index->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_array_set_unchecked, ptr->type, 3);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "array-set-unchecked: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 3 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = index;
    args[2] = value;
    out->as.builtin.args = args;
    out->as.builtin.n = 3;
    return out;
}

static Expr *elab_raw_malloc(Elab *e, const Form *call) {
    /* (raw-malloc size) - allocate raw memory */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-malloc requires exactly 1 argument: (raw-malloc size)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-malloc requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *size = elab_form(e, call->as.list.items[1]);
    if (!size) return NULL;
    if (size->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-malloc requires int as argument, got %s", type_name(size->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_raw_malloc, size->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "raw-malloc: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = size;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

static Expr *elab_raw_free(Elab *e, const Form *call) {
    /* (raw-free ptr) - free raw memory */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-free requires exactly 1 argument: (raw-free ptr)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-free requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-free requires ptr<void> as argument, got %s", type_name(ptr->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_raw_free, ptr->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "raw-free: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = ptr;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

static Expr *elab_raw_realloc(Elab *e, const Form *call) {
    /* (raw-realloc ptr new-size) - reallocate raw memory */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-realloc requires exactly 2 arguments: (raw-realloc ptr new-size)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-realloc requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *ptr = elab_form(e, call->as.list.items[1]);
    if (!ptr) return NULL;
    Expr *new_size = elab_form(e, call->as.list.items[2]);
    if (!new_size) return NULL;
    if (ptr->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-realloc requires ptr<void> as first argument, got %s",
                  type_name(ptr->type));
        return NULL;
    }
    if (new_size->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-realloc requires int as second argument, got %s",
                  type_name(new_size->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_raw_realloc, ptr->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "raw-realloc: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = ptr;
    args[1] = new_size;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

static Expr *elab_raw_memcpy(Elab *e, const Form *call) {
    /* (raw-memcpy dest src n) - copy memory */
    if (call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memcpy requires exactly 3 arguments: (raw-memcpy dest src n)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memcpy requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *dest = elab_form(e, call->as.list.items[1]);
    if (!dest) return NULL;
    Expr *src = elab_form(e, call->as.list.items[2]);
    if (!src) return NULL;
    Expr *n = elab_form(e, call->as.list.items[3]);
    if (!n) return NULL;
    if (dest->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memcpy requires ptr<void> as first argument, got %s",
                  type_name(dest->type));
        return NULL;
    }
    if (src->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memcpy requires ptr<void> as second argument, got %s",
                  type_name(src->type));
        return NULL;
    }
    if (n->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memcpy requires int as third argument, got %s",
                  type_name(n->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_raw_memcpy, dest->type, 3);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "raw-memcpy: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 3 * sizeof(Expr *));
    args[0] = dest;
    args[1] = src;
    args[2] = n;
    out->as.builtin.args = args;
    out->as.builtin.n = 3;
    return out;
}

static Expr *elab_raw_memset(Elab *e, const Form *call) {
    /* (raw-memset dest byte n) - set memory to byte value */
    if (call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memset requires exactly 3 arguments: (raw-memset dest byte n)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memset requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *dest = elab_form(e, call->as.list.items[1]);
    if (!dest) return NULL;
    Expr *byte_val = elab_form(e, call->as.list.items[2]);
    if (!byte_val) return NULL;
    Expr *n = elab_form(e, call->as.list.items[3]);
    if (!n) return NULL;
    if (dest->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memset requires ptr<void> as first argument, got %s",
                  type_name(dest->type));
        return NULL;
    }
    if (byte_val->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memset requires int as second argument, got %s",
                  type_name(byte_val->type));
        return NULL;
    }
    if (n->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "raw-memset requires int as third argument, got %s",
                  type_name(n->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_raw_memset, dest->type, 3);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "raw-memset: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 3 * sizeof(Expr *));
    args[0] = dest;
    args[1] = byte_val;
    args[2] = n;
    out->as.builtin.args = args;
    out->as.builtin.n = 3;
    return out;
}

static Expr *elab_c_call(Elab *e, const Form *call) {
    /* (c-call fn-name arg1 arg2 ...) - call a C function by name
     * For v1, we emit a direct C call. The function must be declared via extern-c.
     * We look up the function name in the scope and emit a call to it.
     * This is a simplified implementation for v1. */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "c-call requires at least a function name: (c-call fn-name arg1 ...)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "c-call requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Form *name_f = call->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "c-call: function name must be a symbol");
        return NULL;
    }
    /* Look up the function in the scope - it should be an extern-c declaration */
    Binding *b = scope_lookup(e->scope, name_f->as.sym);
    if (!b) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "c-call: function '%s' is not declared (use extern-c to declare it)",
                  name_f->as.sym->name);
        return NULL;
    }
    if (b->type.kind != TY_FN) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "c-call: '%s' is not a function", name_f->as.sym->name);
        return NULL;
    }
    /* Elaborate arguments */
    uint32_t n_args = call->as.list.len - 2;
    Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[i + 2]);
        if (!args[i]) return NULL;
    }
    /* Create a call expression */
    Expr *fn_expr = expr_new(e->arena, EX_VAR, b->type, name_f->span);
    fn_expr->as.var.binding = b;
    
    /* For v1, we use the simple call_ structure which stores a binding */
    Expr *out = expr_new(e->arena, EX_CALL, type_from_kind(TY_INT), call->span);
    out->as.call_.fn_binding = b;
    out->as.call_.args = args;
    out->as.call_.n_args = n_args;
    out->as.call_.fn_expr = NULL;
    return out;
}

static Expr *elab_dlopen(Elab *e, const Form *call) {
    /* (dlopen path) - dynamic library open
     * For v1, we emit a call to dlopen with RTLD_LAZY flag
     * Returns ptr<void> (the library handle) */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlopen requires exactly 1 argument: (dlopen path)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlopen requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *path = elab_form(e, call->as.list.items[1]);
    if (!path) return NULL;
    if (path->type.kind != TY_CSTR) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlopen requires cstr as argument, got %s", type_name(path->type));
        return NULL;
    }
    /* Emit: dlopen(path, RTLD_LAZY) */
    const BuiltinSpec *spec = builtin_lookup(e->sym_dlopen, path->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "dlopen: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = path;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

static Expr *elab_dlsym(Elab *e, const Form *call) {
    /* (dlsym handle symbol) - get symbol address from dynamic library
     * For v1, we emit a call to dlsym
     * Returns ptr<void> (the symbol address) */
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlsym requires exactly 2 arguments: (dlsym handle symbol)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlsym requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *handle = elab_form(e, call->as.list.items[1]);
    if (!handle) return NULL;
    Expr *symbol = elab_form(e, call->as.list.items[2]);
    if (!symbol) return NULL;
    if (handle->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlsym requires ptr<void> as first argument, got %s",
                  type_name(handle->type));
        return NULL;
    }
    if (symbol->type.kind != TY_CSTR) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlsym requires cstr as second argument, got %s",
                  type_name(symbol->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_dlsym, handle->type, 2);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "dlsym: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, 2 * sizeof(Expr *));
    args[0] = handle;
    args[1] = symbol;
    out->as.builtin.args = args;
    out->as.builtin.n = 2;
    return out;
}

static Expr *elab_dlclose(Elab *e, const Form *call) {
    /* (dlclose handle) - close dynamic library
     * For v1, we emit a call to dlclose
     * Returns int (0 on success, non-zero on error) */
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlclose requires exactly 1 argument: (dlclose handle)");
        return NULL;
    }
    if (e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlclose requires an enclosing (unsafe ...) block");
        return NULL;
    }
    Expr *handle = elab_form(e, call->as.list.items[1]);
    if (!handle) return NULL;
    if (handle->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, call->span,
                  "dlclose requires ptr<void> as argument, got %s",
                  type_name(handle->type));
        return NULL;
    }
    const BuiltinSpec *spec = builtin_lookup(e->sym_dlclose, handle->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span, "dlclose: builtin lookup failed");
        return NULL;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    Expr **args = arena_alloc(e->arena, sizeof(Expr *));
    args[0] = handle;
    out->as.builtin.args = args;
    out->as.builtin.n = 1;
    return out;
}

/* ---- helpers ---- */

static bool effect_row_contains_symbol(const EffectRow *row, const Symbol *name) {
    if (!row || !name) return false;
    switch (row->kind) {
    case ERK_EMPTY:
        return false;
    case ERK_CONCRETE:
        for (uint8_t i = 0; i < row->as.concrete.n_effects; i++) {
            Effect *eff = row->as.concrete.effects[i];
            if (eff && eff->name == name) return true;
        }
        return false;
    case ERK_VAR:
        return false;
    case ERK_UNION:
        return effect_row_contains_symbol(row->as.union_.left, name) ||
               effect_row_contains_symbol(row->as.union_.right, name);
    case ERK_UNRESOLVED:
        for (uint8_t i = 0; i < row->as.unresolved.n_sym_names; i++) {
            if (row->as.unresolved.sym_names[i] == name) return true;
        }
        return false;
    }
    return false;
}

static Expr *e_nil(Elab *e, Span span) {
    return expr_new(e->arena, EX_NIL_LIT, TYPE_NIL, span);
}

/* ---- special forms ---- */

static Expr *elab_let(Elab *e, const Form *call) {
    /* (let [b1 i1 b2 i2 ...] body...) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span, "let requires a binding vector");
        return NULL;
    }
    Form *bindings_form = call->as.list.items[1];
    if (bindings_form->tag != F_VEC) {
        diag_emit(DIAG_ERROR, bindings_form->span,
                  "let bindings must be a vector [name init ...]");
        return NULL;
    }

    /* Parse bindings: walk left-to-right, optional ^mut / ^persistent prefix per entry. */
    LetBinding *binds = NULL;
    bool       *binding_moved_during_init = NULL; /* tracks moves of preceding bindings during each init elaboration */
    uint32_t    n_binds = 0, cap = 0;
    Scope       inner;
    scope_init(&inner, e->scope);
    e->scope = &inner;

    int rc = 0;
    uint32_t i = 0;
    while (i < bindings_form->as.list.len) {
        Form *cur = bindings_form->as.list.items[i];
        bool is_mut = false;
        bool is_persistent = false;
        bool is_linear_ann = false;
        bool is_unique_ann = false;
        bool is_affine_ann = false;   /* ST0 */
        bool is_relevant_ann = false; /* ST0 */
        /* Phase P3 / LT0 / UT0 / UT2 / ST0: Consume binding annotations in any order.
         * Accepted: ^mut, ^persistent, ^linear, ^unique, ^affine, ^relevant -- may
         * appear in any combination before the binding name, e.g. [^affine v ...]. */
        {
            bool keep_going = true;
            while (keep_going && cur->tag == F_SYM) {
                if (cur->as.sym == e->sym_caret_mut) {
                    is_mut = true;
                } else if (cur->as.sym == e->sym_caret_persistent) {
                    is_persistent = true;
                } else if (cur->as.sym == e->sym_caret_linear) {
                    is_linear_ann = true;
                } else if (cur->as.sym == e->sym_caret_unique) {
                    is_unique_ann = true;
                } else if (cur->as.sym == e->sym_caret_affine) {
                    is_affine_ann = true;
                } else if (cur->as.sym == e->sym_caret_relevant) {
                    is_relevant_ann = true;
                } else {
                    break; /* Not an annotation -- this is the binding name */
                }
                i++;
                if (i >= bindings_form->as.list.len) {
                    diag_emit(DIAG_ERROR, cur->span,
                              "trailing annotation '%s' with no binding name",
                              cur->as.sym->name);
                    rc = -1; keep_going = false; break;
                }
                cur = bindings_form->as.list.items[i];
            }
        }
        if (cur->tag != F_SYM) {
            diag_emit(DIAG_ERROR, cur->span,
                      "let binding name must be a symbol, got %s",
                      cur->tag == F_INT ? "an integer" :
                      cur->tag == F_STR ? "a string"   :
                      cur->tag == F_KEYWORD ? "a keyword" : "non-symbol");
            rc = -1; break;
        }
        const Symbol *name = cur->as.sym;
        Span name_span = cur->span;
        i++;
        if (i >= bindings_form->as.list.len) {
            diag_emit(DIAG_ERROR, cur->span,
                      "let binding for '%s' is missing its initializer",
                      name->name);
            rc = -1; break;
        }
        Form *init_form = bindings_form->as.list.items[i++];

        /* Task 1 (Prereq 1): Snapshot move-state of preceding bindings before elaborating this init.
         * This lets us detect which preceding bindings are moved during this init's elaboration. */
        bool *moved_snapshot = NULL;
        if (n_binds > 0) {
            moved_snapshot = (bool *)malloc(n_binds * sizeof(bool));
            if (!moved_snapshot) { fprintf(stderr, "tur: oom\n"); abort(); }
            for (uint32_t j = 0; j < n_binds; j++) {
                moved_snapshot[j] = binds[j].binding->is_moved;
            }
        }

        Expr *init = elab_form(e, init_form);

        /* Task 2 (Prereq 1): Capture which preceding bindings were newly moved during this init elaboration. */
        if (moved_snapshot) {
            for (uint32_t j = 0; j < n_binds; j++) {
                if (!moved_snapshot[j] && binds[j].binding->is_moved) {
                    binding_moved_during_init[j] = true;
                }
            }
            free(moved_snapshot);
        }

        if (!init) { rc = -1; break; }

        /* Phase 11: Move tracking - if init is a CK_MOVE binding reference, poison it */
        if (init->kind == EX_VAR && type_is_move(init->as.var.binding->type)) {
            binding_mark_moved(init->as.var.binding, init_form->span);
        }

        /* UT1: Alias tracking.
         * (a) When a CK_COPY (non-unique) binding is copied into a new binding,
         *     mark the source as AS_ALIASED so that passing it as ^unique later
         *     is caught (TUR_E0200).
         * (b) When a ^unique binding is transferred (moved), propagate is_unique
         *     to the destination so uniqueness is preserved through rebinding. */
        if (g_unique_enabled && init->kind == EX_VAR) {
            Binding *src = init->as.var.binding;
            if (!src->is_unique && type_is_copy(src->type)) {
                /* CK_COPY binding copied into new name — record alias */
                src->alias_state = AS_ALIASED;
                src->alias_name  = name;
            }
        }

        Binding *b = binding_new(e, name, init->type, is_mut, false, name_span);
        b->is_persistent = is_persistent;
        /* LT0: Mark binding as linear if annotated with ^linear or if initializer
         * type has CK_LINEAR (e.g., returned from a function returning lref<T>). */
        if (is_linear_ann || init->type.copy_kind == CK_LINEAR) {
            b->is_linear = true;
            /* Upgrade copy_kind to CK_LINEAR on the binding's type */
            b->type.copy_kind = CK_LINEAR;
        }
        /* UT0: Mark binding as unique if annotated with ^unique */
        if (is_unique_ann) {
            b->is_unique = true;
            b->type.copy_kind = CK_UNIQUE;
        }
        /* ST0: Mark binding as affine if annotated with ^affine */
        if (is_affine_ann) {
            b->is_affine = true;
            b->type.substruct = SK_AFFINE;
        }
        /* ST0: Mark binding as relevant if annotated with ^relevant */
        if (is_relevant_ann) {
            b->is_relevant = true;
            b->type.substruct = SK_RELEVANT;
        }
        /* UT1: Propagate is_unique through ownership transfer (let [y x] where x is ^unique) */
        if (g_unique_enabled && !is_unique_ann &&
            init->kind == EX_VAR && init->as.var.binding->is_unique) {
            b->is_unique = true;
            /* copy_kind is already CK_UNIQUE, inherited from init->type */
        }
        /* ST1: Propagate affine/relevant through ownership transfer (let [y x] where x is ^affine/^relevant) */
        if (g_substructural_enabled && !is_affine_ann && !is_relevant_ann &&
                init->kind == EX_VAR) {
            Binding *src = init->as.var.binding;
            if (src->is_affine) {
                b->is_affine = true;
                b->type.substruct = SK_AFFINE;
            }
            if (src->is_relevant) {
                b->is_relevant = true;
                b->type.substruct = SK_RELEVANT;
            }
        }
        /* ST3: Propagate affine/relevant through expression type (e.g. from must-use macro
         * returning an EX_LET whose type carries SK_RELEVANT). Only fires when not already
         * set by the EX_VAR propagation above. */
        if (g_substructural_enabled && !is_affine_ann && !is_relevant_ann &&
                !b->is_affine && !b->is_relevant && init->kind != EX_VAR) {
            if (init->type.substruct == SK_RELEVANT) {
                b->is_relevant = true;
                b->type.substruct = SK_RELEVANT;
            } else if (init->type.substruct == SK_AFFINE) {
                b->is_affine = true;
                b->type.substruct = SK_AFFINE;
            }
        }
        /* ST2: Under -Xsubstructural, ref<T> bindings are inferred as SK_LINEAR
         * unless an explicit substructural annotation is already present. */
        if (g_substructural_enabled && !is_linear_ann && !is_affine_ann && !is_relevant_ann
                && !b->is_linear && init && init->type.kind == TY_REF) {
            b->is_linear = true;
            b->type.substruct = SK_LINEAR;
        }
        scope_add(&inner, b);

        if (n_binds == cap) {
            cap = cap ? cap * 2 : 4;
            binds = (LetBinding *)realloc(binds, cap * sizeof(LetBinding));
            if (!binds) { fprintf(stderr, "tur: oom\n"); abort(); }
            binding_moved_during_init = (bool *)realloc(binding_moved_during_init, cap * sizeof(bool));
            if (!binding_moved_during_init) { fprintf(stderr, "tur: oom\n"); abort(); }
        }
        
        /* Phase 3: If init is a closure, set closure_fn_binding on the binding */
        if (init && init->kind == EX_CLOSURE) {
            struct Closure *closure = init->as.closure_.closure;
            b->closure_fn_binding = closure->fn->binding;
        }
        /* Phase HRT4: propagate poly fn metadata through let-bindings.
         * (let [g f] ...) where f is is_poly_fn → g inherits is_poly_fn.
         * (let [g id] ...) where id is a global TY_FN → g.source_binding = id. */
        if (init && init->kind == EX_VAR) {
            Binding *init_b = init->as.var.binding;
            if (init_b->is_poly_fn) {
                b->is_poly_fn = true;
                b->poly_type  = init_b->poly_type;
            } else if (init_b->type.kind == TY_FN) {
                /* Follow any existing source chain to the root global fn. */
                Binding *root = init_b->source_binding ? init_b->source_binding : init_b;
                if (root->is_global) b->source_binding = root;
            }
        }
        
        binds[n_binds].binding = b;
        binds[n_binds].init = init;
        binding_moved_during_init[n_binds] = false; /* new binding, not yet moved during init */
        n_binds++;
    }

    /* Phase 5: Check if any binding is a ref and needs auto-defer drop */
    bool has_ref_bindings = false;
    for (uint32_t k = 0; k < n_binds; k++) {
        /* Skip refs that come from ref/from-rc - they don't own the data */
        /* ST2: Skip refs that are marked linear — LT1 scope-exit check handles those */
        if (binds[k].binding->type.kind == TY_REF &&
            binds[k].init->kind != EX_REF_FROM_RC &&
            !binds[k].binding->is_linear) {
            has_ref_bindings = true;
            break;
        }
    }

    /* The binding_moved_during_init array (built during the binding loop) records which
     * bindings were moved during init elaboration of subsequent bindings in this let form.
     * Combined with is_moved (which also captures body-phase moves), this gives complete
     * move-state tracking across all elaboration phases. */

    Expr *body = NULL;
    if (rc == 0) {
        uint32_t body_count = call->as.list.len - 2;
        if (body_count == 0) {
            body = e_nil(e, call->span);
        } else if (body_count == 1) {
            body = elab_form(e, call->as.list.items[2]);
            if (!body) rc = -1;
        } else {
            Expr **items = (Expr **)arena_alloc(e->arena, body_count * sizeof(Expr *));
            for (uint32_t k = 0; k < body_count; k++) {
                items[k] = elab_form(e, call->as.list.items[2 + k]);
                if (!items[k]) { rc = -1; break; }
            }
            if (rc == 0) {
                body = expr_new(e->arena, EX_DO, items[body_count - 1]->type, call->span);
                body->as.do_.items = items;
                body->as.do_.n = body_count;
            }
        }
        
        /* Phase 5: If we have ref bindings and the body is a single expression
         * (not a do), wrap it in a do so we can add defers */
        if (has_ref_bindings && body && body->kind != EX_DO) {
            Expr **items = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
            items[0] = body;
            body = expr_new(e->arena, EX_DO, body->type, call->span);
            body->as.do_.items = items;
            body->as.do_.n = 1;
        }
        
        /* Phase 5: Inject defers for ref bindings into the do body */
        if (has_ref_bindings && body && body->kind == EX_DO) {
            /* We need to add defer expressions to the do body */
            /* First, collect all ref binding names that need drops (excluding moved ones) */
            uint32_t n_refs = 0;
            for (uint32_t k = 0; k < n_binds; k++) {
                /* Skip refs that come from ref/from-rc - they don't own the data */
                /* Skip refs that were moved during init or body elaboration - avoid use-after-move defer */
                /* ST2: Skip linear refs — LT1 scope-exit check enforces they are consumed explicitly */
                if (binds[k].binding->type.kind == TY_REF &&
                    binds[k].init->kind != EX_REF_FROM_RC &&
                    !binding_moved_during_init[k] &&
                    !binds[k].binding->is_moved &&
                    !binds[k].binding->is_linear) {
                    n_refs++;
                }
            }
            
            if (n_refs > 0) {
                /* Create new items array with space for defers */
                uint32_t new_n = body->as.do_.n + n_refs;
                Expr **new_items = (Expr **)arena_alloc(e->arena, new_n * sizeof(Expr *));
                
                /* Copy existing items */
                memcpy(new_items, body->as.do_.items, body->as.do_.n * sizeof(Expr *));
                
                /* Add defer expressions for each ref binding at the end */
                /* Note: defers execute in LIFO order, so we add them in order and they'll
                 * fire in reverse order. But since we're adding them at the end of the
                 * items array, they'll be after the actual body expressions, which is
                 * what we want for scope-exit behavior. */
                uint32_t defer_idx = body->as.do_.n;
                for (uint32_t k = 0; k < n_binds; k++) {
                    /* Skip refs that come from ref/from-rc - they don't own the data */
                    /* Skip refs moved during init or body elaboration - avoid use-after-move defer */
                    /* ST2: Skip linear refs — they must be consumed explicitly; LT1 enforces this */
                    if (binds[k].binding->type.kind == TY_REF &&
                        binds[k].init->kind != EX_REF_FROM_RC &&
                        !binding_moved_during_init[k] &&
                        !binds[k].binding->is_moved &&
                        !binds[k].binding->is_linear) {
                        /* Create (defer (drop! binding_name)) expression */
                        /* Create a variable reference to the binding */
                        Expr *var_expr = expr_new(e->arena, EX_VAR, binds[k].binding->type, call->span);
                        var_expr->as.var.binding = binds[k].binding;
                        
                        /* Look up the drop! builtin spec and create a BUILTIN expression */
                        const BuiltinSpec *spec = builtin_lookup(e->sym_drop, binds[k].binding->type, 1);
                        if (!spec) {
                            diag_emit(DIAG_ERROR, call->span,
                                      "internal error: drop! builtin not found for ref<T>");
                            rc = -1;
                            break;
                        }
                        
                        /* Create the drop! builtin call */
                        Expr *drop_call = expr_new(e->arena, EX_BUILTIN, TYPE_NIL, call->span);
                        drop_call->as.builtin.spec = spec;
                        drop_call->as.builtin.n = 1;
                        drop_call->as.builtin.args = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
                        drop_call->as.builtin.args[0] = var_expr;
                        
                        /* Create the defer expression */
                        Expr *defer_expr = expr_new(e->arena, EX_DEFER, TYPE_NIL, call->span);
                        defer_expr->as.defer_.body = drop_call;
                        /* Capture analysis for defer body */
                        /* Collect free variables in the defer body (the drop! call references the binding) */
                        uint32_t n_free = 0;
                        Binding **free_vars = collect_free_vars(drop_call, NULL, 0, &n_free);
                        
                        Binding **captures = NULL;
                        uint8_t n_captures = 0;
                        if (n_free > 0) {
                            captures = (Binding **)arena_alloc(e->arena, n_free * sizeof(Binding *));
                            memcpy(captures, free_vars, n_free * sizeof(Binding *));
                            n_captures = (uint8_t)n_free;
                        }
                        free(free_vars);
                        
                        defer_expr->as.defer_.captures = captures;
                        defer_expr->as.defer_.n_captures = n_captures;
                        
                        new_items[defer_idx++] = defer_expr;
                    }
                }
                
                /* Update the body with new items */
                body->as.do_.items = new_items;
                body->as.do_.n = new_n;
            }
        }
    }

    /* Phase 5b: RC auto-drop injection with consumption detection.
     * Inject (defer (rc/drop x)) for let-bound rc/of values that are:
     * 1. Not consumed by ref/from-rc (which would transfer ownership and cause double-free)
     * 2. Not explicitly dropped via (rc/drop x)
     * 3. Not moved to another binding */
    bool has_rc_bindings = false;
    for (uint32_t k = 0; k < n_binds; k++) {
        if (binds[k].binding->type.kind == TY_RC) {
            has_rc_bindings = true;
            break;
        }
    }
    
    /* If we have rc bindings, wrap body in do if needed and inject defers */
    if (has_rc_bindings && body && body->kind != EX_DO) {
        Expr **items = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
        items[0] = body;
        body = expr_new(e->arena, EX_DO, body->type, call->span);
        body->as.do_.items = items;
        body->as.do_.n = 1;
    }
    
    if (has_rc_bindings && body && body->kind == EX_DO) {
        /* Count rc bindings that need auto-drop (excluding consumed/moved ones) */
        uint32_t n_rc_drops = 0;
        for (uint32_t k = 0; k < n_binds; k++) {
            if (binds[k].binding->type.kind == TY_RC && 
                !binding_moved_during_init[k] &&
                !binds[k].binding->is_moved &&
                !is_rc_binding_consumed(body, binds[k].binding)) {
                n_rc_drops++;
            }
        }
        
        if (n_rc_drops > 0) {
            /* Create new items array with space for rc drop defers */
            uint32_t new_n = body->as.do_.n + n_rc_drops;
            Expr **new_items = (Expr **)arena_alloc(e->arena, new_n * sizeof(Expr *));
            
            /* Copy existing items */
            memcpy(new_items, body->as.do_.items, body->as.do_.n * sizeof(Expr *));
            
            /* Add defer expressions for each unconsumed rc binding */
            uint32_t defer_idx = body->as.do_.n;
            for (uint32_t k = 0; k < n_binds; k++) {
                /* Skip RC bindings that are moved or consumed */
                if (binds[k].binding->type.kind == TY_RC && 
                    !binding_moved_during_init[k] &&
                    !binds[k].binding->is_moved &&
                    !is_rc_binding_consumed(body, binds[k].binding)) {
                    
                    /* Create a variable reference to the rc binding */
                    Expr *var_expr = expr_new(e->arena, EX_VAR, binds[k].binding->type, call->span);
                    var_expr->as.var.binding = binds[k].binding;
                    
                    /* Create the rc/drop expression */
                    Expr *rc_drop_expr = expr_new(e->arena, EX_RC_DROP, TYPE_NIL, call->span);
                    rc_drop_expr->as.rc_drop_.expr = var_expr;
                    
                    /* Create the defer expression wrapping the rc/drop */
                    Expr *defer_expr = expr_new(e->arena, EX_DEFER, TYPE_NIL, call->span);
                    defer_expr->as.defer_.body = rc_drop_expr;
                    
                    /* Capture analysis */
                    uint32_t n_free = 0;
                    Binding **free_vars = collect_free_vars(rc_drop_expr, NULL, 0, &n_free);
                    
                    Binding **captures = NULL;
                    uint8_t n_captures = 0;
                    if (n_free > 0) {
                        captures = (Binding **)arena_alloc(e->arena, n_free * sizeof(Binding *));
                        memcpy(captures, free_vars, n_free * sizeof(Binding *));
                        n_captures = (uint8_t)n_free;
                    }
                    free(free_vars);
                    
                    defer_expr->as.defer_.captures = captures;
                    defer_expr->as.defer_.n_captures = n_captures;
                    
                    new_items[defer_idx++] = defer_expr;
                }
            }
            
            /* Update the body with new items */
            body->as.do_.items = new_items;
            body->as.do_.n = new_n;
        }
    }

    /* LT1: At scope exit, verify all linear bindings were consumed */
    if (g_linear_enabled && rc == 0) {
        for (uint32_t k = 0; k < n_binds; k++) {
            Binding *lb = binds[k].binding;
            if (lb->is_linear && !lb->is_linear_consumed && !lb->is_moved) {
                diag_emit_with_code(DIAG_ERROR, lb->span,
                                    TUR_E0100_LINEAR_DROPPED,
                                    "linear value '%s' dropped without being consumed",
                                    lb->name->name);
                rc = -1;
            }
        }
    }

    /* ST1: At scope exit, verify all relevant bindings were used at least once */
    if (g_substructural_enabled && rc == 0) {
        for (uint32_t k = 0; k < n_binds; k++) {
            Binding *lb = binds[k].binding;
            if (lb->is_relevant && lb->usage_state == USAGE_UNUSED && !lb->is_moved) {
                diag_emit_with_code(DIAG_ERROR, lb->span,
                                    TUR_E0151_RELEVANT_DROPPED,
                                    "relevant value '%s' dropped without being used",
                                    lb->name->name);
                rc = -1;
            }
        }
    }

    /* Pop scope before returning. */
    e->scope = inner.parent;
    scope_free(&inner);

    /* Clean up move-state tracking memory */
    if (binding_moved_during_init) {
        free(binding_moved_during_init);
    }

    if (rc != 0) { free(binds); return NULL; }

    Expr *out = expr_new(e->arena, EX_LET, body->type, call->span);
    LetBinding *bcopy = (LetBinding *)arena_alloc(e->arena, n_binds * sizeof(LetBinding));
    memcpy(bcopy, binds, n_binds * sizeof(LetBinding));
    free(binds);
    out->as.let_.bindings = bcopy;
    out->as.let_.n = n_binds;
    out->as.let_.body = body;
    return out;
}

static Expr *elab_do(Elab *e, const Form *call) {
    /* (do body...) — value of last expr; (do) is nil. */
    uint32_t n = call->as.list.len - 1;
    if (n == 0) return e_nil(e, call->span);

    Expr **items = (Expr **)arena_alloc(e->arena, n * sizeof(Expr *));
    for (uint32_t i = 0; i < n; i++) {
        items[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!items[i]) return NULL;
    }
    
    /* Phase R6: Warn on discarded result values */
    if (g_warn_unused_result) {
        /* Check if this is an ignore! pattern: (do <expr> nil) */
        bool is_ignore_pattern = (n == 2 && items[1]->kind == EX_NIL_LIT);
        
        for (uint32_t i = 0; i < n - 1; i++) {
            /* All items except the last have their values discarded */
            if (items[i]->type.kind == TY_PTR_VOID) {
                /* This is a ptr<void> value being discarded - likely a Result */
                /* Skip warning if this is the ignore! pattern: (do expr nil) */
                if (!is_ignore_pattern || i != 0) {
                    diag_emit(DIAG_WARNING, items[i]->span,
                              "discarded result value of type ptr<void>; use ignore! to suppress this warning");
                }
            }
        }
    }
    
    Expr *out = expr_new(e->arena, EX_DO, items[n - 1]->type, call->span);
    out->as.do_.items = items;
    out->as.do_.n = n;
    return out;
}

static Expr *elab_unsafe(Elab *e, const Form *call) {
    /* (unsafe body...) — desugars to (handle (do body...) (Unsafe [] k) (resume k nil))
     * This wraps the body in a handler that discharges the Unsafe effect,
     * ensuring it cannot leak to the caller. */
    uint32_t n = call->as.list.len - 1;
    
    /* Phase U5: Linting for unsafe blocks */
    
    /* Check for nested unsafe blocks */
    if (g_unsafe_warn_nested && e->unsafe_depth > 0) {
        diag_emit(DIAG_WARNING, call->span,
                  "nested unsafe block: unsafe blocks should not be nested");
    }
    
    /* Check for ;;; SAFETY: comment */
    if (g_unsafe_require_safety) {
        /* For v1, we don't have access to the raw source to check for ;;; SAFETY: comments
         * So we just emit a warning that SAFETY comments are required */
        diag_emit(DIAG_WARNING, call->span,
                  "unsafe block requires a ;;; SAFETY: comment documenting why unsafe code is needed");
    }
    
    /* Track statistics */
    if (g_unsafe_stats_enabled) {
        g_unsafe_block_count++;
        g_unsafe_total_lines += n;
    }
    
    /* Check size threshold */
    if (g_unsafe_max_lines > 0 && n > g_unsafe_max_lines) {
        diag_emit(DIAG_WARNING, call->span,
                  "unsafe block has %u expressions (threshold: %u); consider breaking into smaller blocks",
                  n, g_unsafe_max_lines);
    }
    
    if (n == 0) {
        /* Empty unsafe block - warning */
        diag_emit(DIAG_WARNING, call->span,
                  "empty unsafe block has no effect");
        return e_nil(e, call->span);
    }

    /* Elaborate body expressions with increased unsafe_depth
     * (this allows unsafe-only operations inside the block) */
    Expr **items = (Expr **)arena_alloc(e->arena, n * sizeof(Expr *));
    e->unsafe_depth++;
    for (uint32_t i = 0; i < n; i++) {
        items[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!items[i]) {
            e->unsafe_depth--;
            return NULL;
        }
    }
    e->unsafe_depth--;

    /* Wrap multiple expressions in a do block */
    Expr *body;
    if (n == 1) {
        body = items[0];
    } else {
        body = expr_new(e->arena, EX_DO, items[n - 1]->type, call->span);
        body->as.do_.items = items;
        body->as.do_.n = n;
    }

    /* Create handle expression with Unsafe handler
     * The handler just resumes with nil (Unsafe effect carries no value) */
    HandleCase *unsafe_case = arena_alloc(e->arena, sizeof(HandleCase));
    unsafe_case->effect_name = e->sym_effect_unsafe;
    unsafe_case->n_params = 0;
    unsafe_case->param_names = NULL;
    unsafe_case->param_bindings = NULL;
    
    /* Create k binding (dummy continuation for Unsafe which returns nil) */
    Binding *k_binding = binding_new(e, e->sym_k, TYPE_NIL, false, false, call->span);
    k_binding->type.copy_kind = CK_MOVE;
    unsafe_case->k_name = e->sym_k;
    unsafe_case->k_binding = k_binding;
    
    /* Create handler body: (resume k nil) */
    /* First create nil literal */
    Expr *nil_lit = expr_new(e->arena, EX_NIL_LIT, TYPE_NIL, call->span);
    
    /* Create k variable reference */
    Expr *k_var = expr_new(e->arena, EX_VAR, k_binding->type, call->span);
    k_var->as.var.binding = k_binding;
    
    /* Create resume expression directly (resume k nil) */
    ResumeExpr *resume = arena_alloc(e->arena, sizeof(ResumeExpr));
    resume->k = k_var;
    resume->value = nil_lit;
    
    Expr *resume_expr = expr_new(e->arena, EX_RESUME, TYPE_NIL, call->span);
    resume_expr->as.resume_.resume = resume;
    
    unsafe_case->body = resume_expr;

    /* Create handle expression */
    HandleExpr *handle = arena_alloc(e->arena, sizeof(HandleExpr));
    handle->body = body;
    handle->cases = unsafe_case;
    handle->n_cases = 1;

    Expr *out = expr_new(e->arena, EX_HANDLE, body->type, call->span);
    out->as.handle_.handle = handle;
    return out;
}



static Expr *elab_if(Elab *e, const Form *call) {
    if (call->as.list.len != 3 && call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "if expects (if cond then) or (if cond then else); got %u argument(s)",
                  call->as.list.len - 1);
        return NULL;
    }
    Expr *cond = elab_form(e, call->as.list.items[1]);
    if (!cond) return NULL;
    if (!type_eq(cond->type, TYPE_BOOL)) {
        diag_emit(DIAG_ERROR, cond->span,
                  "if condition must be bool, got %s", type_name(cond->type));
        return NULL;
    }

    Binding **move_bindings = NULL;
    bool *before_states = NULL;
    uint32_t n_move_bindings = move_state_snapshot_bindings(e->scope, &move_bindings, &before_states);

    /* LT1: Snapshot linear consumption state before branches. */
    Binding **lin_bindings = NULL;
    bool *lin_before = NULL;
    uint32_t n_lin = 0;
    if (g_linear_enabled) {
        n_lin = linear_state_snapshot_bindings(e->scope, &lin_bindings, &lin_before);
    }

    Expr *then_ = elab_form(e, call->as.list.items[2]);
    if (!then_) {
        free(move_bindings);
        free(before_states);
        free(lin_bindings);
        free(lin_before);
        return NULL;
    }
    bool *then_states = move_state_capture_current(move_bindings, n_move_bindings);

    /* LT1: Capture linear consumption state after then-branch. */
    bool *lin_then = NULL;
    if (g_linear_enabled && n_lin > 0) {
        lin_then = linear_state_capture_current(lin_bindings, n_lin);
    }

    /* Rewind to pre-branch move-state before elaborating else branch. */
    move_state_restore(move_bindings, before_states, n_move_bindings);

    /* LT1: Rewind linear consumption state before elaborating else branch. */
    if (g_linear_enabled && n_lin > 0) {
        linear_state_restore(lin_bindings, lin_before, n_lin);
    }

    Expr *else_ = NULL;
    Type result_t = TYPE_NIL;
    if (call->as.list.len == 4) {
        else_ = elab_form(e, call->as.list.items[3]);
        if (!else_) {
            move_state_restore(move_bindings, before_states, n_move_bindings);
            free(then_states);
            free(move_bindings);
            free(before_states);
            free(lin_then);
            free(lin_bindings);
            free(lin_before);
            return NULL;
        }
        bool *else_states = move_state_capture_current(move_bindings, n_move_bindings);

        /* A move is guaranteed after if/else only if it was present before,
         * or both branches moved the binding. */
        for (uint32_t i = 0; i < n_move_bindings; i++) {
            move_bindings[i]->is_moved = before_states[i] || (then_states[i] && else_states[i]);
        }
        free(else_states);

        /* A branch that diverges — its top-level expression is a return,
         * throw, panic, panic-with, or has `!` (TYPE_NEVER) type — is
         * compatible with any other branch.  The if's result type is then
         * the non-diverging branch's type.  This lets `(? expr)` lower to
         * `(if cond (return ...) ok-val)` even when the two branch types
         * differ. */
        bool then_div = (then_->type.kind == TY_NEVER) ||
                        (then_->kind == EX_RETURN) ||
                        (then_->kind == EX_PANIC)  ||
                        (then_->kind == EX_PANIC_WITH);
        bool else_div = (else_->type.kind == TY_NEVER) ||
                        (else_->kind == EX_RETURN) ||
                        (else_->kind == EX_PANIC)  ||
                        (else_->kind == EX_PANIC_WITH);

        /* LT1: Capture linear state after else, check for branch mismatch, merge. */
        if (g_linear_enabled && n_lin > 0) {
            bool *lin_else = linear_state_capture_current(lin_bindings, n_lin);
            bool lin_ok = true;
            for (uint32_t i = 0; i < n_lin; i++) {
                if (lin_before[i]) continue; /* already consumed before if; fine */
                /* Skip mismatch check for diverging branches. */
                if (!then_div && !else_div && lin_then[i] != lin_else[i]) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                                        TUR_E0104_LINEAR_BRANCH_MISMATCH,
                                        "linear value '%s' consumed in one branch but not the other"
                                        " -- consume it in both branches or neither",
                                        lin_bindings[i]->name->name);
                    lin_ok = false;
                }
                /* Merge: use surviving branch's consumed state. */
                bool merged;
                if (then_div && else_div) {
                    merged = false; /* both diverge; treat as unconsumed */
                } else if (then_div) {
                    merged = lin_else[i];
                } else if (else_div) {
                    merged = lin_then[i];
                } else {
                    merged = lin_then[i] && lin_else[i];
                }
                lin_bindings[i]->is_linear_consumed = merged;
            }
            free(lin_else);
            if (!lin_ok) {
                free(then_states);
                free(move_bindings);
                free(before_states);
                free(lin_then);
                free(lin_bindings);
                free(lin_before);
                return NULL;
            }
        }

        if (then_div && else_div) {
            result_t = then_->type;  /* both diverge; pick either */
        } else if (then_div) {
            result_t = else_->type;
        } else if (else_div) {
            result_t = then_->type;
        } else if (!type_eq(then_->type, else_->type)) {
            free(then_states);
            free(move_bindings);
            free(before_states);
            free(lin_then);
            free(lin_bindings);
            free(lin_before);
            diag_emit(DIAG_ERROR, call->span,
                      "if branches have mismatched types: then=%s else=%s",
                      type_name(then_->type), type_name(else_->type));
            return NULL;
        } else {
            result_t = then_->type;
        }
    } else {
        /* Without else, then-branch moves are not guaranteed after the if. */
        move_state_restore(move_bindings, before_states, n_move_bindings);
        /* LT1: Without else, then-branch linear consumption is not guaranteed;
         * restore to the pre-if state so scope-exit checking fires if needed. */
        if (g_linear_enabled && n_lin > 0) {
            linear_state_restore(lin_bindings, lin_before, n_lin);
        }
    }

    free(then_states);
    free(move_bindings);
    free(before_states);
    free(lin_then);
    free(lin_bindings);
    free(lin_before);

    /* If no else, the if is a statement-style branch with type nil
     * (matches Clojure's behavior of returning nil for a missing else). */
    Expr *out = expr_new(e->arena, EX_IF, result_t, call->span);
    out->as.if_.cond = cond;
    out->as.if_.then_ = then_;
    out->as.if_.else_or_null = else_;
    return out;
}



/* Phase 6: Threading macro ->  */
/* (-> x (f a) (g b)) expands to (g (f x a) b) */
static Expr *elab_thread(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "-> requires at least one argument");
        return NULL;
    }
    
    /* Start with the initial value as a Form */
    Form *current = call->as.list.items[1];
    
    /* Process remaining forms */
    for (uint32_t i = 2; i < call->as.list.len; i++) {
        Form *form = call->as.list.items[i];
        if (form->tag == F_SYM) {
            /* (-> x f) -> (f x) */
            current = form_list(e->arena, call->span,
                (Form *[]){form, current}, 2);
        } else if (form->tag == F_LIST) {
            /* (-> x (f a b)) -> (f x a b) */
            /* Prepend current to the list arguments */
            uint32_t n = form->as.list.len;
            Form **new_items = (Form **)arena_alloc(e->arena, (n + 1) * sizeof(Form *));
            new_items[0] = form->as.list.items[0]; /* function name */
            new_items[1] = current; /* insert current as first arg */
            for (uint32_t j = 1; j < n; j++) {
                new_items[j + 1] = form->as.list.items[j];
            }
            current = form_list(e->arena, call->span, new_items, n + 1);
        } else {
            diag_emit(DIAG_ERROR, form->span,
                      "-> expected symbol or list, got %s",
                      form->tag == F_VEC ? "vector" : "other");
            return NULL;
        }
    }
    
    /* Elaborate the final form */
    return elab_form(e, current);
}

/* Phase 6: Threading macro ->>  */
/* (->> x (f a) (g b)) expands to (g b (f x a)) - value as last arg */
static Expr *elab_thread_last(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "->> requires at least one argument");
        return NULL;
    }
    
    /* Start with the initial value as a Form */
    Form *current = call->as.list.items[1];
    
    /* Process remaining forms */
    for (uint32_t i = 2; i < call->as.list.len; i++) {
        Form *form = call->as.list.items[i];
        if (form->tag == F_SYM) {
            /* (->> x f) -> (f x) - same as -> for single arg */
            current = form_list(e->arena, call->span,
                (Form *[]){form, current}, 2);
        } else if (form->tag == F_LIST) {
            /* (->> x (f a b)) -> (f a b x) - append current as last arg */
            uint32_t n = form->as.list.len;
            Form **new_items = (Form **)arena_alloc(e->arena, (n + 1) * sizeof(Form *));
            for (uint32_t j = 0; j < n; j++) {
                new_items[j] = form->as.list.items[j];
            }
            new_items[n] = current; /* append current as last arg */
            current = form_list(e->arena, call->span, new_items, n + 1);
        } else {
            diag_emit(DIAG_ERROR, form->span,
                      "->> expected symbol or list, got %s",
                      form->tag == F_VEC ? "vector" : "other");
            return NULL;
        }
    }
    
    /* Elaborate the final form */
    return elab_form(e, current);
}

static Expr *elab_set(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "set! takes (set! name value)");
        return NULL;
    }
    Form *target = call->as.list.items[1];

    /* Phase 12: Handle (set! (@ r) value) - mutation through mutable borrow */
    if (target->tag == F_LIST && target->as.list.len == 2
        && target->as.list.items[0]->tag == F_SYM
        && target->as.list.items[0]->as.sym == e->sym_deref) {
        return elab_set_deref(e, call, target);
    }

    if (target->tag != F_SYM) {
        diag_emit(DIAG_ERROR, target->span, "set! target must be a symbol or (@ borrow)");
        return NULL;
    }
    Binding *b = scope_lookup(e->scope, target->as.sym);
    if (!b) {
        diag_emit(DIAG_ERROR, target->span,
                  "set!: '%s' is not bound", target->as.sym->name);
        return NULL;
    }
    /* Phase 11: Check if target binding has been moved */
    if (b->is_moved) {
        diag_emit_with_code(DIAG_ERROR, target->span, TUR_E0005_USE_AFTER_MOVE,
                            "use-after-move: cannot set! '%s' because it was moved",
                            b->name->name);
        return NULL;
    }
    if (!b->is_mut) {
        diag_emit(DIAG_ERROR, target->span,
                  "set!: '%s' is immutable; use ^mut at the binding site to allow it",
                  b->name->name);
        return NULL;
    }
    Expr *value = elab_form(e, call->as.list.items[2]);
    if (!value) return NULL;
    if (!type_eq(value->type, b->type)) {
        diag_emit(DIAG_ERROR, value->span,
                  "set!: value type %s does not match binding type %s",
                  type_name(value->type), type_name(b->type));
        return NULL;
    }

    /* Phase 11: Move tracking - if value is a CK_MOVE binding reference, poison it */
    if (value->kind == EX_VAR && type_is_move(value->as.var.binding->type)) {
        binding_mark_moved(value->as.var.binding, value->span);
    }

    Expr *out = expr_new(e->arena, EX_SET, TYPE_NIL, call->span);
    out->as.set_.target = b;
    out->as.set_.value = value;
    return out;
}

static Expr *elab_while(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span, "while requires a condition");
        return NULL;
    }
    Expr *cond = elab_form(e, call->as.list.items[1]);
    if (!cond) return NULL;
    if (!type_eq(cond->type, TYPE_BOOL)) {
        diag_emit(DIAG_ERROR, cond->span,
                  "while condition must be bool, got %s", type_name(cond->type));
        return NULL;
    }
    uint32_t n = call->as.list.len - 2;
    Expr *body;
    if (n == 0) body = e_nil(e, call->span);
    else if (n == 1) {
        body = elab_form(e, call->as.list.items[2]);
        if (!body) return NULL;
    } else {
        Expr **items = (Expr **)arena_alloc(e->arena, n * sizeof(Expr *));
        for (uint32_t i = 0; i < n; i++) {
            items[i] = elab_form(e, call->as.list.items[2 + i]);
            if (!items[i]) return NULL;
        }
        body = expr_new(e->arena, EX_DO, TYPE_NIL, call->span);
        body->as.do_.items = items;
        body->as.do_.n = n;
    }
    Expr *out = expr_new(e->arena, EX_WHILE, TYPE_NIL, call->span);
    out->as.while_.cond = cond;
    out->as.while_.body = body;
    return out;
}

/* case desugars to (let [g disc] (if (= g v1) e1 (if (= g v2) e2 ... eN))).
 * Syntax: (case val v1 e1 v2 e2 ... :else eN) */
static Expr *elab_case(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "case requires (case val v1 e1 ...), got empty call");
        return NULL;
    }
    uint32_t n = call->as.list.len - 2; /* clause forms after discriminant */
    if ((n & 1) != 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "case expects pairs of (value expr) after discriminant; got %u clause forms", n);
        return NULL;
    }

    /* Generate a gensym for the discriminant to avoid multiple evaluation. */
    char disc_name[32];
    snprintf(disc_name, sizeof(disc_name), "case_disc_%u", e->next_gensym_id++);
    const Symbol *disc_sym = symtab_intern(e->st, strslice(disc_name, (uint32_t)strlen(disc_name)));
    Form *disc_sym_f = form_sym(e->arena, call->span, disc_sym);

    /* Intern = symbol for building (= g vi) tests */
    const Symbol *sym_eq = symtab_intern(e->st, strslice("=", 1));
    Form *sym_eq_f = form_sym(e->arena, call->span, sym_eq);

    /* Intern 'if' and ':else' for building the nested if tree */
    Form *sym_if_f  = form_sym(e->arena, call->span, e->sym_if);

    /* Build the nested if tree right-to-left */
    Form *acc = form_nil(e->arena, call->span);
    for (int i = (int)n - 2; i >= 0; i -= 2) {
        Form *val  = call->as.list.items[2 + i];
        Form *body = call->as.list.items[2 + i + 1];

        if (val->tag == F_KEYWORD && val->as.sym == e->kw_else) {
            /* :else — replace acc with the else body */
            acc = body;
            continue;
        }

        /* Build (= disc_sym val) */
        Form *eq_items[3] = { sym_eq_f, disc_sym_f, val };
        Form *test_f = form_list(e->arena, val->span, eq_items, 3);

        /* Build (if test body acc) */
        Form *if_items[4] = { sym_if_f, test_f, body, acc };
        acc = form_list(e->arena, val->span, if_items, 4);
    }

    /* Wrap in (let [disc_sym disc_expr] acc) */
    Form *disc_f = call->as.list.items[1];
    Form *bind_vec_items[2] = { disc_sym_f, disc_f };
    Form *bind_vec = form_vec(e->arena, call->span, bind_vec_items, 2);
    Form *sym_let_f = form_sym(e->arena, call->span, e->sym_let);
    Form *let_items[3] = { sym_let_f, bind_vec, acc };
    Form *let_form = form_list(e->arena, call->span, let_items, 3);

    return elab_form(e, let_form);
}

/* Phase 4: defer — (defer expr)
 * Records an expression to be evaluated at scope exit, in LIFO order.
 * For now, only valid inside let/do/defn/while bodies.
 * The body is elaborated but its value is discarded (defer always evaluates to nil).
 * v1 lowering (effects-plan.md §6.10): performs capture analysis for thunk lifting.
 * Nested defers are not yet supported.
 */
static Expr *elab_defer(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span, "defer requires an expression");
        return NULL;
    }
    /* Phase M5: module-level defer (top-level scope) is allowed.
     * The body runs at process exit via atexit(). No captures — at global
     * scope all referenced names are already global bindings. */
    if (e->scope == &e->global) {
        Expr *body = elab_form(e, call->as.list.items[1]);
        if (!body) return NULL;
        Expr *out = expr_new(e->arena, EX_DEFER, TYPE_NIL, call->span);
        out->as.defer_.body       = body;
        out->as.defer_.captures   = NULL;
        out->as.defer_.n_captures = 0;
        return out;
    }
    /* Elaborate the body expression */
    Expr *body = elab_form(e, call->as.list.items[1]);
    if (!body) return NULL;
    
    /* v1 lowering: Collect free variables (captures) for thunk lifting.
     * Per effects-plan.md §6.10.1, defers are entries in a list-on-frame.
     * For defers that reference local variables, we need to capture them
     * in an env struct (same pattern as closures in Phase 3).
     * 
     * For defer thunks (unlike closure thunks), there are no "params" — all
     * non-global bindings referenced in the defer body need to be captured
     * since the thunk is a separate function at file scope.
     * We pass empty params to collect_free_vars so all non-global bindings
     * are treated as free variables (captures). */
    Binding **captures = NULL;
    uint8_t n_captures = 0;
    
    /* Collect free variables in the defer body - pass empty params
     * so all non-global bindings are captured */
    uint32_t n_free = 0;
    Binding **free_vars = collect_free_vars(body, NULL, 0, &n_free);
    
    if (n_free > 0) {
        /* Store captures in the defer expression (arena-allocated, lives for compilation) */
        captures = (Binding **)arena_alloc(e->arena, n_free * sizeof(Binding *));
        memcpy(captures, free_vars, n_free * sizeof(Binding *));
        n_captures = (uint8_t)n_free;
    }
    
    free(free_vars);
    
    /* Create EX_DEFER expression with capture info */
    Expr *out = expr_new(e->arena, EX_DEFER, TYPE_NIL, call->span);
    out->as.defer_.body = body;
    out->as.defer_.captures = captures;
    out->as.defer_.n_captures = n_captures;
    return out;
}

/* Phase 3/4: return — (return) or (return expr)
 * Early return from a function, firing all defers in the scope chain.
 * 
 * Grammar: (return) or (return expr)
 * The return value type must match the function's return type.
 * 
 * Per effects-plan.md §6.10: "return: walk every enclosing frame, fire defers 
 * per frame, then function-exit." The codegen emits tur_frame_fire_chain to 
 * walk the parent chain and fire all defers before returning.
 */
static Expr *elab_return(Elab *e, const Form *call) {
    /* return is only valid inside function bodies */
    if (e->scope == &e->global) {
        diag_emit(DIAG_ERROR, call->span,
                  "return is not allowed at module top level");
        return NULL;
    }
    
    /* Check number of arguments */
    if (call->as.list.len > 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "return takes at most one argument: (return) or (return expr)");
        return NULL;
    }
    
    Expr *value = NULL;
    if (call->as.list.len == 2) {
        /* (return expr) */
        value = elab_form(e, call->as.list.items[1]);
        if (!value) return NULL;

        /* Phase 11: returning a move-only binding transfers ownership. */
        if (value->kind == EX_VAR && type_is_move(value->as.var.binding->type)) {
            binding_mark_moved(value->as.var.binding, value->span);
        }
    }
    
    /* Create EX_RETURN expression */
    /* The type will be determined by the function's return type during type checking.
     * For now, use the value's type or NIL if no value. */
    Type return_type = value ? value->type : TYPE_NIL;
    Expr *out = expr_new(e->arena, EX_RETURN, return_type, call->span);
    out->as.return_.value = value;
    return out;
}

/* Phase 5: ref — (ref expr)
 * Creates an owning reference to a heap-allocated value.
 * The compiler injects a defer (drop! r) at the binding site for auto-cleanup.
 * 
 * Grammar: (ref expr)
 * Returns: ref<T> where T is the type of expr
 * 
 * Move semantics: Once a ref binding is moved (assigned to another binding),
 * the source is poisoned and cannot be used again.
 */
static Expr *elab_ref(Elab *e, const Form *call) {
    /* Minimum: (ref expr) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "ref requires an expression: (ref expr)");
        return NULL;
    }
    
    /* Elaborate the inner expression */
    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;
    
    /* ref<T> where T is the inner expression's type */
    Type ref_type = type_ref(inner->type.kind);

    /* Create EX_REF expression */
    Expr *out = expr_new(e->arena, EX_REF, ref_type, call->span);
    out->as.ref_.expr = inner;
    return out;
}

/* LT3: lref/new — (lref/new expr)
 * Heap-allocates expr and returns an lref<T> (linear owning pointer).
 * The caller must consume the returned lref<T> exactly once.
 *
 * Grammar: (lref/new expr)
 * Returns: lref<T>
 */
static Expr *elab_lref_new(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "lref/new requires an expression: (lref/new expr)");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* lref<T> where T is the inner expression's type */
    Type lref_type = type_lref(inner->type.kind);

    /* Create EX_REF expression — lref/new lowers identically to ref at C level */
    Expr *out = expr_new(e->arena, EX_REF, lref_type, call->span);
    out->as.ref_.expr = inner;
    return out;
}

/* Phase 5: deref — (@ expr)
 * Dereferences a ref<T> or ptr<T>, returning T.
 * 
 * Grammar: (@ expr)
 * expr must have type ref<T>, rc<T>, or ptr<T>
 * Returns: T
 */
static Expr *elab_deref(Elab *e, const Form *call) {
    /* Minimum: (@ expr) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "@ requires an expression: (@ expr)");
        return NULL;
    }
    
    /* Elaborate the inner expression */
    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;
    
    /* Check that inner is ref<T>, lref<T>, rc<T>, ptr<T>, &T, or &mut T */
    if (inner->type.kind != TY_REF && inner->type.kind != TY_LREF
        && inner->type.kind != TY_RC && inner->type.kind != TY_PTR_VOID
        && inner->type.kind != TY_REF_IMMUT && inner->type.kind != TY_REF_MUT) {
        diag_emit(DIAG_ERROR, call->span,
                  "@ requires ref<T>, lref<T>, rc<T>, ptr<T>, &T, or &mut T, got %s",
                  type_name(inner->type));
        return NULL;
    }

    /* Return type is the inner type */
    Type result_type;
    if (inner->type.kind == TY_REF || inner->type.kind == TY_LREF) {
        result_type = type_from_kind(inner->type.as.ref.inner);
    } else if (inner->type.kind == TY_RC) {
        result_type = type_from_kind(inner->type.as.rc.inner);
    } else if (inner->type.kind == TY_REF_IMMUT || inner->type.kind == TY_REF_MUT) {
        /* Phase 12: &T and &mut T dereference to T */
        result_type = type_from_kind(inner->type.as.ref_borrow.target);
    } else {
        /* ptr<void> derefs to void* for now - could be more precise */
        result_type = TYPE_PTR_VOID;
    }
    
    /* Create EX_DEREF expression */
    Expr *out = expr_new(e->arena, EX_DEREF, result_type, call->span);
    out->as.deref_.expr = inner;
    return out;
}

/* Phase 5: drop! — (drop! expr)
 * Explicitly drops a ref<T>, freeing the underlying allocation.
 * Grammar: (drop! expr)
 * Returns: nil
 * Note: This is used by auto-injected defers for ref bindings.
 * Move semantics: After drop!, the ref should not be used (enforced at codegen time
 * by the elaborator not tracking moves yet - future work). */
static Expr *elab_drop(Elab *e, const Form *call) {
    /* Minimum: (drop! expr) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "drop! requires an expression: (drop! expr)");
        return NULL;
    }
    if (call->as.list.len > 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "drop! takes exactly 1 argument: (drop! expr)");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* drop! works on ref<T> */
    if (inner->type.kind != TY_REF) {
        diag_emit(DIAG_ERROR, call->span,
                  "drop! requires ref<T>, got %s", type_name(inner->type));
        return NULL;
    }

    /* Look up the drop! builtin spec and create a BUILTIN expression */
    /* drop! is a unary operator on ref<T> that returns nil */
    const BuiltinSpec *spec = builtin_lookup(e->sym_drop, inner->type, 1);
    if (!spec) {
        diag_emit(DIAG_ERROR, call->span,
                  "internal error: drop! builtin not found");
        return NULL;
    }

    /* Create a BUILTIN expression */
    Expr *out = expr_new(e->arena, EX_BUILTIN, TYPE_NIL, call->span);
    out->as.builtin.spec = spec;
    out->as.builtin.n = 1;
    out->as.builtin.args = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
    out->as.builtin.args[0] = inner;

    return out;
}

/* Phase 9: rc<T> operations */

/* (rc/of x) - Create a new rc<T> with x as the value.
 * Allocates a control block, copies/moves x into it.
 * Returns: rc<T>
 */
static Expr *elab_rc_of(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(rc/of x) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* LT1: Reject wrapping a linear value in rc<T> */
    if (g_linear_enabled && inner->type.copy_kind == CK_LINEAR) {
        const char *val_name = (inner->kind == EX_VAR)
            ? inner->as.var.binding->name->name : "linear value";
        diag_emit_with_code(DIAG_ERROR, call->span,
                            TUR_E0103_LINEAR_IN_RC,
                            "cannot wrap linear value '%s' in rc<T> -- "
                            "shared ownership violates linearity",
                            val_name);
        return NULL;
    }

    /* UT1: Reject wrapping a unique value in rc<T> */
    if (g_unique_enabled && inner->kind == EX_VAR &&
        inner->as.var.binding->is_unique) {
        diag_emit_with_code(DIAG_ERROR, call->span,
                            TUR_E0202_UNIQUE_IN_RC,
                            "cannot wrap unique value '%s' in rc<T> -- "
                            "shared ownership violates uniqueness",
                            inner->as.var.binding->name->name);
        return NULL;
    }

    /* rc<T> where T is the inner expression's type */
    Type rc_type = type_rc(inner->type.kind);

    /* Create EX_RC_OF expression */
    Expr *out = expr_new(e->arena, EX_RC_OF, rc_type, call->span);
    out->as.rc_of_.expr = inner;
    return out;
}

/* (rc/clone r) - Increment strong count, return new rc<T> pointing to same value.
 * Returns: rc<T>
 */
static Expr *elab_rc_clone(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(rc/clone r) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* Argument must be rc<T> */
    if (inner->type.kind != TY_RC) {
        diag_emit(DIAG_ERROR, call->span,
                  "rc/clone requires rc<T>, got %s", type_name(inner->type));
        return NULL;
    }

    /* rc/clone returns rc<T> with the same inner type */
    Type rc_type = inner->type;  /* Same type as input */

    /* Create EX_RC_CLONE expression */
    Expr *out = expr_new(e->arena, EX_RC_CLONE, rc_type, call->span);
    out->as.rc_clone_.expr = inner;
    out->as.rc_clone_.elide = false;  /* Phase 9 follow-up: elision pass may set true */
    return out;
}

/* (rc/drop r) - Decrement strong count.
 * Returns: nil
 */
static Expr *elab_rc_drop(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(rc/drop r) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* Argument must be rc<T> */
    if (inner->type.kind != TY_RC) {
        diag_emit(DIAG_ERROR, call->span,
                  "rc/drop requires rc<T>, got %s", type_name(inner->type));
        return NULL;
    }

    /* Create EX_RC_DROP expression */
    Expr *out = expr_new(e->arena, EX_RC_DROP, TYPE_NIL, call->span);
    out->as.rc_drop_.expr = inner;
    out->as.rc_drop_.elide = false;  /* Phase 9 follow-up: elision pass may set true */
    return out;
}

/* (rc->ptr r) - Borrow a ptr<T> from an rc<T>.
 * Returns: ptr<void> (for now; could be more precise with generics)
 */
static Expr *elab_rc_ptr(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(rc->ptr r) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* Argument must be rc<T> */
    if (inner->type.kind != TY_RC) {
        diag_emit(DIAG_ERROR, call->span,
                  "rc->ptr requires rc<T>, got %s", type_name(inner->type));
        return NULL;
    }

    /* Returns ptr<void> for now */
    Type result_type = TYPE_PTR_VOID;

    /* Create EX_RC_PTR expression */
    Expr *out = expr_new(e->arena, EX_RC_PTR, result_type, call->span);
    out->as.rc_ptr_.expr = inner;
    return out;
}

/* (rc/strong-count r) - Get the strong count for debugging.
 * Returns: int
 */
static Expr *elab_rc_strong_count(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(rc/strong-count r) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* Argument must be rc<T> */
    if (inner->type.kind != TY_RC) {
        diag_emit(DIAG_ERROR, call->span,
                  "rc/strong-count requires rc<T>, got %s", type_name(inner->type));
        return NULL;
    }

    /* Create EX_RC_COUNT expression */
    Expr *out = expr_new(e->arena, EX_RC_COUNT, TYPE_INT, call->span);
    out->as.rc_count_.expr = inner;
    return out;
}

/* (rc/from-ref r) - Move a ref<T> into rc<T>.
 * Returns: rc<T>
 */
static Expr *elab_rc_from_ref(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(rc/from-ref r) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    if (inner->type.kind != TY_REF) {
        diag_emit(DIAG_ERROR, call->span,
                  "rc/from-ref requires ref<T>, got %s", type_name(inner->type));
        return NULL;
    }

    if (inner->kind == EX_VAR) {
        binding_mark_moved(inner->as.var.binding, inner->span);
    }

    Type rc_type = type_rc(inner->type.as.ref.inner);
    Expr *out = expr_new(e->arena, EX_RC_FROM_REF, rc_type, call->span);
    out->as.rc_from_ref_.expr = inner;
    return out;
}

/* (ref/from-rc r) - Extract a ref<T> from a unique rc<T>.
 * Requires strong-count == 1 and no weak observers at runtime.
 * Returns: ref<T>
 */
static Expr *elab_ref_from_rc(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(ref/from-rc r) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    if (inner->type.kind != TY_RC) {
        diag_emit(DIAG_ERROR, call->span,
                  "ref/from-rc requires rc<T>, got %s", type_name(inner->type));
        return NULL;
    }

    if (inner->kind == EX_VAR) {
        binding_mark_moved(inner->as.var.binding, inner->span);
    }

    Type ref_type = type_ref(inner->type.as.rc.inner);
    Expr *out = expr_new(e->arena, EX_REF_FROM_RC, ref_type, call->span);
    out->as.ref_from_rc_.expr = inner;
    return out;
}

/* Phase 9: weak<T> operations */

/* (weak r) - Create a weak<T> from an rc<T>.
 * Returns: weak<T>
 */
static Expr *elab_weak(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(weak r) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* Argument must be rc<T> */
    if (inner->type.kind != TY_RC) {
        diag_emit(DIAG_ERROR, call->span,
                  "weak requires rc<T>, got %s", type_name(inner->type));
        return NULL;
    }

    /* weak<T> where T is the inner type of the rc */
    Type weak_type = type_weak(inner->type.as.rc.inner);

    /* Create EX_WEAK expression */
    Expr *out = expr_new(e->arena, EX_WEAK, weak_type, call->span);
    out->as.weak_.expr = inner;
    return out;
}

/* (upgrade w) - Upgrade weak<T> to option<rc<T>>.
 * Returns: option<rc<T>>
 */
static Expr *elab_weak_upgrade(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(upgrade w) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* Argument must be weak<T> */
    if (inner->type.kind != TY_WEAK) {
        diag_emit(DIAG_ERROR, call->span,
                  "upgrade requires weak<T>, got %s", type_name(inner->type));
        return NULL;
    }

    /* Returns option<rc<T>> */
    /* For now, we return rc<T> wrapped in option */
    /* option<rc<T>> would be a separate type, but we'll use rc<T> for simplicity in v1 */
    /* TODO: Proper option type when option is fully implemented */
    Type result_type = inner->type;  /* weak<T> - for now, upgrade returns weak<T> or rc<T> */
    /* Actually, upgrade should return option<rc<T>> */
    /* Since option isn't fully implemented, we'll return rc<T> or nil */
    /* For Phase 9, we'll just return rc<T> to keep it simple */
    result_type = type_rc(inner->type.as.rc.inner);

    /* Create EX_WEAK_UPGRADE expression */
    Expr *out = expr_new(e->arena, EX_WEAK_UPGRADE, result_type, call->span);
    out->as.weak_upgrade_.expr = inner;
    return out;
}

/* (weak? w) - Check if w is a weak<T>.
 * Returns: bool
 */
static Expr *elab_weak_pred(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(weak? w) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* Create EX_WEAK_PRED expression */
    Expr *out = expr_new(e->arena, EX_WEAK_PRED, TYPE_BOOL, call->span);
    out->as.weak_pred_.expr = inner;
    return out;
}

/* (ref? x) - Check if x is a ref<T>.
 * Returns: bool
 */
static Expr *elab_ref_pred(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(ref? x) requires exactly one argument");
        return NULL;
    }

    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* Create EX_REF_PRED expression — always true if the type is TY_REF */
    Expr *out = expr_new(e->arena, EX_REF_PRED, TYPE_BOOL, call->span);
    out->as.ref_pred_.expr = inner;
    return out;
}

/* Phase 10: GC builtins */

/* (gc!) - Force a garbage collection cycle. Returns nil. */
static Expr *elab_gc_force(Elab *e, const Form *call) {
    if (call->as.list.len != 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "(gc!) takes no arguments");
        return NULL;
    }
    /* For Phase 10 v1: emit as inline-C with gc_force() call */
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    ic->code = strslice("gc_force();", 11);
    ic->return_type = TYPE_NIL;
    ic->captures = NULL;
    ic->n_captures = 0;
    Expr *out = expr_new(e->arena, EX_INLINE_C, TYPE_NIL, call->span);
    out->as.inline_c_.inline_c = ic;
    return out;
}

/* (gc-enable!) - Enable cycle collection. Returns nil. */
static Expr *elab_gc_enable(Elab *e, const Form *call) {
    if (call->as.list.len != 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "(gc-enable!) takes no arguments");
        return NULL;
    }
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    ic->code = strslice("gc_enable();", 12);
    ic->return_type = TYPE_NIL;
    ic->captures = NULL;
    ic->n_captures = 0;
    Expr *out = expr_new(e->arena, EX_INLINE_C, TYPE_NIL, call->span);
    out->as.inline_c_.inline_c = ic;
    return out;
}

/* (gc-disable!) - Disable cycle collection. Returns nil. */
static Expr *elab_gc_disable(Elab *e, const Form *call) {
    if (call->as.list.len != 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "(gc-disable!) takes no arguments");
        return NULL;
    }
    InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
    ic->code = strslice("gc_disable();", 13);
    ic->return_type = TYPE_NIL;
    ic->captures = NULL;
    ic->n_captures = 0;
    Expr *out = expr_new(e->arena, EX_INLINE_C, TYPE_NIL, call->span);
    out->as.inline_c_.inline_c = ic;
    return out;
}


/* Phase 18: Delimited continuations */

/* (reset body) - Establish a continuation boundary.
 */
static Expr *elab_reset(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(reset body) requires exactly one argument");
        return NULL;
    }
    Expr *body = elab_form(e, call->as.list.items[1]);
    if (!body) return NULL;
    Expr *out = expr_new(e->arena, EX_RESET, body->type, call->span);
    out->as.reset_.body = body;
    return out;
}

/* (shift k body) - Capture the current continuation.
 */
static Expr *elab_shift(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(shift k body) requires exactly two arguments");
        return NULL;
    }
    Expr *k_expr = elab_form(e, call->as.list.items[1]);
    if (!k_expr) return NULL;
    
    /* Check if k_expr is a function, closure, or a var referencing a function */
    bool is_function = false;
    if (k_expr->kind == EX_FN || k_expr->kind == EX_CLOSURE) {
        is_function = true;
    } else if (k_expr->kind == EX_VAR) {
        /* Check if the binding is a function */
        Binding *b = k_expr->as.var.binding;
        if (b && (b->type.kind == TY_FN || b->closure_fn_binding)) {
            is_function = true;
        }
    }
    
    if (!is_function) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "shift requires a function as first argument");
        return NULL;
    }
    
    Expr *body = elab_form(e, call->as.list.items[2]);
    if (!body) return NULL;
    /* The result type of shift is the result type of calling k_fn with body's value.
     * For now, we use body's type as a placeholder (full type inference deferred). */
    Expr *out = expr_new(e->arena, EX_SHIFT, body->type, call->span);
    out->as.shift_.k_fn = k_expr;
    out->as.shift_.body = body;
    return out;
}

/* (shift0 k body) - One-shot shift.
 */
static Expr *elab_shift0(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(shift0 k body) requires exactly two arguments");
        return NULL;
    }
    Expr *k_expr = elab_form(e, call->as.list.items[1]);
    if (!k_expr) return NULL;
    
    /* Check if k_expr is a function, closure, or a var referencing a function */
    bool is_function = false;
    if (k_expr->kind == EX_FN || k_expr->kind == EX_CLOSURE) {
        is_function = true;
    } else if (k_expr->kind == EX_VAR) {
        /* Check if the binding is a function */
        Binding *b = k_expr->as.var.binding;
        if (b && (b->type.kind == TY_FN || b->closure_fn_binding)) {
            is_function = true;
        }
    }
    
    if (!is_function) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "shift0 requires a function as first argument");
        return NULL;
    }
    
    Expr *body = elab_form(e, call->as.list.items[2]);
    if (!body) return NULL;
    /* The result type of shift0 is the result type of calling k_fn with body's value.
     * For now, we use body's type as a placeholder (full type inference deferred). */
    Expr *out = expr_new(e->arena, EX_SHIFT0, body->type, call->span);
    out->as.shift0_.k_fn = k_expr;
    out->as.shift0_.body = body;
    return out;
}

/* Phase B2: Cloneable continuations */

/* CPS-CL10 (elab-time): Walk all local bindings visible at a cloneable-shift
 * site and emit TUR-E0014 for any that lack a Clone instance.  This is a
 * conservative check — it covers every binding in scope, not just those that
 * are actually live at the shift; liveness-precise checking would require the
 * CPS pass.  Running early (in elab) gives diagnostics before codegen. */
static void check_cloneable_capture(Elab *e, Span span) {
    const Symbol *clone_sym = intern_cstr(e->st, "Clone");
    TypeClass *clone_tc = typeclass_env_lookup_typeclass(&e->typeclass_env, clone_sym);
    if (!clone_tc) return; /* No Clone typeclass in scope; nothing to check */

    for (Scope *s = e->scope; s != NULL && s != &e->global; s = s->parent) {
        for (uint32_t i = 0; i < s->n; i++) {
            Binding *b = s->bindings[i];
            if (!b || !b->name) continue;
            Type t = b->type;
            /* Primitive/function/continuation types are always safe to capture */
            if (t.kind == TY_NIL || t.kind == TY_FN ||
                t.kind == TY_CLONEABLE_CONT) continue;
            TypeClassInstance *inst =
                typeclass_env_lookup_instance(&e->typeclass_env, clone_tc, &t, 1);
            if (!inst) {
                diag_emit_with_code(DIAG_ERROR, span,
                                    TUR_E0014_NOT_CLONE,
                                    "captured binding '%s' does not implement Clone "
                                    "(required by cloneable-shift)", b->name->name);
            }
        }
    }
}

/* (cloneable-reset body) - Establish a continuation boundary with cloneable captures.
 * Similar to reset, but all captured values must implement Clone.
 * The body can use cloneable-shift to capture a cloneable continuation. */
static Expr *elab_cloneable_reset(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(cloneable-reset body) requires exactly one argument");
        return NULL;
    }
    /* CPS-CL7: track nesting depth so cloneable-shift can detect missing reset */
    e->cloneable_reset_depth++;
    Expr *body = elab_form(e, call->as.list.items[1]);
    e->cloneable_reset_depth--;
    if (!body) return NULL;
    Expr *out = expr_new(e->arena, EX_CLONEABLE_RESET, body->type, call->span);
    out->as.cloneable_reset_.body = body;
    return out;
}

/* (cloneable-shift k body) - Capture the current cloneable continuation.
 * Similar to shift, but the continuation passed to k is cloneable (multi-shot).
 * All captured environment values must implement Clone.
 * k is a function that receives the cloneable continuation as its first argument.
 */
static Expr *elab_cloneable_shift(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(cloneable-shift k body) requires exactly two arguments");
        return NULL;
    }

    /* CPS-CL7: cloneable-shift must be inside a cloneable-reset */
    if (e->cloneable_reset_depth == 0) {
        diag_emit_with_code(DIAG_ERROR, call->span,
                            TUR_E0016_CLONEABLE_SHIFT_OUTSIDE_RESET,
                            "cloneable-shift used outside of any cloneable-reset boundary");
        return NULL;
    }

    Expr *k_expr = elab_form(e, call->as.list.items[1]);
    if (!k_expr) return NULL;

    /* Check if k_expr is a function, closure, or a var referencing a function */
    bool is_function = false;
    if (k_expr->kind == EX_FN || k_expr->kind == EX_CLOSURE) {
        is_function = true;
    } else if (k_expr->kind == EX_VAR) {
        /* Check if the binding is a function */
        Binding *b = k_expr->as.var.binding;
        if (b && (b->type.kind == TY_FN || b->closure_fn_binding)) {
            is_function = true;
        }
    }

    if (!is_function) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "cloneable-shift requires a function as first argument");
        return NULL;
    }

    Expr *body = elab_form(e, call->as.list.items[2]);
    if (!body) return NULL;
    /* The result type of cloneable-shift is the result type of calling k_fn with
     * a cloneable continuation and body's value. For now, use body's type as placeholder. */
    Expr *out = expr_new(e->arena, EX_CLONEABLE_SHIFT, body->type, call->span);
    out->as.cloneable_shift_.k_fn = k_expr;
    out->as.cloneable_shift_.body = body;
    out->as.cloneable_shift_.cont_body = NULL;
    /* CPS-CL10: verify all local captures implement Clone at elaboration time */
    check_cloneable_capture(e, call->span);
    return out;
}

/* (call/cc* f) - Sugar for (cloneable-reset (cloneable-shift k (f k)))
 * Multi-shot call/cc that produces a cloneable continuation. */
static Expr *elab_call_cc_star(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(call/cc* f) requires exactly one argument");
        return NULL;
    }
    Expr *f_expr = elab_form(e, call->as.list.items[1]);
    if (!f_expr) return NULL;

    /* CPS-CL8: Desugar (call/cc* f) into:
     *   (cloneable-reset (cloneable-shift f 0))
     *
     * The shift emitter allocates a continuation and passes it to k_fn (= f).
     * So f receives the continuation directly — no wrapper needed. */

    /* Determine f's return type */
    Type call_result_type = TYPE_INT;
    if (f_expr->kind == EX_VAR && f_expr->as.var.binding
        && f_expr->as.var.binding->type.kind == TY_FN) {
        call_result_type = type_from_kind(
            f_expr->as.var.binding->type.as.fn.result_kind);
    }

    /* Build EX_INT_LIT for the default value 0 */
    Expr *zero = expr_new(e->arena, EX_INT_LIT, TYPE_INT, call->span);
    zero->as.i = 0;

    /* Build EX_CLONEABLE_SHIFT: (cloneable-shift f 0) */
    Expr *shift = expr_new(e->arena, EX_CLONEABLE_SHIFT, call_result_type, call->span);
    shift->as.cloneable_shift_.k_fn = f_expr;
    shift->as.cloneable_shift_.body = zero;
    shift->as.cloneable_shift_.live_captures = NULL;
    shift->as.cloneable_shift_.n_live_captures = 0;
    shift->as.cloneable_shift_.cont_body = NULL;

    /* Build EX_CLONEABLE_RESET wrapping the shift */
    Expr *reset = expr_new(e->arena, EX_CLONEABLE_RESET, call_result_type, call->span);
    reset->as.cloneable_reset_.body = shift;

    return reset;
}

/* Phase 21: Serializable continuations */

/* Check that all local bindings visible at a serial-shift site implement
 * the Serializable typeclass.  Mirrors check_cloneable_capture() for Clone. */
static void check_serializable_capture(Elab *e, Span span) {
    const Symbol *ser_sym = intern_cstr(e->st, "Serializable");
    TypeClass *ser_tc = typeclass_env_lookup_typeclass(&e->typeclass_env, ser_sym);
    if (!ser_tc) return; /* Serializable not yet in scope; defer to a later pass */

    for (Scope *s = e->scope; s != NULL && s != &e->global; s = s->parent) {
        for (uint32_t i = 0; i < s->n; i++) {
            Binding *b = s->bindings[i];
            if (!b || !b->name) continue;
            Type t = b->type;
            /* Primitive types that are always serializable */
            if (t.kind == TY_NIL || t.kind == TY_FN ||
                t.kind == TY_CLONEABLE_CONT || t.kind == TY_CONT) continue;
            TypeClassInstance *inst =
                typeclass_env_lookup_instance(&e->typeclass_env, ser_tc, &t, 1);
            if (!inst) {
                diag_emit_with_code(DIAG_ERROR, span,
                                    TUR_E0018_NOT_SERIALIZABLE,
                                    "captured binding '%s' does not implement Serializable "
                                    "(required by serial-shift)\n"
                                    "  = help: use serial-reset outside non-serializable "
                                    "resources, or implement Serializable for the type",
                                    b->name->name);
            }
        }
    }
}

/* (serial-reset body) - Establish a serializable continuation boundary.
 * Like reset, but marks the region so serial-shift can capture it. */
static Expr *elab_serial_reset(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(serial-reset body) requires exactly one argument");
        return NULL;
    }
    e->serial_reset_depth++;
    Expr *body = elab_form(e, call->as.list.items[1]);
    e->serial_reset_depth--;
    if (!body) return NULL;
    Expr *out = expr_new(e->arena, EX_SERIAL_RESET, body->type, call->span);
    out->as.serial_reset_.body = body;
    return out;
}

/* (serial-shift k body) - Capture the current continuation as a serializable one.
 * k is a function that receives the serial-continuation as its argument.
 * All captured environment values must implement Serializable. */
static Expr *elab_serial_shift(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(serial-shift k body) requires exactly two arguments");
        return NULL;
    }

    if (e->serial_reset_depth == 0) {
        diag_emit_with_code(DIAG_ERROR, call->span,
                            TUR_E0019_SERIAL_SHIFT_OUTSIDE_RESET,
                            "serial-shift used outside of any serial-reset boundary");
        return NULL;
    }

    Expr *k_expr = elab_form(e, call->as.list.items[1]);
    if (!k_expr) return NULL;

    bool is_function = false;
    if (k_expr->kind == EX_FN || k_expr->kind == EX_CLOSURE) {
        is_function = true;
    } else if (k_expr->kind == EX_VAR) {
        Binding *b = k_expr->as.var.binding;
        if (b && (b->type.kind == TY_FN || b->closure_fn_binding)) {
            is_function = true;
        }
    }
    if (!is_function) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "serial-shift requires a function as first argument");
        return NULL;
    }

    Expr *body = elab_form(e, call->as.list.items[2]);
    if (!body) return NULL;

    Expr *out = expr_new(e->arena, EX_SERIAL_SHIFT, body->type, call->span);
    out->as.serial_shift_.k_fn = k_expr;
    out->as.serial_shift_.body = body;

    /* Check that all captured bindings implement Serializable. */
    check_serializable_capture(e, call->span);
    return out;
}

/* Phase 19: Algebraic effects */

/* Forward declaration (defined after elab_defeffect) */
static Expr *elab_handle(Elab *e, const Form *call);
/* ET3-E: Forward declaration for compose-handlers */
static Expr *elab_compose_handlers(Elab *e, const Form *call);

/* (try-with body (EffectName [params] k) handler ...)
 * Sugar: identical to (handle body ...).
 * Provided for OCaml/algebraic-effects familiarity.
 */
static Expr *elab_try_with(Elab *e, const Form *call) {
    /* try-with has the same surface syntax as handle; delegate directly. */
    return elab_handle(e, call);
}

/* (defeffect Name [param1 : T1, param2 : T2, ...] : R)
 * (defeffect ^private Name [param1 : T1, param2 : T2, ...] : R)
 * Declares a new algebraic effect with parameters and a result type.
 * The optional ^private annotation restricts the effect to the defining module.
 */
static Expr *elab_defeffect(Elab *e, const Form *call) {
    /* Minimum: (defeffect Name [params...] :ret-type)
     * Or with visibility: (defeffect ^private Name [params...] :ret-type) */
    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "defeffect requires (defeffect Name [params...] result-type)");
        return NULL;
    }

    /* Phase P19-6: Parse optional ^private visibility annotation */
    bool is_private = false;
    uint32_t name_idx = 1;  /* index of the effect name form */
    if (call->as.list.len >= 5) {
        Form *maybe_private = call->as.list.items[1];
        if (maybe_private->tag == F_SYM
            && maybe_private->as.sym == e->sym_caret_private) {
            is_private = true;
            name_idx = 2;
        }
    }

    /* Parse effect name */
    Form *name_f = call->as.list.items[name_idx];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "defeffect: effect name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_f->as.sym;
    
    /* Check if effect already exists */
    if (effect_env_contains(e->effect_env, name)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "defeffect: '%s' is already defined", name->name);
        return NULL;
    }
    
    /* Parse parameter list (index shifts by 1 when ^private is present) */
    Form *params_f = call->as.list.items[name_idx + 1];
    if (params_f->tag != F_LIST && params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "defeffect: expected parameter list, got %s",
                  form_tag_name(params_f->tag));
        return NULL;
    }
    
    /* Parse parameter list.
     * Accepts:
     *   []          — no params
     *   [x y]       — untyped params (default to TY_INT)
     *   [x :int y :cstr] — typed params (name :type pairs)
     */
    uint8_t raw_n = (uint8_t)params_f->as.list.len;
    /* Count actual params (skip type keyword items) */
    uint8_t n_params = 0;
    for (uint8_t i = 0; i < raw_n; i++) {
        Form *f = params_f->as.list.items[i];
        if (f->tag == F_SYM) n_params++;
        /* F_KEYWORD items are type annotations, not params */
    }
    const Symbol **param_names = arena_alloc(e->arena, n_params * sizeof(const Symbol *));
    TypeKind *param_types = arena_alloc(e->arena, n_params * sizeof(TypeKind));

    {
        uint8_t p = 0;
        for (uint8_t i = 0; i < raw_n; i++) {
            Form *param_f = params_f->as.list.items[i];
            if (param_f->tag == F_KEYWORD || param_f->tag == F_TYPE_ANN) {
                /* Type annotation for preceding param — already handled below */
                continue;
            }
            if (param_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, param_f->span,
                          "defeffect: parameter name must be a symbol");
                return NULL;
            }
            param_names[p] = param_f->as.sym;
            /* Check if the next item is a type keyword or F_TYPE_ANN */
            TypeKind pk = TY_INT;
            if (i + 1 < raw_n) {
                Form *next = params_f->as.list.items[i + 1];
                if (next->tag == F_KEYWORD) {
                    pk = typekind_from_symbol(next->as.sym->name);
                    if (pk == TY_UNKNOWN) pk = TY_INT;
                    i++; /* Consume the type keyword */
                } else if (next->tag == F_TYPE_ANN) {
                    if (next->as.list.len > 0) {
                        Type *ann = type_expr_from_form(e, next->as.list.items[0], NULL, NULL, NULL, 0);
                        if (ann) pk = ann->kind;
                    }
                    i++; /* Consume the type annotation */
                }
            }
            param_types[p] = pk;
            p++;
        }
    }
    
    /* Parse return type.
     * v1 accepts both historical symbol syntax and keyword syntax:
     *   (defeffect E [] int)
     *   (defeffect E [] :int)
     */
    Form *ret_f = call->as.list.items[name_idx + 2];
    if (ret_f->tag != F_SYM && ret_f->tag != F_KEYWORD && ret_f->tag != F_TYPE_ANN) {
        diag_emit(DIAG_ERROR, ret_f->span,
                  "defeffect: return type annotation must be a symbol or keyword like :int");
        return NULL;
    }

    TypeKind result_type;
    if (ret_f->tag == F_TYPE_ANN) {
        Type *ann = (ret_f->as.list.len > 0)
            ? type_expr_from_form(e, ret_f->as.list.items[0], NULL, NULL, NULL, 0)
            : NULL;
        result_type = ann ? ann->kind : TY_UNKNOWN;
    } else {
        result_type = typekind_from_symbol(ret_f->as.sym->name);
    }
    if (result_type == TY_UNKNOWN) {
        diag_emit(DIAG_ERROR, ret_f->span,
                  "defeffect: unknown return type '%s'", ret_f->as.sym->name);
        return NULL;
    }
    
    /* ET4: Parse optional ^extends ParentName after the return type */
    const Symbol *parent_name = NULL;
    uint32_t extends_start = name_idx + 3; /* items after (defeffect [^private] Name [params] :ret) */
    for (uint32_t xi = extends_start; xi + 1 < (uint32_t)call->as.list.len; xi++) {
        Form *f = call->as.list.items[xi];
        if (f->tag == F_SYM && f->as.sym == e->sym_caret_extends) {
            Form *pname_f = call->as.list.items[xi + 1];
            if (pname_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, pname_f->span,
                          "defeffect: expected effect name after ^extends");
                return NULL;
            }
            parent_name = pname_f->as.sym;
            break;
        }
    }

    /* Register the effect — pass module visibility info (Phase P19-6) */
    Effect *effect = effect_env_register(e->effect_env, e->arena, name,
                                          param_names, param_types, n_params, result_type,
                                          e->current_module_name, is_private);
    if (!effect) return NULL;

    /* ET4: Resolve parent effect if ^extends was specified */
    if (parent_name) {
        Effect *parent_eff = effect_env_lookup(e->effect_env, parent_name);
        if (!parent_eff) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "defeffect: unknown parent effect '%s' in ^extends clause",
                      parent_name->name);
            return NULL;
        }
        effect->parent = parent_eff;
    }

    /* Create the effect definition expression */
    EffectDef *def = arena_alloc(e->arena, sizeof(EffectDef));
    def->name = name;
    def->param_names = param_names;
    def->param_types = param_types;
    def->n_params = n_params;
    def->result_type = result_type;
    def->is_private = is_private;
    def->defining_module_name = e->current_module_name;
    def->parent_name = parent_name;  /* ET4: ^extends parent effect name */

    Expr *out = expr_new(e->arena, EX_DEFECT, TYPE_NIL, call->span);
    out->as.effect_def_.def = def;
    return out;
}

/* Phase 19 TUR-E0008: Helpers for unhandled-effect scope tracking. */
static bool is_effect_handled(Elab *e, const Symbol *name) {
    for (uint32_t i = 0; i < e->n_handled_effects; i++) {
        if (e->handled_effect_names[i] == name) return true;
    }
    return false;
}

static void push_handled_effect(Elab *e, const Symbol *name) {
    if (e->n_handled_effects >= e->cap_handled_effects) {
        e->cap_handled_effects = e->cap_handled_effects ? e->cap_handled_effects * 2 : 8;
        e->handled_effect_names = (const Symbol **)realloc(
            e->handled_effect_names,
            e->cap_handled_effects * sizeof(const Symbol *));
    }
    e->handled_effect_names[e->n_handled_effects++] = name;
}

/* PR5-3-D: Check if an effect was explicitly imported via :refer [(effect Name)]. */
static bool elab_effect_is_referred(const Elab *e, const Effect *eff) {
    for (uint32_t i = 0; i < e->n_referred_effects; i++) {
        if (e->referred_effects[i] == eff) return true;
    }
    return false;
}

/* (perform (EffectName arg1 arg2 ...))
 * Perform an algebraic effect with arguments.
 */
static Expr *elab_perform(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "perform requires (perform (EffectName arg1 ...))");
        return NULL;
    }
    
    /* Parse effect call form: (EffectName arg1 arg2 ...) */
    Form *effect_call_f = call->as.list.items[1];
    if (effect_call_f->tag != F_LIST) {
        diag_emit(DIAG_ERROR, effect_call_f->span,
                  "perform: expected effect call as list, got %s",
                  form_tag_name(effect_call_f->tag));
        return NULL;
    }
    
    if (effect_call_f->as.list.len < 1) {
        diag_emit(DIAG_ERROR, effect_call_f->span,
                  "perform: effect call must have at least an effect name");
        return NULL;
    }
    
    /* Parse effect name */
    Form *name_f = effect_call_f->as.list.items[0];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "perform: effect name must be a symbol");
        return NULL;
    }
    const Symbol *effect_name = name_f->as.sym;
    
    /* Check if effect exists */
    Effect *effect = effect_env_lookup(e->effect_env, effect_name);
    if (!effect) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "perform: unknown effect '%s'", effect_name->name);
        return NULL;
    }

    /* Phase P19-6 / ER5: Enforce effect visibility — private effects cannot be
     * performed from outside their defining module (TUR-E0021). */
    if (!effect->is_exported
        && effect->defining_module_name != NULL
        && effect->defining_module_name != e->current_module_name
        && !elab_effect_is_referred(e, effect)) {
        diag_emit_with_code(DIAG_ERROR, name_f->span,
                            TUR_E0021_PRIVATE_EFFECT,
                            "effect '%s' is private to module '%s'",
                            effect_name->name, effect->defining_module_name->name);
        diag_emit(DIAG_NOTE, name_f->span,
                  "remove ^private from the defeffect declaration to make it public");
        return NULL;
    }

    /* Phase 19 TUR-E0008: At top level (not inside a defn/fn body), every
     * perform must be wrapped by an enclosing handle expression. Inside a
     * defn/fn the caller is expected to provide the handler, so we skip. */
    if (e->fn_body_depth == 0 && !is_effect_handled(e, effect_name)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "[TUR-E0008]: effect '%s' is performed but has no enclosing handler",
                  effect_name->name);
        diag_emit(DIAG_NOTE, name_f->span,
                  "wrap the expression with a (handle ...) block or a handler macro");
        return NULL;
    }

    /* Parse arguments */
    uint8_t n_args = effect_call_f->as.list.len - 1;
    Expr **args = arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint8_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, effect_call_f->as.list.items[i + 1]);
        if (!args[i]) return NULL;
    }
    
    /* Create the perform expression */
    PerformExpr *perform = arena_alloc(e->arena, sizeof(PerformExpr));
    perform->effect_name = effect_name;
    perform->args = args;
    perform->n_args = n_args;
    
    /* The return type of perform is the result type of the effect */
    Type result_type = type_from_kind(effect->constructor->result_type);
    
    Expr *out = expr_new(e->arena, EX_PERFORM, result_type, call->span);
    out->as.perform_.perform = perform;
    return out;
}

/* (handle expr case1 case2 ...)
 * Handle algebraic effects with cases.
 * Each case: (EffectName [param1 param2 ...] k) body ...
 */
static Expr *elab_handle(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "handle requires (handle expr case1 case2 ...)");
        return NULL;
    }
    
    /* Phase 19 TUR-E0008: Pre-scan case headers to push effect names onto the
     * handled-effects scope BEFORE elaborating the body.  This ensures that
     * perform calls inside the body see the enclosing handle as a handler,
     * without changing the case elaboration logic below. */
    uint32_t n_case_forms_pre = call->as.list.len >= 2 ? call->as.list.len - 2 : 0;
    uint8_t n_cases_pre = (uint8_t)((n_case_forms_pre / 2));
    uint32_t saved_n_handled = e->n_handled_effects;
    for (uint8_t i = 0; i < n_cases_pre; i++) {
        Form *case_f = call->as.list.items[2 + (i * 2)];
        if (case_f->tag == F_LIST && case_f->as.list.len >= 1 &&
            case_f->as.list.items[0]->tag == F_SYM) {
            push_handled_effect(e, case_f->as.list.items[0]->as.sym);
        }
    }

    /* Parse the body to be handled */
    Expr *body = elab_form(e, call->as.list.items[1]);

    /* Pop the pre-scanned handlers after body elaboration */
    e->n_handled_effects = saved_n_handled;

    if (!body) return NULL;
    
    /* Parse handle cases.
     * Surface syntax follows effects-plan.md §4.3:
     *   (handle expr
     *     (Effect [params...] k) body
     *     ...)
     * so cases are provided as header/body pairs.
     */
    uint32_t n_case_forms = call->as.list.len - 2;
    if ((n_case_forms & 1U) != 0U) {
        diag_emit(DIAG_ERROR, call->span,
                  "handle expects pairs of (case-header body)");
        return NULL;
    }

    uint8_t n_cases = (uint8_t)(n_case_forms / 2);
    HandleCase *cases = arena_alloc(e->arena, n_cases * sizeof(HandleCase));
    
    for (uint8_t i = 0; i < n_cases; i++) {
        Form *case_f = call->as.list.items[2 + (i * 2)];
        if (case_f->tag != F_LIST) {
            diag_emit(DIAG_ERROR, case_f->span,
                      "handle: expected case as list, got %s",
                      form_tag_name(case_f->tag));
            return NULL;
        }
        
        /* Case header format: (EffectName [params...] k) */
        if (case_f->as.list.len != 3) {
            diag_emit(DIAG_ERROR, case_f->span,
                      "handle case header requires (EffectName [params...] k)");
            return NULL;
        }
        
        /* Parse effect name */
        Form *name_f = case_f->as.list.items[0];
        if (name_f->tag != F_SYM) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "handle case: effect name must be a symbol");
            return NULL;
        }
        cases[i].effect_name = name_f->as.sym;
        
        /* Parse parameter list */
        Form *params_f = case_f->as.list.items[1];
        if (params_f->tag != F_LIST && params_f->tag != F_VEC) {
            diag_emit(DIAG_ERROR, params_f->span,
                      "handle case: expected parameter list, got %s",
                      form_tag_name(params_f->tag));
            return NULL;
        }
        
        cases[i].n_params = params_f->as.list.len;
        cases[i].param_names = arena_alloc(e->arena, cases[i].n_params * sizeof(const Symbol *));
        cases[i].param_bindings = arena_alloc(e->arena, cases[i].n_params * sizeof(Binding *));
        for (uint8_t j = 0; j < cases[i].n_params; j++) {
            Form *param_f = params_f->as.list.items[j];
            if (param_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, param_f->span,
                          "handle case: parameter name must be a symbol");
                return NULL;
            }
            cases[i].param_names[j] = param_f->as.sym;
        }
        
        /* Parse continuation parameter name */
        Form *k_f = case_f->as.list.items[2];
        if (k_f->tag != F_SYM) {
            diag_emit(DIAG_ERROR, k_f->span,
                      "handle case: continuation name must be a symbol");
            return NULL;
        }
        cases[i].k_name = k_f->as.sym;
        
        /* Look up effect definition to get param types */
        Effect *eff = effect_env_lookup(e->effect_env, cases[i].effect_name);

        /* Phase P19-6 / ER5: Enforce effect visibility in handler declarations
         * (TUR-E0021). */
        if (eff && !eff->is_exported
            && eff->defining_module_name != NULL
            && eff->defining_module_name != e->current_module_name
            && !elab_effect_is_referred(e, eff)) {
            diag_emit_with_code(DIAG_ERROR, name_f->span,
                                TUR_E0021_PRIVATE_EFFECT,
                                "effect '%s' is private to module '%s'",
                                cases[i].effect_name->name, eff->defining_module_name->name);
            diag_emit(DIAG_NOTE, name_f->span,
                      "remove ^private from the defeffect declaration to make it public");
            return NULL;
        }

        /* Create a handler scope with bindings for params and k */
        Scope handler_scope;
        scope_init(&handler_scope, e->scope);
        Scope *saved_scope = e->scope;
        e->scope = &handler_scope;
        
        /* Create bindings for each parameter */
        for (uint8_t j = 0; j < cases[i].n_params; j++) {
            /* Use the effect's declared param type if available, else TY_INT */
            TypeKind pk = (eff && j < eff->constructor->n_params)
                ? eff->constructor->param_types[j] : TY_INT;
            Type ptype = type_from_kind(pk);
            Binding *pb = binding_new(e, cases[i].param_names[j], ptype, false, false, params_f->span);
            cases[i].param_bindings[j] = pb;
            scope_add(&handler_scope, pb);
        }
        
        /* Create binding for k (dummy continuation — int64).
         * Marked CK_MOVE so the one-shot check fires on a second resume/discontinue.
         * Marked is_continuation so async Send checks can detect continuation escape. */
        Binding *kb = binding_new(e, cases[i].k_name, TYPE_INT, false, false, k_f->span);
        kb->type.copy_kind = CK_MOVE;
        kb->is_continuation = true;
        cases[i].k_binding = kb;
        scope_add(&handler_scope, kb);
        
        /* Parse handler body inside the handler scope */
        Form *body_f = call->as.list.items[3 + (i * 2)];
        cases[i].body = elab_form(e, body_f);

        /* Restore outer scope */
        e->scope = saved_scope;
        scope_free(&handler_scope);

        if (!cases[i].body) return NULL;

        /* ET3-C: Check that the handler clause result type matches the body type.
         * Skip the check when either is TY_UNKNOWN or TY_NEVER, or when the clause
         * body ends in a resume expression (resume returns the value type passed to
         * the continuation, not the overall handle result type). */
        {
            /* Helper: check if an expression ends in a resume.
             * Handles bare (resume ...) and (do ... (resume ...)) patterns. */
            const Expr *cb = cases[i].body;
            bool ends_in_resume = (cb->kind == EX_RESUME);
            if (!ends_in_resume && cb->kind == EX_DO && cb->as.do_.n > 0) {
                const Expr *last = cb->as.do_.items[cb->as.do_.n - 1];
                ends_in_resume = (last->kind == EX_RESUME);
            }

            if (!ends_in_resume && i == 0 && body != NULL
                && cases[i].body->type.kind != TY_UNKNOWN
                && cases[i].body->type.kind != TY_NEVER
                && body->type.kind != TY_UNKNOWN
                && body->type.kind != TY_NEVER
                && cases[i].body->type.kind != body->type.kind) {
                diag_emit_with_code(DIAG_ERROR, body_f->span,
                    TUR_E0252_HANDLER_RESULT_MISMATCH,
                    "handler clause result type '%s' does not match handle expression type '%s'",
                    type_name(cases[i].body->type), type_name(body->type));
            } else if (!ends_in_resume && i > 0 && cases[0].body != NULL) {
                /* Also check if case 0's body ends in resume before comparing */
                const Expr *cb0 = cases[0].body;
                bool c0_ends_resume = (cb0->kind == EX_RESUME);
                if (!c0_ends_resume && cb0->kind == EX_DO && cb0->as.do_.n > 0) {
                    const Expr *last0 = cb0->as.do_.items[cb0->as.do_.n - 1];
                    c0_ends_resume = (last0->kind == EX_RESUME);
                }
                if (!c0_ends_resume
                    && cases[i].body->type.kind != TY_UNKNOWN
                    && cases[i].body->type.kind != TY_NEVER
                    && cases[0].body->type.kind != TY_UNKNOWN
                    && cases[0].body->type.kind != TY_NEVER
                    && cases[i].body->type.kind != cases[0].body->type.kind) {
                    diag_emit_with_code(DIAG_ERROR, body_f->span,
                        TUR_E0252_HANDLER_RESULT_MISMATCH,
                        "handler clause result type '%s' does not match other clauses' type '%s'",
                        type_name(cases[i].body->type), type_name(cases[0].body->type));
                }
            }
        }
    }

    /* Create the handle expression */
    HandleExpr *handle = arena_alloc(e->arena, sizeof(HandleExpr));
    handle->body = body;
    handle->cases = cases;
    handle->n_cases = n_cases;

    /* The return type of handle is the same as the body's type */
    Expr *out = expr_new(e->arena, EX_HANDLE, body->type, call->span);
    out->as.handle_.handle = handle;
    return out;
}

/* ET3-E: (compose-handlers h1 h2)
 * Compose two handlers that must handle different effects.
 * Emits TUR_E0251 if both handlers handle the same effect.
 * Returns a nil-typed expression (placeholder; runtime semantics TBD).
 */
static Expr *elab_compose_handlers(Elab *e, const Form *call) {
    if (!g_effect_types_enabled) {
        diag_emit(DIAG_ERROR, call->span,
                  "'compose-handlers' requires -Xeffect-types");
        return NULL;
    }
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(compose-handlers h1 h2) requires exactly two arguments");
        return NULL;
    }
    Expr *h1 = elab_form(e, call->as.list.items[1]);
    if (!h1) return NULL;
    Expr *h2 = elab_form(e, call->as.list.items[2]);
    if (!h2) return NULL;

    /* ET3: Both arguments should have TY_HANDLER type */
    if (h1->type.kind != TY_HANDLER) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "(compose-handlers): first argument must be a handler value, got '%s'",
                  type_name(h1->type));
        return NULL;
    }
    if (h2->type.kind != TY_HANDLER) {
        diag_emit(DIAG_ERROR, call->as.list.items[2]->span,
                  "(compose-handlers): second argument must be a handler value, got '%s'",
                  type_name(h2->type));
        return NULL;
    }

    /* ET3: Handlers must handle different effects (TUR-E0251) */
    if (h1->type.as.handler_.effect_name != NULL
        && h2->type.as.handler_.effect_name != NULL
        && h1->type.as.handler_.effect_name == h2->type.as.handler_.effect_name) {
        diag_emit_with_code(DIAG_ERROR, call->span,
            TUR_E0251_HANDLER_OVERLAP,
            "composed handlers both handle effect '%s'; overlapping effects are not allowed",
            h1->type.as.handler_.effect_name);
        return NULL;
    }

    /* Return a nil-typed placeholder expression */
    Expr *out = expr_new(e->arena, EX_NIL_LIT, TYPE_NIL, call->span);
    return out;
}

/* (resume k value)
 * Resume a captured continuation with a value.
 */
static Expr *elab_resume(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(resume k value) requires exactly two arguments");
        return NULL;
    }
    
    Expr *k = elab_form(e, call->as.list.items[1]);
    if (!k) return NULL;
    
    Expr *value = elab_form(e, call->as.list.items[2]);
    if (!value) return NULL;
    
    /* Phase 19: One-shot continuation check.
     * k is TY_CONT which is CK_MOVE — mark it consumed so any second
     * (resume k ...) triggers the existing use-after-move diagnostic. */
    if (k->kind == EX_VAR && type_is_move(k->as.var.binding->type)) {
        binding_mark_moved(k->as.var.binding, k->span);
    }
    
    ResumeExpr *resume = arena_alloc(e->arena, sizeof(ResumeExpr));
    resume->k = k;
    resume->value = value;
    
    Expr *out = expr_new(e->arena, EX_RESUME, value->type, call->span);
    out->as.resume_.resume = resume;
    return out;
}

/* (discontinue k exception)
 * Discontinue a captured continuation with an exception.
 */
static Expr *elab_discontinue(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(discontinue k exception) requires exactly two arguments");
        return NULL;
    }
    
    Expr *k = elab_form(e, call->as.list.items[1]);
    if (!k) return NULL;
    
    Expr *exception = elab_form(e, call->as.list.items[2]);
    if (!exception) return NULL;
    
    /* Phase 19: One-shot continuation check — same as resume. */
    if (k->kind == EX_VAR && type_is_move(k->as.var.binding->type)) {
        binding_mark_moved(k->as.var.binding, k->span);
    }
    
    DiscontinueExpr *discontinue = arena_alloc(e->arena, sizeof(DiscontinueExpr));
    discontinue->k = k;
    discontinue->exception = exception;
    
    Expr *out = expr_new(e->arena, EX_DISCONTINUE, TYPE_NIL, call->span);
    out->as.discontinue_.discontinue = discontinue;
    return out;
}

/* Phase 19: (cont? k) — check if a Phase 18 continuation has not been consumed.
 * For Phase 19 algebraic-effect continuations (which are CK_MOVE int64_t dummies),
 * the static one-shot check already catches double-resume at compile time.
 * For Phase 18 tur_cont* continuations, this checks tur_cont_consumed at runtime.
 * Returns: bool
 */
static Expr *elab_cont_pred(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(cont? k) requires exactly one argument");
        return NULL;
    }
    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;

    /* Accepts TY_CONT (Phase 18 tur_cont*) or TY_INT (Phase 19 CK_MOVE dummy k) */
    if (inner->type.kind != TY_CONT && inner->type.kind != TY_INT) {
        diag_emit(DIAG_ERROR, call->span,
                  "(cont? k) requires a continuation argument");
        return NULL;
    }

    Expr *out = expr_new(e->arena, EX_CONT_PRED, TYPE_BOOL, call->span);
    out->as.cont_pred_.expr = inner;
    return out;
}

/* Phase 6: defmacro — (defmacro name [params...] body...)
 * Defines a macro that will be expanded at compile time.
 * Syntax: (defmacro name [param1 param2 ...] body...)
 * The macro body can use quasiquote/unquote to build the output form.
 * The macro is stored and will be used to expand subsequent calls.
 */
static Expr *elab_defmacro(Elab *e, const Form *call) {
    /* Minimum: (defmacro name [params...] body...) */
    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "defmacro requires (defmacro name [params...] body...)");
        return NULL;
    }

    /* Parse name */
    Form *name_f = call->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "defmacro name must be a symbol");
        return NULL;
    }

    /* Check if macro already exists */
    if (elab_lookup_macro(e, name_f->as.sym)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "defmacro: '%s' is already defined", name_f->as.sym->name);
        return NULL;
    }

    /* Parse params */
    Form *params_f = call->as.list.items[2];
    if (params_f->tag != F_LIST && params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "defmacro: expected parameter list or vector, got %s", 
                  params_f->tag == F_SYM ? "symbol" : "non-list");
        return NULL;
    }

    /* Extract parameter symbols, detecting optional variadic '& rest' tail. */
    uint32_t n_total = params_f->as.list.len;
    /* Allocate enough space for all params (excluding '&' marker) */
    Form **params = (Form **)arena_alloc(e->arena, n_total * sizeof(Form *));
    uint32_t n_fixed = 0;
    bool is_variadic = false;
    const Symbol *rest_param = NULL;

    for (uint32_t i = 0; i < n_total; i++) {
        Form *p = params_f->as.list.items[i];
        if (p->tag != F_SYM) {
            diag_emit(DIAG_ERROR, p->span,
                      "defmacro: parameter must be a symbol, got %s", 
                      p->tag == F_KEYWORD ? "keyword" : "non-symbol");
            return NULL;
        }
        if (p->as.sym == e->sym_borrow) {
            /* '&' rest-arg marker: the next symbol is the rest param */
            if (i + 1 >= n_total) {
                diag_emit(DIAG_ERROR, p->span,
                          "defmacro: '&' must be followed by a rest parameter name");
                return NULL;
            }
            if (i + 2 < n_total) {
                diag_emit(DIAG_ERROR, params_f->as.list.items[i + 2]->span,
                          "defmacro: only one parameter may follow '&'");
                return NULL;
            }
            Form *rest_f = params_f->as.list.items[i + 1];
            if (rest_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, rest_f->span,
                          "defmacro: rest parameter after '&' must be a symbol");
                return NULL;
            }
            is_variadic = true;
            rest_param = rest_f->as.sym;
            break; /* no more params after & rest */
        }
        params[n_fixed++] = p;
    }

    /* The body is everything after the parameter list */
    uint32_t body_count = call->as.list.len - 3;
    if (body_count == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "defmacro: expected at least one body expression");
        return NULL;
    }

    /* For now, we only support a single body expression
     * (multi-expression macro bodies would need to be wrapped in do) */
    Form *body = call->as.list.items[3];
    if (body_count > 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "defmacro: multi-expression bodies not yet supported; wrap in do");
        return NULL;
    }

    /* Create the macro definition */
    MacroDef *macro = (MacroDef *)arena_alloc(e->arena, sizeof(MacroDef));
    macro->name = name_f->as.sym;
    macro->params = params;
    macro->n_params = n_fixed;
    macro->is_variadic = is_variadic;
    macro->rest_param = rest_param;
    macro->body = body;
    macro->span = call->span;
    macro->defining_module_name = e->current_module_name; /* Phase M4 */

    /* Register the macro */
    elab_register_macro(e, macro);

    /* Return nil - defmacro doesn't produce a value */
    return e_nil(e, call->span);
}

/* Phase 6: gensym — (gensym) or (gensym prefix-sym) generates a fresh symbol Form */
static Expr *elab_gensym(Elab *e, const Form *call) {
    /* Syntax: (gensym) or (gensym prefix) */
    const char *prefix = "g"; /* default prefix */
    
    if (call->as.list.len >= 2) {
        Form *prefix_f = call->as.list.items[1];
        if (prefix_f->tag == F_SYM) {
            prefix = prefix_f->as.sym->name;
        } else if (prefix_f->tag == F_STR) {
            prefix = prefix_f->as.s.p;
        } else if (prefix_f->tag == F_QUOTE && prefix_f->as.list.len == 1) {
            /* (gensym 'prefix) - get the symbol from the quote */
            Form *quoted = prefix_f->as.list.items[0];
            if (quoted->tag == F_SYM) {
                prefix = quoted->as.sym->name;
            } else {
                diag_emit(DIAG_ERROR, prefix_f->span,
                          "gensym: quoted prefix must be a symbol");
                return NULL;
            }
        } else {
            diag_emit(DIAG_ERROR, prefix_f->span,
                      "gensym: expected symbol, string, or quoted symbol prefix, got %s",
                      prefix_f->tag == F_INT ? "integer" : "other");
            return NULL;
        }
    }
    
    /* Generate a fresh symbol name */
    char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "%s_%u", prefix, e->next_gensym_id++);
    const Symbol *fresh_sym = symtab_intern(e->st, strslice(name_buf, (uint32_t)strlen(name_buf)));
    
    /* For phase 6: gensym generates a fresh symbol but doesn't register it. */
    /* The symbol can be used in macro output, but the user must bind it. */
    /* Return a symbol expression - in practice, this will error until gensym */
    /* is used in a proper context (like inside quasiquote in a macro body). */
    Binding *b = binding_new(e, fresh_sym, TYPE_INT, false, false, call->span);
    Expr *out = expr_new(e->arena, EX_VAR, TYPE_INT, call->span);
    out->as.var.binding = b;
    
    return out;
}

/* Phase R1: ? operator — (? expr)
 *
 * Lowers to:
 *   (let [__q_N expr]
 *     (if (err? __q_N)
 *         (return (err (err-val __q_N)))
 *         (ok-val __q_N)))
 *
 * Requires `err?`, `err`, `err-val`, and `ok-val` to be in scope (the fixture
 * defines these alongside the result-shaped type).  Must appear inside a
 * function body.  The if branches have types `!` (from `return`) and the
 * unwrapped ok type; `elab_if` accepts NEVER as compatible with any type and
 * propagates the other branch's type as the result.
 */
static Expr *elab_question(Elab *e, const Form *call) {
    if (e->fn_body_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "? operator is only allowed inside a function body");
        return NULL;
    }
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "? operator requires exactly one argument: (? expr)");
        return NULL;
    }

    Form *inner = call->as.list.items[1];
    Span span = call->span;

    /* Fresh symbol __q_N to avoid multiple evaluation of expr. */
    char q_name[32];
    snprintf(q_name, sizeof(q_name), "__q_%u", e->next_gensym_id++);
    const Symbol *q_sym = symtab_intern(e->st,
        strslice(q_name, (uint32_t)strlen(q_name)));
    Form *q_sym_f = form_sym(e->arena, span, q_sym);

    /* User-defined result helpers — looked up by name in scope. */
    const Symbol *sym_err_q   = symtab_intern(e->st, strslice("err?",    4));
    const Symbol *sym_err     = symtab_intern(e->st, strslice("err",     3));
    const Symbol *sym_err_val = symtab_intern(e->st, strslice("err-val", 7));
    const Symbol *sym_ok_val  = symtab_intern(e->st, strslice("ok-val",  6));

    Form *err_q_f   = form_sym(e->arena, span, sym_err_q);
    Form *err_f     = form_sym(e->arena, span, sym_err);
    Form *err_val_f = form_sym(e->arena, span, sym_err_val);
    Form *ok_val_f  = form_sym(e->arena, span, sym_ok_val);
    Form *if_f      = form_sym(e->arena, span, e->sym_if);
    Form *let_f     = form_sym(e->arena, span, e->sym_let);
    Form *return_f  = form_sym(e->arena, span, e->sym_return);

    /* (err? __q) */
    Form *test_items[2] = { err_q_f, q_sym_f };
    Form *test_form = form_list(e->arena, span, test_items, 2);

    /* (err-val __q) */
    Form *err_val_items[2] = { err_val_f, q_sym_f };
    Form *err_val_form = form_list(e->arena, span, err_val_items, 2);

    /* (err (err-val __q)) */
    Form *err_call_items[2] = { err_f, err_val_form };
    Form *err_call_form = form_list(e->arena, span, err_call_items, 2);

    /* (return (err (err-val __q))) */
    Form *return_items[2] = { return_f, err_call_form };
    Form *return_form = form_list(e->arena, span, return_items, 2);

    /* (ok-val __q) */
    Form *ok_val_items[2] = { ok_val_f, q_sym_f };
    Form *ok_val_form = form_list(e->arena, span, ok_val_items, 2);

    /* (if (err? __q) (return (err (err-val __q))) (ok-val __q)) */
    Form *if_items[4] = { if_f, test_form, return_form, ok_val_form };
    Form *if_form = form_list(e->arena, span, if_items, 4);

    /* (let [__q expr] if_form) */
    Form *bind_vec_items[2] = { q_sym_f, inner };
    Form *bind_vec = form_vec(e->arena, span, bind_vec_items, 2);
    Form *let_items[3] = { let_f, bind_vec, if_form };
    Form *let_form = form_list(e->arena, span, let_items, 3);

    return elab_form(e, let_form);
}

/* Phase G2: forward declaration (used in elab_defn for param constraint resolution) */
static TypeKind gadt_skolem_lookup(const SkolemEnv *env, const char *name);

/* Phase 2: defn — (defn name [param1 param2 ...] : return-type body...)
 * For now, we only support : int return type annotation. Param types are
 * inferred from usage. */
static Expr *elab_defn(Elab *e, const Form *call) {
    /* Phase R5: Check for #[no-unwind] attribute before name */
    uint32_t name_idx = 1;  /* index of name in items (after 'defn') */
    bool no_unwind = false;
    Form *name_f = call->as.list.items[name_idx];
    if (name_f->tag == F_SYM && name_f->as.sym == e->sym_no_unwind_attr) {
        no_unwind = true;
        name_idx++;
        name_f = call->as.list.items[name_idx];
    }

    /* Phase M6: Check for (export-as "c_name") attribute before name.
     * Syntax: (defn (export-as "c_name") fname [...] :ret body...)
     * The attribute is a list whose head is the symbol export-as. */
    const char *c_export_name = NULL;
    if (name_f->tag == F_LIST && name_f->as.list.len == 2 &&
        name_f->as.list.items[0]->tag == F_SYM &&
        name_f->as.list.items[0]->as.sym == e->sym_export_as_attr) {
        Form *cname_arg = name_f->as.list.items[1];
        if (cname_arg->tag != F_STR) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "^:export-as argument must be a string literal: (export-as \"c_name\")");
            return NULL;
        }
        char *cname_buf = (char *)arena_alloc(e->arena, cname_arg->as.s.len + 1);
        memcpy(cname_buf, cname_arg->as.s.p, cname_arg->as.s.len);
        cname_buf[cname_arg->as.s.len] = '\0';
        c_export_name = cname_buf;
        name_idx++;
        name_f = call->as.list.items[name_idx];
    }

    /* Minimum: (defn name []) or (defn #[no-unwind] name []) */
    if (name_idx + 2 >= call->as.list.len) {  /* need name, params, body */
        diag_emit(DIAG_ERROR, call->span,
                  "defn requires (defn name [params...] body...)");
        return NULL;
    }

    /* Parse name */
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "defn name must be a symbol");
        return NULL;
    }
    Binding *existing = scope_lookup(e->scope, name_f->as.sym);
    if (existing) {
        /* Allow forward-declared bindings from pass 1 to be redefined */
        /* Forward declarations have TY_FN type (from pass 1) */
        if (existing->type.kind == TY_FN && existing->is_global) {
            /* This is a forward declaration - proceed with the real definition */
        } else {
            diag_emit(DIAG_ERROR, name_f->span,
                      "defn: '%s' is already defined", name_f->as.sym->name);
            return NULL;
        }
    }

    /* Parse param vector */
    Form *params_f = call->as.list.items[name_idx + 1];
    if (params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "defn: parameter list must be a vector [name1 name2 ...]");
        return NULL;
    }

    /* Parse params - Phase 15 supports typeclass constraints
     * Syntax: [^Eq a x : a, y : a] means:
     *   - ^Eq is a constraint annotation (symbol starting with ^)
     *   - a is a type variable
     *   - x : a is parameter x with type annotation :a
     *   - y : a is parameter y with type annotation :a
     * 
     * We parse sequentially, collecting constraints and creating parameters.
     */
    Binding **params = NULL;
    uint8_t n_params = 0;
    TypeKind param_kinds[MAX_FN_ARITY];
    /* Phase HRT1: full type annotations for rank-2 poly params (NULL if not poly) */
    Type *param_poly_types[MAX_FN_ARITY];
    for (uint8_t _i = 0; _i < MAX_FN_ARITY; _i++) param_poly_types[_i] = NULL;

    /* Phase 15: Constraint parsing */
    /* Track pending constraints that apply to the next type variable */
    TypeClass *pending_constraints[8];  /* Max 8 constraints per function */
    uint8_t n_pending = 0;

    /* Map from type variable name to its index for constraint association */
    /* For v1, we use a simple approach: each constraint applies to the next type var */
    /* const Symbol *current_type_var = NULL; */  /* Deferred to v2 */

    /* Constraint set for this function - allocated on arena */
    TypeConstraint *constraint_list = NULL;
    uint8_t n_constraints = 0;

    /* Phase G3: Equality constraint env built from (: a T) param items */
    SkolemEnv param_constraint_env;
    param_constraint_env.n = 0;

    /* Phase HKT: kind-variable names collected from ^f annotations.
     * These are type-level variables with kind * -> *; no runtime param is
     * created.  They are pushed into a temporary scope so that return-type
     * annotations such as (Equal (f a) (f b)) can resolve (f a) as TY_APP. */
    const Symbol *kind_var_names[8];
    uint8_t n_kind_vars = 0;

    /* LT0: ^linear annotation applies to the next parameter */
    bool next_param_linear = false;
    /* UT0: ^unique annotation applies to the next parameter */
    bool next_param_unique = false;
    /* UT2: ^mut annotation applies to the next parameter */
    bool next_param_mut = false;
    /* ST0: ^affine / ^relevant annotations apply to the next parameter */
    bool next_param_affine    = false;
    bool next_param_relevant  = false;

    for (uint32_t i = 0; i < params_f->as.list.len; i++) {
        Form *p = params_f->as.list.items[i];

        /* Phase G3: Handle equality constraint (: a T) in params.
         * Syntax: (: a int) means "type variable a equals int".
         * This is a type-level constraint; no runtime parameter is created.
         *
         * Two reader representations:
         *  Legacy:  F_LIST([sym(":"), sym("a"), sym("int")])  — 3-item list
         *  New:     F_LIST([F_TYPE_ANN(sym("a")), sym("int")])  — 2-item list
         *           (reader folds `: a` into a single F_TYPE_ANN node)
         */
        {
            Form *var_form = NULL;
            Form *type_form = NULL;
            if (p->tag == F_LIST && p->as.list.len == 3 &&
                p->as.list.items[0]->tag == F_SYM &&
                p->as.list.items[0]->as.sym == e->sym_colon) {
                var_form  = p->as.list.items[1];
                type_form = p->as.list.items[2];
            } else if (p->tag == F_LIST && p->as.list.len == 2 &&
                       p->as.list.items[0]->tag == F_TYPE_ANN &&
                       p->as.list.items[0]->as.list.len == 1) {
                var_form  = p->as.list.items[0]->as.list.items[0];
                type_form = p->as.list.items[1];
            } else if (p->tag == F_LIST && p->as.list.len == 2 &&
                       p->as.list.items[0]->tag == F_UNQUOTE &&
                       p->as.list.items[0]->as.list.len == 1 &&
                       p->as.list.items[0]->as.list.items[0]->tag == F_SYM) {
                /* Phase G3: (~ a T) equality constraint — reader turns (~ a T) into
                 * F_LIST[F_UNQUOTE(a), T] because ~ is the unquote reader macro */
                var_form  = p->as.list.items[0]->as.list.items[0]; /* symbol inside ~a */
                type_form = p->as.list.items[1];
            }
            if (var_form && type_form &&
                var_form->tag == F_SYM &&
                (type_form->tag == F_SYM || type_form->tag == F_KEYWORD)) {
                const char *tn = (type_form->tag == F_KEYWORD)
                    ? type_form->as.sym->name
                    : type_form->as.sym->name;
                TypeKind ck = typekind_from_symbol(tn);
                if (ck != TY_UNKNOWN && param_constraint_env.n < MAX_SKOLEM_BINDINGS) {
                    param_constraint_env.bindings[param_constraint_env.n].name =
                        var_form->as.sym->name;
                    param_constraint_env.bindings[param_constraint_env.n].kind = ck;
                    param_constraint_env.n++;
                }
                continue;
            }
        }

        /* LT0: ^linear annotation marks the next parameter as linear */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_linear) {
            next_param_linear = true;
            continue;
        }

        /* UT0: ^unique annotation marks the next parameter as unique */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_unique) {
            next_param_unique = true;
            continue;
        }

        /* UT2: ^mut annotation marks the next parameter as mutable (used with ^unique ^mut) */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_mut) {
            next_param_mut = true;
            continue;
        }

        /* ST0: ^affine annotation marks the next parameter as affine (no duplication) */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_affine) {
            next_param_affine = true;
            continue;
        }

        /* ST0: ^relevant annotation marks the next parameter as relevant (must be used) */
        if (p->tag == F_SYM && p->as.sym == e->sym_caret_relevant) {
            next_param_relevant = true;
            continue;
        }

        /* Phase 15: Handle constraint annotations (^Eq, ^Show, etc.) */
        if (p->tag == F_SYM && p->as.sym->len > 0 && p->as.sym->name[0] == '^') {
            /* This is a constraint annotation like ^Eq */
            const Symbol *constraint_name = p->as.sym;
            /* Look up the typeclass (skip the ^ character) */
            const char *tc_name_str = constraint_name->name + 1;  /* Skip '^' */
            uint32_t tc_name_len = constraint_name->len - 1;

            /* Phase HKT H4: Kind variable annotation — type-level only, erased at runtime.
             * A kind variable like `^f` declares that `f` ranges over type constructors
             * of kind '* -> *'.  It creates no runtime parameter; it is used as a
             * kind annotation on the function and may be followed by a ^Typeclass f
             * constraint (which adds a regular typeclass constraint handled below). */
            if (tc_name_len > 0 && tc_name_str[0] >= 'a' && tc_name_str[0] <= 'z') {
                /* Phase HKT: Kind variable (e.g. ^f).  Record the bare name so
                 * the return-type parser can resolve (f a) as TY_APP.  No
                 * runtime parameter is created. */
                if (n_kind_vars < 8) {
                    kind_var_names[n_kind_vars++] = symtab_intern(e->st,
                        strslice(tc_name_str, tc_name_len));
                }
                continue;
            }

            /* Create symbol for typeclass name */
            char tmp_name[64];
            snprintf(tmp_name, sizeof(tmp_name), "%.*s", tc_name_len, tc_name_str);
            const Symbol *tc_sym = symtab_intern(e->st, strslice(tmp_name, tc_name_len));
            TypeClass *tc = typeclass_env_lookup_typeclass(&e->typeclass_env, tc_sym);
            if (!tc) {
                diag_emit(DIAG_ERROR, p->span,
                          "defn: typeclass '%.*s' in constraint is not defined",
                          tc_name_len, tc_name_str);
                return NULL;
            }
            if (n_pending < 8) {
                pending_constraints[n_pending++] = tc;
            } else {
                diag_emit(DIAG_ERROR, p->span,
                          "defn: too many constraints (max 8)");
                return NULL;
            }
            continue;
        }
        
        /* Phase 15: Handle type variable declarations */
        /* After constraints, the next symbol is a type variable name */
        if (n_pending > 0 && p->tag == F_SYM) {
            /* This is a type variable that the pending constraints apply to */
            /* For v1, we ignore the type variable name and just record constraints */
            
            /* Register all pending constraints for this type variable */
            /* Allocate space for new constraints */
            uint8_t new_count = n_constraints + n_pending;
            TypeConstraint *new_list = (TypeConstraint *)arena_alloc(e->arena,
                new_count * sizeof(TypeConstraint));
            if (constraint_list) {
                memcpy(new_list, constraint_list, n_constraints * sizeof(TypeConstraint));
            }
            constraint_list = new_list;
            
            for (uint8_t c = 0; c < n_pending; c++) {
                /* For v1, we just record the constraint - type_arg will be resolved later */
                /* We use TYPE_UNKNOWN as a placeholder for the type variable */
                constraint_list[n_constraints + c].typeclass = pending_constraints[c];
                constraint_list[n_constraints + c].type_arg = TYPE_UNKNOWN;
            }
            n_constraints = new_count;
            n_pending = 0;
            continue;
        }
        
        /* Phase HRT1: Handle complex type annotation (list form or F_TYPE_ANN) for previous param.
         * Syntax: [param-name (forall [a] (-> a a))] — list form follows a symbol.
         * Also: [param-name : (-> a b)] — F_TYPE_ANN wrapping any type form. */
        if (p->tag == F_LIST || p->tag == F_VEC || p->tag == F_TYPE_ANN) {
            if (n_params == 0) {
                diag_emit(DIAG_ERROR, p->span,
                          "defn: type annotation without preceding parameter");
                return NULL;
            }
            /* For F_TYPE_ANN, unwrap to the inner type form first */
            const Form *type_form = (p->tag == F_TYPE_ANN) ? p->as.list.items[0] : p;
            /* Parse as a type expression — supports (forall [a] (-> a a)), (-> a b), etc. */
            Type *ann = type_expr_from_form(e, type_form, NULL, NULL, NULL, 0);
            if (!ann) return NULL;
            if (ann->kind == TY_FORALL || ann->kind == TY_EXISTS) {
                /* Rank-2 polymorphic parameter: represented as tur_poly_fn_t at C level */
                param_kinds[n_params - 1] = TY_PTR_VOID;
                params[n_params - 1]->type = TYPE_PTR_VOID;
                params[n_params - 1]->is_poly_fn = true;
                params[n_params - 1]->poly_type = ann;
                param_poly_types[n_params - 1] = ann;
            } else if (ann->kind == TY_FN) {
                /* Plain function type annotation */
                param_kinds[n_params - 1] = TY_FN;
                params[n_params - 1]->type = *ann;
                /* LT2: For function-typed parameters, store full type in param_poly_types
                 * so it propagates into arg_full_types for linearity subtyping checks
                 * at call sites (-Xlinear). */
                if (g_linear_enabled) {
                    param_poly_types[n_params - 1] = ann;
                }
            } else {
                /* Other type annotation — use the kind */
                param_kinds[n_params - 1] = ann->kind;
                params[n_params - 1]->type = *ann;
                /* IT1: For union-typed parameters, store full type in param_poly_types
                 * so it propagates into arg_full_types for subtyping checks at call sites. */
                if (g_union_types_enabled && ann->kind == TY_UNION) {
                    param_poly_types[n_params - 1] = ann;
                }
                /* IT2: For intersection-typed parameters, same propagation. */
                if (g_intersection_types_enabled && ann->kind == TY_INTERSECTION) {
                    param_poly_types[n_params - 1] = ann;
                }
            }
            /* LT3: Propagate linearity from the type annotation (e.g., [p : (lref int)]) */
            if (g_linear_enabled && params[n_params - 1]->type.copy_kind == CK_LINEAR) {
                params[n_params - 1]->is_linear = true;
            }
            /* ST2: Under -Xsubstructural, ref<T> params without an explicit discipline
             * annotation are inferred as SK_LINEAR. */
            if (g_substructural_enabled
                    && !params[n_params - 1]->is_linear
                    && !params[n_params - 1]->is_affine
                    && !params[n_params - 1]->is_relevant
                    && params[n_params - 1]->type.kind == TY_REF) {
                params[n_params - 1]->is_linear = true;
                params[n_params - 1]->type.substruct = SK_LINEAR;
            }
            /* UT0: Propagate uniqueness from the type annotation */
            if (g_unique_enabled && next_param_unique) {
                params[n_params - 1]->is_unique = true;
                params[n_params - 1]->type.copy_kind = CK_UNIQUE;
                next_param_unique = false;
            }
            /* UT2: Propagate mutability from the ^mut annotation */
            if (next_param_mut) {
                params[n_params - 1]->is_mut = true;
                next_param_mut = false;
            }
            continue;
        }

        if (p->tag != F_SYM && p->tag != F_KEYWORD) {
            diag_emit(DIAG_ERROR, p->span,
                      "defn: parameter must be a symbol or type annotation");
            /* params is arena-allocated, no need to free */
            return NULL;
        }

        /* Handle type annotations: if this is a keyword like :int, it's a type for the previous param */
        if (p->tag == F_KEYWORD) {
            /* This is a type annotation for the previous parameter */
            if (n_params == 0) {
                diag_emit(DIAG_ERROR, p->span,
                          "defn: type annotation without preceding parameter");
                return NULL;
            }
            /* Update the type of the last parameter */
            const Symbol *kw = p->as.sym;
            if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                param_kinds[n_params - 1] = TY_INT;
                params[n_params - 1]->type = TYPE_INT;
            } else if (kw->len == 5 && memcmp(kw->name, "float", 5) == 0) {
                param_kinds[n_params - 1] = TY_FLOAT;
                params[n_params - 1]->type = TYPE_FLOAT;
            } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                param_kinds[n_params - 1] = TY_BOOL;
                params[n_params - 1]->type = TYPE_BOOL;
            } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                param_kinds[n_params - 1] = TY_CSTR;
                params[n_params - 1]->type = TYPE_CSTR;
            } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) || 
                       (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                param_kinds[n_params - 1] = TY_NIL;
                params[n_params - 1]->type = TYPE_NIL;
            } else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) {
                param_kinds[n_params - 1] = TY_PTR_VOID;
                params[n_params - 1]->type = TYPE_PTR_VOID;
            } else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) {
                param_kinds[n_params - 1] = TY_PTR_VOID;
                params[n_params - 1]->type = TYPE_PTR_VOID;
            } else if (kw->len == 1 && memcmp(kw->name, "!", 1) == 0) {
                param_kinds[n_params - 1] = TY_NEVER;
                params[n_params - 1]->type = TYPE_NEVER;
            } else if (kw->len == 3 && memcmp(kw->name, "ref", 3) == 0) {
                /* Phase 5: :ref keyword — owning heap pointer (inner type unknown, use int) */
                param_kinds[n_params - 1] = TY_REF;
                params[n_params - 1]->type = type_ref(TY_INT);
                /* ST2: Under -Xsubstructural, :ref params without an explicit discipline
                 * annotation are inferred as SK_LINEAR. */
                if (g_substructural_enabled && !params[n_params - 1]->is_linear
                        && !params[n_params - 1]->is_affine
                        && !params[n_params - 1]->is_relevant) {
                    params[n_params - 1]->is_linear = true;
                    params[n_params - 1]->type.substruct = SK_LINEAR;
                }
            } else if (kw->len == 4 && memcmp(kw->name, "lref", 4) == 0) {
                /* LT3: :lref keyword — linear owning pointer (inner type unknown, use int) */
                param_kinds[n_params - 1] = TY_LREF;
                params[n_params - 1]->type = type_lref(TY_INT);
                /* Mark as linear: lref<T> is always exactly-once */
                if (g_linear_enabled) params[n_params - 1]->is_linear = true;
            } else if (kw->len == 3 && memcmp(kw->name, "set", 3) == 0) {
                /* Phase X3: set type annotation */
                Type set_type = { .kind = TY_SET, .copy_kind = CK_MOVE };
                param_kinds[n_params - 1] = TY_SET;
                params[n_params - 1]->type = set_type;
            } else {
                /* Phase G3: Try constraint env first (type variable resolution) */
                TypeKind ck = gadt_skolem_lookup(&param_constraint_env, kw->name);
                if (ck != TY_UNKNOWN) {
                    /* Resolved via equality constraint */
                    param_kinds[n_params - 1] = ck;
                    params[n_params - 1]->type = type_from_kind(ck);
                    params[n_params - 1]->type.copy_kind = typekind_default_copy_kind(ck);
                } else {
                    /* Try to look up as ADT name */
                    AdtDef *param_adt = NULL;
                    for (uint32_t ai = 0; ai < e->n_adt_defs; ai++) {
                        if (strcmp(e->adt_defs[ai]->name, kw->name) == 0) {
                            param_adt = e->adt_defs[ai];
                            break;
                        }
                    }
                    if (param_adt) {
                        param_kinds[n_params - 1] = TY_ADT;
                        params[n_params - 1]->type = type_adt(param_adt);
                    } else {
                        /* Phase HRT/G2: Unknown keyword -- treat as an implicit type variable.
                         * A parameter annotation like :a where 'a' is not a known type or ADT
                         * is an implicit type variable. Mark the binding TY_TYVAR so that inside
                         * a GADT match arm, the per-arm skolem env can resolve it to a concrete type. */
                        param_kinds[n_params - 1] = TY_TYVAR;
                        params[n_params - 1]->type = type_tyvar_named(kw->name);
                    }
                }
            }
            continue;
        }

        if (n_params >= MAX_FN_ARITY) {
            diag_emit(DIAG_ERROR, p->span,
                      "defn: too many parameters (max %d)", MAX_FN_ARITY);
            /* params is arena-allocated, no need to free */
            return NULL;
        }
        /* For phase 2, default to int */
        param_kinds[n_params] = TY_INT;
        Binding *b = binding_new(e, p->as.sym, TYPE_INT, false, false, p->span);
        /* LT0: If the previous ^linear annotation applied to this parameter, mark it linear */
        if (next_param_linear) {
            b->is_linear = true;
            b->type.copy_kind = CK_LINEAR;
            next_param_linear = false;
        }
        /* UT0: If the previous ^unique annotation applied to this parameter, mark it unique */
        if (next_param_unique) {
            b->is_unique = true;
            b->type.copy_kind = CK_UNIQUE;
            next_param_unique = false;
        }
        /* UT2: If the previous ^mut annotation applied to this parameter, mark it mutable */
        if (next_param_mut) {
            b->is_mut = true;
            next_param_mut = false;
        }
        /* ST0: If the previous ^affine annotation applied to this parameter, mark it affine */
        if (next_param_affine) {
            b->is_affine = true;
            b->type.substruct = SK_AFFINE;
            next_param_affine = false;
        }
        /* ST0: If the previous ^relevant annotation applied to this parameter, mark it relevant */
        if (next_param_relevant) {
            b->is_relevant = true;
            b->type.substruct = SK_RELEVANT;
            next_param_relevant = false;
        }
        if (n_params == 0) {
            params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
        }
        params[n_params++] = b;
    }
    
    /* Phase 13: Lifetime annotations parsing deferred - restore original simple parsing */

    /* Parse return type annotation and body */
    /* body_start is the index of the first element after params (could be return type or body) */
    TypeKind return_kind = TY_NIL;
    AdtDef *return_adt_def = NULL; /* Phase G3: set when return type is an ADT name */
    StructDef *return_struct_def = NULL; /* LT4: set when return type is a struct name */
    uint32_t body_start = name_idx + 2;  /* name_idx + 1 = params, +1 = after params */

    /* Phase 19: Parse optional effect-row annotation #{Read Write} or #{e} before return type.
     * Uppercase names are concrete effects; lowercase are row variables.
     * The row is stored as ERK_UNRESOLVED and resolved after PASS_EFFECT_LOWER. */
    EffectRow *declared_effect_row_defn = NULL;
    if (call->as.list.len >= body_start + 1) {
        Form *maybe_row = call->as.list.items[body_start];
        if (maybe_row->tag == F_MAP) {
            uint8_t n_sym = (uint8_t)maybe_row->as.list.len;
            const Symbol **syms = (const Symbol **)arena_alloc(e->arena,
                                    (n_sym ? n_sym : 1) * sizeof(Symbol *));
            uint8_t n_valid = 0;
            for (uint32_t j = 0; j < maybe_row->as.list.len; j++) {
                Form *item = maybe_row->as.list.items[j];
                if (item->tag == F_SYM) {
                    syms[n_valid++] = item->as.sym;
                }
            }
            declared_effect_row_defn = effect_row_unresolved(e->arena, syms, n_valid);
            body_start++;  /* skip past the effect row map */
        }
    }
    bool fn_declared_unsafe =
        effect_row_contains_symbol(declared_effect_row_defn, e->sym_effect_unsafe);

    /* Phase HKT: Create the inner scope early so that kind-variable bindings
     * (^f → TY_TYVAR/KIND_ARROW) are visible when the return-type annotation
     * is parsed below.  Regular parameter bindings are added to this same
     * scope after the annotation is parsed (see "Push params" below). */
    Scope inner;
    scope_init(&inner, e->scope);
    e->scope = &inner;
    for (uint8_t kvi = 0; kvi < n_kind_vars; kvi++) {
        Type kv_type = type_tyvar_named(kind_var_names[kvi]->name);
        kv_type.hkt_kind = KIND_ARROW;
        Binding *kvb = binding_new(e, kind_var_names[kvi], kv_type,
                                   false, true, call->span);
        scope_add(&inner, kvb);
    }

    /* Check for : return-type annotation */
    if (call->as.list.len >= (body_start + 1)) {
        Form *ret_f = call->as.list.items[body_start];
        if (ret_f->tag == F_KEYWORD) {
            /* : int, : bool, etc. */
            const Symbol *kw = ret_f->as.sym;
            if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                return_kind = TY_INT;
            } else if (kw->len == 5 && memcmp(kw->name, "float", 5) == 0) {
                return_kind = TY_FLOAT;
            } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                return_kind = TY_BOOL;
            } else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) {
                return_kind = TY_NIL;
            } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                return_kind = TY_CSTR;
            } else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) {
                return_kind = TY_PTR_VOID;
            } else if (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0) {
                return_kind = TY_NIL;
            } else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) {
                return_kind = TY_PTR_VOID;
            } else if (kw->len == 2 && memcmp(kw->name, "rc", 2) == 0) {
                return_kind = TY_RC;
            } else if (kw->len == 4 && memcmp(kw->name, "weak", 4) == 0) {
                return_kind = TY_WEAK;
            } else if (kw->len == 4 && memcmp(kw->name, "lref", 4) == 0) {
                /* LT3: :lref return type keyword — lref<T> (linear owning pointer) */
                return_kind = TY_LREF;
            } else if (kw->len == 1 && memcmp(kw->name, "!", 1) == 0) {
                return_kind = TY_NEVER;
            } else if (kw->len == 3 && memcmp(kw->name, "set", 3) == 0) {
                /* Phase X3: set return type */
                return_kind = TY_SET;
            } else {
                /* Phase G3: Try constraint env (type variable resolution) */
                TypeKind ck = gadt_skolem_lookup(&param_constraint_env, kw->name);
                if (ck != TY_UNKNOWN) {
                    return_kind = ck;
                } else {
                    /* Try to look up as ADT name */
                    for (uint32_t ai = 0; ai < e->n_adt_defs; ai++) {
                        if (strcmp(e->adt_defs[ai]->name, kw->name) == 0) {
                            return_adt_def = e->adt_defs[ai];
                            return_kind = TY_ADT;
                            break;
                        }
                    }
                    /* LT4: Try to look up as struct name */
                    if (!return_adt_def) {
                        for (uint32_t si = 0; si < e->n_struct_defs; si++) {
                            if (strcmp(e->struct_defs[si]->name, kw->name) == 0) {
                                return_struct_def = e->struct_defs[si];
                                return_kind = TY_STRUCT;
                                break;
                            }
                        }
                    }
                    if (!return_adt_def && !return_struct_def) {
                        if (g_gadt_enabled) {
                            /* Phase HRT/G2: Unknown return type keyword -- named type variable.
                             * e.g., :a means the function returns the type variable a.
                             * For codegen, we fall through and let the body type determine the
                             * concrete return kind (see the TY_TYVAR inference below). */
                            return_kind = TY_TYVAR;
                        } else {
                            diag_emit(DIAG_ERROR, ret_f->span,
                                      "defn: unsupported return type keyword :%s",
                                      kw->name);
                            return NULL;
                        }
                    }
                }
            }
            body_start++;
        } else if (ret_f->tag == F_TYPE_ANN) {
            /* Compound return type via `: type-expr` syntax: `: (-> a b)`, `: (vec int)`, etc. */
            if (ret_f->as.list.len > 0) {
                Type *ann = type_expr_from_form(e, ret_f->as.list.items[0], NULL, NULL, NULL, 0);
                if (ann) return_kind = ann->kind;
            }
            body_start++;
        }
    }

    /* Elaborate body */
    if (call->as.list.len < body_start + 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "defn: missing body");
        e->scope = inner.parent;
        scope_free(&inner);
        return NULL;
    }

    /* Phase HRT5: Early-update a forward-declared binding's arity and poly param
     * types before elaborating the body.  Without this, recursive calls inside
     * the body see the stale arity-1 / no-arg_full_types from pass-1, which
     * causes spurious arity-mismatch errors for functions with poly fn params. */
    if (existing && existing->type.kind == TY_FN && existing->is_global) {
        existing->type.as.fn.arity = n_params;
        for (uint8_t _ei = 0; _ei < n_params; _ei++) {
            existing->type.as.fn.arg_kinds[_ei] = param_kinds[_ei];
        }
        bool _any_poly = false;
        for (uint8_t _ei = 0; _ei < n_params; _ei++) {
            if (param_poly_types[_ei]) { _any_poly = true; break; }
        }
        if (_any_poly) {
            Type **_aFT = (Type **)arena_alloc(e->arena, n_params * sizeof(Type *));
            for (uint8_t _ei = 0; _ei < n_params; _ei++) _aFT[_ei] = param_poly_types[_ei];
            existing->type.as.fn.arg_full_types = _aFT;
        }
    }

    /* Push params into the inner scope (created earlier for kind-var bindings). */
    for (uint8_t i = 0; i < n_params; i++) {
        scope_add(&inner, params[i]);
    }

    Expr *body = e_nil(e, call->span);
    uint32_t n_body = call->as.list.len - body_start;

    e->fn_body_depth++;
    /* Phase R6: Track current function name for linting */
    e->current_fn_name = name_f->as.sym;
    if (fn_declared_unsafe) e->unsafe_depth++;
    if (n_body == 1) {
        body = elab_form(e, call->as.list.items[body_start]);
        if (!body) {
            if (fn_declared_unsafe) e->unsafe_depth--;
            e->fn_body_depth--;
            /* Phase R6: Reset current function name */
            e->current_fn_name = NULL;
            e->scope = inner.parent;
            scope_free(&inner);
            return NULL;
        }
    } else {
        Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
        for (uint32_t i = 0; i < n_body; i++) {
            items[i] = elab_form(e, call->as.list.items[body_start + i]);
            if (!items[i]) {
                if (fn_declared_unsafe) e->unsafe_depth--;
                e->fn_body_depth--;
                /* Phase R6: Reset current function name */
                e->current_fn_name = NULL;
                e->scope = inner.parent;
                scope_free(&inner);
                return NULL;
            }
        }
        /* Phase R6: Warn on discarded result values in function bodies */
        if (g_warn_unused_result) {
            for (uint32_t i = 0; i < n_body - 1; i++) {
                if (items[i]->type.kind == TY_PTR_VOID) {
                    diag_emit(DIAG_WARNING, items[i]->span,
                              "discarded result value of type ptr<void>; use ignore! to suppress this warning");
                }
            }
        }
        body = expr_new(e->arena, EX_DO, items[n_body - 1]->type, call->span);
        body->as.do_.items = items;
        body->as.do_.n = n_body;
    }
    if (fn_declared_unsafe) e->unsafe_depth--;
    e->fn_body_depth--;
    /* Phase R6: Reset current function name */
    e->current_fn_name = NULL;

    /* LT1: At function scope exit, verify all linear params were consumed */
    bool lt1_param_fail = false;
    if (g_linear_enabled && body) {
        for (uint8_t _li = 0; _li < n_params; _li++) {
            if (params[_li]->is_linear && !params[_li]->is_linear_consumed && !params[_li]->is_moved) {
                diag_emit_with_code(DIAG_ERROR, params[_li]->span,
                                    TUR_E0100_LINEAR_DROPPED,
                                    "linear parameter '%s' dropped without being consumed",
                                    params[_li]->name->name);
                lt1_param_fail = true;
            }
        }
    }

    /* ST1: At function scope exit, verify all relevant params were used at least once */
    bool st1_param_fail = false;
    if (g_substructural_enabled && body) {
        for (uint8_t _li = 0; _li < n_params; _li++) {
            if (params[_li]->is_relevant && params[_li]->usage_state == USAGE_UNUSED
                    && !params[_li]->is_moved) {
                diag_emit_with_code(DIAG_ERROR, params[_li]->span,
                                    TUR_E0151_RELEVANT_DROPPED,
                                    "relevant parameter '%s' dropped without being used",
                                    params[_li]->name->name);
                st1_param_fail = true;
            }
        }
    }

    /* Pop scope */
    e->scope = inner.parent;
    scope_free(&inner);
    if (lt1_param_fail || st1_param_fail) return NULL;

    /* Infer return type from body if not specified or polymorphic (TY_TYVAR).
     * For TY_TYVAR (named type variable like :a), use the body's concrete type
     * for codegen -- the polymorphic annotation is preserved in the declaration
     * but the C function signature uses the concrete type. */
    if ((return_kind == TY_NIL || return_kind == TY_TYVAR) && body->type.kind != TY_NIL
            && body->type.kind != TY_TYVAR) {
        return_kind = body->type.kind;
    }

    /* Create function type */
    TypeKind arg_kinds[MAX_FN_ARITY];
    for (uint8_t i = 0; i < n_params; i++) {
        arg_kinds[i] = param_kinds[i];
    }
    Type fn_type = type_fn(arg_kinds, n_params, return_kind);

    /* Phase G3: attach full ADT return type if declared (for proper def propagation) */
    if (return_adt_def) {
        Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
        *rft = type_adt(return_adt_def);
        fn_type.as.fn.result_full_type = rft;
    }
    /* LT4: attach full struct return type if declared.
     * Stored so that emit.c can retrieve the StructDef (and hence the struct name)
     * without going through type_from_kind, which doesn't carry the def pointer. */
    if (return_struct_def) {
        Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
        memset(rft, 0, sizeof(Type));   /* fully zero before writing fields */
        rft->kind = TY_STRUCT;
        rft->copy_kind = return_struct_def->is_linear ? CK_LINEAR
                       : (return_struct_def->is_copy ? CK_COPY : CK_MOVE);
        rft->hkt_kind = KIND_STAR;
        rft->as.struct_.def = return_struct_def;
        fn_type.as.fn.result_full_type = rft;
    }

    /* Phase HRT1: attach full poly types for rank-2 params */
    {
        bool any_poly = false;
        for (uint8_t i = 0; i < n_params; i++) {
            if (param_poly_types[i]) { any_poly = true; break; }
        }
        if (any_poly) {
            Type **aFT = (Type **)arena_alloc(e->arena, n_params * sizeof(Type *));
            for (uint8_t i = 0; i < n_params; i++) aFT[i] = param_poly_types[i];
            fn_type.as.fn.arg_full_types = aFT;
        }
    }

    /* LT2: Store arg_linear flags from param bindings into fn_type */
    {
        bool any_linear = false;
        for (uint8_t i = 0; i < n_params; i++) {
            if (params[i]->is_linear) { any_linear = true; break; }
        }
        if (any_linear) {
            for (uint8_t i = 0; i < n_params; i++) {
                fn_type.as.fn.arg_linear[i] = params[i]->is_linear;
            }
        }
    }

    /* UT0: Store arg_unique flags from param bindings into fn_type */
    {
        bool any_unique = false;
        for (uint8_t i = 0; i < n_params; i++) {
            if (params[i]->is_unique) { any_unique = true; break; }
        }
        if (any_unique) {
            for (uint8_t i = 0; i < n_params; i++) {
                fn_type.as.fn.arg_unique[i] = params[i]->is_unique;
            }
        }
    }
    /* UT2: Store arg_unique_mut flags (^unique ^mut) from param bindings into fn_type */
    {
        bool any_unique_mut = false;
        for (uint8_t i = 0; i < n_params; i++) {
            if (params[i]->is_unique && params[i]->is_mut) { any_unique_mut = true; break; }
        }
        if (any_unique_mut) {
            for (uint8_t i = 0; i < n_params; i++) {
                fn_type.as.fn.arg_unique_mut[i] = params[i]->is_unique && params[i]->is_mut;
            }
        }
    }
    /* ST0: Store arg_affine flags from param bindings into fn_type */
    {
        bool any_affine = false;
        for (uint8_t i = 0; i < n_params; i++) {
            if (params[i]->is_affine) { any_affine = true; break; }
        }
        if (any_affine) {
            for (uint8_t i = 0; i < n_params; i++) {
                fn_type.as.fn.arg_affine[i] = params[i]->is_affine;
            }
        }
    }
    /* ST0: Store arg_relevant flags from param bindings into fn_type */
    {
        bool any_relevant = false;
        for (uint8_t i = 0; i < n_params; i++) {
            if (params[i]->is_relevant) { any_relevant = true; break; }
        }
        if (any_relevant) {
            for (uint8_t i = 0; i < n_params; i++) {
                fn_type.as.fn.arg_relevant[i] = params[i]->is_relevant;
            }
        }
    }

    /* Create/update binding for the function.
     * Reuse pass-1 forward bindings in place so subsequent lookups observe
     * updated arity/types from the real definition. */
    Binding *b = NULL;
    if (existing && existing->type.kind == TY_FN && existing->is_global) {
        b = existing;
        b->type = fn_type;
        b->span = name_f->span;
        /* Phase M7: Forward declarations from pass 1 don't know module context.
         * Override defining_module_name with the actual module the defn is in. */
        b->defining_module_name = e->current_module_name;
    } else {
        b = binding_new(e, name_f->as.sym, fn_type, false, true, name_f->span);
        scope_add(&e->global, b);
    }
    /* Phase R5: Store #[no-unwind] attribute on the binding */
    b->no_unwind = no_unwind;
    /* Phase M6: Store ^:export-as C name on the binding */
    b->c_export_name = c_export_name;

    /* Build FnDef */
    FnDef *fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    fd->binding = b;
    fd->params = params;
    fd->n_params = n_params;
    fd->body = body;
    fd->is_variadic = false;
    fd->closure = NULL;
    fd->inferred_effect_row = NULL;  /* must be NULL; effect_check_pass reads this */
    /* Phase 19: Store declared effect row (ERK_UNRESOLVED until PASS_EFFECT_ROW_INFER). */
    if (declared_effect_row_defn) {
        b->type.as.fn.effect_row = declared_effect_row_defn;
    }
    /* Store param types for codegen.
     * For TY_FN params (annotated with :(fn [...] #{...} :type)), use the full
     * binding type which preserves the result kind and effect row.
     * For TY_STRUCT params, use the binding type which preserves the StructDef
     * pointer (needed so emit.c can emit the struct name in the C signature).
     * For all other kinds, fall back to type_from_kind which is sufficient. */
    fd->param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
    for (uint8_t i = 0; i < n_params; i++) {
        if (param_kinds[i] == TY_FN && params[i]->type.kind == TY_FN) {
            fd->param_types[i] = params[i]->type;
        } else if (param_kinds[i] == TY_STRUCT && params[i]->type.kind == TY_STRUCT) {
            /* LT4: preserve StructDef so emit.c emits the struct name, not int64_t. */
            fd->param_types[i] = params[i]->type;
        } else {
            fd->param_types[i] = type_from_kind(param_kinds[i]);
        }
    }
    /* Phase 15: Store collected constraints */
    fd->constraints.constraints = constraint_list;
    fd->constraints.n_constraints = n_constraints;
    fd->constraints.cap_constraints = n_constraints;

    Expr *out = expr_new(e->arena, EX_FN_DEF, fn_type, call->span);
    out->as.fn_def_.fn = fd;
    return out;
}

/* Phase 2: fn — (fn [param1 param2 ...] body...) — no capture for phase 2
 * Lifts to a static function. For now, we require a return type annotation.
 * Example: (fn [x y] :int (+ x y)) */
static Expr *elab_fn(Elab *e, const Form *call) {
    /* Minimum: (fn [params...] body...) */
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "fn requires (fn [params...] body...)");
        return NULL;
    }

    /* Parse param vector */
    Form *params_f = call->as.list.items[1];
    if (params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "fn: parameter list must be a vector [name1 name2 ...]");
        return NULL;
    }

    /* Parse params */
    Binding **params = NULL;
    uint8_t n_params = 0;
    TypeKind param_kinds[MAX_FN_ARITY];

    for (uint32_t i = 0; i < params_f->as.list.len; i++) {
        Form *p = params_f->as.list.items[i];
        if (p->tag != F_SYM) {
            diag_emit(DIAG_ERROR, p->span,
                      "fn: parameter name must be a symbol");
            /* params is arena-allocated, no need to free */
            return NULL;
        }
        if (n_params >= MAX_FN_ARITY) {
            diag_emit(DIAG_ERROR, p->span,
                      "fn: too many parameters (max %d)", MAX_FN_ARITY);
            /* params is arena-allocated, no need to free */
            return NULL;
        }
        /* For phase 2, all params are int by default */
        param_kinds[n_params] = TY_INT;
        Binding *b = binding_new(e, p->as.sym, TYPE_INT, false, false, p->span);
        if (n_params == 0) {
            params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
        }
        params[n_params++] = b;
    }

    /* Parse return type annotation and body */
    TypeKind return_kind = TY_NIL;
    uint32_t body_start = 2;

    /* Phase 19: Parse optional effect-row annotation #{Read Write} or #{e} before return type. */
    EffectRow *declared_effect_row_fn = NULL;
    if (call->as.list.len >= 3) {
        Form *maybe_row = call->as.list.items[2];
        if (maybe_row->tag == F_MAP) {
            uint8_t n_sym = (uint8_t)maybe_row->as.list.len;
            const Symbol **syms = (const Symbol **)arena_alloc(e->arena,
                                    (n_sym ? n_sym : 1) * sizeof(Symbol *));
            uint8_t n_valid = 0;
            for (uint32_t j = 0; j < maybe_row->as.list.len; j++) {
                Form *item = maybe_row->as.list.items[j];
                if (item->tag == F_SYM) {
                    syms[n_valid++] = item->as.sym;
                }
            }
            declared_effect_row_fn = effect_row_unresolved(e->arena, syms, n_valid);
            body_start = 3;
        }
    }
    bool fn_declared_unsafe =
        effect_row_contains_symbol(declared_effect_row_fn, e->sym_effect_unsafe);

    /* Check for : return-type annotation */
    if (call->as.list.len >= (body_start + 1)) {
        Form *ret_f = call->as.list.items[body_start];
        if (ret_f->tag == F_KEYWORD) {
            /* : int, : bool, etc. */
            const Symbol *kw = ret_f->as.sym;
            if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                return_kind = TY_INT;
            } else if (kw->len == 5 && memcmp(kw->name, "float", 5) == 0) {
                return_kind = TY_FLOAT;
            } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                return_kind = TY_BOOL;
            } else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) {
                return_kind = TY_NIL;
            } else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) {
                return_kind = TY_PTR_VOID;
            } else if (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0) {
                return_kind = TY_NIL;
            } else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) {
                return_kind = TY_PTR_VOID;
            } else if (kw->len == 2 && memcmp(kw->name, "rc", 2) == 0) {
                return_kind = TY_RC;
            } else if (kw->len == 4 && memcmp(kw->name, "weak", 4) == 0) {
                return_kind = TY_WEAK;
            } else if (kw->len == 1 && memcmp(kw->name, "!", 1) == 0) {
                return_kind = TY_NEVER;
            } else {
                diag_emit(DIAG_ERROR, ret_f->span,
                          "fn: unsupported return type keyword :%s",
                          kw->name);
                /* params is arena-allocated, no need to free */
                return NULL;
            }
            body_start++;
        } else if (ret_f->tag == F_TYPE_ANN) {
            /* Compound return type via `: type-expr` syntax: `: (-> a b)`, `: (vec int)`, etc. */
            if (ret_f->as.list.len > 0) {
                Type *ann = type_expr_from_form(e, ret_f->as.list.items[0], NULL, NULL, NULL, 0);
                if (ann) return_kind = ann->kind;
            }
            body_start++;
        }
    }

    /* Elaborate body */
    if (call->as.list.len < body_start + 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "fn: missing body");
        /* params is arena-allocated, no need to free */
        return NULL;
    }

    /* Push a new scope for the function body with params bound */
    Scope inner;
    scope_init(&inner, e->scope);
    e->scope = &inner;
    for (uint8_t i = 0; i < n_params; i++) {
        scope_add(&inner, params[i]);
    }

    Expr *body = e_nil(e, call->span);
    uint32_t n_body = call->as.list.len - body_start;
    e->fn_body_depth++;
    if (fn_declared_unsafe) e->unsafe_depth++;
    if (n_body == 1) {
        body = elab_form(e, call->as.list.items[body_start]);
        if (!body) {
            if (fn_declared_unsafe) e->unsafe_depth--;
            e->fn_body_depth--;
            e->scope = inner.parent;
            scope_free(&inner);
            return NULL;
        }
    } else {
        Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
        for (uint32_t i = 0; i < n_body; i++) {
            items[i] = elab_form(e, call->as.list.items[body_start + i]);
            if (!items[i]) {
                if (fn_declared_unsafe) e->unsafe_depth--;
                e->fn_body_depth--;
                e->scope = inner.parent;
                scope_free(&inner);
                return NULL;
            }
        }
        body = expr_new(e->arena, EX_DO, items[n_body - 1]->type, call->span);
        body->as.do_.items = items;
        body->as.do_.n = n_body;
    }
    if (fn_declared_unsafe) e->unsafe_depth--;
    e->fn_body_depth--;

    /* Phase 3: Capture analysis - collect free variables in the body */
    /* We need to do this before popping the scope */
    uint32_t n_captures = 0;
    Binding **captures = collect_free_vars(body, params, n_params, &n_captures);

    /* Pop scope */
    e->scope = inner.parent;
    scope_free(&inner);

    /* Infer return type from body if not specified */
    if (return_kind == TY_NIL && body->type.kind != TY_NIL) {
        return_kind = body->type.kind;
    }
    
    /* Create function type */
    TypeKind arg_kinds[MAX_FN_ARITY];
    for (uint8_t i = 0; i < n_params; i++) {
        arg_kinds[i] = TY_INT;  /* All int for phase 2 */
    }
    Type fn_type = type_fn(arg_kinds, n_params, return_kind);

    /* Check if we're at top level */
    bool at_top_level = (e->scope == &e->global);

    /* For anonymous fn, we lift it to a static function with a generated name.
     * We use the arena to allocate a unique name. */
    char fn_name_buf[32];
    snprintf(fn_name_buf, sizeof(fn_name_buf), "__fn_%u", e->next_id++);
    const Symbol *fn_name_sym = symtab_intern(e->st, 
        strslice(fn_name_buf, (uint32_t)strlen(fn_name_buf)));
    
    Binding *b = binding_new(e, fn_name_sym, fn_type, false, true, call->span);
    scope_add(&e->global, b);

    /* Build FnDef */
    FnDef *fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    fd->binding = b;
    fd->params = params;
    fd->n_params = n_params;
    fd->body = body;
    fd->is_variadic = false;
    fd->closure = NULL;
    fd->inferred_effect_row = NULL;  /* must be NULL; effect_check_pass reads this */
    /* Phase 19: Store declared effect row (ERK_UNRESOLVED until PASS_EFFECT_ROW_INFER). */
    if (declared_effect_row_fn) {
        b->type.as.fn.effect_row = declared_effect_row_fn;
    }
    /* Store param types for codegen */
    fd->param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
    for (uint8_t i = 0; i < n_params; i++) {
        fd->param_types[i] = type_from_kind(param_kinds[i]);
    }
    /* Phase 15: Initialize constraints */
    constraint_set_init(&fd->constraints);

    /* Create the FN_DEF expression that will be emitted at file scope */
    Expr *fn_def_expr = expr_new(e->arena, EX_FN_DEF, fn_type, call->span);
    fn_def_expr->as.fn_def_.fn = fd;

    if (n_captures == 0) {
        /* Register FN_DEF for file-scope emission regardless of scope level.
         * Without this, an anonymous fn used as an argument at top level would
         * return EX_VAR("__fn_N") but never emit the EX_FN_DEF that creates
         * the runtime closure. */
        elab_register_file_def(e, fn_def_expr);
        /* Return VAR reference to the function */
        Expr *var_expr = expr_new(e->arena, EX_VAR, fn_type, call->span);
        var_expr->as.var.binding = b;
        free(captures);
        return var_expr;
    } else {
        /* Phase 3: Closure with captures */
        /* Generate env struct name */
        char env_name_buf[32];
        snprintf(env_name_buf, sizeof(env_name_buf), "__env_%u", e->next_id++);
        const Symbol *env_name_sym = symtab_intern(e->st,
            strslice(env_name_buf, (uint32_t)strlen(env_name_buf)));
        
        /* Modify the FnDef to include env parameter as first parameter */
        uint8_t new_n_params = n_params + 1;
        if (new_n_params > MAX_FN_ARITY) {
            diag_emit(DIAG_ERROR, call->span,
                      "fn with captures: too many parameters including env (max %d)", MAX_FN_ARITY);
            free(captures);
            return NULL;
        }
        
        /* Create new params array with env as first parameter */
        Binding **new_params = (Binding **)arena_alloc(e->arena, new_n_params * sizeof(Binding *));
        Type *new_param_types = (Type *)arena_alloc(e->arena, new_n_params * sizeof(Type));
        
        /* First param is env (void*) */
        char env_param_name[32];
        snprintf(env_param_name, sizeof(env_param_name), "__env_p_%u", e->next_id++);
        const Symbol *env_param_sym = symtab_intern(e->st,
            strslice(env_param_name, (uint32_t)strlen(env_param_name)));
        Binding *env_param_binding = binding_new(e, env_param_sym, TYPE_PTR_VOID, false, false, call->span);
        new_params[0] = env_param_binding;
        new_param_types[0] = TYPE_PTR_VOID;
        
        /* Copy existing params */
        for (uint8_t i = 0; i < n_params; i++) {
            new_params[i + 1] = params[i];
            new_param_types[i + 1] = TYPE_INT;
        }
        
        /* Update FnDef with new params */
        fd->params = new_params;
        fd->n_params = new_n_params;
        fd->param_types = new_param_types;
        
        /* Update function type to include env parameter */
        TypeKind new_arg_kinds[MAX_FN_ARITY];
        new_arg_kinds[0] = TY_PTR_VOID;  /* env parameter */
        for (uint8_t i = 0; i < n_params; i++) {
            new_arg_kinds[i + 1] = TY_INT;
        }
        Type new_fn_type = type_fn(new_arg_kinds, new_n_params, return_kind);
        b->type = new_fn_type;
        fd->binding->type = new_fn_type;
        fn_def_expr->type = new_fn_type;
        
        /* Register the modified FN_DEF for file-scope emission */
        if (!at_top_level) {
            elab_register_file_def(e, fn_def_expr);
        }
        
        /* Create Closure struct */
        struct Closure *closure = (struct Closure *)arena_alloc(e->arena, sizeof(struct Closure));
        closure->fn = fd;
        closure->captures = captures;
        closure->n_captures = n_captures;
        closure->env_name = env_name_sym;
        
        /* Store closure reference in FnDef for codegen */
        fd->closure = closure;
        
        /* Create EX_CLOSURE expression */
        /* The closure's type is void* (pointer to closure struct) */
        Expr *closure_expr = expr_new(e->arena, EX_CLOSURE, TYPE_PTR_VOID, call->span);
        closure_expr->as.closure_.closure = closure;
        
        /* Don't free captures - it's now owned by the closure */
        return closure_expr;
    }
}

/* Phase 2: extern-c — (extern-c name [param1 param2 ...] : return-type)
 * Declares an external C function. For phase 2, we don't support capture.
 * Example: (extern-c printf [^cstr fmt] : int)
 * The ^ prefix on a param indicates it's a C type annotation (not yet implemented).
 * For now, all params are treated as int64_t or pointers.
 * 
 * Supported annotations:
 *   ^cstr - const char* (string)
 *   ^ptr  - void* (pointer)
 */
static Expr *elab_extern_c(Elab *e, const Form *call) {
    /* Minimum: (extern-c name [params...] : ret-type) */
    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "extern-c requires (extern-c name [params...] : ret-type)");
        return NULL;
    }

    /* Parse name */
    Form *name_f = call->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "extern-c name must be a symbol");
        return NULL;
    }
    if (scope_lookup(e->scope, name_f->as.sym)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "extern-c: '%s' is already defined", name_f->as.sym->name);
        return NULL;
    }

    /* Parse param vector */
    Form *params_f = call->as.list.items[2];
    if (params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "extern-c: parameter list must be a vector [name1 name2 ...]");
        return NULL;
    }

    /* Parse params - support type annotations: [name :type ...]
     * e.g. (extern-c getenv [key :cstr] :cstr) */
    Binding **params = NULL;
    uint8_t n_params = 0;
    TypeKind param_kinds[MAX_FN_ARITY];

    for (uint32_t i = 0; i < params_f->as.list.len; i++) {
        Form *p = params_f->as.list.items[i];
        /* Handle type annotation keyword or F_TYPE_ANN after the previous param */
        if (p->tag == F_KEYWORD || p->tag == F_TYPE_ANN) {
            if (n_params == 0) {
                diag_emit(DIAG_ERROR, p->span,
                          "extern-c: type annotation without preceding parameter");
                return NULL;
            }
            TypeKind pk;
            if (p->tag == F_TYPE_ANN) {
                Type *ann = (p->as.list.len > 0)
                    ? type_expr_from_form(e, p->as.list.items[0], NULL, NULL, NULL, 0)
                    : NULL;
                if (!ann) return NULL;
                pk = ann->kind;
            } else {
                const Symbol *kw = p->as.sym;
                pk = typekind_from_symbol(kw->name);
                if (pk == TY_UNKNOWN) {
                    /* Legacy fallback names */
                    if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) pk = TY_PTR_VOID;
                    else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) pk = TY_NIL;
                    else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) pk = TY_PTR_VOID;
                    else {
                        diag_emit(DIAG_ERROR, p->span,
                                  "extern-c: unsupported parameter type :%s", kw->name);
                        return NULL;
                    }
                }
            }
            param_kinds[n_params - 1] = pk;
            params[n_params - 1]->type = type_from_kind(pk);
            continue;
        }
        if (p->tag != F_SYM) {
            diag_emit(DIAG_ERROR, p->span,
                      "extern-c: parameter must be a symbol");
            return NULL;
        }
        if (n_params >= MAX_FN_ARITY) {
            diag_emit(DIAG_ERROR, p->span,
                      "extern-c: too many parameters (max %d)", MAX_FN_ARITY);
            return NULL;
        }

        /* Handle ^type prefix: ^int, ^ptr<void>, etc.
         * When a symbol starts with '^', the rest is a C type annotation and the
         * NEXT symbol in the vector is the parameter name. */
        if (p->as.sym->len > 1 && p->as.sym->name[0] == '^') {
            const char *tname = p->as.sym->name + 1;
            size_t tlen = p->as.sym->len - 1;
            TypeKind ck;
            if (tlen == 3 && memcmp(tname, "int", 3) == 0) ck = TY_INT;
            else if (tlen == 4 && memcmp(tname, "bool", 4) == 0) ck = TY_BOOL;
            else if (tlen == 4 && memcmp(tname, "cstr", 4) == 0) ck = TY_CSTR;
            else if (tlen == 3 && memcmp(tname, "ptr", 3) == 0) ck = TY_PTR_VOID;
            else if (tlen == 9 && memcmp(tname, "ptr<void>", 9) == 0) ck = TY_PTR_VOID;
            else if (tlen == 4 && memcmp(tname, "void", 4) == 0) ck = TY_NIL;
            else ck = TY_INT; /* unknown ^type: default to int */
            /* Peek at next element to get the param name */
            i++;
            if (i >= params_f->as.list.len) {
                /* ^type at end with no following name — create anonymous param */
                if (n_params == 0) {
                    params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
                }
                param_kinds[n_params] = ck;
                /* Use the ^type symbol itself as a placeholder name */
                Binding *b = binding_new(e, p->as.sym, type_from_kind(ck), false, false, p->span);
                params[n_params++] = b;
                break;
            }
            Form *name_f = params_f->as.list.items[i];
            if (name_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, name_f->span,
                          "extern-c: expected parameter name after ^type");
                return NULL;
            }
            if (n_params == 0) {
                params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
            }
            param_kinds[n_params] = ck;
            Binding *b = binding_new(e, name_f->as.sym, type_from_kind(ck), false, false, name_f->span);
            params[n_params++] = b;
            continue;
        }

        param_kinds[n_params] = TY_INT;
        Binding *b = binding_new(e, p->as.sym, TYPE_INT, false, false, p->span);
        if (n_params == 0) {
            params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
        }
        params[n_params++] = b;
    }

    /* Parse return type annotation; optionally skip #{Effect...} advisory row */
    uint32_t ret_idx = 3;
    if (ret_idx < call->as.list.len && call->as.list.items[ret_idx]->tag == F_MAP) {
        /* #{...} effect-row annotation: skip silently (advisory in v1) */
        ret_idx++;
    }
    if (ret_idx >= call->as.list.len) {
        diag_emit(DIAG_ERROR, call->span,
                  "extern-c requires (extern-c name [params...] : ret-type)");
        return NULL;
    }
    Form *ret_f = call->as.list.items[ret_idx];
    if (ret_f->tag != F_KEYWORD && ret_f->tag != F_TYPE_ANN) {
        diag_emit(DIAG_ERROR, ret_f->span,
                  "extern-c: return type must be a keyword (:int, :bool, :void, :cstr, :ptr)");
        return NULL;
    }

    TypeKind return_kind;
    if (ret_f->tag == F_TYPE_ANN) {
        Type *ann = (ret_f->as.list.len > 0)
            ? type_expr_from_form(e, ret_f->as.list.items[0], NULL, NULL, NULL, 0)
            : NULL;
        if (!ann) return NULL;
        return_kind = ann->kind;
    } else {
        return_kind = typekind_from_symbol(ret_f->as.sym->name);
        if (return_kind == TY_UNKNOWN) {
            /* Legacy fallback names */
            const Symbol *kw = ret_f->as.sym;
            if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) return_kind = TY_NIL;
            else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) return_kind = TY_PTR_VOID;
            else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) return_kind = TY_PTR_VOID;
            else {
                diag_emit(DIAG_ERROR, ret_f->span,
                          "extern-c: unsupported return type :%s", kw->name);
                return NULL;
            }
        }
    }

    /* Create function type */
    Type fn_type = type_fn(param_kinds, n_params, return_kind);

    /* Create a binding for the extern-c function so it can be looked up and called */
    Binding *b = binding_new(e, name_f->as.sym, fn_type, false, true, call->span);
    b->is_extern_c = true;  /* ER6: mark as extern-c for effect inference */
    scope_add(&e->global, b);

    /* Create ExternC declaration */
    ExternC *ec = (ExternC *)arena_alloc(e->arena, sizeof(ExternC));
    ec->c_name = name_f->as.sym;
    ec->binding = b;
    ec->return_type = type_from_kind(return_kind);
    ec->param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
    for (uint8_t i = 0; i < n_params; i++) {
        ec->param_types[i] = type_from_kind(param_kinds[i]);
    }
    ec->n_params = n_params;
    ec->is_variadic = false;

    Expr *out = expr_new(e->arena, EX_EXTERN_C, fn_type, call->span);
    out->as.extern_c_.ext = ec;
    
    /* params was allocated with arena_alloc, so no need to free */
    return out;
}

static Expr *elab_def(Elab *e, const Form *call) {
    /* Phase P3: Check for ^persistent annotation before name */
    /* Syntax: (def ^persistent name init) */
    uint32_t name_idx = 1;
    bool is_persistent = false;
    
    if (call->as.list.len > 3) {
        Form *first = call->as.list.items[name_idx];
        if (first->tag == F_SYM && first->as.sym == e->sym_caret_persistent) {
            is_persistent = true;
            name_idx++;
        }
    }
    
    if (name_idx + 2 != call->as.list.len) {
        diag_emit(DIAG_ERROR, call->span, "def takes (def [^persistent] name init)");
        return NULL;
    }
    
    Form *name_f = call->as.list.items[name_idx];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "def name must be a symbol");
        return NULL;
    }
    /* Top-level only — error if not in global scope. */
    if (e->scope != &e->global) {
        diag_emit(DIAG_ERROR, call->span, "def is only valid at the top level");
        return NULL;
    }
    if (scope_lookup(e->scope, name_f->as.sym)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "def: '%s' is already defined", name_f->as.sym->name);
        return NULL;
    }
    Expr *init = elab_form(e, call->as.list.items[name_idx + 1]);
    if (!init) return NULL;

    /* Phase 5: ref<T> is scope-local only — disallow at top-level def */
    if (init->type.kind == TY_REF) {
        diag_emit(DIAG_ERROR, call->span,
                  "def: ref<T> values must be scope-local; use let instead of def for '%s'",
                  name_f->as.sym->name);
        return NULL;
    }

    Binding *b = binding_new(e, name_f->as.sym, init->type,
                             /*is_mut=*/false, /*is_global=*/true, name_f->span);
    b->is_persistent = is_persistent;
    scope_add(&e->global, b);

    Expr *out = expr_new(e->arena, EX_DEF, TYPE_NIL, call->span);
    out->as.def_.binding = b;
    out->as.def_.init = init;
    out->as.def_.struct_def = NULL;
    return out;
}

/* Phase P3: HAMT lowering - forward declarations */
static Expr *elab_call_hamt_fn(Elab *e, Span span, const Symbol *fn_name, uint32_t n_args, Expr **args);
static Expr *elab_call_fn(Elab *e, const Form *call, Binding *fn_binding);

/* Phase P3: HAMT lowering - create a call to a HAMT function binding */
static Expr *elab_call_hamt_fn(Elab *e, Span span, const Symbol *fn_name, uint32_t n_args, Expr **args) {
    /* Look up the HAMT function binding */
    bool fn_qual_err = false;
    Binding *fn_binding = elab_lookup_sym(e, fn_name, span, &fn_qual_err);
    if (!fn_binding && fn_qual_err) return NULL;
    if (!fn_binding) {
        /* HAMT module not imported - for now, we require it to be imported */
        diag_emit(DIAG_ERROR, span, "HAMT module must be imported to use persistent maps");
        return NULL;
    }
    
    /* Create the EX_CALL expression with the function's return type */
    Type result_type;
    if (fn_binding->type.kind == TY_FN) {
        result_type = type_from_kind(fn_binding->type.as.fn.result_kind);
    } else {
        result_type = TYPE_NIL;
    }
    Expr *out = expr_new(e->arena, EX_CALL, result_type, span);
    out->as.call_.fn_binding = fn_binding;
    out->as.call_.args = args;
    out->as.call_.n_args = n_args;
    out->as.call_.fn_expr = NULL;
    return out;
}

/* Phase P3: HAMT lowering - lower map function calls when first arg is persistent */
static Expr *elab_lower_map_call(Elab *e, const Form *call, const Symbol *name) {
    uint32_t n_args = call->as.list.len - 1;
    
    /* Elaborate the first argument to check if it's a persistent binding */
    /* For map-new, there are no arguments, so we skip the first arg check */
    if (n_args == 0) {
        /* Only map-new takes 0 arguments */
        if (name != e->sym_map_new) {
            diag_emit(DIAG_ERROR, call->span, "map function '%s' requires at least 1 argument", name->name);
            return NULL;
        }
        /* For map-new, we don't need to check the first arg since there isn't one */
    }
    
    Expr *first_arg = NULL;
    if (n_args > 0) {
        first_arg = elab_form(e, call->as.list.items[1]);
        if (!first_arg) return NULL;
    }
    
    /* Check if first argument is a variable reference to a persistent binding */
    /* For map-new, first_arg is NULL, so we treat it as non-persistent (it creates a new map) */
    bool is_persistent_map = false;
    if (first_arg && first_arg->kind == EX_VAR && first_arg->as.var.binding->is_persistent) {
        is_persistent_map = true;
    }
    
    if (!is_persistent_map) {
        /* Not a persistent binding - fall through to normal elaboration */
        /* Re-elaborate all args together */
        Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
        if (n_args > 0) {
            args[0] = first_arg;
            for (uint32_t i = 1; i < n_args; i++) {
                args[i] = elab_form(e, call->as.list.items[1 + i]);
                if (!args[i]) return NULL;
            }
        }
        
        /* Look up the function binding */
        bool fn_qual_err = false;
        Binding *fn_binding = elab_lookup_sym(e, name, call->as.list.items[0]->span, &fn_qual_err);
        if (!fn_binding && fn_qual_err) return NULL;
        if (!fn_binding) {
            diag_emit(DIAG_ERROR, call->span, "unknown function '%s'", name->name);
            return NULL;
        }
        return elab_call_fn(e, call, fn_binding);
    }
    
    /* Mark that we need HAMT */
    e->needs_hamt = true;
    /* Phase P3: Set global flag for emit phase */
    extern bool g_needs_hamt;
    g_needs_hamt = true;
    
    /* Elaborate remaining arguments (for map-new, n_args is 0, so this is safe) */
    Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    if (n_args > 0) {
        args[0] = first_arg;
        for (uint32_t i = 1; i < n_args; i++) {
            args[i] = elab_form(e, call->as.list.items[1 + i]);
            if (!args[i]) return NULL;
        }
    }
    
    /* Transform based on the function name */
    if (name == e->sym_map_new) {
        if (n_args != 0) {
            diag_emit(DIAG_ERROR, call->span, "map-new takes 0 arguments");
            return NULL;
        }
        /* map-new -> (hamt/new) */
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_new, 0, NULL);
    } else if (name == e->sym_assoc) {
        if (n_args != 3) {
            diag_emit(DIAG_ERROR, call->span, "assoc takes 3 arguments: (assoc map key value)");
            return NULL;
        }
        /* assoc m k v -> (hamt/set m (hamt_hash_ptr k) k v) */
        /* First, compute the hash: (hamt_hash_ptr k) */
        bool hash_qual_err = false;
        Binding *hash_binding = elab_lookup_sym(e, e->sym_hamt_hash_ptr, call->span, &hash_qual_err);
        if (!hash_binding && hash_qual_err) return NULL;
        if (!hash_binding) {
            diag_emit(DIAG_ERROR, call->span, "HAMT module must be imported to use persistent maps");
            return NULL;
        }
        
        /* Create the hash argument */
        Expr **hash_args = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
        hash_args[0] = args[1];  /* key */
        Expr *hash_call = expr_new(e->arena, EX_CALL, (hash_binding->type.kind == TY_FN ? type_from_kind(hash_binding->type.as.fn.result_kind) : TYPE_NIL), call->span);
        hash_call->as.call_.fn_binding = hash_binding;
        hash_call->as.call_.args = hash_args;
        hash_call->as.call_.n_args = 1;
        hash_call->as.call_.fn_expr = NULL;
        
        /* Create args for hamt/set: m, hash, k, v */
        Expr **set_args = (Expr **)arena_alloc(e->arena, 4 * sizeof(Expr *));
        set_args[0] = args[0];  /* m */
        set_args[1] = hash_call;  /* hash */
        set_args[2] = args[1];  /* k */
        set_args[3] = args[2];  /* v */
        
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_set, 4, set_args);
    } else if (name == e->sym_dissoc) {
        if (n_args != 2) {
            diag_emit(DIAG_ERROR, call->span, "dissoc takes 2 arguments: (dissoc map key)");
            return NULL;
        }
        /* dissoc m k -> (hamt/del m (hamt_hash_ptr k) k) */
        bool hash_qual_err2 = false;
        Binding *hash_binding = elab_lookup_sym(e, e->sym_hamt_hash_ptr, call->span, &hash_qual_err2);
        if (!hash_binding && hash_qual_err2) return NULL;
        if (!hash_binding) {
            diag_emit(DIAG_ERROR, call->span, "HAMT module must be imported to use persistent maps");
            return NULL;
        }
        
        Expr **hash_args = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
        hash_args[0] = args[1];
        Expr *hash_call = expr_new(e->arena, EX_CALL, (hash_binding->type.kind == TY_FN ? type_from_kind(hash_binding->type.as.fn.result_kind) : TYPE_NIL), call->span);
        hash_call->as.call_.fn_binding = hash_binding;
        hash_call->as.call_.args = hash_args;
        hash_call->as.call_.n_args = 1;
        hash_call->as.call_.fn_expr = NULL;
        
        Expr **del_args = (Expr **)arena_alloc(e->arena, 3 * sizeof(Expr *));
        del_args[0] = args[0];  /* m */
        del_args[1] = hash_call;  /* hash */
        del_args[2] = args[1];  /* k */
        
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_del, 3, del_args);
    } else if (name == e->sym_map_get) {
        if (n_args != 2) {
            diag_emit(DIAG_ERROR, call->span, "get takes 2 arguments: (get map key)");
            return NULL;
        }
        /* get m k -> (hamt/get m (hamt_hash_ptr k) k) */
        bool hash_qual_err4 = false;
        Binding *hash_binding = elab_lookup_sym(e, e->sym_hamt_hash_ptr, call->span, &hash_qual_err4);
        if (!hash_binding && hash_qual_err4) return NULL;
        if (!hash_binding) {
            diag_emit(DIAG_ERROR, call->span, "HAMT module must be imported to use persistent maps");
            return NULL;
        }
        
        Expr **hash_args = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
        hash_args[0] = args[1];
        Expr *hash_call = expr_new(e->arena, EX_CALL, (hash_binding->type.kind == TY_FN ? type_from_kind(hash_binding->type.as.fn.result_kind) : TYPE_NIL), call->span);
        hash_call->as.call_.fn_binding = hash_binding;
        hash_call->as.call_.args = hash_args;
        hash_call->as.call_.n_args = 1;
        hash_call->as.call_.fn_expr = NULL;
        
        Expr **get_args = (Expr **)arena_alloc(e->arena, 3 * sizeof(Expr *));
        get_args[0] = args[0];  /* m */
        get_args[1] = hash_call;  /* hash */
        get_args[2] = args[1];  /* k */
        
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_get, 3, get_args);
    } else if (name == e->sym_map_has) {
        if (n_args != 2) {
            diag_emit(DIAG_ERROR, call->span, "has? takes 2 arguments: (has? map key)");
            return NULL;
        }
        /* has? m k -> (hamt/has? m (hamt_hash_ptr k) k) */
        bool hash_qual_err3 = false;
        Binding *hash_binding = elab_lookup_sym(e, e->sym_hamt_hash_ptr, call->span, &hash_qual_err3);
        if (!hash_binding && hash_qual_err3) return NULL;
        if (!hash_binding) {
            diag_emit(DIAG_ERROR, call->span, "HAMT module must be imported to use persistent maps");
            return NULL;
        }
        
        Expr **hash_args = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
        hash_args[0] = args[1];
        Expr *hash_call = expr_new(e->arena, EX_CALL, (hash_binding->type.kind == TY_FN ? type_from_kind(hash_binding->type.as.fn.result_kind) : TYPE_NIL), call->span);
        hash_call->as.call_.fn_binding = hash_binding;
        hash_call->as.call_.args = hash_args;
        hash_call->as.call_.n_args = 1;
        hash_call->as.call_.fn_expr = NULL;
        
        Expr **has_args = (Expr **)arena_alloc(e->arena, 3 * sizeof(Expr *));
        has_args[0] = args[0];  /* m */
        has_args[1] = hash_call;  /* hash */
        has_args[2] = args[1];  /* k */
        
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_has, 3, has_args);
    } else if (name == e->sym_map_count) {
        if (n_args != 1) {
            diag_emit(DIAG_ERROR, call->span, "count takes 1 argument: (count map)");
            return NULL;
        }
        /* count m -> (hamt/count m) */
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_count, 1, args);
    } else if (name == e->sym_map_merge) {
        if (n_args != 2) {
            diag_emit(DIAG_ERROR, call->span, "merge takes 2 arguments: (merge a b)");
            return NULL;
        }
        /* merge a b -> (hamt/merge a b) */
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_merge, 2, args);
    }
    
    diag_emit(DIAG_ERROR, call->span, "unexpected map function '%s'", name->name);
    return NULL;
}

/* ---- Phase 2: defn, fn, extern-c ---- */

static Expr *elab_defn(Elab *e, const Form *call);
static Expr *elab_fn(Elab *e, const Form *call);
/* Phase M0: Module system */
static Expr *elab_load(Elab *e, const Form *call);
static Expr *elab_defmodule(Elab *e, const Form *call);
static Expr *elab_extern_c(Elab *e, const Form *call);
static Expr *elab_call_fn(Elab *e, const Form *call, Binding *fn_binding);
/* Phase 11: defstruct */
static Expr *elab_defstruct(Elab *e, const Form *call);
static Expr *elab_make_struct(Elab *e, const Form *call);
/* Phase G0: ADTs */
static Expr *elab_defdata(Elab *e, const Form *call);
static Expr *elab_match(Elab *e, const Form *call);
static CtorDef *elab_lookup_ctor(Elab *e, const Symbol *name);
/* Phase G1: GADTs */
static Expr *elab_defgadt(Elab *e, const Form *call);
/* Phase G3: coerce */
static Expr *elab_coerce(Elab *e, const Form *call);
/* Phase 12: Borrow traits */
static Expr *elab_borrow_immut(Elab *e, const Form *call);
static Expr *elab_borrow_mut(Elab *e, const Form *call);
/* Phase 15: Typeclasses */
static Expr *elab_defclass(Elab *e, const Form *call);
static Expr *elab_definstance(Elab *e, const Form *call);
static Expr *elab_method_call(Elab *e, const Form *call);
static TypeClassMethod *parse_typeclass_method(Elab *e, Form *method_form, Span span);
/* Phase HKT H5: kind aliases */
static Expr *elab_defkind(Elab *e, const Form *call);
static Expr *elab_defrec(Elab *e, const Form *call);  /* Phase HKT-P2 */
static Expr *elab_deftype(Elab *e, const Form *call);  /* Phase HKT-P2 */
static Expr *elab_type_app(Elab *e, const Form *call);  /* Phase HKT-P1 */
/* Phase 18: call/cc and escape sugar */
static Expr *elab_call_cc(Elab *e, const Form *call);
static Expr *elab_escape(Elab *e, const Form *call);
/* Phase 19: cont? predicate */
static Expr *elab_cont_pred(Elab *e, const Form *call);
/* Phase R2: Panic */
static Expr *elab_panic(Elab *e, const Form *call);
/* Phase T19-B: thread-spawn (Send-safety check for cross-thread closures) */
static Expr *elab_thread_spawn(Elab *e, const Form *call);
/* Phase T21-F: async/await sugar */
static Expr *elab_async(Elab *e, const Form *call);
static Expr *elab_await(Elab *e, const Form *call);
/* Phase SEL1: fair multi-channel select */
static Expr *elab_select(Elab *e, const Form *call);
/* Phase 20: Software Transactional Memory */
static Expr *elab_stm(Elab *e, const Form *call);
static Expr *elab_atomically(Elab *e, const Form *call);
static Expr *elab_retry(Elab *e, const Form *call);
static Expr *elab_check(Elab *e, const Form *call);
static Expr *elab_or_else(Elab *e, const Form *call);
static Expr *elab_tvar_new(Elab *e, const Form *call);
static Expr *elab_tvar_read(Elab *e, const Form *call);
static Expr *elab_tvar_write(Elab *e, const Form *call);
static Expr *elab_tvar_modify(Elab *e, const Form *call);
static Expr *elab_tvar_swap(Elab *e, const Form *call);
static Expr *elab_tvar_cas(Elab *e, const Form *call);

/* Phase N: (as TargetType expr) — explicit numeric cast (defined later) */
static Expr *elab_as_cast(Elab *e, const Form *call);

/* ---- general elab ---- */

static Expr *elab_call(Elab *e, Form *call) {
    /* Already established: call->tag == F_LIST and len >= 1. */
    Form *head = call->as.list.items[0];

    /* CY1: Support ((expr) args...) where expr evaluates to a fat closure (ptr<void>).
     * Elaborate the head expression and treat it as a fat-closure dynamic call. */
    if (head->tag != F_SYM) {
        Expr *head_expr = elab_form(e, head);
        if (!head_expr) return NULL;
        if (head_expr->type.kind != TY_PTR_VOID) {
            diag_emit(DIAG_ERROR, head->span,
                      "call head must be a symbol or closure expression");
            return NULL;
        }
        /* Treat as a fat-closure dynamic call: create a synthetic binding for the head */
        uint32_t n_args = call->as.list.len - 1;
        /* Elaborate arguments */
        Expr **args = NULL;
        if (n_args > 0) {
            args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
            for (uint32_t i = 0; i < n_args; i++) {
                args[i] = elab_form(e, call->as.list.items[1 + i]);
                if (!args[i]) return NULL;
            }
        }
        /* We need a temporary binding for the head value so emit.c can name it.
         * Wrap in EX_LET: let [__tmp = head_expr] (call __tmp args...) */
        char tmp_name[32];
        snprintf(tmp_name, sizeof(tmp_name), "__ccall%u", e->next_id++);
        const Symbol *tmp_sym = symtab_intern(e->st, strslice(tmp_name, (uint32_t)strlen(tmp_name)));
        Binding *tmp_b = binding_new(e, tmp_sym, TYPE_PTR_VOID, false, false, head->span);
        /* Build call expression using tmp_b as fn_binding */
        Expr *call_expr = expr_new(e->arena, EX_CALL, TYPE_INT, call->span);
        call_expr->as.call_.fn_binding = tmp_b;
        call_expr->as.call_.args = args;
        call_expr->as.call_.n_args = n_args;
        call_expr->as.call_.fn_expr = NULL;
        call_expr->as.call_.dict_arg = NULL;
        call_expr->as.call_.is_poly_call = false;
        call_expr->as.call_.poly_arg_mask = 0;
        /* Wrap in EX_LET */
        LetBinding *let_bs = (LetBinding *)arena_alloc(e->arena, sizeof(LetBinding));
        let_bs->binding = tmp_b;
        let_bs->init = head_expr;
        Expr *let_expr = expr_new(e->arena, EX_LET, TYPE_INT, call->span);
        let_expr->as.let_.bindings = let_bs;
        let_expr->as.let_.n = 1;
        let_expr->as.let_.body = call_expr;
        return let_expr;
    }
    const Symbol *name = head->as.sym;

    /* Special forms. */
    if (name == e->sym_def)    return elab_def   (e, call);
    if (name == e->sym_let)    return elab_let   (e, call);
    if (name == e->sym_if)     return elab_if    (e, call);
    if (name == e->sym_do)     return elab_do    (e, call);
    if (name == e->sym_unsafe) return elab_unsafe(e, call);
    if (name == e->sym_set)    return elab_set   (e, call);
    if (name == e->sym_while)  return elab_while (e, call);
    if (name == e->sym_case)   return elab_case  (e, call);
    /* Phase 4 */
    if (name == e->sym_defer)  return elab_defer (e, call);
    if (name == e->sym_return) return elab_return(e, call);
    /* Phase 5 */
    if (name == e->sym_ref)    return elab_ref   (e, call);
    if (name == e->sym_deref)  return elab_deref (e, call);
    if (name == e->sym_drop)   return elab_drop  (e, call);
    /* LT3: lref<T> */
    if (name == e->sym_lref_new) return elab_lref_new(e, call);
    /* Phase 9: rc<T> + weak<T> */
    if (name == e->sym_rc_of)       return elab_rc_of(e, call);
    if (name == e->sym_rc_clone)    return elab_rc_clone(e, call);
    if (name == e->sym_rc_drop)     return elab_rc_drop(e, call);
    if (name == e->sym_rc_ptr)      return elab_rc_ptr(e, call);
    if (name == e->sym_rc_strong_count) return elab_rc_strong_count(e, call);
    if (name == e->sym_rc_from_ref) return elab_rc_from_ref(e, call);
    if (name == e->sym_ref_from_rc) return elab_ref_from_rc(e, call);
    if (name == e->sym_weak)        return elab_weak(e, call);
    if (name == e->sym_upgrade)     return elab_weak_upgrade(e, call);
    if (name == e->sym_weak_pred)   return elab_weak_pred(e, call);
    if (name == e->sym_ref_pred)    return elab_ref_pred(e, call);
    /* Phase 18: Delimited continuations */
    if (name == e->sym_reset)      return elab_reset(e, call);
    if (name == e->sym_shift)      return elab_shift(e, call);
    if (name == e->sym_shift0)     return elab_shift0(e, call);
    if (name == e->sym_call_cc)    return elab_call_cc(e, call);
    if (name == e->sym_escape)     return elab_escape(e, call);
    /* Phase B2: Cloneable continuations */
    if (name == e->sym_cloneable_reset)  return elab_cloneable_reset(e, call);
    if (name == e->sym_cloneable_shift)  return elab_cloneable_shift(e, call);
    if (name == e->sym_call_cc_star)      return elab_call_cc_star(e, call);
    /* Phase 21: Serializable continuations */
    if (name == e->sym_serial_reset) return elab_serial_reset(e, call);
    if (name == e->sym_serial_shift) return elab_serial_shift(e, call);
    /* Phase 19: Algebraic effects */
    if (name == e->sym_defeffect) return elab_defeffect(e, call);
    if (name == e->sym_perform)   return elab_perform(e, call);
    if (name == e->sym_handle)       return elab_handle(e, call);
    if (name == e->sym_try_with)     return elab_try_with(e, call);
    if (name == e->sym_with_handler) return elab_handle(e, call);  /* T25: sugar for handle in async context */
    if (name == e->sym_resume)    return elab_resume(e, call);
    if (name == e->sym_discontinue) return elab_discontinue(e, call);
    /* ET3-E: compose-handlers */
    if (name == e->sym_compose_handlers) return elab_compose_handlers(e, call);
    if (name == e->sym_cont_pred)   return elab_cont_pred(e, call);
    /* Phase 10: GC */
    if (name == e->sym_gc_force)    return elab_gc_force(e, call);
    if (name == e->sym_gc_enable)   return elab_gc_enable(e, call);
    if (name == e->sym_gc_disable)  return elab_gc_disable(e, call);
    /* Phase M0: Module system */
    if (name == e->sym_load)      return elab_load(e, call);
    if (name == e->sym_defmodule) return elab_defmodule(e, call);
    if (name == e->sym_export) {
        diag_emit(DIAG_ERROR, call->span,
                  "export is only allowed inside defmodule");
        return NULL;
    }
    if (name == e->sym_import) {
        diag_emit(DIAG_ERROR, call->span,
                  "import is only allowed inside defmodule");
        return NULL;
    }
    /* Phase N: numeric cast */
    if (name == e->sym_as) return elab_as_cast(e, call);
    /* IT4: gradual typing */
    if (name == e->sym_type_of) return elab_any_type_of(e, call);
    if (name == e->sym_cast)    return elab_any_cast(e, call);
    /* Phase 11: defstruct */
    if (name == e->sym_defstruct) return elab_defstruct(e, call);
    if (name == e->sym_make_struct) return elab_make_struct(e, call);
    /* Phase G0: ADTs */
    if (name == e->sym_defdata) return elab_defdata(e, call);
    if (name == e->sym_match) return elab_match(e, call);
    if (name == e->sym_defgadt) return elab_defgadt(e, call);
    if (name == e->sym_coerce)  return elab_coerce(e, call);
    /* Phase 12: Borrow traits */
    if (name == e->sym_borrow) return elab_borrow_immut(e, call);
    if (name == e->sym_borrow_mut) return elab_borrow_mut(e, call);
    /* Phase 15: Typeclasses */
    if (name == e->sym_defclass) return elab_defclass(e, call);
    if (name == e->sym_definstance) return elab_definstance(e, call);
    /* Phase HKT H5: kind aliases */
    if (name == e->sym_defkind) return elab_defkind(e, call);
    /* Phase HKT-P2: recursive type binders */
    if (name == e->sym_defrec) return elab_defrec(e, call);
    if (name == e->sym_deftype) return elab_deftype(e, call);
    /* Phase HRT0: forall/exists are type-level forms; reject in expression position */
    if (name == e->sym_forall || name == e->sym_forall_u) {
        diag_emit(DIAG_ERROR, call->span,
                  "'forall' is a type-level annotation and cannot appear in expression position "
                  "(use it in a type annotation: (deftype MyType (forall [a] ...)))");
        return NULL;
    }
    if (name == e->sym_exists || name == e->sym_exists_u) {
        diag_emit(DIAG_ERROR, call->span,
                  "'exists' is a type-level annotation and cannot appear in expression position "
                  "(use it in a type annotation: (deftype MyType (exists [a] ...)))");
        return NULL;
    }
    /* Phase HKT-P1: type-level application */
    if (name == e->sym_type_app) return elab_type_app(e, call);
    /* Phase HRT1: (:: expr type) — type ascription */
    if (name == e->sym_ascribe) return elab_ascribe(e, call);
    /* Phase HRT2: existential types */
    if (name == e->sym_pack) return elab_pack(e, call);
    if (name == e->sym_open) return elab_open(e, call);
    /* Phase R2: Panic */
    if (name == e->sym_panic) return elab_panic(e, call);
    if (name == e->sym_panic_with) return elab_panic_with(e, call);
    if (name == e->sym_catch_unwind) return elab_catch_unwind(e, call);
    if (name == e->sym_catch_panic_of) return elab_catch_panic_of(e, call);
    if (name == e->sym_throw) return elab_throw(e, call);
    if (name == e->sym_try)   return elab_try_catch(e, call);
    if (name == e->sym_panic_payload_type) return elab_panic_payload_type(e, call);
    if (name == e->sym_panic_payload_value) return elab_panic_payload_value(e, call);
    if (name == e->sym_panic_payload_file) return elab_panic_payload_file(e, call);
    if (name == e->sym_panic_payload_line) return elab_panic_payload_line(e, call);
    if (name == e->sym_panic_payload_downcast) return elab_panic_payload_downcast(e, call);
    /* Phase U3: Unsafe primitives - pointer operations */
    if (name == e->sym_ptr_deref)   return elab_ptr_deref(e, call);
    if (name == e->sym_ptr_write)  return elab_ptr_write(e, call);
    if (name == e->sym_ptr_add)     return elab_ptr_add(e, call);
    if (name == e->sym_ptr_sub)     return elab_ptr_sub(e, call);
    if (name == e->sym_ptr_nullq)   return elab_ptr_nullq(e, call);
    if (name == e->sym_ptr_of)      return elab_ptr_of(e, call);
    /* Phase U3: Unsafe primitives - type casting */
    if (name == e->sym_unsafe_cast) return elab_unsafe_cast(e, call);
    if (name == e->sym_reinterpret) return elab_reinterpret(e, call);
    if (name == e->sym_transmute)   return elab_transmute(e, call);
    /* Phase U3: Unsafe primitives - unchecked array ops */
    if (name == e->sym_array_get_unchecked)  return elab_array_get_unchecked(e, call);
    if (name == e->sym_array_set_unchecked)  return elab_array_set_unchecked(e, call);
    /* Phase U3: Unsafe primitives - raw memory */
    if (name == e->sym_raw_malloc)  return elab_raw_malloc(e, call);
    if (name == e->sym_raw_free)    return elab_raw_free(e, call);
    if (name == e->sym_raw_realloc) return elab_raw_realloc(e, call);
    if (name == e->sym_raw_memcpy)  return elab_raw_memcpy(e, call);
    if (name == e->sym_raw_memset)  return elab_raw_memset(e, call);
    /* Phase U3: Unsafe primitives - FFI */
    if (name == e->sym_c_call)      return elab_c_call(e, call);
    if (name == e->sym_dlopen)      return elab_dlopen(e, call);
    if (name == e->sym_dlsym)       return elab_dlsym(e, call);
    if (name == e->sym_dlclose)     return elab_dlclose(e, call);
    /* Phase T19-B: thread-spawn (Send-safety check for cross-thread closures) */
    /* Only intercept when arg[1] is a literal (fn ...) form; if the user has  */
    /* defined their own thread-spawn function, let it fall through below.      */
    if (name == e->sym_thread_spawn &&
        call->as.list.len == 2 &&
        call->as.list.items[1]->tag == F_LIST &&
        call->as.list.items[1]->as.list.len >= 1 &&
        call->as.list.items[1]->as.list.items[0]->tag == F_SYM &&
        call->as.list.items[1]->as.list.items[0]->as.sym == e->sym_fn)
        return elab_thread_spawn(e, call);
    /* Phase T21-F: async/await sugar */
    if (name == e->sym_async && call->as.list.len == 2)
        return elab_async(e, call);
    if (name == e->sym_await && call->as.list.len == 2)
        return elab_await(e, call);
    /* Phase SEL1: fair multi-channel select */
    if (name == e->sym_select && call->as.list.len >= 2)
        return elab_select(e, call);
    /* Phase 20: Software Transactional Memory */
    if (name == e->sym_stm) return elab_stm(e, call);
    if (name == e->sym_atomically && call->as.list.len == 2)
        return elab_atomically(e, call);
    if (name == e->sym_retry) return elab_retry(e, call);
    if (name == e->sym_check && call->as.list.len == 2)
        return elab_check(e, call);
    if (name == e->sym_or_else && call->as.list.len == 3)
        return elab_or_else(e, call);
    if (name == e->sym_tvar_new && call->as.list.len == 2)
        return elab_tvar_new(e, call);
    if (name == e->sym_tvar_read && call->as.list.len == 2)
        return elab_tvar_read(e, call);
    if (name == e->sym_tvar_write && call->as.list.len == 3)
        return elab_tvar_write(e, call);
    if (name == e->sym_tvar_modify && call->as.list.len == 3)
        return elab_tvar_modify(e, call);
    if (name == e->sym_tvar_swap && call->as.list.len == 3)
        return elab_tvar_swap(e, call);
    if (name == e->sym_tvar_cas && call->as.list.len == 4)
        return elab_tvar_cas(e, call);
    /* TVar operations with / syntax */
    if (name == e->sym_tvar && call->as.list.len >= 2) {
        Form *op = call->as.list.items[1];
        if (op->tag == F_SYM) {
            if (op->as.sym == e->sym_new && call->as.list.len == 3)
                return elab_tvar_new(e, call);
            if (op->as.sym == e->sym_read && call->as.list.len == 3)
                return elab_tvar_read(e, call);
            if (op->as.sym == e->sym_write && call->as.list.len == 4)
                return elab_tvar_write(e, call);
            if (op->as.sym == e->sym_modify && call->as.list.len == 4)
                return elab_tvar_modify(e, call);
            if (op->as.sym == e->sym_swap && call->as.list.len == 4)
                return elab_tvar_swap(e, call);
            if (op->as.sym == e->sym_cas && call->as.list.len == 5)
                return elab_tvar_cas(e, call);
        }
    }
    /* Phase R1: ? operator — lowers to early-return on err */
    if (name == e->sym_question) {
        return elab_question(e, call);
    }
    /* Phase 15: Method call syntax - (.method obj arg1 arg2) */
    if (name->len > 0 && name->name[0] == '.') {
        return elab_method_call(e, call);
    }
    /* Phase 6 */
    if (name == e->sym_defmacro) return elab_defmacro(e, call);
    if (name == e->sym_quote)    return elab_form(e, call->as.list.items[1]); /* (quote x) -> x */
    if (name == e->sym_gensym)   return elab_gensym(e, call);
    if (name == e->sym_thread)    return elab_thread(e, call);
    if (name == e->sym_thread_last) return elab_thread_last(e, call);

    /* Phase 2 */
    if (name == e->sym_defn)    return elab_defn  (e, call);
    if (name == e->sym_fn)      return elab_fn    (e, call);
    if (name == e->sym_lambda)  return elab_fn    (e, call); /* λ aliases fn */
    if (name == e->sym_extern_c) return elab_extern_c(e, call);

    /* Phase P3: HAMT lowering - lower map function calls when first arg is persistent */
    if (name == e->sym_map_new || name == e->sym_assoc || name == e->sym_dissoc ||
        name == e->sym_map_get || name == e->sym_map_has || name == e->sym_map_count ||
        name == e->sym_map_merge) {
        return elab_lower_map_call(e, call, name);
    }

    /* Phase 6: Check if it's a macro call */
    MacroDef *macro = elab_lookup_macro(e, name);
    if (macro) {
        if (e->macro_expand_depth >= ELAB_MAX_MACRO_EXPANSION_DEPTH) {
            diag_emit(DIAG_ERROR, call->span, "maximum macro expansion depth exceeded");
            return NULL;
        }
        e->macro_expand_depth++;
        /* Expand the macro with arguments */
        /* Extract arguments (rest of list) */
        uint32_t n_args = call->as.list.len - 1;
        Form **args = (n_args == 0) ? NULL : (Form **)arena_alloc(e->arena, n_args * sizeof(Form *));
        for (uint32_t i = 0; i < n_args; i++) {
            args[i] = call->as.list.items[1 + i];
        }
        
        Form *expanded = elab_expand_macro(e, macro, args, n_args);
        if (!expanded) {
            e->macro_expand_depth--;
            return NULL;
        }

        /* Phase M4: Keep the expansion-module context active while elaborating
         * the expanded form so private helper macros from the same module are
         * visible when the expansion calls them (e.g. triple → helper-double). */
        const Symbol *saved_expansion = e->macro_expansion_module;
        e->macro_expansion_module = macro->defining_module_name;
        Expr *out = elab_form(e, expanded);
        e->macro_expansion_module = saved_expansion;
        e->macro_expand_depth--;
        return out;
    }

    /* Phase 2: Check if it's a user-defined function call.
     * M1: Use elab_lookup_sym for visibility + qualified name resolution. */
    bool fn_qual_err = false;
    Binding *fn_binding = elab_lookup_sym(e, name, head->span, &fn_qual_err);
    if (!fn_binding && fn_qual_err) return NULL;

    /* Phase G0: constructor call — (Ctor) or (Ctor :T1 ...) */
    if (fn_binding && fn_binding->type.kind == TY_ADT) {
        /* 0-arg constructor */
        AdtDef *adt = fn_binding->type.as.adt_.def;
        CtorDef *ctor = NULL;
        for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
            if (strcmp(adt->ctors[ci]->name, name->name) == 0) {
                ctor = adt->ctors[ci];
                break;
            }
        }
        if (ctor && ctor->n_fields == 0) {
            uint32_t n_args_given = call->as.list.len - 1;
            if (n_args_given != 0) {
                diag_emit(DIAG_ERROR, call->span,
                          "constructor '%s' takes 0 arguments, got %u",
                          name->name, n_args_given);
                return NULL;
            }
            Expr *out = expr_new(e->arena, EX_CALL, fn_binding->type, call->span);
            out->as.call_.fn_binding = fn_binding;
            out->as.call_.args = NULL;
            out->as.call_.n_args = 0;
            out->as.call_.fn_expr = NULL;
            out->as.call_.dict_arg = NULL;
            return out;
        }
    }

    /* Phase G0: N-arg constructor call — result type needs ADT def pointer */
    if (fn_binding && fn_binding->type.kind == TY_FN &&
        fn_binding->type.as.fn.result_kind == TY_ADT) {
        /* Look up the constructor to find its AdtDef */
        CtorDef *ctor = elab_lookup_ctor(e, name);
        if (ctor) {
            /* Use elab_call_fn but fix up the result type after */
            Expr *call_expr = elab_call_fn(e, call, fn_binding);
            if (call_expr) {
                /* Patch result type with proper AdtDef pointer */
                call_expr->type = type_adt(ctor->adt);
            }
            return call_expr;
        }
        /* Phase G3: Non-constructor function returning ADT — patch result from result_full_type */
        Expr *call_expr = elab_call_fn(e, call, fn_binding);
        if (call_expr && fn_binding->type.as.fn.result_full_type) {
            call_expr->type = *fn_binding->type.as.fn.result_full_type;
        }
        return call_expr;
    }

    if (fn_binding && (fn_binding->type.kind == TY_FN ||
                       (fn_binding->type.kind == TY_PTR_VOID && fn_binding->closure_fn_binding) ||
                       fn_binding->closure_fn_binding)) {
        return elab_call_fn(e, call, fn_binding);
    }

    /* Phase 19: Allow calling any binding (for function parameters, higher-order functions) */
    if (fn_binding) {
        return elab_call_fn(e, call, fn_binding);
    }

    /* Builtin operator. Evaluate args first, then look up. */
    uint32_t n_args = call->as.list.len - 1;
    Expr **args = (n_args == 0) ? NULL :
        (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!args[i]) return NULL;
        /* Phase 11: Move tracking - if arg is a CK_MOVE binding reference, poison it.
         * UT2 exception: ^unique ^mut bindings represent exclusive mutable access;
         * builtins never take unique ownership, so don't consume them. */
        if (args[i]->kind == EX_VAR && type_is_move(args[i]->as.var.binding->type)) {
            Binding *arg_b2 = args[i]->as.var.binding;
            bool arg_is_unique_mut = g_unique_enabled && arg_b2->is_unique && arg_b2->is_mut;
            if (!arg_is_unique_mut) {
                binding_mark_moved(arg_b2, args[i]->span);
            }
        }
    }
    Type first_t = (n_args > 0) ? args[0]->type : TYPE_NIL;
    const BuiltinSpec *spec = builtin_lookup(name, first_t, n_args);
    if (!spec) {
        const BuiltinSpec *any = builtin_first_with_name(name);
        if (any) {
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0006_OPERATOR_LOOKUP_FAILED,
                                "operator lookup failed for '%s': got %u arg(s), first arg type %s",
                                name->name, n_args,
                                n_args > 0 ? type_name(first_t) : "<none>");

            const BuiltinSpec *overloads[32];
            uint32_t n_overloads = builtin_collect_with_name(name, overloads, 32);
            for (uint32_t oi = 0; oi < n_overloads; oi++) {
                const BuiltinSpec *ov = overloads[oi];
                const char *arg_name = (ov->arg_type.kind == TY_UNKNOWN)
                    ? "any"
                    : type_name(ov->arg_type);
                const char *res_name = type_name(ov->result_type);
                if (ov->max_arity < 0) {
                    diag_emit(DIAG_NOTE, call->span,
                              "available overload: %s arity %d..* arg=%s result=%s",
                              ov->name, ov->min_arity, arg_name, res_name);
                } else {
                    diag_emit(DIAG_NOTE, call->span,
                              "available overload: %s arity %d..%d arg=%s result=%s",
                              ov->name, ov->min_arity, ov->max_arity, arg_name, res_name);
                }
            }
        } else if (e->separate_compilation) {
            diag_emit(DIAG_ERROR, head->span,
                      "unknown function or operator '%s'", name->name);
        } else {
            /* eval mode: create a runtime-dispatch call so native builtins
             * registered in TuriEnv (e.g. async scheduler functions) are
             * callable without a compile-time declaration.
             * The binding is NOT added to any scope so future lookups don't
             * find a TYPE_INT entry and route through elab_call_fn. */
            Binding *dyn_b = binding_new(e, name, TYPE_INT, false, false, head->span);
            Expr *var_expr = expr_new(e->arena, EX_VAR, TYPE_INT, head->span);
            var_expr->as.var.binding = dyn_b;
            Expr *out = expr_new(e->arena, EX_CALL, TYPE_INT, call->span);
            out->as.call_.fn_binding = NULL;
            out->as.call_.fn_expr    = var_expr;
            out->as.call_.args       = args;
            out->as.call_.n_args     = n_args;
            return out;
        }
        return NULL;
    }
    /* All args must match the spec's arg type. */
    for (uint32_t i = 0; i < n_args; i++) {
        if (!type_eq(args[i]->type, spec->arg_type)) {
            const char *expected_str = type_name(spec->arg_type);
            const char *actual_str = type_name(args[i]->type);
            
            /* Check if we can suggest a coercion */
            const char *suggestion = NULL;
            if (args[i]->type.kind == TY_BOOL && spec->arg_type.kind == TY_INT) {
                suggestion = "try wrapping the bool in (if x 1 0)";
            } else if (typekind_is_numeric(args[i]->type.kind) &&
                       typekind_is_numeric(spec->arg_type.kind)) {
                suggestion = "use (as <type> expr) for explicit numeric conversion";
            }

            diag_emit_with_code(DIAG_ERROR, args[i]->span, TUR_E0001_TYPE_MISMATCH,
                                "'%s' arg %u: type mismatch - expected %s, got %s",
                                name->name, i + 1, expected_str, actual_str);
            if (suggestion) {
                diag_emit(DIAG_HELP, args[i]->span, "%s", suggestion);
            }
            diag_emit(DIAG_NOTE, args[i]->span, "argument has this type");
            return NULL;
        }
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, spec->result_type, call->span);
    out->as.builtin.spec = spec;
    out->as.builtin.args = args;
    out->as.builtin.n = n_args;
    return out;
}

/* CY1: Partially apply a function, returning a closure over the provided args.
 *
 * fn_type   -- the EFFECTIVE function type (thunk type if closure, including env param)
 * fn_binding -- the binding being called (closure_fn_binding != NULL iff it's a closure)
 * elab_args -- already-elaborated argument expressions [0..n_provided-1]
 * n_provided -- number of arguments already provided (< full_arity)
 */
static Expr *elab_partial_apply(Elab *e, const Form *call, Binding *fn_binding,
                                 Type fn_type, Expr **elab_args, uint32_t n_provided) {
    bool fn_is_closure = (fn_binding->closure_fn_binding != NULL);

    /* full_arity = user-visible arg count (strips env from thunk arity) */
    uint32_t full_arity = fn_type.as.fn.arity;
    if (fn_is_closure) full_arity--; /* strip hidden env param */

    uint32_t n_remaining = full_arity - n_provided;
    TypeKind result_kind = fn_type.as.fn.result_kind;

    /* Build capture bindings for provided args */
    Binding **cap_bindings = (Binding **)arena_alloc(e->arena, n_provided * sizeof(Binding *));
    for (uint32_t i = 0; i < n_provided; i++) {
        char cap_name[32];
        snprintf(cap_name, sizeof(cap_name), "__papc%u", e->next_id++);
        const Symbol *cap_sym = symtab_intern(e->st, strslice(cap_name, (uint32_t)strlen(cap_name)));
        /* arg type from fn_type: skip index 0 if closure (env), so index = i+1 if closure, else i */
        TypeKind cap_kind = fn_type.as.fn.arg_kinds[fn_is_closure ? (i + 1) : i];
        Type cap_type = type_from_kind(cap_kind);
        Binding *cap_b = binding_new(e, cap_sym, cap_type, false, false, call->span);
        cap_bindings[i] = cap_b;
    }

    /* Build remaining param bindings for the new thunk */
    Binding **rem_params = (Binding **)arena_alloc(e->arena, n_remaining * sizeof(Binding *));
    TypeKind rem_kinds[MAX_FN_ARITY];
    for (uint32_t i = 0; i < n_remaining; i++) {
        char rem_name[32];
        snprintf(rem_name, sizeof(rem_name), "__papr%u", e->next_id++);
        const Symbol *rem_sym = symtab_intern(e->st, strslice(rem_name, (uint32_t)strlen(rem_name)));
        TypeKind rem_kind = fn_type.as.fn.arg_kinds[fn_is_closure ? (n_provided + 1 + i) : (n_provided + i)];
        Type rem_type = type_from_kind(rem_kind);
        Binding *rem_b = binding_new(e, rem_sym, rem_type, false, false, call->span);
        rem_params[i] = rem_b;
        rem_kinds[i] = rem_kind;
    }

    /* Build the inner call expression: (fn_binding cap0 cap1 ... rem0 rem1 ...) */
    /* n_call_args = full_arity (all user-visible args) */
    uint32_t n_call_args = full_arity;
    Expr **call_args = (Expr **)arena_alloc(e->arena, n_call_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_provided; i++) {
        Expr *var = expr_new(e->arena, EX_VAR, cap_bindings[i]->type, call->span);
        var->as.var.binding = cap_bindings[i];
        call_args[i] = var;
    }
    for (uint32_t i = 0; i < n_remaining; i++) {
        Expr *var = expr_new(e->arena, EX_VAR, rem_params[i]->type, call->span);
        var->as.var.binding = rem_params[i];
        call_args[n_provided + i] = var;
    }

    Type body_result_type = type_from_kind(result_kind);
    Expr *inner_call = expr_new(e->arena, EX_CALL, body_result_type, call->span);
    inner_call->as.call_.fn_binding = fn_binding;
    inner_call->as.call_.args = call_args;
    inner_call->as.call_.n_args = n_call_args;
    inner_call->as.call_.fn_expr = NULL;
    inner_call->as.call_.dict_arg = NULL;
    inner_call->as.call_.is_poly_call = false;
    inner_call->as.call_.poly_arg_mask = 0;

    /* Build the thunk FnDef */
    /* Thunk params: [env_param (TY_PTR_VOID), rem_param_0, ..., rem_param_{n_remaining-1}] */
    uint8_t thunk_n_params = (uint8_t)(1 + n_remaining);
    Binding **thunk_params = (Binding **)arena_alloc(e->arena, thunk_n_params * sizeof(Binding *));
    Type *thunk_param_types = (Type *)arena_alloc(e->arena, thunk_n_params * sizeof(Type));

    /* env param */
    char env_param_name[32];
    snprintf(env_param_name, sizeof(env_param_name), "__pap_env_%u", e->next_id++);
    const Symbol *env_param_sym = symtab_intern(e->st, strslice(env_param_name, (uint32_t)strlen(env_param_name)));
    Binding *env_param_b = binding_new(e, env_param_sym, TYPE_PTR_VOID, false, false, call->span);
    thunk_params[0] = env_param_b;
    thunk_param_types[0] = TYPE_PTR_VOID;

    for (uint32_t i = 0; i < n_remaining; i++) {
        thunk_params[1 + i] = rem_params[i];
        thunk_param_types[1 + i] = type_from_kind(rem_kinds[i]);
    }

    /* Thunk type: (TY_PTR_VOID, rem_kinds...) -> result_kind */
    TypeKind thunk_arg_kinds[MAX_FN_ARITY];
    thunk_arg_kinds[0] = TY_PTR_VOID;
    for (uint32_t i = 0; i < n_remaining; i++) {
        thunk_arg_kinds[1 + i] = rem_kinds[i];
    }
    Type thunk_type = type_fn(thunk_arg_kinds, thunk_n_params, result_kind);

    /* Thunk binding (global) */
    char pap_name[32];
    snprintf(pap_name, sizeof(pap_name), "__pap%u", e->next_id++);
    const Symbol *pap_sym = symtab_intern(e->st, strslice(pap_name, (uint32_t)strlen(pap_name)));
    Binding *thunk_binding = binding_new(e, pap_sym, thunk_type, false, true, call->span);
    scope_add(&e->global, thunk_binding);

    /* Build FnDef */
    FnDef *pap_fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    memset(pap_fd, 0, sizeof(FnDef));
    pap_fd->binding = thunk_binding;
    pap_fd->params = thunk_params;
    pap_fd->n_params = thunk_n_params;
    pap_fd->param_types = thunk_param_types;
    pap_fd->body = inner_call;
    pap_fd->is_variadic = false;
    pap_fd->inferred_effect_row = NULL;
    constraint_set_init(&pap_fd->constraints);

    /* Build the env struct name */
    char pap_env_name[32];
    snprintf(pap_env_name, sizeof(pap_env_name), "__pap_env_s_%u", e->next_id++);
    const Symbol *pap_env_sym = symtab_intern(e->st, strslice(pap_env_name, (uint32_t)strlen(pap_env_name)));

    /* Build captures list: [cap_binding[0], ..., cap_binding[n_provided-1]] + fn_binding if closure */
    uint32_t n_pap_captures = n_provided + (fn_is_closure ? 1 : 0);
    Binding **pap_captures = (Binding **)arena_alloc(e->arena, (n_pap_captures ? n_pap_captures : 1) * sizeof(Binding *));
    for (uint32_t i = 0; i < n_provided; i++) {
        pap_captures[i] = cap_bindings[i];
    }
    if (fn_is_closure) {
        pap_captures[n_provided] = fn_binding;
    }

    /* Build Closure struct */
    struct Closure *pap_closure = (struct Closure *)arena_alloc(e->arena, sizeof(struct Closure));
    pap_closure->fn = pap_fd;
    pap_closure->captures = pap_captures;
    pap_closure->n_captures = (uint8_t)n_pap_captures;
    pap_closure->env_name = pap_env_sym;

    /* Wire closure into FnDef (required for emit_fn_def to emit the env struct) */
    pap_fd->closure = pap_closure;

    /* Register thunk at file scope */
    Expr *fn_def_expr = expr_new(e->arena, EX_FN_DEF, thunk_type, call->span);
    fn_def_expr->as.fn_def_.fn = pap_fd;
    elab_register_file_def(e, fn_def_expr);

    /* Build EX_CLOSURE */
    Expr *closure_expr = expr_new(e->arena, EX_CLOSURE, TYPE_PTR_VOID, call->span);
    closure_expr->as.closure_.closure = pap_closure;

    if (n_provided == 0) {
        /* Edge case: no args provided, just return the closure directly */
        return closure_expr;
    }

    /* Wrap in EX_LET: let [cap0 = arg0, cap1 = arg1, ...] closure_expr */
    LetBinding *let_bs = (LetBinding *)arena_alloc(e->arena, n_provided * sizeof(LetBinding));
    for (uint32_t i = 0; i < n_provided; i++) {
        let_bs[i].binding = cap_bindings[i];
        let_bs[i].init = elab_args[i];
    }

    Expr *let_expr = expr_new(e->arena, EX_LET, TYPE_PTR_VOID, call->span);
    let_expr->as.let_.bindings = let_bs;
    let_expr->as.let_.n = n_provided;
    let_expr->as.let_.body = closure_expr;

    return let_expr;
}

/* Phase 2: Elaborate a function call (f a b c) */
static Expr *elab_call_fn(Elab *e, const Form *call, Binding *fn_binding) {
    uint32_t n_args = call->as.list.len - 1;

    /* Get the function type */
    Type fn_type = fn_binding->type;
    
    /* Phase HRT1: rank-2 polymorphic function parameter call — intercept before closure/PTR_VOID. */
    if (fn_binding->is_poly_fn) {
        return elab_poly_call(e, call, fn_binding);
    }

    /* For closure bindings, use the closure's thunk function type */
    if (fn_binding->closure_fn_binding) {
        /* This is a closure - get the thunk function type */
        fn_type = fn_binding->closure_fn_binding->type;
    } else if (fn_binding->type.kind == TY_PTR_VOID) {
        /* CY2: fat-closure dynamic dispatch through ptr<void> binding.
         * Supports 0-arg (original behavior) and n-arg (new fat-closure call). */
        Expr **cb_args = NULL;
        if (n_args > 0) {
            cb_args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
            for (uint32_t i = 0; i < n_args; i++) {
                cb_args[i] = elab_form(e, call->as.list.items[1 + i]);
                if (!cb_args[i]) return NULL;
            }
        }
        Expr *out = expr_new(e->arena, EX_CALL, TYPE_INT, call->span);
        out->as.call_.fn_binding = fn_binding;
        out->as.call_.args = cb_args;
        out->as.call_.n_args = n_args;
        out->as.call_.fn_expr = NULL;
        out->as.call_.dict_arg = NULL;
        out->as.call_.is_poly_call = false;
        out->as.call_.poly_arg_mask = 0;
        return out;
    }
    
    if (fn_type.kind != TY_FN && fn_type.kind != TY_CONT) {
        diag_emit(DIAG_ERROR, call->span,
                  "'%s' is not a function or continuation", fn_binding->name->name);
        return NULL;
    }

    if (fn_type.kind == TY_FN &&
        e->unsafe_depth == 0 &&
        effect_row_contains_symbol(fn_type.as.fn.effect_row, e->sym_effect_unsafe)) {
        diag_emit(DIAG_ERROR, call->span,
                  "unsafe function '%s' requires an enclosing (unsafe ...)",
                  fn_binding->name->name);
        return NULL;
    }

    uint8_t expected_arity = 0;
    if (fn_type.kind == TY_FN) {
        expected_arity = fn_type.as.fn.arity;
        
        /* For closure bindings, the thunk function has an extra env parameter */
        if (fn_binding->closure_fn_binding) {
            expected_arity--;  /* Subtract the hidden env parameter */
        }
    } else if (fn_type.kind == TY_CONT) {
        /* Continuations are callable with exactly 1 argument (the resume value) */
        expected_arity = 1;
    }
    
    if (n_args < expected_arity && fn_type.kind == TY_FN) {
        /* CY1: Partial application */
        Expr **pap_elab_args = (n_args > 0)
            ? (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *))
            : NULL;
        for (uint32_t i = 0; i < n_args; i++) {
            pap_elab_args[i] = elab_form(e, call->as.list.items[1 + i]);
            if (!pap_elab_args[i]) return NULL;
        }
        return elab_partial_apply(e, call, fn_binding, fn_type, pap_elab_args, n_args);
    }
    if (n_args > expected_arity && fn_type.kind == TY_FN) {
        /* CY2: Over-application */
        TypeKind result_kind = fn_type.as.fn.result_kind;
        if (result_kind != TY_PTR_VOID) {
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0002_ARITY_MISMATCH,
                                "function '%s' returns %s, which is not callable -- "
                                "did you mean to pass all %u argument(s)?",
                                fn_binding->name->name,
                                type_name(type_from_kind(result_kind)),
                                expected_arity);
            return NULL;
        }
        /* Elaborate the first expected_arity args (used in inner call) */
        Expr **inner_args = (Expr **)arena_alloc(e->arena, expected_arity * sizeof(Expr *));
        for (uint32_t i = 0; i < expected_arity; i++) {
            inner_args[i] = elab_form(e, call->as.list.items[1 + i]);
            if (!inner_args[i]) return NULL;
        }
        /* Build inner call expression */
        Type inner_result_type = type_from_kind(result_kind);
        Expr *inner_call = expr_new(e->arena, EX_CALL, inner_result_type, call->span);
        inner_call->as.call_.fn_binding = fn_binding;
        inner_call->as.call_.args = inner_args;
        inner_call->as.call_.n_args = expected_arity;
        inner_call->as.call_.fn_expr = NULL;
        inner_call->as.call_.dict_arg = NULL;
        inner_call->as.call_.is_poly_call = false;
        inner_call->as.call_.poly_arg_mask = 0;
        /* Create a let-binding for the intermediate closure result */
        char oar_name[32];
        snprintf(oar_name, sizeof(oar_name), "__oar%u", e->next_id++);
        const Symbol *oar_sym = symtab_intern(e->st, strslice(oar_name, (uint32_t)strlen(oar_name)));
        Binding *oar_binding = binding_new(e, oar_sym, inner_result_type, false, false, call->span);
        /* Set closure_fn_binding if the inner result is a closure */
        /* (We don't know at this point, but EX_CLOSURE wrapping in emit handles it dynamically) */
        /* Elaborate remaining args */
        uint32_t n_outer = n_args - expected_arity;
        Expr **outer_args = (Expr **)arena_alloc(e->arena, n_outer * sizeof(Expr *));
        for (uint32_t i = 0; i < n_outer; i++) {
            outer_args[i] = elab_form(e, call->as.list.items[1 + expected_arity + i]);
            if (!outer_args[i]) return NULL;
        }
        /* Result type of outer call: TY_INT as default for opaque fat closures */
        Type outer_result_type = TYPE_INT;
        if (fn_type.as.fn.result_full_type &&
            fn_type.as.fn.result_full_type->kind == TY_FN) {
            outer_result_type = type_from_kind(fn_type.as.fn.result_full_type->as.fn.result_kind);
        }
        Expr *outer_call = expr_new(e->arena, EX_CALL, outer_result_type, call->span);
        outer_call->as.call_.fn_binding = oar_binding;
        outer_call->as.call_.args = outer_args;
        outer_call->as.call_.n_args = n_outer;
        outer_call->as.call_.fn_expr = NULL;
        outer_call->as.call_.dict_arg = NULL;
        outer_call->as.call_.is_poly_call = false;
        outer_call->as.call_.poly_arg_mask = 0;
        /* Wrap in EX_LET */
        LetBinding *oar_let_bs = (LetBinding *)arena_alloc(e->arena, sizeof(LetBinding));
        oar_let_bs->binding = oar_binding;
        oar_let_bs->init = inner_call;
        Expr *oar_let = expr_new(e->arena, EX_LET, outer_result_type, call->span);
        oar_let->as.let_.bindings = oar_let_bs;
        oar_let->as.let_.n = 1;
        oar_let->as.let_.body = outer_call;
        return oar_let;
    }
    if (n_args != expected_arity) {
        /* Phase 8: Enhanced arity mismatch diagnostic with error code */
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0002_ARITY_MISMATCH,
                            "function '%s' expects %u argument(s), got %u",
                            fn_binding->name->name, expected_arity, n_args);
        return NULL;
    }

    /* Elaborate arguments */
    Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!args[i]) return NULL;
        TypeKind expected_arg_kind = TY_INT;
        if (fn_type.kind == TY_FN) {
            uint32_t fn_arg_idx = i;
            if (fn_binding->closure_fn_binding) {
                /* Closure thunk arg[0] is hidden env ptr. */
                fn_arg_idx = i + 1;
            }
            expected_arg_kind = fn_type.as.fn.arg_kinds[fn_arg_idx];
        }

        /* Phase HRT1: Detect rank-2 poly param and wrap arg in EX_POLY_WRAP.
         * arg_full_types[fn_arg_idx] is TY_FORALL → this is a rank-2 param. */
        bool is_rank2_param = false;
        if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
            uint32_t fn_arg_idx2 = i;
            if (fn_binding->closure_fn_binding) fn_arg_idx2 = i + 1;
            if (fn_arg_idx2 < fn_type.as.fn.arity) {
                Type *aft = fn_type.as.fn.arg_full_types[fn_arg_idx2];
                if (aft && (aft->kind == TY_FORALL || aft->kind == TY_EXISTS)) {
                    is_rank2_param = true;
                }
            }
        }

        bool arg_ok = (args[i]->type.kind == expected_arg_kind);
        /* Phase HRT/G2: A TY_TYVAR parameter (named type variable like :a) accepts any argument.
         * The concrete type is resolved per-arm inside a GADT match. */
        if (!arg_ok && expected_arg_kind == TY_TYVAR) {
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_PTR_VOID && args[i]->type.kind == TY_FN) {
            /* Allow passing a function value where callback pointer is expected.
             * If this is a rank-2 param, we'll wrap it in a poly wrapper below. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_PTR_VOID &&
                (args[i]->type.kind == TY_FORALL || args[i]->type.kind == TY_EXISTS)) {
            /* Allow passing an ascribed poly type (from ::) where rank-2 is expected. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_PTR_VOID && args[i]->type.kind == TY_NIL) {
            /* Allow nil as a null pointer for ptr<void> parameters. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_STRUCT) {
            /* Phase HKT H3: Allow passing an HKT container (TY_STRUCT) where int64_t
             * is expected.  HKT type constructor values are opaque int64_t at runtime. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_ADT) {
            /* Phase G0: ADT values are heap-allocated and passed as int64_t pointers.
             * Allow passing a TY_ADT where int64_t is expected. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_APP) {
            /* Phase HKT §3: Allow passing a partially-applied type (TY_APP) where int64_t
             * is expected.  Partial type application values are opaque int64_t at runtime. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_APP && args[i]->type.kind == TY_ADT) {
            /* Phase HKT/G4: Allow passing a TY_ADT where TY_APP is expected.
             * Both lower to int64_t at runtime.  This arises when a function
             * parameter is annotated with a parameterised type like (Equal a b)
             * (which parses as TY_APP) and the caller passes a GADT constructor
             * value such as Refl (which has type TY_ADT). */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_APP && args[i]->type.kind == TY_TYVAR) {
            /* Phase HKT/G4: Allow passing a TY_TYVAR where TY_APP is expected.
             * Type variables and applied types share int64_t representation. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_FN) {
            /* Phase HKT H3/H4: Allow passing a function value where int64_t is expected.
             * Function references are represented as int64_t in HKT helper calls. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_PTR_VOID) {
            /* Phase HKT §5: Allow passing a capturing closure (heap-allocated env struct,
             * TY_PTR_VOID) where int64_t is expected.  emit.c will apply the
             * (int64_t)(intptr_t) cast so the generated C99 code is valid. */
            arg_ok = true;
        }
        /* IT4: Union type subtyping — accept a value of type A where (A | B) is expected.
         * Wrap the argument with EX_UNION_INJECT so emit.c produces TUR_TAG(idx, val). */
        if (!arg_ok && g_union_types_enabled && expected_arg_kind == TY_UNION) {
            uint32_t fn_arg_idx3 = i;
            if (fn_binding->closure_fn_binding) fn_arg_idx3 = i + 1;
            if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types &&
                fn_arg_idx3 < fn_type.as.fn.arity) {
                Type *union_t = fn_type.as.fn.arg_full_types[fn_arg_idx3];
                if (union_t && union_t->kind == TY_UNION) {
                    for (uint8_t um = 0; um < union_t->as.union_.n_members; um++) {
                        Type *mem = union_t->as.union_.members[um];
                        if (mem && type_eq(args[i]->type, *mem)) {
                            arg_ok = true;
                            /* IT4: wrap in EX_UNION_INJECT to tag the value at runtime */
                            Expr *inject = expr_new(e->arena, EX_UNION_INJECT,
                                                    *union_t, args[i]->span);
                            inject->as.union_inject_.tag_idx = (int64_t)um;
                            inject->as.union_inject_.value = args[i];
                            args[i] = inject;
                            break;
                        }
                    }
                    /* Also accept if the argument is already the same union type (no injection needed) */
                    if (!arg_ok && args[i]->type.kind == TY_UNION) {
                        arg_ok = type_eq(args[i]->type, *union_t);
                    }
                }
            }
        }
        /* IT1: Widening — accept a member type where the expected union matches. */
        if (!arg_ok && g_union_types_enabled && args[i]->type.kind == TY_UNION &&
            expected_arg_kind == TY_UNION) {
            arg_ok = (args[i]->type.kind == expected_arg_kind);
        }
        /* IT3: Intersection elimination — accept a value of intersection type (A & B)
         * where any single member type is expected.  (A & B) <: A and (A & B) <: B. */
        if (!arg_ok && g_intersection_types_enabled &&
            args[i]->type.kind == TY_INTERSECTION) {
            Type *isect_t = &args[i]->type;
            for (uint8_t im = 0; im < isect_t->as.intersection_.n_members; im++) {
                Type *mem = isect_t->as.intersection_.members[im];
                if (mem && mem->kind == expected_arg_kind) {
                    arg_ok = true;
                    break;
                }
            }
        }
        /* IT3: Intersection introduction check — function expects (A & B), arg must
         * satisfy all members.  Emit TUR_E0351 for the first unsatisfied member. */
        if (!arg_ok && g_intersection_types_enabled && expected_arg_kind == TY_INTERSECTION) {
            uint32_t fn_arg_idx5 = fn_binding->closure_fn_binding ? i + 1 : i;
            if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types &&
                fn_arg_idx5 < fn_type.as.fn.arity) {
                Type *isect_t = fn_type.as.fn.arg_full_types[fn_arg_idx5];
                if (isect_t && isect_t->kind == TY_INTERSECTION) {
                    /* Check each member -- the arg must match all of them */
                    bool all_ok = true;
                    const char *first_mismatch = NULL;
                    for (uint8_t im = 0; im < isect_t->as.intersection_.n_members; im++) {
                        Type *mem = isect_t->as.intersection_.members[im];
                        if (!mem) continue;
                        /* For concrete member types, the arg must have an equal or
                         * compatible type.  For typeclass members we cannot yet do
                         * instance resolution here, so skip them. */
                        if (!typekind_is_concrete_for_disjoint(mem->kind)) continue;
                        if (!type_eq(args[i]->type, *mem)) {
                            all_ok = false;
                            first_mismatch = type_name(*mem);
                            break;
                        }
                    }
                    if (all_ok) {
                        arg_ok = true;
                    } else {
                        diag_emit_with_code(DIAG_ERROR, args[i]->span,
                            TUR_E0351_INTERSECTION_MEMBER_MISMATCH,
                            "function '%s' arg %u: value of type %s does not satisfy "
                            "intersection member %s",
                            fn_binding->name->name, i + 1,
                            type_name(args[i]->type),
                            first_mismatch ? first_mismatch : "?");
                        return NULL;
                    }
                }
            }
        }

        /* IT4: A <: any — any value satisfies the top type.
         * Wrap with EX_UNION_INJECT using the TypeKind of the value as the tag,
         * so (type-of) and (cast) can retrieve it at runtime. */
        if (!arg_ok && (g_union_types_enabled || g_intersection_types_enabled) &&
            expected_arg_kind == TY_ANY) {
            arg_ok = true;
            Type any_type; memset(&any_type, 0, sizeof(any_type)); any_type.kind = TY_ANY;
            Expr *inject = expr_new(e->arena, EX_UNION_INJECT, any_type, args[i]->span);
            inject->as.union_inject_.tag_idx = (int64_t)args[i]->type.kind;
            inject->as.union_inject_.value = args[i];
            args[i] = inject;
        }

        /* LT2: When both expected and actual argument types are function types,
         * verify that their arg_linear flags match.  This catches attempts to
         * pass a (-> T R) function where (-> ^linear T R) is required (or vice
         * versa) in higher-order call positions. */
        if (arg_ok && g_linear_enabled &&
            expected_arg_kind == TY_FN && args[i]->type.kind == TY_FN) {
            uint32_t fn_arg_idx_lt2 = fn_binding->closure_fn_binding ? i + 1 : i;
            if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types &&
                fn_arg_idx_lt2 < fn_type.as.fn.arity) {
                const Type *expected_fn = fn_type.as.fn.arg_full_types[fn_arg_idx_lt2];
                if (expected_fn && expected_fn->kind == TY_FN &&
                    !fn_type_subtype(args[i]->type, *expected_fn)) {
                    diag_emit_with_code(DIAG_ERROR, args[i]->span,
                                        TUR_E0001_TYPE_MISMATCH,
                                        "function '%s' arg %u: linear function type mismatch"
                                        " -- expected %s but got a function with different"
                                        " linearity annotations; ^linear parameters must match exactly",
                                        fn_binding->name->name, i + 1,
                                        type_name(*expected_fn));
                    return NULL;
                }
            }
        }

        if (!arg_ok) {
            /* Phase 8: Enhanced type mismatch with error code */
            /* IT1: Use union-specific error code when union type is involved */
            DiagCode err_code = TUR_E0001_TYPE_MISMATCH;
            if (g_union_types_enabled && (expected_arg_kind == TY_UNION ||
                                           args[i]->type.kind == TY_UNION)) {
                err_code = TUR_E0300_UNION_TYPE_MISMATCH;
            }
            /* Compute expected type name for diagnostic.
             * For compound types (union, intersection) that store their full type in
             * arg_full_types, look it up there so the name includes member types. */
            const char *expected_str;
            if ((expected_arg_kind == TY_UNION || expected_arg_kind == TY_INTERSECTION) &&
                fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
                uint32_t fn_arg_idx4 = fn_binding->closure_fn_binding ? i + 1 : i;
                Type *ct = (fn_arg_idx4 < fn_type.as.fn.arity)
                    ? fn_type.as.fn.arg_full_types[fn_arg_idx4] : NULL;
                expected_str = ct ? type_name(*ct)
                                  : type_name(type_from_kind(expected_arg_kind));
            } else {
                expected_str = type_name(type_from_kind(expected_arg_kind));
            }
            diag_emit_with_code(DIAG_ERROR, args[i]->span, err_code,
                                "function '%s' arg %u: expected %s, got %s",
                                fn_binding->name->name, i + 1,
                                expected_str,
                                type_name(args[i]->type));
            return NULL;
        }

        /* Phase HRT1/HRT4: wrap rank-2 args with EX_POLY_WRAP + create wrapper thunk.
         * Phase HRT4: also handles TY_PTR_VOID is_poly_fn bindings (pass-through). */
        if (is_rank2_param && (args[i]->type.kind == TY_FN ||
                                args[i]->type.kind == TY_FORALL ||
                                args[i]->type.kind == TY_EXISTS ||
                                args[i]->type.kind == TY_PTR_VOID)) {
            Binding *inner_fn_b = poly_arg_fn_binding(args[i]);
            if (!inner_fn_b) {
                diag_emit(DIAG_ERROR, args[i]->span,
                          "rank-2 argument must be a named function (capturing closures not yet supported)");
                return NULL;
            }
            Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, args[i]->span);
            wrap->as.poly_wrap_.inner = args[i];
            if (inner_fn_b->is_poly_fn) {
                /* HRT4: pass-through — binding is already a tur_poly_fn_t, no wrapper needed. */
                wrap->as.poly_wrap_.wrapper_binding = NULL;
            } else {
                uint8_t inner_arity = (inner_fn_b->type.kind == TY_FN)
                    ? inner_fn_b->type.as.fn.arity : 1;
                /* Closures have an env param counted in arity — subtract it */
                if (inner_fn_b->closure_fn_binding) inner_arity--;
                Binding *wrapper_b = make_poly_wrapper(e, inner_fn_b, inner_arity, args[i]->span);
                if (!wrapper_b) return NULL;
                wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
            }
            args[i] = wrap;
        }

        /* UT1: TUR_E0200 -- reject aliased value passed to ^unique parameter */
        if (g_unique_enabled && fn_type.kind == TY_FN &&
            i < fn_type.as.fn.arity && fn_type.as.fn.arg_unique[i] &&
            args[i]->kind == EX_VAR) {
            Binding *arg_b = args[i]->as.var.binding;
            if (arg_b->alias_state == AS_ALIASED) {
                if (arg_b->alias_name) {
                    diag_emit_with_code(DIAG_ERROR, args[i]->span,
                                        TUR_E0200_UNIQUE_ALIASED,
                                        "value '%s' is not unique -- aliased by '%s'",
                                        arg_b->name->name, arg_b->alias_name->name);
                } else {
                    diag_emit_with_code(DIAG_ERROR, args[i]->span,
                                        TUR_E0200_UNIQUE_ALIASED,
                                        "value '%s' is not unique -- it has been aliased",
                                        arg_b->name->name);
                }
                return NULL;
            }
        }

        /* UT2: Reject argument passed to ^unique ^mut param when active borrows exist.
         * A ^unique ^mut parameter requires exclusive mutable access; any live borrow
         * (&T or &mut T) on the same binding would violate that guarantee. */
        if (g_unique_enabled && fn_type.kind == TY_FN &&
            i < fn_type.as.fn.arity && fn_type.as.fn.arg_unique_mut[i] &&
            args[i]->kind == EX_VAR) {
            Binding *arg_b = args[i]->as.var.binding;
            if (scope_borrow_conflicts(e->scope, arg_b, BK_MUT)) {
                diag_emit_with_code(DIAG_ERROR, args[i]->span,
                                    TUR_E0200_UNIQUE_ALIASED,
                                    "cannot pass '%s' as ^unique ^mut -- active borrow exists",
                                    arg_b->name->name);
                return NULL;
            }
        }

        /* Phase 11: Move tracking - if arg is a CK_MOVE binding reference, poison it.
         * UT2 semantics:
         *  - ^unique ^mut param: exclusive mutable ACCESS (borrow-like); caller keeps
         *    ownership -- do NOT mark arg as moved, regardless of arg's own flags.
         *  - ^unique param (no ^mut): OWNERSHIP TRANSFER; mark arg as moved so it
         *    can't be used again.
         *  - ordinary param with a ^unique ^mut arg binding: also do not consume,
         *    since the binding is a mutable unique cell that may be freely read. */
        if (args[i]->kind == EX_VAR && type_is_move(args[i]->as.var.binding->type)) {
            Binding *arg_b2 = args[i]->as.var.binding;
            bool param_is_unique_mut = g_unique_enabled && fn_type.kind == TY_FN &&
                i < fn_type.as.fn.arity && fn_type.as.fn.arg_unique_mut[i];
            bool arg_is_unique_mut = g_unique_enabled && arg_b2->is_unique && arg_b2->is_mut;
            if (!param_is_unique_mut && !arg_is_unique_mut) {
                binding_mark_moved(arg_b2, args[i]->span);
            }
        }
    }

    /* Result type is the function's return type */
    Type result_type;
    if (fn_type.kind == TY_FN) {
        TypeKind result_kind = fn_type.as.fn.result_kind;
        result_type = type_from_kind(result_kind);
    } else if (fn_type.kind == TY_CONT) {
        /* Calling a continuation returns its result type (though in practice it jumps) */
        result_type = type_from_kind(fn_type.as.cont.returns);
    } else {
        result_type = TYPE_NIL;
    }

    Expr *out = expr_new(e->arena, EX_CALL, result_type, call->span);
    out->as.call_.fn_binding = fn_binding;
    out->as.call_.args = args;
    out->as.call_.n_args = n_args;
    out->as.call_.fn_expr = NULL;
    return out;
}

/* Phase M0: Module system */

/* Validate that a module name only contains [a-zA-Z0-9_\-/] */
static bool module_name_valid(const char *name, uint32_t len) {
    if (len == 0) return false;
    for (uint32_t i = 0; i < len; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '/') {
            continue;
        }
        return false;
    }
    /* Must not start or end with '/' */
    if (name[0] == '/' || name[len - 1] == '/') return false;
    return true;
}

/* Phase M: (load "path") — source-file inclusion form.
 * Reads the file at the given path, elaborates all its top-level forms into
 * the current module's scope, and returns nil.  Uses the loaded_modules
 * registry (keyed by interned path) to prevent duplicate loads.
 *
 * Syntax: (load "relative/or/absolute/path.tur")
 */
/* Phase M: (load "path") — handled by load-expansion preprocessor in
 * elaborate_program before the two-pass elab.  This dispatch path should be
 * unreachable for top-level loads; we return nil to allow stray (load ...)
 * inside e.g. (do ...) blocks to no-op (loaded forms are already expanded
 * at top level by then).  See expand_loads_in_forms. */
static Expr *elab_load(Elab *e, const Form *call) {
    (void)call;
    return expr_new(e->arena, EX_NIL_LIT, TYPE_NIL, call->span);
}

/* Phase M2: Load and elaborate an imported module file.
 * Returns the registry entry (may have 0 exports on parse/elab failure).
 * Returns NULL only on fatal error (circular import or OOM). */
static ElabModule *elab_load_module(Elab *e, const Symbol *name, Span import_span) {
    /* Already in registry? */
    ElabModule *existing = elab_find_loaded_module(e, name);
    if (existing) {
        if (existing->is_loading) {
            diag_emit(DIAG_ERROR, import_span,
                      "circular import: module '%s' is already being loaded", name->name);
            return NULL;
        }
        return existing;
    }

    /* Reserve a registry slot first (for circular-import detection). */
    if (e->n_loaded_modules >= e->cap_loaded_modules) {
        e->cap_loaded_modules = e->cap_loaded_modules ? e->cap_loaded_modules * 2 : 8;
        e->loaded_modules = (ElabModule *)realloc(e->loaded_modules,
                             e->cap_loaded_modules * sizeof(ElabModule));
        if (!e->loaded_modules) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    ElabModule *slot = &e->loaded_modules[e->n_loaded_modules++];
    slot->name = name;
    slot->exports = NULL;
    slot->n_exports = 0;
    slot->is_loading = true;

    /* Build file path: 'geom/vector' -> '{base_dir}/geom/vector.tur'
     * The '/' in module names maps directly to directory separators. */
    char path_buf[4096];
    const char *base = e->module_base_dir ? e->module_base_dir : ".";
    int plen = snprintf(path_buf, sizeof(path_buf), "%s/%s.tur", base, name->name);
    if (plen < 0 || (size_t)plen >= sizeof(path_buf)) {
        diag_emit(DIAG_ERROR, import_span,
                  "module path too long for '%s'", name->name);
        slot->is_loading = false;
        return NULL;
    }

    /* Read source file. */
    char *src_raw = NULL;
    size_t src_len = 0;
    if (elab_read_file(path_buf, &src_raw, &src_len) != 0) {
        /* Fallback: try the stdlib directory (e.g. for `import turi/eval`). */
        bool found_in_stdlib = false;
        if (e->module_stdlib_dir) {
            char stdlib_path[4096];
            int splen = snprintf(stdlib_path, sizeof(stdlib_path), "%s/%s.tur",
                                 e->module_stdlib_dir, name->name);
            if (splen > 0 && (size_t)splen < sizeof(stdlib_path)) {
                if (elab_read_file(stdlib_path, &src_raw, &src_len) == 0) {
                    memcpy(path_buf, stdlib_path, (size_t)splen + 1);
                    plen = splen;
                    found_in_stdlib = true;
                }
            }
        }
        if (!found_in_stdlib) {
            /* Fallback: try each -I include directory (spice paths). */
            bool found_in_includes = false;
            for (int ii = 0; ii < e->n_module_include_dirs && !found_in_includes; ii++) {
                char inc_path[4096];
                int iplen = snprintf(inc_path, sizeof(inc_path), "%s/%s.tur",
                                     e->module_include_dirs[ii], name->name);
                if (iplen > 0 && (size_t)iplen < sizeof(inc_path)) {
                    if (elab_read_file(inc_path, &src_raw, &src_len) == 0) {
                        memcpy(path_buf, inc_path, (size_t)iplen + 1);
                        plen = iplen;
                        found_in_includes = true;
                    }
                }
            }
            if (!found_in_includes) {
                diag_emit(DIAG_ERROR, import_span,
                          "module '%s' not found (looked for '%s')", name->name, path_buf);
                slot->is_loading = false;
                return NULL;
            }
        }
    }

    /* Copy source into the arena so it outlives the load. */
    char *src_copy = (char *)arena_alloc(e->arena, src_len + 1);
    memcpy(src_copy, src_raw, src_len);
    src_copy[src_len] = '\0';
    free(src_raw);

    /* Register the source file for diagnostics.  Path must also live in arena. */
    char *path_copy = (char *)arena_alloc(e->arena, (size_t)plen + 1);
    memcpy(path_copy, path_buf, (size_t)plen + 1);

    SourceFile *sfile = (SourceFile *)arena_alloc(e->arena, sizeof(SourceFile));
    *sfile = (SourceFile){0};
    sfile->path = path_copy;
    sfile->src = src_copy;
    sfile->len = src_len;
    sfile->file_id = e->next_import_file_id++;
    sfile->reader_type = READER_TURMERIC;
    diag_register_file(sfile);

    /* Parse the source into forms. */
    uint32_t nforms = 0;
    Form **forms = read_all(e->arena, e->st, sfile, &nforms);
    if (!forms) {
        slot->is_loading = false;
        return NULL;
    }

    /* Elaborate the imported module's forms.
     * Save and restore fields that track the current defmodule context so the
     * outer caller's state is not corrupted. */
    bool         saved_has_defmodule   = e->has_defmodule;
    const Symbol *saved_module_name    = e->current_module_name;
    const DefModule *saved_module      = e->current_module;
    e->has_defmodule       = false;
    e->current_module_name = NULL;
    e->current_module      = NULL;

    /* Phase M4: capture the DefModule so we can check its export list for macros. */
    const DefModule *loaded_defmod = NULL;

    for (uint32_t i = 0; i < nforms; i++) {
        Expr *ex = elab_form(e, forms[i]);
        if (!ex) {
            e->has_defmodule       = saved_has_defmodule;
            e->current_module_name = saved_module_name;
            e->current_module      = saved_module;
            slot->is_loading = false;
            return NULL;
        }
        /* Register EX_DEFMODULE into file_scope_defs so its body gets emitted.
         * emit.c's flatten_program_items expands these into top-level C items.
         * Phase M3: Skip inlining when compiling each module separately; the
         * implementation file #includes the imported module's header instead. */
        if (ex->kind == EX_DEFMODULE) {
            loaded_defmod = ex->as.defmodule_.mod; /* Phase M4 */
            if (!e->separate_compilation) elab_register_file_def(e, ex);
        }
    }

    e->has_defmodule       = saved_has_defmodule;
    e->current_module_name = saved_module_name;
    e->current_module      = saved_module;

    /* Collect exported bindings: those in the global scope owned by this module. */
    uint32_t n_exp = 0;
    for (uint32_t i = 0; i < e->global.n; i++) {
        Binding *b = e->global.bindings[i];
        if (b->defining_module_name == name && b->is_exported) n_exp++;
    }
    Binding **exp_arr = (n_exp == 0) ? NULL :
        (Binding **)arena_alloc(e->arena, n_exp * sizeof(Binding *));
    uint32_t idx = 0;
    for (uint32_t i = 0; i < e->global.n; i++) {
        Binding *b = e->global.bindings[i];
        if (b->defining_module_name == name && b->is_exported)
            exp_arr[idx++] = b;
    }

    slot->exports   = exp_arr;
    slot->n_exports = n_exp;

    /* Phase M4: Collect exported macros from this module.
     * A macro is exported if its defining_module_name matches this module's name
     * AND its name appears in the module's (export ...) list. */
    slot->exported_macros   = NULL;
    slot->n_exported_macros = 0;
    if (loaded_defmod != NULL && loaded_defmod->n_exports > 0) {
        uint32_t n_mexp = 0;
        for (uint32_t i = 0; i < e->n_macros; i++) {
            MacroDef *m = e->macros[i];
            if (m->defining_module_name != name) continue;
            /* Check if this macro's name is in the export list */
            for (uint32_t j = 0; j < loaded_defmod->n_exports; j++) {
                if (loaded_defmod->exports[j] == m->name) { n_mexp++; break; }
            }
        }
        if (n_mexp > 0) {
            struct MacroDef **mexp_arr =
                (struct MacroDef **)arena_alloc(e->arena, n_mexp * sizeof(struct MacroDef *));
            uint32_t midx = 0;
            for (uint32_t i = 0; i < e->n_macros; i++) {
                MacroDef *m = e->macros[i];
                if (m->defining_module_name != name) continue;
                for (uint32_t j = 0; j < loaded_defmod->n_exports; j++) {
                    if (loaded_defmod->exports[j] == m->name) {
                        mexp_arr[midx++] = m; break;
                    }
                }
            }
            slot->exported_macros   = mexp_arr;
            slot->n_exported_macros = n_mexp;
        }
    }

    /* PR5-3-D: Collect exported effects for this module. */
    {
        uint32_t n_eeff = 0;
        for (uint32_t i = 0; i < e->effect_env->n_effects; i++) {
            Effect *eff = e->effect_env->effects[i];
            if (eff->defining_module_name == name && eff->is_exported) n_eeff++;
        }
        Effect **eeff_arr = (n_eeff == 0) ? NULL :
            (Effect **)arena_alloc(e->arena, n_eeff * sizeof(Effect *));
        uint32_t eidx = 0;
        for (uint32_t i = 0; i < e->effect_env->n_effects; i++) {
            Effect *eff = e->effect_env->effects[i];
            if (eff->defining_module_name == name && eff->is_exported)
                eeff_arr[eidx++] = eff;
        }
        slot->exported_effects   = eeff_arr;
        slot->n_exported_effects = n_eeff;
    }

    slot->is_loading = false;
    return slot;
}

/* Parse a single (import module-name [:as alias] [:refer [syms...]]) form */
static bool parse_import_spec(Elab *e, const Form *f, ImportSpec *out) {
    if (f->tag != F_LIST || f->as.list.len < 2) {
        diag_emit(DIAG_ERROR, f->span,
                  "import requires a module name: (import module-name [:as alias] [:refer [syms...]])");
        return false;
    }
    Form *head = f->as.list.items[0];
    if (head->tag != F_SYM || head->as.sym != e->sym_import) {
        diag_emit(DIAG_ERROR, f->span, "expected (import ...)");
        return false;
    }
    Form *name_f = f->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "import module name must be a symbol");
        return false;
    }
    if (!module_name_valid(name_f->as.sym->name, name_f->as.sym->len)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "invalid module name '%s': only alphanumeric, '-', '_', '/' allowed",
                  name_f->as.sym->name);
        return false;
    }
    out->module_name = name_f->as.sym;
    out->alias = NULL;
    out->refer_syms = NULL;
    out->n_refer = 0;
    out->refer_effect_syms = NULL;
    out->n_refer_effects   = 0;
    out->span = f->span;

    uint32_t i = 2;
    while (i < f->as.list.len) {
        Form *kw = f->as.list.items[i];
        if (kw->tag == F_KEYWORD && kw->as.sym == e->kw_as) {
            i++;
            if (i >= f->as.list.len) {
                diag_emit(DIAG_ERROR, kw->span, ":as requires an alias symbol");
                return false;
            }
            Form *alias_f = f->as.list.items[i];
            if (alias_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, alias_f->span, ":as alias must be a symbol");
                return false;
            }
            out->alias = alias_f->as.sym;
            i++;
        } else if (kw->tag == F_KEYWORD && kw->as.sym == e->kw_refer) {
            i++;
            if (i >= f->as.list.len) {
                diag_emit(DIAG_ERROR, kw->span, ":refer requires a vector of symbols");
                return false;
            }
            Form *refer_f = f->as.list.items[i];
            if (refer_f->tag != F_VEC) {
                diag_emit(DIAG_ERROR, refer_f->span, ":refer requires a vector [sym1 sym2 ...]");
                return false;
            }
            uint32_t n = refer_f->as.list.len;
            /* Count plain symbols vs (effect Name) entries. */
            uint32_t n_syms = 0, n_effs = 0;
            for (uint32_t j = 0; j < n; j++) {
                Form *sf = refer_f->as.list.items[j];
                if (sf->tag == F_SYM) {
                    n_syms++;
                } else if (sf->tag == F_LIST && sf->as.list.len == 2
                           && sf->as.list.items[0]->tag == F_SYM
                           && sf->as.list.items[0]->as.sym == e->sym_effect) {
                    n_effs++;
                } else {
                    diag_emit(DIAG_ERROR, sf->span,
                              ":refer list must contain symbols or (effect Name) forms");
                    return false;
                }
            }
            const Symbol **syms = (n_syms == 0) ? NULL :
                (const Symbol **)arena_alloc(e->arena, n_syms * sizeof(Symbol *));
            const Symbol **esyms = (n_effs == 0) ? NULL :
                (const Symbol **)arena_alloc(e->arena, n_effs * sizeof(Symbol *));
            uint32_t si = 0, ei = 0;
            for (uint32_t j = 0; j < n; j++) {
                Form *sf = refer_f->as.list.items[j];
                if (sf->tag == F_SYM) {
                    syms[si++] = sf->as.sym;
                } else {
                    /* (effect Name) -- already validated above */
                    Form *en = sf->as.list.items[1];
                    if (en->tag != F_SYM) {
                        diag_emit(DIAG_ERROR, en->span, "effect name in :refer must be a symbol");
                        return false;
                    }
                    esyms[ei++] = en->as.sym;
                }
            }
            out->refer_syms        = syms;
            out->n_refer           = n_syms;
            out->refer_effect_syms = esyms;
            out->n_refer_effects   = n_effs;
            i++;
        } else {
            diag_emit(DIAG_ERROR, kw->span,
                      "unexpected token in import; expected :as or :refer");
            return false;
        }
    }
    return true;
}

static Expr *elab_defmodule(Elab *e, const Form *call) {
    /* Only valid at the top level */
    if (e->scope != &e->global) {
        diag_emit(DIAG_ERROR, call->span, "defmodule is only valid at the top level");
        return NULL;
    }
    /* Syntax: (defmodule name [docstring] (export ...) (import ...)... body...) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "defmodule requires a module name: (defmodule name ...)");
        return NULL;
    }
    Form *name_f = call->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "defmodule name must be a symbol");
        return NULL;
    }
    if (!module_name_valid(name_f->as.sym->name, name_f->as.sym->len)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "invalid module name '%s': only alphanumeric, '-', '_', '/' allowed",
                  name_f->as.sym->name);
        return NULL;
    }
    if (e->has_defmodule) {
        diag_emit(DIAG_ERROR, call->span, "only one defmodule is allowed per file");
        return NULL;
    }
    e->has_defmodule = true;

    uint32_t i = 2;
    const char *docstring = NULL;

    /* Optional docstring */
    if (i < call->as.list.len && call->as.list.items[i]->tag == F_STR) {
        docstring = call->as.list.items[i]->as.s.p;
        i++;
    }

    /* Collect export symbols */
    const Symbol **exports = NULL;
    uint32_t n_exports = 0;
    uint32_t cap_exports = 0;
    const Symbol **exp_effects = NULL;
    uint32_t n_exp_effects = 0;
    uint32_t cap_exp_effects = 0;

    /* Collect import specs */
    ImportSpec *imports = NULL;
    uint32_t n_imports = 0;
    uint32_t cap_imports = 0;

    /* Consume (export ...) and (import ...) forms */
    while (i < call->as.list.len) {
        Form *item = call->as.list.items[i];
        if (item->tag != F_LIST || item->as.list.len == 0) break;
        Form *head = item->as.list.items[0];
        if (head->tag != F_SYM) break;

        if (head->as.sym == e->sym_export) {
            /* Parse (export sym1 sym2 ... (effect Name) ...) */
            for (uint32_t j = 1; j < item->as.list.len; j++) {
                Form *sf = item->as.list.items[j];
                if (sf->tag == F_SYM) {
                    /* Regular symbol export */
                    if (n_exports >= cap_exports) {
                        cap_exports = cap_exports ? cap_exports * 2 : 4;
                        exports = (const Symbol **)realloc(exports, cap_exports * sizeof(Symbol *));
                    }
                    exports[n_exports++] = sf->as.sym;
                } else if (sf->tag == F_LIST && sf->as.list.len == 2
                           && sf->as.list.items[0]->tag == F_SYM
                           && sf->as.list.items[0]->as.sym == e->sym_effect) {
                    /* (effect Name) export */
                    Form *ename_f = sf->as.list.items[1];
                    if (ename_f->tag != F_SYM) {
                        diag_emit(DIAG_ERROR, ename_f->span,
                                  "effect name in export list must be a symbol");
                        free(exports); free(exp_effects); free(imports);
                        return NULL;
                    }
                    if (n_exp_effects >= cap_exp_effects) {
                        cap_exp_effects = cap_exp_effects ? cap_exp_effects * 2 : 4;
                        exp_effects = (const Symbol **)realloc(exp_effects, cap_exp_effects * sizeof(Symbol *));
                    }
                    exp_effects[n_exp_effects++] = ename_f->as.sym;
                } else {
                    diag_emit(DIAG_ERROR, sf->span,
                              "export list entries must be symbols or (effect Name) forms");
                    free(exports); free(exp_effects); free(imports);
                    return NULL;
                }
            }
            i++;
        } else if (head->as.sym == e->sym_import) {
            /* Parse (import module-name [:as alias] [:refer [syms...]]) */
            if (n_imports >= cap_imports) {
                cap_imports = cap_imports ? cap_imports * 2 : 4;
                imports = (ImportSpec *)realloc(imports, cap_imports * sizeof(ImportSpec));
            }
            if (!parse_import_spec(e, item, &imports[n_imports])) {
                free(exports); free(exp_effects); free(imports);
                return NULL;
            }
            n_imports++;
            i++;
        } else {
            break; /* Body starts here */
        }
    }

    uint32_t body_start = i;

    /* M1: Allocate module struct early and populate imports so elab_lookup_sym
     * can resolve qualified names and import aliases during body elaboration. */
    DefModule *mod = (DefModule *)arena_alloc(e->arena, sizeof(DefModule));
    memset(mod, 0, sizeof(DefModule));
    mod->name = name_f->as.sym;
    mod->docstring = docstring;

    /* Copy exports to arena (used for marking after pass 2) */
    if (n_exports > 0) {
        mod->exports = (const Symbol **)arena_alloc(e->arena, n_exports * sizeof(Symbol *));
        for (uint32_t j = 0; j < n_exports; j++) mod->exports[j] = exports[j];
        mod->n_exports = n_exports;
    }
    free(exports); exports = NULL;

    /* PR5-3-B: Copy exported_effects to arena */
    if (n_exp_effects > 0) {
        mod->exported_effects = (const Symbol **)arena_alloc(e->arena, n_exp_effects * sizeof(Symbol *));
        for (uint32_t j = 0; j < n_exp_effects; j++) mod->exported_effects[j] = exp_effects[j];
        mod->n_exported_effects = n_exp_effects;
    }
    free(exp_effects); exp_effects = NULL;

    /* Copy imports to arena (needed for alias resolution during elaboration) */
    if (n_imports > 0) {
        mod->imports = (ImportSpec *)arena_alloc(e->arena, n_imports * sizeof(ImportSpec));
        for (uint32_t j = 0; j < n_imports; j++) mod->imports[j] = imports[j];
        mod->n_imports = n_imports;
    }
    free(imports); imports = NULL;

    /* M1: Set module context — body bindings will inherit defining_module_name */
    e->current_module_name = mod->name;
    e->current_module = mod;

    /* M2: Process imports — load each referenced module and inject :refer symbols. */
    for (uint32_t j = 0; j < mod->n_imports; j++) {
        const ImportSpec *imp = &mod->imports[j];
        ElabModule *loaded = elab_load_module(e, imp->module_name, imp->span);
        if (!loaded) {
            e->current_module_name = NULL;
            e->current_module = NULL;
            return NULL;
        }
        /* :refer — add each referred symbol (binding or macro) to the current scope. */
        for (uint32_t k = 0; k < imp->n_refer; k++) {
            const Symbol *ref_sym = imp->refer_syms[k];

            /* First check bindings. */
            Binding *ref_b = NULL;
            for (uint32_t m = 0; m < loaded->n_exports; m++) {
                if (loaded->exports[m]->name == ref_sym) {
                    ref_b = loaded->exports[m];
                    break;
                }
            }
            if (ref_b) {
                scope_add(&e->global, ref_b);
                continue;
            }

            /* Phase M4: Check exported macros. */
            MacroDef *ref_macro = NULL;
            for (uint32_t m = 0; m < loaded->n_exported_macros; m++) {
                if (loaded->exported_macros[m]->name == ref_sym) {
                    ref_macro = loaded->exported_macros[m];
                    break;
                }
            }
            if (ref_macro) {
                /* Inject an alias that is visible everywhere via is_referred,
                 * but keeps defining_module_name so private helpers of the
                 * original module remain accessible during expansion. */
                MacroDef *alias = (MacroDef *)arena_alloc(e->arena, sizeof(MacroDef));
                *alias = *ref_macro;
                alias->is_referred = true;
                /* Check for name collision with existing global macros. */
                if (elab_lookup_macro(e, ref_sym) != NULL) {
                    diag_emit(DIAG_ERROR, imp->span,
                              "macro '%s' from module '%s' conflicts with an existing macro",
                              ref_sym->name, imp->module_name->name);
                    e->current_module_name = NULL;
                    e->current_module = NULL;
                    return NULL;
                }
                elab_register_macro(e, alias);
                continue;
            }

            diag_emit(DIAG_ERROR, imp->span,
                      "symbol '%s' is not exported from module '%s'",
                      ref_sym->name, imp->module_name->name);
            e->current_module_name = NULL;
            e->current_module = NULL;
            return NULL;
        }

        /* PR5-3-D: Process :refer [(effect Name)] imports. */
        for (uint32_t k = 0; k < imp->n_refer_effects; k++) {
            const Symbol *ref_eff_sym = imp->refer_effect_syms[k];
            Effect *ref_eff = NULL;
            for (uint32_t m = 0; m < loaded->n_exported_effects; m++) {
                if (loaded->exported_effects[m]->name == ref_eff_sym) {
                    ref_eff = loaded->exported_effects[m];
                    break;
                }
            }
            if (!ref_eff) {
                diag_emit(DIAG_ERROR, imp->span,
                          "effect '%s' is not exported from module '%s'",
                          ref_eff_sym->name, imp->module_name->name);
                e->current_module_name = NULL;
                e->current_module = NULL;
                return NULL;
            }
            /* Add to referred_effects so the visibility guard allows access. */
            if (e->n_referred_effects >= e->cap_referred_effects) {
                e->cap_referred_effects = e->cap_referred_effects ? e->cap_referred_effects * 2 : 4;
                e->referred_effects = (Effect **)realloc(e->referred_effects,
                                      e->cap_referred_effects * sizeof(Effect *));
            }
            e->referred_effects[e->n_referred_effects++] = ref_eff;
        }
    }

    /* Pass 1: forward-declare all defn bodies (for mutual recursion) */
    for (uint32_t j = body_start; j < call->as.list.len; j++) {
        Form *f = call->as.list.items[j];
        if (f->tag == F_LIST && f->as.list.len > 0) {
            Form *h = f->as.list.items[0];
            if (h->tag == F_SYM && h->as.sym == e->sym_defn) {
                if (f->as.list.len >= 3) {
                    uint32_t name_idx = 1;
                    if (f->as.list.items[1]->tag == F_SYM &&
                        f->as.list.items[1]->as.sym == e->sym_no_unwind_attr) {
                        name_idx = 2;
                    }
                    if ((uint32_t)f->as.list.len <= name_idx) continue;
                    Form *fn_name_f = f->as.list.items[name_idx];
                    if (fn_name_f->tag == F_SYM) {
                        /* Check not already defined */
                        if (!scope_lookup(&e->global, fn_name_f->as.sym)) {
                            TypeKind arg_kinds[1] = {TY_INT};
                            Type fn_type = type_fn(arg_kinds, 1, TY_INT);
                            Binding *b = binding_new(e, fn_name_f->as.sym, fn_type, false, true, f->span);
                            scope_add(&e->global, b);
                        }
                    }
                }
            }
        }
    }

    /* Pass 2: elaborate body forms */
    uint32_t n_body = call->as.list.len - body_start;
    Expr **body = (n_body == 0) ? NULL :
        (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
    uint32_t actual_n_body = 0;

    for (uint32_t j = body_start; j < call->as.list.len; j++) {
        Expr *be = elab_form(e, call->as.list.items[j]);
        if (!be) {
            e->current_module_name = NULL;
            e->current_module = NULL;
            return NULL;
        }
        body[actual_n_body++] = be;
    }

    mod->body = body;
    mod->n_body = actual_n_body;

    /* M1: Mark exported bindings and validate they exist in this module.
     * Phase M4: macro names in the export list are also valid exports. */
    for (uint32_t j = 0; j < mod->n_exports; j++) {
        Binding *exp_b = scope_lookup(&e->global, mod->exports[j]);
        if (!exp_b || exp_b->defining_module_name != mod->name) {
            /* Not a binding — check if it's a macro defined in this module. */
            bool found_macro = false;
            for (uint32_t k = 0; k < e->n_macros; k++) {
                if (e->macros[k]->name == mod->exports[j] &&
                    e->macros[k]->defining_module_name == mod->name) {
                    found_macro = true;
                    break;
                }
            }
            if (!found_macro) {
                diag_emit(DIAG_ERROR, call->span,
                          "exported symbol '%s' is not defined in this module",
                          mod->exports[j]->name);
                diag_emit(DIAG_NOTE, call->span,
                          "ensure '%s' has a matching (defn ...) or (defmacro ...) inside the (defmodule %s ...) body, "
                          "and check for typos in the (export ...) list",
                          mod->exports[j]->name, mod->name->name);
                e->current_module_name = NULL;
                e->current_module = NULL;
                return NULL;
            }
            /* Macro exports don't need is_exported flag; they're collected by elab_load_module. */
        } else {
            exp_b->is_exported = true;
        }
    }

    /* PR5-3-B: Validate and mark exported effects. */
    for (uint32_t j = 0; j < mod->n_exported_effects; j++) {
        const Symbol *eff_name = mod->exported_effects[j];
        Effect *eff = effect_env_lookup(e->effect_env, eff_name);
        if (!eff || eff->defining_module_name != mod->name) {
            diag_emit(DIAG_ERROR, call->span,
                      "exported effect '%s' is not defined in this module",
                      eff_name->name);
            e->current_module_name = NULL;
            e->current_module = NULL;
            return NULL;
        }
        if (eff->is_private) {
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0021_PRIVATE_EFFECT,
                                "private effect '%s' cannot be exported",
                                eff_name->name);
            e->current_module_name = NULL;
            e->current_module = NULL;
            return NULL;
        }
        eff->is_exported = true; /* explicit export — already true, but record intent */
    }

    /* Reset module context */
    e->current_module_name = NULL;
    e->current_module = NULL;

    Expr *out = expr_new(e->arena, EX_DEFMODULE, TYPE_NIL, call->span);
    out->as.defmodule_.mod = mod;
    return out;
}

/* M1: Resolve a symbol with module visibility and qualified name support.
 * Returns the binding on success.
 * Returns NULL with *had_error = false: symbol not found, caller emits "unbound".
 * Returns NULL with *had_error = true: error already emitted, caller returns NULL. */
static Binding *elab_lookup_sym(Elab *e, const Symbol *sym, Span span, bool *had_error) {
    *had_error = false;

    /* Direct scope lookup */
    Binding *b = scope_lookup(e->scope, sym);
    if (b) {
        /* Visibility: private symbol accessed from outside its module */
        if (b->defining_module_name != NULL
            && e->current_module_name != b->defining_module_name
            && !b->is_exported) {
            diag_emit(DIAG_ERROR, span,
                      "symbol '%s' is private to module '%s'",
                      sym->name, b->defining_module_name->name);
            diag_emit(DIAG_NOTE, b->span,
                      "defined here; add '%s' to module '%s''s (export ...) list to expose it",
                      sym->name, b->defining_module_name->name);
            *had_error = true;
            return NULL;
        }
        return b;
    }

    /* Qualified name resolution — only if symbol contains '/' */
    const char *sym_str = sym->name;
    uint32_t    sym_len = sym->len;
    bool has_slash = false;
    for (uint32_t i = 0; i < sym_len; i++) {
        if (sym_str[i] == '/') { has_slash = true; break; }
    }
    if (!has_slash) return NULL;

    /* Self-qualified: current module name is a prefix */
    if (e->current_module_name != NULL) {
        const Symbol *mn = e->current_module_name;
        if (sym_len > mn->len + 1
            && sym_str[mn->len] == '/'
            && memcmp(sym_str, mn->name, mn->len) == 0) {
            const char *suffix     = sym_str + mn->len + 1;
            uint32_t    suffix_len = sym_len - mn->len - 1;
            const Symbol *ss = symtab_intern(e->st, strslice(suffix, suffix_len));
            Binding *b2 = scope_lookup(e->scope, ss);
            if (b2) return b2;
            diag_emit_with_code(DIAG_ERROR, span, TUR_E0003_UNBOUND_SYMBOL,
                                "unbound symbol '%.*s' in module '%s'",
                                (int)suffix_len, suffix, mn->name);
            *had_error = true;
            return NULL;
        }
    }

    /* M2: Cross-module qualified resolution. */
    if (e->current_module != NULL) {
        /* alias/sym — match by :as alias */
        for (uint32_t i = 0; i < e->current_module->n_imports; i++) {
            const ImportSpec *imp = &e->current_module->imports[i];
            if (!imp->alias) continue;
            const Symbol *alias = imp->alias;
            if (sym_len > alias->len + 1
                && sym_str[alias->len] == '/'
                && memcmp(sym_str, alias->name, alias->len) == 0) {
                const char *sym_part     = sym_str + alias->len + 1;
                uint32_t    sym_part_len = sym_len - alias->len - 1;
                const Symbol *sym_key = symtab_intern(e->st, strslice(sym_part, sym_part_len));
                ElabModule *loaded = elab_find_loaded_module(e, imp->module_name);
                if (!loaded) {
                    diag_emit(DIAG_ERROR, span,
                              "module '%s' (alias '%s') was not loaded",
                              imp->module_name->name, alias->name);
                    *had_error = true;
                    return NULL;
                }
                for (uint32_t m = 0; m < loaded->n_exports; m++) {
                    if (loaded->exports[m]->name == sym_key)
                        return loaded->exports[m];
                }
                /* M7: check if the symbol IS defined in that module but private. */
                Binding *priv = NULL;
                for (uint32_t k = 0; k < e->global.n; k++) {
                    Binding *gb = e->global.bindings[k];
                    if (gb->name == sym_key &&
                        gb->defining_module_name == imp->module_name) {
                        priv = gb; break;
                    }
                }
                diag_emit(DIAG_ERROR, span,
                          "symbol '%s' is not exported from module '%s'",
                          sym_key->name, imp->module_name->name);
                if (priv) {
                    diag_emit(DIAG_NOTE, priv->span,
                              "'%s' is defined here but is private; add it to module '%s''s (export ...) list",
                              sym_key->name, imp->module_name->name);
                }
                *had_error = true;
                return NULL;
            }
        }
        /* full-module-name/sym — match by full module name */
        for (uint32_t i = 0; i < e->current_module->n_imports; i++) {
            const ImportSpec *imp = &e->current_module->imports[i];
            const Symbol *mn = imp->module_name;
            if (sym_len > mn->len + 1
                && sym_str[mn->len] == '/'
                && memcmp(sym_str, mn->name, mn->len) == 0) {
                const char *sym_part     = sym_str + mn->len + 1;
                uint32_t    sym_part_len = sym_len - mn->len - 1;
                const Symbol *sym_key = symtab_intern(e->st, strslice(sym_part, sym_part_len));
                ElabModule *loaded = elab_find_loaded_module(e, mn);
                if (!loaded) {
                    diag_emit(DIAG_ERROR, span,
                              "module '%s' was not loaded", mn->name);
                    *had_error = true;
                    return NULL;
                }
                for (uint32_t m = 0; m < loaded->n_exports; m++) {
                    if (loaded->exports[m]->name == sym_key)
                        return loaded->exports[m];
                }
                /* M7: check if the symbol IS defined in that module but private. */
                Binding *priv = NULL;
                for (uint32_t k = 0; k < e->global.n; k++) {
                    Binding *gb = e->global.bindings[k];
                    if (gb->name == sym_key && gb->defining_module_name == mn) {
                        priv = gb; break;
                    }
                }
                diag_emit(DIAG_ERROR, span,
                          "symbol '%s' is not exported from module '%s'",
                          sym_key->name, mn->name);
                if (priv) {
                    diag_emit(DIAG_NOTE, priv->span,
                              "'%s' is defined here but is private; add it to module '%s''s (export ...) list",
                              sym_key->name, mn->name);
                }
                *had_error = true;
                return NULL;
            }
        }
    }

    return NULL; /* Not a recognised qualified name; caller handles "unbound" */
}

static Expr *elab_as_cast(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "'as' requires exactly two arguments: (as type expr)");
        return NULL;
    }
    Form *type_form = call->as.list.items[1];
    Form *expr_form = call->as.list.items[2];

    if (type_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "'as' expects a type name as first argument");
        return NULL;
    }
    TypeKind target_kind = typekind_from_symbol(type_form->as.sym->name);
    if (target_kind == TY_UNKNOWN) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "unknown type '%s' in 'as' cast", type_form->as.sym->name);
        return NULL;
    }
    if (!typekind_is_numeric(target_kind)) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "'as' casts are only valid for numeric types");
        return NULL;
    }

    Expr *inner = elab_form(e, expr_form);
    if (!inner) return NULL;

    if (!typekind_is_numeric(inner->type.kind)) {
        diag_emit(DIAG_ERROR, expr_form->span,
                  "cannot cast non-numeric type '%s' with 'as'",
                  type_name(inner->type));
        return NULL;
    }

    /* No-op cast: same type */
    if (inner->type.kind == target_kind) return inner;

    Type result_type = type_simple(target_kind, CK_COPY);
    Expr *out = expr_new(e->arena, EX_CAST, result_type, call->span);
    out->as.cast_.expr = inner;
    out->as.cast_.target_kind = target_kind;
    return out;
}

/* IT4: (type-of x) — return the cstr type name of an any-typed value. */
static Expr *elab_any_type_of(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "'type-of' requires exactly one argument: (type-of x)");
        return NULL;
    }
    Expr *val = elab_form(e, call->as.list.items[1]);
    if (!val) return NULL;
    if (val->type.kind != TY_ANY) {
        diag_emit(DIAG_ERROR, call->span,
                  "'type-of' expects an 'any'-typed argument, got '%s'",
                  type_name(val->type));
        return NULL;
    }
    Type cstr_t = type_simple(TY_CSTR, CK_COPY);
    Expr *out = expr_new(e->arena, EX_ANY_TYPE_OF, cstr_t, call->span);
    out->as.any_type_of_.value = val;
    return out;
}

/* IT4: (cast x T) — unsafe downcast from any; returns the inner value unboxed as T.
 * T must be a primitive type name. No runtime tag check is emitted (unsafe). */
static Expr *elab_any_cast(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "'cast' requires exactly two arguments: (cast x T)");
        return NULL;
    }
    Expr *val = elab_form(e, call->as.list.items[1]);
    if (!val) return NULL;
    if (val->type.kind != TY_ANY) {
        diag_emit(DIAG_ERROR, call->span,
                  "'cast' expects an 'any'-typed first argument, got '%s'",
                  type_name(val->type));
        return NULL;
    }
    Form *type_form = call->as.list.items[2];
    if (type_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "'cast' expects a type name as second argument");
        return NULL;
    }
    TypeKind target_kind = typekind_from_symbol(type_form->as.sym->name);
    if (target_kind == TY_UNKNOWN) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "unknown type '%s' in 'cast'", type_form->as.sym->name);
        return NULL;
    }
    Type result_type = type_simple(target_kind, CK_COPY);
    Expr *out = expr_new(e->arena, EX_ANY_CAST, result_type, call->span);
    out->as.any_cast_.value = val;
    out->as.any_cast_.target_kind = target_kind;
    return out;
}

static Expr *elab_form(Elab *e, Form *f) {
    switch (f->tag) {
        case F_NIL:  return e_nil(e, f->span);
        case F_BOOL: {
            Expr *out = expr_new(e->arena, EX_BOOL_LIT, TYPE_BOOL, f->span);
            out->as.b = f->as.b;
            return out;
        }
        case F_INT: {
            /* Phase N: dispatch to fixed-width type based on suffix */
            Type lit_type;
            switch (f->lit_suffix) {
                case LIT_SUF_I8:  lit_type = TYPE_INT8;    break;
                case LIT_SUF_I16: lit_type = TYPE_INT16;   break;
                case LIT_SUF_I32: lit_type = TYPE_INT32;   break;
                case LIT_SUF_I64: lit_type = TYPE_INT64;   break;
                case LIT_SUF_U8:  lit_type = TYPE_UINT8;   break;
                case LIT_SUF_U16: lit_type = TYPE_UINT16;  break;
                case LIT_SUF_U32: lit_type = TYPE_UINT32;  break;
                case LIT_SUF_U64: lit_type = TYPE_UINT64;  break;
                case LIT_SUF_F32: lit_type = TYPE_FLOAT32; break;
                case LIT_SUF_F64: lit_type = TYPE_FLOAT64; break;
                default:          lit_type = TYPE_INT;     break;
            }
            bool is_float_suffix = (f->lit_suffix == LIT_SUF_F32 || f->lit_suffix == LIT_SUF_F64);
            ExprKind ek = is_float_suffix ? EX_FLOAT_LIT : EX_INT_LIT;
            Expr *out = expr_new(e->arena, ek, lit_type, f->span);
            if (is_float_suffix) out->as.f = (double)f->as.i;
            else                 out->as.i = f->as.i;
            return out;
        }
        case F_FLOAT: {
            /* Phase N: dispatch float suffix */
            Type lit_type;
            switch (f->lit_suffix) {
                case LIT_SUF_F32: lit_type = TYPE_FLOAT32; break;
                case LIT_SUF_F64: lit_type = TYPE_FLOAT64; break;
                default:          lit_type = TYPE_FLOAT;   break;
            }
            Expr *out = expr_new(e->arena, EX_FLOAT_LIT, lit_type, f->span);
            out->as.f = f->as.f;
            return out;
        }
        case F_STR: {
            Expr *out = expr_new(e->arena, EX_CSTR_LIT, TYPE_CSTR, f->span);
            out->as.s = f->as.s;
            return out;
        }
        case F_KEYWORD:
            diag_emit(DIAG_ERROR, f->span,
                      "phase 1: keywords are only allowed as :else in cond or case");
            return NULL;
        case F_SYM: {
            /* M1: Use elab_lookup_sym for visibility + qualified name resolution */
            bool sym_qual_err = false;
            Binding *b = elab_lookup_sym(e, f->as.sym, f->span, &sym_qual_err);
            if (!b) {
                if (sym_qual_err) return NULL; /* error already emitted */
                /* Phase 8: Enhanced unbound symbol diagnostic with suggestions */
                const Symbol *best_match = NULL;
                int best_distance = 3;
                for (Scope *cur = e->scope; cur; cur = cur->parent) {
                    for (uint32_t i = 0; i < cur->n; i++) {
                        Binding *candidate = cur->bindings[i];
                        int dist = sym_levenshtein_distance(f->as.sym, candidate->name);
                        if (dist > 0 && dist < best_distance) {
                            best_distance = dist;
                            best_match = candidate->name;
                        }
                    }
                }
                if (best_match) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "unbound symbol '%s'", f->as.sym->name);
                    char sug_text[128];
                    snprintf(sug_text, sizeof(sug_text), "Did you mean '%s'?", best_match->name);
                    DiagSuggestion sug = {
                        sug_text,
                        NULL,
                        "https://turmeric-lang.dev/docs/errors/TUR-E0003"
                    };
                    diag_emit_with_suggestion(DIAG_ERROR, f->span, msg, &sug);
                } else {
                    diag_emit_with_code(DIAG_ERROR, f->span, TUR_E0003_UNBOUND_SYMBOL,
                                        "unbound symbol '%s'", f->as.sym->name);
                }
                return NULL;
            }
            /* UT1: Check for use-after-consume of a unique binding (more specific than generic move) */
            if (g_unique_enabled && b->is_unique && b->is_moved) {
                diag_emit_with_code(DIAG_ERROR, f->span,
                                    TUR_E0201_UNIQUE_COPY,
                                    "cannot copy unique value '%s' -- "
                                    "unique values may be used at most once",
                                    b->name->name);
                return NULL;
            }
            /* Phase 11: Check for use-after-move */
            if (!binding_check_not_moved(b, f->span, "binding")) {
                return NULL;
            }
            /* LT1: Check for use-after-consume of a linear binding */
            if (g_linear_enabled && b->is_linear) {
                if (b->is_linear_consumed) {
                    diag_emit_with_code(DIAG_ERROR, f->span,
                                        TUR_E0101_LINEAR_USE_AFTER_CONSUME,
                                        "linear value '%s' used after being consumed",
                                        b->name->name);
                    return NULL;
                }
                /* Mark linear binding as consumed on use */
                b->is_linear_consumed = true;
            }
            /* ST1: Substructural usage tracking.
             * ^affine: may not be used more than once (TUR_E0150).
             * ^relevant: must be used at least once; duplicates are fine. */
            if (g_substructural_enabled) {
                if (b->is_affine) {
                    if (b->usage_state >= USAGE_USED_ONCE) {
                        diag_emit_with_code(DIAG_ERROR, f->span,
                                            TUR_E0150_AFFINE_USED_TWICE,
                                            "affine value '%s' used more than once",
                                            b->name->name);
                        return NULL;
                    }
                    b->usage_state = USAGE_USED_ONCE;
                } else if (b->is_relevant) {
                    /* Increment usage; any count >= 1 satisfies the must-use requirement */
                    b->usage_state = (b->usage_state == USAGE_UNUSED) ? USAGE_USED_ONCE
                                                                       : USAGE_USED_MANY;
                }
            }
            Expr *out = expr_new(e->arena, EX_VAR, b->type, f->span);
            out->as.var.binding = b;
            /* Phase HRT/G2: Resolve named type variable through current GADT match arm skolem env.
             * If a binding was declared as :a (TY_TYVAR with name "a"), and we are currently
             * inside a GADT match arm that has a skolem binding for "a", use the concrete type. */
            if (b->type.kind == TY_TYVAR && b->type.as.tyvar_.name != NULL && e->g2_skolem_env) {
                TypeKind resolved = gadt_skolem_lookup(e->g2_skolem_env, b->type.as.tyvar_.name);
                if (resolved != TY_UNKNOWN) {
                    out->type = type_from_kind(resolved);
                    out->type.copy_kind = typekind_default_copy_kind(resolved);
                }
            }
            return out;
        }
        case F_VEC:
            diag_emit(DIAG_ERROR, f->span,
                      "phase 1: vector literals are only allowed in let bindings");
            return NULL;
        case F_MAP:
            diag_emit(DIAG_ERROR, f->span,
                      "phase 1: map literals are parsed but not yet supported by elaboration");
            return NULL;
        case F_SET: {
            /* Phase X3: Elaborate set literal #s(e1 e2 ...) -> EX_SET_LIT */
            uint32_t n = f->as.list.len;
            Expr **items = arena_alloc(e->arena, n * sizeof(Expr *));
            for (uint32_t i = 0; i < n; i++) {
                items[i] = elab_form(e, f->as.list.items[i]);
                if (!items[i]) return NULL;
            }
            Type set_type = { .kind = TY_SET, .copy_kind = CK_COPY };
            Expr *ex = expr_new(e->arena, EX_SET_LIT, set_type, f->span);
            ex->as.set_lit_.items = items;
            ex->as.set_lit_.n = n;
            return ex;
        }
        /* Phase 6: quote form */
        case F_QUOTE: {
            /* (quote x) returns x as a literal without evaluating x */
            if (f->as.list.len != 1) {
                diag_emit(DIAG_ERROR, f->span,
                          "quote requires exactly one argument");
                return NULL;
            }
            Form *quoted = f->as.list.items[0];
            /* Quote just returns the inner form as a literal */
            /* For now, support quoting literals and symbols */
            return elab_form(e, quoted);
        }
        /* Phase 6: quasiquote forms - expand them */
        case F_QUASIQUOTE:
        case F_UNQUOTE:
        case F_UNQUOTE_SPLICING:
            /* Expand quasiquote forms first */
            {
                Form *expanded = quasiquote_expand_form(e, f);
                return elab_form(e, expanded);
            }
        case F_CBLOCK: {
            /* Phase 2: inline C code block ```c ... ``` */
            /* For now, we don't support captures, so the InlineC has no captures */
            InlineC *ic = (InlineC *)arena_alloc(e->arena, sizeof(InlineC));
            ic->code = f->as.cblock;
            ic->return_type = TYPE_NIL; /* Will be inferred from context or default to void */
            ic->captures = NULL;
            ic->n_captures = 0;
            
            Expr *out = expr_new(e->arena, EX_INLINE_C, TYPE_NIL, f->span);
            out->as.inline_c_.inline_c = ic;
            return out;
        }
        case F_LIST:
            if (f->as.list.len == 0) {
                diag_emit(DIAG_ERROR, f->span, "empty list ()");
                return NULL;
            }
            return elab_call(e, f);
        case F_TYPE_ANN:
            diag_emit(DIAG_ERROR, f->span,
                      "type annotation ': type' is only valid after a parameter name or as a return type");
            return NULL;
    }
    return NULL;
}

/* Phase 11: defstruct - define a struct type
 * Syntax: (defstruct Name [:copy] [field1 :type1 field2 :type2 ...])
 * 
 * The :copy annotation indicates the struct is bitwise-copyable (all fields must be Copy).
 * Without :copy, the struct is move-only (default).
 * 
 * Returns an EX_DEF expression that defines the struct type at file scope.
 */

/* Helper: parse a field type keyword like "int", "rc<int>", "ref<bool>", "ptr<void>" */
static void parse_struct_field_type(const char *tname, uint32_t tlen,
                                     TypeKind *out_kind, TypeKind *out_inner) {
    *out_kind = TY_UNKNOWN;
    *out_inner = TY_UNKNOWN;

    if (tlen == 3  && memcmp(tname, "int",   3) == 0) { *out_kind = TY_INT;      return; }
    if (tlen == 5  && memcmp(tname, "int64", 5) == 0) { *out_kind = TY_INT;      return; }
    if (tlen == 4  && memcmp(tname, "bool",  4) == 0) { *out_kind = TY_BOOL;     return; }
    if (tlen == 5  && memcmp(tname, "float", 5) == 0) { *out_kind = TY_FLOAT;    return; }
    if (tlen == 7  && memcmp(tname, "float64", 7) == 0) { *out_kind = TY_FLOAT;  return; }
    if (tlen == 4  && memcmp(tname, "int8",  4) == 0) { *out_kind = TY_INT8;     return; }
    if (tlen == 5  && memcmp(tname, "int16", 5) == 0) { *out_kind = TY_INT16;    return; }
    if (tlen == 5  && memcmp(tname, "int32", 5) == 0) { *out_kind = TY_INT32;    return; }
    if (tlen == 5  && memcmp(tname, "uint8", 5) == 0) { *out_kind = TY_UINT8;    return; }
    if (tlen == 6  && memcmp(tname, "uint16", 6) == 0) { *out_kind = TY_UINT16;  return; }
    if (tlen == 6  && memcmp(tname, "uint32", 6) == 0) { *out_kind = TY_UINT32;  return; }
    if (tlen == 6  && memcmp(tname, "uint64", 6) == 0) { *out_kind = TY_UINT64;  return; }
    if (tlen == 7  && memcmp(tname, "float32", 7) == 0) { *out_kind = TY_FLOAT32; return; }
    if (tlen == 4  && memcmp(tname, "cstr",  4) == 0) { *out_kind = TY_CSTR;     return; }
    if (tlen == 3  && memcmp(tname, "nil",   3) == 0) { *out_kind = TY_NIL;      return; }
    if (tlen == 4  && memcmp(tname, "void",  4) == 0) { *out_kind = TY_NIL;      return; }
    if (tlen == 9  && memcmp(tname, "ptr<void>", 9) == 0) { *out_kind = TY_PTR_VOID; return; }
    /* Phase 16 v2: :fn field type — function pointer (may carry #{...} effect-row annotation) */
    if (tlen == 2  && memcmp(tname, "fn",    2) == 0) { *out_kind = TY_FN;       return; }

    /* Compound types: rc<T>, ref<T>, lref<T>, weak<T> */
    /* Parse the prefix and inner type */
    const char *prefix_rc   = "rc<";
    const char *prefix_ref  = "ref<";
    const char *prefix_lref = "lref<";
    const char *prefix_weak = "weak<";

    TypeKind prefix_kind = TY_UNKNOWN;
    uint32_t prefix_len = 0;
    if (tlen > 3 && memcmp(tname, prefix_rc, 3) == 0)   { prefix_kind = TY_RC;   prefix_len = 3; }
    if (tlen > 4 && memcmp(tname, prefix_ref, 4) == 0)  { prefix_kind = TY_REF;  prefix_len = 4; }
    if (tlen > 5 && memcmp(tname, prefix_lref, 5) == 0) { prefix_kind = TY_LREF; prefix_len = 5; }
    if (tlen > 5 && memcmp(tname, prefix_weak, 5) == 0) { prefix_kind = TY_WEAK; prefix_len = 5; }

    if (prefix_kind != TY_UNKNOWN && prefix_len > 0 && tname[tlen - 1] == '>') {
        const char *inner_name = tname + prefix_len;
        uint32_t inner_len = tlen - prefix_len - 1; /* strip trailing '>' */
        TypeKind inner_kind = TY_UNKNOWN, dummy = TY_UNKNOWN;
        parse_struct_field_type(inner_name, inner_len, &inner_kind, &dummy);
        *out_kind = prefix_kind;
        *out_inner = inner_kind;
        return;
    }

    /* Unknown type - leave as TY_UNKNOWN */
}

/* Helper: check if a TypeKind is considered copy */
static bool typekind_is_copy_for_struct(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_BOOL: case TY_FLOAT: case TY_CSTR:
        case TY_PTR_VOID: case TY_NIL:
        case TY_INT8: case TY_INT16: case TY_INT32:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32:
        /* Phase 16 v2: :fn fields are stored as int64_t at the C level, so they are
         * trivially copyable (function pointer stored as an integer value). */
        case TY_FN:
            return true;
        default:
            return false;
    }
}

/* Phase RF0: Look up a binding for a user-defined struct or ADT type by name.
 * Unlike scope_lookup (which returns the most recent binding), this searches all
 * bindings for one with kind TY_STRUCT or TY_ADT.  Needed because a constructor
 * with the same name as its type (e.g. (defdata Expr (Expr :ExprNode))) shadows
 * the type binding with a TY_FN constructor binding. */
static Binding *scope_lookup_type_def(Scope *s, const Symbol *name) {
    for (Scope *cur = s; cur; cur = cur->parent) {
        for (uint32_t i = cur->n; i > 0; i--) {
            Binding *b = cur->bindings[i - 1];
            if (b->name == name &&
                (b->type.kind == TY_STRUCT || b->type.kind == TY_ADT)) {
                return b;
            }
        }
    }
    return NULL;
}

/* Phase RF0: Check if a symbol was registered as a forward-declared type stub */
static bool elab_is_forward_type(Elab *e, const Symbol *sym) {
    for (uint32_t i = 0; i < e->n_forward_type_syms; i++) {
        if (e->forward_type_syms[i] == sym) return true;
    }
    return false;
}

/* Phase RF0: Add a symbol to the forward-declared types list */
static void elab_add_forward_type(Elab *e, const Symbol *sym) {
    if (e->n_forward_type_syms >= e->cap_forward_type_syms) {
        e->cap_forward_type_syms = e->cap_forward_type_syms ? e->cap_forward_type_syms * 2 : 8;
        e->forward_type_syms = (const Symbol **)realloc(e->forward_type_syms,
            e->cap_forward_type_syms * sizeof(Symbol *));
    }
    e->forward_type_syms[e->n_forward_type_syms++] = sym;
}

/* Helper: add StructDef to the elab registry */
static void elab_register_struct_def(Elab *e, StructDef *def) {
    if (e->n_struct_defs >= e->cap_struct_defs) {
        e->cap_struct_defs = e->cap_struct_defs ? e->cap_struct_defs * 2 : 8;
        e->struct_defs = (StructDef **)realloc(e->struct_defs,
            e->cap_struct_defs * sizeof(StructDef *));
    }
    e->struct_defs[e->n_struct_defs++] = def;
}

static Expr *elab_defstruct(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "defstruct requires a name and field list: (defstruct Name [:copy] [f1 : T1 ...])");
        return NULL;
    }
    
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defstruct name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;
    
    /* Check for optional :copy / :move / :linear annotation */
    bool is_copy = false;
    bool is_linear = false;
    uint32_t fields_start_idx = 2;

    if (call->as.list.len >= 3) {
        Form *kw_form = call->as.list.items[2];
        if (kw_form->tag == F_KEYWORD && kw_form->as.sym == e->kw_copy) {
            is_copy = true;
            fields_start_idx = 3;
        } else if (kw_form->tag == F_KEYWORD && kw_form->as.sym == e->kw_move) {
            is_copy = false;
            fields_start_idx = 3;
        } else if (kw_form->tag == F_KEYWORD && kw_form->as.sym == e->kw_linear) {
            /* LT4: :linear structs are exactly-once (CK_LINEAR). */
            is_linear = true;
            fields_start_idx = 3;
        }
    }
    
    if (call->as.list.len < fields_start_idx + 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "defstruct requires a field list");
        return NULL;
    }
    
    Form *fields_form = call->as.list.items[fields_start_idx];
    if (fields_form->tag != F_VEC) {
        diag_emit(DIAG_ERROR, fields_form->span,
                  "defstruct field list must be a vector [f1 : T1 f2 : T2 ...]");
        return NULL;
    }
    
    /* Phase RF0: allow re-elaboration of forward-declared stub types */
    bool is_forward_stub = false;
    Binding *existing_b = scope_lookup(e->scope, name);
    if (existing_b) {
        if (elab_is_forward_type(e, name)) {
            is_forward_stub = true;
        } else {
            diag_emit(DIAG_ERROR, name_form->span,
                      "defstruct: '%s' is already defined", name->name);
            return NULL;
        }
    }

    /* Validate field list */
    uint32_t n_fields = fields_form->as.list.len;
    if (n_fields == 0) {
        diag_emit(DIAG_ERROR, fields_form->span,
                  "defstruct field list cannot be empty");
        return NULL;
    }

    uint32_t n_items = n_fields; /* n_fields = fields_form->as.list.len */

    /* Phase 16 v2: Field list may contain optional #{...} after :fn type annotations.
     * Pre-scan to count actual fields and validate structure. */
    uint32_t actual_n_fields = 0;
    {
        uint32_t scan = 0;
        while (scan < n_items) {
            if (fields_form->as.list.items[scan]->tag != F_SYM) {
                diag_emit(DIAG_ERROR, fields_form->as.list.items[scan]->span,
                          "defstruct field list: expected field name symbol");
                return NULL;
            }
            scan++; /* consume name */
            if (scan >= n_items || fields_form->as.list.items[scan]->tag != F_KEYWORD) {
                diag_emit(DIAG_ERROR, fields_form->span,
                          "defstruct field list must have [name :type ...] pairs");
                return NULL;
            }
            const char *tname = fields_form->as.list.items[scan]->as.sym->name;
            uint32_t tlen = fields_form->as.list.items[scan]->as.sym->len;
            scan++; /* consume type keyword */
            /* Optional #{...} effect-row only for :fn fields */
            bool is_fn_field = (tlen == 2 && memcmp(tname, "fn", 2) == 0);
            if (is_fn_field && scan < n_items &&
                fields_form->as.list.items[scan]->tag == F_MAP) {
                scan++; /* consume #{...} annotation */
            }
            actual_n_fields++;
        }
    }

    /* Phase RF0: Allocate (or reuse the forward stub) StructDef and register
     * in global scope BEFORE parsing fields, so that self-referential and
     * mutually-recursive field type annotations resolve correctly. */
    StructDef *def;
    Binding *b;
    if (is_forward_stub) {
        /* Reuse the pre-registered stub and fill it in */
        b = existing_b;
        def = b->type.as.struct_.def;
        def->n_fields = actual_n_fields;
        def->fields = (StructField *)arena_alloc(e->arena, actual_n_fields * sizeof(StructField));
        def->is_copy = is_copy;
        def->is_linear = is_linear; /* LT4 */
        def->needs_drop_glue = false;
        def->origin_file_id = call->span.file_id;
        /* Already in global scope and elab registry from the pre-pass */
    } else {
        def = (StructDef *)arena_alloc(e->arena, sizeof(StructDef));
        def->name = name->name;
        def->n_fields = actual_n_fields;
        def->fields = (StructField *)arena_alloc(e->arena, actual_n_fields * sizeof(StructField));
        def->is_copy = is_copy;
        def->is_linear = is_linear; /* LT4 */
        def->needs_drop_glue = false;
        /* Phase HKT-P4: record the file that defined this struct. */
        def->origin_file_id = call->span.file_id;

        Type struct_type = type_struct(def);
        b = binding_new(e, name, struct_type, false, true, name_form->span);
        scope_add(&e->global, b);
        elab_register_struct_def(e, def);
    }

    /* Parse fields */
    {
        uint32_t scan = 0;
        for (uint32_t fi = 0; fi < actual_n_fields; fi++) {
            Form *field_name_form = fields_form->as.list.items[scan++];
            Form *field_type_form = fields_form->as.list.items[scan++];

            const char *tname = field_type_form->as.sym->name;
            uint32_t tlen = field_type_form->as.sym->len;
            TypeKind fkind, finner;
            parse_struct_field_type(tname, tlen, &fkind, &finner);

            if (fkind == TY_UNKNOWN) {
                /* Phase RF0: fall back to user-defined type lookup.  Any struct or
                 * ADT is heap-allocated and stored as an opaque int64_t pointer, so
                 * it is safe to use as a recursive field type without any layout change. */
                const Symbol *type_sym = symtab_intern(e->st, strslice(tname, tlen));
                Binding *tb = scope_lookup_type_def(e->scope, type_sym);
                if (tb && (tb->type.kind == TY_STRUCT || tb->type.kind == TY_ADT)) {
                    fkind = TY_INT;
                    finner = TY_UNKNOWN;
                } else {
                    diag_emit(DIAG_ERROR, field_type_form->span,
                              "defstruct field '%s' has unrecognized type :%s",
                              field_name_form->as.sym->name, tname);
                    return NULL;
                }
            }

            /* LT1: E0102 -- linear fields cannot appear in :copy structs.
             * A field of type lref<T> (CK_LINEAR) makes the struct non-copyable,
             * which is incompatible with :copy semantics. :move structs may hold lref<T>. */
            if (g_linear_enabled && is_copy && typekind_default_copy_kind(fkind) == CK_LINEAR) {
                diag_emit_with_code(DIAG_ERROR, field_type_form->span,
                                    TUR_E0102_LINEAR_COPY,
                                    "cannot copy linear field '%s' -- "
                                    "linear values cannot appear in :copy structs",
                                    field_name_form->as.sym->name);
                return NULL;
            }
            /* :copy struct validation: all fields must be copy */
            if (is_copy && !typekind_is_copy_for_struct(fkind)) {
                diag_emit(DIAG_ERROR, field_type_form->span,
                          "defstruct: field '%s' has non-copy type :%s and cannot be used in :copy struct",
                          field_name_form->as.sym->name, tname);
                return NULL;
            }

            def->fields[fi].name = field_name_form->as.sym->name;
            def->fields[fi].kind = fkind;
            def->fields[fi].inner_kind = finner;
            def->fields[fi].effect_row = NULL;

            /* Phase 16 v2: parse optional #{...} effect-row annotation for :fn fields */
            if (fkind == TY_FN && scan < n_items &&
                fields_form->as.list.items[scan]->tag == F_MAP) {
                Form *row_form = fields_form->as.list.items[scan++];
                uint8_t n_sym = (uint8_t)row_form->as.list.len;
                const Symbol **syms = (const Symbol **)arena_alloc(e->arena,
                                        (n_sym ? n_sym : 1) * sizeof(Symbol *));
                uint8_t n_valid = 0;
                for (uint32_t j = 0; j < row_form->as.list.len; j++) {
                    Form *item = row_form->as.list.items[j];
                    if (item->tag == F_SYM) {
                        syms[n_valid++] = item->as.sym;
                    }
                }
                def->fields[fi].effect_row = effect_row_unresolved(e->arena, syms, n_valid);
            }

            /* Check if this field requires drop glue */
            if (fkind == TY_RC || fkind == TY_REF || fkind == TY_WEAK) {
                def->needs_drop_glue = true;
            }
        }
    }

    /* Return EX_DEF with struct_def populated.
     * Registration in global scope and elab registry was done above (RF0). */
    Expr *out = expr_new(e->arena, EX_DEF, TYPE_NIL, call->span);
    out->as.def_.binding = b;
    out->as.def_.init = NULL;
    out->as.def_.struct_def = def;
    return out;
}

/* Phase G0: Helper - register AdtDef in elab registry */
static void elab_register_adt_def(Elab *e, AdtDef *def) {
    if (e->n_adt_defs >= e->cap_adt_defs) {
        e->cap_adt_defs = e->cap_adt_defs ? e->cap_adt_defs * 2 : 8;
        e->adt_defs = (AdtDef **)realloc(e->adt_defs,
            e->cap_adt_defs * sizeof(AdtDef *));
    }
    e->adt_defs[e->n_adt_defs++] = def;
}

/* Phase G0: defdata — define a sum type (ADT)
 * Syntax: (defdata Name [:copy]
 *           (Ctor1)
 *           (Ctor2 :T1 :T2)
 *           ...)
 */
static Expr *elab_defdata(Elab *e, const Form *call) {
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "defdata requires a name and at least one constructor: "
                  "(defdata Name (Ctor1) (Ctor2 :T1) ...)");
        return NULL;
    }

    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defdata name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;

    /* Check for optional :copy annotation */
    bool is_copy = false;
    uint32_t ctors_start_idx = 2;
    if (call->as.list.len >= 3) {
        Form *kw_form = call->as.list.items[2];
        if (kw_form->tag == F_KEYWORD && kw_form->as.sym == e->kw_copy) {
            is_copy = true;
            ctors_start_idx = 3;
        } else if (kw_form->tag == F_KEYWORD && kw_form->as.sym == e->kw_move) {
            is_copy = false;
            ctors_start_idx = 3;
        }
    }

    /* Phase RF1: Check for an optional type-parameter vector [^f a b ...] between
     * the name (or :copy annotation) and the first constructor.  Type parameters
     * are stored on the AdtDef and used for documentation / future type-checking;
     * they do not affect C codegen (all values are int64_t pointers). */
    const char **type_params = NULL;
    uint8_t n_type_params = 0;
    if (ctors_start_idx < call->as.list.len &&
        call->as.list.items[ctors_start_idx]->tag == F_VEC) {
        Form *tp_form = call->as.list.items[ctors_start_idx];
        n_type_params = (uint8_t)tp_form->as.list.len;
        if (n_type_params > 0) {
            type_params = (const char **)arena_alloc(e->arena,
                              n_type_params * sizeof(char *));
            for (uint8_t pi = 0; pi < n_type_params; pi++) {
                Form *pf = tp_form->as.list.items[pi];
                if (pf->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, pf->span,
                              "defdata: type parameter must be a symbol, e.g. a, ^f");
                    return NULL;
                }
                type_params[pi] = pf->as.sym->name;
            }
        }
        ctors_start_idx++;
    }

    /* Phase RF0: allow re-elaboration of forward-declared stub types */
    bool is_forward_stub_adt = false;
    Binding *existing_adt_b = scope_lookup(e->scope, name);
    if (existing_adt_b) {
        if (elab_is_forward_type(e, name)) {
            is_forward_stub_adt = true;
        } else {
            diag_emit(DIAG_ERROR, name_form->span,
                      "defdata: '%s' is already defined", name->name);
            return NULL;
        }
    }

    uint32_t n_ctors = call->as.list.len - ctors_start_idx;
    if (n_ctors == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "defdata: '%s' must have at least one constructor", name->name);
        return NULL;
    }

    /* Phase RF0: Allocate (or reuse forward stub) AdtDef and register BEFORE
     * parsing constructors so that self-referential and mutually-recursive
     * constructor field types resolve correctly. */
    AdtDef *def;
    Binding *adt_binding;
    Type adt_type;
    if (is_forward_stub_adt) {
        /* Reuse the pre-registered stub and fill it in */
        adt_binding = existing_adt_b;
        def = adt_binding->type.as.adt_.def;
        def->n_ctors = n_ctors;
        def->ctors = (CtorDef **)arena_alloc(e->arena, n_ctors * sizeof(CtorDef *));
        def->is_copy = is_copy;
        def->needs_drop_glue = false;
        def->is_gadt = false;
        def->type_params = type_params;
        def->n_type_params = n_type_params;
        /* Refresh adt_type from the def so copy_kind reflects is_copy correctly.
         * The pre-pass stub was created with is_copy=false; now that we know the
         * real is_copy flag, regenerate the type and update the binding. */
        adt_type = type_adt(def);
        /* Phase G1/HKT: Apply KIND_ARROW fix so that the kind check can detect
         * when a parameterized defdata type is used in a kind-* slot. */
        if (n_type_params >= 2)     adt_type.hkt_kind = KIND_ARROW2;
        else if (n_type_params == 1) adt_type.hkt_kind = KIND_ARROW;
        adt_binding->type = adt_type;
        /* Already in global scope and elab registry from the pre-pass */
    } else {
        def = (AdtDef *)arena_alloc(e->arena, sizeof(AdtDef));
        def->name = name->name;
        def->n_ctors = n_ctors;
        def->ctors = (CtorDef **)arena_alloc(e->arena, n_ctors * sizeof(CtorDef *));
        def->is_copy = is_copy;
        def->needs_drop_glue = false;
        /* Phase RF1: store type parameters */
        def->is_gadt = false;
        def->type_params = type_params;
        def->n_type_params = n_type_params;

        /* Pre-register ADT type so constructors can reference it.
         * Phase G1/HKT: Set hkt_kind based on type-parameter count so that
         * elab_defgadt's belt-and-suspenders kind check can detect when a
         * parameterized type constructor is used in a kind-* argument slot. */
        adt_type = type_adt(def);
        if (n_type_params >= 2)     adt_type.hkt_kind = KIND_ARROW2;
        else if (n_type_params == 1) adt_type.hkt_kind = KIND_ARROW;
        adt_binding = binding_new(e, name, adt_type, false, true, name_form->span);
        scope_add(&e->global, adt_binding);
    }

    /* Parse each constructor */
    for (uint32_t ci = 0; ci < n_ctors; ci++) {
        Form *ctor_form = call->as.list.items[ctors_start_idx + ci];
        if (ctor_form->tag != F_LIST) {
            diag_emit(DIAG_ERROR, ctor_form->span,
                      "defdata: constructor must be a list form (Ctor :T1 :T2 ...)");
            return NULL;
        }
        if (ctor_form->as.list.len < 1) {
            diag_emit(DIAG_ERROR, ctor_form->span,
                      "defdata: constructor form cannot be empty");
            return NULL;
        }
        Form *ctor_name_form = ctor_form->as.list.items[0];
        if (ctor_name_form->tag != F_SYM) {
            diag_emit(DIAG_ERROR, ctor_name_form->span,
                      "defdata: constructor name must be a symbol");
            return NULL;
        }
        const Symbol *ctor_name = ctor_name_form->as.sym;

        uint32_t n_fields = ctor_form->as.list.len - 1;
        CtorDef *ctor = (CtorDef *)arena_alloc(e->arena, sizeof(CtorDef));
        ctor->name = ctor_name->name;
        ctor->n_fields = n_fields;
        ctor->fields = n_fields > 0
            ? (CtorField *)arena_alloc(e->arena, n_fields * sizeof(CtorField))
            : NULL;
        ctor->adt = def;
        ctor->tag = ci;
        ctor->result_type_form = NULL; /* Phase G1: NULL for defdata */
        ctor->field_forms = NULL;      /* Phase G2: NULL for defdata */

        /* Parse field types */
        for (uint32_t fi = 0; fi < n_fields; fi++) {
            Form *ft_form = ctor_form->as.list.items[1 + fi];
            if (ft_form->tag != F_KEYWORD) {
                diag_emit(DIAG_ERROR, ft_form->span,
                          "defdata: constructor field type must be a keyword like :int, :bool, :cstr");
                return NULL;
            }
            const char *tname = ft_form->as.sym->name;
            uint32_t tlen = ft_form->as.sym->len;
            TypeKind fkind, finner;
            parse_struct_field_type(tname, tlen, &fkind, &finner);
            if (fkind == TY_UNKNOWN) {
                /* Phase RF0: fall back to user-defined type lookup.
                 * All struct/ADT values are heap-allocated int64_t pointers. */
                const Symbol *type_sym = symtab_intern(e->st, strslice(tname, tlen));
                Binding *tb = scope_lookup_type_def(e->scope, type_sym);
                if (tb && (tb->type.kind == TY_STRUCT || tb->type.kind == TY_ADT)) {
                    fkind = TY_INT;
                    finner = TY_UNKNOWN;
                } else {
                    diag_emit(DIAG_ERROR, ft_form->span,
                              "defdata: field has unrecognized type :%s", tname);
                    return NULL;
                }
            }
            ctor->fields[fi].kind = fkind;
            ctor->fields[fi].inner_kind = finner;
            if (fkind == TY_RC || fkind == TY_REF || fkind == TY_WEAK) {
                def->needs_drop_glue = true;
            }
        }

        def->ctors[ci] = ctor;

        /* Register constructor as a global binding.
         * 0-arg constructor: TY_ADT binding (it IS a value).
         * N-arg constructor: TY_FN binding (call it like a function). */
        if (n_fields == 0) {
            /* 0-arg: register as TY_ADT so calls to (Red) work */
            Binding *cb = binding_new(e, ctor_name, adt_type, false, true,
                                      ctor_name_form->span);
            scope_add(&e->global, cb);
        } else {
            /* N-arg: register as TY_FN */
            TypeKind arg_kinds[MAX_FN_ARITY];
            uint8_t arity = (uint8_t)(n_fields > MAX_FN_ARITY ? MAX_FN_ARITY : n_fields);
            for (uint8_t fi = 0; fi < arity; fi++) {
                arg_kinds[fi] = ctor->fields[fi].kind;
            }
            Type fn_type = type_fn(arg_kinds, arity, TY_ADT);
            Binding *cb = binding_new(e, ctor_name, fn_type, false, true,
                                      ctor_name_form->span);
            scope_add(&e->global, cb);
        }
    }

    /* Register ADT in elab registry (skip if it was a forward stub -- already registered) */
    if (!is_forward_stub_adt) {
        elab_register_adt_def(e, def);
    }

    /* Return EX_DEFDATA node */
    Expr *out = expr_new(e->arena, EX_DEFDATA, TYPE_NIL, call->span);
    out->as.defdata_.def = def;
    out->as.defdata_.binding = adt_binding;
    return out;
}

/* Phase G2: Look up a type parameter name in the current skolem environment.
 * Returns TY_UNKNOWN if not found. */
static TypeKind gadt_skolem_lookup(const SkolemEnv *env, const char *name) {
    if (!env) return TY_UNKNOWN;
    for (uint8_t i = 0; i < env->n; i++) {
        if (strcmp(env->bindings[i].name, name) == 0)
            return env->bindings[i].kind;
    }
    return TY_UNKNOWN;
}

/* Phase G2: Resolve a type form to a full Type using the current skolem env.
 * For primitive symbols → concrete Type.
 * For type variable names → look up in senv; TY_TYVAR if unresolved.
 * For ADT reference forms `(AdtName ...)` → look up ADT in global scope.
 * Falls back to TY_INT (opaque int64_t) for unknown forms.
 */
static Type gadt_resolve_type_from_form(Elab *e, const AdtDef *gadt, const Form *f,
                                         const SkolemEnv *senv) {
    if (!f) return type_from_kind(TY_INT);

    if (f->tag == F_SYM) {
        const char *n = f->as.sym->name;
        /* Primitive types */
        if (strcmp(n, "int")    == 0) return type_from_kind(TY_INT);
        if (strcmp(n, "bool")   == 0) return type_from_kind(TY_BOOL);
        if (strcmp(n, "float")  == 0) return type_from_kind(TY_FLOAT);
        if (strcmp(n, "cstr")   == 0) return type_from_kind(TY_CSTR);
        if (strcmp(n, "ptr")    == 0) return type_from_kind(TY_PTR_VOID);
        if (strcmp(n, "int8")   == 0) return type_from_kind(TY_INT8);
        if (strcmp(n, "int16")  == 0) return type_from_kind(TY_INT16);
        if (strcmp(n, "int32")  == 0) return type_from_kind(TY_INT32);
        if (strcmp(n, "int64")  == 0) return type_from_kind(TY_INT64);
        if (strcmp(n, "uint8")  == 0) return type_from_kind(TY_UINT8);
        if (strcmp(n, "uint16") == 0) return type_from_kind(TY_UINT16);
        if (strcmp(n, "uint32") == 0) return type_from_kind(TY_UINT32);
        if (strcmp(n, "uint64") == 0) return type_from_kind(TY_UINT64);
        if (strcmp(n, "float32")== 0) return type_from_kind(TY_FLOAT32);
        if (strcmp(n, "float64")== 0) return type_from_kind(TY_FLOAT64);
        /* Type variable: look up in skolem env */
        TypeKind resolved = gadt_skolem_lookup(senv, n);
        if (resolved != TY_UNKNOWN) return type_from_kind(resolved);
        /* Unresolved type variable → anonymous TY_TYVAR (name=NULL signals skolem escape) */
        (void)gadt; /* suppress unused warning */
        return type_tyvar_named(NULL);
    }

    if (f->tag == F_LIST && f->as.list.len >= 1) {
        /* Possibly an ADT reference: (AdtName type-args...) */
        Form *head = f->as.list.items[0];
        if (head->tag == F_SYM) {
            Binding *b = scope_lookup(e->scope, head->as.sym);
            if (!b) b = scope_lookup(&e->global, head->as.sym);
            if (b && b->type.kind == TY_ADT && b->type.as.adt_.def) {
                return b->type; /* TY_ADT with the def pointer */
            }
            /* Phase HKT: kind-variable application (f a) where f : * -> *.
             * Return an anonymous TY_TYVAR so the arm body is accepted as
             * int64_t-sized (same runtime repr as TY_ADT/TY_APP). */
            if (b && b->type.kind == TY_TYVAR && b->type.hkt_kind == KIND_ARROW) {
                return type_tyvar_named(NULL);
            }
        }
    }

    return type_from_kind(TY_INT); /* fallback: opaque int64_t */
}

/* Phase G2: Build a SkolemEnv for a GADT constructor arm.
 * Parses the constructor's result_type_form (e.g. "(Expr int)") against
 * the ADT's type_params (e.g. ["a"]) to produce concrete bindings
 * such as {a → TY_INT}.  Only primitive-type args are mapped; type-variable
 * args are skipped (leaving those params unresolved → TY_TYVAR in fields). */
static void gadt_build_skolem_env(SkolemEnv *out, const AdtDef *def,
                                   const CtorDef *ctor) {
    out->n = 0;
    if (!ctor->result_type_form || def->n_type_params == 0) return;

    const Form *rt = ctor->result_type_form;
    /* rt should be (AdtName arg0 arg1 ...) */
    if (rt->tag != F_LIST || rt->as.list.len < 2) return;

    /* arg i is at items[1+i] */
    uint32_t n_args = rt->as.list.len - 1;
    uint32_t n_bind = (n_args < def->n_type_params) ? n_args : def->n_type_params;

    for (uint32_t i = 0; i < n_bind && out->n < MAX_SKOLEM_BINDINGS; i++) {
        Form *arg = rt->as.list.items[1 + i];
        const char *param_name = def->type_params[i];
        TypeKind k = TY_UNKNOWN;

        if (arg->tag == F_SYM) {
            const char *an = arg->as.sym->name;
            if (strcmp(an, "int")    == 0) k = TY_INT;
            else if (strcmp(an, "bool")   == 0) k = TY_BOOL;
            else if (strcmp(an, "float")  == 0) k = TY_FLOAT;
            else if (strcmp(an, "cstr")   == 0) k = TY_CSTR;
            else if (strcmp(an, "int8")   == 0) k = TY_INT8;
            else if (strcmp(an, "int16")  == 0) k = TY_INT16;
            else if (strcmp(an, "int32")  == 0) k = TY_INT32;
            else if (strcmp(an, "int64")  == 0) k = TY_INT64;
            else if (strcmp(an, "uint8")  == 0) k = TY_UINT8;
            else if (strcmp(an, "uint16") == 0) k = TY_UINT16;
            else if (strcmp(an, "uint32") == 0) k = TY_UINT32;
            else if (strcmp(an, "uint64") == 0) k = TY_UINT64;
            else if (strcmp(an, "float32")== 0) k = TY_FLOAT32;
            else if (strcmp(an, "float64")== 0) k = TY_FLOAT64;
            /* else: type variable or unknown — skip (param stays unresolved) */
        }
        /* List form like (Expr int) — ADT ref; treat as int64_t / TY_INT */
        else if (arg->tag == F_LIST) {
            k = TY_INT; /* opaque ADT reference */
        }

        if (k != TY_UNKNOWN) {
            out->bindings[out->n].name = param_name;
            out->bindings[out->n].kind = k;
            out->n++;
        }
    }
}

/* Phase G1: Map a simple type form to a TypeKind for codegen.
 * Handles primitive names (int, bool, cstr, etc.) and falls back to
 * TY_INT (opaque int64_t) for ADT references and type variables. */
static TypeKind gadt_field_typekind_from_form(const Form *f) {
    if (!f) return TY_INT;
    if (f->tag == F_SYM) {
        const char *n = f->as.sym->name;
        if (strcmp(n, "int")   == 0) return TY_INT;
        if (strcmp(n, "bool")  == 0) return TY_BOOL;
        if (strcmp(n, "float") == 0) return TY_FLOAT;
        if (strcmp(n, "cstr")  == 0) return TY_CSTR;
        if (strcmp(n, "ptr")   == 0) return TY_PTR_VOID;
        if (strcmp(n, "int8")  == 0) return TY_INT8;
        if (strcmp(n, "int16") == 0) return TY_INT16;
        if (strcmp(n, "int32") == 0) return TY_INT32;
        if (strcmp(n, "int64") == 0) return TY_INT64;
        if (strcmp(n, "uint8") == 0) return TY_UINT8;
        if (strcmp(n, "uint16")== 0) return TY_UINT16;
        if (strcmp(n, "uint32")== 0) return TY_UINT32;
        if (strcmp(n, "uint64")== 0) return TY_UINT64;
        if (strcmp(n, "float32")== 0) return TY_FLOAT32;
        if (strcmp(n, "float64")== 0) return TY_FLOAT64;
        /* Type variable or unknown type — opaque int64_t */
        return TY_INT;
    }
    /* List form like (Expr int) — ADT reference, opaque int64_t */
    return TY_INT;
}

/* Phase G1: defgadt — define a GADT (Generalized Algebraic Data Type).
 * Syntax: (defgadt Name [type-params...]
 *           (Ctor1 : return-type)
 *           (Ctor2 FieldType1 FieldType2 : return-type)
 *           ...)
 *
 * The ':' separator is a bare F_SYM(":") token.
 * Field types are forms appearing between the constructor name and ':'.
 * The return-type annotation is stored on the CtorDef for future G2 use.
 * Codegen is identical to defdata (tagged union).
 */
static Expr *elab_defgadt(Elab *e, const Form *call) {
    if (!g_gadt_enabled) {
        diag_emit(DIAG_ERROR, call->span,
                  "defgadt requires the -Xgadt flag to enable GADT support\n"
                  "  hint: recompile with -Xgadt");
        return NULL;
    }
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "defgadt requires a name, type-params, and constructors: "
                  "(defgadt Name [params] (Ctor : return-type) ...)");
        return NULL;
    }

    /* Parse name */
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span, "defgadt name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;

    /* Parse type parameters — must be a vector [a b c] */
    uint32_t ctors_start_idx = 2;
    uint8_t n_type_params = 0;
    const char **type_params = NULL;
    if (call->as.list.len >= 3 && call->as.list.items[2]->tag == F_VEC) {
        Form *params_form = call->as.list.items[2];
        n_type_params = (uint8_t)params_form->as.list.len;
        if (n_type_params > 0) {
            type_params = (const char **)arena_alloc(e->arena,
                                                      n_type_params * sizeof(const char *));
            for (uint8_t i = 0; i < n_type_params; i++) {
                Form *pf = params_form->as.list.items[i];
                if (pf->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, pf->span,
                              "defgadt: type parameter must be a symbol");
                    return NULL;
                }
                type_params[i] = pf->as.sym->name;
            }
        }
        ctors_start_idx = 3;
    }

    if (call->as.list.len <= ctors_start_idx) {
        diag_emit(DIAG_ERROR, call->span,
                  "defgadt: '%s' must have at least one constructor", name->name);
        return NULL;
    }

    /* Phase RF0: allow re-elaboration of forward-declared stub types */
    bool is_forward_stub_gadt = false;
    Binding *existing_gadt_b = scope_lookup(e->scope, name);
    if (existing_gadt_b) {
        if (elab_is_forward_type(e, name)) {
            is_forward_stub_gadt = true;
        } else {
            diag_emit(DIAG_ERROR, name_form->span,
                      "defgadt: '%s' is already defined", name->name);
            return NULL;
        }
    }

    uint32_t n_ctors = call->as.list.len - ctors_start_idx;

    /* Phase RF0: Allocate (or reuse forward stub) AdtDef and register BEFORE
     * parsing constructors so that self-referential and mutually-recursive
     * constructor field types resolve correctly. */
    AdtDef *def;
    Binding *adt_binding;
    Type adt_type;
    if (is_forward_stub_gadt) {
        /* Reuse the pre-registered stub and fill it in */
        adt_binding = existing_gadt_b;
        def = adt_binding->type.as.adt_.def;
        def->n_ctors = n_ctors;
        def->ctors = (CtorDef **)arena_alloc(e->arena, n_ctors * sizeof(CtorDef *));
        def->is_copy = false;
        def->needs_drop_glue = false;
        def->is_gadt = true;
        def->type_params = type_params;
        def->n_type_params = n_type_params;
        adt_type = type_adt(def);
        /* Phase G1/HKT: Apply the same KIND_ARROW fix as the non-stub branch so
         * that kind checks see the correct kind for parameterized GADTs. */
        if (n_type_params >= 2)     adt_type.hkt_kind = KIND_ARROW2;
        else if (n_type_params == 1) adt_type.hkt_kind = KIND_ARROW;
        adt_binding->type = adt_type;
        /* Already in global scope and elab registry from the pre-pass */
    } else {
        def = (AdtDef *)arena_alloc(e->arena, sizeof(AdtDef));
        def->name = name->name;
        def->n_ctors = n_ctors;
        def->ctors = (CtorDef **)arena_alloc(e->arena, n_ctors * sizeof(CtorDef *));
        def->is_copy = false;
        def->needs_drop_glue = false;
        def->is_gadt = true;
        def->type_params = type_params;
        def->n_type_params = n_type_params;

        /* Pre-register ADT type so constructors can reference it.
         * Phase G1/HKT: Set hkt_kind based on type-parameter count so that
         * the belt-and-suspenders kind check can detect when a parameterized
         * GADT is used in a kind-* argument slot of another GADT. */
        adt_type = type_adt(def);
        if (n_type_params >= 2)     adt_type.hkt_kind = KIND_ARROW2;
        else if (n_type_params == 1) adt_type.hkt_kind = KIND_ARROW;
        adt_binding = binding_new(e, name, adt_type, false, true, name_form->span);
        scope_add(&e->global, adt_binding);
    }

    /* Parse each constructor */
    for (uint32_t ci = 0; ci < n_ctors; ci++) {
        Form *ctor_form = call->as.list.items[ctors_start_idx + ci];
        if (ctor_form->tag != F_LIST || ctor_form->as.list.len < 1) {
            diag_emit(DIAG_ERROR, ctor_form->span,
                      "defgadt: constructor must be a list form");
            return NULL;
        }
        Form *ctor_name_form = ctor_form->as.list.items[0];
        if (ctor_name_form->tag != F_SYM) {
            diag_emit(DIAG_ERROR, ctor_name_form->span,
                      "defgadt: constructor name must be a symbol");
            return NULL;
        }
        const Symbol *ctor_name = ctor_name_form->as.sym;

        /* Find the ':' separator in the constructor form.
         * Format: (CtorName FieldType1 ... FieldTypeN : return-type-form)
         * The ':' may be a bare F_SYM(":") token (legacy) or an F_TYPE_ANN node
         * produced by the new `: type-expr` reader (Phase G3 compat). */
        int colon_idx = -1;
        bool type_ann_colon = false; /* true when the ':' was absorbed into F_TYPE_ANN */
        for (uint32_t fi = 1; fi < ctor_form->as.list.len; fi++) {
            Form *item = ctor_form->as.list.items[fi];
            if (item->tag == F_SYM && item->as.sym == e->sym_colon) {
                colon_idx = (int)fi;
                break;
            }
            if (item->tag == F_TYPE_ANN) {
                /* `: return-type` was folded into a single F_TYPE_ANN node.
                 * Treat this index as the separator; the return type is inside. */
                colon_idx = (int)fi;
                type_ann_colon = true;
                break;
            }
        }
        if (colon_idx < 0) {
            diag_emit(DIAG_ERROR, ctor_form->span,
                      "defgadt: constructor '%s' requires an explicit return-type annotation\n"
                      "  hint: add ': (return-type)' after the constructor name",
                      ctor_name->name);
            return NULL;
        }
        /* For the F_TYPE_ANN case the return-type form is the inner form;
         * for the bare-':' case it is the item immediately after. */
        Form *return_type_form;
        if (type_ann_colon) {
            Form *ann = ctor_form->as.list.items[colon_idx];
            if (ann->as.list.len < 1) {
                diag_emit(DIAG_ERROR, ann->span,
                          "defgadt: constructor '%s': missing return type after ':'",
                          ctor_name->name);
                return NULL;
            }
            return_type_form = ann->as.list.items[0];
        } else {
            if ((uint32_t)colon_idx + 1 >= ctor_form->as.list.len) {
                diag_emit(DIAG_ERROR, ctor_form->span,
                          "defgadt: constructor '%s': missing return type after ':'",
                          ctor_name->name);
                return NULL;
            }
            return_type_form = ctor_form->as.list.items[colon_idx + 1];
        }
        /* Validate: return type must mention the GADT name */
        bool mentions_gadt = false;
        if (return_type_form->tag == F_LIST && return_type_form->as.list.len >= 1) {
            Form *head = return_type_form->as.list.items[0];
            if (head->tag == F_SYM && strcmp(head->as.sym->name, name->name) == 0) {
                mentions_gadt = true;
            }
        } else if (return_type_form->tag == F_SYM &&
                   strcmp(return_type_form->as.sym->name, name->name) == 0) {
            mentions_gadt = true;
        }
        if (!mentions_gadt) {
            diag_emit(DIAG_ERROR, return_type_form->span,
                      "defgadt: constructor '%s' return type must be an application of '%s'",
                      ctor_name->name, name->name);
            return NULL;
        }

        /* Change 3: Validate that the number of type args in the return type
         * matches the GADT's declared type-parameter count. */
        if (return_type_form->tag == F_LIST) {
            uint32_t n_rt_args = return_type_form->as.list.len - 1; /* subtract head */
            if (n_type_params > 0 && n_rt_args != n_type_params) {
                diag_emit(DIAG_ERROR, return_type_form->span,
                          "defgadt: constructor '%s' return type has %u type argument(s) "
                          "but '%s' has %u type parameter(s)",
                          ctor_name->name, n_rt_args, name->name, n_type_params);
                return NULL;
            }
        }

        /* Change 4: Validate each type arg in the return type is a known type. */
        if (return_type_form->tag == F_LIST) {
            for (uint32_t ai = 1; ai < return_type_form->as.list.len; ai++) {
                Form *arg = return_type_form->as.list.items[ai];
                if (arg->tag == F_LIST) continue; /* type application -- ok */
                if (arg->tag != F_SYM) continue;  /* other forms -- ok */
                const char *an = arg->as.sym->name;
                /* Check if it's a bound type param */
                bool is_param = false;
                for (uint8_t pi = 0; pi < n_type_params; pi++) {
                    if (strcmp(type_params[pi], an) == 0) { is_param = true; break; }
                }
                if (is_param) continue;
                /* Check if it's a concrete primitive */
                static const char *primitives[] = {
                    "int", "bool", "float", "cstr", "nil", "void", "ptr",
                    "int8", "int16", "int32", "int64",
                    "uint8", "uint16", "uint32", "uint64",
                    "float32", "float64", NULL
                };
                bool is_prim = false;
                for (int pi = 0; primitives[pi]; pi++) {
                    if (strcmp(primitives[pi], an) == 0) { is_prim = true; break; }
                }
                if (is_prim) continue;
                /* Check if it's a known type in scope */
                const Symbol *type_sym = symtab_intern(e->st,
                    strslice(an, (uint32_t)strlen(an)));
                Binding *tb = scope_lookup_type_def(e->scope, type_sym);
                if (tb && (tb->type.kind == TY_STRUCT || tb->type.kind == TY_ADT)) {
                    /* Phase G1/HKT: Belt-and-suspenders kind check.
                     * All GADT type parameters have kind * in Phase G1.  A
                     * type constructor of kind * -> * (hkt_kind == KIND_ARROW)
                     * in a plain type-argument slot is a kind mismatch. */
                    if (tb->type.hkt_kind == KIND_ARROW ||
                            tb->type.hkt_kind == KIND_ARROW2) {
                        diag_emit_with_code(DIAG_ERROR, arg->span,
                            TUR_E0012_KIND_MISMATCH,
                            "kind mismatch (TUR-E0012): type argument '%s' in constructor "
                            "'%s' return type has kind '* -> *' but kind '*' is expected",
                            an, ctor_name->name);
                        return NULL;
                    }
                    continue;
                }
                /* Unknown -- error */
                diag_emit(DIAG_ERROR, arg->span,
                          "defgadt: unknown type argument '%s' in return type of constructor '%s' "
                          "(must be a type parameter, primitive type, or defined type)",
                          an, ctor_name->name);
                return NULL;
            }
        }

        /* Field types: items[1 .. colon_idx-1] */
        uint32_t n_fields = (uint32_t)(colon_idx - 1);
        CtorDef *ctor = (CtorDef *)arena_alloc(e->arena, sizeof(CtorDef));
        ctor->name = ctor_name->name;
        ctor->n_fields = n_fields;
        ctor->fields = n_fields > 0
            ? (CtorField *)arena_alloc(e->arena, n_fields * sizeof(CtorField))
            : NULL;
        ctor->adt = def;
        ctor->tag = ci;
        ctor->result_type_form = return_type_form;
        /* Phase G2: store raw field-type annotation forms for per-arm resolution */
        ctor->field_forms = n_fields > 0
            ? (const struct Form **)arena_alloc(e->arena, n_fields * sizeof(const Form *))
            : NULL;

        for (uint32_t fi = 0; fi < n_fields; fi++) {
            Form *ft_form = ctor_form->as.list.items[1 + fi];
            TypeKind fkind = gadt_field_typekind_from_form(ft_form);
            ctor->fields[fi].kind = fkind;
            ctor->fields[fi].inner_kind = TY_UNKNOWN;
            if (fkind == TY_RC || fkind == TY_REF || fkind == TY_WEAK) {
                def->needs_drop_glue = true;
            }
            /* Phase G2: also stash the raw form for per-arm type resolution */
            if (ctor->field_forms) ctor->field_forms[fi] = ft_form;
        }
        def->ctors[ci] = ctor;

        /* Register constructor binding */
        if (n_fields == 0) {
            Binding *cb = binding_new(e, ctor_name, adt_type, false, true,
                                      ctor_name_form->span);
            scope_add(&e->global, cb);
        } else {
            TypeKind arg_kinds[MAX_FN_ARITY];
            uint8_t arity = (uint8_t)(n_fields > MAX_FN_ARITY ? MAX_FN_ARITY : n_fields);
            for (uint8_t fi = 0; fi < arity; fi++) {
                arg_kinds[fi] = ctor->fields[fi].kind;
            }
            Type fn_type = type_fn(arg_kinds, arity, TY_ADT);
            Binding *cb = binding_new(e, ctor_name, fn_type, false, true,
                                      ctor_name_form->span);
            scope_add(&e->global, cb);
        }
    }

    /* Register ADT in elab registry (skip if it was a forward stub -- already registered) */
    if (!is_forward_stub_gadt) {
        elab_register_adt_def(e, def);
    }

    Expr *out = expr_new(e->arena, EX_DEFGADT, TYPE_NIL, call->span);
    out->as.defgadt_.def = def;
    out->as.defgadt_.binding = adt_binding;
    return out;
}

/* Phase G3: coerce — (coerce eq x) where eq : (Equal a b), x : a → x : b
 * Zero-cost cast: the runtime representation of a and b are identical (both int64_t).
 * The equality proof eq is evaluated for side-effects but its value is discarded.
 * Error if eq is not a value of the built-in Equal GADT. */
static Expr *elab_coerce(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "coerce requires exactly 2 arguments: (coerce eq x)");
        return NULL;
    }
    Expr *eq = elab_form(e, call->as.list.items[1]);
    if (!eq) return NULL;
    /* Verify eq has type Equal */
    if (eq->type.kind != TY_ADT ||
        !eq->type.as.adt_.def ||
        strcmp(eq->type.as.adt_.def->name, "Equal") != 0) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "coerce requires an (Equal a b) proof as first argument");
        return NULL;
    }
    Expr *x = elab_form(e, call->as.list.items[2]);
    if (!x) return NULL;
    /* Zero-cost cast: return x unchanged (same runtime representation) */
    return x;
}

/* Phase G0: Helper - look up CtorDef by name across all known ADTs */
static CtorDef *elab_lookup_ctor(Elab *e, const Symbol *name) {
    for (uint32_t ai = 0; ai < e->n_adt_defs; ai++) {
        AdtDef *adt = e->adt_defs[ai];
        for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
            if (strcmp(adt->ctors[ci]->name, name->name) == 0) {
                return adt->ctors[ci];
            }
        }
    }
    return NULL;
}

/* Phase G0: match expression
 * Syntax: (match scrutinee
 *   (Ctor1 x y) body1
 *   (Ctor2 z)   body2
 *   _           default-body)
 * Arms are interleaved: pattern body pattern body ...
 */
static Expr *elab_match(Elab *e, const Form *call) {
    /* call->as.list.items[0] = "match"
     * call->as.list.items[1] = scrutinee
     * call->as.list.items[2..] = pattern body pattern body ... */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "match requires a scrutinee: (match scrutinee pattern body ...)");
        return NULL;
    }
    /* Phase G4: Pre-scan arms to count and find per-arm start indices.
     * Arms can be (pat body) or (pat when guard body). */
    uint32_t arm_start[256];    /* start index of each arm's pattern */
    bool arm_has_guard[256];    /* whether the arm has a when-guard */
    uint32_t n_arms = 0;
    {
        uint32_t idx = 2; /* skip 'match' and scrutinee */
        while (idx < call->as.list.len) {
            if (n_arms >= 256) {
                diag_emit(DIAG_ERROR, call->span, "match: too many arms (max 256)");
                return NULL;
            }
            arm_start[n_arms] = idx;
            arm_has_guard[n_arms] = false;
            idx++; /* skip pattern */
            /* Check for optional 'when guard' */
            if (idx + 1 < call->as.list.len &&
                call->as.list.items[idx]->tag == F_SYM &&
                call->as.list.items[idx]->as.sym == e->sym_when) {
                arm_has_guard[n_arms] = true;
                idx += 2; /* skip 'when' and guard expr */
            }
            idx++; /* skip body */
            n_arms++;
        }
        /* Verify last arm ends exactly at list end */
        if (idx != call->as.list.len) {
            diag_emit(DIAG_ERROR, call->span,
                      "match: malformed arm list (missing body for last arm?)");
            return NULL;
        }
    }
    if (n_arms == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "match requires at least one arm: (match scrutinee pattern body)");
        return NULL;
    }

    /* Elaborate scrutinee */
    Expr *scrutinee = elab_form(e, call->as.list.items[1]);
    if (!scrutinee) return NULL;

    /* IT1: Union type match — when scrutinee is TY_UNION, handle type-narrowing patterns.
     * Pattern syntax: (varname : TypeName) or bare _ / variable for wildcard.
     * Returns early via the union match path. */
    if (g_union_types_enabled && scrutinee->type.kind == TY_UNION) {
        Type *union_t = &scrutinee->type;
        uint8_t n_members = union_t->as.union_.n_members;

        /* Track which union members are covered */
        bool *member_covered = (bool *)calloc(n_members, sizeof(bool));
        bool has_wildcard = false;
        Type result_type = TYPE_UNKNOWN;

        MatchArm *arms = (MatchArm *)arena_alloc(e->arena, n_arms * sizeof(MatchArm));

        for (uint32_t ai = 0; ai < n_arms; ai++) {
            Form *pat_form = call->as.list.items[2 + ai * 2];
            Form *body_form = call->as.list.items[3 + ai * 2];
            MatchPattern *pat = &arms[ai].pattern;
            memset(pat, 0, sizeof(MatchPattern));

            /* Wildcard: bare _ or bare variable symbol */
            if (pat_form->tag == F_SYM) {
                const Symbol *sym_wildcard = intern_cstr(e->st, "_");
                if (pat_form->as.sym == sym_wildcard) {
                    pat->is_wildcard = true;
                } else {
                    pat->is_var = true;
                    pat->var_sym = pat_form->as.sym;
                }
                pat->union_member_idx = -1; /* IT4: wildcard arm — no specific member */
                has_wildcard = true;
                Expr *body = elab_form(e, body_form);
                if (!body) { free(member_covered); return NULL; }
                arms[ai].body = body;
                if (result_type.kind == TY_UNKNOWN) result_type = body->type;
                continue;
            }

            /* Type-narrowing pattern: (varname : TypeName)
             * The reader collapses ': TypeName' into a single F_TYPE_ANN node, so the
             * list has 2 items: [F_SYM varname, F_TYPE_ANN{inner}].
             * The 3-item form [F_SYM varname, F_SYM ":", F_SYM TypeName] is also
             * supported for defgadt-style bare colon separators. */
            bool is_type_narrowing_2 = (pat_form->tag == F_LIST &&
                pat_form->as.list.len == 2 &&
                pat_form->as.list.items[0]->tag == F_SYM &&
                pat_form->as.list.items[1]->tag == F_TYPE_ANN);
            bool is_type_narrowing_3 = (pat_form->tag == F_LIST &&
                pat_form->as.list.len == 3 &&
                pat_form->as.list.items[0]->tag == F_SYM &&
                pat_form->as.list.items[1]->tag == F_SYM &&
                pat_form->as.list.items[1]->as.sym == e->sym_colon);
            if (is_type_narrowing_2 || is_type_narrowing_3) {
                Form *var_form  = pat_form->as.list.items[0];
                Form *type_form = is_type_narrowing_2
                    ? pat_form->as.list.items[1]->as.list.items[0]  /* inner of F_TYPE_ANN */
                    : pat_form->as.list.items[2];
                /* Parse narrowed type */
                Type *narrowed = type_expr_from_form(e, type_form, NULL, NULL, NULL, 0);
                if (!narrowed) { free(member_covered); return NULL; }

                /* Find which union member this pattern covers */
                int covered_member = -1;
                for (uint8_t um = 0; um < n_members; um++) {
                    if (union_t->as.union_.members[um] &&
                        type_eq(*narrowed, *union_t->as.union_.members[um])) {
                        covered_member = (int)um;
                        break;
                    }
                }
                if (covered_member < 0) {
                    diag_emit_with_code(DIAG_ERROR, pat_form->span,
                                        TUR_E0300_UNION_TYPE_MISMATCH,
                                        "match: type '%s' is not a member of union '%s'",
                                        type_name(*narrowed), type_name(*union_t));
                    free(member_covered);
                    return NULL;
                }
                member_covered[covered_member] = true;
                pat->union_member_idx = covered_member; /* IT4: record for tag-dispatch in emit.c */

                /* Introduce arm scope with the narrowed binding */
                Scope arm_scope;
                scope_init(&arm_scope, e->scope);
                Scope *saved_scope = e->scope;
                e->scope = &arm_scope;

                Binding *var_b = binding_new(e, var_form->as.sym, *narrowed,
                                             false, false, var_form->span);
                scope_add(&arm_scope, var_b);

                Expr *body = elab_form(e, body_form);
                e->scope = saved_scope;
                if (!body) { free(member_covered); return NULL; }

                /* Record the binding in the pattern */
                pat->is_var = true;
                pat->var_sym = var_form->as.sym;
                pat->n_bindings = 1;
                pat->bindings = (Binding **)arena_alloc(e->arena, sizeof(Binding *));
                pat->bindings[0] = var_b;

                arms[ai].body = body;
                if (result_type.kind == TY_UNKNOWN) result_type = body->type;
                continue;
            }

            /* Unrecognized pattern for union match */
            diag_emit(DIAG_ERROR, pat_form->span,
                      "match on union type: expected '(varname : Type)' or wildcard '_', got unexpected pattern");
            free(member_covered);
            return NULL;
        }

        /* Exhaustiveness check: every union member must be covered */
        if (!has_wildcard) {
            for (uint8_t um = 0; um < n_members; um++) {
                if (!member_covered[um] && union_t->as.union_.members[um]) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                                        TUR_E0301_NON_EXHAUSTIVE_UNION_MATCH,
                                        "match on union type '%s': missing arm for member '%s'",
                                        type_name(*union_t),
                                        type_name(*union_t->as.union_.members[um]));
                    free(member_covered);
                    return NULL;
                }
            }
        }
        free(member_covered);

        if (result_type.kind == TY_UNKNOWN) result_type = TYPE_NIL;
        Expr *out = expr_new(e->arena, EX_MATCH, result_type, call->span);
        out->as.match_.scrutinee = scrutinee;
        out->as.match_.arms = arms;
        out->as.match_.n_arms = n_arms;
        return out;
    }

    /* If the scrutinee type is not already TY_ADT (e.g. an untyped param
     * that defaulted to TY_INT), or is TY_ADT with no def (e.g. return of
     * ADT-returning fn without result_full_type), infer the ADT from the
     * first constructor pattern in the arm list. */
    if (scrutinee->type.kind != TY_ADT || !scrutinee->type.as.adt_.def) {
        AdtDef *inferred_adt = NULL;
        for (uint32_t ai = 0; ai < n_arms && !inferred_adt; ai++) {
            Form *pat_f = call->as.list.items[arm_start[ai]];
            if (pat_f->tag == F_LIST && pat_f->as.list.len >= 1 &&
                pat_f->as.list.items[0]->tag == F_SYM) {
                CtorDef *cd = elab_lookup_ctor(e, pat_f->as.list.items[0]->as.sym);
                if (cd) inferred_adt = cd->adt;
            }
        }
        if (inferred_adt) {
            /* Patch the scrutinee type to the inferred ADT */
            scrutinee->type = type_adt(inferred_adt);
        } else {
            diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                      "match: scrutinee must be an ADT type, got %s",
                      typekind_to_string(scrutinee->type.kind));
            return NULL;
        }
    }

    AdtDef *adt = scrutinee->type.as.adt_.def;

    /* Allocate arms array */
    MatchArm *arms = (MatchArm *)arena_alloc(e->arena, n_arms * sizeof(MatchArm));

    /* Track covered constructors for exhaustiveness */
    bool *covered = (bool *)calloc(adt->n_ctors, sizeof(bool));
    bool has_wildcard = false;

    Type result_type = TYPE_UNKNOWN;

    /* LT1: Snapshot outer-scope linear consumed state before match arms.
     * We restore before each arm's body and verify consistency across arms at the end. */
    Binding **match_lin_bindings = NULL;
    bool *match_lin_before = NULL;
    uint32_t n_match_lin = 0;
    bool **arm_lin_states = NULL;
    bool *arm_diverges = NULL;
    if (g_linear_enabled) {
        n_match_lin = linear_state_snapshot_bindings(e->scope, &match_lin_bindings,
                                                     &match_lin_before);
        if (n_match_lin > 0) {
            arm_lin_states = (bool **)calloc(n_arms, sizeof(bool *));
            arm_diverges   = (bool  *)calloc(n_arms, sizeof(bool));
        }
    }

    for (uint32_t ai = 0; ai < n_arms; ai++) {
        uint32_t base = arm_start[ai];
        Form *pat_form = call->as.list.items[base];
        Form *guard_form_raw = arm_has_guard[ai]
            ? call->as.list.items[base + 2]
            : NULL;
        Form *body_form = call->as.list.items[arm_has_guard[ai] ? base + 3 : base + 1];

        MatchPattern *pat = &arms[ai].pattern;
        memset(pat, 0, sizeof(MatchPattern));
        arms[ai].guard = NULL;

        if (pat_form->tag == F_SYM) {
            /* Bare symbol: either _ wildcard or variable capture */
            const Symbol *sym = pat_form->as.sym;
            /* intern "_" */
            const Symbol *sym_wildcard = intern_cstr(e->st, "_");
            if (sym == sym_wildcard) {
                pat->is_wildcard = true;
                has_wildcard = true;
            } else {
                /* Variable binding — captures entire scrutinee */
                pat->is_var = true;
                pat->var_sym = sym;
                has_wildcard = true; /* covers all remaining */
            }
            /* No new scope needed; elaborate body directly */
            /* LT1: Restore outer linear state before this arm's body. */
            if (g_linear_enabled && n_match_lin > 0) {
                linear_state_restore(match_lin_bindings, match_lin_before, n_match_lin);
            }
            Expr *body = elab_form(e, body_form);
            if (!body) { free(covered); return NULL; }
            /* LT1: Capture outer linear state after this arm's body. */
            if (g_linear_enabled && n_match_lin > 0) {
                arm_lin_states[ai] = linear_state_capture_current(match_lin_bindings, n_match_lin);
                arm_diverges[ai] = (body->type.kind == TY_NEVER) ||
                                   (body->kind == EX_RETURN) ||
                                   (body->kind == EX_PANIC)  ||
                                   (body->kind == EX_PANIC_WITH);
                linear_state_restore(match_lin_bindings, match_lin_before, n_match_lin);
            }
            /* Phase G0: Arm body type consistency check for wildcard/variable arms. */
            if (result_type.kind != TY_UNKNOWN
                    && body->type.kind != TY_UNKNOWN
                    && !type_eq(result_type, body->type)) {
                diag_emit_with_code(DIAG_ERROR, body_form->span,
                                    TUR_E0001_TYPE_MISMATCH,
                                    "match: arm types are incompatible -- "
                                    "expected %s (from earlier arm), got %s",
                                    typekind_to_string(result_type.kind),
                                    typekind_to_string(body->type.kind));
                free(covered); return NULL;
            }
            arms[ai].body = body;
            if (result_type.kind == TY_UNKNOWN) result_type = body->type;
        } else if (pat_form->tag == F_LIST) {
            /* Constructor pattern: (CtorName var1 var2 ...) */
            if (pat_form->as.list.len < 1) {
                diag_emit(DIAG_ERROR, pat_form->span,
                          "match: empty constructor pattern");
                free(covered); return NULL;
            }
            Form *ctor_name_form = pat_form->as.list.items[0];
            if (ctor_name_form->tag != F_SYM) {
                diag_emit(DIAG_ERROR, ctor_name_form->span,
                          "match: constructor pattern must start with a constructor name");
                free(covered); return NULL;
            }
            const Symbol *ctor_sym = ctor_name_form->as.sym;
            /* Look up constructor in this ADT */
            CtorDef *ctor = NULL;
            for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
                if (strcmp(adt->ctors[ci]->name, ctor_sym->name) == 0) {
                    ctor = adt->ctors[ci];
                    break;
                }
            }
            if (!ctor) {
                diag_emit(DIAG_ERROR, ctor_name_form->span,
                          "match: '%s' is not a constructor of '%s'",
                          ctor_sym->name, adt->name);
                free(covered); return NULL;
            }

            uint32_t n_bindings = pat_form->as.list.len - 1;
            if (n_bindings != ctor->n_fields) {
                diag_emit(DIAG_ERROR, pat_form->span,
                          "match: constructor '%s' expects %u fields, got %u",
                          ctor->name, ctor->n_fields, n_bindings);
                free(covered); return NULL;
            }

            pat->ctor = ctor;
            pat->n_bindings = n_bindings;
            pat->bindings = n_bindings > 0
                ? (Binding **)arena_alloc(e->arena, n_bindings * sizeof(Binding *))
                : NULL;

            /* Phase G0: Redundant arm warning -- emit a warning if this constructor
             * was already covered by an earlier non-guarded arm.  We still
             * elaborate the body so any errors inside it are reported. */
            if (covered[ctor->tag]) {
                diag_emit(DIAG_WARNING, pat_form->span,
                          "match: arm for constructor '%s' is unreachable -- "
                          "already covered by an earlier arm",
                          ctor->name);
            }

            /* Phase G4: guarded arm doesn't guarantee coverage */
            covered[ctor->tag] = !arm_has_guard[ai];

            /* Phase G2: For GADT arms, build a per-arm SkolemEnv to resolve
             * type-variable field types to concrete kinds. */
            SkolemEnv arm_senv;
            arm_senv.n = 0;
            if (adt->is_gadt && ctor->result_type_form) {
                gadt_build_skolem_env(&arm_senv, adt, ctor);
            }
            SkolemEnv *saved_senv = e->g2_skolem_env;
            const CtorDef *saved_ctor = e->g2_current_ctor;
            if (adt->is_gadt) {
                e->g2_skolem_env = &arm_senv;
                e->g2_current_ctor = ctor;
            }

            /* Introduce a new scope with the field bindings */
            Scope arm_scope;
            scope_init(&arm_scope, e->scope);
            Scope *saved_scope = e->scope;
            e->scope = &arm_scope;

            for (uint32_t bi = 0; bi < n_bindings; bi++) {
                Form *var_form = pat_form->as.list.items[1 + bi];
                if (var_form->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, var_form->span,
                              "match: field binding must be a symbol");
                    e->scope = saved_scope;
                    scope_free(&arm_scope);
                    e->g2_skolem_env = saved_senv;
                    e->g2_current_ctor = saved_ctor;
                    free(covered); return NULL;
                }
                Type ftype;
                if (adt->is_gadt && ctor->field_forms && ctor->field_forms[bi]) {
                    /* Phase G2: resolve field type using the skolem env */
                    ftype = gadt_resolve_type_from_form(e, adt,
                                                        ctor->field_forms[bi], &arm_senv);
                } else {
                    ftype = type_from_kind(ctor->fields[bi].kind);
                }
                Binding *fb = binding_new(e, var_form->as.sym, ftype, false, false,
                                          var_form->span);
                /* LT1: Propagate linearity from the field's type to its binding */
                if (g_linear_enabled && ftype.copy_kind == CK_LINEAR) {
                    fb->is_linear = true;
                }
                scope_add(&arm_scope, fb);
                pat->bindings[bi] = fb;
            }

            /* LT1: Restore outer linear state before this arm's body. */
            if (g_linear_enabled && n_match_lin > 0) {
                linear_state_restore(match_lin_bindings, match_lin_before, n_match_lin);
            }
            Expr *body = elab_form(e, body_form);

            /* Phase G4: Elaborate optional when-guard while arm scope is still live */
            Expr *guard_expr = NULL;
            if (arm_has_guard[ai] && guard_form_raw) {
                guard_expr = elab_form(e, guard_form_raw);
                if (!guard_expr) {
                    e->scope = saved_scope;
                    scope_free(&arm_scope);
                    e->g2_skolem_env = saved_senv;
                    e->g2_current_ctor = saved_ctor;
                    free(covered); return NULL;
                }
                if (guard_expr->type.kind != TY_BOOL) {
                    diag_emit(DIAG_ERROR, guard_form_raw->span,
                              "match: when-guard must have type bool, got %s",
                              typekind_to_string(guard_expr->type.kind));
                    e->scope = saved_scope;
                    scope_free(&arm_scope);
                    e->g2_skolem_env = saved_senv;
                    e->g2_current_ctor = saved_ctor;
                    free(covered); return NULL;
                }
            }

            e->scope = saved_scope;
            scope_free(&arm_scope);
            e->g2_skolem_env = saved_senv;
            e->g2_current_ctor = saved_ctor;
            /* LT1: Verify all linear field bindings in this arm were consumed */
            bool lt1_arm_fail = false;
            if (g_linear_enabled && body) {
                for (uint32_t bi2 = 0; bi2 < n_bindings; bi2++) {
                    Binding *fb2 = pat->bindings[bi2];
                    if (fb2->is_linear && !fb2->is_linear_consumed && !fb2->is_moved) {
                        diag_emit_with_code(DIAG_ERROR, fb2->span,
                                            TUR_E0100_LINEAR_DROPPED,
                                            "linear field '%s' dropped without being consumed in match arm",
                                            fb2->name->name);
                        lt1_arm_fail = true;
                    }
                }
            }
            /* ST1: Verify all relevant field bindings in this arm were used */
            bool st1_arm_fail = false;
            if (g_substructural_enabled && body) {
                for (uint32_t bi2 = 0; bi2 < n_bindings; bi2++) {
                    Binding *fb2 = pat->bindings[bi2];
                    if (fb2->is_relevant && fb2->usage_state == USAGE_UNUSED && !fb2->is_moved) {
                        diag_emit_with_code(DIAG_ERROR, fb2->span,
                                            TUR_E0151_RELEVANT_DROPPED,
                                            "relevant field '%s' dropped without being used in match arm",
                                            fb2->name->name);
                        st1_arm_fail = true;
                    }
                }
            }
            /* Phase G2: GADT constructor context -- when body elaboration fails
             * inside a GADT arm, emit a note naming the constructor whose
             * return-type annotation caused the type refinement and listing the
             * active skolem equalities (e.g. "in this arm: a ~ int").
             * This surfaces the "why" behind type errors that occur because of
             * GADT-induced substitutions. */
            if (!body && adt->is_gadt && arm_senv.n > 0) {
                char skolem_note[256];
                int pos = 0;
                for (uint8_t si = 0; si < arm_senv.n && pos < 230; si++) {
                    if (si > 0) pos += snprintf(skolem_note + pos,
                                                sizeof(skolem_note) - pos, ", ");
                    pos += snprintf(skolem_note + pos, sizeof(skolem_note) - pos,
                                   "%s ~ %s",
                                   arm_senv.bindings[si].name,
                                   typekind_to_string(arm_senv.bindings[si].kind));
                }
                skolem_note[pos] = '\0';
                diag_emit(DIAG_NOTE, body_form->span,
                          "inside arm for constructor '%s' of '%s'; "
                          "active refinements: %s",
                          ctor->name, adt->name, skolem_note);
            }
            if (!body || lt1_arm_fail || st1_arm_fail) { free(covered); return NULL; }

            /* Phase G2/HRT: detect skolem escape -- arm body result is anonymous TY_TYVAR.
             * Named TY_TYVAR (type variable from a :a parameter annotation) is a properly
             * polymorphic result and is allowed; only anonymous TY_TYVAR (unresolved field
             * type with no name) indicates a skolem that escaped its scope. */
            if (adt->is_gadt && body->type.kind == TY_TYVAR
                    && body->type.as.tyvar_.name == NULL) {
                diag_emit(DIAG_ERROR, body_form->span,
                          "match: skolem type variable escapes match arm "
                          "(constructor '%s' of '%s'): the arm body has an "
                          "unresolved GADT type variable as its result type",
                          ctor->name, adt->name);
                free(covered); return NULL;
            }

            /* Phase G0/G2: Arm body type consistency check.
             * All arms of a match expression must return the same type.
             * If this arm's body type differs from the type established by the
             * first arm, emit TUR_E0001_TYPE_MISMATCH.  For GADT arms, also
             * emit a note listing the active skolem equalities so the user can
             * see which type refinement is in effect. */
            if (result_type.kind != TY_UNKNOWN
                    && body->type.kind != TY_UNKNOWN
                    && !type_eq(result_type, body->type)) {
                if (adt->is_gadt && arm_senv.n > 0) {
                    char skolem_note[256];
                    int pos = 0;
                    for (uint8_t si = 0; si < arm_senv.n && pos < 230; si++) {
                        if (si > 0) pos += snprintf(skolem_note + pos,
                                                    sizeof(skolem_note) - pos, ", ");
                        pos += snprintf(skolem_note + pos, sizeof(skolem_note) - pos,
                                       "%s ~ %s",
                                       arm_senv.bindings[si].name,
                                       typekind_to_string(arm_senv.bindings[si].kind));
                    }
                    skolem_note[pos] = '\0';
                    diag_emit(DIAG_NOTE, body_form->span,
                              "inside arm for constructor '%s' of '%s'; "
                              "active refinements: %s",
                              ctor->name, adt->name, skolem_note);
                }
                diag_emit_with_code(DIAG_ERROR, body_form->span,
                                    TUR_E0001_TYPE_MISMATCH,
                                    "match: arm types are incompatible -- "
                                    "expected %s (from earlier arm), got %s",
                                    typekind_to_string(result_type.kind),
                                    typekind_to_string(body->type.kind));
                free(covered); return NULL;
            }

            arms[ai].body = body;
            arms[ai].guard = guard_expr;
            if (result_type.kind == TY_UNKNOWN) result_type = body->type;
            /* LT1: Capture outer linear state after this arm's body. */
            if (g_linear_enabled && n_match_lin > 0) {
                arm_lin_states[ai] = linear_state_capture_current(match_lin_bindings, n_match_lin);
                arm_diverges[ai] = (body->type.kind == TY_NEVER) ||
                                   (body->kind == EX_RETURN) ||
                                   (body->kind == EX_PANIC)  ||
                                   (body->kind == EX_PANIC_WITH);
                linear_state_restore(match_lin_bindings, match_lin_before, n_match_lin);
            }
        } else {
            diag_emit(DIAG_ERROR, pat_form->span,
                      "match: pattern must be a constructor list or _ wildcard");
            free(covered); return NULL;
        }
    }

    /* LT1: Verify consistent outer-scope linear consumption across match arms */
    if (g_linear_enabled && n_match_lin > 0 && arm_lin_states) {
        /* Find the first non-diverging arm as the reference. */
        int ref_ai = -1;
        for (uint32_t ai = 0; ai < n_arms; ai++) {
            if (arm_lin_states[ai] && !arm_diverges[ai]) {
                ref_ai = (int)ai; break;
            }
        }
        bool lin_ok = true;
        if (ref_ai >= 0) {
            for (uint32_t ai = (uint32_t)(ref_ai + 1); ai < n_arms; ai++) {
                if (!arm_lin_states[ai] || arm_diverges[ai]) continue;
                for (uint32_t li = 0; li < n_match_lin; li++) {
                    if (!match_lin_before[li] &&
                        arm_lin_states[ai][li] != arm_lin_states[ref_ai][li]) {
                        diag_emit_with_code(DIAG_ERROR, call->span,
                                            TUR_E0104_LINEAR_BRANCH_MISMATCH,
                                            "linear value '%s' consumed in some match arms but "
                                            "not others -- consume it in all arms or none",
                                            match_lin_bindings[li]->name->name);
                        lin_ok = false;
                    }
                }
            }
        }
        /* Merge: consumed after match only if all non-diverging arms consumed it. */
        for (uint32_t li = 0; li < n_match_lin; li++) {
            match_lin_bindings[li]->is_linear_consumed =
                (ref_ai >= 0) ? arm_lin_states[ref_ai][li] : false;
        }
        for (uint32_t ai = 0; ai < n_arms; ai++) free(arm_lin_states[ai]);
        free(arm_lin_states);
        free(arm_diverges);
        free(match_lin_before);
        free(match_lin_bindings);
        if (!lin_ok) { free(covered); return NULL; }
    }

    /* Exhaustiveness check */
    if (!has_wildcard) {
        /* Phase G2: For GADT ADTs, warn about unreachable constructors rather
         * than erroring — a constructor whose concrete type-argument instantiation
         * differs from every covered arm's instantiation is unreachable.
         * An uncovered constructor that IS reachable (or whose reachability is
         * unknown) is still an error. */
        if (adt->is_gadt) {
            /* Collect the first type-arg string from each covered arm's result type. */
            const char *covered_arg0[64]; /* max constructors we check */
            uint32_t n_covered = 0;
            for (uint32_t ci = 0; ci < adt->n_ctors && n_covered < 64; ci++) {
                if (!covered[ci]) continue;
                CtorDef *c = adt->ctors[ci];
                const char *a0 = NULL;
                if (c->result_type_form && c->result_type_form->tag == F_LIST
                    && c->result_type_form->as.list.len >= 2) {
                    Form *arg = c->result_type_form->as.list.items[1];
                    if (arg->tag == F_SYM) a0 = arg->as.sym->name;
                }
                covered_arg0[n_covered++] = a0;
            }
            for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
                if (covered[ci]) continue;
                CtorDef *c = adt->ctors[ci];
                /* Extract first type arg from this constructor's return type */
                const char *my_a0 = NULL;
                if (c->result_type_form && c->result_type_form->tag == F_LIST
                    && c->result_type_form->as.list.len >= 2) {
                    Form *arg = c->result_type_form->as.list.items[1];
                    if (arg->tag == F_SYM) my_a0 = arg->as.sym->name;
                }
                /* Check if my_a0 conflicts with all covered arms */
                bool all_covered_differ = (n_covered > 0) && (my_a0 != NULL);
                for (uint32_t k = 0; k < n_covered && all_covered_differ; k++) {
                    if (covered_arg0[k] == NULL ||
                        strcmp(covered_arg0[k], my_a0) == 0) {
                        all_covered_differ = false;
                    }
                }
                if (all_covered_differ) {
                    diag_emit(DIAG_WARNING, call->span,
                              "match: constructor '%s' of '%s' is unreachable for "
                              "this GADT instantiation",
                              c->name, adt->name);
                } else {
                    diag_emit(DIAG_ERROR, call->span,
                              "match: non-exhaustive patterns — constructor '%s' of '%s' not covered",
                              c->name, adt->name);
                    free(covered); return NULL;
                }
            }
        } else {
            for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
                if (!covered[ci]) {
                    diag_emit(DIAG_ERROR, call->span,
                              "match: non-exhaustive patterns — constructor '%s' of '%s' not covered",
                              adt->ctors[ci]->name, adt->name);
                    free(covered); return NULL;
                }
            }
        }
    }
    free(covered);

    if (result_type.kind == TY_UNKNOWN) result_type = TYPE_NIL;

    Expr *out = expr_new(e->arena, EX_MATCH, result_type, call->span);
    out->as.match_.scrutinee = scrutinee;
    out->as.match_.arms = arms;
    out->as.match_.n_arms = n_arms;
    return out;
}

/* Phase 11: make-struct - construct a struct value
 * Syntax: (make-struct StructName val1 val2 ...)
 * Returns a struct value (TY_STRUCT) with fields filled in positional order.
 */
static Expr *elab_make_struct(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "make-struct requires a struct name: (make-struct StructName val1 ...)");
        return NULL;
    }

    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "make-struct: first argument must be a struct name");
        return NULL;
    }

    /* Look up the struct binding */
    Binding *struct_binding = scope_lookup(e->scope, name_form->as.sym);
    if (!struct_binding || struct_binding->type.kind != TY_STRUCT) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "make-struct: '%s' is not a defined struct type",
                  name_form->as.sym->name);
        return NULL;
    }

    StructDef *def = struct_binding->type.as.struct_.def;
    uint32_t n_given = call->as.list.len - 2; /* args after name */

    if (n_given != def->n_fields) {
        diag_emit(DIAG_ERROR, call->span,
                  "make-struct '%s': expected %u field value(s), got %u",
                  def->name, def->n_fields, n_given);
        return NULL;
    }

    /* Elaborate each field value */
    Expr **field_values = (Expr **)arena_alloc(e->arena, def->n_fields * sizeof(Expr *));
    for (uint32_t i = 0; i < def->n_fields; i++) {
        Expr *fv = elab_form(e, call->as.list.items[2 + i]);
        if (!fv) return NULL;
        field_values[i] = fv;
    }

    /* Build the result type */
    Type result_type = type_struct(def);

    Expr *out = expr_new(e->arena, EX_MAKE_STRUCT, result_type, call->span);
    out->as.make_struct_.def = def;
    out->as.make_struct_.field_values = field_values;
    out->as.make_struct_.n_fields = def->n_fields;
    return out;
}

/* Phase 12: Borrow traits */

/* Elaborate (set! (@ r) value) - mutation through a mutable borrow.
 *
 * Called when elab_set detects a deref-assignment target: (set! (@ r) value).
 * Only &mut T is allowed as the borrow; &T produces an immutable-borrow diagnostic.
 */
static Expr *elab_set_deref(Elab *e, const Form *call, const Form *deref_form) {
    /* Elaborate the borrow expression r */
    Expr *ref = elab_form(e, deref_form->as.list.items[1]);
    if (!ref) return NULL;

    /* Must be &mut T */
    if (ref->type.kind == TY_REF_IMMUT) {
        diag_emit(DIAG_ERROR, deref_form->span,
                  "cannot assign through immutable borrow; use `&mut T` for mutation");
        return NULL;
    }
    if (ref->type.kind != TY_REF_MUT) {
        diag_emit(DIAG_ERROR, deref_form->span,
                  "set! via @ requires a &mut T borrow, got %s",
                  type_name(ref->type));
        return NULL;
    }

    /* Elaborate the value */
    Expr *value = elab_form(e, call->as.list.items[2]);
    if (!value) return NULL;

    /* Type-check: value must match the inner type */
    Type inner_type = type_from_kind(ref->type.as.ref_borrow.target);
    if (!type_eq(value->type, inner_type)) {
        diag_emit(DIAG_ERROR, value->span,
                  "set! type mismatch: cannot assign %s through &mut %s borrow",
                  type_name(value->type), type_name(inner_type));
        return NULL;
    }

    Expr *out = expr_new(e->arena, EX_SET_DEREF, TYPE_NIL, call->span);
    out->as.set_deref_.ref = ref;
    out->as.set_deref_.value = value;
    return out;
}

/* Elaborate (& expr) - create an immutable borrow
 * 
 * Syntax: (& expr)
 * Returns: &T where T is the type of expr
 * The borrow is valid for the duration of the enclosing scope.
 */
static Expr *elab_borrow_immut(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "& takes exactly one argument: (& expr)");
        return NULL;
    }
    
    /* Elaborate the expression being borrowed */
    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;
    
    /* Phase 12: Borrow tracking - check if inner is a binding and track the borrow */
    if (inner->kind == EX_VAR) {
        Binding *target = inner->as.var.binding;
        /* Check for use-after-move */
        if (target->is_moved) {
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0005_USE_AFTER_MOVE,
                                "cannot borrow `%s` because it was moved",
                                target->name->name);
            return NULL;
        }
        /* Check for borrow conflicts and add to active borrows */
        if (!scope_add_borrow(e->scope, target, BK_IMMUT, call->span)) {
            return NULL; /* Error already emitted */
        }
    }
    
    /* Phase U: Borrowing from ptr<void> requires an unsafe context. */
    if (inner->type.kind == TY_PTR_VOID && e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "cannot borrow from ptr<void> outside (unsafe ...)");
        return NULL;
    }

    /* Create the borrow type: &T where T is the referenced value's type.
     * Special cases:
     *   (& r) where r: ref<T>     → &T (borrow from owning ref)
     *   (& r) where r: &T or &mut T → &T (reborrow — same target type, not &&T)
     *   (& x) where x: T           → &T (plain borrow)
     */
    Type borrow_type;
    if (inner->type.kind == TY_REF) {
        borrow_type = type_ref_immut(inner->type.as.ref.inner);
    } else if (inner->type.kind == TY_REF_IMMUT || inner->type.kind == TY_REF_MUT) {
        borrow_type = type_ref_immut(inner->type.as.ref_borrow.target);
    } else {
        borrow_type = type_ref_immut(inner->type.kind);
    }
    
    /* Create the borrow expression */
    Expr *out = expr_new(e->arena, EX_BORROW_IMMUT, borrow_type, call->span);
    out->as.borrow_immut_.expr = inner;
    return out;
}

/* Elaborate (&mut expr) - create a mutable borrow
 * 
 * Syntax: (&mut expr)
 * Returns: &mut T where T is the type of expr
 * The borrow is valid for the duration of the enclosing scope.
 * Only one &mut T can exist for a given T at a time.
 */
static Expr *elab_borrow_mut(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "&mut takes exactly one argument: (&mut expr)");
        return NULL;
    }
    
    /* Elaborate the expression being borrowed */
    Expr *inner = elab_form(e, call->as.list.items[1]);
    if (!inner) return NULL;
    
    /* Phase 12: Borrow tracking - check if inner is a binding and track the borrow */
    if (inner->kind == EX_VAR) {
        Binding *target = inner->as.var.binding;
        /* Check for use-after-move */
        if (target->is_moved) {
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0005_USE_AFTER_MOVE,
                                "cannot mutably borrow `%s` because it was moved",
                                target->name->name);
            return NULL;
        }
        /* Check for borrow conflicts and add to active borrows */
        if (!scope_add_borrow(e->scope, target, BK_MUT, call->span)) {
            return NULL; /* Error already emitted */
        }
    }
    
    /* Phase U: Borrowing from ptr<void> requires an unsafe context. */
    if (inner->type.kind == TY_PTR_VOID && e->unsafe_depth == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "cannot borrow from ptr<void> outside (unsafe ...)");
        return NULL;
    }

    /* Create the borrow type: &mut T where T is the referenced value's type.
     * Special cases:
     *   (&mut r) where r: ref<T>  → &mut T (mutable borrow from owning ref)
     *   (&mut r) where r: &mut T  → &mut T (mutable reborrow)
     *   (&mut r) where r: &T      → error (cannot take mutable borrow of immutable borrow)
     *   (&mut x) where x: T       → &mut T (plain mutable borrow)
     */
    Type borrow_type;
    if (inner->type.kind == TY_REF) {
        borrow_type = type_ref_mut(inner->type.as.ref.inner);
    } else if (inner->type.kind == TY_REF_IMMUT) {
        diag_emit(DIAG_ERROR, call->span,
                  "cannot borrow as mutable: source is already an immutable borrow `&T`");
        return NULL;
    } else if (inner->type.kind == TY_REF_MUT) {
        borrow_type = type_ref_mut(inner->type.as.ref_borrow.target);
    } else {
        borrow_type = type_ref_mut(inner->type.kind);
    }
    
    /* Create the borrow expression */
    Expr *out = expr_new(e->arena, EX_BORROW_MUT, borrow_type, call->span);
    out->as.borrow_mut_.expr = inner;
    return out;
}

/* Phase 15: Typeclasses */

/* Parse a single typeclass method definition from a Form.
 * Syntax: (method-name [param1 : type1, param2 : type2, ...] : return-type)
 * or: (method-name [param1 param2 ...] : return-type) - types inferred from usage
 */
static TypeClassMethod *parse_typeclass_method(Elab *e, Form *method_form, Span span) {
    if (method_form->tag != F_LIST || method_form->as.list.len < 3) {
        diag_emit(DIAG_ERROR, span,
                  "typeclass method requires (name [params...] : return-type)");
        return NULL;
    }
    
    /* Parse method name */
    Form *name_form = method_form->as.list.items[0];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "typeclass method name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;
    
    /* Parse parameter vector */
    Form *params_form = method_form->as.list.items[1];
    if (params_form->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_form->span,
                  "typeclass method parameter list must be a vector");
        return NULL;
    }
    
    /* Parse parameters */
    uint8_t n_params = params_form->as.list.len;
    const Symbol **param_names = NULL;
    Type *param_types = NULL;
    /* Phase CCL: callable-param flag array — parallel to param_names/param_types.
     * param_is_fn[i] = true when the i-th param is declared with [name :fn] syntax,
     * marking it as a single-argument callable that should receive tur_poly_fn_t. */
    bool *param_is_fn = NULL;

    if (n_params > 0) {
        param_names = (const Symbol **)arena_alloc(e->arena, n_params * sizeof(const Symbol *));
        param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
        param_is_fn = (bool *)arena_alloc(e->arena, n_params * sizeof(bool));
        for (uint8_t i = 0; i < n_params; i++) param_is_fn[i] = false;

        /* actual_p: number of real parameters encountered (keywords don't count). */
        uint8_t actual_p = 0;
        for (uint8_t i = 0; i < n_params; i++) {
            Form *p = params_form->as.list.items[i];
            if (p->tag == F_SYM) {
                param_names[actual_p] = p->as.sym;
                /* Default to int for now - type inference for method params deferred */
                param_types[actual_p] = TYPE_INT;
                param_is_fn[actual_p] = false;
                actual_p++;
            } else if (p->tag == F_KEYWORD) {
                /* Inline type annotation for the previous parameter:
                 * e.g. [b :ptr<void>] where :ptr<void> annotates b */
                if (actual_p == 0) {
                    diag_emit(DIAG_ERROR, p->span,
                              "type annotation without preceding parameter");
                    return NULL;
                }
                uint8_t prev = actual_p - 1;
                const Symbol *kw = p->as.sym;
                if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                    param_types[prev] = TYPE_INT;
                } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                    param_types[prev] = TYPE_BOOL;
                } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                    param_types[prev] = TYPE_CSTR;
                } else if ((kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) ||
                           (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0)) {
                    param_types[prev] = TYPE_PTR_VOID;
                } else if (kw->len == 2 && memcmp(kw->name, "fn", 2) == 0) {
                    param_types[prev] = TYPE_PTR_VOID;
                    param_is_fn[prev] = true;
                } else {
                    diag_emit(DIAG_ERROR, p->span,
                              "unsupported type in typeclass method parameter");
                    return NULL;
                }
            } else if (p->tag == F_VEC && p->as.list.len >= 2) {
                /* [name : type] or [name :fn] nested vector syntax */
                Form *name_f = p->as.list.items[0];
                Form *type_f = p->as.list.items[1];
                if (name_f->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, name_f->span,
                              "parameter name must be a symbol");
                    return NULL;
                }
                param_names[actual_p] = name_f->as.sym;
                param_is_fn[actual_p] = false;
                /* Parse type annotation */
                if (type_f->tag == F_KEYWORD) {
                    const Symbol *kw = type_f->as.sym;
                    if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                        param_types[actual_p] = TYPE_INT;
                    } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                        param_types[actual_p] = TYPE_BOOL;
                    } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                        param_types[actual_p] = TYPE_CSTR;
                    } else if ((kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) ||
                               (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0)) {
                        param_types[actual_p] = TYPE_PTR_VOID;
                    } else if (kw->len == 2 && memcmp(kw->name, "fn", 2) == 0) {
                        /* Phase CCL: :fn marks this param as a single-argument
                         * callable; it will be passed as tur_poly_fn_t at call
                         * sites so that capturing closures work transparently. */
                        param_types[actual_p] = TYPE_PTR_VOID;
                        param_is_fn[actual_p] = true;
                    } else {
                        diag_emit(DIAG_ERROR, type_f->span,
                                  "unsupported type in typeclass method parameter");
                        return NULL;
                    }
                } else if (type_f->tag == F_TYPE_ANN) {
                    /* `: type-expr` compound annotation */
                    Type *ft = (type_f->as.list.len > 0)
                        ? type_expr_from_form(e, type_f->as.list.items[0], NULL, NULL, NULL, 0)
                        : NULL;
                    if (!ft) {
                        diag_emit(DIAG_ERROR, type_f->span,
                                  "unsupported type form in typeclass method parameter");
                        return NULL;
                    }
                    param_types[actual_p] = *ft;
                } else if (type_f->tag == F_LIST || type_f->tag == F_VEC) {
                    /* Phase HRT3: allow forall/exists type forms as parameter types */
                    Type *ft = type_expr_from_form(e, type_f, NULL, NULL, NULL, 0);
                    if (!ft) {
                        diag_emit(DIAG_ERROR, type_f->span,
                                  "unsupported type form in typeclass method parameter");
                        return NULL;
                    }
                    param_types[actual_p] = *ft;
                } else {
                    param_types[actual_p] = TYPE_INT; /* default */
                }
                actual_p++;
            } else {
                diag_emit(DIAG_ERROR, p->span,
                          "parameter must be a symbol or [name : type] vector");
                return NULL;
            }
        }
        n_params = actual_p;
    }
    
    /* Parse return type - must be after params */
    /* Syntax: (method [params] : return-type)
     *      or (method [params] : #{Effect...} return-type)  -- effect row annotation
     *      or (method [params] #{Effect...} : return-type)  -- effect row annotation alt
     *
     * ER3: #{...} (F_MAP) in return-type position is now parsed and stored on
     * the method so effect_check_pass can enforce it against instance method bodies. */
    EffectRow *method_effect_row = NULL;
    Type return_type = TYPE_NIL;
    uint32_t ret_idx = 2;   /* first element after params vector */
    if (method_form->as.list.len > ret_idx) {
        Form *maybe_row = method_form->as.list.items[ret_idx];
        if (maybe_row->tag == F_MAP) {
            /* #{Effect...} effect-row annotation -- parse and store it. */
            uint8_t n_sym = (uint8_t)maybe_row->as.list.len;
            const Symbol **syms = (const Symbol **)arena_alloc(e->arena,
                                    (n_sym ? n_sym : 1) * sizeof(Symbol *));
            uint8_t n_valid = 0;
            for (uint32_t j = 0; j < maybe_row->as.list.len; j++) {
                Form *item = maybe_row->as.list.items[j];
                if (item->tag == F_SYM) {
                    syms[n_valid++] = item->as.sym;
                }
            }
            method_effect_row = effect_row_unresolved(e->arena, syms, n_valid);
            ret_idx++;
        }
    }
    if (method_form->as.list.len > ret_idx) {
        Form *ret_form = method_form->as.list.items[ret_idx];
        if (ret_form->tag == F_MAP) {
            /* another effect row or #{} after the params — skip silently */
            /* (ignore the rest; return type stays TYPE_NIL) */
        } else if (ret_form->tag == F_KEYWORD) {
            const Symbol *kw = ret_form->as.sym;
            if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                return_type = TYPE_INT;
            } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                return_type = TYPE_BOOL;
            } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                return_type = TYPE_CSTR;
            } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
                       (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                return_type = TYPE_NIL;
            } else if ((kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) ||
                       (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0)) {
                return_type = TYPE_PTR_VOID;
            } else {
                diag_emit(DIAG_ERROR, ret_form->span,
                          "unsupported return type in typeclass method");
                return NULL;
            }
        } else if (ret_form->tag == F_TYPE_ANN) {
            /* `: type-expr` compound return type annotation */
            Type *ft = (ret_form->as.list.len > 0)
                ? type_expr_from_form(e, ret_form->as.list.items[0], NULL, NULL, NULL, 0)
                : NULL;
            if (!ft) {
                diag_emit(DIAG_ERROR, ret_form->span,
                          "unsupported return type form in typeclass method");
                return NULL;
            }
            return_type = *ft;
        } else if (ret_form->tag == F_LIST || ret_form->tag == F_VEC) {
            /* Phase HRT3: allow forall/exists type forms as return types */
            Type *ft = type_expr_from_form(e, ret_form, NULL, NULL, NULL, 0);
            if (!ft) {
                diag_emit(DIAG_ERROR, ret_form->span,
                          "unsupported return type form in typeclass method");
                return NULL;
            }
            return_type = *ft;
        } else {
            diag_emit(DIAG_ERROR, ret_form->span,
                      "typeclass method return type must be a keyword like :int");
            return NULL;
        }
    }
    
    TypeClassMethod *method = (TypeClassMethod *)arena_alloc(e->arena, sizeof(TypeClassMethod));
    method->name = name;
    method->param_names = param_names;
    method->param_types = param_types;
    method->param_is_fn = param_is_fn;
    method->n_params = n_params;
    method->return_type = return_type;
    method->effect_row = method_effect_row;  /* ER3: NULL if not annotated */
    return method;
}

/* Phase T19-B: (thread-spawn closure)
 *
 * Performs a Send-safety check on all variables captured by the closure.
 * Types that are not Send (ref<T>, rc<T>, weak<T>, cont<T>, &T, &mut T)
 * are rejected with TUR-E0010.
 *
 * This form is the elaboration-level gate for cross-thread closure passing.
 * Actual OS-thread spawning is provided by stdlib/thread.tur (T19-C); this
 * form only validates send-safety and returns nil as a compile-time assertion.
 *
 * Usage: (thread-spawn (fn [...] body))
 */
static Expr *elab_thread_spawn(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "thread-spawn requires exactly one argument: "
                  "(thread-spawn closure)");
        return NULL;
    }

    Expr *closure_expr = elab_form(e, call->as.list.items[1]);
    if (!closure_expr) return NULL;

    /* Send-check: walk the capture set of any closure argument. */
    if (closure_expr->kind == EX_CLOSURE) {
        struct Closure *cl = closure_expr->as.closure_.closure;
        bool had_error = false;
        for (uint8_t i = 0; i < cl->n_captures; i++) {
            Binding *cap = cl->captures[i];
            if (!type_is_send(cap->type)) {
                diag_emit_with_code(DIAG_ERROR, call->span,
                    TUR_E0010_NOT_SEND,
                    "type `%s` cannot be sent across thread boundaries "
                    "(not `Send`); captured variable `%s`",
                    type_name(cap->type), cap->name->name);
                had_error = true;
            }
        }
        if (had_error) return NULL;
    }

    /* Return the closure expression (type flows through; T19-C provides the
     * actual runtime thread-spawn wrapper in stdlib/thread.tur).            */
    return closure_expr;
}

/* Phase T21-F: (async fn-expr) — launch fn-expr (no-arg function) in a new
 * thread; return a TurAsyncTask* as ptr<void> (the future handle).          */
static Expr *elab_async(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(async fn-expr) requires exactly one argument");
        return NULL;
    }
    Expr *fn_expr = elab_form(e, call->as.list.items[1]);
    if (!fn_expr) return NULL;
    
    /* AW-012 / AW-011B-1: Send-check for async closures.
     * Values captured in async blocks must be Send (can be moved to fiber context).
     * T25: Also check that no effect-handler continuation (k) escapes into an async
     * block, which would cause a resume-on-wrong-fiber runtime error. */
    if (fn_expr->kind == EX_FN || fn_expr->kind == EX_CLOSURE) {
        const struct Closure *cl = NULL;
        if (fn_expr->kind == EX_CLOSURE) {
            cl = fn_expr->as.closure_.closure;
        } else if (fn_expr->kind == EX_FN) {
            cl = fn_expr->as.fn_.fn->closure;
        }
        if (cl) {
            bool had_error = false;
            for (uint8_t i = 0; i < cl->n_captures; i++) {
                Binding *cap = cl->captures[i];
                if (!type_is_send(cap->type)) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                        TUR_E0010_NOT_SEND,
                        "type `%s` cannot be sent across thread boundaries "
                        "(not `Send`); captured variable `%s` in async block",
                        type_name(cap->type), cap->name->name);
                    had_error = true;
                }
                /* T25: Prevent continuation escape into async scope.
                 * A handler continuation k cannot be captured by an async block because
                 * the continuation is bound to the fiber where perform was called; resuming
                 * it from a different fiber produces a runtime error. Catching this at
                 * compile time is safer. */
                if (cap->is_continuation) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                        TUR_E0017_CONT_ESCAPE_ASYNC,
                        "effect handler continuation `%s` cannot be captured by an async block; "
                        "resuming it from a different fiber would cause a runtime error",
                        cap->name->name);
                    had_error = true;
                }
            }
            if (had_error) return NULL;
        }
    }
    
    Expr *out = expr_new(e->arena, EX_ASYNC, TYPE_PTR_VOID, call->span);
    out->as.async_.fn_expr = fn_expr;
    return out;
}

/* Phase T21-F: (await fut) — block on TurAsyncTask* future; return int64_t. */
static Expr *elab_await(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(await fut) requires exactly one argument");
        return NULL;
    }
    Expr *fut_expr = elab_form(e, call->as.list.items[1]);
    if (!fut_expr) return NULL;
    Expr *out = expr_new(e->arena, EX_AWAIT, TYPE_INT, call->span);
    out->as.await_.fut_expr = fut_expr;
    return out;
}

/* Phase SEL1: (select ((ch :recv v) body) ... (:default body))
 * Waiter-based fair multi-channel select. Returns the value of the selected clause body.
 * All channel expressions must be ptr<void>. Clause bodies must be type-compatible. */
static Expr *elab_select(Elab *e, const Form *call) {
    /* call->as.list.items[0] = "select"
     * call->as.list.items[1..n] = clauses
     * Each clause is a 2-element list:
     *   ((chan :recv v) body)   -- recv clause
     *   ((chan :send val) body) -- send clause
     *   (:default body)        -- default arm (only keyword at head, rest is body)
     */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(select ...) requires at least one clause");
        return NULL;
    }
    uint32_t n_clauses_raw = call->as.list.len - 1;
    SelectClauseEntry *clauses = (SelectClauseEntry *)arena_alloc(
        e->arena, n_clauses_raw * sizeof(SelectClauseEntry));
    uint32_t n_clauses = 0;
    int has_default = 0;
    Expr *default_body = NULL;
    const Symbol *sym_default = intern_cstr(e->st, "default");

    for (uint32_t ci = 0; ci < n_clauses_raw; ci++) {
        Form *clause_form = call->as.list.items[1 + ci];
        /* Must be a list with exactly 2 items: descriptor and body */
        if (clause_form->tag != F_LIST || clause_form->as.list.len != 2) {
            diag_emit(DIAG_ERROR, clause_form->span,
                      "select clause must be ((chan :recv v) body) or ((chan :send val) body) or (:default body)");
            return NULL;
        }
        Form *desc      = clause_form->as.list.items[0];
        Form *body_form = clause_form->as.list.items[1];

        /* Check for (:default body) */
        if (desc->tag == F_KEYWORD && desc->as.sym == sym_default) {
            if (has_default) {
                diag_emit(DIAG_ERROR, clause_form->span,
                          "select: at most one :default arm is allowed");
                return NULL;
            }
            has_default = 1;
            default_body = elab_form(e, body_form);
            if (!default_body) return NULL;
            continue;
        }

        /* Channel clause: desc must be (chan :recv v) or (chan :send val) */
        if (desc->tag != F_LIST || desc->as.list.len < 2) {
            diag_emit(DIAG_ERROR, desc->span,
                      "select channel descriptor must be (chan :recv binding) or (chan :send val)");
            return NULL;
        }
        Form *chan_form = desc->as.list.items[0];
        Form *op_form   = desc->as.list.items[1];

        if (op_form->tag != F_KEYWORD) {
            diag_emit(DIAG_ERROR, op_form->span,
                      "select clause operation must be :recv or :send");
            return NULL;
        }
        int op;
        if (op_form->as.sym == e->sym_recv) {
            op = 0;
        } else if (op_form->as.sym == e->sym_send) {
            op = 1;
        } else {
            diag_emit(DIAG_ERROR, op_form->span,
                      "select clause operation must be :recv or :send");
            return NULL;
        }

        /* Elaborate channel expression */
        Expr *chan_expr = elab_form(e, chan_form);
        if (!chan_expr) return NULL;

        Expr *send_val = NULL;
        Binding *recv_bind = NULL;

        if (op == 0) {
            /* recv: (chan :recv binding) -- 3 elements */
            if (desc->as.list.len != 3) {
                diag_emit(DIAG_ERROR, desc->span,
                          "select :recv clause descriptor: (chan :recv binding)");
                return NULL;
            }
            Form *bind_form = desc->as.list.items[2];
            if (bind_form->tag != F_SYM) {
                diag_emit(DIAG_ERROR, bind_form->span,
                          "select :recv binding must be a symbol");
                return NULL;
            }
            recv_bind = binding_new(e, bind_form->as.sym, TYPE_INT, false, false, body_form->span);
        } else {
            /* send: (chan :send val) -- 3 elements */
            if (desc->as.list.len != 3) {
                diag_emit(DIAG_ERROR, desc->span,
                          "select :send clause descriptor: (chan :send val)");
                return NULL;
            }
            Form *val_form = desc->as.list.items[2];
            send_val = elab_form(e, val_form);
            if (!send_val) return NULL;
        }

        /* Elaborate body with recv binding in scope (if recv clause) */
        Expr *body_expr;
        if (op == 0 && recv_bind) {
            /* Introduce a scope with the received value binding */
            Scope recv_scope;
            scope_init(&recv_scope, e->scope);
            e->scope = &recv_scope;
            scope_add(&recv_scope, recv_bind);
            body_expr = elab_form(e, body_form);
            e->scope = recv_scope.parent;
        } else {
            body_expr = elab_form(e, body_form);
        }
        if (!body_expr) return NULL;

        SelectClauseEntry *sc = &clauses[n_clauses++];
        sc->chan          = chan_expr;
        sc->op            = op;
        sc->send_val      = send_val;
        sc->recv_binding  = recv_bind;
        sc->body          = body_expr;
    }

    if (n_clauses == 0 && !has_default) {
        diag_emit(DIAG_ERROR, call->span,
                  "select: requires at least one channel clause");
        return NULL;
    }

    /* Determine return type from first clause body (or default body) */
    Type result_type = TYPE_INT;
    if (n_clauses > 0) result_type = clauses[0].body->type;
    else if (default_body) result_type = default_body->type;

    Expr *out = expr_new(e->arena, EX_SELECT, result_type, call->span);
    out->as.select_.clauses      = clauses;
    out->as.select_.n_clauses    = n_clauses;
    out->as.select_.has_default  = has_default;
    out->as.select_.default_body = default_body;
    return out;
}

/* Phase R2: (panic msg) — print msg to stderr, then abort.
 * msg must be of type :cstr (or a string literal).
 * Return type is TYPE_NEVER (diverging; caller never observes a value). */
static Expr *elab_panic(Elab *e, const Form *call) {
    /* Phase R6: Lint panic usage */
    if (g_lint_panic && e->fn_body_depth > 0) {
        /* We're inside a function body - check if it's main or a test function */
        bool in_test_or_main = false;
        if (e->current_fn_name) {
            if (strcmp(e->current_fn_name->name, "main") == 0) {
                in_test_or_main = true;
            } else if (strncmp(e->current_fn_name->name, "test-", 5) == 0) {
                in_test_or_main = true;
            }
        }
        if (!in_test_or_main) {
            diag_emit(DIAG_WARNING, call->span,
                      "panic called outside of main or test function; consider using Result instead");
        }
    }
    
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic msg) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC, TYPE_NEVER, call->span);
    out->as.panic_.payload = payload;
    return out;
}

/* Phase R2: (panic-with payload) — panic with typed payload.
 * payload is any value; its TypeKind is stored for catch-panic-of filtering.
 * Return type is TYPE_NEVER (diverging). */
static Expr *elab_panic_with(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-with payload) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC_WITH, TYPE_NEVER, call->span);
    out->as.panic_with_.payload = payload;
    return out;
}

/* Phase R2: (catch-unwind thunk) — catch any panic at a boundary.
 * thunk is a nullary function; returns result<T, panic-payload>.
 * In v1, result is ptr<void> and lowering uses tur_catch_unwind from runtime. */
static Expr *elab_catch_unwind(Elab *e, const Form *call) {
    /* Phase R6: Lint catch_unwind usage */
    if (g_lint_panic) {
        diag_emit(DIAG_WARNING, call->span,
                  "catch_unwind should only be used at effect boundaries, not for normal error handling; consider using Result instead");
    }
    
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(catch-unwind thunk) requires exactly one argument");
        return NULL;
    }
    Expr *thunk = elab_form(e, call->as.list.items[1]);
    if (!thunk) return NULL;
    /* Result type is result<T, panic-payload> - ptr<void> in v1 */
    Expr *out = expr_new(e->arena, EX_CATCH_UNWIND, TYPE_PTR_VOID, call->span);
    out->as.catch_unwind_.thunk = thunk;
    return out;
}

/* Phase R2: (catch-panic-of Type thunk) — catch panics of a specific type.
 * Type is a type identifier (symbol); thunk is a nullary function.
 * Returns result<T, panic-payload> if type matches, otherwise re-panics.
 * In v1, result is ptr<void> and lowering uses tur_catch_panic_of from runtime. */
static Expr *elab_catch_panic_of(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(catch-panic-of Type thunk) requires exactly two arguments");
        return NULL;
    }
    /* First arg: type identifier */
    Form *type_form = call->as.list.items[1];
    TypeKind type_kind = TY_UNKNOWN;
    if (type_form->tag == F_SYM) {
        const char *type_name = type_form->as.sym->name;
        type_kind = typekind_from_name(type_name);
    }
    /* Second arg: thunk */
    Expr *thunk = elab_form(e, call->as.list.items[2]);
    if (!thunk) return NULL;
    /* Result type is result<T, panic-payload> - ptr<void> in v1 */
    Expr *out = expr_new(e->arena, EX_CATCH_PANIC_OF, TYPE_PTR_VOID, call->span);
    out->as.catch_panic_of_.type_kind = type_kind;
    out->as.catch_panic_of_.thunk = thunk;
    return out;
}

/* Phase S4: (throw expr) -- raise a catchable exception; always TY_NEVER */
static Expr *elab_throw(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span, "throw: expected exactly one argument");
        return NULL;
    }
    Expr *val = elab_form(e, call->as.list.items[1]);
    if (!val) return NULL;
    Expr *out = expr_new(e->arena, EX_THROW, TYPE_NEVER, call->span);
    out->as.throw_.value = val;
    return out;
}

/* Phase S4: (try body (catch [bind] handler) ...)
 * Each catch clause is (catch [name] body) or (catch [name :Type] body).
 * The [name] vector is parsed directly as a binding pattern (not a value). */
static Expr *elab_try_catch(Elab *e, const Form *call) {
    uint32_t len = call->as.list.len;
    if (len < 2) {
        diag_emit(DIAG_ERROR, call->span, "try: expected body");
        return NULL;
    }
    /* Elaborate the body */
    Expr *body = elab_form(e, call->as.list.items[1]);
    if (!body) return NULL;

    /* Parse catch clauses: (catch [binding] handler) */
    uint32_t n_catches = len - 2;
    Binding  **bindings = n_catches ? (Binding **)arena_alloc(e->arena, n_catches * sizeof(Binding *)) : NULL;
    Expr     **handlers = n_catches ? (Expr **)arena_alloc(e->arena, n_catches * sizeof(Expr *))    : NULL;

    for (uint32_t i = 0; i < n_catches; i++) {
        Form *clause = call->as.list.items[i + 2];
        /* clause should be (catch [bind] handler) */
        if (!clause || clause->tag != F_LIST || clause->as.list.len < 3) {
            diag_emit(DIAG_ERROR, clause ? clause->span : call->span,
                      "try: catch clause must be (catch [binding] handler)");
            return NULL;
        }
        /* Second element: binding vector [name] or [name :Type] */
        Form *bind_vec = clause->as.list.items[1];
        Binding *catch_bind = NULL;
        if (bind_vec && bind_vec->tag == F_VEC && bind_vec->as.list.len >= 1) {
            Form *name_form = bind_vec->as.list.items[0];
            if (name_form && name_form->tag == F_SYM) {
                /* Create a fresh binding for the caught value */
                catch_bind = binding_new(e, name_form->as.sym, TYPE_INT, false, false, name_form->span);
            }
        }
        bindings[i] = catch_bind;

        /* Push scope with the binding, elaborate handler */
        Scope catch_scope;
        Scope *saved_scope = e->scope;
        scope_init(&catch_scope, e->scope);
        e->scope = &catch_scope;
        if (catch_bind) scope_add(&catch_scope, catch_bind);
        Expr *handler = elab_form(e, clause->as.list.items[2]);
        e->scope = saved_scope;
        if (!handler) return NULL;
        handlers[i] = handler;
    }

    /* Result type: use body type (simplification; could be union with handler types) */
    Expr *out = expr_new(e->arena, EX_TRY_CATCH, body->type, call->span);
    out->as.try_catch_.body           = body;
    out->as.try_catch_.catch_bindings = bindings;
    out->as.try_catch_.catch_handlers = handlers;
    out->as.try_catch_.n_catches      = n_catches;
    return out;
}

/* Phase R2: (panic-payload-type p) — get the TypeKind tag from a panic payload. */
static Expr *elab_panic_payload_type(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-payload-type payload) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC_PAYLOAD_TYPE, TYPE_INT, call->span);
    out->as.panic_payload_type_.payload = payload;
    return out;
}

/* Phase R2: (panic-payload-value p) — get the boxed value from a panic payload. */
static Expr *elab_panic_payload_value(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-payload-value payload) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC_PAYLOAD_VALUE, TYPE_PTR_VOID, call->span);
    out->as.panic_payload_value_.payload = payload;
    return out;
}

/* Phase R2: (panic-payload-file p) — get the source file from a panic payload. */
static Expr *elab_panic_payload_file(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-payload-file payload) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC_PAYLOAD_FILE, TYPE_CSTR, call->span);
    out->as.panic_payload_file_.payload = payload;
    return out;
}

/* Phase R2: (panic-payload-line p) — get the source line from a panic payload. */
static Expr *elab_panic_payload_line(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-payload-line payload) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_PANIC_PAYLOAD_LINE, TYPE_INT, call->span);
    out->as.panic_payload_line_.payload = payload;
    return out;
}

/* Phase R2: (panic-payload-downcast p Type) — cast panic payload to a specific type.
 * Returns the boxed value if type matches, NULL otherwise. */
static Expr *elab_panic_payload_downcast(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "(panic-payload-downcast payload Type) requires exactly two arguments");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    /* Second arg: type identifier */
    Form *type_form = call->as.list.items[2];
    TypeKind target_type = TY_UNKNOWN;
    if (type_form->tag == F_SYM) {
        const char *type_name = type_form->as.sym->name;
        target_type = typekind_from_name(type_name);
    }
    Expr *out = expr_new(e->arena, EX_PANIC_PAYLOAD_DOWNS, TYPE_PTR_VOID, call->span);
    out->as.panic_payload_downs_.payload = payload;
    out->as.panic_payload_downs_.target_type = target_type;
    return out;
}

/* Phase 18: (call/cc f) - capture the current (delimited) continuation.
 * v1 sugar: (call/cc f) => (let [__cc_f f] (__cc_f (fn [__v] __v)))
 *
 * In v1, full continuation capture is not yet implemented (requires CPS).
 * `f` receives an identity function as the continuation `k`; calling `(k v)`
 * just returns `v`.  This supports escape/abort patterns where f immediately
 * returns `(k result)` without relying on the rest of the computation. */
static Expr *elab_call_cc(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(call/cc f) requires exactly one argument");
        return NULL;
    }
    /* v1 sugar: (call/cc f) => (let [__cc_f f] (__cc_f 0))
     *
     * In v1 all lambda parameters default to TY_INT, so the continuation
     * is passed as the integer 0.  Full continuation capture (where k is
     * actually callable) requires CPS and is deferred to a future phase. */
    Arena *a = e->arena;
    Span sp = call->span;

    /* integer literal 0 — the dummy v1 continuation */
    Form *zero     = form_int(a, sp, 0);

    /* let binding: [__cc_f <user-fn>] */
    Form *sym_ff    = form_sym(a, sp, intern_cstr(e->st, "__cc_f"));
    Form *fn_form   = call->as.list.items[1];
    Form **bv       = (Form **)arena_alloc(a, 2 * sizeof(Form *));
    bv[0] = sym_ff;
    bv[1] = fn_form;
    Form *bind_vec  = form_vec(a, sp, bv, 2);

    /* inner call: (__cc_f 0) */
    Form **ic       = (Form **)arena_alloc(a, 2 * sizeof(Form *));
    ic[0] = sym_ff;
    ic[1] = zero;
    Form *inner     = form_list(a, sp, ic, 2);

    /* (let [__cc_f fn_form] (__cc_f 0)) */
    Form *sym_let   = form_sym(a, sp, e->sym_let);
    Form **li       = (Form **)arena_alloc(a, 3 * sizeof(Form *));
    li[0] = sym_let;
    li[1] = bind_vec;
    li[2] = inner;
    return elab_form(e, form_list(a, sp, li, 3));
}

/* Phase 18: (escape f) - one-shot escape continuation.
 * v1 sugar: (escape f) => (let [__esc_f f] (__esc_f 0))
 *
 * `f` receives 0 as the escape procedure.  Full early-exit semantics require
 * CPS and are deferred to a future phase. */
static Expr *elab_escape(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(escape f) requires exactly one argument");
        return NULL;
    }
    Arena *a = e->arena;
    Span sp = call->span;

    Form *zero     = form_int(a, sp, 0);

    Form *sym_ff    = form_sym(a, sp, intern_cstr(e->st, "__esc_f"));
    Form *fn_form   = call->as.list.items[1];
    Form **bv       = (Form **)arena_alloc(a, 2 * sizeof(Form *));
    bv[0] = sym_ff;
    bv[1] = fn_form;
    Form *bind_vec  = form_vec(a, sp, bv, 2);

    Form **ic       = (Form **)arena_alloc(a, 2 * sizeof(Form *));
    ic[0] = sym_ff;
    ic[1] = zero;
    Form *inner     = form_list(a, sp, ic, 2);

    Form *sym_let   = form_sym(a, sp, e->sym_let);
    Form **li       = (Form **)arena_alloc(a, 3 * sizeof(Form *));
    li[0] = sym_let;
    li[1] = bind_vec;
    li[2] = inner;
    return elab_form(e, form_list(a, sp, li, 3));
}

/* Phase HKT H5: (defkind Name kind-expr)
 *
 * Registers a kind alias.  The kind-expr is currently stored as a string
 * for diagnostic purposes but not enforced at use sites (future phases).
 * Syntax: (defkind F1 (* -> *))
 *         (defkind F2 (* -> * -> *))
 */
static Expr *elab_defkind(Elab *e, const Form *call) {
    /* Minimum: (defkind Name kind-expr) */
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "defkind requires (defkind Name kind-expr)");
        return NULL;
    }

    /* Validate name */
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defkind: name must be a symbol");
        return NULL;
    }

    /* kind-expr is accepted in any form (list, symbol, etc.) — parsed for
     * documentation only.  Currently treated as a no-op. */
    (void)call->as.list.items[2];

    /* Return nil — defkind has no runtime effect. */
    return e_nil(e, call->span);
}

/* Elaborate (defrec Name [params])
 *
 * Defines a recursive type constructor Name and registers it in global scope
 * as a TY_REC binding with kind KIND_ARROW (i.e., kind * -> *).
 * In v1 the body expression is accepted but not evaluated; TY_REC.body = NULL.
 *
 * Syntax: (defrec Fix [^f])
 *         (defrec Name [params] body-ignored-in-v1)
 */
static Expr *elab_defrec(Elab *e, const Form *call) {
    /* Minimum: (defrec Name) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "defrec requires (defrec Name [params])");
        return NULL;
    }

    /* Validate name */
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defrec: name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;

    /* Accept params if present (arg 2) — validate is a list/vector */
    uint32_t body_idx = 2;
    if (call->as.list.len >= 3) {
        Form *params_f = call->as.list.items[2];
        if (params_f->tag != F_LIST && params_f->tag != F_VEC) {
            diag_emit(DIAG_ERROR, params_f->span,
                      "defrec: expected parameter list");
            return NULL;
        }
        body_idx = 3;
    }

    /* Parse body form (arg body_idx) — optional in v1 */
    /* The body can reference the recursive type itself, creating a circular dependency.
     * In v1, we just store the form without elaborating it. */
    Form *body_form = (call->as.list.len >= body_idx + 1) ? call->as.list.items[body_idx] : NULL;

    /* Create TY_REC type: kind * -> * (recursive type constructor) */
    Type rec_type;
    memset(&rec_type, 0, sizeof(rec_type));
    rec_type.kind      = TY_REC;
    rec_type.copy_kind = CK_MOVE;
    rec_type.hkt_kind  = KIND_ARROW;    /* defrec types are kind * -> * */
    rec_type.as.rec.name = name->name;
    rec_type.as.rec.body = NULL;        /* v1: body not yet evaluated, but we store the form for future use */
    
    /* If a body was provided, store it for later elaboration */
    if (body_form) {
        /* For now, we just note that a body exists but don't elaborate it */
        /* This is a placeholder for when full recursive type support is added */
    }

    /* Register in global scope */
    Binding *b = binding_new(e, name, rec_type, false, true, name_form->span);
    scope_add(&e->global, b);

    /* defrec has no runtime effect */
    return e_nil(e, call->span);
}

/* Helper: parse a type expression form into a Type for deftype body.
 * Supports:
 *   - Symbols: primitive types (int, bool, etc.) or type bindings
 *   - Type parameters: ^f, ^^f (stripped to f and looked up in type_params)
 *   - Type applications: (ctor arg) -> TY_APP
 *   - Recursive references: the type name being defined
 * Returns NULL on error. */
static Type *type_expr_from_form(Elab *e, const Form *form, const Symbol *rec_name,
                                  const Symbol **type_params, Kind *type_param_kinds,
                                  uint8_t n_type_params) {
    /* ET3: bare `nil` literal (F_NIL) in a type expression means the nil/unit type */
    if (form->tag == F_NIL) {
        Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
        *t = TYPE_NIL;
        return t;
    }
    if (form->tag == F_SYM) {
        const Symbol *sym = form->as.sym;
        
        /* Check if it's the recursive type name */
        if (sym == rec_name) {
            /* Return a TY_REC type referencing itself (will be resolved later) */
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            memset(t, 0, sizeof(Type));
            t->kind = TY_REC;
            t->copy_kind = CK_MOVE;
            t->hkt_kind = KIND_STAR; /* Will be updated based on context */
            t->as.rec.name = rec_name->name;
            t->as.rec.body = NULL; /* Circular - will be filled in later */
            return t;
        }
        
        /* Check if it's a type parameter (^f or ^^f) */
        if (sym->len > 1 && sym->name[0] == '^') {
            /* Strip the ^ prefix(es) and look up in type_params */
            const char *bare = NULL;
            uint32_t bare_len = 0;
            uint8_t param_idx = 0;
            bool found = false;
            
            if (sym->len > 2 && sym->name[0] == '^' && sym->name[1] == '^') {
                bare = sym->name + 2;
                bare_len = sym->len - 2;
                /* Look for the bare name in type_params (which stores the stripped name) */
                for (param_idx = 0; param_idx < n_type_params; param_idx++) {
                    if (type_params[param_idx] && 
                        type_params[param_idx]->len == bare_len &&
                        memcmp(type_params[param_idx]->name, bare, bare_len) == 0) {
                        found = true;
                        break;
                    }
                }
            } else if (sym->len > 1) {
                bare = sym->name + 1;
                bare_len = sym->len - 1;
                /* Look for the bare name in type_params */
                for (param_idx = 0; param_idx < n_type_params; param_idx++) {
                    if (type_params[param_idx] && 
                        type_params[param_idx]->len == bare_len &&
                        memcmp(type_params[param_idx]->name, bare, bare_len) == 0) {
                        found = true;
                        break;
                    }
                }
            }
            
            if (found) {
                /* Return a type variable - represented as TY_STRUCT with no def
                 * The kind is stored in type_param_kinds[param_idx] */
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                memset(t, 0, sizeof(Type));
                t->kind = TY_STRUCT;
                t->copy_kind = CK_MOVE;
                t->hkt_kind = type_param_kinds[param_idx];
                t->as.struct_.def = NULL;
                return t;
            }
            /* If not found, fall through to look up as a type binding */
        }
        
        /* Check if it's a bare type parameter name (without ^ prefix) */
        if (n_type_params > 0) {
            for (uint8_t i = 0; i < n_type_params; i++) {
                if (type_params[i] && type_params[i] == sym) {
                    /* Return a type variable */
                    Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                    memset(t, 0, sizeof(Type));
                    t->kind = TY_STRUCT;
                    t->copy_kind = CK_MOVE;
                    t->hkt_kind = type_param_kinds[i];
                    t->as.struct_.def = NULL;
                    return t;
                }
            }
        }
        
        /* Look up as a type binding (primitive or user-defined type) */
        Binding *b = scope_lookup(e->scope, sym);
        if (!b) {
            b = scope_lookup(&e->global, sym);
        }
        if (b) {
            /* Return a copy of the binding's type */
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = b->type;
            return t;
        }
        
        /* Primitive type keywords */
        if (sym->len == 3 && memcmp(sym->name, "int", 3) == 0) {
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = TYPE_INT;
            return t;
        } else if (sym->len == 4 && memcmp(sym->name, "bool", 4) == 0) {
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = TYPE_BOOL;
            return t;
        } else if (sym->len == 4 && memcmp(sym->name, "cstr", 4) == 0) {
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = TYPE_CSTR;
            return t;
        } else if ((sym->len == 4 && memcmp(sym->name, "void", 4) == 0) ||
                   (sym->len == 3 && memcmp(sym->name, "nil", 3) == 0)) {
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = TYPE_NIL;
            return t;
        } else if (sym->len == 9 && memcmp(sym->name, "ptr<void>", 9) == 0) {
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = TYPE_PTR_VOID;
            return t;
        }
        
        /* IT4: any — top type.  Available when -Xunion-types or -Xintersection-types is on. */
        if (sym->len == 3 && memcmp(sym->name, "any", 3) == 0) {
            if (!g_union_types_enabled && !g_intersection_types_enabled) {
                diag_emit(DIAG_ERROR, form->span,
                          "'any' type requires -Xunion-types or -Xintersection-types");
                return NULL;
            }
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            memset(t, 0, sizeof(Type));
            t->kind     = TY_ANY;
            t->copy_kind = CK_COPY;
            t->hkt_kind  = KIND_STAR;
            return t;
        }

        /* Unknown - return as opaque struct */
        Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
        memset(t, 0, sizeof(Type));
        t->kind = TY_STRUCT;
        t->copy_kind = CK_MOVE;
        t->hkt_kind = KIND_STAR;
        t->as.struct_.def = NULL;
        return t;
    } else if (form->tag == F_LIST) {
        /* IT0: (A | B | C) — union type expression.
         * Detect by scanning for any element that is the "|" pipe symbol.
         * Only active when -Xunion-types is enabled. */
        {
            bool has_pipe = false;
            for (uint32_t uk = 0; uk < form->as.list.len; uk++) {
                Form *uit = form->as.list.items[uk];
                if (uit->tag == F_SYM && uit->as.sym == e->sym_pipe) {
                    has_pipe = true;
                    break;
                }
            }
            if (has_pipe) {
                if (!g_union_types_enabled) {
                    diag_emit(DIAG_ERROR, form->span,
                              "union type syntax requires -Xunion-types; found '|' in type expression");
                    return NULL;
                }
                /* Count non-pipe member forms */
                uint8_t n_members = 0;
                for (uint32_t uk = 0; uk < form->as.list.len; uk++) {
                    Form *uit = form->as.list.items[uk];
                    if (uit->tag == F_SYM && uit->as.sym == e->sym_pipe) continue;
                    n_members++;
                }
                if (n_members < 2) {
                    diag_emit(DIAG_ERROR, form->span,
                              "union type requires at least two member types separated by '|'");
                    return NULL;
                }
                /* Collect member types */
                Type **members = (Type **)arena_alloc(e->arena, n_members * sizeof(Type *));
                uint8_t mi = 0;
                for (uint32_t uk = 0; uk < form->as.list.len; uk++) {
                    Form *uit = form->as.list.items[uk];
                    if (uit->tag == F_SYM && uit->as.sym == e->sym_pipe) continue;
                    Type *mt = type_expr_from_form(e, uit, rec_name,
                                                   type_params, type_param_kinds, n_type_params);
                    if (!mt) return NULL;
                    members[mi++] = mt;
                }
                /* Build and return union type (type_union_build flattens nested unions) */
                Type built = type_union_build(e->arena, members, n_members);
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = built;
                return t;
            }
        }

        /* IT2: (A & B & C) — intersection type expression.
         * Detect by scanning for any element that is the bare "&" ampersand symbol.
         * Only active when -Xintersection-types is enabled. */
        {
            bool has_amp = false;
            for (uint32_t uk = 0; uk < form->as.list.len; uk++) {
                Form *uit = form->as.list.items[uk];
                if (uit->tag == F_SYM && uit->as.sym == e->sym_ampersand) {
                    has_amp = true;
                    break;
                }
            }
            if (has_amp) {
                if (!g_intersection_types_enabled) {
                    diag_emit(DIAG_ERROR, form->span,
                              "intersection type syntax requires -Xintersection-types; found '&' in type expression");
                    return NULL;
                }
                /* Count non-ampersand member forms */
                uint8_t n_members = 0;
                for (uint32_t uk = 0; uk < form->as.list.len; uk++) {
                    Form *uit = form->as.list.items[uk];
                    if (uit->tag == F_SYM && uit->as.sym == e->sym_ampersand) continue;
                    n_members++;
                }
                if (n_members < 2) {
                    diag_emit(DIAG_ERROR, form->span,
                              "intersection type requires at least two member types separated by '&'");
                    return NULL;
                }
                /* Collect member types */
                Type **members = (Type **)arena_alloc(e->arena, n_members * sizeof(Type *));
                uint8_t mi = 0;
                for (uint32_t uk = 0; uk < form->as.list.len; uk++) {
                    Form *uit = form->as.list.items[uk];
                    if (uit->tag == F_SYM && uit->as.sym == e->sym_ampersand) continue;
                    Type *mt = type_expr_from_form(e, uit, rec_name,
                                                   type_params, type_param_kinds, n_type_params);
                    if (!mt) return NULL;
                    members[mi++] = mt;
                }
                /* IT3: TUR_E0350 -- reject provably-unsatisfiable intersections.
                 * For each pair of members, check if they are known-disjoint concrete
                 * types. Only emit one error (the first pair found). */
                for (uint8_t ia = 0; ia < n_members; ia++) {
                    for (uint8_t ib = ia + 1; ib < n_members; ib++) {
                        if (types_provably_disjoint(members[ia], members[ib])) {
                            diag_emit_with_code(DIAG_ERROR, form->span,
                                TUR_E0350_INTERSECTION_UNSATISFIABLE,
                                "intersection type is unsatisfiable: no value can be both %s and %s",
                                type_name(*members[ia]), type_name(*members[ib]));
                            return NULL;
                        }
                    }
                }
                /* Build and return intersection type (type_intersection_build flattens nested) */
                Type built = type_intersection_build(e->arena, members, n_members);
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                *t = built;
                return t;
            }
        }

        /* ET3-A: (handler EffectName ValueType ResultType) type expression */
        if (form->as.list.len == 4 && form->as.list.items[0]->tag == F_SYM
                && form->as.list.items[0]->as.sym == e->sym_handler_type) {
            if (!g_effect_types_enabled) {
                diag_emit(DIAG_ERROR, form->span,
                          "'handler' type expression requires -Xeffect-types");
                return NULL;
            }
            Form *eff_f = form->as.list.items[1];
            Form *val_f = form->as.list.items[2];
            Form *res_f = form->as.list.items[3];
            if (eff_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, eff_f->span, "(handler E V R): effect name must be a symbol");
                return NULL;
            }
            Type *val_t = type_expr_from_form(e, val_f, rec_name, type_params, type_param_kinds, n_type_params);
            Type *res_t = type_expr_from_form(e, res_f, rec_name, type_params, type_param_kinds, n_type_params);
            if (!val_t || !res_t) return NULL;
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            memset(t, 0, sizeof(Type));
            t->kind = TY_HANDLER;
            t->copy_kind = CK_COPY;
            t->hkt_kind = KIND_STAR;
            t->as.handler_.effect_name = eff_f->as.sym->name;
            t->as.handler_.value_kind = val_t->kind;
            t->as.handler_.result_kind = res_t->kind;
            return t;
        }

        /* Phase HRT0: Handle (forall [vars...] body) and (exists [vars...] body) */
        if (form->as.list.len >= 1 && form->as.list.items[0]->tag == F_SYM) {
            const Symbol *head = form->as.list.items[0]->as.sym;
            bool is_forall_form = (head == e->sym_forall || head == e->sym_forall_u);
            bool is_exists_form = (head == e->sym_exists || head == e->sym_exists_u);

            if (is_forall_form || is_exists_form) {
                /* Syntax: (forall [a b ...] body-type) or (exists [a b ...] body-type) */
                if (form->as.list.len != 3) {
                    diag_emit(DIAG_ERROR, form->span,
                              "'%s' requires exactly two elements: a variable list and a body type",
                              is_forall_form ? "forall" : "exists");
                    return NULL;
                }
                Form *vars_form = form->as.list.items[1];
                Form *body_form = form->as.list.items[2];

                if (vars_form->tag != F_VEC) {
                    diag_emit(DIAG_ERROR, vars_form->span,
                              "'%s' variable list must be a vector, e.g. [a b]",
                              is_forall_form ? "forall" : "exists");
                    return NULL;
                }

                uint8_t n_bound = (uint8_t)vars_form->as.list.len;
                if (n_bound == 0) {
                    diag_emit(DIAG_ERROR, vars_form->span,
                              "'%s' requires at least one bound variable",
                              is_forall_form ? "forall" : "exists");
                    return NULL;
                }

                /* Collect bound variable names and kinds */
                const char **var_names = (const char **)arena_alloc(
                    e->arena, n_bound * sizeof(const char *));
                Kind *var_kinds = (Kind *)arena_alloc(
                    e->arena, n_bound * sizeof(Kind));

                /* Build extended type_params including the new bound variables */
                uint8_t n_extended = n_type_params + n_bound;
                const Symbol **ext_params = (const Symbol **)arena_alloc(
                    e->arena, n_extended * sizeof(const Symbol *));
                Kind *ext_kinds = (Kind *)arena_alloc(
                    e->arena, n_extended * sizeof(Kind));

                /* Copy existing type params */
                for (uint8_t i = 0; i < n_type_params; i++) {
                    ext_params[i] = type_params[i];
                    ext_kinds[i] = type_param_kinds[i];
                }

                /* Parse each bound variable */
                for (uint8_t i = 0; i < n_bound; i++) {
                    Form *vf = vars_form->as.list.items[i];
                    if (vf->tag != F_SYM) {
                        diag_emit(DIAG_ERROR, vf->span,
                                  "'%s' bound variable must be a symbol",
                                  is_forall_form ? "forall" : "exists");
                        return NULL;
                    }
                    /* Reject shadowing of outer type params */
                    for (uint8_t j = 0; j < n_type_params; j++) {
                        if (type_params[j] == vf->as.sym) {
                            diag_emit(DIAG_WARNING, vf->span,
                                      "'%s' bound variable '%s' shadows an outer type variable",
                                      is_forall_form ? "forall" : "exists",
                                      vf->as.sym->name);
                        }
                    }
                    var_names[i] = vf->as.sym->name;
                    /* ET2-A: lowercase binders are row variables (effect rows);
                     * uppercase binders remain type variables (KIND_STAR). */
                    bool is_row_binder = (vf->as.sym->name[0] >= 'a'
                                          && vf->as.sym->name[0] <= 'z');
                    var_kinds[i] = is_row_binder ? KIND_ROW : KIND_STAR;
                    ext_params[n_type_params + i] = vf->as.sym;
                    ext_kinds[n_type_params + i] = is_row_binder ? KIND_ROW : KIND_STAR;
                }

                /* Parse the body type with bound vars in scope */
                Type *body_type = type_expr_from_form(e, body_form, rec_name,
                    ext_params, ext_kinds, n_extended);
                if (!body_type) return NULL;

                /* Build the TY_FORALL or TY_EXISTS node */
                Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
                memset(t, 0, sizeof(Type));
                t->kind = is_forall_form ? TY_FORALL : TY_EXISTS;
                t->copy_kind = CK_MOVE;
                t->hkt_kind = KIND_STAR;
                t->as.forall_.var_names = var_names;
                t->as.forall_.var_kinds = var_kinds;
                t->as.forall_.n_vars = n_bound;
                t->as.forall_.body = body_type;
                return t;
            }
        }

        /* ER2: (fn [param-types...] #{effect-row} :return-type) — function type
         * annotation form used in parameter position.
         * Syntax: (fn [] :int)            — zero-arg fn returning int, no effects
         *         (fn [] #{e} :int)       — zero-arg fn with effect-row variable e
         *         (fn [:int] #{Write} :nil) — one-arg fn with concrete effect row
         *
         * The param list is a vector of type annotations (or empty).
         * The optional #{...} map is an effect-row annotation.
         * The last element is the return type. */
        if (form->as.list.len >= 2 && form->as.list.items[0]->tag == F_SYM
                && form->as.list.items[0]->as.sym == e->sym_fn) {
            uint32_t idx = 1; /* skip 'fn' head */
            /* Expect a vector of param type annotations */
            if (idx >= form->as.list.len || form->as.list.items[idx]->tag != F_VEC) {
                diag_emit(DIAG_ERROR, form->span,
                          "'fn' type expression requires a parameter vector: (fn [params...] :return)");
                return NULL;
            }
            Form *params_vec = form->as.list.items[idx++];
            /* Parse param types from the vector */
            uint8_t n_fn_args = (uint8_t)params_vec->as.list.len;
            TypeKind fn_arg_kinds[MAX_FN_ARITY];
            memset(fn_arg_kinds, 0, sizeof(fn_arg_kinds));
            for (uint8_t pi2 = 0; pi2 < n_fn_args && pi2 < MAX_FN_ARITY; pi2++) {
                Type *at = type_expr_from_form(e, params_vec->as.list.items[pi2],
                                               rec_name, type_params, type_param_kinds, n_type_params);
                fn_arg_kinds[pi2] = at ? at->kind : TY_INT;
            }
            /* Optional #{...} effect-row annotation */
            EffectRow *fn_effect_row = NULL;
            if (idx < form->as.list.len && form->as.list.items[idx]->tag == F_MAP) {
                Form *row_form = form->as.list.items[idx++];
                uint8_t n_row_sym = (uint8_t)row_form->as.list.len;
                const Symbol **row_syms = (const Symbol **)arena_alloc(e->arena,
                    (n_row_sym ? n_row_sym : 1) * sizeof(Symbol *));
                uint8_t n_row_valid = 0;
                for (uint32_t rj = 0; rj < row_form->as.list.len; rj++) {
                    Form *item = row_form->as.list.items[rj];
                    if (item->tag == F_SYM) row_syms[n_row_valid++] = item->as.sym;
                }
                fn_effect_row = effect_row_unresolved(e->arena, row_syms, n_row_valid);
            }
            /* Return type — must be the last element */
            if (idx >= form->as.list.len) {
                diag_emit(DIAG_ERROR, form->span,
                          "'fn' type expression requires a return type: (fn [params...] :return)");
                return NULL;
            }
            Type *ret_t = type_expr_from_form(e, form->as.list.items[idx],
                                               rec_name, type_params, type_param_kinds, n_type_params);
            TypeKind ret_kind = ret_t ? ret_t->kind : TY_INT;

            Type *fn_t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *fn_t = type_fn(fn_arg_kinds, n_fn_args, ret_kind);
            if (fn_effect_row) fn_t->as.fn.effect_row = fn_effect_row;
            return fn_t;
        }

        /* Phase HRT1/LT2: (-> T1 T2 ... Tn) — function type expression.
         * All but the last element are argument types; the last is the result type.
         * LT2: ^linear may prefix any arg type: (-> ^linear T1 T2 R) marks T1 as linear.
         * Requires at least (-> result) i.e. 2 elements. */
        if (form->as.list.len >= 1 && form->as.list.items[0]->tag == F_SYM
                && form->as.list.items[0]->as.sym == e->sym_arrow) {
            uint32_t len = form->as.list.len;
            if (len < 2) {
                diag_emit(DIAG_ERROR, form->span,
                          "'->': function type requires at least a result type: (-> result) or (-> arg result)");
                return NULL;
            }
            /* LT2: Two-pass parse to handle optional ^linear annotations.
             * items[1..len-2] may contain ^linear prefixes before each arg type.
             * Pass 1: count actual type items (skip ^linear / ^unique / ^mut symbols) */
            uint8_t n_args = 0;
            for (uint32_t _pi = 1; _pi < len - 1; _pi++) {
                Form *_it = form->as.list.items[_pi];
                if (_it->tag == F_SYM && _it->as.sym == e->sym_caret_linear)   continue;
                if (_it->tag == F_SYM && _it->as.sym == e->sym_caret_unique)   continue;
                if (_it->tag == F_SYM && _it->as.sym == e->sym_caret_mut)      continue;
                if (_it->tag == F_SYM && _it->as.sym == e->sym_caret_affine)   continue;
                if (_it->tag == F_SYM && _it->as.sym == e->sym_caret_relevant) continue;
                n_args++;
            }
            if (n_args > MAX_FN_ARITY) {
                diag_emit(DIAG_ERROR, form->span,
                          "'->': too many function arguments (max %d)", MAX_FN_ARITY);
                return NULL;
            }

            TypeKind arg_kinds[MAX_FN_ARITY];
            Type *arg_full_types_local[MAX_FN_ARITY];
            bool arg_linear_local[MAX_FN_ARITY];
            bool arg_unique_local[MAX_FN_ARITY];
            bool arg_unique_mut_local[MAX_FN_ARITY];
            bool arg_affine_local[MAX_FN_ARITY];    /* ST0 */
            bool arg_relevant_local[MAX_FN_ARITY];  /* ST0 */
            bool any_poly_arg = false;
            for (uint8_t _ai = 0; _ai < MAX_FN_ARITY; _ai++) arg_linear_local[_ai] = false;
            for (uint8_t _ai = 0; _ai < MAX_FN_ARITY; _ai++) arg_unique_local[_ai] = false;
            for (uint8_t _ai = 0; _ai < MAX_FN_ARITY; _ai++) arg_unique_mut_local[_ai] = false;
            for (uint8_t _ai = 0; _ai < MAX_FN_ARITY; _ai++) arg_affine_local[_ai] = false;
            for (uint8_t _ai = 0; _ai < MAX_FN_ARITY; _ai++) arg_relevant_local[_ai] = false;

            /* Pass 2: parse types, tracking ^linear / ^unique / ^mut / ^affine / ^relevant prefix per arg */
            uint8_t arg_idx = 0;
            bool next_linear   = false;
            bool next_unique   = false;
            bool next_mut      = false;
            bool next_affine   = false;  /* ST0 */
            bool next_relevant = false;  /* ST0 */
            for (uint32_t _pi2 = 1; _pi2 < len - 1; _pi2++) {
                Form *_it2 = form->as.list.items[_pi2];
                if (_it2->tag == F_SYM && _it2->as.sym == e->sym_caret_linear) {
                    next_linear = true;
                    continue;
                }
                if (_it2->tag == F_SYM && _it2->as.sym == e->sym_caret_unique) {
                    next_unique = true;
                    continue;
                }
                if (_it2->tag == F_SYM && _it2->as.sym == e->sym_caret_mut) {
                    next_mut = true;
                    continue;
                }
                /* ST0: ^affine and ^relevant prefix annotations */
                if (_it2->tag == F_SYM && _it2->as.sym == e->sym_caret_affine) {
                    next_affine = true;
                    continue;
                }
                if (_it2->tag == F_SYM && _it2->as.sym == e->sym_caret_relevant) {
                    next_relevant = true;
                    continue;
                }
                Type *at = type_expr_from_form(e, _it2,
                                               rec_name, type_params, type_param_kinds, n_type_params);
                if (!at) return NULL;
                arg_full_types_local[arg_idx] = at;
                arg_linear_local[arg_idx] = next_linear;
                arg_unique_local[arg_idx] = next_unique;
                /* UT2: ^unique ^mut combination — track as unique_mut */
                arg_unique_mut_local[arg_idx] = next_unique && next_mut;
                arg_affine_local[arg_idx]   = next_affine;    /* ST0 */
                arg_relevant_local[arg_idx] = next_relevant;  /* ST0 */
                next_linear   = false;
                next_unique   = false;
                next_mut      = false;
                next_affine   = false;   /* ST0 */
                next_relevant = false;   /* ST0 */
                /* Type variables (TY_STRUCT with def=NULL) and quantified types
                 * are represented as TY_INT (int64_t) at the C level. */
                if (at->kind == TY_STRUCT && at->as.struct_.def == NULL) {
                    arg_kinds[arg_idx] = TY_INT; /* type variable -> universal int64_t */
                    any_poly_arg = true;
                } else if (at->kind == TY_FORALL || at->kind == TY_EXISTS) {
                    arg_kinds[arg_idx] = TY_PTR_VOID; /* rank-2 fn -> tur_poly_fn_t */
                    any_poly_arg = true;
                } else {
                    arg_kinds[arg_idx] = at->kind;
                }
                arg_idx++;
            }

            /* Result type — always items[len-1] */
            Type *result_type = type_expr_from_form(e, form->as.list.items[len - 1],
                                                     rec_name, type_params, type_param_kinds, n_type_params);
            if (!result_type) return NULL;

            TypeKind result_kind;
            bool poly_result = false;
            if (result_type->kind == TY_STRUCT && result_type->as.struct_.def == NULL) {
                result_kind = TY_INT;
                poly_result = true;
            } else {
                result_kind = result_type->kind;
            }

            Type *fn_t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *fn_t = type_fn(arg_kinds, n_args, result_kind);

            /* Store full type info if any polymorphic args or result */
            if (any_poly_arg || poly_result) {
                Type **aFT = (Type **)arena_alloc(e->arena, n_args * sizeof(Type *));
                for (uint8_t i = 0; i < n_args; i++) aFT[i] = arg_full_types_local[i];
                fn_t->as.fn.arg_full_types = aFT;
                if (poly_result) fn_t->as.fn.result_full_type = result_type;
            }

            /* LT2: Store arg_linear flags if any were annotated */
            {
                bool any_lin = false;
                for (uint8_t i = 0; i < n_args; i++) {
                    if (arg_linear_local[i]) { any_lin = true; break; }
                }
                if (any_lin) {
                    for (uint8_t i = 0; i < n_args; i++) {
                        fn_t->as.fn.arg_linear[i] = arg_linear_local[i];
                    }
                }
            }

            /* UT0: Store arg_unique flags if any were annotated */
            {
                bool any_uniq = false;
                for (uint8_t i = 0; i < n_args; i++) {
                    if (arg_unique_local[i]) { any_uniq = true; break; }
                }
                if (any_uniq) {
                    for (uint8_t i = 0; i < n_args; i++) {
                        fn_t->as.fn.arg_unique[i] = arg_unique_local[i];
                    }
                }
            }

            /* UT2: Store arg_unique_mut flags if any were annotated */
            {
                bool any_uniq_mut = false;
                for (uint8_t i = 0; i < n_args; i++) {
                    if (arg_unique_mut_local[i]) { any_uniq_mut = true; break; }
                }
                if (any_uniq_mut) {
                    for (uint8_t i = 0; i < n_args; i++) {
                        fn_t->as.fn.arg_unique_mut[i] = arg_unique_mut_local[i];
                    }
                }
            }

            /* ST0: Store arg_affine flags if any were annotated */
            {
                bool any_aff = false;
                for (uint8_t i = 0; i < n_args; i++) {
                    if (arg_affine_local[i]) { any_aff = true; break; }
                }
                if (any_aff) {
                    for (uint8_t i = 0; i < n_args; i++) {
                        fn_t->as.fn.arg_affine[i] = arg_affine_local[i];
                    }
                }
            }

            /* ST0: Store arg_relevant flags if any were annotated */
            {
                bool any_rel = false;
                for (uint8_t i = 0; i < n_args; i++) {
                    if (arg_relevant_local[i]) { any_rel = true; break; }
                }
                if (any_rel) {
                    for (uint8_t i = 0; i < n_args; i++) {
                        fn_t->as.fn.arg_relevant[i] = arg_relevant_local[i];
                    }
                }
            }

            return fn_t;
        }

        /* LT3: (lref T) — linear owning pointer type expression.
         * Syntax: (lref inner-type)  e.g. (lref int), (lref MyStruct)
         * Produces a TY_LREF type with CK_LINEAR copy kind. */
        if (form->as.list.len == 2
                && form->as.list.items[0]->tag == F_SYM
                && form->as.list.items[0]->as.sym == e->sym_lref) {
            Type *inner_t = type_expr_from_form(e, form->as.list.items[1],
                                                rec_name, type_params, type_param_kinds, n_type_params);
            if (!inner_t) return NULL;
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = type_lref(inner_t->kind);
            return t;
        }

        /* Empty list () = unit / nil type */
        if (form->as.list.len == 0) {
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = TYPE_NIL;
            return t;
        }

        /* Type application: (ctor arg1 arg2 ...) — left-associative currying.
         * (ctor a b) == ((ctor a) b), (ctor a b c) == (((ctor a) b) c), etc.
         * Requires at least 2 elements (ctor + 1 arg). */
        if (form->as.list.len < 2) {
            diag_emit(DIAG_ERROR, form->span,
                      "type expression must be a symbol or (ctor arg ...) application");
            return NULL;
        }

        /* Parse constructor */
        Type *ctor_type = type_expr_from_form(e, form->as.list.items[0], rec_name,
                                              type_params, type_param_kinds, n_type_params);
        if (!ctor_type) return NULL;

        /* Left-associatively apply each argument: (ctor a b c) → (((ctor a) b) c) */
        Type *t = ctor_type;
        for (uint32_t ai = 1; ai < form->as.list.len; ai++) {
            Type *arg_type = type_expr_from_form(e, form->as.list.items[ai], rec_name,
                                                  type_params, type_param_kinds, n_type_params);
            if (!arg_type) return NULL;

            /* Create TY_APP node */
            Type *app = (Type *)arena_alloc(e->arena, sizeof(Type));
            memset(app, 0, sizeof(Type));
            app->kind = TY_APP;
            app->copy_kind = CK_COPY;
            /* Derive kind: KIND_ARROW2 → KIND_ARROW → KIND_STAR */
            if (t->hkt_kind == KIND_ARROW2) {
                app->hkt_kind = KIND_ARROW;
            } else if (t->hkt_kind == KIND_ARROW) {
                app->hkt_kind = KIND_STAR;
            } else {
                app->hkt_kind = KIND_STAR;
            }
            app->as.app.fn  = t;
            app->as.app.arg = arg_type;
            t = app;
        }
        return t;
    } else if (form->tag == F_KEYWORD) {
        /* `:int`, `:bool`, etc. — treat the keyword name as a primitive type lookup */
        const Symbol *sym = form->as.sym;
        TypeKind k = typekind_from_symbol(sym->name);
        /* IT4: gate `any` behind the feature flags */
        if (k == TY_ANY && !g_union_types_enabled && !g_intersection_types_enabled) {
            diag_emit(DIAG_ERROR, form->span,
                      "'any' type requires -Xunion-types or -Xintersection-types");
            return NULL;
        }
        if (k != TY_UNKNOWN) {
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = type_from_kind(k);
            return t;
        }
        /* Unknown keyword type — return opaque struct placeholder */
        Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
        memset(t, 0, sizeof(Type));
        t->kind = TY_STRUCT;
        t->copy_kind = CK_MOVE;
        t->hkt_kind = KIND_STAR;
        t->as.struct_.def = NULL;
        return t;
    } else if (form->tag == F_TYPE_ANN) {
        /* Unwrap the `: type-expr` annotation wrapper and process the inner form */
        if (form->as.list.len == 0) {
            diag_emit(DIAG_ERROR, form->span, "empty type annotation");
            return NULL;
        }
        return type_expr_from_form(e, form->as.list.items[0], rec_name,
                                   type_params, type_param_kinds, n_type_params);
    } else {
        diag_emit(DIAG_ERROR, form->span,
                  "unsupported type expression form (expected symbol, keyword, or list)");
        return NULL;
    }
}

/* Elaborate (deftype Name [type-params...] body)
 *
 * Defines a recursive type alias with kind-polymorphic parameters.
 * Similar to defrec but accepts kind annotations on parameters and a body.
 * In v1, the body is stored but not fully validated.
 *
 * Syntax: (deftype Fix [^f] (Fix (f (Fix f))))
 *         (deftype Name [^f ^a] body)
 *
 * The kind of the deftype is determined by the kind parameters:
 *   []        -> KIND_STAR
 *   [^f]      -> KIND_ARROW
 *   [^f ^a]   -> KIND_ARROW (if ^f is *->* and ^a is *)
 *   [^^f]     -> KIND_ARROW2
 *
 * Phase HKT-P2: This is the primary mechanism for defining Fix and Free.
 */
static Expr *elab_deftype(Elab *e, const Form *call) {
    /* Minimum: (deftype Name [params] body) or (deftype Name body) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "deftype requires a name: (deftype Name [params] body)");
        return NULL;
    }

    /* Parse name */
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "deftype name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;

    /* Parse type parameters (optional) - arg 2 */
    const Symbol **type_params = NULL;
    Kind         *type_param_kinds = NULL;
    uint8_t n_type_params = 0;
    uint32_t body_index = 2;

    if (call->as.list.len >= 3) {
        Form *params_form = call->as.list.items[2];
        if (params_form->tag == F_VEC) {
            n_type_params = params_form->as.list.len;
            if (n_type_params > 0) {
                type_params = (const Symbol **)arena_alloc(e->arena,
                    n_type_params * sizeof(const Symbol *));
                type_param_kinds = (Kind *)arena_alloc(e->arena,
                    n_type_params * sizeof(Kind));
                for (uint8_t i = 0; i < n_type_params; i++) {
                    type_param_kinds[i] = KIND_STAR;  /* Default kind */
                }

                for (uint8_t i = 0; i < n_type_params; i++) {
                    Form *p = params_form->as.list.items[i];
                    if (p->tag != F_SYM) {
                        diag_emit(DIAG_ERROR, p->span,
                                  "deftype type parameter must be a symbol");
                        return NULL;
                    }
                    /* Phase HKT H1: '^name' prefix marks a kind * -> * (type constructor) */
                    /* Phase HKT H1: '^^name' prefix marks a kind * -> * -> * (binary type constructor) */
                    if (p->as.sym->len > 2 && p->as.sym->name[0] == '^' && p->as.sym->name[1] == '^') {
                        const char  *bare     = p->as.sym->name + 2;
                        uint32_t     bare_len = p->as.sym->len  - 2;
                        if (bare_len > 0 && bare[0] >= 'a' && bare[0] <= 'z') {
                            type_params[i]      = symtab_intern(e->st, strslice(bare, bare_len));
                            type_param_kinds[i] = KIND_ARROW2;
                        } else {
                            diag_emit(DIAG_ERROR, p->span,
                                      "'%s' is not a valid type parameter; "
                                      "use lowercase '^^name' for a kind '* -> * -> *' parameter",
                                      p->as.sym->name);
                            return NULL;
                        }
                    } else if (p->as.sym->len > 1 && p->as.sym->name[0] == '^') {
                        const char  *bare     = p->as.sym->name + 1;
                        uint32_t     bare_len = p->as.sym->len  - 1;
                        if (bare_len > 0 && bare[0] >= 'a' && bare[0] <= 'z') {
                            type_params[i]      = symtab_intern(e->st, strslice(bare, bare_len));
                            type_param_kinds[i] = KIND_ARROW;
                        } else {
                            diag_emit(DIAG_ERROR, p->span,
                                      "'%s' is not a valid type parameter; "
                                      "use lowercase '^name' for a kind '* -> *' parameter",
                                      p->as.sym->name);
                            return NULL;
                        }
                    } else {
                        type_params[i]      = p->as.sym;
                        type_param_kinds[i] = KIND_STAR;
                    }
                }
            }
            body_index = 3;
        }
    }

    /* Parse body - required */
    if (call->as.list.len <= body_index) {
        diag_emit(DIAG_ERROR, call->span,
                  "deftype requires a body type expression");
        return NULL;
    }
    Form *body_form = call->as.list.items[body_index];

    /* Parse the body as a type expression */
    Type *body_type = type_expr_from_form(e, body_form, name,
                                          type_params, type_param_kinds, n_type_params);
    if (!body_type) {
        return NULL;
    }

    /* Phase HKT-P2: Validate guarded recursion
     * The recursive type name must only appear under type constructors
     * (i.e., as an argument to a type constructor, not at the top level) */
    if (!type_is_guarded_recursive(*body_type, name->name)) {
        diag_emit(DIAG_ERROR, body_form->span,
                  "recursive type '%s' must have guarded recursion: "
                  "'%s' appears at top level of its own body; "
                  "wrap it in a type constructor (e.g., (Fix (f (Fix f))) not (Fix Fix))",
                  name->name, name->name);
        return NULL;
    }

    /* Determine the kind of this deftype:
     * - If there are kind parameters, the result kind is KIND_ARROW or KIND_ARROW2
     *   based on the number and kinds of parameters.
     * - If there are no parameters, it's KIND_STAR.
     * For simplicity in v1, we just check: if any param is KIND_ARROW2, result is KIND_ARROW2.
     * Otherwise, if any param is KIND_ARROW, result is KIND_ARROW.
     * Otherwise, KIND_STAR. */
    Kind result_kind = KIND_STAR;
    if (n_type_params > 0) {
        result_kind = KIND_STAR;
        for (uint8_t i = 0; i < n_type_params; i++) {
            if (type_param_kinds[i] == KIND_ARROW2) {
                result_kind = KIND_ARROW2;
                break;
            }
            if (type_param_kinds[i] == KIND_ARROW) {
                result_kind = KIND_ARROW;
            }
        }
    }

    /* Create TY_REC type */
    Type rec_type;
    memset(&rec_type, 0, sizeof(rec_type));
    rec_type.kind      = TY_REC;
    rec_type.copy_kind = CK_MOVE;
    rec_type.hkt_kind  = result_kind;
    rec_type.as.rec.name = name->name;
    rec_type.as.rec.body = body_type;

    /* Register in global scope */
    Binding *b = binding_new(e, name, rec_type, false, true, name_form->span);
    scope_add(&e->global, b);

    /* deftype has no runtime effect */
    return e_nil(e, call->span);
}

/* Elaborate (type-app F A)
 *
 * Type-level application: applies type constructor F to type argument A.
 * Creates a TY_APP type and validates kind constraints.
 *
 * Syntax: (type-app result int)
 *         (type-app Fix (result int))
 *
 * The result kind is derived from F's kind:
 *   - If F has kind KIND_ARROW (* -> *), result is KIND_STAR
 *   - If F has kind KIND_ARROW2 (* -> * -> *), result is KIND_ARROW
 *
 * Phase HKT-P1.
 */
static Expr *elab_type_app(Elab *e, const Form *call) {
    /* Minimum: (type-app F A) */
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "type-app requires (type-app F A)");
        return NULL;
    }

    /* Parse type constructor F (arg 1) - must be a symbol */
    Form *fn_form = call->as.list.items[1];
    if (fn_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, fn_form->span,
                  "type-app: type constructor must be a symbol");
        return NULL;
    }

    /* Look up F in the type environment */
    Binding *fn_binding = scope_lookup(e->scope, fn_form->as.sym);
    if (!fn_binding) {
        fn_binding = scope_lookup(&e->global, fn_form->as.sym);
    }
    Type fn_type;
    if (!fn_binding) {
        /* Fall back to typeclass environment: (type-app Functor option) */
        TypeClass *tc = typeclass_env_lookup_typeclass(&e->typeclass_env, fn_form->as.sym);
        if (!tc) {
            diag_emit(DIAG_ERROR, fn_form->span,
                      "type-app: unknown type constructor '%s'", fn_form->as.sym->name);
            return NULL;
        }
        fn_type = type_typeclass(tc);
        /* Set hkt_kind from number of type parameters so kind_of_type_app succeeds */
        if (tc->n_type_params == 1) {
            fn_type.hkt_kind = KIND_ARROW;
        } else if (tc->n_type_params >= 2) {
            fn_type.hkt_kind = KIND_ARROW2;
        } else {
            fn_type.hkt_kind = KIND_STAR;
        }
    } else {
        fn_type = fn_binding->type;
    }

    /* Parse type argument A (arg 2) - can be a symbol, keyword, or nested type-app */
    Form *arg_form = call->as.list.items[2];
    Type arg_type;
    
    if (arg_form->tag == F_SYM || arg_form->tag == F_KEYWORD || arg_form->tag == F_TYPE_ANN) {
        /* Unwrap F_TYPE_ANN if present */
        if (arg_form->tag == F_TYPE_ANN) {
            arg_form = (arg_form->as.list.len > 0) ? arg_form->as.list.items[0] : arg_form;
        }
        const Symbol *arg_sym = arg_form->as.sym;
        /* Try primitive types first */
        if (arg_sym->len == 3 && memcmp(arg_sym->name, "int", 3) == 0) {
            arg_type = TYPE_INT;
        } else if (arg_sym->len == 4 && memcmp(arg_sym->name, "bool", 4) == 0) {
            arg_type = TYPE_BOOL;
        } else if (arg_sym->len == 4 && memcmp(arg_sym->name, "cstr", 4) == 0) {
            arg_type = TYPE_CSTR;
        } else if ((arg_sym->len == 4 && memcmp(arg_sym->name, "void", 4) == 0) ||
                   (arg_sym->len == 3 && memcmp(arg_sym->name, "nil", 3) == 0)) {
            arg_type = TYPE_NIL;
        } else if (arg_sym->len == 9 && memcmp(arg_sym->name, "ptr<void>", 9) == 0) {
            arg_type = TYPE_PTR_VOID;
        } else {
            /* Look up as a type binding */
            Binding *arg_binding = scope_lookup(e->scope, arg_sym);
            if (!arg_binding) {
                arg_binding = scope_lookup(&e->global, arg_sym);
            }
            if (!arg_binding) {
                /* Unknown name — treat as an opaque type constructor (like definstance does).
                 * TY_STRUCT with no def emits void* in codegen, correct for heap-allocated
                 * containers like option, vec, etc. */
                memset(&arg_type, 0, sizeof(arg_type));
                arg_type.kind = TY_STRUCT;
                arg_type.copy_kind = CK_MOVE;
                arg_type.as.struct_.def = NULL;
            } else {
                arg_type = arg_binding->type;
            }
        }
    } else if (arg_form->tag == F_LIST) {
        /* Nested type application: (type-app result (type-app option int))
         * For now, recursively elaborate as type-app */
        Expr *arg_expr = elab_type_app(e, arg_form);
        if (!arg_expr) {
            return NULL;
        }
        diag_emit(DIAG_ERROR, arg_form->span,
                  "type-app: nested type applications not yet supported");
        return NULL;
    } else {
        diag_emit(DIAG_ERROR, arg_form->span,
                  "type-app: type argument must be a symbol, keyword, or type application");
        return NULL;
    }

    /* Create TY_APP type using the type_app helper */
    /* The type is computed but not stored - in v1, type-app is a compile-time-only
     * construct that validates the kind application. Future work will register
     * the result type for use in type annotations. */
    Type app_type = type_app(e->arena, fn_type, arg_type, call->span);
    (void)app_type;  /* Suppress unused variable warning in v1 */

    /* type-app has no runtime effect; return nil */
    return e_nil(e, call->span);
}

/* Phase HRT1: (:: expr type) — type ascription.
 * The ascribed type is checked for well-formedness but erased at codegen.
 * Syntax: (:: expr type-form) */
static Expr *elab_ascribe(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "'::' requires exactly two arguments: (:: expr type)");
        return NULL;
    }
    Form *expr_form = call->as.list.items[1];
    Form *type_form = call->as.list.items[2];

    /* Parse the ascribed type */
    Type *ascribed = type_expr_from_form(e, type_form, NULL, NULL, NULL, 0);
    if (!ascribed) return NULL;

    /* Elaborate the expression */
    Expr *inner = elab_form(e, expr_form);
    if (!inner) return NULL;

    /* Build ascription node — the expression's type is overridden by the ascribed type
     * for downstream type-checking purposes. Type is erased at codegen. */
    Expr *out = expr_new(e->arena, EX_ASCRIBE, *ascribed, call->span);
    out->as.ascribe_.inner = inner;
    return out;
}

/* Phase HRT2: (pack value (exists [a] T)) — existential introduction.
 * Boxes the value as an opaque existential; result type is the TY_EXISTS type node.
 * Syntax: (pack expr (exists [a] T)) */
static Expr *elab_pack(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "'pack' requires exactly 2 arguments: (pack value (exists [a] T))");
        return NULL;
    }
    Form *val_form  = call->as.list.items[1];
    Form *type_form = call->as.list.items[2];

    /* Parse the existential type annotation */
    Type *ex_type = type_expr_from_form(e, type_form, NULL, NULL, NULL, 0);
    if (!ex_type) return NULL;
    if (ex_type->kind != TY_EXISTS) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "pack: second argument must be an existential type (exists [a] T), got %s",
                  typekind_to_string(ex_type->kind));
        return NULL;
    }

    /* Elaborate the value to pack */
    Expr *val = elab_form(e, val_form);
    if (!val) return NULL;

    /* Result type is the full TY_EXISTS type node (void* at codegen) */
    Expr *out = expr_new(e->arena, EX_EXISTS_PACK, *ex_type, call->span);
    out->as.exists_pack_.value = val;
    return out;
}

/* Phase HRT2: (open packed [a v] body) — existential elimination.
 * Unboxes the existential value, binding v to the inner value.
 * Syntax: (open packed-expr [abstract-type-var value-name] body...) */
static Expr *elab_open(Elab *e, const Form *call) {
    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "'open' requires at least 3 arguments: (open packed [a v] body)");
        return NULL;
    }
    Form *packed_form  = call->as.list.items[1];
    Form *binders_form = call->as.list.items[2];

    /* Elaborate the packed expression */
    Expr *packed = elab_form(e, packed_form);
    if (!packed) return NULL;

    /* Accept TY_EXISTS (full info) or TY_PTR_VOID (opaque) */
    if (packed->type.kind != TY_EXISTS && packed->type.kind != TY_PTR_VOID) {
        diag_emit(DIAG_ERROR, packed_form->span,
                  "open: expected existential value (exists [a] T) or ptr<void>, got %s",
                  typekind_to_string(packed->type.kind));
        return NULL;
    }

    /* Parse [a v] binders */
    if ((binders_form->tag != F_VEC && binders_form->tag != F_LIST) ||
        binders_form->as.list.len != 2) {
        diag_emit(DIAG_ERROR, binders_form->span,
                  "open: binders must be a 2-element vector [abstract-type-var value-name]");
        return NULL;
    }
    Form *abstract_form = binders_form->as.list.items[0];
    Form *val_name_form = binders_form->as.list.items[1];
    if (abstract_form->tag != F_SYM || val_name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, binders_form->span,
                  "open: binders must be symbols");
        return NULL;
    }
    const Symbol *val_sym = val_name_form->as.sym;

    /* Determine type of v from the existential body type.
     * Type variables (TY_STRUCT with def=NULL) resolve to TY_INT (int64_t at runtime). */
    Type v_type = TYPE_INT;  /* default: int64_t */
    if (packed->type.kind == TY_EXISTS) {
        const Type *T = packed->type.as.forall_.body;
        if (T) {
            if (T->kind == TY_STRUCT && T->as.struct_.def == NULL) {
                v_type = TYPE_INT;  /* type variable → int64_t */
            } else {
                v_type = *T;  /* use body type directly (by value) */
            }
        }
    }

    /* Create binding for v in the open body */
    Binding *v_binding = binding_new(e, val_sym, v_type, false, false, val_name_form->span);

    /* Push a scope with v */
    Scope inner;
    scope_init(&inner, e->scope);
    scope_add(&inner, v_binding);
    e->scope = &inner;

    /* Elaborate body (one or more forms) */
    uint32_t n_body = call->as.list.len - 3;
    Expr *body = NULL;
    bool body_ok = true;
    if (n_body == 1) {
        body = elab_form(e, call->as.list.items[3]);
        if (!body) body_ok = false;
    } else {
        Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
        for (uint32_t i = 0; i < n_body && body_ok; i++) {
            items[i] = elab_form(e, call->as.list.items[3 + i]);
            if (!items[i]) { body_ok = false; break; }
        }
        if (body_ok) {
            body = expr_new(e->arena, EX_DO, items[n_body - 1]->type, call->span);
            body->as.do_.items = items;
            body->as.do_.n = n_body;
        }
    }

    /* Restore scope */
    e->scope = inner.parent;
    scope_free(&inner);

    if (!body_ok || !body) return NULL;

    Expr *out = expr_new(e->arena, EX_EXISTS_OPEN, body->type, call->span);
    out->as.exists_open_.packed      = packed;
    out->as.exists_open_.var_binding = v_binding;
    out->as.exists_open_.body        = body;
    return out;
}

/* Phase HRT1: create a poly wrapper thunk for passing a function to a rank-2 param.
 * The wrapper has signature: int64_t __poly_N(void *env, int64_t x0, ..., int64_t x_{arity-1})
 * Its body calls inner_b(x0, ..., x_{arity-1}), ignoring env.
 * Registers the wrapper as a file-level EX_FN_DEF and returns the wrapper Binding. */
static Binding *make_poly_wrapper(Elab *e, Binding *inner_b, uint8_t inner_arity, Span span) {
    /* Wrapper name */
    char wname[32];
    snprintf(wname, sizeof(wname), "__poly_%u", e->next_id++);
    const Symbol *wsym = symtab_intern(e->st, strslice(wname, (uint32_t)strlen(wname)));

    /* Wrapper params: env (ptr<void>) + inner_arity int64_t args */
    uint8_t w_arity = inner_arity + 1;
    if (w_arity > MAX_FN_ARITY) {
        diag_emit(DIAG_ERROR, span, "rank-2 wrapper: too many arguments");
        return NULL;
    }
    Binding **wparams = (Binding **)arena_alloc(e->arena, w_arity * sizeof(Binding *));
    Type *wparam_types = (Type *)arena_alloc(e->arena, w_arity * sizeof(Type));

    /* env param */
    char env_pname[40];
    snprintf(env_pname, sizeof(env_pname), "__poly_env_%u", e->next_id++);
    const Symbol *env_psym = symtab_intern(e->st, strslice(env_pname, (uint32_t)strlen(env_pname)));
    Binding *env_pb = binding_new(e, env_psym, TYPE_PTR_VOID, false, false, span);
    wparams[0] = env_pb;
    wparam_types[0] = TYPE_PTR_VOID;

    /* Arg params x0, x1, ... */
    Binding *arg_bs[MAX_FN_ARITY];
    for (uint8_t i = 0; i < inner_arity; i++) {
        char apname[40];
        snprintf(apname, sizeof(apname), "__poly_x%u_%u", i, e->next_id++);
        const Symbol *apsym = symtab_intern(e->st, strslice(apname, (uint32_t)strlen(apname)));
        Binding *apb = binding_new(e, apsym, TYPE_INT, false, false, span);
        wparams[i + 1] = apb;
        wparam_types[i + 1] = TYPE_INT;
        arg_bs[i] = apb;
    }

    /* Build call body: (inner_b x0 x1 ...) */
    TypeKind inner_result_kind = (inner_b->type.kind == TY_FN)
        ? inner_b->type.as.fn.result_kind : TY_INT;
    Expr **call_args = (Expr **)arena_alloc(e->arena, (inner_arity ? inner_arity : 1) * sizeof(Expr *));
    uint32_t call_poly_mask = 0;
    for (uint8_t i = 0; i < inner_arity; i++) {
        Expr *av = expr_new(e->arena, EX_VAR, TYPE_INT, span);
        av->as.var.binding = arg_bs[i];
        call_args[i] = av;
        /* Phase HRT3: if inner_b's param i is a poly fn, the wrapper receives it as int64_t
         * (a pointer to a stack-allocated tur_poly_fn_t). Mark it so emit can dereference. */
        if (inner_b->type.kind == TY_FN && inner_b->type.as.fn.arg_full_types) {
            const Type *aft = inner_b->type.as.fn.arg_full_types[i];
            if (aft && aft->kind == TY_FORALL) {
                call_poly_mask |= (1u << i);
            }
        }
    }
    Expr *call_body = expr_new(e->arena, EX_CALL, type_from_kind(inner_result_kind), span);
    call_body->as.call_.fn_binding = inner_b;
    call_body->as.call_.args = inner_arity > 0 ? call_args : NULL;
    call_body->as.call_.n_args = inner_arity;
    call_body->as.call_.fn_expr = NULL;
    call_body->as.call_.dict_arg = NULL;
    call_body->as.call_.is_poly_call = false;
    call_body->as.call_.poly_arg_mask = call_poly_mask;

    /* Build wrapper fn type */
    TypeKind warg_kinds[MAX_FN_ARITY];
    warg_kinds[0] = TY_PTR_VOID;
    for (uint8_t i = 0; i < inner_arity; i++) warg_kinds[i + 1] = TY_INT;
    Type wfn_type = type_fn(warg_kinds, w_arity, inner_result_kind);

    Binding *wb = binding_new(e, wsym, wfn_type, false, true, span);
    scope_add(&e->global, wb);

    FnDef *wfd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    memset(wfd, 0, sizeof(FnDef));
    wfd->binding = wb;
    wfd->params = wparams;
    wfd->n_params = w_arity;
    wfd->body = call_body;
    wfd->is_variadic = false;
    wfd->closure = NULL;
    wfd->inferred_effect_row = NULL;
    wfd->param_types = wparam_types;
    constraint_set_init(&wfd->constraints);

    Expr *wdef = expr_new(e->arena, EX_FN_DEF, wfn_type, span);
    wdef->as.fn_def_.fn = wfd;
    elab_register_file_def(e, wdef);

    return wb;
}

/* Phase HRT1/HRT4: Helper to extract the underlying fn binding from a poly arg expression.
 * Handles EX_VAR and EX_ASCRIBE(EX_VAR). Returns NULL if not a simple fn ref.
 * Phase HRT4: follows source_binding for let-bound aliases of global functions. */
static Binding *poly_arg_fn_binding(Expr *arg) {
    if (arg->kind == EX_VAR) {
        Binding *b = arg->as.var.binding;
        /* For is_poly_fn bindings (already tur_poly_fn_t), return as-is — caller uses passthrough. */
        if (b->is_poly_fn) return b;
        /* Follow source_binding chain to resolve let-bound aliases back to global fns. */
        if (b->source_binding) return b->source_binding;
        return b;
    }
    if (arg->kind == EX_ASCRIBE) return poly_arg_fn_binding(arg->as.ascribe_.inner);
    return NULL;
}

/* Phase HRT1: Elaborate a call through a rank-2 polymorphic function parameter.
 * fn_binding->is_poly_fn is true; the call emits fn_name.fn(fn_name.env, args...) */
static Expr *elab_poly_call(Elab *e, const Form *call, Binding *fn_binding) {
    uint32_t n_args = call->as.list.len - 1;

    /* Elaborate all arguments normally */
    Expr **args = (Expr **)arena_alloc(e->arena, (n_args ? n_args : 1) * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!args[i]) return NULL;
    }

    /* Phase HRT3: Detect nested poly-fn args in the body type.
     * If body->arg_full_types[i] is TY_FORALL, wrap that arg with EX_POLY_WRAP
     * and mark it in poly_arg_mask so emit can pass it by pointer. */
    const Type *poly = fn_binding->poly_type;
    uint32_t poly_arg_mask = 0;
    if (poly && poly->kind == TY_FORALL) {
        const Type *pbody = poly->as.forall_.body;
        if (pbody && pbody->kind == TY_FN && pbody->as.fn.arg_full_types) {
            for (uint32_t i = 0; i < n_args && i < (uint32_t)pbody->as.fn.arity; i++) {
                const Type *aft = pbody->as.fn.arg_full_types[i];
                if (aft && aft->kind == TY_FORALL) {
                    /* Arg i is a nested poly fn — wrap it or pass through if already poly fn. */
                    Binding *inner_b = poly_arg_fn_binding(args[i]);
                    if (!inner_b) {
                        diag_emit(DIAG_ERROR, call->as.list.items[1 + i]->span,
                                  "rank-3: polymorphic function argument must be a named function");
                        return NULL;
                    }
                    Expr *orig_arg = args[i];
                    Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, orig_arg->span);
                    wrap->as.poly_wrap_.inner = orig_arg;
                    if (inner_b->is_poly_fn) {
                        /* HRT4: pass-through — already a tur_poly_fn_t. */
                        wrap->as.poly_wrap_.wrapper_binding = NULL;
                    } else {
                        uint8_t inner_arity = (inner_b->type.kind == TY_FN)
                            ? (uint8_t)inner_b->type.as.fn.arity : 1;
                        Binding *wrapper_b = make_poly_wrapper(e, inner_b, inner_arity, args[i]->span);
                        if (!wrapper_b) return NULL;
                        wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
                    }
                    args[i] = wrap;
                    poly_arg_mask |= (1u << i);
                }
            }
        }
    }

    /* Determine return type by instantiation.
     * For (forall [a] (-> a a)): result matches first arg's type.
     * For (forall [s] (-> s int)): result is int (concrete). */
    TypeKind result_kind = TY_INT;
    if (poly && poly->kind == TY_FORALL) {
        const Type *body = poly->as.forall_.body;
        if (body && body->kind == TY_FN) {
            const Type *rfull = body->as.fn.result_full_type;
            if (rfull && rfull->kind == TY_STRUCT && rfull->as.struct_.def == NULL) {
                /* Result is a type variable — instantiate from first arg's type */
                result_kind = (n_args > 0 && args[0]) ? args[0]->type.kind : TY_INT;
            } else if (rfull) {
                result_kind = rfull->kind;
            } else {
                result_kind = body->as.fn.result_kind;
            }
        }
    }

    Expr *out = expr_new(e->arena, EX_CALL, type_from_kind(result_kind), call->span);
    out->as.call_.fn_binding = fn_binding;
    out->as.call_.args = n_args > 0 ? args : NULL;
    out->as.call_.n_args = n_args;
    out->as.call_.fn_expr = NULL;
    out->as.call_.dict_arg = NULL;
    out->as.call_.is_poly_call = true;
    out->as.call_.poly_arg_mask = poly_arg_mask;
    return out;
}

/* Elaborate (defclass Name [type-params...] (method1 ...) (method2 ...) ...)
 *
 * Defines a new typeclass with type parameters and methods.
 * Syntax: (defclass Eq [a] (eq? [x : a, y : a] : bool))
 *         (defclass Show [a] (show [x : a] : cstr))
 */
static Expr *elab_defclass(Elab *e, const Form *call) {
    /* Minimum: (defclass Name) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "defclass requires a name: (defclass Name [...])");
        return NULL;
    }
    
    /* Parse typeclass name */
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defclass name must be a symbol");
        return NULL;
    }
    const Symbol *name = name_form->as.sym;

    /* Phase HKT H3: Functor, Applicative, Monad, Traversable, Foldable are now
     * defined (in stdlib/typeclass.tur), not reserved.  The only guard remaining
     * is the standard "already defined" check below. */

    /* Check if already defined */
    TypeClass *existing = typeclass_env_lookup_typeclass(&e->typeclass_env, name);
    if (existing) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "typeclass '%s' is already defined", name->name);
        return NULL;
    }
    
    /* Parse type parameters (optional) */
    const Symbol **type_params = NULL;
    Kind         *type_param_kinds = NULL;
    uint8_t n_type_params = 0;
    uint32_t methods_start = 2;

    if (call->as.list.len >= 3) {
        Form *params_form = call->as.list.items[2];
        if (params_form->tag == F_VEC) {
            n_type_params = params_form->as.list.len;
            if (n_type_params > 0) {
                type_params = (const Symbol **)arena_alloc(e->arena,
                    n_type_params * sizeof(const Symbol *));
                /* Phase PTC2: Explicitly initialize all type_param_kinds to KIND_STAR.
                 * Note: arena_alloc does NOT zero memory, contrary to the old comment. */
                type_param_kinds = (Kind *)arena_alloc(e->arena,
                    n_type_params * sizeof(Kind));
                for (uint8_t i = 0; i < n_type_params; i++) {
                    type_param_kinds[i] = KIND_STAR;  /* Default kind for all params */
                }
                
                for (uint8_t i = 0; i < n_type_params; i++) {
                    Form *p = params_form->as.list.items[i];
                    /* Phase HKT H1: [f :kind] vector form — not yet supported;
                     * users should use the '^name' prefix instead. */
                    if (p->tag == F_VEC) {
                        diag_emit(DIAG_ERROR, p->span,
                                  "kind annotations in defclass type parameters must use "
                                  "the '^name' prefix syntax (e.g. '^f' for kind '* -> *')");
                        return NULL;
                    }
                    if (p->tag != F_SYM) {
                        diag_emit(DIAG_ERROR, p->span,
                                  "type parameter must be a symbol");
                        return NULL;
                    }
                    /* Phase HKT H1: '^name' prefix marks a kind * -> * (type constructor)
                     * parameter.  The canonical name stored is 'name' (without '^').
                     * Phase HKT H5: '^^name' prefix marks a kind * -> * -> * (binary
                     * type constructor) parameter. */
                    if (p->as.sym->len > 2 && p->as.sym->name[0] == '^' && p->as.sym->name[1] == '^') {
                        const char  *bare     = p->as.sym->name + 2;
                        uint32_t     bare_len = p->as.sym->len  - 2;
                        /* Only lowercase-leading names are kind variables. */
                        if (bare_len > 0 && bare[0] >= 'a' && bare[0] <= 'z') {
                            type_params[i]      = symtab_intern(e->st, strslice(bare, bare_len));
                            type_param_kinds[i] = KIND_ARROW2;
                        } else {
                            diag_emit(DIAG_ERROR, p->span,
                                      "'%s' is not a valid type parameter; "
                                      "use lowercase '^^name' for a kind '* -> * -> *' parameter",
                                      p->as.sym->name);
                            return NULL;
                        }
                    } else if (p->as.sym->len > 1 && p->as.sym->name[0] == '^') {
                        const char  *bare     = p->as.sym->name + 1;
                        uint32_t     bare_len = p->as.sym->len  - 1;
                        /* Only lowercase-leading names are kind variables. */
                        if (bare_len > 0 && bare[0] >= 'a' && bare[0] <= 'z') {
                            type_params[i]      = symtab_intern(e->st, strslice(bare, bare_len));
                            type_param_kinds[i] = KIND_ARROW;
                        } else {
                            /* Uppercase — treat as a constraint annotation in wrong place. */
                            diag_emit(DIAG_ERROR, p->span,
                                      "'%s' is not a valid type parameter; "
                                      "use lowercase '^name' for a kind '* -> *' parameter",
                                      p->as.sym->name);
                            return NULL;
                        }
                    } else {
                        type_params[i]      = p->as.sym;
                        type_param_kinds[i] = KIND_STAR;
                    }
                }
            }
            methods_start = 3;
        }
    }
    
    /* Parse methods */
    TypeClassMethod *methods = NULL;
    uint8_t n_methods = 0;
    
    /* First pass: count methods */
    for (uint32_t i = methods_start; i < call->as.list.len; i++) {
        n_methods++;
    }
    
    if (n_methods == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "defclass requires at least one method");
        return NULL;
    }
    
    /* Allocate methods array */
    methods = (TypeClassMethod *)arena_alloc(e->arena, n_methods * sizeof(TypeClassMethod));
    
    /* Second pass: parse each method */
    for (uint32_t i = 0; i < n_methods; i++) {
        Form *method_form = call->as.list.items[methods_start + i];
        TypeClassMethod *method = parse_typeclass_method(e, method_form, call->span);
        if (!method) return NULL;
        methods[i] = *method;  /* Copy the method struct */
    }
    
    /* Register the typeclass in the environment */
    TypeClass *tc = typeclass_env_register_typeclass(&e->typeclass_env, name);
    if (!tc) {
        diag_emit(DIAG_ERROR, call->span,
                  "failed to register typeclass '%s'", name->name);
        return NULL;
    }
    
    tc->type_params       = type_params;
    tc->type_param_kinds  = type_param_kinds;
    tc->n_type_params     = n_type_params;
    tc->methods           = methods;
    tc->n_methods         = n_methods;
    /* Phase HKT-P4: record the file that defined this typeclass. */
    tc->origin_file_id    = call->span.file_id;

    /* Create a TYPECLASS_DEF expression for codegen */
    Expr *tc_expr = expr_new(e->arena, EX_TYPECLASS_DEF, TYPE_NIL, call->span);
    tc_expr->as.typeclass_def_.typeclass = tc;
    elab_register_file_def(e, tc_expr);
    
    /* Create a nil expression as the result (defclass returns nothing) */
    return e_nil(e, call->span);
}

/* Elaborate (definstance ClassName [type-args...] (method1 [args...] body...) ...)
 *
 * Defines an instance of a typeclass for concrete types.
 * Syntax: (definstance Eq int (eq? [x y] (== x y)))
 *         (definstance Show int (show [x] (int->str x)))
 */
static Expr *elab_definstance(Elab *e, const Form *call) {
    /* Minimum: (definstance ClassName) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "definstance requires a typeclass name: (definstance ClassName ...)");
        return NULL;
    }
    
    /* Parse typeclass name */
    Form *tc_form = call->as.list.items[1];
    if (tc_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, tc_form->span,
                  "definstance typeclass name must be a symbol");
        return NULL;
    }
    const Symbol *tc_name = tc_form->as.sym;
    
    /* Look up the typeclass */
    TypeClass *tc = typeclass_env_lookup_typeclass(&e->typeclass_env, tc_name);
    if (!tc) {
        diag_emit(DIAG_ERROR, tc_form->span,
                  "typeclass '%s' is not defined", tc_name->name);
        return NULL;
    }
    
    /* Parse type arguments (optional) */
    Type *type_args = NULL;
    /* Phase HKT H3: track original symbol for each type arg so method name
     * mangling can use "option", "vec", etc. instead of the generic "T".
     * Only allocated when needed (at least one unknown/constructor type arg). */
    const Symbol **type_arg_syms = NULL;
    uint8_t n_type_args = 0;
    uint32_t impls_start = 2;
    
    if (call->as.list.len >= 3) {
        Form *args_form = call->as.list.items[2];
        if (args_form->tag == F_VEC) {
            n_type_args = args_form->as.list.len;
            if (n_type_args > 0) {
                type_args = (Type *)arena_alloc(e->arena, n_type_args * sizeof(Type));
                type_arg_syms = (const Symbol **)arena_alloc(e->arena,
                    n_type_args * sizeof(const Symbol *));
                for (uint8_t i = 0; i < n_type_args; i++) {
                    type_arg_syms[i] = NULL;  /* NULL means use default name */
                }
                for (uint8_t i = 0; i < n_type_args; i++) {
                    Form *arg = args_form->as.list.items[i];
                    /* Parse type keywords or symbols */
                    if (arg->tag == F_KEYWORD || arg->tag == F_SYM) {
                        const Symbol *kw = arg->as.sym;
                        if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                            type_args[i] = TYPE_INT;
                        } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                            type_args[i] = TYPE_BOOL;
                        } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                            type_args[i] = TYPE_CSTR;
                        } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
                                   (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                            type_args[i] = TYPE_NIL;
                        } else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) {
                            type_args[i] = TYPE_PTR_VOID;
                        } else {
                            /* Phase N4: Try new numeric type names first. */
                            TypeKind nk = typekind_from_symbol(kw->name);
                            if (nk != TY_UNKNOWN) {
                                type_args[i] = type_simple(nk, CK_COPY);
                            } else {
                                /* Check if this name refers to a known struct type.
                                 * If so, preserve the StructDef pointer so the kind
                                 * system can distinguish concrete structs (kind *)
                                 * from opaque type constructors (kind * -> *). */
                                Binding *sb = scope_lookup(e->scope, kw);
                                if (sb && sb->type.kind == TY_STRUCT && sb->type.as.struct_.def) {
                                    type_args[i] = sb->type;
                                } else {
                                    /* Phase HKT H3: Unknown name — treat as an opaque type constructor.
                                     * TY_STRUCT without a StructDef causes codegen to emit 'void *' for
                                     * all parameters that inherit this type, which is the correct C type
                                     * for containers represented as heap pointers (option, vec, etc.).
                                     * Track the symbol name so method name mangling can use it. */
                                    memset(&type_args[i], 0, sizeof(type_args[i]));
                                    type_args[i].kind = TY_STRUCT;
                                    type_args[i].copy_kind = CK_MOVE;
                                    type_args[i].as.struct_.def = NULL;
                                }
                                type_arg_syms[i] = kw;
                            }
                        }
                    } else if (arg->tag == F_LIST && arg->as.list.len == 2) {
                        /* Phase HKT §3: (constructor arg) — partial type application.
                         * Parses `(result int)` in type position as TY_APP where
                         * fn = TY_STRUCT(constructor, KIND_ARROW2) and arg = concrete type.
                         * This allows `(definstance Functor [(result int)] ...)`. */
                        Form *ctor_form = arg->as.list.items[0];
                        Form *aarg_form = arg->as.list.items[1];
                        if (ctor_form->tag != F_SYM && ctor_form->tag != F_KEYWORD) {
                            diag_emit(DIAG_ERROR, ctor_form->span,
                                      "type application constructor must be a symbol");
                            return NULL;
                        }
                        const Symbol *ctor_sym = ctor_form->as.sym;
                        /* Parse the argument type (must be a primitive or known sym) */
                        Type app_arg_type;
                        if (aarg_form->tag == F_SYM || aarg_form->tag == F_KEYWORD) {
                            const Symbol *akw = aarg_form->as.sym;
                            if (akw->len == 3 && memcmp(akw->name, "int", 3) == 0) {
                                app_arg_type = TYPE_INT;
                            } else if (akw->len == 4 && memcmp(akw->name, "bool", 4) == 0) {
                                app_arg_type = TYPE_BOOL;
                            } else if (akw->len == 4 && memcmp(akw->name, "cstr", 4) == 0) {
                                app_arg_type = TYPE_CSTR;
                            } else if ((akw->len == 4 && memcmp(akw->name, "void", 4) == 0) ||
                                       (akw->len == 3 && memcmp(akw->name, "nil", 3) == 0)) {
                                app_arg_type = TYPE_NIL;
                            } else {
                                /* Unknown type arg — treat as opaque struct */
                                memset(&app_arg_type, 0, sizeof(app_arg_type));
                                app_arg_type.kind = TY_STRUCT;
                                app_arg_type.copy_kind = CK_MOVE;
                                app_arg_type.as.struct_.def = NULL;
                            }
                        } else {
                            diag_emit(DIAG_ERROR, aarg_form->span,
                                      "type application argument must be a type keyword or symbol");
                            return NULL;
                        }
                        /* Build fn type: TY_STRUCT with no def, KIND_ARROW2 (binary constructor) */
                        Type *fn_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                        memset(fn_type, 0, sizeof(Type));
                        fn_type->kind = TY_STRUCT;
                        fn_type->copy_kind = CK_MOVE;
                        fn_type->hkt_kind = KIND_ARROW2;
                        fn_type->as.struct_.def = NULL;
                        /* Build arg type on arena */
                        Type *arg_type_ptr = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *arg_type_ptr = app_arg_type;
                        /* Assemble TY_APP */
                        memset(&type_args[i], 0, sizeof(type_args[i]));
                        type_args[i].kind = TY_APP;
                        type_args[i].copy_kind = CK_MOVE;
                        /* fn of KIND_ARROW2 applied to one arg → KIND_ARROW */
                        type_args[i].hkt_kind = KIND_ARROW;
                        type_args[i].as.app.fn  = fn_type;
                        type_args[i].as.app.arg = arg_type_ptr;
                        /* Store constructor sym for name mangling (e.g. "result") */
                        type_arg_syms[i] = ctor_sym;
                    } else {
                        diag_emit(DIAG_ERROR, arg->span,
                                  "unsupported type argument in definstance");
                        return NULL;
                    }
                }
            }
            /* Phase HKT-P1: After parsing individual type arguments, combine consecutive
             * symbols into TY_APP for implicit type application syntax [result int].
             * This allows both [(result int)] (explicit) and [result int] (implicit). */
            if (n_type_args > 0) {
                for (uint8_t i = 0; i < n_type_args; ) {
                    if (i + 1 < n_type_args) {
                        /* Check if current is TY_STRUCT (potential constructor) and next is a type */
                        if (type_args[i].kind == TY_STRUCT && type_args[i].as.struct_.def == NULL) {
                            Type *next_type = &type_args[i + 1];
                            /* Next can be any concrete type (primitive, TY_STRUCT, or TY_APP) */
                            if (next_type->kind != TY_UNKNOWN) {
                                /* Combine into TY_APP */
                                Type *fn_type = (Type *)arena_alloc(e->arena, sizeof(Type));
                                *fn_type = type_args[i];  /* Copy the constructor type */
                                fn_type->hkt_kind = KIND_ARROW2;  /* Assume binary constructor */
                                
                                Type *arg_type_ptr = (Type *)arena_alloc(e->arena, sizeof(Type));
                                *arg_type_ptr = type_args[i + 1];
                                
                                /* Create TY_APP */
                                type_args[i].kind = TY_APP;
                                type_args[i].copy_kind = CK_MOVE;
                                type_args[i].hkt_kind = KIND_ARROW;  /* ARROW2 applied to 1 arg */
                                type_args[i].as.app.fn = fn_type;
                                type_args[i].as.app.arg = arg_type_ptr;
                                /* Store constructor sym for name mangling if we have it */
                                /* type_arg_syms[i] already contains the constructor symbol */
                                
                                /* Remove the second type arg by shifting */
                                for (uint8_t j = i + 1; j < n_type_args - 1; j++) {
                                    type_args[j] = type_args[j + 1];
                                    if (type_arg_syms) {
                                        type_arg_syms[j] = type_arg_syms[j + 1];
                                    }
                                }
                                n_type_args--;
                                /* Don't advance i - recheck current position */
                                continue;
                            }
                        }
                    }
                    i++;
                }
            }
            impls_start = 3;
        }
    }
    
    /* Phase PTC1: Parse type parameter constraints (optional)
     * Syntax: (definstance Clone [Pair a b] [(Clone a) (Clone b)] (clone [x] ...))
     * Constraint vector is a vector of lists: [(Clone a) (Clone b)]
     * Each constraint is a list (Clone a) where Clone is the typeclass and a is the type param.
     * After type args at index 2, check for a constraint vector at index impls_start.
     */
    TypeConstraint *type_param_constraints = NULL;
    uint8_t n_type_param_constraints = 0;
    
    if (call->as.list.len > impls_start) {
        Form *next_form = call->as.list.items[impls_start];
        if (next_form->tag == F_VEC && next_form->as.list.len > 0) {
            /* Check if this is a constraint vector by looking at the first item */
            /* If the first item is a list (F_LIST), it's likely [(Clone a) (Clone b)] */
            /* If the first item is a symbol (F_SYM), it might be a flat [Clone a Clone b] vector */
            bool is_constraint_vector = false;
            if (next_form->as.list.len > 0) {
                Form *first_item = next_form->as.list.items[0];
                if (first_item->tag == F_LIST) {
                    /* Vector of lists format: [(Clone a) (Clone b)] */
                    is_constraint_vector = true;
                    n_type_param_constraints = next_form->as.list.len;
                } else if (next_form->as.list.len >= 2) {
                    /* Could be flat format [Clone a Clone b ...] */
                    /* Check if all items alternate between SYM (typeclass) and SYM/KEYWORD (type arg) */
                    is_constraint_vector = true;
                    n_type_param_constraints = next_form->as.list.len / 2;
                }
            }
            
            if (is_constraint_vector && n_type_param_constraints > 0) {
                type_param_constraints = (TypeConstraint *)arena_alloc(
                    e->arena, n_type_param_constraints * sizeof(TypeConstraint));
                
                Form *first_item = next_form->as.list.items[0];
                if (first_item->tag == F_LIST) {
                    /* Parse as vector of lists: [(Clone a) (Clone b)] */
                    for (uint8_t i = 0; i < n_type_param_constraints; i++) {
                        Form *constraint_form = next_form->as.list.items[i];
                        if (constraint_form->tag != F_LIST || constraint_form->as.list.len < 1) {
                            diag_emit(DIAG_ERROR, constraint_form->span,
                                      "definstance: constraint must be a list like (Clone a), got tag %d with %d items",
                                      constraint_form->tag, constraint_form->as.list.len);
                            return NULL;
                        }
                        
                        Form *tc_name_form = constraint_form->as.list.items[0];
                        if (tc_name_form->tag != F_SYM) {
                            diag_emit(DIAG_ERROR, tc_name_form->span,
                                      "definstance: constraint typeclass name must be a symbol");
                            return NULL;
                        }
                        
                        TypeClass *constraint_tc = typeclass_env_lookup_typeclass(
                            &e->typeclass_env, tc_name_form->as.sym);
                        if (!constraint_tc) {
                            diag_emit(DIAG_ERROR, tc_name_form->span,
                                      "definstance: constraint typeclass '%s' is not defined",
                                      tc_name_form->as.sym->name);
                            return NULL;
                        }
                        
                        /* Type argument being constrained (optional, at index 1) */
                        Type constrained_type = TYPE_INT; /* Default */
                        if (constraint_form->as.list.len >= 2) {
                            Form *type_arg_form = constraint_form->as.list.items[1];
                            if (type_arg_form->tag == F_SYM) {
                                const Symbol *type_param_name = type_arg_form->as.sym;
                                for (uint8_t j = 0; j < n_type_args; j++) {
                                    if (type_arg_syms && type_arg_syms[j] &&
                                        type_arg_syms[j] == type_param_name) {
                                        constrained_type = type_args[j];
                                        break;
                                    }
                                }
                                if (constrained_type.kind == TY_INT) {
                                    if (type_arg_form->as.sym->len == 3 &&
                                        memcmp(type_arg_form->as.sym->name, "int", 3) == 0) {
                                        constrained_type = TYPE_INT;
                                    } else if (type_arg_form->as.sym->len == 4 &&
                                               memcmp(type_arg_form->as.sym->name, "bool", 4) == 0) {
                                        constrained_type = TYPE_BOOL;
                                    } else if (type_arg_form->as.sym->len == 4 &&
                                               memcmp(type_arg_form->as.sym->name, "cstr", 4) == 0) {
                                        constrained_type = TYPE_CSTR;
                                    }
                                }
                            }
                        }
                        
                        type_param_constraints[i] = (TypeConstraint){
                            .typeclass = constraint_tc,
                            .type_arg = constrained_type
                        };
                    }
                } else {
                    /* Parse as flat vector: [Clone a Clone b ...] */
                    uint8_t n_items = next_form->as.list.len;
                    if (n_items % 2 != 0) {
                        diag_emit(DIAG_ERROR, next_form->span,
                                  "definstance: flat constraint vector must have an even number of items");
                        return NULL;
                    }
                    n_type_param_constraints = n_items / 2;
                    /* Reallocate for flat format - old allocation will be GC'd with arena */
                    type_param_constraints = (TypeConstraint *)arena_alloc(
                        e->arena, n_type_param_constraints * sizeof(TypeConstraint));
                    
                    for (uint8_t i = 0; i < n_type_param_constraints; i++) {
                        uint8_t idx = i * 2;
                        Form *tc_name_form = next_form->as.list.items[idx];
                        if (tc_name_form->tag != F_SYM) {
                            diag_emit(DIAG_ERROR, tc_name_form->span,
                                      "definstance: constraint typeclass name must be a symbol");
                            return NULL;
                        }
                        
                        TypeClass *constraint_tc = typeclass_env_lookup_typeclass(
                            &e->typeclass_env, tc_name_form->as.sym);
                        if (!constraint_tc) {
                            diag_emit(DIAG_ERROR, tc_name_form->span,
                                      "definstance: constraint typeclass '%s' is not defined",
                                      tc_name_form->as.sym->name);
                            return NULL;
                        }
                        
                        Type constrained_type = TYPE_INT;
                        if (idx + 1 < n_items) {
                            Form *type_arg_form = next_form->as.list.items[idx + 1];
                            if (type_arg_form->tag == F_SYM) {
                                const Symbol *type_param_name = type_arg_form->as.sym;
                                for (uint8_t j = 0; j < n_type_args; j++) {
                                    if (type_arg_syms && type_arg_syms[j] &&
                                        type_arg_syms[j] == type_param_name) {
                                        constrained_type = type_args[j];
                                        break;
                                    }
                                }
                                if (constrained_type.kind == TY_INT) {
                                    if (type_arg_form->as.sym->len == 3 &&
                                        memcmp(type_arg_form->as.sym->name, "int", 3) == 0) {
                                        constrained_type = TYPE_INT;
                                    } else if (type_arg_form->as.sym->len == 4 &&
                                               memcmp(type_arg_form->as.sym->name, "bool", 4) == 0) {
                                        constrained_type = TYPE_BOOL;
                                    } else if (type_arg_form->as.sym->len == 4 &&
                                               memcmp(type_arg_form->as.sym->name, "cstr", 4) == 0) {
                                        constrained_type = TYPE_CSTR;
                                    }
                                }
                            }
                        }
                        
                        type_param_constraints[i] = (TypeConstraint){
                            .typeclass = constraint_tc,
                            .type_arg = constrained_type
                        };
                    }
                }
                impls_start++; /* Skip past the constraint vector */
            }
        }
    }

    /* Phase PTC2: Validate type parameter constraints */
    if (type_param_constraints && n_type_param_constraints > 0) {
        for (uint8_t i = 0; i < n_type_param_constraints; i++) {
            TypeClass *constraint_tc = type_param_constraints[i].typeclass;
            Type constrained_type = type_param_constraints[i].type_arg;
            bool is_primitive = (constrained_type.kind == TY_INT ||
                                 constrained_type.kind == TY_BOOL ||
                                 constrained_type.kind == TY_CSTR ||
                                 constrained_type.kind == TY_NIL ||
                                 constrained_type.kind == TY_FLOAT ||
                                 constrained_type.kind == TY_PTR_VOID);
            
            /* PTC2: For primitive types, validate that a constraint instance exists.
             * For user-defined types (structs, etc.), defer validation to PTC3.
             * Phase B1: float is treated as a primitive for constraint purposes. */
            if (is_primitive) {
                Type lookup_type = constrained_type;
                if (constrained_type.kind == TY_BOOL) {
                    lookup_type = TYPE_BOOL;
                } else if (constrained_type.kind == TY_CSTR) {
                    lookup_type = TYPE_CSTR;
                } else if (constrained_type.kind == TY_NIL) {
                    lookup_type = TYPE_NIL;
                } else if (constrained_type.kind == TY_PTR_VOID) {
                    lookup_type = TYPE_PTR_VOID;
                } else if (constrained_type.kind == TY_FLOAT) {
                    lookup_type = TYPE_FLOAT;
                }
                
                TypeClassInstance *inst = typeclass_env_lookup_instance(
                    &e->typeclass_env, constraint_tc, &lookup_type, 1);
                if (!inst) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                        TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED,
                        "typeclass constraint not satisfied: no instance of '%s' for type '%s'",
                        constraint_tc->name->name, type_name(constrained_type));
                    return NULL;
                }
            }
            /* For user-defined types, the constraint is stored on the instance
             * but not validated here (deferred to PTC3 for constraint propagation). */
        }
    }

    /* Validate type argument count matches typeclass parameters */
    if (n_type_args != tc->n_type_params) {
        diag_emit(DIAG_ERROR, call->span,
                  "definstance: expected %d type arguments for '%s', got %d",
                  tc->n_type_params, tc_name->name, n_type_args);
        return NULL;
    }

    /* Phase HKT H1: Kind constraint validation.
     * If the typeclass has kind-annotated parameters (type_param_kinds != NULL),
     * verify that each type argument satisfies the expected kind.
     * Primitive types (int, bool, cstr, nil, float) have kind *.
     * Struct types and other user-defined types are treated as kind * -> *.
     * A definstance that supplies a primitive where kind '* -> *' is expected
     * is a compile-time error (TUR-E0012). */
    if (tc->type_param_kinds != NULL) {
        for (uint8_t i = 0; i < n_type_args; i++) {
            Kind expected = tc->type_param_kinds[i];
            if (expected == KIND_ARROW || expected == KIND_ARROW2) {
                TypeKind tk = type_args[i].kind;
                bool is_primitive = (tk == TY_INT  || tk == TY_BOOL  || tk == TY_CSTR ||
                                     tk == TY_NIL  || tk == TY_FLOAT || tk == TY_PTR_VOID);
                if (is_primitive) {
                    diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0012_KIND_MISMATCH,
                        "kind mismatch (TUR-E0012): typeclass '%s' parameter %d expects kind "
                        "'%s' (a type constructor), but '%s' has kind '*'",
                        tc_name->name, (int)(i + 1),
                        kind_to_string(expected),
                        type_name(type_args[i]));
                    return NULL;
                }
            }
        }
    }

    /* Parse method implementations */
    /* Each method impl is a function definition without the 'defn' keyword */
    /* Syntax: (method-name [param1 param2 ...] body...)
     * The number of methods must match the typeclass definition.
     */
    
    if (call->as.list.len - impls_start < tc->n_methods) {
        diag_emit(DIAG_ERROR, call->span,
                  "definstance: expected %d method implementations for '%s', got %d",
                  tc->n_methods, tc_name->name, call->as.list.len - impls_start);
        return NULL;
    }
    
    /* For Phase 15 v1, we store method implementations as FnDef pointers.
     * In a full implementation, these would be stored in the instance and
     * codegen would generate dictionary structs. For now, we validate syntax.
     */
    FnDef **method_impls = NULL;
    
    for (uint8_t i = 0; i < tc->n_methods; i++) {
        Form *impl_form = call->as.list.items[impls_start + i];
        if (impl_form->tag != F_LIST || impl_form->as.list.len < 3) {
            diag_emit(DIAG_ERROR, impl_form->span,
                      "method implementation requires (name [params...] body...)");
            return NULL;
        }
        
        /* Parse the method implementation as a function */
        /* For now, we just validate the name matches */
        Form *impl_name_form = impl_form->as.list.items[0];
        if (impl_name_form->tag != F_SYM) {
            diag_emit(DIAG_ERROR, impl_name_form->span,
                      "method implementation name must be a symbol");
            return NULL;
        }
        
        if (impl_name_form->as.sym != tc->methods[i].name) {
            diag_emit(DIAG_ERROR, impl_name_form->span,
                      "method implementation name '%s' doesn't match typeclass method '%s'",
                      impl_name_form->as.sym->name, tc->methods[i].name->name);
            return NULL;
        }
        
        /* Elaborate the method implementation as a function */
        /* The form is (method-name [params...] body...) */
        if (!method_impls) {
            method_impls = (FnDef **)arena_alloc(e->arena, tc->n_methods * sizeof(FnDef *));
        }
        
        /* Create a synthetic name for this method implementation */
        /* Format: __inst_<typeclass>_<method>_<typeargs> e.g. __inst_MyEq_eq_int */
        enum {
            MAX_INSTANCE_METHOD_NAME_LEN = 192,
            MAX_SANITIZED_METHOD_NAME_LEN = 64,
            MAX_INSTANCE_TYPE_SUFFIX_LEN = 64,
        };
        char method_name[MAX_INSTANCE_METHOD_NAME_LEN];
        
        /* Sanitize method name for C identifier (replace invalid chars with _) */
        char sanitized_method_name[MAX_SANITIZED_METHOD_NAME_LEN];
        const char *method_name_str = tc->methods[i].name->name;
        uint32_t method_name_len = tc->methods[i].name->len;
        if (method_name_len >= sizeof(sanitized_method_name)) {
            method_name_len = sizeof(sanitized_method_name) - 1;
        }
        memcpy(sanitized_method_name, method_name_str, method_name_len);
        sanitized_method_name[method_name_len] = '\0';
        for (char *p = sanitized_method_name; *p; p++) {
            if (!isalnum((unsigned char)*p) && *p != '_') {
                *p = '_';
            }
        }
        
        /* Build type arg suffix */
        char type_suffix[MAX_INSTANCE_TYPE_SUFFIX_LEN] = "";
        size_t type_suffix_len = 0;
        for (uint8_t j = 0; j < n_type_args; j++) {
            const char *type_component = NULL;
            char ctor_name_buf[32];  /* for TY_STRUCT/constructor names */
            switch (type_args[j].kind) {
                case TY_INT: type_component = "int"; break;
                case TY_BOOL: type_component = "bool"; break;
                case TY_CSTR: type_component = "cstr"; break;
                case TY_NIL: type_component = "nil"; break;
                case TY_PTR_VOID: type_component = "ptr_void"; break;
                case TY_STRUCT:
                    /* Phase HKT H3: use the original symbol name when available,
                     * falling back to "T" for unnamed struct type args. */
                    if (type_arg_syms && type_arg_syms[j]) {
                        /* Sanitise the symbol name to a valid C identifier component */
                        uint32_t sym_len = type_arg_syms[j]->len;
                        if (sym_len >= sizeof(ctor_name_buf))
                            sym_len = (uint32_t)(sizeof(ctor_name_buf) - 1);
                        memcpy(ctor_name_buf, type_arg_syms[j]->name, sym_len);
                        ctor_name_buf[sym_len] = '\0';
                        for (char *p = ctor_name_buf; *p; p++) {
                            if (!isalnum((unsigned char)*p)) *p = '_';
                        }
                        type_component = ctor_name_buf;
                    } else if (type_args[j].as.struct_.def) {
                        type_component = type_args[j].as.struct_.def->name;
                    } else {
                        type_component = "T";
                    }
                    break;
                case TY_APP: {
                    /* Phase HKT §3: partial type application — encode as "ctor_arg" */
                    const char *ctor_part = "T";
                    const char *arg_part  = "T";
                    if (type_arg_syms && type_arg_syms[j]) {
                        ctor_part = type_arg_syms[j]->name;
                    }
                    if (type_args[j].as.app.arg) {
                        const char *n = type_name(*type_args[j].as.app.arg);
                        if (n) arg_part = n;
                    }
                    snprintf(ctor_name_buf, sizeof(ctor_name_buf), "%s_%s", ctor_part, arg_part);
                    for (char *p = ctor_name_buf; *p; p++) {
                        if (!isalnum((unsigned char)*p)) *p = '_';
                    }
                    type_component = ctor_name_buf;
                    break;
                }
                default: type_component = "T"; break;
            }
            int written = snprintf(type_suffix + type_suffix_len,
                                   sizeof(type_suffix) - type_suffix_len,
                                   "%s%s", j == 0 ? "_" : "", type_component);
            if (written < 0 || (size_t)written >= sizeof(type_suffix) - type_suffix_len) {
                diag_emit(DIAG_ERROR, impl_form->span,
                          "typeclass instance method name is too long");
                return NULL;
            }
            type_suffix_len += (size_t)written;
        }
        int method_name_written =
            snprintf(method_name, sizeof(method_name), "__inst_%.*s_%s%s",
                     (int)tc_name->len, tc_name->name, sanitized_method_name, type_suffix);
        if (method_name_written < 0 || (size_t)method_name_written >= sizeof(method_name)) {
            diag_emit(DIAG_ERROR, impl_form->span,
                      "typeclass instance method name is too long");
            return NULL;
        }
        
        const Symbol *method_sym = symtab_intern(e->st, 
            strslice(method_name, (uint32_t)strlen(method_name)));
        
        /* Parse the method implementation form */
        /* impl_form is (method-name [params...] :return-type body...) */
        /* or (method-name [params...] body...) if no return type */
        Form *impl_params_form = impl_form->as.list.items[1];
        uint32_t impl_body_start = 2;
        Type return_type = tc->methods[i].return_type;  /* Default from typeclass */
        
        /* Check for return type annotation after params */
        if (impl_form->as.list.len >= 3) {
            Form *ret_or_body = impl_form->as.list.items[2];
            if (ret_or_body->tag == F_KEYWORD) {
                /* This is a return type annotation */
                const Symbol *kw = ret_or_body->as.sym;
                if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                    return_type = TYPE_INT;
                } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                    return_type = TYPE_BOOL;
                } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                    return_type = TYPE_CSTR;
                } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
                           (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                    return_type = TYPE_NIL;
                } else if ((kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) ||
                           (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0)) {
                    return_type = TYPE_PTR_VOID;
                }
                impl_body_start = 3;
            }
        }
        
        /* Parse parameters */
        Binding **method_params = NULL;
        uint8_t n_method_params = 0;
        Type *method_param_types = NULL;
        
        if (impl_params_form->tag == F_VEC) {
            n_method_params = impl_params_form->as.list.len;
            if (n_method_params > 0) {
                method_params = (Binding **)arena_alloc(e->arena, 
                    n_method_params * sizeof(Binding *));
                method_param_types = (Type *)arena_alloc(e->arena, 
                    n_method_params * sizeof(Type));
                
                for (uint8_t j = 0; j < n_method_params; j++) {
                    Form *p = impl_params_form->as.list.items[j];
                    Type param_type = TYPE_INT;
                    
                    /* Phase 15: Try to use type from typeclass method definition */
                    if (tc->methods[i].param_types && j < tc->methods[i].n_params) {
                        param_type = tc->methods[i].param_types[j];
                    }
                    
                    /* Phase 15: Substitute type variables with type args */
                    /* For v1: if the param type is TYPE_INT (default) and we have type args,
                     * use the first type arg */
                    if (param_type.kind == TY_INT && n_type_args > 0) {
                        param_type = type_args[0];
                    }
                    
                    /* Phase HRT3: if the param type is TY_FORALL, treat it as a poly fn param.
                     * Phase CCL: also treat :fn-annotated params (param_is_fn) as poly fn. */
                    bool param_is_poly = (param_type.kind == TY_FORALL || param_type.kind == TY_EXISTS);
                    if (!param_is_poly
                        && tc->methods[i].param_is_fn
                        && j < tc->methods[i].n_params
                        && tc->methods[i].param_is_fn[j]) {
                        param_is_poly = true;
                        param_type = TYPE_PTR_VOID;
                    }
                    Type c_param_type = param_is_poly ? TYPE_PTR_VOID : param_type;
                    if (p->tag == F_SYM) {
                        /* Simple parameter name */
                        method_params[j] = binding_new(e, p->as.sym, c_param_type, false, false, p->span);
                        if (param_is_poly) {
                            method_params[j]->is_poly_fn = true;
                            Type *pt = (Type *)arena_alloc(e->arena, sizeof(Type));
                            *pt = param_type;
                            method_params[j]->poly_type = pt;
                        }
                        method_param_types[j] = c_param_type;
                    } else if (p->tag == F_VEC && p->as.list.len >= 1) {
                        /* Parameter with type annotation: [name : type] */
                        Form *name_f = p->as.list.items[0];
                        if (name_f->tag == F_SYM) {
                            /* Check for type annotation */
                            if (p->as.list.len >= 2 && p->as.list.items[1]->tag == F_KEYWORD) {
                                const Symbol *kw = p->as.list.items[1]->as.sym;
                                if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                                    param_type = TYPE_INT;
                                } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                                    param_type = TYPE_BOOL;
                                } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                                    param_type = TYPE_CSTR;
                                } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
                                           (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                                    param_type = TYPE_NIL;
                                }
                            }
                            /* Phase CCL: :fn-annotated params also become poly fn */
                            if (!param_is_poly
                                && tc->methods[i].param_is_fn
                                && j < tc->methods[i].n_params
                                && tc->methods[i].param_is_fn[j]) {
                                param_is_poly = true;
                                param_type = TYPE_PTR_VOID;
                            }
                            param_is_poly = param_is_poly
                                || (param_type.kind == TY_FORALL || param_type.kind == TY_EXISTS);
                            c_param_type = param_is_poly ? TYPE_PTR_VOID : param_type;
                            method_params[j] = binding_new(e, name_f->as.sym, c_param_type, false, false, p->span);
                            if (param_is_poly) {
                                method_params[j]->is_poly_fn = true;
                                Type *pt = (Type *)arena_alloc(e->arena, sizeof(Type));
                                *pt = param_type;
                                method_params[j]->poly_type = pt;
                            }
                            method_param_types[j] = c_param_type;
                        } else {
                            diag_emit(DIAG_ERROR, p->span,
                                      "method parameter name must be a symbol");
                            return NULL;
                        }
                    } else {
                        diag_emit(DIAG_ERROR, p->span,
                                  "method parameter must be a symbol or vector");
                        return NULL;
                    }
                }
            }
        }
        
        /* Elaborate the body - push a scope with method parameters */
        Scope method_scope;
        scope_init(&method_scope, e->scope);
        e->scope = &method_scope;

        /* Add method parameters to scope */
        for (uint8_t j = 0; j < n_method_params; j++) {
            scope_add(&method_scope, method_params[j]);
        }

        /* ER3: Increment fn_body_depth so that (perform ...) inside an instance
         * method body does not trigger TUR-E0008 (unhandled effect at top level).
         * The handler is expected to be provided at the call site. */
        e->fn_body_depth++;

        Expr *method_body = e_nil(e, impl_form->span);
        uint32_t n_body = impl_form->as.list.len - impl_body_start;
        if (n_body > 0) {
            if (n_body == 1) {
                method_body = elab_form(e, impl_form->as.list.items[impl_body_start]);
            } else {
                Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
                for (uint32_t k = 0; k < n_body; k++) {
                    items[k] = elab_form(e, impl_form->as.list.items[impl_body_start + k]);
                    if (!items[k]) { e->fn_body_depth--; e->scope = method_scope.parent; scope_free(&method_scope); return NULL; }
                }
                method_body = expr_new(e->arena, EX_DO, items[n_body - 1]->type, impl_form->span);
                method_body->as.do_.items = items;
                method_body->as.do_.n = n_body;
            }
        }

        e->fn_body_depth--;

        /* Pop method scope */
        e->scope = method_scope.parent;
        scope_free(&method_scope);
        
        /* Create a proper function type for the method */
        TypeKind param_kinds[MAX_FN_ARITY];
        for (uint8_t j = 0; j < n_method_params; j++) {
            param_kinds[j] = method_param_types[j].kind;
        }
        Type fn_type = type_fn(param_kinds, n_method_params, return_type.kind);
        
        /* Create FnDef for the method implementation */
        FnDef *method_fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
        Binding *method_binding = binding_new(e, method_sym, fn_type, false, true, impl_form->span);
        method_fd->binding = method_binding;
        method_fd->params = method_params;
        method_fd->n_params = n_method_params;
        method_fd->body = method_body;
        method_fd->is_variadic = false;
        method_fd->closure = NULL;
        method_fd->param_types = method_param_types;
        method_fd->may_capture = false;
        method_fd->inferred_effect_row = NULL;  /* must be NULL; effect_check_pass reads this */
        constraint_set_init(&method_fd->constraints);
        
        /* Register the method function at file scope */
        scope_add(&e->global, method_binding);
        
        /* Create a file-scope definition expression */
        Expr *method_def_expr = expr_new(e->arena, EX_FN_DEF, fn_type, impl_form->span);
        method_def_expr->as.fn_def_.fn = method_fd;
        elab_register_file_def(e, method_def_expr);
        
        method_impls[i] = method_fd;
    }
    
    /* Register the instance */
    TypeClassInstance *inst = typeclass_env_register_instance(&e->typeclass_env, tc);
    if (!inst) {
        diag_emit(DIAG_ERROR, call->span,
                  "failed to register instance for '%s'", tc_name->name);
        return NULL;
    }
    
    inst->type_args = type_args;
    inst->n_type_args = n_type_args;
    inst->type_arg_syms = type_arg_syms;  /* Phase HKT §1: store for dict naming */
    inst->method_impls = method_impls;
    inst->n_method_impls = tc->n_methods;
    /* Phase PTC1: Store type parameter constraints */
    inst->type_param_constraints = type_param_constraints;
    inst->n_type_param_constraints = n_type_param_constraints;
    /* Phase HKT-P4: record the file that defined this instance. */
    inst->origin_file_id = call->span.file_id;

    /* Phase HKT-P4: Orphan instance check.
     *
     * Rule: an instance is "orphan" when NEITHER the typeclass NOR any
     * struct-type type argument was defined in the current compilation unit.
     * In Rust terms: you may only define Foo<Bar> if you own Foo or Bar.
     *
     * Now a hard DIAG_ERROR since the module system (P19-6) has landed. */
    if (tc->origin_file_id != 0 && tc->origin_file_id != call->span.file_id) {
        /* The typeclass is from a different file.
         * Check if any struct type-arg was defined here. */
        bool owns_a_type_arg = false;
        for (uint8_t i = 0; i < n_type_args && !owns_a_type_arg; i++) {
            if (type_args[i].kind == TY_STRUCT && type_args[i].as.struct_.def) {
                if (type_args[i].as.struct_.def->origin_file_id == call->span.file_id) {
                    owns_a_type_arg = true;
                }
            }
        }
        if (!owns_a_type_arg) {
            diag_emit_with_code(DIAG_ERROR, call->span,
                      TUR_E0013_ORPHAN_INSTANCE,
                      "orphan instance: typeclass '%s' is defined in a different "
                      "module and none of the type arguments belong to this module; "
                      "move the instance to the module that defines the typeclass or "
                      "one of the type arguments",
                      tc_name->name);
        }
    }
    
    /* Create an INSTANCE_DEF expression for codegen */
    Expr *inst_expr = expr_new(e->arena, EX_INSTANCE_DEF, TYPE_NIL, call->span);
    inst_expr->as.instance_def_.instance = inst;
    elab_register_file_def(e, inst_expr);
    
    /* Create a nil expression as the result (definstance returns nothing) */
    return e_nil(e, call->span);
}

/* Phase 15: Elaborate (.method obj arg1 arg2 ...) - typeclass method call
 * 
 * Syntax: (.method obj arg1 arg2 ...)
 * Looks up the method in the typeclass for the type of obj, and generates a call.
 * For v1, we use direct method function calls (monomorphic only).
 * Full dictionary passing deferred to v2.
 */
/* Phase 12: EX_GET_FIELD — struct field access via (.fieldname s)
 *
 * Syntax: (.field s)  where s has type TY_STRUCT
 * Returns: the type of the named field
 * Also resolves immutable/mutable borrow of a field:
 *   (& (.field s))   → EX_BORROW_IMMUT wrapping EX_GET_FIELD
 *   (&mut (.field s)) → EX_BORROW_MUT wrapping EX_GET_FIELD
 */

/* Phase H §1: Build an EX_DICT node for a typeclass instance singleton.
 * The dict_name field is computed from the instance's typeclass and type args
 * using the same naming convention as emit.c (emit_dict_name / EX_INSTANCE_DEF).
 * Returns a TY_PTR_VOID-typed Expr that, when emitted, yields the address of
 * the global dictionary singleton cast to int64_t. */
static Expr *make_dict_expr(Elab *e, TypeClassInstance *inst, Span span) {
    Expr *d = expr_new(e->arena, EX_DICT, type_from_kind(TY_PTR_VOID), span);
    d->as.dict_.instance = inst;

    /* Compute dict_name: "dict_<TypeClass>_<typearg>..." */
    const TypeClass *tc = inst->typeclass;
    char *dst = d->as.dict_.dict_name;
    size_t dstlen = sizeof(d->as.dict_.dict_name);
    char type_suffix[64] = "";
    for (uint8_t i = 0; i < inst->n_type_args; i++) {
        if (i == 0) strncat(type_suffix, "_", sizeof(type_suffix) - strlen(type_suffix) - 1);
        const char *component = "T";
        switch (inst->type_args[i].kind) {
            case TY_INT:      component = "int";      break;
            case TY_BOOL:     component = "bool";     break;
            case TY_CSTR:     component = "cstr";     break;
            case TY_NIL:      component = "nil";      break;
            case TY_PTR_VOID: component = "ptr_void"; break;
            case TY_STRUCT:
                if (inst->type_arg_syms && inst->type_arg_syms[i])
                    component = inst->type_arg_syms[i]->name;
                else if (inst->type_args[i].as.struct_.def &&
                         inst->type_args[i].as.struct_.def->name)
                    component = inst->type_args[i].as.struct_.def->name;
                break;
            default: break;
        }
        char comp_buf[32];
        strncpy(comp_buf, component, sizeof(comp_buf) - 1);
        comp_buf[sizeof(comp_buf) - 1] = '\0';
        for (char *p = comp_buf; *p; p++) {
            if (!isalnum((unsigned char)*p)) *p = '_';
        }
        strncat(type_suffix, comp_buf, sizeof(type_suffix) - strlen(type_suffix) - 1);
    }
    snprintf(dst, dstlen, "dict_%s%s", tc->name->name, type_suffix);
    return d;
}

static Expr *elab_method_call(Elab *e, const Form *call) {
    /* call is (.method obj arg1 arg2 ...)
     * call->as.list.items[0] is the symbol .method
     */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "method call requires (.method obj arg1 ...)");
        return NULL;
    }
    
    /* Parse method name from the symbol (skip the leading '.') */
    Form *head = call->as.list.items[0];
    const Symbol *method_sym = head->as.sym;
    const char *method_name = method_sym->name + 1;  /* Skip '.' */
    uint32_t method_name_len = method_sym->len - 1;

    /* Phase D1: Type witness @TypeName at call sites.
     * The reader converts @TypeName into (deref TypeName), so we detect
     * the pattern (deref sym) at items[1] where sym names a registered
     * typeclass instance type argument for this method.  When found the
     * witness pins the dispatch directly to that instance; the receiver
     * is items[2] and extra arguments start at items[3]. */
    if (call->as.list.len >= 3 &&
        call->as.list.items[1]->tag == F_LIST &&
        call->as.list.items[1]->as.list.len == 2 &&
        call->as.list.items[1]->as.list.items[0]->tag == F_SYM &&
        strcmp(call->as.list.items[1]->as.list.items[0]->as.sym->name, "deref") == 0 &&
        call->as.list.items[1]->as.list.items[1]->tag == F_SYM) {

        const Symbol *witness_sym = call->as.list.items[1]->as.list.items[1]->as.sym;
        const char   *witness_name = witness_sym->name;
        uint32_t      witness_len  = witness_sym->len;

        /* Walk all registered instances looking for one whose type arg symbol
         * (or primitive type name) matches the witness identifier. */
        TypeClassInstance *witness_inst   = NULL;
        FnDef             *witness_method_fn = NULL;
        bool               any_inst_for_method = false;

        for (TypeClassInstance *inst = e->typeclass_env.instances;
             inst != NULL && !witness_inst; inst = inst->next) {
            for (uint8_t mi = 0; mi < inst->typeclass->n_methods; mi++) {
                const TypeClassMethod *m = &inst->typeclass->methods[mi];
                if (m->name->len != method_name_len ||
                    memcmp(m->name->name, method_name, method_name_len) != 0) continue;
                any_inst_for_method = true;
                /* Check if a type arg name matches the witness identifier. */
                bool name_match = false;
                for (uint8_t ti = 0; ti < inst->n_type_args && !name_match; ti++) {
                    if (inst->type_arg_syms && inst->type_arg_syms[ti] &&
                        inst->type_arg_syms[ti]->len == witness_len &&
                        memcmp(inst->type_arg_syms[ti]->name, witness_name, witness_len) == 0) {
                        name_match = true;
                    }
                    if (!name_match) {
                        /* Primitive type names that have no symbol (e.g. int, bool). */
                        const char *prim = NULL;
                        switch (inst->type_args[ti].kind) {
                            case TY_INT:   prim = "int";   break;
                            case TY_BOOL:  prim = "bool";  break;
                            case TY_CSTR:  prim = "cstr";  break;
                            case TY_NIL:   prim = "nil";   break;
                            case TY_FLOAT: prim = "float"; break;
                            default: break;
                        }
                        if (prim && strcmp(prim, witness_name) == 0) name_match = true;
                    }
                }
                if (name_match) {
                    witness_inst      = inst;
                    witness_method_fn = inst->method_impls[mi];
                }
                break; /* one method match per instance is enough */
            }
        }

        if (witness_inst) {
            /* Witness resolved: receiver is items[2], extra args are items[3..]. */
            Expr *obj_w = elab_form(e, call->as.list.items[2]);
            if (!obj_w) return NULL;

            uint32_t n_args_w = call->as.list.len - 3;
            Expr **args_w = (Expr **)arena_alloc(e->arena, n_args_w * sizeof(Expr *));
            for (uint32_t i = 0; i < n_args_w; i++) {
                args_w[i] = elab_form(e, call->as.list.items[3 + i]);
                if (!args_w[i]) return NULL;
            }

            /* Determine result type from the method's binding. */
            Type result_type_w;
            if (witness_method_fn->binding->type.kind == TY_FN) {
                result_type_w = type_from_kind(witness_method_fn->binding->type.as.fn.result_kind);
            } else {
                result_type_w = witness_method_fn->body ? witness_method_fn->body->type : TYPE_INT;
                if (result_type_w.kind == TY_UNKNOWN || result_type_w.kind == TY_NIL)
                    result_type_w = TYPE_INT;
            }

            /* Build the EX_DICT node for the pinned instance. */
            Expr *dict_w = make_dict_expr(e, witness_inst, call->span);
            strncpy(dict_w->as.dict_.method_name, method_name,
                    sizeof(dict_w->as.dict_.method_name) - 1);
            dict_w->as.dict_.method_name[sizeof(dict_w->as.dict_.method_name) - 1] = '\0';
            for (char *p = dict_w->as.dict_.method_name; *p; p++) {
                if (!isalnum((unsigned char)*p) && *p != '_') *p = '_';
            }

            /* Build EX_CALL: args array is [obj_w, args_w...]. */
            Expr **call_args_w = (Expr **)arena_alloc(e->arena,
                                                       (n_args_w + 1) * sizeof(Expr *));
            call_args_w[0] = obj_w;
            for (uint32_t i = 0; i < n_args_w; i++) call_args_w[i + 1] = args_w[i];

            Expr *out_w = expr_new(e->arena, EX_CALL, result_type_w, call->span);
            out_w->as.call_.fn_binding = NULL;
            out_w->as.call_.fn_expr    = dict_w;
            out_w->as.call_.args       = call_args_w;
            out_w->as.call_.n_args     = n_args_w + 1;
            out_w->as.call_.dict_arg   = dict_w;
            return out_w;

        } else if (any_inst_for_method) {
            /* Instances exist for this method but none match the witness type name.
             * The user clearly intended a witness (not a deref); emit a specific error. */
            diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                      "no instance of typeclass method '.%.*s' for type '%s' -- "
                      "check that (definstance ... [%s] ...) is in scope",
                      (int)method_name_len, method_name, witness_name, witness_name);
            return NULL;
        }
        /* No instances at all for this method: fall through to the normal path
         * so that (deref sym) is elaborated as the receiver and the existing
         * "no typeclass method found" error is emitted. */
    }

    /* Phase HKT H2: Elaborate the receiver object first so we can use its
     * type to select the correct typeclass instance (type-based dispatch).
     * This replaces the old "name-only / first-match" approach with a lookup
     * that distinguishes multiple instances of the same typeclass for
     * different types (e.g. MyShow[int] vs MyShow[bool]). */
    Expr *obj = elab_form(e, call->as.list.items[1]);
    if (!obj) return NULL;

    /* Phase 12: EX_GET_FIELD — if the form is exactly (.field s) with no extra
     * args, try to resolve it as a struct field access first. */
    if (call->as.list.len == 2) {
        /* Unwrap borrow types */
        Type base = obj->type;
        if (base.kind == TY_REF_IMMUT || base.kind == TY_REF_MUT) {
            base = type_from_kind(base.as.ref_borrow.target);
        }
        if (base.kind == TY_STRUCT) {
            /* Object is a struct — try field lookup */
            StructDef *def = base.as.struct_.def;
            for (uint32_t i = 0; i < def->n_fields; i++) {
                if (strcmp(def->fields[i].name, method_name) == 0) {
                    /* Found matching field — build EX_GET_FIELD */
                    TypeKind fkind = def->fields[i].kind;
                    TypeKind finner = def->fields[i].inner_kind;
                    Type field_type;
                    if (fkind == TY_REF || fkind == TY_LREF || fkind == TY_RC || fkind == TY_WEAK) {
                        field_type.kind = fkind;
                        field_type.copy_kind = typekind_default_copy_kind(fkind);
                        field_type.as.ref.inner = finner;
                    } else {
                        field_type = type_from_kind(fkind);
                    }
                    Expr *out = expr_new(e->arena, EX_GET_FIELD, field_type, call->span);
                    out->as.get_field_.struct_expr = obj;
                    out->as.get_field_.field_idx = i;
                    out->as.get_field_.def = def;
                    /* LT1/T3: Extracting an lref<T> field from a :move struct transfers
                     * linear ownership of that field out of the struct.  Mark the struct
                     * binding as moved so a second extraction of the same field triggers
                     * TUR_E0005 (use-after-move).  :linear struct receivers are already
                     * handled by the F_SYM is_linear_consumed path above. */
                    if (g_linear_enabled && fkind == TY_LREF &&
                            obj->kind == EX_VAR && type_is_move(obj->as.var.binding->type)) {
                        binding_mark_moved(obj->as.var.binding, call->span);
                    }
                    return out;
                }
            }
            /* Struct but no matching field — fall through to typeclass method lookup */
            /* In Phase PTC4, this allows (.method obj) to dispatch to typeclass
             * methods even when obj is a struct that doesn't have that field. */
        }
    }

    /* Phase 16 v2: capability field call — (.field-name cap arg1 arg2 ...)
     * When the receiver is a struct and the named field is :fn (TY_FN), and
     * there are arguments, emit an indirect function-pointer call through the
     * field. The call carries the EX_GET_FIELD as fn_expr for effect-row
     * propagation. Effect rows on the field are advisory in v1 (codegen erases
     * to a plain function pointer call). */
    if (call->as.list.len > 2) {
        Type base = obj->type;
        if (base.kind == TY_REF_IMMUT || base.kind == TY_REF_MUT) {
            base = type_from_kind(base.as.ref_borrow.target);
        }
        if (base.kind == TY_STRUCT) {
            StructDef *def = base.as.struct_.def;
            for (uint32_t i = 0; i < def->n_fields; i++) {
                if (strcmp(def->fields[i].name, method_name) == 0 &&
                    def->fields[i].kind == TY_FN) {
                    /* Build EX_GET_FIELD for the function pointer */
                    Type field_type = type_from_kind(TY_FN);
                    Expr *get_field = expr_new(e->arena, EX_GET_FIELD, field_type, call->span);
                    get_field->as.get_field_.struct_expr = obj;
                    get_field->as.get_field_.field_idx = i;
                    get_field->as.get_field_.def = def;

                    /* Elaborate arguments */
                    uint32_t n_args = call->as.list.len - 2;
                    Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
                    for (uint32_t j = 0; j < n_args; j++) {
                        args[j] = elab_form(e, call->as.list.items[2 + j]);
                        if (!args[j]) return NULL;
                    }

                    /* Build indirect EX_CALL through fn_expr */
                    Expr *call_out = expr_new(e->arena, EX_CALL, TYPE_INT, call->span);
                    call_out->as.call_.fn_binding = NULL;
                    call_out->as.call_.fn_expr = get_field;
                    call_out->as.call_.args = args;
                    call_out->as.call_.n_args = n_args;
                    return call_out;
                }
            }
        }
    }

    /* Phase 16 v2 fallback: when receiver has TY_UNKNOWN or TY_INT type
     * (untyped parameters default to TY_INT), search all registered struct defs
     * for a :fn field matching the method name.
     * This allows (.method-name cap args...) when cap has no explicit type annotation,
     * as long as exactly one struct in scope has a TY_FN field with that name. */
    if (call->as.list.len > 2 &&
        (obj->type.kind == TY_UNKNOWN || obj->type.kind == TY_INT)) {
        StructDef *matched_def = NULL;
        uint32_t matched_idx = 0;
        bool ambiguous = false;
        for (uint32_t sd = 0; sd < e->n_struct_defs; sd++) {
            StructDef *sdef = e->struct_defs[sd];
            for (uint32_t i = 0; i < sdef->n_fields; i++) {
                if (sdef->fields[i].kind == TY_FN &&
                    strlen(sdef->fields[i].name) == method_name_len &&
                    memcmp(sdef->fields[i].name, method_name, method_name_len) == 0) {
                    if (matched_def != NULL && matched_def != sdef) {
                        ambiguous = true;
                    } else {
                        matched_def = sdef;
                        matched_idx = i;
                    }
                }
            }
        }
        if (matched_def && !ambiguous) {
            Type field_type = type_from_kind(TY_FN);
            Expr *get_field = expr_new(e->arena, EX_GET_FIELD, field_type, call->span);
            get_field->as.get_field_.struct_expr = obj;
            get_field->as.get_field_.field_idx = matched_idx;
            get_field->as.get_field_.def = matched_def;

            uint32_t n_args = call->as.list.len - 2;
            Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
            for (uint32_t j = 0; j < n_args; j++) {
                args[j] = elab_form(e, call->as.list.items[2 + j]);
                if (!args[j]) return NULL;
            }

            Expr *call_out = expr_new(e->arena, EX_CALL, TYPE_INT, call->span);
            call_out->as.call_.fn_binding = NULL;
            call_out->as.call_.fn_expr = get_field;
            call_out->as.call_.args = args;
            call_out->as.call_.n_args = n_args;
            return call_out;
        }
    }

    /* IT4: Typeclass intersection dispatch on union types.
     * When obj : (A | B), and every member type has an instance for .method,
     * generate a tag-dispatched EX_MATCH that calls the right instance per arm.
     * The synthetic scrutinee is `obj`; each arm unboxes the value and calls the
     * per-member method implementation directly (bypassing dictionary dispatch
     * to avoid nested tur_tagged_t complications). */
    if (obj->type.kind == TY_UNION) {
        uint8_t n_members = obj->type.as.union_.n_members;
        /* Elaborate the extra arguments once (they are shared across arms). */
        uint32_t n_extra = call->as.list.len - 2;
        Expr **extra_args = (Expr **)arena_alloc(e->arena, n_extra * sizeof(Expr *));
        for (uint32_t i = 0; i < n_extra; i++) {
            extra_args[i] = elab_form(e, call->as.list.items[2 + i]);
            if (!extra_args[i]) return NULL;
        }

        /* For each member, find a matching typeclass instance. */
        FnDef **member_methods = (FnDef **)arena_alloc(e->arena, n_members * sizeof(FnDef *));
        for (uint8_t um = 0; um < n_members; um++) {
            Type *mem_t = obj->type.as.union_.members[um];
            if (!mem_t) { member_methods[um] = NULL; continue; }
            FnDef *found = NULL;
            for (TypeClassInstance *inst = e->typeclass_env.instances;
                 inst != NULL && !found; inst = inst->next) {
                for (uint8_t mi = 0; mi < inst->typeclass->n_methods; mi++) {
                    const TypeClassMethod *meth = &inst->typeclass->methods[mi];
                    if (meth->name->len != method_name_len ||
                        memcmp(meth->name->name, method_name, method_name_len) != 0) continue;
                    if (inst->n_type_args > 0 &&
                        inst->type_args[0].kind != mem_t->kind) continue;
                    found = inst->method_impls[mi];
                    break;
                }
            }
            member_methods[um] = found;
            if (!found) {
                diag_emit(DIAG_ERROR, call->span,
                          "typeclass method '%.*s' not available for union member '%s'",
                          (int)method_name_len, method_name, type_name(*mem_t));
                return NULL;
            }
        }

        /* Determine result type from the first member's method. */
        Type result_type = TYPE_NIL;
        if (n_members > 0 && member_methods[0]) {
            FnDef *m0 = member_methods[0];
            if (m0->binding->type.kind == TY_FN)
                result_type = type_from_kind(m0->binding->type.as.fn.result_kind);
            else if (m0->body)
                result_type = m0->body->type;
        }
        if (result_type.kind == TY_UNKNOWN) result_type = TYPE_INT;

        /* Build a fresh binding name for the unboxed arm variable. */
        static uint32_t union_dispatch_ctr = 0;
        char arm_name_buf[32];
        snprintf(arm_name_buf, sizeof(arm_name_buf), "__udisp_%u", union_dispatch_ctr++);
        const Symbol *arm_sym = intern_cstr(e->st, arm_name_buf);

        /* Build the arms array. */
        MatchArm *arms = (MatchArm *)arena_alloc(e->arena, n_members * sizeof(MatchArm));
        for (uint8_t um = 0; um < n_members; um++) {
            Type *mem_t = obj->type.as.union_.members[um];
            FnDef *meth = member_methods[um];

            /* Pattern: type-narrowing, binds arm_sym to the unboxed value. */
            MatchArm *arm = &arms[um];
            memset(arm, 0, sizeof(*arm));
            arm->pattern.is_var = true;
            arm->pattern.var_sym = arm_sym;
            arm->pattern.union_member_idx = (int)um;
            arm->pattern.n_bindings = 1;
            arm->pattern.bindings = (Binding **)arena_alloc(e->arena, sizeof(Binding *));
            Binding *var_b = binding_new(e, arm_sym, *mem_t, false, false, call->span);
            arm->pattern.bindings[0] = var_b;
            arm->pattern.var_binding = var_b;
            arm->guard = NULL;

            /* Body: call meth with (var_b, extra_args...) */
            Expr *var_expr = expr_new(e->arena, EX_VAR, *mem_t, call->span);
            var_expr->as.var.binding = var_b;

            uint32_t total_args = 1 + n_extra;
            Expr **call_args = (Expr **)arena_alloc(e->arena, total_args * sizeof(Expr *));
            call_args[0] = var_expr;
            for (uint32_t ei = 0; ei < n_extra; ei++) call_args[1 + ei] = extra_args[ei];

            Expr *body_call = expr_new(e->arena, EX_CALL, result_type, call->span);
            body_call->as.call_.fn_binding = meth->binding;
            body_call->as.call_.fn_expr = NULL;
            body_call->as.call_.args = call_args;
            body_call->as.call_.n_args = total_args;
            arm->body = body_call;
        }

        Expr *out = expr_new(e->arena, EX_MATCH, result_type, call->span);
        out->as.match_.scrutinee = obj;
        out->as.match_.arms = arms;
        out->as.match_.n_arms = n_members;
        return out;
    }

    /* Phase HKT H2: Type-based instance lookup.
     * Build a TypeClassDispatchKey from the obj type, then use
     * typeclass_env_lookup_instance_by_key for a two-level search.
     * Fall back to name-only search if the type-based lookup yields nothing
     * (e.g. TY_UNKNOWN during forward-reference elaboration). */
    FnDef *best_method = NULL;
    /* Phase H §1: Track the selected instance so we can build an EX_DICT node. */
    TypeClassInstance *best_inst = NULL;
    /* Phase D0: count fallback candidates and track whether an exact match was found. */
    int fallback_count = 0;
    bool exact_match_found = false;

    /* Determine the effective constructor kind from the obj type. */
    Kind obj_ck = KIND_STAR;
    {
        TypeKind tk = obj->type.kind;
        bool is_primitive = (tk == TY_INT  || tk == TY_BOOL  || tk == TY_CSTR ||
                             tk == TY_NIL  || tk == TY_FLOAT || tk == TY_PTR_VOID ||
                             tk == TY_UNKNOWN);
        obj_ck = is_primitive ? KIND_STAR : KIND_ARROW;
    }

    /* Search instances — prefer the one whose type_args[0] matches obj's type. */
    for (TypeClassInstance *inst = e->typeclass_env.instances; inst != NULL; inst = inst->next) {
        for (uint8_t i = 0; i < inst->typeclass->n_methods; i++) {
            const TypeClassMethod *method = &inst->typeclass->methods[i];
            if (method->name->len != method_name_len ||
                memcmp(method->name->name, method_name, method_name_len) != 0) {
                continue;
            }
            /* Name matched.  Now check if this instance's first type_arg
             * matches the obj type.  For KIND_STAR we compare TypeKind
             * exactly; for KIND_ARROW we accept any non-primitive. */
            if (inst->n_type_args > 0 && obj->type.kind != TY_UNKNOWN) {
                bool type_ok;
                if (obj_ck == KIND_STAR) {
                    type_ok = (inst->type_args[0].kind == obj->type.kind);
                } else {
                    /* KIND_ARROW: accept non-primitive instance type_args */
                    TypeKind itk = inst->type_args[0].kind;
                    bool inst_is_primitive =
                        (itk == TY_INT  || itk == TY_BOOL || itk == TY_CSTR ||
                         itk == TY_NIL  || itk == TY_FLOAT || itk == TY_PTR_VOID);
                    type_ok = !inst_is_primitive;
                }
                if (!type_ok) {
                    /* Record as fallback but keep searching. */
                    fallback_count++;
                    if (!best_method) { best_method = inst->method_impls[i]; best_inst = inst; }
                    continue;
                }
            }
            /* Phase PTC3: Check type parameter constraints on this instance.
             * If the instance has constraints, verify they are satisfied before
             * selecting it. For v1, we use the obj type as the only type argument.
             * Phase PTC4 will handle full parameterized type matching. */
            if (inst->type_param_constraints && inst->n_type_param_constraints > 0) {
                Type obj_type = obj->type;
                if (!typeclass_instance_constraints_satisfied(inst, &obj_type, 1, &e->typeclass_env)) {
                    /* Constraints not satisfied - skip this instance */
                    continue;
                }
            }
            /* Good match (or no type_args to check). */
            best_method = inst->method_impls[i];
            best_inst = inst;
            exact_match_found = true;
            goto found_method;
        }
    }
found_method:;
    
    if (!best_method) {
        /* No matching method found */
        diag_emit(DIAG_ERROR, call->span,
                  "no typeclass method found for '%.*s'",
                  method_name_len, method_name);
        return NULL;
    }

    /* Phase D0: Ambiguous dispatch diagnostic.
     * If we reached here via the fallback path (no exact type match) and
     * more than one instance matched by name, emit TUR_E0020 so the user
     * gets a clear error instead of a silent wrong-instance selection. */
    if (!exact_match_found && fallback_count > 1) {
        /* Build a comma-separated list of matching instance names for the message. */
        char inst_list[512];
        int pos = 0;
        int listed = 0;
        for (TypeClassInstance *ci = e->typeclass_env.instances;
             ci != NULL && pos < (int)sizeof(inst_list) - 2; ci = ci->next) {
            for (uint8_t mi = 0; mi < ci->typeclass->n_methods; mi++) {
                const TypeClassMethod *cm = &ci->typeclass->methods[mi];
                if (cm->name->len != method_name_len ||
                    memcmp(cm->name->name, method_name, method_name_len) != 0) continue;
                if (listed > 0 && pos < (int)sizeof(inst_list) - 3) {
                    inst_list[pos++] = ','; inst_list[pos++] = ' ';
                }
                int wrote = 0;
                if (ci->n_type_args > 0 && ci->type_arg_syms && ci->type_arg_syms[0]) {
                    wrote = snprintf(inst_list + pos, sizeof(inst_list) - (size_t)pos,
                                     "%s[%s]", ci->typeclass->name->name,
                                     ci->type_arg_syms[0]->name);
                } else {
                    wrote = snprintf(inst_list + pos, sizeof(inst_list) - (size_t)pos,
                                     "%s[?]", ci->typeclass->name->name);
                }
                if (wrote > 0) pos += wrote;
                listed++;
                break; /* one method match per instance is enough */
            }
        }
        inst_list[pos] = '\0';
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0020_AMBIGUOUS_DISPATCH,
                            "ambiguous method dispatch: '.%.*s' matches %d instances "
                            "(%s) -- receiver type is erased (int64_t). "
                            "Hint: annotate the receiver's type or use @TypeName syntax (see D1).",
                            (int)method_name_len, method_name, fallback_count, inst_list);
        return NULL;
    }

    /* obj was already elaborated above for dispatch; elaborate the remaining args. */
    uint32_t n_args = call->as.list.len - 2;
    Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[2 + i]);
        if (!args[i]) return NULL;
    }

    /* Phase HRT3/HRT4: For methods with rank-N (poly fn) parameters, wrap matching args
     * as EX_POLY_WRAP so they can be passed as tur_poly_fn_t.
     * params[0] is the receiver (obj), so method param i+1 matches arg i.
     * Phase HRT4: if the arg is already is_poly_fn, use pass-through (wrapper_binding=NULL). */
    bool has_poly_params = false;
    /* Check params[0] which corresponds to obj (the first/receiver argument). */
    if (best_method->n_params > 0 && best_method->params[0]->is_poly_fn) {
        has_poly_params = true;
        Binding *inner_b = poly_arg_fn_binding(obj);
        if (!inner_b) {
            diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                      "rank-N typeclass method argument must be a named function");
            return NULL;
        }
        Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, obj->span);
        wrap->as.poly_wrap_.inner = obj;
        if (inner_b->is_poly_fn) {
            wrap->as.poly_wrap_.wrapper_binding = NULL; /* HRT4: pass-through */
        } else {
            uint8_t inner_arity = (inner_b->type.kind == TY_FN)
                ? (uint8_t)inner_b->type.as.fn.arity : 1;
            Binding *wrapper_b = make_poly_wrapper(e, inner_b, inner_arity, obj->span);
            if (!wrapper_b) return NULL;
            wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
        }
        obj = wrap;
    }
    for (uint32_t i = 0; i < n_args; i++) {
        uint8_t param_idx = 1 + (uint8_t)i;  /* params[0] is the receiver */
        if (param_idx < best_method->n_params && best_method->params[param_idx]->is_poly_fn) {
            has_poly_params = true;
            Binding *inner_b = poly_arg_fn_binding(args[i]);
            if (!inner_b) {
                /* Phase CCL: no named-function binding found.  If the argument
                 * is a fat closure (TY_PTR_VOID — capturing or non-capturing
                 * lambda), wrap it for tur_poly_fn_t packing in the emitter. */
                if (args[i]->type.kind == TY_PTR_VOID) {
                    Expr *orig2 = args[i];
                    Expr *cwrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, orig2->span);
                    cwrap->as.poly_wrap_.inner = orig2;
                    cwrap->as.poly_wrap_.wrapper_binding = NULL;
                    cwrap->as.poly_wrap_.is_closure = true;
                    args[i] = cwrap;
                    continue;
                }
                diag_emit(DIAG_ERROR, call->as.list.items[2 + i]->span,
                          "rank-N typeclass method argument must be a named function or closure");
                return NULL;
            }
            Expr *orig = args[i];
            Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, orig->span);
            wrap->as.poly_wrap_.inner = orig;
            if (inner_b->is_poly_fn) {
                wrap->as.poly_wrap_.wrapper_binding = NULL; /* HRT4: pass-through */
            } else {
                uint8_t inner_arity = (inner_b->type.kind == TY_FN)
                    ? (uint8_t)inner_b->type.as.fn.arity : 1;
                Binding *wrapper_b = make_poly_wrapper(e, inner_b, inner_arity, args[i]->span);
                if (!wrapper_b) return NULL;
                wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
            }
            args[i] = wrap;
        }
    }

    /* Allocate arguments array with obj prepended */
    Expr **call_args = (Expr **)arena_alloc(e->arena, (n_args + 1) * sizeof(Expr *));
    call_args[0] = obj;
    for (uint32_t i = 0; i < n_args; i++) {
        call_args[i + 1] = args[i];
    }
    
    /* Create a call to the method function */
    /* The result type is the return type of the method.
     * For inline-C bodies the body type is TYPE_NIL, so prefer the
     * declared return type from the method's binding function type. */
    Type result_type;
    if (best_method->binding->type.kind == TY_FN) {
        result_type = type_from_kind(best_method->binding->type.as.fn.result_kind);
    } else {
        result_type = best_method->body->type;
        if (result_type.kind == TY_UNKNOWN || result_type.kind == TY_NIL) {
            result_type = TYPE_INT;
        }
    }
    
    /* Phase H §1 (dict load): Build an EX_DICT node that carries both the
     * singleton identity AND the method field name.  When fn_expr is this
     * node, emit.c dispatches through the dictionary struct at the call site:
     *   dict_<Class>_<type>_singleton.<method>(args...)
     * instead of a direct call to the impl function.  Fall back to the direct
     * binding if, for some reason, no instance was resolved. */
    Expr *dict_expr = NULL;
    if (best_inst) {
        dict_expr = make_dict_expr(e, best_inst, call->span);
        /* Copy the method name into the EX_DICT node and sanitize for C. */
        strncpy(dict_expr->as.dict_.method_name, method_name,
                sizeof(dict_expr->as.dict_.method_name) - 1);
        dict_expr->as.dict_.method_name[sizeof(dict_expr->as.dict_.method_name) - 1] = '\0';
        for (char *p = dict_expr->as.dict_.method_name; *p; p++) {
            if (!isalnum((unsigned char)*p) && *p != '_') *p = '_';
        }
    }

    Expr *out = expr_new(e->arena, EX_CALL, result_type, call->span);
    if (dict_expr && !has_poly_params) {
        /* Dictionary dispatch: indirect call through the vtable field. */
        out->as.call_.fn_binding = NULL;
        out->as.call_.fn_expr    = dict_expr;
    } else {
        /* Direct call: either no instance resolved, or method has rank-N (poly fn)
         * params that require tur_poly_fn_t calling convention — dictionary dispatch
         * doesn't support tur_poly_fn_t params, so bypass it. */
        out->as.call_.fn_binding = best_method->binding;
        out->as.call_.fn_expr    = NULL;
    }
    out->as.call_.args    = call_args;
    out->as.call_.n_args  = n_args + 1;
    out->as.call_.dict_arg = dict_expr;  /* annotation for downstream passes */
    return out;
}


/* Phase M6: Compute the mangled C name for an exported binding.
 * Mirrors the logic in emit.c:raw_name_for_binding.
 * Returns a malloc'd string that the caller must free. */
static char *elab_mangle_binding_name(const Binding *b) {
    if (b->c_export_name) return strdup(b->c_export_name);

    char mod_prefix[512];
    size_t mod_prefix_len = 0;
    bool is_main_binding = (b->name->len == 4 &&
                             memcmp(b->name->name, "main", 4) == 0);
    /* Phase M7: mirror emit.c — only globals get module-prefixed C names. */
    if (b->defining_module_name != NULL && !is_main_binding && b->is_global) {
        const char *mn = b->defining_module_name->name;
        size_t mn_len  = b->defining_module_name->len;
        size_t j = 0;
        for (size_t i = 0; i < mn_len && j < sizeof(mod_prefix) - 3; i++) {
            char c = mn[i];
            if (c == '/') { mod_prefix[j++] = '_'; mod_prefix[j++] = '_'; }
            else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '_') { mod_prefix[j++] = c; }
            else { mod_prefix[j++] = '_'; }
        }
        mod_prefix[j++] = '_';
        mod_prefix[j++] = '_';
        mod_prefix[j]   = '\0';
        mod_prefix_len  = j;
    }

    size_t total = mod_prefix_len + b->name->len + 1;
    char *p = (char *)malloc(total);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    size_t k = 0;
    if (mod_prefix_len > 0) { memcpy(p, mod_prefix, mod_prefix_len); k = mod_prefix_len; }
    for (uint32_t i = 0; i < b->name->len; i++) {
        char c = b->name->name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') { p[k++] = c; }
        else { p[k++] = '_'; }
    }
    p[k] = '\0';
    return p;
}

Expr *elaborate_program(Arena *arena, SymbolTable *st,
                        Form *const *forms, uint32_t nforms,
                        uint32_t stdlib_prefix,
                        const char *module_base_dir,
                        bool separate_compilation,
                        TypeClassEnv *out_tc_env,
                        const char **include_dirs,
                        int n_include_dirs) {
    Elab e;
    elab_init_state(&e, arena, st);
    e.module_base_dir = module_base_dir ? module_base_dir : ".";
    /* stdlib fallback: TUR_STDLIB_DIR env var, else "stdlib" */
    {
        const char *sdir = getenv("TUR_STDLIB_DIR");
        e.module_stdlib_dir = (sdir && *sdir) ? sdir : "stdlib";
    }
    e.module_include_dirs   = include_dirs;
    e.n_module_include_dirs = n_include_dirs;
    e.separate_compilation = separate_compilation;
    builtins_init(st);

    int rc = 0;

    /* Phase M: (load "path") preprocessing.
     * Recursively expand all top-level (load "path") forms in place by
     * parsing the referenced file and splicing its forms into the form list.
     * Tracks visited paths to prevent duplicate loads and detect cycles.
     * Loaded forms then participate in the normal two-pass elaboration. */
    {
        const Symbol **loaded = NULL;
        uint32_t n_loaded = 0, cap_loaded = 0;
        Form **work = (Form **)arena_alloc(arena, nforms * sizeof(Form *));
        for (uint32_t i = 0; i < nforms; i++) work[i] = forms[i];
        uint32_t n_work = nforms;
        uint32_t cap_work = nforms;
        bool changed = true;
        while (changed) {
            changed = false;
            uint32_t out_n = 0;
            uint32_t out_cap = n_work + 16;
            Form **out = (Form **)arena_alloc(arena, out_cap * sizeof(Form *));
            for (uint32_t i = 0; i < n_work; i++) {
                Form *f = work[i];
                bool is_load = false;
                const Form *path_f = NULL;
                if (f->tag == F_LIST && f->as.list.len == 2 &&
                    f->as.list.items[0]->tag == F_SYM &&
                    f->as.list.items[0]->as.sym == e.sym_load &&
                    f->as.list.items[1]->tag == F_STR) {
                    is_load = true;
                    path_f = f->as.list.items[1];
                }
                if (!is_load) {
                    if (out_n >= out_cap) {
                        out_cap *= 2;
                        Form **nout = (Form **)arena_alloc(arena, out_cap * sizeof(Form *));
                        for (uint32_t k = 0; k < out_n; k++) nout[k] = out[k];
                        out = nout;
                    }
                    out[out_n++] = f;
                    continue;
                }
                /* (load "path") — read & parse */
                uint32_t plen = path_f->as.s.len;
                if (plen == 0 || plen >= 4096) {
                    diag_emit(DIAG_ERROR, path_f->span,
                              "load: path must be non-empty and < 4096 chars");
                    rc = -1;
                    continue;
                }
                char path_buf[4096];
                memcpy(path_buf, path_f->as.s.p, plen);
                path_buf[plen] = '\0';
                const Symbol *key = intern_cstr(st, path_buf);
                /* Already loaded? Skip silently (idempotent). */
                bool already = false;
                for (uint32_t k = 0; k < n_loaded; k++) {
                    if (loaded[k] == key) { already = true; break; }
                }
                if (already) continue;
                if (n_loaded >= cap_loaded) {
                    cap_loaded = cap_loaded ? cap_loaded * 2 : 8;
                    loaded = (const Symbol **)realloc((void *)loaded,
                              cap_loaded * sizeof(const Symbol *));
                    if (!loaded) { fprintf(stderr, "tur: oom\n"); abort(); }
                }
                loaded[n_loaded++] = key;
                /* Read source */
                char *src_raw = NULL;
                size_t src_len = 0;
                if (elab_read_file(path_buf, &src_raw, &src_len) != 0) {
                    diag_emit(DIAG_ERROR, path_f->span,
                              "load: cannot open '%s'", path_buf);
                    rc = -1;
                    continue;
                }
                char *src_copy = (char *)arena_alloc(arena, src_len + 1);
                memcpy(src_copy, src_raw, src_len);
                src_copy[src_len] = '\0';
                free(src_raw);
                char *path_copy = (char *)arena_alloc(arena, plen + 1);
                memcpy(path_copy, path_buf, plen + 1);
                SourceFile *sfile = (SourceFile *)arena_alloc(arena, sizeof(SourceFile));
                *sfile = (SourceFile){0};
                sfile->path = path_copy;
                sfile->src = src_copy;
                sfile->len = src_len;
                sfile->file_id = e.next_import_file_id++;
                sfile->reader_type = reader_type_from_extension(path_buf);
                if (sfile->reader_type == READER_UNKNOWN) sfile->reader_type = READER_TURMERIC;
                diag_register_file(sfile);
                uint32_t lf_n = 0;
                Form **lf = read_all(arena, st, sfile, &lf_n);
                if (!lf) { rc = -1; continue; }
                /* Splice loaded forms into output list */
                if (out_n + lf_n > out_cap) {
                    while (out_n + lf_n > out_cap) out_cap *= 2;
                    Form **nout = (Form **)arena_alloc(arena, out_cap * sizeof(Form *));
                    for (uint32_t k = 0; k < out_n; k++) nout[k] = out[k];
                    out = nout;
                }
                for (uint32_t k = 0; k < lf_n; k++) out[out_n++] = lf[k];
                changed = true; /* a new load may appear in loaded forms */
            }
            work = out;
            n_work = out_n;
            cap_work = out_cap;
            (void)cap_work;
        }
        free((void *)loaded);
        /* Replace forms/nforms with the expanded list for the rest of
         * elaborate_program.  Cast away const since we're in our own copy. */
        forms = (Form *const *)work;
        nforms = n_work;
    }

    Expr **items = (nforms == 0) ? NULL :
        (Expr **)arena_alloc(arena, nforms * sizeof(Expr *));

    /* Phase RF0: Type pre-pass -- pre-register all top-level defstruct and defdata
     * names as forward stubs BEFORE any elaboration, so that mutually recursive type
     * definitions can reference each other regardless of declaration order. */
    for (uint32_t i = 0; i < nforms; i++) {
        Form *f = forms[i];
        if (f->tag != F_LIST || f->as.list.len < 2) continue;
        Form *head = f->as.list.items[0];
        if (!head || head->tag != F_SYM) continue;
        bool is_defstruct = (head->as.sym == e.sym_defstruct);
        bool is_defdata   = (head->as.sym == e.sym_defdata);
        bool is_defgadt   = (head->as.sym == e.sym_defgadt);
        if (!is_defstruct && !is_defdata && !is_defgadt) continue;
        /* GADTs are only registered if -Xgadt is enabled */
        if (is_defgadt && !g_gadt_enabled) continue;
        Form *name_f = f->as.list.items[1];
        if (!name_f || name_f->tag != F_SYM) continue;
        const Symbol *type_name = name_f->as.sym;
        /* Skip if already in scope (e.g. stdlib defines a type with this name) */
        if (scope_lookup(&e.global, type_name)) continue;
        /* Pre-allocate a stub def and register a forward binding */
        elab_add_forward_type(&e, type_name);
        if (is_defstruct) {
            StructDef *stub = (StructDef *)arena_alloc(arena, sizeof(StructDef));
            stub->name = type_name->name;
            stub->n_fields = 0;
            stub->fields = NULL;
            stub->is_copy = false;
            stub->needs_drop_glue = false;
            stub->origin_file_id = name_f->span.file_id;
            elab_register_struct_def(&e, stub);
            Type t = type_struct(stub);
            Binding *b = binding_new(&e, type_name, t, false, true, name_f->span);
            scope_add(&e.global, b);
        } else if (is_defgadt) {
            AdtDef *stub = (AdtDef *)arena_alloc(arena, sizeof(AdtDef));
            stub->name = type_name->name;
            stub->n_ctors = 0;
            stub->ctors = NULL;
            stub->is_copy = false;
            stub->needs_drop_glue = false;
            stub->is_gadt = true;
            stub->type_params = NULL;
            stub->n_type_params = 0;
            elab_register_adt_def(&e, stub);
            Type t = type_adt(stub);
            Binding *b = binding_new(&e, type_name, t, false, true, name_f->span);
            scope_add(&e.global, b);
        } else {
            AdtDef *stub = (AdtDef *)arena_alloc(arena, sizeof(AdtDef));
            stub->name = type_name->name;
            stub->n_ctors = 0;
            stub->ctors = NULL;
            stub->is_copy = false;
            stub->needs_drop_glue = false;
            stub->is_gadt = false;
            stub->type_params = NULL;
            stub->n_type_params = 0;
            elab_register_adt_def(&e, stub);
            Type t = type_adt(stub);
            Binding *b = binding_new(&e, type_name, t, false, true, name_f->span);
            scope_add(&e.global, b);
        }
    }

    /* Phase 2: Two-pass elaboration for mutual recursion support.
     * Pass 1: Collect all top-level defn declarations and add them to scope.
     * This allows mutually recursive functions to see each other. */
    for (uint32_t i = 0; i < nforms; i++) {
        Form *f = forms[i];
        if (f->tag == F_LIST && f->as.list.len > 0) {
            Form *head = f->as.list.items[0];
            if (head->tag == F_SYM) {
                if (head->as.sym == e.sym_defn) {
                    /* Parse defn declaration without body */
                    if (f->as.list.len >= 3) {
                        /* Phase R5: skip optional #[no-unwind] attribute */
                        uint32_t name_idx = 1;
                        if (f->as.list.items[1]->tag == F_SYM &&
                            f->as.list.items[1]->as.sym == e.sym_no_unwind_attr) {
                            name_idx = 2;
                        }
                        /* Phase M6: skip optional (export-as "c_name") attribute */
                        if ((uint32_t)f->as.list.len > name_idx &&
                            f->as.list.items[name_idx]->tag == F_LIST &&
                            f->as.list.items[name_idx]->as.list.len == 2 &&
                            f->as.list.items[name_idx]->as.list.items[0]->tag == F_SYM &&
                            f->as.list.items[name_idx]->as.list.items[0]->as.sym == e.sym_export_as_attr) {
                            name_idx += 1; /* skip (export-as "c_name") */
                        }
                        if ((uint32_t)f->as.list.len <= name_idx) goto next_form;
                        Form *name_f = f->as.list.items[name_idx];
                        if (name_f->tag == F_SYM) {
                            /* Parse return type annotation if present */
                            TypeKind return_kind = TY_INT; /* default */
                            uint32_t ret_idx = name_idx + 2; /* name params :ret */
                            if (f->as.list.len > ret_idx) {
                                Form *ret_f = f->as.list.items[ret_idx];
                                if (ret_f->tag == F_KEYWORD) {
                                    const Symbol *kw = ret_f->as.sym;
                                    if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                                        return_kind = TY_INT;
                                    } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                                        return_kind = TY_BOOL;
                                    } else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) {
                                        return_kind = TY_NIL;
                                    } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                                        return_kind = TY_CSTR;
                                    } else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) {
                                        return_kind = TY_PTR_VOID;
                                    }
                                }
                            }
                            /* Create a forward function type with 1 int param and parsed return type */
                            TypeKind arg_kinds[MAX_FN_ARITY] = {TY_INT};
                            Type fn_type = type_fn(arg_kinds, 1, return_kind);
                            Binding *b = binding_new(&e, name_f->as.sym, fn_type, false, true, f->span);
                            scope_add(&e.global, b);
                        }
                    }
                    next_form:;
                }
            }
        }
    }

    /* Phase M0: Validate defmodule position — must be the first user form */
    for (uint32_t i = stdlib_prefix; i < nforms; i++) {
        Form *f = forms[i];
        if (f->tag == F_LIST && f->as.list.len > 0) {
            Form *head = f->as.list.items[0];
            if (head->tag == F_SYM && head->as.sym == e.sym_defmodule) {
                if (i != stdlib_prefix) {
                    diag_emit(DIAG_ERROR, head->span,
                              "defmodule must be the first form in the file");
                    diag_emit(DIAG_NOTE, forms[stdlib_prefix]->span,
                              "this form comes before defmodule; move it inside the defmodule body or below it");
                    rc = -1;
                }
                break; /* only check the first occurrence */
            }
        }
    }
    if (rc != 0) {
        scope_free(&e.global);
        free(e.struct_defs);
        free(e.adt_defs);
        free(e.handled_effect_names);
        free(e.macros);
        return NULL;
    }

    /* Pass 2: Elaborate all forms */
    for (uint32_t i = 0; i < nforms; i++) {
        items[i] = elab_form(&e, forms[i]);
        if (!items[i]) { rc = -1; /* keep going to surface more diagnostics */ }

        /* Phase M7: Each auto-loaded stdlib file is conceptually its own file,
         * so reset has_defmodule after each stdlib defmodule. Otherwise the
         * second stdlib (safe.tur after macros.tur) trips the "one defmodule
         * per file" check. */
        if (i < stdlib_prefix && items[i] && items[i]->kind == EX_DEFMODULE) {
            e.has_defmodule = false;
        }

        /* Phase M7: Promote auto-loaded stdlib module exports back to
         * "stdlib pre-module" status (defining_module_name = NULL) so they
         * remain globally visible from user code without explicit import.
         * Triggered after the last stdlib form has been processed. */
        if (i + 1 == stdlib_prefix && stdlib_prefix > 0) {
            for (uint32_t k = 0; k < e.global.n; k++) {
                Binding *gb = e.global.bindings[k];
                if (gb->defining_module_name != NULL &&
                    gb->defining_module_name->len >= 4 &&
                    memcmp(gb->defining_module_name->name, "tur/", 4) == 0) {
                    gb->defining_module_name = NULL;
                }
            }
            for (uint32_t k = 0; k < e.n_macros; k++) {
                MacroDef *m = e.macros[k];
                if (m->defining_module_name != NULL &&
                    m->defining_module_name->len >= 4 &&
                    memcmp(m->defining_module_name->name, "tur/", 4) == 0) {
                    m->defining_module_name = NULL;
                }
            }
        }
    }

    /* Phase 3: Prepend file-scope definitions (from nested fn) */
    if (e.n_file_scope_defs > 0) {
        /* Allocate new items array with room for file-scope defs */
        Expr **new_items = (Expr **)arena_alloc(arena, 
            (nforms + e.n_file_scope_defs) * sizeof(Expr *));
        /* Copy file-scope defs first */
        for (uint32_t i = 0; i < e.n_file_scope_defs; i++) {
            new_items[i] = e.file_scope_defs[i];
        }
        /* Copy original items */
        for (uint32_t i = 0; i < nforms; i++) {
            new_items[e.n_file_scope_defs + i] = items[i];
        }
        items = new_items;
        nforms += e.n_file_scope_defs;
        /* Free the malloc'd file_scope_defs array */
        free(e.file_scope_defs);
    }

    /* Phase M6: Check for C symbol name collisions among exported bindings.
     * Two exported bindings from different modules collide when their mangled
     * C names are identical (e.g. module "my-lib" and "my_lib" both exporting
     * "foo" would both produce "my_lib__foo"). */
    if (rc == 0) {
        uint32_t n_exp = 0;
        for (uint32_t i = 0; i < e.global.n; i++) {
            if (e.global.bindings[i]->is_exported &&
                e.global.bindings[i]->defining_module_name != NULL)
                n_exp++;
        }
        if (n_exp > 1) {
            char **mangled = (char **)malloc(n_exp * sizeof(char *));
            Binding **exp_bindings = (Binding **)malloc(n_exp * sizeof(Binding *));
            if (!mangled || !exp_bindings) { fprintf(stderr, "tur: oom\n"); abort(); }
            uint32_t idx = 0;
            for (uint32_t i = 0; i < e.global.n; i++) {
                Binding *b = e.global.bindings[i];
                if (b->is_exported && b->defining_module_name != NULL) {
                    mangled[idx] = elab_mangle_binding_name(b);
                    exp_bindings[idx] = b;
                    idx++;
                }
            }
            for (uint32_t i = 0; i < n_exp; i++) {
                for (uint32_t j = i + 1; j < n_exp; j++) {
                    if (exp_bindings[i] == exp_bindings[j]) continue;
                    if (strcmp(mangled[i], mangled[j]) == 0) {
                        diag_emit(DIAG_ERROR, exp_bindings[j]->span,
                                  "exported symbol '%s' from module '%s' mangles to "
                                  "the same C name '%s' as '%s' from module '%s'; "
                                  "rename one or use (export-as \"...\") to assign a unique C name",
                                  exp_bindings[j]->name->name,
                                  exp_bindings[j]->defining_module_name->name,
                                  mangled[j],
                                  exp_bindings[i]->name->name,
                                  exp_bindings[i]->defining_module_name->name);
                        rc = -1;
                    }
                }
            }
            for (uint32_t i = 0; i < n_exp; i++) free(mangled[i]);
            free(mangled);
            free(exp_bindings);
        }
    }

    scope_free(&e.global);
    free(e.struct_defs);
    free(e.adt_defs);
    free(e.handled_effect_names);
    free(e.macros);
    free(e.loaded_modules); /* Phase M2 */
    if (rc != 0) return NULL;

    Expr *prog = expr_new(arena, EX_PROGRAM, TYPE_NIL,
                          nforms > 0 ? forms[0]->span : (Span){0,0,0,0,0,0});
    prog->as.program.items = items;
    prog->as.program.n = nforms;
    /* CPS-CL10: expose typeclass env to callers (e.g. cps_transform) */
    if (out_tc_env) *out_tc_env = e.typeclass_env;
    return prog;
}

/* ============================================================================
 * Phase 20: Software Transactional Memory - Elaboration
 * ============================================================================ */

/* Track whether we're inside an stm block for TUR-E0009 checking */
static bool elab_in_stm = false;

static Expr *elab_stm(Elab *e, const Form *call) {
    if (call->as.list.len < 1) {
        diag_emit(DIAG_ERROR, call->span, "stm requires at least one body expression");
        return NULL;
    }

    /* Save previous stm state */
    bool prev_in_stm = elab_in_stm;
    elab_in_stm = true;

    /* Elaborate body expressions */
    Expr **body = arena_alloc(e->arena, (call->as.list.len - 1) * sizeof(Expr *));
    uint32_t n_body = 0;
    for (uint32_t i = 1; i < call->as.list.len; i++) {
        Expr *expr = elab_form(e, call->as.list.items[i]);
        if (!expr) {
            elab_in_stm = prev_in_stm;
            return NULL;
        }
        body[n_body++] = expr;
    }

    /* Create STM block expression */
    /* The type is the type of the last expression, or NIL if empty */
    Type result_type = TYPE_NIL;
    if (n_body > 0) {
        result_type = body[n_body - 1]->type;
    }
    Expr *result = expr_new(e->arena, EX_STM, result_type, call->span);
    result->as.stm_.body = body;
    result->as.stm_.n_body = n_body;

    /* Restore stm state */
    elab_in_stm = prev_in_stm;

    return result;
}

static Expr *elab_atomically(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span, "atomically requires exactly one stm block argument");
        return NULL;
    }

    Form *arg = call->as.list.items[1];

    /* The argument should be an stm block or something that evaluates to one */
    /* For v1, we just wrap it in atomically */
    Expr *stm_expr = elab_form(e, arg);
    if (!stm_expr) return NULL;

    /* Check if it's already an stm block */
    if (stm_expr->kind != EX_STM) {
        diag_emit(DIAG_ERROR, call->span, "atomically requires an stm block as argument");
        return NULL;
    }

    Expr *result = expr_new(e->arena, EX_ATOMICALLY, stm_expr->type, call->span);
    result->as.atomically_.stm_expr = stm_expr;
    return result;
}

static Expr *elab_retry(Elab *e, const Form *call) {
    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "retry can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *result = expr_new(e->arena, EX_RETRY, TYPE_NIL, call->span);
    return result;
}

static Expr *elab_check(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span, "check requires exactly one condition argument");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "check can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *cond = elab_form(e, call->as.list.items[1]);
    if (!cond) return NULL;

    /* Check should return bool */
    if (cond->type.kind != TY_BOOL) {
        diag_emit(DIAG_ERROR, call->span, "check condition must be a bool");
        return NULL;
    }

    Expr *result = expr_new(e->arena, EX_CHECK, TYPE_NIL, call->span);
    result->as.check_.cond = cond;
    return result;
}

static Expr *elab_or_else(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "or-else requires exactly two stm block arguments");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "or-else can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *stm1 = elab_form(e, call->as.list.items[1]);
    Expr *stm2 = elab_form(e, call->as.list.items[2]);
    if (!stm1 || !stm2) return NULL;

    if (stm1->kind != EX_STM) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span, "or-else first argument must be an stm block");
        return NULL;
    }
    if (stm2->kind != EX_STM) {
        diag_emit(DIAG_ERROR, call->as.list.items[2]->span, "or-else second argument must be an stm block");
        return NULL;
    }

    /* or-else result type is the common type of both branches */
    /* For simplicity, use stm2's type (the fallback branch) */
    Type result_type = stm2->type;
    /* If stm2 returns nil, try stm1's type */
    if (result_type.kind == TY_NIL) {
        result_type = stm1->type;
    }

    Expr *result = expr_new(e->arena, EX_OR_ELSE, result_type, call->span);
    result->as.or_else_.stm1 = stm1;
    result->as.or_else_.stm2 = stm2;
    return result;
}

static Expr *elab_tvar_new(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span, "TVar/new requires exactly one initial value argument");
        return NULL;
    }

    Expr *init = elab_form(e, call->as.list.items[1]);
    if (!init) return NULL;

    /* TVar::new returns a TVar pointer (ptr), not the type of the initial value */
    Expr *result = expr_new(e->arena, EX_TVAR_NEW, TYPE_PTR_VOID, call->span);
    result->as.tvar_new_.init = init;
    return result;
}

static Expr *elab_tvar_read(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span, "TVar/read requires exactly one TVar argument");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "TVar/read can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *tvar = elab_form(e, call->as.list.items[1]);
    if (!tvar) return NULL;

    /* TVar/read returns the value stored in the TVar, which is a ptr<void> */
    Expr *result = expr_new(e->arena, EX_TVAR_READ, TYPE_PTR_VOID, call->span);
    result->as.tvar_read_.tvar = tvar;
    return result;
}

static Expr *elab_tvar_write(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "TVar/write requires exactly two arguments: tvar and value");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "TVar/write can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *tvar = elab_form(e, call->as.list.items[1]);
    Expr *value = elab_form(e, call->as.list.items[2]);
    if (!tvar || !value) return NULL;

    Expr *result = expr_new(e->arena, EX_TVAR_WRITE, TYPE_NIL, call->span);
    result->as.tvar_write_.tvar = tvar;
    result->as.tvar_write_.value = value;
    return result;
}

static Expr *elab_tvar_modify(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "TVar/modify requires exactly two arguments: tvar and function");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "TVar/modify can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *tvar = elab_form(e, call->as.list.items[1]);
    Expr *fn = elab_form(e, call->as.list.items[2]);
    if (!tvar || !fn) return NULL;

    /* TVar/modify returns the old value, which is a ptr<void> */
    Expr *result = expr_new(e->arena, EX_TVAR_MODIFY, TYPE_PTR_VOID, call->span);
    result->as.tvar_modify_.tvar = tvar;
    result->as.tvar_modify_.fn = fn;
    return result;
}

static Expr *elab_tvar_swap(Elab *e, const Form *call) {
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "TVar/swap requires exactly two arguments: tvar and new value");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "TVar/swap can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *tvar = elab_form(e, call->as.list.items[1]);
    Expr *new_val = elab_form(e, call->as.list.items[2]);
    if (!tvar || !new_val) return NULL;

    /* TVar/swap returns the old value, which is a ptr<void> */
    Expr *result = expr_new(e->arena, EX_TVAR_SWAP, TYPE_PTR_VOID, call->span);
    result->as.tvar_swap_.tvar = tvar;
    result->as.tvar_swap_.new_val = new_val;
    return result;
}

static Expr *elab_tvar_cas(Elab *e, const Form *call) {
    if (call->as.list.len != 4) {
        diag_emit(DIAG_ERROR, call->span, "TVar/cas requires exactly three arguments: tvar, old value, new value");
        return NULL;
    }

    if (!elab_in_stm) {
        diag_emit(DIAG_ERROR, call->span, "TVar/cas can only be used inside an stm block (TUR-E0009)");
        return NULL;
    }

    Expr *tvar = elab_form(e, call->as.list.items[1]);
    Expr *old_val = elab_form(e, call->as.list.items[2]);
    Expr *new_val = elab_form(e, call->as.list.items[3]);
    if (!tvar || !old_val || !new_val) return NULL;

    Expr *result = expr_new(e->arena, EX_TVAR_CAS, TYPE_BOOL, call->span);
    result->as.tvar_cas_.tvar = tvar;
    result->as.tvar_cas_.old_val = old_val;
    result->as.tvar_cas_.new_val = new_val;
    return result;
}
