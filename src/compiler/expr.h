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

/* UT1: Alias state for uniqueness checking.
 * AS_UNIQUE: no live aliases for this binding.
 * AS_ALIASED: at least one other live binding refers to the same value. */
typedef enum AliasState {
    AS_UNIQUE  = 0,  /* Default: no aliases */
    AS_ALIASED = 1,  /* One or more aliases exist */
} AliasState;

/* ST1: Usage state for substructural discipline checking.
 * Tracks how many times a binding has been referenced.
 * USAGE_UNUSED == 0 so zero-initialised Bindings start unused. */
typedef enum UsageState {
    USAGE_UNUSED    = 0,  /* Not yet used */
    USAGE_USED_ONCE,      /* Used exactly once */
    USAGE_USED_MANY,      /* Used two or more times */
} UsageState;

/* A Binding is the resolved target of a `let`/`def`/`defn` name introduction.
 * Bindings are owned by the elaborator and live in the arena. */
struct Binding {
    const Symbol *name;
    Type          type;
    bool          is_mut;
    bool          is_global;     /* top-level def vs. local let */
    bool          is_param;      /* function/extern parameter binding */
    uint32_t      id;            /* unique within the program */
    Span          span;
    /* TY4: lexical scope depth at declaration (0 = outermost). Stamped by
     * binding_new from the live scope chain; the borrow-escape check compares
     * a borrow referent's depth against where the borrow lands. */
    uint32_t      scope_depth;
    /* Phase 3: For closure bindings, this points to the thunk function binding */
    struct Binding *closure_fn_binding;
    /* Returned-closure metadata: if evaluating this binding yields a closure value,
     * this points at the closure's thunk binding. */
    struct Binding *returns_closure_fn_binding;
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
    /* Phase P3: HAMT lowering - whether this binding is ^persistent (immutable map) */
    bool          is_persistent;
    /* LT1: Linear type checking — whether this binding holds a linear value */
    bool          is_linear;
    /* LT1: whether the linear value has been consumed (moved/used) */
    bool          is_linear_consumed;
    /* UT0: Uniqueness type -- whether this binding holds a unique value */
    bool          is_unique;
    /* UT0: whether the unique value has been consumed (moved/aliased) */
    bool          is_unique_consumed;
    /* ST0: Substructural annotations — ^affine and ^relevant */
    bool          is_affine;       /* true if annotated with ^affine (no duplication) */
    bool          is_relevant;     /* true if annotated with ^relevant (must be used) */
    /* ST1: Usage tracking for substructural discipline checking */
    UsageState    usage_state;     /* how many times this binding has been referenced */
    /* UT1: alias tracking -- current alias state for this binding */
    AliasState    alias_state;
    /* UT1: name of the binding that aliased this one (for TUR_E0200 message), or NULL */
    const Symbol *alias_name;
    /* Phase HRT1: rank-2 polymorphic function parameter */
    bool          is_poly_fn;     /* true if this binding is a rank-2 poly fn param */
    const struct Type *poly_type; /* full TY_FORALL type, NULL if not rank-2 */
    /* Phase HRT4: for let-bound aliases of global functions, tracks the original
     * function binding so poly_arg_fn_binding can find the callable C name. */
    struct Binding *source_binding;
    /* ER6: true if this binding was introduced by an (extern-c ...) declaration.
     * Used by effect_check to infer #{Unsafe} for calls to extern-c functions. */
    bool          is_extern_c;
    /* DV0: true if this binding was introduced by (defdynamic ...).
     * Used by DV1 binding/set! dispatch to distinguish dynamic vars from plain locals. */
    bool          is_dynvar;
    struct DynVarEntry *dynvar_entry; /* non-NULL iff is_dynvar */
    /* F4 (cross-plan-followups): ^deprecated annotation on a defn/def.
     * Each use site (EX_VAR resolution) emits a DIAG_WARNING with the
     * stored message (or a generic one if message is NULL).  Suppressed
     * for self-recursive references via e->current_fn_name. */
    bool          is_deprecated;
    const char   *deprecation_message;   /* NUL-terminated, arena-owned, or NULL */
    /* MF3 (test-suite-cleanup-plan): true if this binding came from an
     * auto-loaded stdlib module. Set during the M7 promotion in
     * elab_toplevel.c. Used by elab_defn to hard-error on user code that
     * shadows a stdlib name (which would otherwise produce conflicting
     * static functions of the same C name and break the C compile). */
    bool          is_from_stdlib;
    /* KB-021: true when this binding's emitted C value is a *by-value* concrete
     * carrier-ABI aggregate (e.g. a `Tuple2__int__int`/`Cons__int` local or
     * parameter) rather than the int64_t carrier.  Carrier-ABI types have two
     * coexisting C representations; the dictionary-dispatch callsite consults
     * this flag to decide whether a var argument must be bridged to the carrier
     * before the call.  Set at the binding's declaration site (let binding or
     * function parameter emission). */
    bool          emit_byvalue_carrier_abi;
};

