#include "elab.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtins.h"
#include "diag.h"
#include "typeclass.h"  /* Phase 15 */
#include "types.h"
#include "effect.h"    /* Phase 19 */

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

/* Helper to convert a type name string to TypeKind (Phase 19). */
static TypeKind typekind_from_symbol(const char *name) {
    if (strcmp(name, "int") == 0) return TY_INT;
    if (strcmp(name, "bool") == 0) return TY_BOOL;
    if (strcmp(name, "float") == 0) return TY_FLOAT;
    if (strcmp(name, "cstr") == 0) return TY_CSTR;
    if (strcmp(name, "nil") == 0) return TY_NIL;
    if (strcmp(name, "ptr-void") == 0) return TY_PTR_VOID;
    if (strcmp(name, "ref") == 0) return TY_REF;
    if (strcmp(name, "rc") == 0) return TY_RC;
    if (strcmp(name, "weak") == 0) return TY_WEAK;
    if (strcmp(name, "exception") == 0) return TY_EXCEPTION;
    if (strcmp(name, "cont") == 0) return TY_CONT;
    return TY_UNKNOWN;
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
            default:
                break;
        }
    }
    free(stack);
    *n_out = n;
    return result;
}

/* ---- elaborator state ---- */

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
    const Symbol *sym_when;
    const Symbol *sym_unless;
    const Symbol *sym_cond;
    const Symbol *sym_set;
    const Symbol *sym_while;
    const Symbol *sym_defn;     /* Phase 2 */
    const Symbol *sym_fn;       /* Phase 2 */
    const Symbol *sym_extern_c; /* Phase 2 */
    const Symbol *sym_caret_mut;   /* ^mut */
    const Symbol *sym_defer;      /* Phase 4 */
    const Symbol *sym_return;     /* return - early return with defer firing */
    /* Phase 5 */
    const Symbol *sym_ref;        /* ref */
    const Symbol *sym_deref;      /* @ (deref operator) - stored as symbol for parsing */
    const Symbol *sym_drop;       /* drop! - explicit drop for ref<T> */
    const Symbol *kw_else;         /* :else (the symbol named "else") */
    const Symbol *kw_derive;       /* :as (the symbol named "as") - for inline-C */
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
    /* Phase 17: Exceptions */
    const Symbol *sym_throw;      /* throw */
    const Symbol *sym_throw_bang; /* throw! (sugar for throw) */
    const Symbol *sym_try;        /* try */
    const Symbol *sym_catch;      /* catch */
    const Symbol *sym_finally;    /* finally */
    /* Phase 18: Delimited continuations */
    const Symbol *sym_reset;      /* reset */
    const Symbol *sym_shift;      /* shift */
    const Symbol *sym_shift0;     /* shift0 */
    const Symbol *sym_call_cc;    /* call/cc (sugar: (reset (shift k (f k)))) */
    const Symbol *sym_escape;     /* escape (sugar: (reset (shift k (f (fn [_] k))))) */
    /* Phase 19: Algebraic effects */
    const Symbol *sym_defeffect;  /* defeffect */
    const Symbol *sym_perform;    /* perform */
    const Symbol *sym_handle;     /* handle */
    const Symbol *sym_try_with;   /* try-with (sugar for handle) */
    const Symbol *sym_resume;     /* resume */
    const Symbol *sym_discontinue;/* discontinue */
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
    /* Phase 12: Borrow traits */
    const Symbol *sym_borrow;      /* & symbol for immutable borrow */
    const Symbol *sym_borrow_mut;  /* &mut for mutable borrow */
    /* Phase 15: Typeclasses */
    const Symbol *sym_defclass;    /* defclass */
    const Symbol *sym_definstance; /* definstance */
    /* Phase HKT (v2): reserved typeclass names — user definitions rejected with diagnostic */
    const Symbol *sym_hkt_Functor;
    const Symbol *sym_hkt_Applicative;
    const Symbol *sym_hkt_Monad;
    const Symbol *sym_hkt_Traversable;
    const Symbol *sym_hkt_Foldable;
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
    /* Phase 11: Struct registry - maps struct names to StructDef */
    StructDef **struct_defs;
    uint32_t n_struct_defs;
    uint32_t cap_struct_defs;
} Elab;

/* Phase 6: Macro definition */
typedef struct MacroDef {
    const Symbol *name;
    Form **params;
    uint32_t n_params;
    Form *body;
    Span span;
} MacroDef;

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
            case EX_THROW:
                stack[sp++] = cur->as.throw_.payload;
                break;
            case EX_TRY:
                stack[sp++] = cur->as.try_.body;
                for (uint8_t i = 0; i < cur->as.try_.n_clauses; i++) {
                    if (cur->as.try_.clauses[i].handler) {
                        stack[sp++] = cur->as.try_.clauses[i].handler;
                    }
                }
                if (cur->as.try_.finally_body) {
                    stack[sp++] = cur->as.try_.finally_body;
                }
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
    typeclass_env_init(&e->typeclass_env);
    /* Phase 19: Effect environment */
    e->effect_env = effect_env_new(arena);

    e->sym_def       = intern_cstr(st, "def");
    e->sym_let       = intern_cstr(st, "let");
    e->sym_if        = intern_cstr(st, "if");
    e->sym_do        = intern_cstr(st, "do");
    e->sym_when      = intern_cstr(st, "when");
    e->sym_unless    = intern_cstr(st, "unless");
    e->sym_cond      = intern_cstr(st, "cond");
    e->sym_cond      = intern_cstr(st, "cond");
    e->sym_set       = intern_cstr(st, "set!");
    e->sym_while     = intern_cstr(st, "while");
    e->sym_defn      = intern_cstr(st, "defn");
    e->sym_fn        = intern_cstr(st, "fn");
    e->sym_extern_c  = intern_cstr(st, "extern-c");
    e->sym_caret_mut = intern_cstr(st, "^mut");
    e->sym_defer     = intern_cstr(st, "defer");
    e->sym_return    = intern_cstr(st, "return");
    /* Phase 5 */
    e->sym_ref       = intern_cstr(st, "ref");
    e->sym_deref     = intern_cstr(st, "deref");
    e->sym_drop      = intern_cstr(st, "drop!");
    e->kw_else       = intern_cstr(st, "else");
    e->kw_derive     = intern_cstr(st, "as");
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
    /* Phase 17: Exceptions */
    e->sym_throw = intern_cstr(st, "throw");
    e->sym_throw_bang = intern_cstr(st, "throw!");
    e->sym_try = intern_cstr(st, "try");
    e->sym_catch = intern_cstr(st, "catch");
    e->sym_finally = intern_cstr(st, "finally");
    /* Phase 18: Delimited continuations */
    e->sym_reset = intern_cstr(st, "reset");
    e->sym_shift = intern_cstr(st, "shift");
    e->sym_shift0 = intern_cstr(st, "shift0");
    e->sym_call_cc = intern_cstr(st, "call/cc");
    e->sym_escape = intern_cstr(st, "escape");
    /* Phase 19: Algebraic effects */
    e->sym_defeffect = intern_cstr(st, "defeffect");
    e->sym_perform = intern_cstr(st, "perform");
    e->sym_handle = intern_cstr(st, "handle");
    e->sym_try_with = intern_cstr(st, "try-with");
    e->sym_resume = intern_cstr(st, "resume");
    e->sym_discontinue = intern_cstr(st, "discontinue");
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
    /* Phase 11: struct registry */
    e->struct_defs = NULL;
    e->n_struct_defs = 0;
    e->cap_struct_defs = 0;
    /* Phase 12: Borrow traits */
    e->sym_borrow = intern_cstr(st, "&");
    e->sym_borrow_mut = intern_cstr(st, "&mut");
    /* Phase 15: Typeclasses */
    e->sym_defclass = intern_cstr(st, "defclass");
    e->sym_definstance = intern_cstr(st, "definstance");
    /* Phase HKT (v2): reserved typeclass names */
    e->sym_hkt_Functor      = intern_cstr(st, "Functor");
    e->sym_hkt_Applicative  = intern_cstr(st, "Applicative");
    e->sym_hkt_Monad        = intern_cstr(st, "Monad");
    e->sym_hkt_Traversable  = intern_cstr(st, "Traversable");
    e->sym_hkt_Foldable     = intern_cstr(st, "Foldable");
    /* Macro storage */
    e->macros = NULL;
    e->n_macros = 0;
    e->cap_macros = 0;
}

/* Phase 13: Lifetime annotation helpers (deferred - infrastructure in place) */
/* Phase 15: Typeclass cached symbols */

