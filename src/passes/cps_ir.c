#include "cps_ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cps.h"
#include "builtins.h"

/* =========================================================================
 * CPS2 (cps-transform-plan): ANF/CPS translation for colored functions.
 *
 * The translation is the textbook two-function scheme:
 *   - cps_tail(e, k)      : emit a term that delivers e's value to continuation k.
 *   - cps_bind(e, x, rest): emit a term that binds e's value to x, then runs rest.
 * Non-trivial arguments are first "atomized" (named by a fresh binder), which is
 * what establishes the ANF invariant. Colored callees are tail-called with the
 * continuation threaded through (CT_TAILCALL); in non-tail position a join
 * continuation (CT_LETCONT) names the call's result. See cps_ir.h.
 * ========================================================================= */

typedef struct CpsB {
    Arena *a;
    Expr  *program;     /* for colored-callee lookup */
    uint32_t counter;   /* fresh-id source */
    CKont  retk;        /* the function's return continuation (KK_RET) */
} CpsB;

/* ---- small allocation helpers ----------------------------------------- */

static CTerm *new_term(CpsB *b, CTermKind k) {
    CTerm *t = arena_alloc(b->a, sizeof(CTerm));
    memset(t, 0, sizeof(CTerm));
    t->kind = k;
    return t;
}

static CVar fresh_cvar(CpsB *b, TypeKind ty) {
    CVar v;
    v.id = b->counter++;
    char buf[24];
    snprintf(buf, sizeof(buf), "t%u", v.id);
    v.name = arena_strdup(b->a, buf, strlen(buf));
    v.ty = ty;
    return v;
}

static CVar cvar_of_binding(const Binding *bd) {
    CVar v;
    v.id = bd->id;
    v.name = (bd->name ? bd->name->name : "_");
    v.ty = bd->type.kind;
    return v;
}

static CKont kont_var(CVar j) {
    CKont k; k.kind = KK_VAR; k.id = j.id; k.ty = j.ty; return k;
}

static CKont kont_prompt(TypeKind ty) {
    CKont k; k.kind = KK_PROMPT; k.id = 0; k.ty = ty; return k;
}

/* ---- atoms ------------------------------------------------------------ */

/* Type ascription `(:: e T)` is erased at codegen (its value is the inner
 * expression's value), so peel it everywhere the translator inspects a form. */
static const Expr *ascribe_peel(const Expr *e) {
    while (e && e->kind == EX_ASCRIBE) e = e->as.ascribe_.inner;
    return e;
}

static bool is_atomic(const Expr *e) {
    e = ascribe_peel(e);
    if (!e) return false;
    switch (e->kind) {
        case EX_INT_LIT:
        case EX_BOOL_LIT:
        case EX_NIL_LIT:
        case EX_FLOAT_LIT:
        case EX_CSTR_LIT:
        case EX_VAR:
            return true;
        default:
            return false;
    }
}

static CAtom atom_of(const Expr *e) {
    e = ascribe_peel(e);
    CAtom a; memset(&a, 0, sizeof(a));
    a.ty = e ? e->type.kind : TY_UNKNOWN;
    if (!e) { a.kind = CA_UNIT; return a; }
    switch (e->kind) {
        case EX_INT_LIT:   a.kind = CA_INT;  a.i = e->as.i; break;
        case EX_BOOL_LIT:  a.kind = CA_BOOL; a.b = e->as.b; break;
        case EX_NIL_LIT:   a.kind = CA_UNIT; break;
        case EX_FLOAT_LIT: a.kind = CA_OTHER; break;
        case EX_CSTR_LIT:  a.kind = CA_STR;  a.str = e->as.s; break;
        case EX_VAR:       a.kind = CA_VAR;  a.var = e->as.var.binding; break;
        default:           a.kind = CA_OTHER; break;
    }
    return a;
}

static CAtom atom_cvar(CVar v) {
    CAtom a; memset(&a, 0, sizeof(a));
    a.kind = CA_CVAR; a.ty = v.ty; a.cvar_id = v.id; a.cvar_name = v.name;
    return a;
}

