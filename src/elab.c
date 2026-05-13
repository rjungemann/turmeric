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

/* Phase U5: External declarations for global unsafe linting configuration */
extern uint32_t g_unsafe_max_lines;
extern bool g_unsafe_warn_nested;
extern bool g_unsafe_require_safety;
extern bool g_unsafe_stats_enabled;
extern bool g_lint_unsafe_enabled;
extern uint32_t g_unsafe_block_count;
extern uint32_t g_unsafe_total_lines;

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
            case EX_GET_FIELD:
                stack[sp++] = cur->as.get_field_.struct_expr;
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

/* Phase M2: A loaded module's exported bindings. */
typedef struct ElabModule {
    const Symbol *name;          /* module name (interned) */
    Binding     **exports;       /* exported bindings (each has .name for lookup) */
    uint32_t      n_exports;
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
    /* Phase B2: Cloneable continuations */
    const Symbol *sym_cloneable_reset;   /* cloneable-reset */
    const Symbol *sym_cloneable_shift;   /* cloneable-shift */
    const Symbol *sym_call_cc_star;       /* call/cc* (sugar for cloneable call/cc) */
    /* Phase 19: Algebraic effects */
    const Symbol *sym_defeffect;  /* defeffect */
    const Symbol *sym_perform;    /* perform */
    const Symbol *sym_handle;     /* handle */
    const Symbol *sym_try_with;   /* try-with (sugar for handle) */
    const Symbol *sym_resume;     /* resume */
    const Symbol *sym_discontinue;/* discontinue */
    const Symbol *sym_k;          /* continuation parameter name k */
    const Symbol *sym_effect_unsafe; /* built-in effect name: Unsafe */
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
    /* Phase R5: no-unwind attribute */
    const Symbol *sym_no_unwind_attr;
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
    /* Phase M0: Module system */
    const Symbol *sym_defmodule;  /* defmodule */
    const Symbol *sym_export;     /* export */
    const Symbol *sym_import;     /* import */
    const Symbol *kw_as;          /* :as */
    const Symbol *kw_refer;       /* :refer */
    bool has_defmodule;           /* whether defmodule has been seen in this file */
    /* Phase M1: Module namespace system */
    const Symbol    *current_module_name; /* name of module being elaborated, or NULL */
    const DefModule *current_module;      /* DefModule being elaborated, or NULL */
    /* Phase M2: Module registry */
    const char      *module_base_dir;     /* base dir for resolving module file paths */
    struct ElabModule *loaded_modules;    /* registry of loaded modules */
    uint32_t         n_loaded_modules;
    uint32_t         cap_loaded_modules;
    uint16_t         next_import_file_id; /* file_id counter for imported source files */
    /* Phase M3: Separate compilation — skip inlining imported modules */
    bool             separate_compilation;
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
            case EX_PANIC:
                stack[sp++] = cur->as.panic_.payload;
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
    /* Phase B2: Cloneable continuations */
    e->sym_cloneable_reset = intern_cstr(st, "cloneable-reset");
    e->sym_cloneable_shift = intern_cstr(st, "cloneable-shift");
    e->sym_call_cc_star = intern_cstr(st, "call/cc*");
    /* Phase 19: Algebraic effects */
    e->sym_defeffect = intern_cstr(st, "defeffect");
    e->sym_perform = intern_cstr(st, "perform");
    e->sym_handle = intern_cstr(st, "handle");
    e->sym_try_with = intern_cstr(st, "try-with");
    e->sym_resume = intern_cstr(st, "resume");
    e->sym_discontinue = intern_cstr(st, "discontinue");
    e->sym_k = intern_cstr(st, "k");
    e->sym_cont_pred = intern_cstr(st, "cont?");
    e->sym_effect_unsafe = intern_cstr(st, EFFECT_NAME_UNSAFE);
    effect_env_register_builtin_unsafe(e->effect_env, e->arena, e->sym_effect_unsafe);
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
    /* Phase HKT H5: defkind */
    e->sym_defkind = intern_cstr(st, "defkind");
    /* Phase HKT-P2: defrec */
    e->sym_defrec = intern_cstr(st, "defrec");
    e->sym_deftype = intern_cstr(st, "deftype");
    /* Phase HKT-P1: type-app */
    e->sym_type_app = intern_cstr(st, "type-app");
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
    e->sym_panic_payload_type = intern_cstr(st, "panic-payload-type");
    /* Phase R5: no-unwind attribute */
    e->sym_no_unwind_attr = intern_cstr(st, "#no-unwind");
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
    /* Macro storage */
    e->macros = NULL;
    e->n_macros = 0;
    e->cap_macros = 0;
    /* Phase M0: Module system */
    e->sym_defmodule = intern_cstr(st, "defmodule");
    e->sym_export = intern_cstr(st, "export");
    e->sym_import = intern_cstr(st, "import");
    e->kw_as = intern_cstr(st, "as");
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
        case F_LIST:
        case F_VEC:
        case F_MAP:
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
        case F_MAP: {
            uint32_t n_in = f->as.list.len;
            Form **items = (Form **)arena_alloc(env->elab->arena, n_in * sizeof(Form *));
            for (uint32_t i = 0; i < n_in; i++) items[i] = ct_eval_quasiquote(env, f->as.list.items[i]);
            return (f->tag == F_MAP)
                ? form_map(env->elab->arena, f->span, items, n_in)
                : form_vec(env->elab->arena, f->span, items, n_in);
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
                    ct_symbol_name(head, "list")) {
                    return true;
                }
            }
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (form_contains_ct_builtins(f->as.list.items[i])) return true;
            }
            return false;
        case F_VEC:
        case F_MAP:
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
                     ((args[0]->tag == F_LIST || args[0]->tag == F_VEC || args[0]->tag == F_MAP) && args[0]->as.list.len == 0);
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
        case F_SYM: {
            CtValue v;
            if (ct_env_lookup(env, f->as.sym, &v)) return v;
            return ct_value_form(f);
        }
        case F_LIST:
            return ct_eval_call(env, f);
        case F_VEC:
        case F_MAP: {
            Form **items = (f->as.list.len == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, f->as.list.len * sizeof(Form *));
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                CtValue v = ct_eval_form(env, f->as.list.items[i]);
                items[i] = ct_value_to_form(env, v, f->as.list.items[i]->span);
                if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
            }
            return ct_value_form((f->tag == F_MAP)
                ? form_map(env->elab->arena, f->span, items, f->as.list.len)
                : form_vec(env->elab->arena, f->span, items, f->as.list.len));
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
    if (macro->is_variadic) {
        if (n_args < macro->n_params) {
            diag_emit(DIAG_ERROR, macro->span,
                      "macro '%s' expects at least %u arguments, got %u",
                      macro->name->name, macro->n_params, n_args);
            return NULL;
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
        if (!templ) return NULL;
        if (!form_contains_ct_builtins(templ)) return templ;
        return elab_eval_macro_form(e, templ);
    }
    if (n_args != macro->n_params) {
        diag_emit(DIAG_ERROR, macro->span,
                  "macro '%s' expects %u arguments, got %u",
                  macro->name->name, macro->n_params, n_args);
        return NULL;
    }
    
    /* Substitute parameters in the macro body */
    Form *templ = substitute_params(e, macro->body, macro, args);
    if (!templ) return NULL;
    if (!form_contains_ct_builtins(templ)) return templ;
    return elab_eval_macro_form(e, templ);
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
    b->no_unwind = false;  /* Phase R5: #[no-unwind] attribute */
    b->is_exported = false;
    b->defining_module_name = e->current_module_name;
    return b;
}