/* Phase 6: Macro lookup */
static MacroDef *elab_lookup_macro(Elab *e, const Symbol *name) {
    for (uint32_t i = 0; i < e->n_macros; i++) {
        if (e->macros[i]->name == name) {
            return e->macros[i];
        }
    }
    return NULL;
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
            /* Vectors/maps inside quasiquote: process each element */
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
                if (f->tag == F_MAP) {
                    return form_map(e->arena, f->span, new_items, f->as.list.len);
                }
                return form_vec(e->arena, f->span, new_items, f->as.list.len);
            }
        case F_CBLOCK:
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
            /* Literals and quote forms are returned as-is */
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
            Form **items = (Form **)arena_alloc(e->arena, f->as.list.len * sizeof(Form *));
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                items[i] = substitute_params(e, f->as.list.items[i], macro, args);
            }
            return form_list(e->arena, f->span, items, f->as.list.len);
        }
        case F_SYM: {
            /* Check if this symbol is a parameter - if so, replace with corresponding arg */
            for (uint32_t i = 0; i < macro->n_params; i++) {
                Form *param = macro->params[i];
                if (param->tag == F_SYM && param->as.sym == f->as.sym) {
                    return args[i];
                }
            }
            /* Not a parameter - return as-is */
            return f;
        }
        case F_VEC:
        case F_MAP: {
            /* Recursively substitute in vector/map items */
            Form **items = (Form **)arena_alloc(e->arena, f->as.list.len * sizeof(Form *));
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                items[i] = substitute_params(e, f->as.list.items[i], macro, args);
            }
            if (f->tag == F_MAP) {
                return form_map(e->arena, f->span, items, f->as.list.len);
            }
            return form_vec(e->arena, f->span, items, f->as.list.len);
        }
    }
    return f;
}

/* Phase 6: Expand a macro call with arguments */
static Form *elab_expand_macro(Elab *e, MacroDef *macro, Form **args, uint32_t n_args) {
    /* Check arity */
    if (n_args != macro->n_params) {
        diag_emit(DIAG_ERROR, macro->span,
                  "macro '%s' expects %u arguments, got %u",
                  macro->name->name, macro->n_params, n_args);
        return NULL;
    }
    
    /* Substitute parameters in the macro body */
    return substitute_params(e, macro->body, macro, args);
}

static Binding *binding_new(Elab *e, const Symbol *name, Type type,
                            bool is_mut, bool is_global, Span span) {
    Binding *b = (Binding *)arena_alloc(e->arena, sizeof(Binding));
    b->name = name;
    b->type = type;
    b->is_mut = is_mut;
    b->is_global = is_global;
    b->id = e->next_id++;
    b->span = span;
    b->closure_fn_binding = NULL;
    b->is_moved = false;  /* Phase 5: move semantics */
    b->moved_at = SPAN_UNKNOWN;
    return b;
}

/* Forward declarations. */
static Expr *elab_form(Elab *e, Form *f);
/* Phase 5 */
static Expr *elab_ref(Elab *e, const Form *call);
static Expr *elab_deref(Elab *e, const Form *call);
static Expr *elab_drop(Elab *e, const Form *call);
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

/* ---- helpers ---- */