/* ---- colored-callee lookup -------------------------------------------- */

/* True if `fn` resolves to a colored top-level function. */
static bool callee_colored(CpsB *b, const Binding *fn) {
    if (!fn || !b->program || b->program->kind != EX_PROGRAM) return false;
    for (uint32_t i = 0; i < b->program->as.program.n; i++) {
        Expr *it = b->program->as.program.items[i];
        if (it && it->kind == EX_FN_DEF && it->as.fn_def_.fn &&
            it->as.fn_def_.fn->binding == fn)
            return it->as.fn_def_.fn->cps_colored;
    }
    return false;
}

/* ---- pending bindings (drives atomization order) ---------------------- */

typedef struct { Expr *expr; CVar x; } PendItem;
typedef struct { PendItem items[32]; uint32_t n; } Pending;

/* forward decls */
static CTerm *cps_tail(CpsB *b, Expr *e, CKont kont);
static CTerm *cps_bind(CpsB *b, Expr *e, CVar x, CTerm *rest);

static CAtom atomize(CpsB *b, Expr *e, Pending *p) {
    if (is_atomic(e)) return atom_of(e);
    CVar x = fresh_cvar(b, e ? e->type.kind : TY_UNKNOWN);
    if (p->n < 32) { p->items[p->n].expr = e; p->items[p->n].x = x; p->n++; }
    return atom_cvar(x);
}

/* Wrap `core` with the pending bindings, leftmost outermost. */
static CTerm *fold_pending(CpsB *b, Pending *p, CTerm *core) {
    for (int i = (int)p->n - 1; i >= 0; i--)
        core = cps_bind(b, p->items[i].expr, p->items[i].x, core);
    return core;
}

static const char *builtin_name(const Expr *e) {
    if (e->kind == EX_BUILTIN && e->as.builtin.spec && e->as.builtin.spec->name)
        return e->as.builtin.spec->name;
    return "?";
}

/* The value a `(shift k_fn body)` delivers to its prompt is `k_fn(body)` (the
 * abortive-shift semantics: the receiver is applied to the body value, and the
 * result becomes the enclosing reset's value; see eval_abortive_shift).  Model
 * that by synthesizing the application `(k_fn body)` and CPS-translating it to
 * the prompt continuation.  A receiver that is not a directly-callable binding
 * leaves fn_binding NULL, so cps_tail yields CT_UNSUPPORTED and the backend
 * falls back cleanly. */
static CTerm *cps_shift_body(CpsB *b, Expr *e) {
    Expr *recv = e->as.shift_.k_fn;
    Expr *arg  = e->as.shift_.body;
    Expr *call = arena_alloc(b->a, sizeof(Expr));
    memset(call, 0, sizeof(Expr));
    call->kind = EX_CALL;
    call->type = e->type;
    call->as.call_.fn_binding =
        (recv && recv->kind == EX_VAR) ? recv->as.var.binding : NULL;
    call->as.call_.fn_expr = recv;
    call->as.call_.args = arena_alloc(b->a, sizeof(Expr *));
    call->as.call_.args[0] = arg;
    call->as.call_.n_args = 1;
    return cps_tail(b, call, kont_prompt(e->type.kind));
}

/* ---- algebraic effects: handle / perform / resume --------------------- *
 * Lowered to CT_HANDLE / CT_PERFORM / CT_RESUME.  `cont` is the continuation
 * term (an APPCONT to the enclosing kont in tail position, or the `rest` term in
 * bind position).  perform/resume atomize their sub-expressions into `p`. */