/* GF1: Generator definition -- one per (gen ...) expression */
typedef struct GenDef {
    struct Expr  *body;              /* elaborated body expression */
    struct Binding **captures;       /* free variables captured from enclosing scope */
    uint32_t      n_captures;
    struct Binding **struct_bindings; /* all let bindings in gen body (yield-live) */
    uint32_t      n_struct_bindings;
    uint32_t      n_yield_points;    /* number of (yield ...) forms in body */
    TypeKind      element_kind;      /* TypeKind of values yielded */
    char          struct_name[64];   /* e.g. "__gen_myfn_0_t" */
    char          next_fn[64];       /* e.g. "__gen_myfn_0_next" */
    char          create_fn[64];     /* e.g. "__gen_myfn_0_create" */
} GenDef;

typedef enum ExprKind {
    EX_NIL_LIT = 1,
    EX_BOOL_LIT,
    EX_INT_LIT,
    EX_FLOAT_LIT,       /* Phase 1: Float literals */
    EX_CSTR_LIT,
    EX_VAR,
    EX_LET,
    EX_LETREC,             /* letrec -- mutually-recursive let bindings */
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
    /* FH2-FH5: first-class effect handler values */
    EX_HANDLER_LIT,    /* (handler (E [params] k) body) - handler value literal */
    EX_WITH_HANDLER,   /* (with-handler hv body) - apply a handler value to a body */
    EX_COMPOSE_HANDLERS, /* (compose-handlers h1 h2) - concat two handler tables */
    EX_RESUME,         /* (resume k value) - resume continuation with value */
    EX_DISCONTINUE,    /* (discontinue k exception) - discontinue with exception */
    EX_CONT_PRED,      /* (cont? k) - check if continuation is unconsumed */
    /* Phase T21-F: async/await sugar */
    EX_ASYNC,          /* (async fn-expr) - run no-arg fn in thread; return Future (ptr<void>) */
    EX_AWAIT,          /* (await fut)     - block on Future; return int value */
    /* Phase SEL1: fair multi-channel select */
    EX_SELECT,         /* (select ((ch :recv v) body) ... (:default body)) */
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
    /* Phase N: numeric cast */
    EX_CAST,           /* (as TargetType expr) — explicit numeric cast */
    EX_REINTERPRET,    /* compiler-only bit reinterpret between same-size scalar types */
    /* Phase H §1: dictionary passing */
    EX_DICT,           /* implicit dictionary argument — address of a typeclass instance singleton */
    /* Phase 11: Struct operations */
    EX_MAKE_STRUCT,    /* (make-struct Name v1 v2 ...) - struct literal */
    EX_GET_FIELD,      /* (.field s) - struct field access */
    /* Phase DS3: struct field assignment */
    EX_SET_FIELD,      /* (set! (.field s) v) - struct field write */
    EX_PROGRAM,
    /* Phase M0: Module system */
    EX_DEFMODULE,      /* (defmodule name [docstring] (export ...) (import ...) body...) */
    /* Phase 21: Serializable continuations */
    EX_SERIAL_RESET,   /* (serial-reset body) - establish serializable continuation boundary */
    EX_SERIAL_SHIFT,   /* (serial-shift k body) - capture serializable continuation */
    EX_THROW,       /* (throw expr) - raise catchable exception; type is TY_NEVER */
    EX_TRY_CATCH,   /* (try body (catch [bind] handler) ...) - catch block */
    /* Phase X3: Set literal */
    EX_SET_LIT,        /* #s(e1 e2 ...) - set literal */
    /* Phase HRT1: Rank-2 higher-ranked types */
    EX_POLY_WRAP,      /* wraps a fn/closure as tur_poly_fn_t for rank-2 param passing */
    EX_ASCRIBE,        /* (:: expr type) — inline type ascription; erased at codegen */
    /* Phase HRT2: Existential types */
    EX_EXISTS_PACK,    /* (pack expr (exists [a] T)) — boxes value as existential */
    EX_EXISTS_OPEN,    /* (open packed [a v] body) — unboxes existential, binds v */
    /* Phase G0: Plain ADTs */
    EX_DEFDATA,        /* (defdata Name [:copy] (Ctor1) (Ctor2 T1 T2) ...) */
    EX_MATCH,          /* (match scrutinee (Ctor1 x y) body1 _ default-body) */
    /* Phase G1: GADTs */
    EX_DEFGADT,        /* (defgadt Name [params] (Ctor : return-type) ...) */
    /* IT4: Tagged union injection — wraps a value into tur_tagged_t */
    EX_UNION_INJECT,   /* (union-inject tag_idx value) — tags a member value for TY_UNION/TY_ANY */
    /* IT4 gradual typing */
    EX_ANY_TYPE_OF,    /* (type-of x) — returns cstr type name of an any-typed value */
    EX_ANY_CAST,       /* (cast x T) — unsafe downcast from any; returns the inner value as T */
    EX_ANY_IS,         /* TY3: (is? x T) — runtime type test; returns bool */
    /* DV0-DV1: Dynamic vars (-Xdynamic-vars) */
    EX_DEFDYNAMIC,       /* (defdynamic *name* :type root-expr) -- declare a dynamic var */
    EX_DYNVAR_READ,      /* *name* -- read current value of a dynamic var */
    EX_DYNVAR_BINDING,   /* (binding [*v* expr ...] body) -- dynamic binding form */
    EX_DYNVAR_SET,       /* (set! *name* expr) on a dynvar -- mutate top binding frame */
    /* GF1: Generator forms */
    EX_GEN,              /* (gen [] body) -- generator expression; returns TY_GENERATOR */
    EX_YIELD,            /* (yield expr)  -- yield a value inside gen body; type TY_NIL */
    EX_GEN_NEXT,         /* (gen-next g)  -- advance generator; returns ptr<void> (option) */
    EX_GEN_DONE,         /* (gen-done? g) -- check if generator is exhausted; returns bool */
    /* AR8: Variadic rest-list construction at call sites */
    EX_CONS_LIST,        /* build a right-folded cons list from N items for & rest param */
} ExprKind;