static int form_is_symbol_named(const Form *f, const Symbol *name) {
    return f && f->tag == F_SYM && f->as.sym == name;
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

    /* Parse bindings: walk left-to-right, optional ^mut prefix per entry. */
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
        if (form_is_symbol_named(cur, e->sym_caret_mut)) {
            is_mut = true;
            i++;
            if (i >= bindings_form->as.list.len) {
                diag_emit(DIAG_ERROR, cur->span, "trailing ^mut with no binding name");
                rc = -1; break;
            }
            cur = bindings_form->as.list.items[i];
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

        Binding *b = binding_new(e, name, init->type, is_mut, false, name_span);
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
        
        binds[n_binds].binding = b;
        binds[n_binds].init = init;
        binding_moved_during_init[n_binds] = false; /* new binding, not yet moved during init */
        n_binds++;
    }

    /* Phase 5: Check if any binding is a ref and needs auto-defer drop */
    bool has_ref_bindings = false;
    for (uint32_t k = 0; k < n_binds; k++) {
        /* Skip refs that come from ref/from-rc - they don't own the data */
        if (binds[k].binding->type.kind == TY_REF &&
            binds[k].init->kind != EX_REF_FROM_RC) {
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
                if (binds[k].binding->type.kind == TY_REF &&
                    binds[k].init->kind != EX_REF_FROM_RC &&
                    !binding_moved_during_init[k] &&
                    !binds[k].binding->is_moved) {
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
                    if (binds[k].binding->type.kind == TY_REF &&
                        binds[k].init->kind != EX_REF_FROM_RC &&
                        !binding_moved_during_init[k] &&
                        !binds[k].binding->is_moved) {
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
    Expr *out = expr_new(e->arena, EX_DO, items[n - 1]->type, call->span);
    out->as.do_.items = items;
    out->as.do_.n = n;
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

    Expr *then_ = elab_form(e, call->as.list.items[2]);
    if (!then_) {
        free(move_bindings);
        free(before_states);
        return NULL;
    }
    bool *then_states = move_state_capture_current(move_bindings, n_move_bindings);

    /* Rewind to pre-branch move-state before elaborating else branch. */
    move_state_restore(move_bindings, before_states, n_move_bindings);

    Expr *else_ = NULL;
    Type result_t = TYPE_NIL;
    if (call->as.list.len == 4) {
        else_ = elab_form(e, call->as.list.items[3]);
        if (!else_) {
            move_state_restore(move_bindings, before_states, n_move_bindings);
            free(then_states);
            free(move_bindings);
            free(before_states);
            return NULL;
        }
        bool *else_states = move_state_capture_current(move_bindings, n_move_bindings);

        /* A move is guaranteed after if/else only if it was present before,
         * or both branches moved the binding. */
        for (uint32_t i = 0; i < n_move_bindings; i++) {
            move_bindings[i]->is_moved = before_states[i] || (then_states[i] && else_states[i]);
        }
        free(else_states);

        if (!type_eq(then_->type, else_->type)) {
            free(then_states);
            free(move_bindings);
            free(before_states);
            diag_emit(DIAG_ERROR, call->span,
                      "if branches have mismatched types: then=%s else=%s",
                      type_name(then_->type), type_name(else_->type));
            return NULL;
        }
        result_t = then_->type;
    } else {
        /* Without else, then-branch moves are not guaranteed after the if. */
        move_state_restore(move_bindings, before_states, n_move_bindings);
    }

    free(then_states);
    free(move_bindings);
    free(before_states);

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

/* cond desugars to nested if. Supports :else as the last clause. */
static Expr *elab_cond(Elab *e, const Form *call) {
    /* (cond t1 e1 t2 e2 ... :else efinal) — desugars to nested if. */
    uint32_t n = call->as.list.len - 1;
    if ((n & 1) != 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "cond expects pairs of (test expr); got %u clauses", n);
        return NULL;
    }
    /* Build right-to-left. */
    Expr *acc = e_nil(e, call->span);
    Type acc_t = TYPE_NIL;
    for (int i = (int)n - 2; i >= 0; i -= 2) {
        Form *test = call->as.list.items[1 + i];
        Form *body = call->as.list.items[1 + i + 1];

        /* Check for :else keyword */
        if (test->tag == F_KEYWORD && test->as.sym == e->kw_else) {
            acc = elab_form(e, body);
            if (!acc) return NULL;
            acc_t = acc->type;
            continue;
        }

        Expr *cond = elab_form(e, test);
        if (!cond) return NULL;
        if (!type_eq(cond->type, TYPE_BOOL)) {
            diag_emit(DIAG_ERROR, cond->span,
                      "cond test must be bool, got %s", type_name(cond->type));
            return NULL;
        }
        Expr *then_ = elab_form(e, body);
        if (!then_) return NULL;

        /* If this is the first clause (right-most we processed), it sets
         * acc_t. Subsequent clauses must match. */
        if (acc_t.kind == TY_NIL && acc->kind == EX_NIL_LIT) {
            acc_t = then_->type;
            /* allow plain (cond ...) without :else: result type is then_'s type
             * if all branches happen to match; otherwise we'd need a "maybe"
             * type which we don't have. */
            if (then_->type.kind != TY_NIL) {
                /* leave acc as nil; this is the implicit fallthrough branch */
            }
        }
        if (!type_eq(then_->type, acc_t)) {
            diag_emit(DIAG_ERROR, then_->span,
                      "cond branch type %s does not match earlier branch type %s",
                      type_name(then_->type), type_name(acc_t));
            return NULL;
        }

        Expr *out = expr_new(e->arena, EX_IF, acc_t, test->span);
        out->as.if_.cond = cond;
        out->as.if_.then_ = then_;
        out->as.if_.else_or_null = acc;
        acc = out;
    }
    return acc;
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
    /* Defer is only valid inside scope-introducing forms */
    if (e->scope == &e->global) {
        diag_emit(DIAG_ERROR, call->span,
                  "defer is not allowed at module top level");
        return NULL;
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
    
    /* Check that inner is ref<T>, rc<T>, ptr<T>, &T, or &mut T */
    if (inner->type.kind != TY_REF && inner->type.kind != TY_RC && inner->type.kind != TY_PTR_VOID
        && inner->type.kind != TY_REF_IMMUT && inner->type.kind != TY_REF_MUT) {
        diag_emit(DIAG_ERROR, call->span,
                  "@ requires ref<T>, rc<T>, ptr<T>, &T, or &mut T, got %s",
                  type_name(inner->type));
        return NULL;
    }
    
    /* Return type is the inner type */
    Type result_type;
    if (inner->type.kind == TY_REF) {
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

/* Phase 17: Exceptions */

/* (throw expr) - Raise an exception with expr as the payload.
 * Returns: never (always throws)
 * 
 * Elaborates to an EX_THROW expression that contains the expression to evaluate.
 * At codegen time, this lowers to a call to tur_throw().
 */
static Expr *elab_throw(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(throw expr) requires exactly one argument");
        return NULL;
    }
    
    /* Elaborate the expression to throw */
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    
    /* Create EX_THROW expression */
    Expr *out = expr_new(e->arena, EX_THROW, TYPE_NIL, call->span);
    out->as.throw_.payload = payload;
    return out;
}

/* (try body (catch [e] handler) ...) - Try-catch expression.
 * Supports multiple catch clauses and optional finally.
 * 
 * Syntax:
 *   (try body)
 *   (try body (catch [e] handler))
 *   (try body (finally cleanup))
 *   (try body (catch [e] handler) (finally cleanup))
 * 
 * Returns: the result of body, or the result of the matching catch handler.
 * 
 * Elaborates to EX_TRY expression that contains:
 *   - body: the try body
 *   - catch_clauses: array of catch patterns and handlers
 *   - finally_body: optional cleanup body
 */
static Expr *elab_try(Elab *e, const Form *call) {
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(try body ...) requires at least a body");
        return NULL;
    }
    
    /* Parse body (can be single expression or do-form) */
    Expr *body = elab_form(e, call->as.list.items[1]);
    if (!body) return NULL;
    Type result_type = body->type;
    
    /* Parse remaining clauses (catch and finally) */
    /* Phase 17: Use fixed-size array for catch clauses (max 8) for simplicity */
    #define MAX_CATCH_CLAUSES 8
    TryCatchClause clauses[MAX_CATCH_CLAUSES];
    uint8_t n_clauses = 0;
    Expr *finally_body = NULL;
    
    for (uint32_t i = 2; i < call->as.list.len; i++) {
        Form *clause_form = call->as.list.items[i];
        
        if (clause_form->tag != F_LIST) {
            diag_emit(DIAG_ERROR, clause_form->span,
                      "try clause must be a list like (catch ...) or (finally ...)");
            return NULL;
        }
        
        if (clause_form->as.list.len == 0) {
            diag_emit(DIAG_ERROR, clause_form->span,
                      "try clause cannot be empty");
            return NULL;
        }
        
        Form *clause_name = clause_form->as.list.items[0];
        if (clause_name->tag != F_SYM) {
            diag_emit(DIAG_ERROR, clause_name->span,
                      "try clause must start with a symbol (catch or finally)");
            return NULL;
        }
        
        if (clause_name->as.sym == e->sym_catch) {
            /* Parse catch clause: (catch [e] handler) or (catch [e : type] handler) */
            if (clause_form->as.list.len < 2) {
                diag_emit(DIAG_ERROR, clause_form->span,
                          "(catch ...) requires a binding and handler");
                return NULL;
            }
            
            /* Parse binding - can be [e] or [e : type] */
            Form *binding_form = clause_form->as.list.items[1];
            if (binding_form->tag != F_VEC) {
                diag_emit(DIAG_ERROR, binding_form->span,
                          "catch binding must be a vector like [e] or [e : type]");
                return NULL;
            }
            
            if (binding_form->as.list.len != 1 && binding_form->as.list.len != 2) {
                diag_emit(DIAG_ERROR, binding_form->span,
                          "catch binding must have 1 or 2 elements (name and optional type)");
                return NULL;
            }
            
            Form *var_name = binding_form->as.list.items[0];
            if (var_name->tag != F_SYM) {
                diag_emit(DIAG_ERROR, var_name->span,
                          "catch variable must be a symbol");
                return NULL;
            }
            
            /* Parse optional type annotation */
            TypeKind catch_type = TY_NIL;  /* No annotation = catch-all (TY_NIL) */
            if (binding_form->as.list.len == 2) {
                Form *type_form = binding_form->as.list.items[1];
                if (type_form->tag != F_KEYWORD) {
                    diag_emit(DIAG_ERROR, type_form->span,
                              "catch type annotation must be a keyword like :int");
                    return NULL;
                }
                const Symbol *kw = type_form->as.sym;
                if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                    catch_type = TY_INT;
                } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                    catch_type = TY_BOOL;
                } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                    catch_type = TY_CSTR;
                } else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) {
                    catch_type = TY_PTR_VOID;
                } else if ((kw->len == 4 && memcmp(kw->name, "void", 4) == 0) ||
                           (kw->len == 3 && memcmp(kw->name, "nil", 3) == 0)) {
                    catch_type = TY_NIL;  /* Catch-all */
                } else {
                    diag_emit(DIAG_ERROR, type_form->span,
                              "unsupported type in catch clause");
                    return NULL;
                }
            }
            
            /* Parse handler body */
            if (clause_form->as.list.len < 3) {
                diag_emit(DIAG_ERROR, clause_form->span,
                          "(catch ...) requires a handler body");
                return NULL;
            }
            
            /* Create a binding for the catch variable in a new scope */
            TypeKind catch_var_kind = (catch_type == TY_NIL) ? TY_PTR_VOID : catch_type;
            Type catch_var_type = {.kind = catch_var_kind, .copy_kind = CK_MOVE, .n_lifetimes = 0};
            Binding *catch_binding = binding_new(e, var_name->as.sym, catch_var_type, false, false, var_name->span);
            
            /* Push a new scope for the handler with the catch variable bound */
            Scope handler_scope;
            scope_init(&handler_scope, e->scope);
            e->scope = &handler_scope;
            scope_add(&handler_scope, catch_binding);
            
            Expr *handler = elab_form(e, clause_form->as.list.items[2]);
            if (!handler) { 
                e->scope = handler_scope.parent; 
                scope_free(&handler_scope); 
                return NULL; 
            }

            /* try/catch is an expression: body and each catch handler must agree on type.
             * Treat nil-typed branches as throw-only/bottom-like so they can unify with
             * a concrete type from other branches. */
            if (!type_eq(handler->type, result_type)) {
                if (result_type.kind == TY_NIL && handler->type.kind != TY_NIL) {
                    result_type = handler->type;
                } else if (handler->type.kind == TY_NIL && result_type.kind != TY_NIL) {
                    /* Keep current result_type; nil handler is compatible. */
                } else {
                    diag_emit(DIAG_ERROR, clause_form->span,
                              "try/catch type mismatch: body is %s but catch handler is %s",
                              type_name(result_type), type_name(handler->type));
                    e->scope = handler_scope.parent;
                    scope_free(&handler_scope);
                    return NULL;
                }
            }
            
            /* Pop the handler scope */
            e->scope = handler_scope.parent;
            
            /* Store catch clause */
            if (n_clauses >= MAX_CATCH_CLAUSES) {
                diag_emit(DIAG_ERROR, clause_form->span,
                          "too many catch clauses (max %d)", MAX_CATCH_CLAUSES);
                scope_free(&handler_scope);
                return NULL;
            }
            clauses[n_clauses].var_name = var_name->as.sym;
            clauses[n_clauses].binding = catch_binding;
            clauses[n_clauses].catch_type = catch_type;
            clauses[n_clauses].handler = handler;
            n_clauses++;
            
            scope_free(&handler_scope);
            
        } else if (clause_name->as.sym == e->sym_finally) {
            /* Parse finally clause: (finally cleanup) */
            if (clause_form->as.list.len < 2) {
                diag_emit(DIAG_ERROR, clause_form->span,
                          "(finally ...) requires a cleanup body");
                return NULL;
            }
            
            if (finally_body) {
                diag_emit(DIAG_ERROR, clause_form->span,
                          "try can have at most one finally clause");
                return NULL;
            }
            
            finally_body = elab_form(e, clause_form->as.list.items[1]);
            if (!finally_body) return NULL;
            
        } else {
            diag_emit(DIAG_ERROR, clause_name->span,
                      "unknown try clause: expected catch or finally");
            return NULL;
        }
    }
    
    /* Create EX_TRY expression */
    /* Copy clauses to arena memory */
    TryCatchClause *arena_clauses = NULL;
    if (n_clauses > 0) {
        arena_clauses = (TryCatchClause *)arena_alloc(e->arena, n_clauses * sizeof(TryCatchClause));
        memcpy(arena_clauses, clauses, n_clauses * sizeof(TryCatchClause));
    }
    
    Expr *out = expr_new(e->arena, EX_TRY, result_type, call->span);
    out->as.try_.body = body;
    out->as.try_.clauses = arena_clauses;
    out->as.try_.n_clauses = n_clauses;
    out->as.try_.finally_body = finally_body;
    return out;
}

/* catch and finally are not standalone - they're handled as part of try */
static Expr *elab_catch(Elab *e, const Form *call) {
    diag_emit(DIAG_ERROR, call->span,
              "(catch ...) must be inside a (try ...) expression");
    return NULL;
}

static Expr *elab_finally(Elab *e, const Form *call) {
    diag_emit(DIAG_ERROR, call->span,
              "(finally ...) must be inside a (try ...) expression");
    return NULL;
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

/* Phase 19: Algebraic effects */

/* Forward declaration (defined after elab_defeffect) */
static Expr *elab_handle(Elab *e, const Form *call);

/* (try-with body (EffectName [params] k) handler ...)
 * Sugar: identical to (handle body ...).
 * Provided for OCaml/algebraic-effects familiarity.
 */
static Expr *elab_try_with(Elab *e, const Form *call) {
    /* try-with has the same surface syntax as handle; delegate directly. */
    return elab_handle(e, call);
}

/* (defeffect Name [param1 : T1, param2 : T2, ...] : R)
 * Declares a new algebraic effect with parameters and a result type.
 */
static Expr *elab_defeffect(Elab *e, const Form *call) {
    /* Minimum: (defeffect Name [params...] :ret-type) */
    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "defeffect requires (defeffect Name [params...] result-type)");
        return NULL;
    }
    
    /* Parse effect name */
    Form *name_f = call->as.list.items[1];
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
    
    /* Parse parameter list */
    Form *params_f = call->as.list.items[2];
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
            if (param_f->tag == F_KEYWORD) {
                /* Type annotation for preceding param — already handled below */
                continue;
            }
            if (param_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, param_f->span,
                          "defeffect: parameter name must be a symbol");
                return NULL;
            }
            param_names[p] = param_f->as.sym;
            /* Check if the next item is a type keyword */
            TypeKind pk = TY_INT;
            if (i + 1 < raw_n) {
                Form *next = params_f->as.list.items[i + 1];
                if (next->tag == F_KEYWORD) {
                    pk = typekind_from_symbol(next->as.sym->name);
                    if (pk == TY_UNKNOWN) pk = TY_INT;
                    i++; /* Consume the type keyword */
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
    Form *ret_f = call->as.list.items[3];
    if (ret_f->tag != F_SYM && ret_f->tag != F_KEYWORD) {
        diag_emit(DIAG_ERROR, ret_f->span,
                  "defeffect: return type annotation must be a symbol or keyword like :int");
        return NULL;
    }
    
    TypeKind result_type = typekind_from_symbol(ret_f->as.sym->name);
    if (result_type == TY_UNKNOWN) {
        diag_emit(DIAG_ERROR, ret_f->span,
                  "defeffect: unknown return type '%s'", ret_f->as.sym->name);
        return NULL;
    }
    
    /* Register the effect */
    Effect *effect = effect_env_register(e->effect_env, e->arena, name,
                                          param_names, param_types, n_params, result_type);
    if (!effect) return NULL;
    
    /* Create the effect definition expression */
    EffectDef *def = arena_alloc(e->arena, sizeof(EffectDef));
    def->name = name;
    def->param_names = param_names;
    def->param_types = param_types;
    def->n_params = n_params;
    def->result_type = result_type;
    
    Expr *out = expr_new(e->arena, EX_DEFECT, TYPE_NIL, call->span);
    out->as.effect_def_.def = def;
    return out;
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
    
    /* Parse the body to be handled */
    Expr *body = elab_form(e, call->as.list.items[1]);
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
         * Marked CK_MOVE so the one-shot check fires on a second resume/discontinue. */
        Binding *kb = binding_new(e, cases[i].k_name, TYPE_INT, false, false, k_f->span);
        kb->type.copy_kind = CK_MOVE;
        cases[i].k_binding = kb;
        scope_add(&handler_scope, kb);
        
        /* Parse handler body inside the handler scope */
        Form *body_f = call->as.list.items[3 + (i * 2)];
        cases[i].body = elab_form(e, body_f);
        
        /* Restore outer scope */
        e->scope = saved_scope;
        scope_free(&handler_scope);
        
        if (!cases[i].body) return NULL;
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

    /* Extract parameter symbols */
    Form **params = (Form **)arena_alloc(e->arena, params_f->as.list.len * sizeof(Form *));
    for (uint32_t i = 0; i < params_f->as.list.len; i++) {
        Form *p = params_f->as.list.items[i];
        if (p->tag != F_SYM) {
            diag_emit(DIAG_ERROR, p->span,
                      "defmacro: parameter must be a symbol, got %s", 
                      p->tag == F_KEYWORD ? "keyword" : "non-symbol");
            return NULL;
        }
        params[i] = p;
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
    macro->n_params = params_f->as.list.len;
    macro->body = body;
    macro->span = call->span;

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

/* Phase 2: defn — (defn name [param1 param2 ...] : return-type body...)
 * For now, we only support : int return type annotation. Param types are
 * inferred from usage. */
static Expr *elab_defn(Elab *e, const Form *call) {
    /* Minimum: (defn name []) */
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "defn requires (defn name [params...] body...)");
        return NULL;
    }

    /* Parse name */
    Form *name_f = call->as.list.items[1];
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
    Form *params_f = call->as.list.items[2];
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
    
    for (uint32_t i = 0; i < params_f->as.list.len; i++) {
        Form *p = params_f->as.list.items[i];
        
        /* Phase 15: Handle constraint annotations (^Eq, ^Show, etc.) */
        if (p->tag == F_SYM && p->as.sym->len > 0 && p->as.sym->name[0] == '^') {
            /* This is a constraint annotation like ^Eq */
            const Symbol *constraint_name = p->as.sym;
            /* Look up the typeclass (skip the ^ character) */
            const char *tc_name_str = constraint_name->name + 1;  /* Skip '^' */
            uint32_t tc_name_len = constraint_name->len - 1;
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
            } else if (kw->len == 9 && memcmp(kw->name, "ptr<void>", 9) == 0) {
                param_kinds[n_params - 1] = TY_PTR_VOID;
                params[n_params - 1]->type = TYPE_PTR_VOID;
            } else {
                /* Try to look up as a type variable or typeclass */
                /* For now, default to int */
                param_kinds[n_params - 1] = TY_INT;
                params[n_params - 1]->type = TYPE_INT;
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
        if (n_params == 0) {
            params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
        }
        params[n_params++] = b;
    }
    
    /* Phase 13: Lifetime annotations parsing deferred - restore original simple parsing */

    /* Parse return type annotation and body */
    TypeKind return_kind = TY_NIL;
    uint32_t body_start = 3;

    /* Phase 19: Skip optional effect-row annotation #{Read Write} before return type.
     * In v1 these are parsed and accepted but not enforced by the type checker. */
    if (call->as.list.len >= 4) {
        Form *maybe_row = call->as.list.items[3];
        if (maybe_row->tag == F_MAP) {
            diag_emit(DIAG_NOTE, maybe_row->span,
                      "effect-row annotations are not yet enforced (Phase 19 v1)");
            body_start = 4;
        }
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
            } else {
                diag_emit(DIAG_ERROR, ret_f->span,
                          "defn: unsupported return type keyword :%s",
                          kw->name);
                return NULL;
            }
            body_start++;
        }
    }

    /* Elaborate body */
    if (call->as.list.len < body_start + 1) {
        diag_emit(DIAG_ERROR, call->span,
                  "defn: missing body");
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
    if (n_body == 1) {
        body = elab_form(e, call->as.list.items[body_start]);
        if (!body) { e->scope = inner.parent; scope_free(&inner); return NULL; }
    } else {
        Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
        for (uint32_t i = 0; i < n_body; i++) {
            items[i] = elab_form(e, call->as.list.items[body_start + i]);
            if (!items[i]) { e->scope = inner.parent; scope_free(&inner); return NULL; }
        }
        body = expr_new(e->arena, EX_DO, items[n_body - 1]->type, call->span);
        body->as.do_.items = items;
        body->as.do_.n = n_body;
    }

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
        arg_kinds[i] = param_kinds[i];
    }
    Type fn_type = type_fn(arg_kinds, n_params, return_kind);

    /* Create/update binding for the function.
     * Reuse pass-1 forward bindings in place so subsequent lookups observe
     * updated arity/types from the real definition. */
    Binding *b = NULL;
    if (existing && existing->type.kind == TY_FN && existing->is_global) {
        b = existing;
        b->type = fn_type;
        b->span = name_f->span;
    } else {
        b = binding_new(e, name_f->as.sym, fn_type, false, true, name_f->span);
        scope_add(&e->global, b);
    }

    /* Build FnDef */
    FnDef *fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    fd->binding = b;
    fd->params = params;
    fd->n_params = n_params;
    fd->body = body;
    fd->is_variadic = false;
    fd->closure = NULL;
    /* Store param types for codegen */
    fd->param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
    for (uint8_t i = 0; i < n_params; i++) {
        fd->param_types[i] = type_from_kind(param_kinds[i]);
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

    /* Phase 19: Skip optional effect-row annotation #{Read Write} before return type. */
    if (call->as.list.len >= 3) {
        Form *maybe_row = call->as.list.items[2];
        if (maybe_row->tag == F_MAP) {
            diag_emit(DIAG_NOTE, maybe_row->span,
                      "effect-row annotations are not yet enforced (Phase 19 v1)");
            body_start = 3;
        }
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
            } else {
                diag_emit(DIAG_ERROR, ret_f->span,
                          "fn: unsupported return type keyword :%s",
                          kw->name);
                /* params is arena-allocated, no need to free */
                return NULL;
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
    if (n_body == 1) {
        body = elab_form(e, call->as.list.items[body_start]);
        if (!body) { e->scope = inner.parent; scope_free(&inner); /* params is arena-allocated */ return NULL; }
    } else {
        Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
        for (uint32_t i = 0; i < n_body; i++) {
            items[i] = elab_form(e, call->as.list.items[body_start + i]);
            if (!items[i]) { e->scope = inner.parent; scope_free(&inner); /* params is arena-allocated */ return NULL; }
        }
        body = expr_new(e->arena, EX_DO, items[n_body - 1]->type, call->span);
        body->as.do_.items = items;
        body->as.do_.n = n_body;
    }

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
        /* No captures - can use static function */
        if (!at_top_level) {
            /* Nested without captures: register FN_DEF for file-scope emission */
            elab_register_file_def(e, fn_def_expr);
        }
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
        /* Handle type annotation keyword after the previous param */
        if (p->tag == F_KEYWORD) {
            if (n_params == 0) {
                diag_emit(DIAG_ERROR, p->span,
                          "extern-c: type annotation without preceding parameter");
                return NULL;
            }
            const Symbol *kw = p->as.sym;
            TypeKind pk = TY_INT;
            if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0)        pk = TY_INT;
            else if (kw->len == 5 && memcmp(kw->name, "float", 5) == 0) pk = TY_FLOAT;
            else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0)  pk = TY_BOOL;
            else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0)  pk = TY_CSTR;
            else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0)   pk = TY_PTR_VOID;
            else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0)  pk = TY_NIL;
            else {
                diag_emit(DIAG_ERROR, p->span,
                          "extern-c: unsupported parameter type :%s", kw->name);
                return NULL;
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
        
        param_kinds[n_params] = TY_INT;
        Binding *b = binding_new(e, p->as.sym, TYPE_INT, false, false, p->span);
        if (n_params == 0) {
            params = (Binding **)arena_alloc(e->arena, MAX_FN_ARITY * sizeof(Binding *));
        }
        params[n_params++] = b;
    }

    /* Parse return type annotation */
    Form *ret_f = call->as.list.items[3];
    if (ret_f->tag != F_KEYWORD) {
        diag_emit(DIAG_ERROR, ret_f->span,
                  "extern-c: return type must be a keyword (:int, :bool, :void, :cstr, :ptr)");
        return NULL;
    }

    TypeKind return_kind = TY_NIL;
    const Symbol *kw = ret_f->as.sym;
    if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
        return_kind = TY_INT;
    } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
        return_kind = TY_BOOL;
    } else if (kw->len == 4 && memcmp(kw->name, "void", 4) == 0) {
        return_kind = TY_NIL;
    } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
        return_kind = TY_CSTR;
    } else if (kw->len == 3 && memcmp(kw->name, "ptr", 3) == 0) {
        return_kind = TY_PTR_VOID;
    } else {
        diag_emit(DIAG_ERROR, ret_f->span,
                  "extern-c: unsupported return type :%s", kw->name);
        return NULL;
    }

    /* Create function type */
    Type fn_type = type_fn(param_kinds, n_params, return_kind);

    /* Create a binding for the extern-c function so it can be looked up and called */
    Binding *b = binding_new(e, name_f->as.sym, fn_type, false, true, call->span);
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
    if (call->as.list.len != 3) {
        diag_emit(DIAG_ERROR, call->span, "def takes (def name init)");
        return NULL;
    }
    Form *name_f = call->as.list.items[1];
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
    Expr *init = elab_form(e, call->as.list.items[2]);
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
    scope_add(&e->global, b);

    Expr *out = expr_new(e->arena, EX_DEF, TYPE_NIL, call->span);
    out->as.def_.binding = b;
    out->as.def_.init = init;
    out->as.def_.struct_def = NULL;
    return out;
}