static CTerm *build_handle(CpsB *b, Expr *e, CVar x, CTerm *cont) {
    HandleExpr *h = e->as.handle_.handle;
    /* C4 subset: exactly one handler case. */
    if (!h || h->n_cases != 1) {
        CTerm *u = new_term(b, CT_UNSUPPORTED);
        u->as.unsupported.why = "handle: only single-case handlers in CPS subset";
        return u;
    }
    HandleCase *c = &h->cases[0];
    CTerm *t = new_term(b, CT_HANDLE);
    t->as.handle.x = x;
    t->as.handle.delim = cps_tail(b, h->body, kont_prompt(e->type.kind));
    t->as.handle.effect = c->effect_name;
    t->as.handle.n_params = c->n_params;
    if (c->n_params) {
        const Binding **ps = arena_alloc(b->a, c->n_params * sizeof(const Binding *));
        for (uint8_t i = 0; i < c->n_params; i++) ps[i] = c->param_bindings[i];
        t->as.handle.params = ps;
    }
    t->as.handle.k = c->k_binding;
    /* The case clause delivers its value by return (KK_PROMPT), which dk_perform
     * routes to the handler's outer continuation. */
    t->as.handle.case_body = cps_tail(b, c->body, kont_prompt(c->body ? c->body->type.kind : TY_INT));
    t->as.handle.body = cont;
    return t;
}

static CTerm *build_perform(CpsB *b, Expr *e, CVar x, CTerm *cont, Pending *p) {
    PerformExpr *pf = e->as.perform_.perform;
    CTerm *t = new_term(b, CT_PERFORM);
    t->as.perform.effect = pf->effect_name;
    t->as.perform.n = pf->n_args;
    CAtom *args = arena_alloc(b->a, (pf->n_args ? pf->n_args : 1) * sizeof(CAtom));
    for (uint8_t i = 0; i < pf->n_args; i++) args[i] = atomize(b, pf->args[i], p);
    t->as.perform.args = args;
    t->as.perform.x = x;
    t->as.perform.body = cont;
    return t;
}

static CTerm *build_resume(CpsB *b, Expr *e, CVar x, CTerm *cont, Pending *p) {
    ResumeExpr *r = e->as.resume_.resume;
    CTerm *t = new_term(b, CT_RESUME);
    t->as.resume.k = atomize(b, r->k, p);
    t->as.resume.v = atomize(b, r->value, p);
    t->as.resume.x = x;
    t->as.resume.body = cont;
    return t;
}

/* ---- cps_tail: deliver e's value to `kont` ---------------------------- */