/* Phase 2: FnDef represents a function definition from defn or lifted fn. */
struct FnDef {
    Binding        *binding;     /* name binding */
    Binding       **params;      /* param bindings */
    uint8_t        n_params;
    Type          *param_types;  /* param types (for codegen) */
    /* LS2: full declared return Type, including borrow lifetimes (&'a T).  The
     * binding's TY_FN only carries result_kind (a bare TypeKind), which loses
     * the lifetime IDs the lifetime pass needs; this preserves them. */
    Type           return_type;
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
    /* CT0: Contract pre/post-conditions — NULL if not specified */
    const struct Form *pre_cond;   /* :pre predicate form, or NULL */
    const struct Form *post_cond;  /* :post predicate form, or NULL */
};

/* Phase 2: ExternC represents an (extern-c ...) declaration. */
struct ExternC {
    const Symbol  *c_name;       /* C identifier */
    Binding       *binding;     /* Turmeric binding (optional) */
    Type           return_type;
    Type          *param_types;
    uint8_t        n_params;
    bool           is_variadic;
    /* CT4: Contract pre/post-conditions on extern-c calls — NULL if not specified */
    const struct Form *pre_cond;   /* :pre predicate form, or NULL */
    const struct Form *post_cond;  /* :post predicate form, or NULL */
};

/* Phase 2: InlineC represents an inline C block. ```c ... ``` */
struct InlineC {
    StrSlice       code;         /* the raw C code (may contain __TUR_CAP_N__ / __TUR_VAL_N__) */
    Type           return_type; /* annotated : T */
    Binding      **captures;     /* captured bindings; __TUR_CAP_N__ substitutes captures[N]'s C name */
    uint8_t        n_captures;
    struct Expr  **val_exprs;    /* SS2: sub-expressions; __TUR_VAL_N__ evaluates val_exprs[N] */
    uint8_t        n_val_exprs;
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
    /* ET4: effect hierarchy -- NULL if no ^extends */
    const Symbol *parent_name;          /* name of parent effect (for ^extends), or NULL */
} EffectDef;

