#ifndef TUR_EXPR_H
#define TUR_EXPR_H

#include <stdint.h>
#include <stdbool.h>

#include "arena.h"
#include "buf.h"
#include "forms.h"   /* Span */
#include "symbols.h"
#include "types.h"

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
};

typedef enum ExprKind {
    EX_NIL_LIT = 1,
    EX_BOOL_LIT,
    EX_INT_LIT,
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
    EX_DEFER,           /* Phase 4: defer expression */
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

struct Expr {
    ExprKind kind;
    Type     type;
    Span     span;
    union {
        bool         b;
        int64_t      i;
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
        struct { Expr *body; }                                               defer_;

        struct { Expr **items; uint32_t n; }                               program;
    } as;
};

Expr *expr_new(Arena *a, ExprKind k, Type t, Span span);

void  expr_print(Buf *b, const Expr *e);   /* debug only */

#endif