static CTerm *cps_tail(CpsB *b, Expr *e, CKont kont) {
    e = (Expr *)ascribe_peel(e);
    if (!e) {
        CTerm *t = new_term(b, CT_UNSUPPORTED);
        t->as.unsupported.why = "null";
        return t;
    }
    if (is_atomic(e)) {
        CTerm *t = new_term(b, CT_APPCONT);
        t->as.appcont.kont = kont;
        t->as.appcont.v = atom_of(e);
        return t;
    }
    switch (e->kind) {
        case EX_BUILTIN: {
            Pending p = {0};
            CAtom *args = arena_alloc(b->a, (e->as.builtin.n ? e->as.builtin.n : 1) * sizeof(CAtom));
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                args[i] = atomize(b, e->as.builtin.args[i], &p);
            CVar x = fresh_cvar(b, e->type.kind);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            CTerm *t = new_term(b, CT_LETPRIM);
            t->as.letprim.x = x; t->as.letprim.op = builtin_name(e);
            t->as.letprim.spec = e->as.builtin.spec;
            t->as.letprim.args = args; t->as.letprim.n = e->as.builtin.n;
            t->as.letprim.body = ac;
            return fold_pending(b, &p, t);
        }
        case EX_CALL: {
            const Binding *fn = e->as.call_.fn_binding;
            if (!fn) {
                CTerm *t = new_term(b, CT_UNSUPPORTED);
                t->as.unsupported.why = "indirect call";
                return t;
            }
            Pending p = {0};
            uint32_t n = e->as.call_.n_args;
            CAtom *args = arena_alloc(b->a, (n ? n : 1) * sizeof(CAtom));
            for (uint32_t i = 0; i < n; i++)
                args[i] = atomize(b, e->as.call_.args[i], &p);
            if (callee_colored(b, fn)) {
                CTerm *t = new_term(b, CT_TAILCALL);
                t->as.tailcall.fn = fn; t->as.tailcall.args = args;
                t->as.tailcall.n = n; t->as.tailcall.kont = kont;
                return fold_pending(b, &p, t);
            } else {
                CVar x = fresh_cvar(b, e->type.kind);
                CTerm *ac = new_term(b, CT_APPCONT);
                ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
                CTerm *t = new_term(b, CT_LETCALL);
                t->as.letcall.x = x; t->as.letcall.fn = fn;
                t->as.letcall.args = args; t->as.letcall.n = n; t->as.letcall.body = ac;
                return fold_pending(b, &p, t);
            }
        }
        case EX_IF: {
            Pending p = {0};
            CAtom c = atomize(b, e->as.if_.cond, &p);
            CTerm *t = new_term(b, CT_IF);
            t->as.if_.cond = c;
            t->as.if_.then_ = cps_tail(b, e->as.if_.then_, kont);
            t->as.if_.else_ = e->as.if_.else_or_null
                ? cps_tail(b, e->as.if_.else_or_null, kont)
                : cps_tail(b, NULL, kont);
            return fold_pending(b, &p, t);
        }
        case EX_LET: {
            CTerm *rest = cps_tail(b, e->as.let_.body, kont);
            for (int i = (int)e->as.let_.n - 1; i >= 0; i--)
                rest = cps_bind(b, e->as.let_.bindings[i].init,
                                cvar_of_binding(e->as.let_.bindings[i].binding), rest);
            return rest;
        }
        case EX_DO: {
            uint32_t n = e->as.do_.n;
            if (n == 0) return cps_tail(b, NULL, kont);
            CTerm *rest = cps_tail(b, e->as.do_.items[n - 1], kont);
            for (int i = (int)n - 2; i >= 0; i--) {
                CVar discard = fresh_cvar(b, e->as.do_.items[i]->type.kind);
                rest = cps_bind(b, e->as.do_.items[i], discard, rest);
            }
            return rest;
        }
        case EX_RETURN:
            return cps_tail(b, e->as.return_.value, b->retk);
        case EX_RESET: {
            CVar x = fresh_cvar(b, e->type.kind);
            CTerm *t = new_term(b, CT_RESET);
            t->as.reset.x = x;
            t->as.reset.delim = cps_tail(b, e->as.reset_.body, kont_prompt(e->type.kind));
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            t->as.reset.body = ac;
            return t;
        }
        case EX_SHIFT: {
            CVar k = fresh_cvar(b, e->type.kind);
            k.name = "k'";
            CTerm *t = new_term(b, CT_SHIFT);
            t->as.shift.k = k;
            t->as.shift.body = cps_shift_body(b, e);
            return t;
        }
        case EX_HANDLE: {
            CVar x = fresh_cvar(b, e->type.kind);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            return build_handle(b, e, x, ac);
        }
        case EX_PERFORM: {
            Pending p = {0};
            CVar x = fresh_cvar(b, e->type.kind);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            CTerm *core = build_perform(b, e, x, ac, &p);
            return fold_pending(b, &p, core);
        }
        case EX_RESUME: {
            Pending p = {0};
            CVar x = fresh_cvar(b, e->type.kind);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            CTerm *core = build_resume(b, e, x, ac, &p);
            return fold_pending(b, &p, core);
        }
        default: {
            CTerm *t = new_term(b, CT_UNSUPPORTED);
            t->as.unsupported.why = "form not in CPS2 subset";
            return t;
        }
    }
}

/* ---- cps_bind: bind e's value to x, then run rest --------------------- */