/* DV0: Dynamic var metadata entry (-Xdynamic-vars).
 * One entry per (defdynamic ...) declaration. */
typedef struct DynVarEntry {
    const Symbol *name;       /* interned symbol, e.g. "*log-level*" */
    Type          value_type; /* declared element type */
    int           index;      /* sequential ID; used as pthread_key_t index in DV2 */
    bool          is_private; /* ^private annotation */
} DynVarEntry;

/* DV1: One override pair inside a (binding [...] body) form. */
typedef struct DynBinding {
    DynVarEntry *entry;         /* which dynamic var is being overridden */
    struct Expr *override_expr; /* the new value for this scope */
} DynBinding;

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
    /* LC0: ownership discipline for k.
     * CK_UNIQUE (default): at most one resume/discontinue (affine).
     * CK_LINEAR (^linear k): exactly one resume/discontinue required.
     * CK_COPY (^unsafe-multishot k): no ownership tracking; multi-shot allowed.
     * CK_MULTISHOT (^multishot k): MS1: safe multi-shot via snapshot semantics. */
    CopyKind cont_kind;
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
    const Symbol **refer_syms;        /* :refer list (or NULL = none) */
    uint32_t n_refer;
    const Symbol **refer_effect_syms; /* PR5-3-D: (effect Name) entries from :refer list */
    uint32_t       n_refer_effects;
    Span span;
} ImportSpec;

typedef struct DefModule {
    const Symbol *name;          /* module name, e.g. "geom/vector" */
    const char *docstring;       /* optional docstring (or NULL) */
    const Symbol **exports;           /* exported symbols */
    uint32_t n_exports;
    const Symbol **exported_effects;  /* PR5-3-B: effect names in (export (effect Name)) */
    uint32_t       n_exported_effects;
    ImportSpec *imports;         /* import specs */
    uint32_t n_imports;
    Expr **body;                 /* elaborated body expressions */
    uint32_t n_body;
} DefModule;

/* Phase G0: A single pattern in a match arm */
typedef struct MatchPattern {
    bool is_wildcard;           /* _ wildcard */
    bool is_var;                /* bare variable capture (not a constructor) */
    bool is_literal;            /* literal value comparison (int/bool/float/cstr) */
    CtorDef *ctor;              /* NULL for wildcard/var/literal */
    Binding **bindings;         /* arena-allocated bindings for ctor fields */
    uint32_t n_bindings;
    const Symbol *var_sym;      /* for is_var: the variable being bound */
    Binding *var_binding;       /* resolved binding for is_var */
    int union_member_idx;       /* IT4: index into union members array for type-narrowing arms;
                                 * -1 for wildcard/var arms and non-union (ADT) arms */
    /* for is_literal: the literal value to compare against */
    int8_t      lit_kind;       /* F_BOOL/F_INT/F_FLOAT/F_STR/F_NIL from forms.h */
    bool        lit_bool;
    int64_t     lit_int;
    double      lit_float;
    const char *lit_cstr;
} MatchPattern;

/* Phase G0: One arm of a match expression */
typedef struct MatchArm {
    MatchPattern pattern;
    struct Expr *body;
    struct Expr *guard;  /* Phase G4: optional when-guard; NULL if no guard */
} MatchArm;

/* Phase SEL1: one clause of a (select ...) expression */
typedef struct SelectClauseEntry {
    struct Expr     *chan;         /* channel expression (ptr<void>) */
    int              op;          /* 0=recv, 1=send */
    struct Expr     *send_val;    /* for send: value expression; NULL for recv */
    Binding         *recv_binding; /* for recv: binding allocated by elab; NULL for send */
    struct Expr     *body;        /* clause body expression */
} SelectClauseEntry;

/* GS5/CS3: named-tyvar -> concrete type binding produced during call elaboration.
 * Attached to EX_CALL so emit_module.c can drive ABI specialization without
 * re-deriving the substitution. Owned by the elab arena. */
typedef struct AbiTypeBinding {
    const char *name;
    Type        type;
} AbiTypeBinding;

