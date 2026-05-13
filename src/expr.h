#ifndef TUR_EXPR_H
#define TUR_EXPR_H

#include <stdint.h>
#include <stdbool.h>

#include "arena.h"
#include "buf.h"
#include "forms.h"   /* Span */
#include "symbols.h"
#include "types.h"
#include "lifetimes.h"  /* Phase 13: Lifetime annotations */
#include "typeclass.h"  /* Phase 15: Typeclass constraints */
#include "effect.h"    /* Phase 19: Algebraic effects */

/* Forward declarations. */
typedef struct Expr        Expr;
typedef struct Binding     Binding;
typedef struct BuiltinSpec BuiltinSpec;
typedef struct FnDef       FnDef;      /* Phase 2: function definition */
typedef struct ExternC     ExternC;    /* Phase 2: extern C declaration */
typedef struct InlineC     InlineC;    /* Phase 2: inline C block */

/* A Binding is the resolved target of a `let`/`def`/`defn` name introduction.
 * Bindings are owned by the elaborator and live in the arena. */
struct Binding {
    const Symbol *name;
    Type          type;
    bool          is_mut;
    bool          is_global;     /* top-level def vs. local let */
    uint32_t      id;            /* unique within the program */
    Span          span;
    /* Phase 3: For closure bindings, this points to the thunk function binding */
    struct Binding *closure_fn_binding;
    /* Phase 5: Move semantics - whether this ref binding has been moved */
    bool          is_moved;
    /* Phase 11: span of first move for note chaining diagnostics */
    Span          moved_at;
    /* Phase R5: #[no-unwind] attribute on defn */
    bool          no_unwind;
    /* Phase T25: true if this binding is an algebraic-effect continuation (k in handle cases).
     * Used to detect continuation escape into async blocks at compile time. */
    bool          is_continuation;
    /* Phase M1: Module visibility */
    bool          is_exported;          /* listed in module's (export ...) */
    const Symbol *defining_module_name; /* owning module's name, or NULL for top-level */
    /* Phase M6: explicit C symbol name from ^:export-as attribute, or NULL */
    const char   *c_export_name;
};