static CTerm *cps_bind(CpsB *b, Expr *e, CVar x, CTerm *rest) {
    e = (Expr *)ascribe_peel(e);
    if (!e) return rest;
    if (is_atomic(e)) {
        CTerm *t = new_term(b, CT_LETVAL);
        t->as.letval.x = x; t->as.letval.v = atom_of(e); t->as.letval.body = rest;
        return t;
    }
    switch (e->kind) {
        case EX_BUILTIN: {
            Pending p = {0};
            CAtom *args = arena_alloc(b->a, (e->as.builtin.n ? e->as.builtin.n : 1) * sizeof(CAtom));
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                args[i] = atomize(b, e->as.builtin.args[i], &p);
            CTerm *t = new_term(b, CT_LETPRIM);
            t->as.letprim.x = x; t->as.letprim.op = builtin_name(e);
            t->as.letprim.spec = e->as.builtin.spec;
            t->as.letprim.args = args; t->as.letprim.n = e->as.builtin.n;
            t->as.letprim.body = rest;
            return fold_pending(b, &p, t);
        }
        case EX_CALL: {
            const Binding *fn = e->as.call_.fn_binding;
            if (!fn) {
                CTerm *t = new_term(b, CT_UNSUPPORTED);
                t->as.unsupported.why = "indirect call";
                return t;
            }
            Pending p = {0};
            uint32_t n = e->as.call_.n_args;
            CAtom *args = arena_alloc(b->a, (n ? n : 1) * sizeof(CAtom));
            for (uint32_t i = 0; i < n; i++)
                args[i] = atomize(b, e->as.call_.args[i], &p);
            if (callee_colored(b, fn)) {
                CVar j = fresh_cvar(b, x.ty);
                j.name = arena_strdup(b->a, "j", 1);
                CTerm *call = new_term(b, CT_TAILCALL);
                call->as.tailcall.fn = fn; call->as.tailcall.args = args;
                call->as.tailcall.n = n; call->as.tailcall.kont = kont_var(j);
                CTerm *t = new_term(b, CT_LETCONT);
                t->as.letcont.j = j; t->as.letcont.param = x;
                t->as.letcont.jbody = rest; t->as.letcont.body = call;
                return fold_pending(b, &p, t);
            } else {
                CTerm *t = new_term(b, CT_LETCALL);
                t->as.letcall.x = x; t->as.letcall.fn = fn;
                t->as.letcall.args = args; t->as.letcall.n = n; t->as.letcall.body = rest;
                return fold_pending(b, &p, t);
            }
        }
        case EX_IF: {
            Pending p = {0};
            CAtom c = atomize(b, e->as.if_.cond, &p);
            CVar j = fresh_cvar(b, x.ty);
            j.name = arena_strdup(b->a, "j", 1);
            CTerm *body = new_term(b, CT_IF);
            body->as.if_.cond = c;
            body->as.if_.then_ = cps_tail(b, e->as.if_.then_, kont_var(j));
            body->as.if_.else_ = e->as.if_.else_or_null
                ? cps_tail(b, e->as.if_.else_or_null, kont_var(j))
                : cps_tail(b, NULL, kont_var(j));
            CTerm *t = new_term(b, CT_LETCONT);
            t->as.letcont.j = j; t->as.letcont.param = x;
            t->as.letcont.jbody = rest; t->as.letcont.body = body;
            return fold_pending(b, &p, t);
        }
        case EX_LET: {
            CTerm *r = cps_bind(b, e->as.let_.body, x, rest);
            for (int i = (int)e->as.let_.n - 1; i >= 0; i--)
                r = cps_bind(b, e->as.let_.bindings[i].init,
                             cvar_of_binding(e->as.let_.bindings[i].binding), r);
            return r;
        }
        case EX_DO: {
            uint32_t n = e->as.do_.n;
            if (n == 0) { return cps_bind(b, NULL, x, rest); }
            CTerm *r = cps_bind(b, e->as.do_.items[n - 1], x, rest);
            for (int i = (int)n - 2; i >= 0; i--) {
                CVar discard = fresh_cvar(b, e->as.do_.items[i]->type.kind);
                r = cps_bind(b, e->as.do_.items[i], discard, r);
            }
            return r;
        }
        case EX_RETURN:
            return cps_tail(b, e->as.return_.value, b->retk);
        case EX_RESET: {
            CTerm *t = new_term(b, CT_RESET);
            t->as.reset.x = x;
            t->as.reset.delim = cps_tail(b, e->as.reset_.body, kont_prompt(e->type.kind));
            t->as.reset.body = rest;
            return t;
        }
        case EX_SHIFT: {
            CVar k = fresh_cvar(b, e->type.kind);
            k.name = "k'";
            CTerm *t = new_term(b, CT_SHIFT);
            t->as.shift.k = k;
            t->as.shift.body = cps_shift_body(b, e);
            return t;
        }
        case EX_HANDLE:
            return build_handle(b, e, x, rest);
        case EX_PERFORM: {
            Pending p = {0};
            CTerm *core = build_perform(b, e, x, rest, &p);
            return fold_pending(b, &p, core);
        }
        case EX_RESUME: {
            Pending p = {0};
            CTerm *core = build_resume(b, e, x, rest, &p);
            return fold_pending(b, &p, core);
        }
        default: {
            CTerm *t = new_term(b, CT_UNSUPPORTED);
            t->as.unsupported.why = "form not in CPS2 subset";
            return t;
        }
    }
}

