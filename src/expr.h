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

/* A Binding is the resolved target of a `let`/`def`/`defn` name introduction.
 * Bindings are owned by the elaborator and live in the arena. */
struct Binding {
    const Symbol *name;
    Type          type;
    bool          is_mut;
    bool          is_global;     /* top-level def vs. local let */
    uint32_t      id;            /* unique within the program */
    Span          span;
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
    EX_PROGRAM,
} ExprKind;

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
        struct { Expr **items; uint32_t n; }                               program;
    } as;
};

Expr *expr_new(Arena *a, ExprKind k, Type t, Span span);

void  expr_print(Buf *b, const Expr *e);   /* debug only */

#endif