typedef enum ExprKind {
    EX_NIL_LIT = 1,
    EX_BOOL_LIT,
    EX_INT_LIT,
    EX_FLOAT_LIT,       /* Phase 1: Float literals */
    EX_CSTR_LIT,
    EX_VAR,
    EX_LET,
    EX_IF,
    EX_DO,
    EX_WHILE,
    EX_SET,
    EX_DEF,
    EX_BUILTIN,
    EX_FN,              /* Phase 2: anonymous function (no capture) */
    EX_CALL,            /* Phase 2: function call (f a b c) */
    EX_FN_DEF,          /* Phase 2: top-level function definition (defn) */
    EX_EXTERN_C,        /* Phase 2: extern C declaration */
    EX_INLINE_C,        /* Phase 2: inline C block */
    EX_CLOSURE,         /* Phase 3: closure with captured env */
    EX_RETURN,          /* Phase 3/4: early return with defer firing */
    EX_DEFER,           /* Phase 4: defer expression */
    /* Phase 5: ref<T> with move semantics */
    EX_REF,             /* (ref expr) - owning reference constructor */
    EX_DEREF,           /* (@ expr) - dereference ref<T> or ptr<T> */
    /* Phase 9: rc<T> + weak<T> reference counting */
    EX_RC_OF,          /* (rc/of x) - create new rc<T> */
    EX_RC_CLONE,       /* (rc/clone r) - increment strong count */
    EX_RC_DROP,        /* (rc/drop r) - decrement strong count */
    /* Note: (@ r) for rc<T> reuses EX_DEREF */
    EX_RC_PTR,         /* (rc->ptr r) - borrow ptr<T> from rc<T> */
    EX_RC_COUNT,       /* (rc/strong-count r) - get strong count */
    EX_RC_FROM_REF,    /* (rc/from-ref r) - move ref<T> into rc<T> */
    EX_REF_FROM_RC,    /* (ref/from-rc r) - extract unique ref<T> from rc<T> */
    EX_WEAK,           /* (weak r) - create weak<T> from rc<T> */
    EX_WEAK_UPGRADE,   /* (upgrade w) - upgrade weak<T> to option<rc<T>> */
    EX_WEAK_PRED,      /* (weak? w) - check if w is weak<T> */
    EX_REF_PRED,       /* (ref? x) - check if x is ref<T> */
    /* Phase 12: Borrow traits */
    EX_BORROW_IMMUT,   /* (& expr) - create immutable borrow */
    EX_BORROW_MUT,     /* (&mut expr) - create mutable borrow */
    EX_SET_DEREF,      /* (set! (@ r) v) - mutation through mutable borrow */
    /* Phase 15: Typeclasses */
    EX_TYPECLASS_DEF,   /* (defclass ...) - typeclass definition */
    EX_INSTANCE_DEF,   /* (definstance ...) - typeclass instance definition */
    /* Phase 17: Exceptions */
    EX_THROW,          /* (throw expr) - raise an exception */
    EX_TRY,            /* (try body (catch ...) (finally ...)) - try-catch-finally */
    /* Phase R2: Panic */
    EX_PANIC,          /* (panic msg) - print msg to stderr and abort */
    EX_PANIC_WITH,     /* (panic-with payload) - panic with typed payload */
    EX_CATCH_UNWIND,   /* (catch-unwind thunk) - catch any panic at boundary */
    EX_CATCH_PANIC_OF, /* (catch-panic-of Type thunk) - catch panic of specific type */
    EX_PANIC_PAYLOAD_TYPE,    /* (panic-payload-type p) - get type tag */
    EX_PANIC_PAYLOAD_VALUE,   /* (panic-payload-value p) - get value */
    EX_PANIC_PAYLOAD_FILE,    /* (panic-payload-file p) - get file */
    EX_PANIC_PAYLOAD_LINE,    /* (panic-payload-line p) - get line */
    EX_PANIC_PAYLOAD_DOWNS,   /* (panic-payload-downcast p Type) - downcast */
    /* Phase 18: Delimited continuations */
    EX_RESET,          /* (reset body) - establish continuation boundary */
    EX_SHIFT,          /* (shift k body) - capture continuation, pass to k */
    EX_SHIFT0,         /* (shift0 k body) - one-shot shift */
    /* Phase B2: Cloneable continuations */
    EX_CLONEABLE_RESET, /* (cloneable-reset body) - continuation boundary with cloneable captures */
    EX_CLONEABLE_SHIFT, /* (cloneable-shift k body) - capture cloneable continuation */
    /* Phase 19: Algebraic effects */
    EX_DEFECT,         /* (defeffect Name [params...] : result) - define an effect */
    EX_PERFORM,        /* (perform (EffectName args...)) - perform an effect */
    EX_HANDLE,         /* (handle expr cases...) - handle effects */
    EX_RESUME,         /* (resume k value) - resume continuation with value */
    EX_DISCONTINUE,    /* (discontinue k exception) - discontinue with exception */
    EX_CONT_PRED,      /* (cont? k) - check if continuation is unconsumed */
    /* Phase T21-F: async/await sugar */
    EX_ASYNC,          /* (async fn-expr) - run no-arg fn in thread; return Future (ptr<void>) */
    EX_AWAIT,          /* (await fut)     - block on Future; return int value */
    /* Phase 20: Software Transactional Memory */
    EX_STM,            /* (stm & body) - STM transaction block */
    EX_ATOMICALLY,     /* (atomically stm-block) - execute STM transaction atomically */
    EX_RETRY,          /* (retry) - retry transaction from within stm block */
    EX_CHECK,          /* (check cond) - abort transaction if cond is false */
    EX_OR_ELSE,        /* (or-else stm1 stm2) - try stm1, retry with stm2 if retry */
    EX_TVAR_NEW,       /* (TVar::new init) - create new TVar */
    EX_TVAR_READ,      /* (TVar::read tvar) - read TVar within stm block */
    EX_TVAR_WRITE,     /* (TVar::write tvar value) - write TVar within stm block */
    EX_TVAR_MODIFY,    /* (TVar::modify tvar fn) - modify TVar within stm block */
    EX_TVAR_SWAP,      /* (TVar::swap tvar new) - swap TVar value within stm block */
    EX_TVAR_CAS,       /* (TVar::cas tvar old new) - compare-and-swap within stm block */
    /* Phase H §1: dictionary passing */
    EX_DICT,           /* implicit dictionary argument — address of a typeclass instance singleton */
    /* Phase 11: Struct operations */
    EX_MAKE_STRUCT,    /* (make-struct Name v1 v2 ...) - struct literal */
    EX_GET_FIELD,      /* (.field s) - struct field access */
    EX_PROGRAM,
    /* Phase M0: Module system */
    EX_DEFMODULE,      /* (defmodule name [docstring] (export ...) (import ...) body...) */
    /* Phase N: Numeric type cast */
    EX_CAST,           /* (as TargetType expr) — explicit numeric coercion */
} ExprKind;

