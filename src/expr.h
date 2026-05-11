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
    /* Phase 15: Typeclasses */
    EX_TYPECLASS_DEF,   /* (defclass ...) - typeclass definition */
    EX_INSTANCE_DEF,   /* (definstance ...) - typeclass instance definition */
    /* Phase 17: Exceptions */
    EX_THROW,          /* (throw expr) - raise an exception */
    EX_TRY,            /* (try body (catch ...) (finally ...)) - try-catch-finally */
    /* Phase 18: Delimited continuations */
    EX_RESET,          /* (reset body) - establish continuation boundary */
    EX_SHIFT,          /* (shift k body) - capture continuation, pass to k */
    EX_SHIFT0,         /* (shift0 k body) - one-shot shift */
    /* Phase 19: Algebraic effects */
    EX_DEFECT,         /* (defeffect Name [params...] : result) - define an effect */
    EX_PERFORM,        /* (perform (EffectName args...)) - perform an effect */
    EX_HANDLE,         /* (handle expr cases...) - handle effects */
    EX_RESUME,         /* (resume k value) - resume continuation with value */
    EX_DISCONTINUE,    /* (discontinue k exception) - discontinue with exception */
    EX_PROGRAM,
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
    uint8_t n_params;           /* Number of parameters */
    const Symbol *k_name;        /* Name of the continuation parameter */
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
        struct { Binding *binding; Expr *init; }                           def_;
        struct { const BuiltinSpec *spec; Expr **args; uint32_t n; }       builtin;

        /* Phase 2 */
        struct { FnDef *fn; }                                               fn_def_;
        struct { Binding *fn_binding; Expr **args; uint32_t n_args; }       call_;
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
        /* Phase 15: Typeclasses */
        struct { TypeClass *typeclass; }                                  typeclass_def_;
        struct { TypeClassInstance *instance; }                          instance_def_;
        /* Phase 17: Exceptions */
        struct { Expr *payload; }        throw_;    /* (throw expr) - expression to throw */
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
        /* Phase 19: Algebraic effects */
        struct { EffectDef *def; }                   effect_def_;   /* (defeffect ...) */
        struct { PerformExpr *perform; }             perform_;     /* (perform ...) */
        struct { HandleExpr *handle; }               handle_;      /* (handle ...) */
        struct { ResumeExpr *resume; }               resume_;      /* (resume k v) */
        struct { DiscontinueExpr *discontinue; }     discontinue_; /* (discontinue k e) */

        struct { Expr **items; uint32_t n; }                               program;
    } as;
};

Expr *expr_new(Arena *a, ExprKind k, Type t, Span span);

void  expr_print(Buf *b, const Expr *e);   /* debug only */

#endif