/* Forward declarations. */
static Binding *elab_lookup_sym(Elab *e, const Symbol *sym, Span span, bool *had_error); /* Phase M1 */
static ElabModule *elab_load_module(Elab *e, const Symbol *name, Span span);             /* Phase M2 */
static Expr *elab_form(Elab *e, Form *f);
static Expr *elab_unsafe(Elab *e, const Form *call);
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
    /* For v1, we don't do the compile-time size check yet */
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

static int form_is_symbol_named(const Form *f, const Symbol *name) {
    return f && f->tag == F_SYM && f->as.sym == name;
}

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
                } else if (kw->len == 1 && memcmp(kw->name, "!", 1) == 0) {
                    catch_type = TY_NEVER;
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

/* Phase B2: Cloneable continuations */

/* (cloneable-reset body) - Establish a continuation boundary with cloneable captures.
 * Similar to reset, but all captured values must implement Clone.
 * The body can use cloneable-shift to capture a cloneable continuation. */
static Expr *elab_cloneable_reset(Elab *e, const Form *call) {
    if (call->as.list.len != 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "(cloneable-reset body) requires exactly one argument");
        return NULL;
    }
    Expr *body = elab_form(e, call->as.list.items[1]);
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
    
    /* For now, emit an error that this is not yet implemented. */
    diag_emit(DIAG_ERROR, call->span,
              "call/cc* sugar is not yet implemented (Phase B2)");
    return NULL;
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
    /* Phase R5: Check for #[no-unwind] attribute before name */
    uint32_t name_idx = 1;  /* index of name in items (after 'defn') */
    bool no_unwind = false;
    Form *name_f = call->as.list.items[name_idx];
    if (name_f->tag == F_SYM && name_f->as.sym == e->sym_no_unwind_attr) {
        no_unwind = true;
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

            /* Phase HKT H4: Kind variable annotation — type-level only, erased at runtime.
             * A kind variable like `^f` declares that `f` ranges over type constructors
             * of kind '* -> *'.  It creates no runtime parameter; it is used as a
             * kind annotation on the function and may be followed by a ^Typeclass f
             * constraint (which adds a regular typeclass constraint handled below). */
            if (tc_name_len > 0 && tc_name_str[0] >= 'a' && tc_name_str[0] <= 'z') {
                /* Kind variable — silently skip; no runtime parameter is created. */
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
            } else if (kw->len == 1 && memcmp(kw->name, "!", 1) == 0) {
                param_kinds[n_params - 1] = TY_NEVER;
                params[n_params - 1]->type = TYPE_NEVER;
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
    /* body_start is the index of the first element after params (could be return type or body) */
    TypeKind return_kind = TY_NIL;
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
            } else if (kw->len == 1 && memcmp(kw->name, "!", 1) == 0) {
                return_kind = TY_NEVER;
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
    /* Phase R5: Store #[no-unwind] attribute on the binding */
    b->no_unwind = no_unwind;

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
/* Phase M0: Module system */
static Expr *elab_defmodule(Elab *e, const Form *call);
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
/* Phase HKT H5: kind aliases */
static Expr *elab_defkind(Elab *e, const Form *call);
static Expr *elab_defrec(Elab *e, const Form *call);  /* Phase HKT-P2 */
static Expr *elab_deftype(Elab *e, const Form *call);  /* Phase HKT-P2 */
static Expr *elab_type_app(Elab *e, const Form *call);  /* Phase HKT-P1 */
/* Phase 17: throw! sugar */
static Expr *elab_throw_bang(Elab *e, const Form *call);
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
    /* Phase B2: Cloneable continuations */
    if (name == e->sym_cloneable_reset)  return elab_cloneable_reset(e, call);
    if (name == e->sym_cloneable_shift)  return elab_cloneable_shift(e, call);
    if (name == e->sym_call_cc_star)      return elab_call_cc_star(e, call);
    /* Phase 19: Algebraic effects */
    if (name == e->sym_defeffect) return elab_defeffect(e, call);
    if (name == e->sym_perform)   return elab_perform(e, call);
    if (name == e->sym_handle)    return elab_handle(e, call);
    if (name == e->sym_try_with)  return elab_try_with(e, call);
    if (name == e->sym_resume)    return elab_resume(e, call);
    if (name == e->sym_discontinue) return elab_discontinue(e, call);
    if (name == e->sym_cont_pred)   return elab_cont_pred(e, call);
    /* Phase 10: GC */
    if (name == e->sym_gc_force)    return elab_gc_force(e, call);
    if (name == e->sym_gc_enable)   return elab_gc_enable(e, call);
    if (name == e->sym_gc_disable)  return elab_gc_disable(e, call);
    /* Phase M0: Module system */
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
    /* Phase 11: defstruct */
    if (name == e->sym_defstruct) return elab_defstruct(e, call);
    if (name == e->sym_make_struct) return elab_make_struct(e, call);
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
    /* Phase HKT-P1: type-level application */
    if (name == e->sym_type_app) return elab_type_app(e, call);
    /* Phase R2: Panic */
    if (name == e->sym_panic) return elab_panic(e, call);
    if (name == e->sym_panic_with) return elab_panic_with(e, call);
    if (name == e->sym_catch_unwind) return elab_catch_unwind(e, call);
    if (name == e->sym_catch_panic_of) return elab_catch_panic_of(e, call);
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
    /* Phase R1: ? operator (reserved, not yet implemented) */
    if (name == e->sym_question) {
        diag_emit(DIAG_ERROR, call->span,
            "? operator is not yet implemented (Phase R1)");
        return NULL;
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
    if (name == e->sym_extern_c) return elab_extern_c(e, call);

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
        
        /* Recursively elaborate the expanded form */
        Expr *out = elab_form(e, expanded);
        e->macro_expand_depth--;
        return out;
    }

    /* Phase 2: Check if it's a user-defined function call.
     * M1: Use elab_lookup_sym for visibility + qualified name resolution. */
    bool fn_qual_err = false;
    Binding *fn_binding = elab_lookup_sym(e, name, head->span, &fn_qual_err);
    if (!fn_binding && fn_qual_err) return NULL;
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
        out->as.call_.fn_expr = NULL;
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
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_STRUCT) {
            /* Phase HKT H3: Allow passing an HKT container (TY_STRUCT) where int64_t
             * is expected.  HKT type constructor values are opaque int64_t at runtime. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_APP) {
            /* Phase HKT §3: Allow passing a partially-applied type (TY_APP) where int64_t
             * is expected.  Partial type application values are opaque int64_t at runtime. */
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
        diag_emit(DIAG_ERROR, import_span,
                  "module '%s' not found (looked for '%s')", name->name, path_buf);
        slot->is_loading = false;
        return NULL;
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
        if (ex->kind == EX_DEFMODULE && !e->separate_compilation) {
            elab_register_file_def(e, ex);
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
            const Symbol **syms = (n == 0) ? NULL :
                (const Symbol **)arena_alloc(e->arena, n * sizeof(Symbol *));
            for (uint32_t j = 0; j < n; j++) {
                Form *sf = refer_f->as.list.items[j];
                if (sf->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, sf->span, ":refer list must contain only symbols");
                    return false;
                }
                syms[j] = sf->as.sym;
            }
            out->refer_syms = syms;
            out->n_refer = n;
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
            /* Parse (export sym1 sym2 ...) */
            for (uint32_t j = 1; j < item->as.list.len; j++) {
                Form *sf = item->as.list.items[j];
                if (sf->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, sf->span, "export list must contain only symbols");
                    free(exports); free(imports);
                    return NULL;
                }
                if (n_exports >= cap_exports) {
                    cap_exports = cap_exports ? cap_exports * 2 : 4;
                    exports = (const Symbol **)realloc(exports, cap_exports * sizeof(Symbol *));
                }
                exports[n_exports++] = sf->as.sym;
            }
            i++;
        } else if (head->as.sym == e->sym_import) {
            /* Parse (import module-name [:as alias] [:refer [syms...]]) */
            if (n_imports >= cap_imports) {
                cap_imports = cap_imports ? cap_imports * 2 : 4;
                imports = (ImportSpec *)realloc(imports, cap_imports * sizeof(ImportSpec));
            }
            if (!parse_import_spec(e, item, &imports[n_imports])) {
                free(exports); free(imports);
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
        /* :refer — add each referred symbol directly to the current scope. */
        for (uint32_t k = 0; k < imp->n_refer; k++) {
            const Symbol *ref_sym = imp->refer_syms[k];
            Binding *ref_b = NULL;
            for (uint32_t m = 0; m < loaded->n_exports; m++) {
                if (loaded->exports[m]->name == ref_sym) {
                    ref_b = loaded->exports[m];
                    break;
                }
            }
            if (!ref_b) {
                diag_emit(DIAG_ERROR, imp->span,
                          "symbol '%s' is not exported from module '%s'",
                          ref_sym->name, imp->module_name->name);
                e->current_module_name = NULL;
                e->current_module = NULL;
                return NULL;
            }
            scope_add(&e->global, ref_b);
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

    /* M1: Mark exported bindings and validate they exist in this module */
    for (uint32_t j = 0; j < mod->n_exports; j++) {
        Binding *exp_b = scope_lookup(&e->global, mod->exports[j]);
        if (!exp_b || exp_b->defining_module_name != mod->name) {
            diag_emit(DIAG_ERROR, call->span,
                      "exported symbol '%s' is not defined in this module",
                      mod->exports[j]->name);
            e->current_module_name = NULL;
            e->current_module = NULL;
            return NULL;
        }
        exp_b->is_exported = true;
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
                diag_emit(DIAG_ERROR, span,
                          "symbol '%s' is not exported from module '%s'",
                          sym_key->name, imp->module_name->name);
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
                diag_emit(DIAG_ERROR, span,
                          "symbol '%s' is not exported from module '%s'",
                          sym_key->name, mn->name);
                *had_error = true;
                return NULL;
            }
        }
    }

    return NULL; /* Not a recognised qualified name; caller handles "unbound" */
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
    /* Phase 16 v2: :fn field type — function pointer (may carry #{...} effect-row annotation) */
    if (tlen == 2  && memcmp(tname, "fn",    2) == 0) { *out_kind = TY_FN;       return; }

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
        /* Phase 16 v2: :fn fields are stored as int64_t at the C level, so they are
         * trivially copyable (function pointer stored as an integer value). */
        case TY_FN:
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

    /* Allocate StructDef and field array */
    StructDef *def = (StructDef *)arena_alloc(e->arena, sizeof(StructDef));
    def->name = name->name;
    def->n_fields = actual_n_fields;
    def->fields = (StructField *)arena_alloc(e->arena, actual_n_fields * sizeof(StructField));
    def->is_copy = is_copy;
    def->needs_drop_glue = false;
    /* Phase HKT-P4: record the file that defined this struct. */
    def->origin_file_id = call->span.file_id;

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
                diag_emit(DIAG_ERROR, field_type_form->span,
                          "defstruct field '%s' has unrecognized type :%s",
                          field_name_form->as.sym->name, tname);
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
     * Values captured in async blocks must be Send (can be moved to fiber context). */
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

/* Phase R2: (panic msg) — print msg to stderr, then abort.
 * msg must be of type :cstr (or a string literal).
 * Return type is TYPE_NEVER (diverging; caller never observes a value). */
static Expr *elab_panic(Elab *e, const Form *call) {
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

    /* Phase HKT-P2: In v1, we accept the body but don't fully elaborate it.
     * We just need to validate it exists and is a valid form.
     * For now, we'll store it as a TY_REC with NULL body.
     * Future work: fully elaborate the body and validate guarded recursion. */
    (void)body_form;  /* Body is accepted but not processed in v1 */

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
    /* v1: body not yet evaluated - we'd need a full type elaborator to handle it */
    rec_type.as.rec.body = NULL;

    /* Register in global scope */
    Binding *b = binding_new(e, name, rec_type, false, true, name_form->span);
    scope_add(&e->global, b);

    /* deftype has no runtime effect */
    return e_nil(e, call->span);
}
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
    if (!fn_binding) {
        diag_emit(DIAG_ERROR, fn_form->span,
                  "type-app: unknown type constructor '%s'", fn_form->as.sym->name);
        return NULL;
    }
    Type fn_type = fn_binding->type;

    /* Parse type argument A (arg 2) - can be a symbol, keyword, or nested type-app */
    Form *arg_form = call->as.list.items[2];
    Type arg_type;
    
    if (arg_form->tag == F_SYM || arg_form->tag == F_KEYWORD) {
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
                diag_emit(DIAG_ERROR, arg_form->span,
                          "type-app: unknown type '%s'", arg_sym->name);
                return NULL;
            }
            arg_type = arg_binding->type;
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
                            /* Phase HKT H3: Unknown name — treat as an opaque type constructor.
                             * TY_STRUCT without a StructDef causes codegen to emit 'void *' for
                             * all parameters that inherit this type, which is the correct C type
                             * for containers represented as heap pointers (option, vec, etc.).
                             * Track the symbol name so method name mangling can use it. */
                            memset(&type_args[i], 0, sizeof(type_args[i]));
                            type_args[i].kind = TY_STRUCT;
                            type_args[i].copy_kind = CK_MOVE;
                            type_args[i].as.struct_.def = NULL;
                            type_arg_syms[i] = kw;
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

    /* Phase HKT-P4: Orphan instance check (advisory in v1 — single-file
     * compilation means all definitions share the same file_id, so this check
     * never fires in v1 unless a module system is added later).
     *
     * Rule: an instance is "orphan" when NEITHER the typeclass NOR any
     * struct-type type argument was defined in the current compilation unit.
     * In Rust terms: you may only define Foo<Bar> if you own Foo or Bar.
     *
     * Promote to DIAG_ERROR once P19-6 (module system) lands. */
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
            diag_emit_with_code(DIAG_WARNING, call->span,
                      TUR_E0013_ORPHAN_INSTANCE,
                      "orphan instance: '%s' is defined in a different compilation "
                      "unit and none of the type arguments are defined here; "
                      "this may conflict when a module system is added (see P19-6)",
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
                    if (fkind == TY_REF || fkind == TY_RC || fkind == TY_WEAK) {
                        field_type.kind = fkind;
                        field_type.as.ref.inner = finner;
                    } else {
                        field_type = type_from_kind(fkind);
                    }
                    Expr *out = expr_new(e->arena, EX_GET_FIELD, field_type, call->span);
                    out->as.get_field_.struct_expr = obj;
                    out->as.get_field_.field_idx = i;
                    out->as.get_field_.def = def;
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

    /* Phase HKT H2: Type-based instance lookup.
     * Build a TypeClassDispatchKey from the obj type, then use
     * typeclass_env_lookup_instance_by_key for a two-level search.
     * Fall back to name-only search if the type-based lookup yields nothing
     * (e.g. TY_UNKNOWN during forward-reference elaboration). */
    FnDef *best_method = NULL;
    /* Phase H §1: Track the selected instance so we can build an EX_DICT node. */
    TypeClassInstance *best_inst = NULL;

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

    /* obj was already elaborated above for dispatch; elaborate the remaining args. */
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
    if (dict_expr) {
        /* Dictionary dispatch: indirect call through the vtable field. */
        out->as.call_.fn_binding = NULL;
        out->as.call_.fn_expr    = dict_expr;
    } else {
        /* Fallback: direct call (no instance resolved — should not happen). */
        out->as.call_.fn_binding = best_method->binding;
        out->as.call_.fn_expr    = NULL;
    }
    out->as.call_.args    = call_args;
    out->as.call_.n_args  = n_args + 1;
    out->as.call_.dict_arg = dict_expr;  /* annotation for downstream passes */
    return out;
}


Expr *elaborate_program(Arena *arena, SymbolTable *st,
                        Form *const *forms, uint32_t nforms,
                        uint32_t stdlib_prefix,
                        const char *module_base_dir,
                        bool separate_compilation) {
    Elab e;
    elab_init_state(&e, arena, st);
    e.module_base_dir = module_base_dir ? module_base_dir : ".";
    e.separate_compilation = separate_compilation;
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
                        /* Phase R5: skip optional #[no-unwind] attribute */
                        uint32_t name_idx = 1;
                        if (f->as.list.items[1]->tag == F_SYM &&
                            f->as.list.items[1]->as.sym == e.sym_no_unwind_attr) {
                            name_idx = 2;
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
                    rc = -1;
                }
                break; /* only check the first occurrence */
            }
        }
    }
    if (rc != 0) {
        scope_free(&e.global);
        free(e.struct_defs);
        free(e.handled_effect_names);
        free(e.macros);
        return NULL;
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
    free(e.struct_defs);
    free(e.handled_effect_names);
    free(e.macros);
    free(e.loaded_modules); /* Phase M2 */
    if (rc != 0) return NULL;

    Expr *prog = expr_new(arena, EX_PROGRAM, TYPE_NIL,
                          nforms > 0 ? forms[0]->span : (Span){0,0,0,0,0,0});
    prog->as.program.items = items;
    prog->as.program.n = nforms;
    return prog;
}