/* Phase 2: FnDef represents a function definition from defn or lifted fn. */
struct FnDef {
    Binding        *binding;     /* name binding */
    Binding       **params;      /* param bindings */
    uint8_t        n_params;
    Type          *param_types;  /* param types (for codegen) */
    Expr          *body;
    bool           is_variadic;  /* not yet supported in phase 2 */
    /* Phase 3: For closure thunks, store the closure info */
    struct Closure *closure;    /* NULL for non-closure functions */
    /* Phase 4: Future-proofing for v3 effects (effects-plan.md §6.10) - whether this
     * function may capture continuations. Always false in v0/v1. */
    bool           may_capture;
    /* Phase 13: Lifetime annotations */
    LifetimeContext lifetime_ctx;  /* Lifetime parameters and constraints for this function */
    /* Phase 15: Typeclass constraints */
    ConstraintSet  constraints;    /* Typeclass constraints for this function */
    /* Phase P19-3: Inferred effect row for this function.
     * NULL until the effect-row inference pass (P19-2) runs.
     * declared_effect_row lives in Type.as.fn.effect_row (the annotated row);
     * this field carries the pass-computed row so both can coexist. */
    EffectRow     *inferred_effect_row;
};

/* Phase 2: ExternC represents an (extern-c ...) declaration. */
struct ExternC {
    const Symbol  *c_name;       /* C identifier */
    Binding       *binding;     /* Turmeric binding (optional) */
    Type           return_type;
    Type          *param_types;
    uint8_t        n_params;
    bool           is_variadic;
};

/* Phase 2: InlineC represents an inline C block. ```c ... ``` */
struct InlineC {
    StrSlice       code;         /* the raw C code */
    Type           return_type; /* annotated : T */
    Binding      **captures;     /* captured bindings from enclosing scope */
    uint8_t        n_captures;
};

/* Phase 3: Closure represents a fn with captured environment. */
struct Closure {
    FnDef         *fn;           /* the function definition (modified with env param) */
    Binding      **captures;     /* captured bindings from enclosing scope */
    uint8_t        n_captures;
    const Symbol *env_name;     /* generated name for the env struct type */
};

typedef struct LetBinding {
    Binding *binding;
    Expr    *init;
} LetBinding;

/* Phase 17: Exception handling - Try-catch clause structure */
typedef struct TryCatchClause {
    const Symbol *var_name;    /* Name of the exception variable (e.g., 'e' in (catch [e] ...)) */
    Binding *binding;          /* Resolved binding for codegen/name resolution */
    TypeKind catch_type;        /* Type to match (TY_INT, TY_BOOL, etc.), TY_UNKNOWN = catch-all */
    Expr *handler;             /* Handler body expression */
} TryCatchClause;

/* Phase 19: Algebraic effects */

/* Effect definition: (defeffect Name [param1 : T1, ...] : R) */
typedef struct EffectDef {
    const Symbol *name;           /* Effect name */
    const Symbol **param_names;  /* Parameter names */
    TypeKind *param_types;       /* Parameter types (TypeKind for now) */
    uint8_t n_params;
    TypeKind result_type;        /* Result type of the effect operation */
    /* Phase P19-6: Module visibility */
    bool          is_private;           /* declared with ^private */
    const Symbol *defining_module_name; /* module that declared this effect, or NULL */
} EffectDef;

