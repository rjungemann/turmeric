#ifndef TUR_CPS_IR_H
#define TUR_CPS_IR_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "expr.h"
#include "arena.h"
#include "types.h"

/* =========================================================================
 * CPS2 (cps-transform-plan): A-normal-form / CPS intermediate representation.
 *
 * This is the representation the selective-CPS lowering (CPS3) targets. It is
 * produced ONLY for colored (may-capture) functions; uncolored functions keep
 * their direct-style Expr tree untouched. The IR is intentionally small:
 *
 *   - Atoms are trivial values (variables and literals): no atom ever needs to
 *     be reduced, which is exactly the ANF invariant.
 *   - Every non-trivial subexpression is named by a `let`-style binder
 *     (CT_LETPRIM / CT_LETCALL / CT_LETVAL), so evaluation order is explicit.
 *   - The current continuation is reified as a CKont and threaded through:
 *     a tail position becomes either an application of the continuation
 *     (CT_APPCONT, "(k v)") or a tail call that passes the continuation on
 *     (CT_TAILCALL, "f(args, k)").
 *   - The function's return continuation `k` has type cont<T> (CPS2.3), where
 *     T is the function's result type kind (KK_RET carries it).
 *
 * The IR is dump-only at CPS2 (exposed via --dump-cps); it is not yet wired
 * into codegen. CPS3 consumes it.
 * ========================================================================= */

/* ---- Atoms: trivial (already-evaluated) values ------------------------- */
typedef enum CAtomKind {
    CA_VAR,       /* reference to a source binding or a CPS-introduced var */
    CA_CVAR,      /* reference to a CPS-introduced result var (by id+name) */
    CA_INT,       /* integer literal */
    CA_BOOL,      /* boolean literal */
    CA_UNIT,      /* nil / unit literal */
    CA_STR,       /* string literal (cstr) */
    CA_OTHER,     /* a trivial value we don't model precisely (e.g. float) */
} CAtomKind;

typedef struct CAtom {
    CAtomKind     kind;
    TypeKind      ty;          /* type kind of the value */
    const Binding *var;        /* CA_VAR */
    uint32_t      cvar_id;     /* CA_CVAR */
    const char   *cvar_name;   /* CA_CVAR */
    int64_t       i;           /* CA_INT */
    bool          b;           /* CA_BOOL */
    StrSlice      str;         /* CA_STR */
} CAtom;

/* ---- Continuations ---------------------------------------------------- */
typedef enum CKontKind {
    KK_RET,       /* the function's return continuation parameter `k : cont<T>` */
    KK_VAR,       /* a local continuation variable introduced by CT_LETCONT */
    KK_PROMPT,    /* the value delivered to the nearest delimited prompt (reset) */
} CKontKind;

typedef struct CKont {
    CKontKind kind;
    uint32_t  id;     /* KK_VAR id, or the result-type kind for KK_RET */
    TypeKind  ty;     /* result type kind the continuation accepts */
} CKont;

/* ---- Terms ------------------------------------------------------------ */
typedef enum CTermKind {
    CT_APPCONT,      /* (kont atom)                : deliver atom to a continuation */
    CT_LETVAL,       /* let x = atom in body       : trivial rebind */
    CT_LETPRIM,      /* let x = op(atoms...) in body */
    CT_LETCALL,      /* let x = f(atoms...) in body : direct (uncolored) call */
    CT_TAILCALL,     /* f(atoms..., kont)          : colored tail call, threads kont */
    CT_LETCONT,      /* letcont j(x) = jbody in body : a join point */
    CT_IF,           /* if atom then t else e */
    CT_RESET,        /* reset: bind x = <delimited body's value> in body */
    CT_SHIFT,        /* shift: capture current cont as k', run body to the prompt */
    CT_HANDLE,       /* handle: run delim under an effect handler; body is the continuation */
    CT_PERFORM,      /* perform: bind x = perform(effect, args), continue body */
    CT_RESUME,       /* resume: bind x = resume(k, v) [= dk_invoke], continue body */
    CT_UNSUPPORTED,  /* a source form outside the CPS2 subset (carries a reason) */
} CTermKind;

typedef struct CTerm CTerm;

typedef struct CVar {     /* a CPS-introduced binder */
    uint32_t    id;
    const char *name;
    TypeKind    ty;
} CVar;

struct CTerm {
    CTermKind kind;
    union {
        struct { CKont kont; CAtom v; }                                   appcont;
        struct { CVar x; CAtom v; CTerm *body; }                          letval;
        struct { CVar x; const char *op; const BuiltinSpec *spec; CAtom *args; uint32_t n; CTerm *body; } letprim;
        struct { CVar x; const Binding *fn; CAtom *args; uint32_t n; CTerm *body; } letcall;
        struct { const Binding *fn; CAtom *args; uint32_t n; CKont kont; } tailcall;
        struct { CVar j; CVar param; CTerm *jbody; CTerm *body; }         letcont;
        struct { CAtom cond; CTerm *then_; CTerm *else_; }                if_;
        struct { CVar x; CTerm *delim; CTerm *body; }                     reset;
        struct { CVar k; CTerm *body; }                                   shift;
        /* handle: delim = body threading the handler prompt; body = the handle's
         * continuation; case_body = the single handler clause (delivered by
         * return); k / params bound in case_body. */
        struct { CVar x; CTerm *delim; CTerm *body;
                 const Symbol *effect; const Binding **params; uint32_t n_params;
                 const Binding *k; CTerm *case_body; }                    handle;
        struct { const Symbol *effect; CAtom *args; uint32_t n;
                 CVar x; CTerm *body; }                                   perform;
        struct { CAtom k; CAtom v; CVar x; CTerm *body; }                 resume;
        struct { const char *why; }                                       unsupported;
    } as;
};

/* Translate one colored function body into CPS. Returns NULL if `fd` has no
 * body. The result is a CTerm delivering the body's value to KK_RET. */
CTerm *cps_ir_translate_fn(Arena *a, Expr *program, FnDef *fd);

/* Pretty-print a CPS term. */
void cps_ir_print(const CTerm *t, FILE *out, int indent);

/* --dump-cps: color `program`, then translate and print every colored
 * user-level top-level function in CPS form. */
void cps_ir_dump_program(Arena *a, Expr *program, FILE *out);

#endif