/* ---- Phase 2: defn, fn, extern-c ---- */

static Expr *elab_defn(Elab *e, const Form *call);
static Expr *elab_fn(Elab *e, const Form *call);
static Expr *elab_extern_c(Elab *e, const Form *call);
static Expr *elab_call_fn(Elab *e, const Form *call, Binding *fn_binding);
/* Phase 11: defstruct */
static Expr *elab_defstruct(Elab *e, const Form *call);
static Expr *elab_make_struct(Elab *e, const Form *call);
/* Phase 12: Borrow traits */
static Expr *elab_borrow_immut(Elab *e, const Form *call);
static Expr *elab_borrow_mut(Elab *e, const Form *call);
/* Phase 15: Typeclasses */
static Expr *elab_defclass(Elab *e, const Form *call);
static Expr *elab_definstance(Elab *e, const Form *call);
static Expr *elab_method_call(Elab *e, const Form *call);
static TypeClassMethod *parse_typeclass_method(Elab *e, Form *method_form, Span span);
/* Phase 17: throw! sugar */
static Expr *elab_throw_bang(Elab *e, const Form *call);
/* Phase 18: call/cc and escape sugar */
static Expr *elab_call_cc(Elab *e, const Form *call);
static Expr *elab_escape(Elab *e, const Form *call);