/* ---- public: translate a function ------------------------------------ */

CTerm *cps_ir_translate_fn(Arena *a, Expr *program, FnDef *fd) {
    if (!fd || !fd->body) return NULL;
    CpsB b;
    b.a = a; b.program = program; b.counter = 0;
    b.retk.kind = KK_RET; b.retk.id = 0; b.retk.ty = fd->return_type.kind;
    return cps_tail(&b, fd->body, b.retk);
}

/* ---- printing --------------------------------------------------------- */

static void print_indent(FILE *out, int n) { for (int i = 0; i < n; i++) fputs("  ", out); }

static const char *kind_name(TypeKind k) {
    switch (k) {
        case TY_INT:   return "int";
        case TY_BOOL:  return "bool";
        case TY_FLOAT: return "float";
        case TY_NIL:   return "nil";
        case TY_CSTR:  return "cstr";
        default:       return "_";
    }
}

static void print_atom(const CAtom *a, FILE *out) {
    switch (a->kind) {
        case CA_VAR:  fprintf(out, "%s", a->var && a->var->name ? a->var->name->name : "_"); break;
        case CA_CVAR: fprintf(out, "%s", a->cvar_name ? a->cvar_name : "_"); break;
        case CA_INT:  fprintf(out, "%lld", (long long)a->i); break;
        case CA_BOOL: fprintf(out, "%s", a->b ? "true" : "false"); break;
        case CA_UNIT: fprintf(out, "()"); break;
        case CA_STR:  fprintf(out, "\"%.*s\"", (int)a->str.len, a->str.p ? a->str.p : ""); break;
        default:      fprintf(out, "<val>"); break;
    }
}

static void print_kont(const CKont *k, FILE *out) {
    switch (k->kind) {
        case KK_RET:    fprintf(out, "k"); break;
        case KK_VAR:    fprintf(out, "j%u", k->id); break;
        case KK_PROMPT: fprintf(out, "<prompt>"); break;
    }
}

static void print_atoms(const CAtom *args, uint32_t n, FILE *out) {
    for (uint32_t i = 0; i < n; i++) { if (i) fputc(' ', out); print_atom(&args[i], out); }
}