/* Perform expression: (perform (EffectName arg1 arg2 ...)) */
typedef struct PerformExpr {
    const Symbol *effect_name;   /* Name of the effect to perform */
    Expr **args;                /* Arguments to the effect */
    uint8_t n_args;
} PerformExpr;

/* Handle case: (EffectName [param1 param2 ...] k) body ... */
typedef struct HandleCase {
    const Symbol *effect_name;   /* Name of the effect being handled */
    const Symbol **param_names;  /* Parameter names for the effect */
    struct Binding **param_bindings; /* Resolved bindings for params (set by elab) */
    uint8_t n_params;           /* Number of parameters */
    const Symbol *k_name;        /* Name of the continuation parameter */
    struct Binding *k_binding;   /* Resolved binding for k (set by elab) */
    Expr *body;                 /* Handler body */
} HandleCase;

/* Handle expression: (handle expr case1 case2 ...) */
typedef struct HandleExpr {
    Expr *body;                 /* The expression being handled */
    HandleCase *cases;         /* Array of handle cases */
    uint8_t n_cases;
} HandleExpr;

/* Resume expression: (resume k value) */
typedef struct ResumeExpr {
    Expr *k;                   /* The continuation to resume */
    Expr *value;               /* The value to resume with */
} ResumeExpr;

/* Discontinue expression: (discontinue k exception) */
typedef struct DiscontinueExpr {
    Expr *k;                   /* The continuation to discontinue */
    Expr *exception;           /* The exception to raise */
} DiscontinueExpr;

/* Phase M0: Module system */

typedef struct {
    const Symbol *module_name;   /* module to import, e.g. "geom/vector" */
    const Symbol *alias;         /* :as alias (or NULL) */
    const Symbol **refer_syms;  /* :refer list (or NULL = none) */
    uint32_t n_refer;
    Span span;
} ImportSpec;

typedef struct DefModule {
    const Symbol *name;          /* module name, e.g. "geom/vector" */
    const char *docstring;       /* optional docstring (or NULL) */
    const Symbol **exports;      /* exported symbols */
    uint32_t n_exports;
    ImportSpec *imports;         /* import specs */
    uint32_t n_imports;
    Expr **body;                 /* elaborated body expressions */
    uint32_t n_body;
} DefModule;

struct Expr {
    ExprKind kind;
    Type     type;
    Span     span;
    union {
        bool         b;
        int64_t      i;
        double       f;        /* Phase 1: Float literal value */
        StrSlice     s;

        struct { Binding *binding; }                                       var;
        struct { LetBinding *bindings; uint32_t n; Expr *body; }           let_;
        struct { Expr *cond; Expr *then_; Expr *else_or_null; }            if_;
        struct { Expr **items; uint32_t n; }                               do_;
        struct { Expr *cond; Expr *body; }                                 while_;
        struct { Binding *target; Expr *value; }                           set_;
        struct { Binding *binding; Expr *init; StructDef *struct_def; }     def_;
        struct { const BuiltinSpec *spec; Expr **args; uint32_t n; }       builtin;

        /* Phase 2 */
        struct { FnDef *fn; }                                               fn_def_;
        /* Phase 16 v2: fn_expr is non-NULL for indirect capability field calls
         * (EX_GET_FIELD callee). fn_binding is NULL in that case. */
        /* Phase H §1: dict_arg is non-NULL for typeclass method calls; holds the
         * EX_DICT node for the selected instance's dictionary singleton.
         * Full runtime dict passing is deferred; this field annotates the IR
         * so the information is available for future passes. */
        struct { Binding *fn_binding; Expr **args; uint32_t n_args;
                 struct Expr *fn_expr;
                 struct Expr *dict_arg; }                                   call_;
        struct { FnDef *fn; }                                               fn_;
        struct { ExternC *ext; }                                            extern_c_;
        struct { InlineC *inline_c; }                                       inline_c_;
        /* Phase 3 */
        struct { struct Closure *closure; }                                 closure_;
        /* Phase 4 */
        struct { 
            Expr *body;              /* the defer body expression */
            /* v1 lowering: closure-style capture for defer bodies that reference
             * local variables. These are lifted into thunk functions with env structs.
             * Per effects-plan.md §6.10.1, this allows the S1/S2/S3 strategy choice
             * to be a runtime policy decision. */
            Binding **captures;       /* captured bindings from enclosing scope */
            uint8_t n_captures;
        } defer_;
        /* Phase 3/4: (return) or (return expr) - early return with defer firing */
        struct { Expr *value; } return_;
        /* Phase 5 */
        struct { Expr *expr; }        ref_;    /* (ref expr) - inner expression */
        struct { Expr *expr; }        deref_;  /* (@ expr) - expression to dereference */