/* ---- general elab ---- */

static Expr *elab_call(Elab *e, Form *call) {
    /* Already established: call->tag == F_LIST and len >= 1. */
    Form *head = call->as.list.items[0];
    if (head->tag != F_SYM) {
        diag_emit(DIAG_ERROR, head->span,
                  "call head must be a symbol (functions arrive in phase 2)");
        return NULL;
    }
    const Symbol *name = head->as.sym;

    /* Special forms. */
    if (name == e->sym_def)    return elab_def   (e, call);
    if (name == e->sym_let)    return elab_let   (e, call);
    if (name == e->sym_if)     return elab_if    (e, call);
    if (name == e->sym_do)     return elab_do    (e, call);
    if (name == e->sym_set)    return elab_set   (e, call);
    if (name == e->sym_while)  return elab_while (e, call);
    if (name == e->sym_cond)   return elab_cond  (e, call);
    /* Phase 4 */
    if (name == e->sym_defer)  return elab_defer (e, call);
    if (name == e->sym_return) return elab_return(e, call);
    /* Phase 5 */
    if (name == e->sym_ref)    return elab_ref   (e, call);
    if (name == e->sym_deref)  return elab_deref (e, call);
    if (name == e->sym_drop)   return elab_drop  (e, call);
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
    /* Phase 17: Exceptions */
    if (name == e->sym_throw)       return elab_throw(e, call);
    if (name == e->sym_throw_bang)  return elab_throw_bang(e, call);
    if (name == e->sym_try)         return elab_try(e, call);
    if (name == e->sym_catch)       return elab_catch(e, call);
    if (name == e->sym_finally)     return elab_finally(e, call);
    /* Phase 18: Delimited continuations */
    if (name == e->sym_reset)      return elab_reset(e, call);
    if (name == e->sym_shift)      return elab_shift(e, call);
    if (name == e->sym_shift0)     return elab_shift0(e, call);
    if (name == e->sym_call_cc)    return elab_call_cc(e, call);
    if (name == e->sym_escape)     return elab_escape(e, call);
    /* Phase 19: Algebraic effects */
    if (name == e->sym_defeffect) return elab_defeffect(e, call);
    if (name == e->sym_perform)   return elab_perform(e, call);
    if (name == e->sym_handle)    return elab_handle(e, call);
    if (name == e->sym_try_with)  return elab_try_with(e, call);
    if (name == e->sym_resume)    return elab_resume(e, call);
    if (name == e->sym_discontinue) return elab_discontinue(e, call);
    /* Phase 10: GC */
    if (name == e->sym_gc_force)    return elab_gc_force(e, call);
    if (name == e->sym_gc_enable)   return elab_gc_enable(e, call);
    if (name == e->sym_gc_disable)  return elab_gc_disable(e, call);
    /* Phase 11: defstruct */
    if (name == e->sym_defstruct) return elab_defstruct(e, call);
    if (name == e->sym_make_struct) return elab_make_struct(e, call);
    /* Phase 12: Borrow traits */
    if (name == e->sym_borrow) return elab_borrow_immut(e, call);
    if (name == e->sym_borrow_mut) return elab_borrow_mut(e, call);
    /* Phase 15: Typeclasses */
    if (name == e->sym_defclass) return elab_defclass(e, call);
    if (name == e->sym_definstance) return elab_definstance(e, call);
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
    if (name == e->sym_extern_c) return elab_extern_c(e, call);

    /* Phase 6: Check if it's a macro call */
    MacroDef *macro = elab_lookup_macro(e, name);
    if (macro) {
        /* Expand the macro with arguments */
        /* Extract arguments (rest of list) */
        uint32_t n_args = call->as.list.len - 1;
        Form **args = (n_args == 0) ? NULL : (Form **)arena_alloc(e->arena, n_args * sizeof(Form *));
        for (uint32_t i = 0; i < n_args; i++) {
            args[i] = call->as.list.items[1 + i];
        }
        
        Form *expanded = elab_expand_macro(e, macro, args, n_args);
        if (!expanded) return NULL;
        
        /* Recursively elaborate the expanded form */
        return elab_form(e, expanded);
    }

    /* Phase 2: Check if it's a user-defined function call */
    Binding *fn_binding = scope_lookup(e->scope, name);
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
        /* Phase 11: Move tracking - if arg is a CK_MOVE binding reference, poison it */
        if (args[i]->kind == EX_VAR && type_is_move(args[i]->as.var.binding->type)) {
            binding_mark_moved(args[i]->as.var.binding, args[i]->span);
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
        } else {
            diag_emit(DIAG_ERROR, head->span,
                      "unknown function or operator '%s'", name->name);
        }
        return NULL;
    }
    /* All args must match the spec's arg type. */
    for (uint32_t i = 0; i < n_args; i++) {
        if (!type_eq(args[i]->type, spec->arg_type)) {
            /* Phase 8: Enhanced type mismatch diagnostic with error code */
            DiagNote notes[1];
            notes[0] = (DiagNote){DIAG_NOTE, args[i]->span, "argument has this type"};
            
            const char *expected_str = type_name(spec->arg_type);
            const char *actual_str = type_name(args[i]->type);
            
            /* Check if we can suggest a coercion */
            const char *suggestion = NULL;
            if (args[i]->type.kind == TY_BOOL && spec->arg_type.kind == TY_INT) {
                suggestion = "try wrapping the bool in (if x 1 0)";
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

/* Phase 2: Elaborate a function call (f a b c) */
static Expr *elab_call_fn(Elab *e, const Form *call, Binding *fn_binding) {
    uint32_t n_args = call->as.list.len - 1;

    /* Get the function type */
    Type fn_type = fn_binding->type;
    
    /* For closure bindings, use the closure's thunk function type */
    if (fn_binding->closure_fn_binding) {
        /* This is a closure - get the thunk function type */
        fn_type = fn_binding->closure_fn_binding->type;
    } else if (fn_binding->type.kind == TY_PTR_VOID) {
        /* Phase 19: callback values passed as ptr<void> are callable (v1: no-arg). */
        if (n_args != 0) {
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0002_ARITY_MISMATCH,
                                "callback '%s' expects %u argument(s), got %u",
                                fn_binding->name->name, 0u, n_args);
            return NULL;
        }
        Expr *out = expr_new(e->arena, EX_CALL, TYPE_INT, call->span);
        out->as.call_.fn_binding = fn_binding;
        out->as.call_.args = NULL;
        out->as.call_.n_args = 0;
        return out;
    }
    
    if (fn_type.kind != TY_FN && fn_type.kind != TY_CONT) {
        diag_emit(DIAG_ERROR, call->span,
                  "'%s' is not a function or continuation", fn_binding->name->name);
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

        bool arg_ok = (args[i]->type.kind == expected_arg_kind);
        if (!arg_ok && expected_arg_kind == TY_PTR_VOID && args[i]->type.kind == TY_FN) {
            /* Allow passing a function value where callback pointer is expected. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_PTR_VOID && args[i]->type.kind == TY_NIL) {
            /* Allow nil as a null pointer for ptr<void> parameters. */
            arg_ok = true;
        }

        if (!arg_ok) {
            /* Phase 8: Enhanced type mismatch with error code */
            diag_emit_with_code(DIAG_ERROR, args[i]->span, TUR_E0001_TYPE_MISMATCH,
                                "function '%s' arg %u: expected %s, got %s",
                                fn_binding->name->name, i + 1,
                                type_name(type_from_kind(expected_arg_kind)),
                                type_name(args[i]->type));
            return NULL;
        }
        /* Phase 11: Move tracking - if arg is a CK_MOVE binding reference, poison it */
        if (args[i]->kind == EX_VAR && type_is_move(args[i]->as.var.binding->type)) {
            binding_mark_moved(args[i]->as.var.binding, args[i]->span);
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
            Expr *out = expr_new(e->arena, EX_INT_LIT, TYPE_INT, f->span);
            out->as.i = f->as.i;
            return out;
        }
        case F_FLOAT: {
            Expr *out = expr_new(e->arena, EX_FLOAT_LIT, TYPE_FLOAT, f->span);
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
                      "phase 1: keywords are only allowed as :else in cond");
            return NULL;
        case F_SYM: {
            Binding *b = scope_lookup(e->scope, f->as.sym);
            if (!b) {
                /* Phase 8: Enhanced unbound symbol diagnostic with suggestions */
                /* Try to find similar symbols in scope for "did you mean" suggestions */
                const Symbol *best_match = NULL;
                int best_distance = 3; /* Max edit distance for suggestions */
                
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
            /* Phase 11: Check for use-after-move */
            if (!binding_check_not_moved(b, f->span, "binding")) {
                return NULL;
            }
            Expr *out = expr_new(e->arena, EX_VAR, b->type, f->span);
            out->as.var.binding = b;
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
    if (tlen == 4  && memcmp(tname, "bool",  4) == 0) { *out_kind = TY_BOOL;     return; }
    if (tlen == 5  && memcmp(tname, "float", 5) == 0) { *out_kind = TY_FLOAT;    return; }
    if (tlen == 4  && memcmp(tname, "cstr",  4) == 0) { *out_kind = TY_CSTR;     return; }
    if (tlen == 3  && memcmp(tname, "nil",   3) == 0) { *out_kind = TY_NIL;      return; }
    if (tlen == 4  && memcmp(tname, "void",  4) == 0) { *out_kind = TY_NIL;      return; }
    if (tlen == 9  && memcmp(tname, "ptr<void>", 9) == 0) { *out_kind = TY_PTR_VOID; return; }

    /* Compound types: rc<T>, ref<T>, weak<T> */
    /* Parse the prefix and inner type */
    const char *prefix_rc   = "rc<";
    const char *prefix_ref  = "ref<";
    const char *prefix_weak = "weak<";

    TypeKind prefix_kind = TY_UNKNOWN;
    uint32_t prefix_len = 0;
    if (tlen > 3 && memcmp(tname, prefix_rc, 3) == 0)   { prefix_kind = TY_RC;   prefix_len = 3; }
    if (tlen > 4 && memcmp(tname, prefix_ref, 4) == 0)  { prefix_kind = TY_REF;  prefix_len = 4; }
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
            return true;
        default:
            return false;
    }
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
    
    /* Check for optional :copy / :move annotation */
    bool is_copy = false;
    uint32_t fields_start_idx = 2;
    
    if (call->as.list.len >= 3) {
        Form *kw_form = call->as.list.items[2];
        if (kw_form->tag == F_KEYWORD && kw_form->as.sym == e->kw_copy) {
            is_copy = true;
            fields_start_idx = 3;
        } else if (kw_form->tag == F_KEYWORD && kw_form->as.sym == e->kw_move) {
            is_copy = false;
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
    
    if (scope_lookup(e->scope, name)) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defstruct: '%s' is already defined", name->name);
        return NULL;
    }
    
    /* Validate field list */
    uint32_t n_fields = fields_form->as.list.len;
    if (n_fields == 0) {
        diag_emit(DIAG_ERROR, fields_form->span,
                  "defstruct field list cannot be empty");
        return NULL;
    }
    if ((n_fields % 2) != 0) {
        diag_emit(DIAG_ERROR, fields_form->span,
                  "defstruct field list must have [name :type ...] pairs");
        return NULL;
    }

    uint32_t actual_n_fields = n_fields / 2;

    /* Allocate StructDef and field array */
    StructDef *def = (StructDef *)malloc(sizeof(StructDef));
    def->name = name->name;
    def->n_fields = actual_n_fields;
    def->fields = (StructField *)malloc(actual_n_fields * sizeof(StructField));
    def->is_copy = is_copy;
    def->needs_drop_glue = false;

    /* Parse field pairs */
    for (uint32_t i = 0; i < n_fields; i += 2) {
        uint32_t fi = i / 2;
        Form *field_name_form = fields_form->as.list.items[i];
        Form *field_type_form = fields_form->as.list.items[i + 1];

        if (field_name_form->tag != F_SYM) {
            diag_emit(DIAG_ERROR, field_name_form->span,
                      "defstruct field name must be a symbol");
            free(def->fields); free(def);
            return NULL;
        }
        if (field_type_form->tag != F_KEYWORD) {
            diag_emit(DIAG_ERROR, field_type_form->span,
                      "defstruct field '%s' type must be a keyword like :int",
                      field_name_form->as.sym->name);
            free(def->fields); free(def);
            return NULL;
        }

        const char *tname = field_type_form->as.sym->name;
        uint32_t tlen = field_type_form->as.sym->len;
        TypeKind fkind, finner;
        parse_struct_field_type(tname, tlen, &fkind, &finner);

        if (fkind == TY_UNKNOWN) {
            diag_emit(DIAG_ERROR, field_type_form->span,
                      "defstruct field '%s' has unrecognized type :%s",
                      field_name_form->as.sym->name, tname);
            free(def->fields); free(def);
            return NULL;
        }

        /* :copy struct validation: all fields must be copy */
        if (is_copy && !typekind_is_copy_for_struct(fkind)) {
            diag_emit(DIAG_ERROR, field_type_form->span,
                      "defstruct: field '%s' has non-copy type :%s and cannot be used in :copy struct",
                      field_name_form->as.sym->name, tname);
            free(def->fields); free(def);
            return NULL;
        }

        def->fields[fi].name = field_name_form->as.sym->name;
        def->fields[fi].kind = fkind;
        def->fields[fi].inner_kind = finner;

        /* Check if this field requires drop glue */
        if (fkind == TY_RC || fkind == TY_REF || fkind == TY_WEAK) {
            def->needs_drop_glue = true;
        }
    }

    /* Register struct in elab registry */
    elab_register_struct_def(e, def);

    /* Create a global binding for the struct type with proper TY_STRUCT type */
    Type struct_type = type_struct(def);
    Binding *b = binding_new(e, name, struct_type, false, true, name_form->span);
    scope_add(&e->global, b);

    /* Return EX_DEF with struct_def populated */
    Expr *out = expr_new(e->arena, EX_DEF, TYPE_NIL, call->span);
    out->as.def_.binding = b;
    out->as.def_.init = NULL;
    out->as.def_.struct_def = def;
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
    
    if (n_params > 0) {
        param_names = (const Symbol **)arena_alloc(e->arena, n_params * sizeof(const Symbol *));
        param_types = (Type *)arena_alloc(e->arena, n_params * sizeof(Type));
        
        for (uint8_t i = 0; i < n_params; i++) {
            Form *p = params_form->as.list.items[i];
            if (p->tag == F_SYM) {
                param_names[i] = p->as.sym;
                /* Default to int for now - type inference for method params deferred */
                param_types[i] = TYPE_INT;
            } else if (p->tag == F_VEC && p->as.list.len >= 2) {
                /* [name : type] syntax */
                Form *name_f = p->as.list.items[0];
                Form *type_f = p->as.list.items[1];
                if (name_f->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, name_f->span,
                              "parameter name must be a symbol");
                    return NULL;
                }
                param_names[i] = name_f->as.sym;
                /* Parse type annotation */
                if (type_f->tag == F_KEYWORD) {
                    const Symbol *kw = type_f->as.sym;
                    if (kw->len == 3 && memcmp(kw->name, "int", 3) == 0) {
                        param_types[i] = TYPE_INT;
                    } else if (kw->len == 4 && memcmp(kw->name, "bool", 4) == 0) {
                        param_types[i] = TYPE_BOOL;
                    } else if (kw->len == 4 && memcmp(kw->name, "cstr", 4) == 0) {
                        param_types[i] = TYPE_CSTR;
                    } else {
                        diag_emit(DIAG_ERROR, type_f->span,
                                  "unsupported type in typeclass method parameter");
                        return NULL;
                    }
                } else {
                    param_types[i] = TYPE_INT; /* default */
                }
            } else {
                diag_emit(DIAG_ERROR, p->span,
                          "parameter must be a symbol or [name : type] vector");
                return NULL;
            }
        }
    }
    
    /* Parse return type - must be after params */
    /* Syntax: (method [params] : return-type)
     *      or (method [params] : #{Effect...} return-type)  — effect row annotation
     *      or (method [params] #{Effect...} : return-type)  — effect row annotation alt
     *
     * Phase 15: #{...} (F_MAP) in return-type position is an advisory effect-row
     * annotation.  We skip over it silently; enforcement deferred to Phase 19. */
    Type return_type = TYPE_NIL;
    uint32_t ret_idx = 2;   /* first element after params vector */
    if (method_form->as.list.len > ret_idx) {
        Form *maybe_row = method_form->as.list.items[ret_idx];
        if (maybe_row->tag == F_MAP) {
            /* #{Effect...} effect-row annotation — skip silently (v1 advisory) */
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
            } else {
                diag_emit(DIAG_ERROR, ret_form->span,
                          "unsupported return type in typeclass method");
                return NULL;
            }
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
    method->n_params = n_params;
    method->return_type = return_type;
    return method;
}

/* Phase 17: (throw! msg) - sugar for (throw msg).
 * Provides a bang-style helper so callers can write (throw! "oops") without
 * needing to construct an Error struct explicitly. */
static Expr *elab_throw_bang(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(throw! msg) requires exactly one argument");
        return NULL;
    }
    Expr *payload = elab_form(e, call->as.list.items[1]);
    if (!payload) return NULL;
    Expr *out = expr_new(e->arena, EX_THROW, TYPE_NIL, call->span);
    out->as.throw_.payload = payload;
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

    /* Phase HKT (v2): reserved names — not yet implemented */
    if (name == e->sym_hkt_Functor || name == e->sym_hkt_Applicative ||
        name == e->sym_hkt_Monad   || name == e->sym_hkt_Traversable ||
        name == e->sym_hkt_Foldable) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "'%s' is reserved for the higher-kinded typeclass system (not yet implemented)",
                  name->name);
        return NULL;
    }

    /* Check if already defined */
    TypeClass *existing = typeclass_env_lookup_typeclass(&e->typeclass_env, name);
    if (existing) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "typeclass '%s' is already defined", name->name);
        return NULL;
    }
    
    /* Parse type parameters (optional) */
    const Symbol **type_params = NULL;
    uint8_t n_type_params = 0;
    uint32_t methods_start = 2;
    
    if (call->as.list.len >= 3) {
        Form *params_form = call->as.list.items[2];
        if (params_form->tag == F_VEC) {
            n_type_params = params_form->as.list.len;
            if (n_type_params > 0) {
                type_params = (const Symbol **)arena_alloc(e->arena, 
                    n_type_params * sizeof(const Symbol *));
                for (uint8_t i = 0; i < n_type_params; i++) {
                    Form *p = params_form->as.list.items[i];
                    /* Phase HKT (v2): detect kind annotation [f : * -> *] written as
                     * a vector [f kind] inside the type-params vector.  Reject with a
                     * clear "not yet supported" diagnostic rather than a confusing one. */
                    if (p->tag == F_VEC) {
                        diag_emit(DIAG_ERROR, p->span,
                                  "kind annotations in defclass type parameters are not yet supported (Phase HKT)");
                        return NULL;
                    }
                    if (p->tag != F_SYM) {
                        diag_emit(DIAG_ERROR, p->span,
                                  "type parameter must be a symbol");
                        return NULL;
                    }
                    type_params[i] = p->as.sym;
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
    
    tc->type_params = type_params;
    tc->n_type_params = n_type_params;
    tc->methods = methods;
    tc->n_methods = n_methods;
    
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
    uint8_t n_type_args = 0;
    uint32_t impls_start = 2;
    
    if (call->as.list.len >= 3) {
        Form *args_form = call->as.list.items[2];
        if (args_form->tag == F_VEC) {
            n_type_args = args_form->as.list.len;
            if (n_type_args > 0) {
                type_args = (Type *)arena_alloc(e->arena, n_type_args * sizeof(Type));
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
                        } else if (kw->len == 8 && memcmp(kw->name, "ptr<void>", 8) == 0) {
                            type_args[i] = TYPE_PTR_VOID;
                        } else {
                            /* Type variable reference - default to int for now */
                            type_args[i] = TYPE_INT;
                        }
                    } else {
                        diag_emit(DIAG_ERROR, arg->span,
                                  "unsupported type argument in definstance");
                        return NULL;
                    }
                }
            }
            impls_start = 3;
        }
    }
    
    /* Validate type argument count matches typeclass parameters */
    if (n_type_args != tc->n_type_params) {
        diag_emit(DIAG_ERROR, call->span,
                  "definstance: expected %d type arguments for '%s', got %d",
                  tc->n_type_params, tc_name->name, n_type_args);
        return NULL;
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
        char method_name[128];
        
        /* Sanitize method name for C identifier (replace invalid chars with _) */
        char sanitized_method_name[64];
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
        char type_suffix[64] = "";
        for (uint8_t j = 0; j < n_type_args; j++) {
            if (j == 0) {
                strcat(type_suffix, "_");
            }
            switch (type_args[j].kind) {
                case TY_INT: strcat(type_suffix, "int"); break;
                case TY_BOOL: strcat(type_suffix, "bool"); break;
                case TY_CSTR: strcat(type_suffix, "cstr"); break;
                case TY_NIL: strcat(type_suffix, "nil"); break;
                case TY_PTR_VOID: strcat(type_suffix, "ptr_void"); break;
                default: strcat(type_suffix, "T"); break;
            }
        }
        snprintf(method_name, sizeof(method_name), "__inst_%s_%s%s",
                 tc_name->name, sanitized_method_name, type_suffix);
        
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
                    
                    if (p->tag == F_SYM) {
                        /* Simple parameter name */
                        method_params[j] = binding_new(e, p->as.sym, param_type, false, false, p->span);
                        method_param_types[j] = param_type;
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
                            method_params[j] = binding_new(e, name_f->as.sym, param_type, false, false, p->span);
                            method_param_types[j] = param_type;
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
        
        Expr *method_body = e_nil(e, impl_form->span);
        uint32_t n_body = impl_form->as.list.len - impl_body_start;
        if (n_body > 0) {
            if (n_body == 1) {
                method_body = elab_form(e, impl_form->as.list.items[impl_body_start]);
            } else {
                Expr **items = (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
                for (uint32_t k = 0; k < n_body; k++) {
                    items[k] = elab_form(e, impl_form->as.list.items[impl_body_start + k]);
                    if (!items[k]) { e->scope = method_scope.parent; scope_free(&method_scope); return NULL; }
                }
                method_body = expr_new(e->arena, EX_DO, items[n_body - 1]->type, impl_form->span);
                method_body->as.do_.items = items;
                method_body->as.do_.n = n_body;
            }
        }
        
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
    inst->method_impls = method_impls;
    inst->n_method_impls = tc->n_methods;
    
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
    
    /* For v1: Find a method implementation that matches */
    /* We search through all instances to find one with a matching method */
    /* This is a simplified approach - proper resolution would use type inference */
    
    FnDef *best_method = NULL;
    
    for (TypeClassInstance *inst = e->typeclass_env.instances; inst != NULL; inst = inst->next) {
        for (uint8_t i = 0; i < inst->typeclass->n_methods; i++) {
            const TypeClassMethod *method = &inst->typeclass->methods[i];
            /* Check if method name matches (case-sensitive) */
            if (method->name->len == method_name_len &&
                memcmp(method->name->name, method_name, method_name_len) == 0) {
                /* Found a matching method */
                best_method = inst->method_impls[i];
                break;
            }
        }
        if (best_method) break;
    }
    
    if (!best_method) {
        /* No matching method found */
        diag_emit(DIAG_ERROR, call->span,
                  "no typeclass method found for '%.*s'",
                  method_name_len, method_name);
        return NULL;
    }
    
    /* Elaborate the object and arguments */
    Expr *obj = elab_form(e, call->as.list.items[1]);
    if (!obj) return NULL;
    
    uint32_t n_args = call->as.list.len - 2;
    Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[2 + i]);
        if (!args[i]) return NULL;
    }
    
    /* For v1: Generate a direct call to the method implementation */
    /* The method function name is stored in best_method->binding->name */
    /* We need to create a call expression to that function */
    
    /* Allocate arguments array with obj prepended */
    Expr **call_args = (Expr **)arena_alloc(e->arena, (n_args + 1) * sizeof(Expr *));
    call_args[0] = obj;
    for (uint32_t i = 0; i < n_args; i++) {
        call_args[i + 1] = args[i];
    }
    
    /* Create a call to the method function */
    /* The result type is the return type of the method */
    Type result_type = best_method->param_types[0]; /* First param type is the instance type, result is last */
    /* For v1, use the body type of the method */
    result_type = best_method->body->type;
    
    Expr *out = expr_new(e->arena, EX_CALL, result_type, call->span);
    out->as.call_.fn_binding = best_method->binding;
    out->as.call_.args = call_args;
    out->as.call_.n_args = n_args + 1;
    return out;
}


Expr *elaborate_program(Arena *arena, SymbolTable *st,
                        Form *const *forms, uint32_t nforms) {
    Elab e;
    elab_init_state(&e, arena, st);
    builtins_init(st);

    Expr **items = (nforms == 0) ? NULL :
        (Expr **)arena_alloc(arena, nforms * sizeof(Expr *));
    int rc = 0;

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
                        Form *name_f = f->as.list.items[1];
                        if (name_f->tag == F_SYM) {
                            /* Parse return type annotation if present */
                            TypeKind return_kind = TY_INT; /* default */
                            if (f->as.list.len >= 4) {
                                Form *ret_f = f->as.list.items[3];
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
                }
            }
        }
    }

    /* Pass 2: Elaborate all forms */
    for (uint32_t i = 0; i < nforms; i++) {
        items[i] = elab_form(&e, forms[i]);
        if (!items[i]) { rc = -1; /* keep going to surface more diagnostics */ }
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

    scope_free(&e.global);
    if (rc != 0) return NULL;

    Expr *prog = expr_new(arena, EX_PROGRAM, TYPE_NIL,
                          nforms > 0 ? forms[0]->span : (Span){0,0,0,0,0,0});
    prog->as.program.items = items;
    prog->as.program.n = nforms;
    return prog;
}