void cps_ir_print(const CTerm *t, FILE *out, int indent) {
    if (!t) { print_indent(out, indent); fputs("<null>\n", out); return; }
    print_indent(out, indent);
    switch (t->kind) {
        case CT_APPCONT:
            fputc('(', out); print_kont(&t->as.appcont.kont, out); fputc(' ', out);
            print_atom(&t->as.appcont.v, out); fputs(")\n", out);
            break;
        case CT_LETVAL:
            fprintf(out, "let %s = ", t->as.letval.x.name);
            print_atom(&t->as.letval.v, out); fputs("\n", out);
            cps_ir_print(t->as.letval.body, out, indent);
            break;
        case CT_LETPRIM:
            fprintf(out, "let %s = (%s ", t->as.letprim.x.name, t->as.letprim.op);
            print_atoms(t->as.letprim.args, t->as.letprim.n, out); fputs(")\n", out);
            cps_ir_print(t->as.letprim.body, out, indent);
            break;
        case CT_LETCALL:
            fprintf(out, "let %s = call %s(", t->as.letcall.x.name,
                    t->as.letcall.fn && t->as.letcall.fn->name ? t->as.letcall.fn->name->name : "?");
            print_atoms(t->as.letcall.args, t->as.letcall.n, out);
            fputs(")  ; cps->direct\n", out);
            cps_ir_print(t->as.letcall.body, out, indent);
            break;
        case CT_TAILCALL:
            fprintf(out, "tailcall %s(",
                    t->as.tailcall.fn && t->as.tailcall.fn->name ? t->as.tailcall.fn->name->name : "?");
            print_atoms(t->as.tailcall.args, t->as.tailcall.n, out);
            if (t->as.tailcall.n) fputc(' ', out);
            print_kont(&t->as.tailcall.kont, out); fputs(")  ; cps->cps\n", out);
            break;
        case CT_LETCONT:
            fprintf(out, "letcont j%u(%s) =\n", t->as.letcont.j.id, t->as.letcont.param.name);
            cps_ir_print(t->as.letcont.jbody, out, indent + 1);
            print_indent(out, indent); fputs("in\n", out);
            cps_ir_print(t->as.letcont.body, out, indent);
            break;
        case CT_IF:
            fputs("if ", out); print_atom(&t->as.if_.cond, out); fputs("\n", out);
            print_indent(out, indent); fputs("then\n", out);
            cps_ir_print(t->as.if_.then_, out, indent + 1);
            print_indent(out, indent); fputs("else\n", out);
            cps_ir_print(t->as.if_.else_, out, indent + 1);
            break;
        case CT_RESET:
            fprintf(out, "reset %s {\n", t->as.reset.x.name);
            cps_ir_print(t->as.reset.delim, out, indent + 1);
            print_indent(out, indent); fputs("}\n", out);
            cps_ir_print(t->as.reset.body, out, indent);
            break;
        case CT_SHIFT:
            fprintf(out, "shift %s.\n", t->as.shift.k.name);
            cps_ir_print(t->as.shift.body, out, indent + 1);
            break;
        case CT_HANDLE:
            fprintf(out, "handle %s = {\n", t->as.handle.x.name);
            cps_ir_print(t->as.handle.delim, out, indent + 1);
            print_indent(out, indent);
            fprintf(out, "} with %s(",
                    t->as.handle.effect ? t->as.handle.effect->name : "?");
            for (uint32_t i = 0; i < t->as.handle.n_params; i++) {
                if (i) fputc(' ', out);
                fputs(t->as.handle.params[i] && t->as.handle.params[i]->name
                          ? t->as.handle.params[i]->name->name : "_", out);
            }
            fprintf(out, ") %s ->\n",
                    t->as.handle.k && t->as.handle.k->name ? t->as.handle.k->name->name : "k");
            cps_ir_print(t->as.handle.case_body, out, indent + 1);
            print_indent(out, indent); fputs("in\n", out);
            cps_ir_print(t->as.handle.body, out, indent);
            break;
        case CT_PERFORM:
            fprintf(out, "let %s = perform %s(", t->as.perform.x.name,
                    t->as.perform.effect ? t->as.perform.effect->name : "?");
            print_atoms(t->as.perform.args, t->as.perform.n, out); fputs(")\n", out);
            cps_ir_print(t->as.perform.body, out, indent);
            break;
        case CT_RESUME:
            fprintf(out, "let %s = resume ", t->as.resume.x.name);
            print_atom(&t->as.resume.k, out); fputc(' ', out);
            print_atom(&t->as.resume.v, out); fputs("\n", out);
            cps_ir_print(t->as.resume.body, out, indent);
            break;
        case CT_UNSUPPORTED:
            fprintf(out, "<unsupported: %s>\n", t->as.unsupported.why ? t->as.unsupported.why : "?");
            break;
    }
}