#define ABI_TYPE_BINDINGS_MAX 16

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
                 struct Expr *dict_arg;
                 bool is_poly_call;   /* Phase HRT1: call through rank-2 poly fn param */
                 uint32_t poly_arg_mask; /* Phase HRT3: bitmask of args that are nested poly fns.
                                          * In poly_call: bit i → pass arg by pointer (stack-alloc).
                                          * In direct call: bit i → deref int64_t arg as tur_poly_fn_t*. */
                 AbiTypeBinding *abi_bindings; /* GS5/CS3: named-tyvar substitution captured at the call site;
                                                * NULL when the call has no named-tyvar bindings. Arena-owned. */
                 uint8_t  n_abi_bindings;
                 bool is_tail_self_call; /* CF1: direct self-tail-call lowered to a goto backedge
                                          * by the emitter (set by emit_fns.c tco_mark). */ } call_;
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
        /* FH2-FH5: first-class handler values */
        struct { HandleExpr *handle; }               handler_lit_; /* (handler ...) -- cases, body==NULL */
        struct { Expr *handler; Expr *body; }        with_handler_;/* (with-handler hv body) */
        struct { Expr *h1; Expr *h2; }               compose_handlers_; /* (compose-handlers h1 h2) */
        struct { ResumeExpr *resume; }               resume_;      /* (resume k v) */
        struct { DiscontinueExpr *discontinue; }     discontinue_; /* (discontinue k e) */
        struct { Expr *expr; }                       cont_pred_;   /* (cont? k) */
        /* Phase T21-F: async/await */
        struct { Expr *fn_expr; }                    async_;       /* (async fn-expr) */
        struct { Expr *fut_expr; }                   await_;       /* (await fut) */
        /* Phase SEL1: fair multi-channel select */
        struct {
            SelectClauseEntry *clauses;   /* arena-allocated array */
            uint32_t           n_clauses;
            int                has_default;   /* 1 if :default arm present */
            struct Expr       *default_body;  /* :default body; NULL if none */
        } select_;                                                  /* (select ...) */
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
        /* Phase N: numeric cast */
        struct { Expr *expr; TypeKind target_kind; } cast_;        /* (as T e) */
        struct { Expr *expr; TypeKind source_kind; TypeKind target_kind; } reinterpret_;
        /* Phase H §1: dictionary passing */
        struct {
            TypeClassInstance *instance;
            char dict_name[128];  /* "dict_<Class>_<type>" singleton name */
            char method_name[64]; /* sanitized method field name; '\0' = address-only mode */
        } dict_; /* (dict Instance) */

        /* Phase 11: Struct operations */
        struct { StructDef *def; Expr **field_values; uint32_t n_fields; } make_struct_; /* (make-struct Name v1...) */
        struct { Expr *struct_expr; uint32_t field_idx; StructDef *def; } get_field_; /* (.field s) - field read */
        /* Phase DS3: (set! (.field s) v) - struct field write.  receiver_is_rc
         * is true when the receiver expression is rc<Struct> (auto-deref); in
         * that case codegen casts through the rc-block's value pointer. */
        struct { Expr *receiver; Expr *value; uint32_t field_idx; StructDef *def; bool receiver_is_rc; } set_field_;

        struct { Expr **items; uint32_t n; }                               program;

        /* Phase X3: Set literal */
        struct { Expr **items; uint32_t n; }                               set_lit_;

        /* Phase M0: Module system */
        struct { struct DefModule *mod; }                                   defmodule_;
        /* Phase 21: Serializable continuations */
        struct { Expr *body; }         serial_reset_; /* (serial-reset body) */
        struct {
            Expr *k_fn;   /* function receiving the serial continuation */
            Expr *body;   /* body expression */
        } serial_shift_;
        /* Phase S4: throw / try-catch */
        struct { Expr *value; }                                             throw_;
        struct {
            Expr      *body;
            Binding  **catch_bindings;   /* NULL entry = unnamed catch */
            Expr     **catch_handlers;
            uint32_t   n_catches;
        }                                                                   try_catch_;
        /* Phase HRT1: Higher-ranked types */
        struct {
            struct Expr    *inner;           /* the fn/closure being wrapped */
            struct Binding *wrapper_binding; /* the __poly_N wrapper thunk binding */
            /* Phase CCL: true when inner is a fat closure (void*) rather than a
             * named function.  The emitter packs it into tur_poly_fn_t at the
             * call site instead of emitting a (tur_poly_fn_t){ NULL, wrapper }. */
            bool            is_closure;
        } poly_wrap_;
        struct { struct Expr *inner; } ascribe_; /* (:: expr type) — type erased at codegen */
        /* Phase HRT2: Existential types.
         * Phase EX1c: optional resolved constraint witnesses (one per constraint
         * in the target existential type).  NULL when the target has no
         * constraints. */
        struct {
            struct Expr           *value;     /* the value being packed */
            TypeClassInstance    **witnesses; /* arena-allocated; length n_witnesses */
            uint8_t                n_witnesses;
        } exists_pack_;
        struct {
            struct Expr    *packed;      /* the packed existential expression */
            struct Binding *var_binding; /* binding for v (the unboxed inner value) */
            struct Expr    *body;        /* the open body expression */
        } exists_open_;
        /* Phase G0: ADT definition */
        struct {
            AdtDef  *def;      /* the ADT being defined */
            Binding *binding;  /* global binding for the ADT type */
        } defdata_;
        /* Phase G1: GADT definition — same layout as defdata_ */
        struct {
            AdtDef  *def;      /* the GADT being defined (is_gadt=true) */
            Binding *binding;  /* global binding for the GADT type */
        } defgadt_;
        /* Phase G0: match expression */
        struct {
            struct Expr *scrutinee;
            MatchArm    *arms;      /* arena-allocated array */
            uint32_t     n_arms;
        } match_;
        /* IT4: Tagged union injection — wraps a member value into tur_tagged_t */
        struct {
            int64_t     tag_idx;  /* member index (for TY_UNION) or TypeKind (for TY_ANY) */
            struct Expr *value;   /* the value being injected */
            /* TY2.2: when the injected value is a by-value struct it cannot ride
             * the int64_t carrier; box_struct names the StructDef so codegen
             * emits a heap copy (malloc + store) and stores the pointer. NULL
             * for carrier-compatible payloads (int/bool/float/nil/cstr/ptr/ADT). */
            const struct StructDef *box_struct;
        } union_inject_;
        /* IT4 gradual typing */
        struct { struct Expr *value; } any_type_of_;   /* (type-of x) — x must be TY_ANY */
        /* TY3: (is? x T) — runtime type test; emits TUR_GETTAG(x) == test_tag. */
        struct { struct Expr *value; int64_t test_tag; } any_is_;
        /* TY2.3: (cast x T) — checked downcast; panics on tag mismatch.
         * target_struct is non-NULL when T is a struct (heap-unbox via deref). */
        struct {
            struct Expr *value;
            TypeKind     target_kind;
            const struct StructDef *target_struct;
        } any_cast_;
        /* DV0: Dynamic var declaration */
        struct {
            DynVarEntry        *entry;      /* the registered dynvar (name, value_type, index) */
            struct Expr        *root_expr;  /* elaborated root value expression */
        } defdynamic_;
        /* DV1: Read the current value of a dynamic var */
        struct {
            DynVarEntry        *entry;      /* which dynamic var to read */
        } dynvar_read_;
        /* DV1: (binding [*v* expr ...] body) -- push override frames, run body, pop */
        struct {
            DynBinding         *pairs;      /* override pairs (var + override expr) */
            uint32_t            n_pairs;
            struct Expr        *body;       /* body evaluated with overrides active */
        } dynvar_binding_;
        /* DV1: (set! *name* expr) on a dynamic var -- mutate top binding frame */
        struct {
            DynVarEntry        *entry;      /* which dynamic var to mutate */
            struct Expr        *value;      /* new value */
        } dynvar_set_;
        /* GF1: Generator expression */
        struct {
            GenDef             *def;        /* generator definition */
        } gen_;
        /* GF1: Yield expression -- inside a gen body */
        struct {
            struct Expr        *value;      /* value being yielded */
            uint32_t            yield_id;   /* 1-based yield point index */
        } yield_;
        /* GF1: Advance a generator -- returns ptr<void> (option) */
        struct {
            struct Expr        *gen_expr;   /* the generator value */
            GenDef             *def;        /* generator definition */
        } gen_next_;
        /* GF1: Check if a generator is exhausted */
        struct {
            struct Expr        *gen_expr;   /* the generator value */
        } gen_done_;
        /* AR8: variadic rest-list construction */
        struct {
            struct Expr **items;   /* the surplus args to pack into a cons list */
            uint32_t     n;        /* number of items */
            TypeKind     item_kind; /* element type (determines cast in emitter) */
        } cons_list_;
    } as;
};

Expr *expr_new(Arena *a, ExprKind k, Type t, Span span);

void  expr_print(Buf *b, const Expr *e);   /* debug only */

#endif