        /* Phase 9: rc<T> + weak<T> operations */
        struct { Expr *expr; }        rc_of_;      /* (rc/of x) - value to wrap */
        struct { Expr *expr; bool elide; } rc_clone_;  /* (rc/clone r) - rc to clone; elide=true skips rc_strong_increment */
        struct { Expr *expr; bool elide; } rc_drop_;   /* (rc/drop r) - rc to drop; elide=true skips rc_strong_decrement */
        /* Note: (@ r) for rc<T> reuses deref_ field */
        struct { Expr *expr; }        rc_ptr_;    /* (rc->ptr r) - rc to borrow ptr from */
        struct { Expr *expr; }        rc_count_;  /* (rc/strong-count r) - rc to count */
        struct { Expr *expr; }        rc_from_ref_; /* (rc/from-ref r) - ref to convert */
        struct { Expr *expr; }        ref_from_rc_; /* (ref/from-rc r) - rc to convert */
        struct { Expr *expr; }        weak_;      /* (weak r) - rc to create weak from */
        struct { Expr *expr; }        weak_upgrade_; /* (upgrade w) - weak to upgrade */
        struct { Expr *expr; }        weak_pred_;   /* (weak? w) - expr to check */
        struct { Expr *expr; }        ref_pred_;    /* (ref? x) - expr to check */
        /* Phase 12: Borrow traits */
        struct { Expr *expr; }        borrow_immut_; /* (& expr) - expression to borrow immutably */
        struct { Expr *expr; }        borrow_mut_;   /* (&mut expr) - expression to borrow mutably */
        struct { Expr *ref; Expr *value; } set_deref_; /* (set! (@ r) v) - mutation through &mut T */
        /* Phase 15: Typeclasses */
        struct { TypeClass *typeclass; }                                  typeclass_def_;
        struct { TypeClassInstance *instance; }                          instance_def_;
        /* Phase 17: Exceptions */
        struct { Expr *payload; }        throw_;    /* (throw expr) - expression to throw */
        /* Phase R2: Panic */
        struct { Expr *payload; }        panic_;    /* (panic msg) - message to print before abort */
        struct { Expr *payload; }        panic_with_;    /* (panic-with payload) - typed payload */
        struct { Expr *thunk; }        catch_unwind_;    /* (catch-unwind thunk) - thunk to call */
        struct { TypeKind type_kind; Expr *thunk; } catch_panic_of_; /* (catch-panic-of Type thunk) */
        struct { Expr *payload; }        panic_payload_type_;   /* (panic-payload-type p) */
        struct { Expr *payload; }        panic_payload_value_;  /* (panic-payload-value p) */
        struct { Expr *payload; }        panic_payload_file_;   /* (panic-payload-file p) */
        struct { Expr *payload; }        panic_payload_line_;   /* (panic-payload-line p) */
        struct { Expr *payload; TypeKind target_type; } panic_payload_downs_; /* (panic-payload-downcast p Type) */
        struct {
            Expr *body;              /* try body expression */
            TryCatchClause *clauses; /* catch clauses */
            uint8_t n_clauses;      /* number of catch clauses */
            Expr *finally_body;     /* finally body (NULL if none) */
        } try_;
        /* Phase 18: Delimited continuations */
        struct { Expr *body; }         reset_;      /* (reset body) - body to run with fresh continuation */
        struct { 
            Expr *k_fn;             /* (shift k body) - k is a function (fn [v] ...) that receives the continuation */
            Expr *body;             /* body to run with captured continuation */
        } shift_;
        struct { 
            Expr *k_fn;             /* (shift0 k body) - k is a function that cannot resume */
            Expr *body;             /* body to run */
        } shift0_;
        /* Phase B2: Cloneable continuations */
        struct { Expr *body; }         cloneable_reset_; /* (cloneable-reset body) */
        struct {
            Expr *k_fn;             /* (cloneable-shift k body) - k receives cloneable continuation */
            Expr *body;             /* body to run with captured cloneable continuation */
            /* CPS-CL1: live locals at this shift site (arena-allocated, set by cps_transform) */
            Binding   **live_captures;
            uint32_t    n_live_captures;
            /* CPS-CL3: the post-shift subtree (code that runs after this shift inside
             * the enclosing reset).  Set by cps_transform; NULL until then. */
            Expr       *cont_body;
            /* CPS-CL11: per-capture deep-clone / drop function names recorded by
             * cps_emit_capture_environment().  Each entry is parallel to
             * live_captures[i]; an entry may be NULL when no Clone method is
             * available (in which case emit.c falls back to a bitwise copy).
             * Strings live in the arena and are safe to inline into emitted C. */
            const char **capture_clone_fns;
            const char **capture_drop_fns;
        } cloneable_shift_;
        /* Phase 19: Algebraic effects */
        struct { EffectDef *def; }                   effect_def_;   /* (defeffect ...) */
        struct { PerformExpr *perform; }             perform_;     /* (perform ...) */
        struct { HandleExpr *handle; }               handle_;      /* (handle ...) */
        struct { ResumeExpr *resume; }               resume_;      /* (resume k v) */
        struct { DiscontinueExpr *discontinue; }     discontinue_; /* (discontinue k e) */
        struct { Expr *expr; }                       cont_pred_;   /* (cont? k) */
        /* Phase T21-F: async/await */
        struct { Expr *fn_expr; }                    async_;       /* (async fn-expr) */
        struct { Expr *fut_expr; }                   await_;       /* (await fut) */
        /* Phase 20: Software Transactional Memory */
        struct { Expr **body; uint32_t n_body; }      stm_;         /* (stm expr1 expr2 ...) */
        struct { Expr *stm_expr; }                   atomically_;  /* (atomically stm-expr) */
        struct { Span dummy; }                       retry_;       /* (retry) - no fields, dummy for GNU */
        struct { Expr *cond; }                       check_;       /* (check cond) */
        struct { Expr *stm1; Expr *stm2; }           or_else_;     /* (or-else stm1 stm2) */
        struct { Expr *init; }                       tvar_new_;    /* (TVar::new init) */
        struct { Expr *tvar; }                       tvar_read_;   /* (TVar::read tvar) */
        struct { Expr *tvar; Expr *value; }           tvar_write_;  /* (TVar::write tvar value) */
        struct { Expr *tvar; Expr *fn; }              tvar_modify_; /* (TVar::modify tvar fn) */
        struct { Expr *tvar; Expr *new_val; }         tvar_swap_;   /* (TVar::swap tvar new) */
        struct { Expr *tvar; Expr *old_val; Expr *new_val; } tvar_cas_; /* (TVar::cas tvar old new) */
        /* Phase H §1: dictionary passing */
        struct {
            TypeClassInstance *instance;
            char dict_name[128];  /* "dict_<Class>_<type>" singleton name */
            char method_name[64]; /* sanitized method field name; '\0' = address-only mode */
        } dict_; /* (dict Instance) */

        /* Phase 11: Struct operations */
        struct { StructDef *def; Expr **field_values; uint32_t n_fields; } make_struct_; /* (make-struct Name v1...) */
        struct { Expr *struct_expr; uint32_t field_idx; StructDef *def; } get_field_; /* (.field s) - field read */

        struct { Expr **items; uint32_t n; }                               program;

        /* Phase M0: Module system */
        struct { struct DefModule *mod; }                                   defmodule_;
        /* Phase N: Numeric cast — (as TargetType expr) */
        struct { Expr *expr; TypeKind target_kind; }                       cast_;
    } as;
};

Expr *expr_new(Arena *a, ExprKind k, Type t, Span span);

void  expr_print(Buf *b, const Expr *e);   /* debug only */

#endif