/* ---- CPS3: direct<->CPS boundary classification ----------------------- */

/* Does e's subtree resolved-call `target`? (no descent into nested fns). */
static bool body_calls_binding(const Expr *e, const Binding *target) {
    if (!e) return false;
    switch (e->kind) {
        case EX_CALL:
            if (e->as.call_.fn_binding == target) return true;
            if (body_calls_binding(e->as.call_.fn_expr, target)) return true;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (body_calls_binding(e->as.call_.args[i], target)) return true;
            return false;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (body_calls_binding(e->as.let_.bindings[i].init, target)) return true;
            return body_calls_binding(e->as.let_.body, target);
        case EX_IF:
            return body_calls_binding(e->as.if_.cond, target)
                || body_calls_binding(e->as.if_.then_, target)
                || body_calls_binding(e->as.if_.else_or_null, target);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (body_calls_binding(e->as.do_.items[i], target)) return true;
            return false;
        case EX_WHILE:
            return body_calls_binding(e->as.while_.cond, target)
                || body_calls_binding(e->as.while_.body, target);
        case EX_SET:    return body_calls_binding(e->as.set_.value, target);
        case EX_DEF:    return body_calls_binding(e->as.def_.init, target);
        case EX_RETURN: return body_calls_binding(e->as.return_.value, target);
        case EX_DEFER:  return body_calls_binding(e->as.defer_.body, target);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (body_calls_binding(e->as.builtin.args[i], target)) return true;
            return false;
        case EX_RESET:  return body_calls_binding(e->as.reset_.body, target);
        case EX_SHIFT:
            return body_calls_binding(e->as.shift_.k_fn, target)
                || body_calls_binding(e->as.shift_.body, target);
        case EX_SHIFT0:
            return body_calls_binding(e->as.shift0_.k_fn, target)
                || body_calls_binding(e->as.shift0_.body, target);
        /* nested fn definitions are boundaries */
        case EX_FN_DEF: case EX_FN: case EX_CLOSURE:
            return false;
        default:
            return false;
    }
}

/* A colored function is a direct->CPS *entry* (needs a driver to supply the
 * initial continuation) if no colored function resolved-calls it. */
static bool is_cps_entry(Expr *program, FnDef *fd) {
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        Expr *it = program->as.program.items[i];
        if (!it || it->kind != EX_FN_DEF || !it->as.fn_def_.fn) continue;
        FnDef *g = it->as.fn_def_.fn;
        if (g == fd || !g->cps_colored) continue;
        if (body_calls_binding(g->body, fd->binding)) return false;
    }
    return true;
}

/* ---- public: dump all colored functions ------------------------------ */

void cps_ir_dump_program(Arena *a, Expr *program, FILE *out) {
    if (!program || program->kind != EX_PROGRAM || !out) return;
    cps_color_program(a, program);
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        Expr *it = program->as.program.items[i];
        if (!it || it->kind != EX_FN_DEF || !it->as.fn_def_.fn) continue;
        FnDef *fd = it->as.fn_def_.fn;
        if (!fd->cps_colored) continue;
        if (!fd->binding || !fd->binding->name) continue;
        if (fd->binding->defining_module_name) continue;
        fprintf(out, "cps-fn %s [", fd->binding->name->name);
        for (uint8_t pi = 0; pi < fd->n_params; pi++) {
            if (pi) fputc(' ', out);
            fprintf(out, "%s", fd->params[pi]->name ? fd->params[pi]->name->name : "_");
        }
        fprintf(out, "] k:cont<%s> %s\n", kind_name(fd->return_type.kind),
                is_cps_entry(program, fd) ? "entry" : "internal");
        CTerm *t = cps_ir_translate_fn(a, program, fd);
        cps_ir_print(t, out, 1);
        fputs("cps-end\n", out);
    }
}
