#include "emit_cps_ir.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "cps_ir.h"
#include "cps.h"
#include "builtins.h"
#include "globals.h"
#include "experiments.h"
#include "arena.h"
#include "expr.h"

/* =========================================================================
 * CPS-IR-to-C backend -- Phase C1.  See emit_cps_ir.h for the ABI and scope.
 *
 * Two responsibilities:
 *   1. Whole-program classification (ensure_S): which colored functions the
 *      backend emits.  A function is emittable iff its CTerm is in the C1 core
 *      subset (scalar types, no reset/shift/effects, no unsupported form) AND
 *      no join point in it must be reified onto the heap chain -- i.e. no
 *      non-tail call to another emittable function.  That last clause is a
 *      monotone fixpoint (removing a function from the set only turns its
 *      callers' cps->cps edges into ordinary synchronous calls, which never
 *      forces a heap join), so it converges.
 *   2. Emission (emit_cps_ir_try_fn): walk the CTerm and write the DK-ABI body
 *      plus a direct-entry wrapper.
 *
 * Join points (CT_LETCONT) lower to a C local + a forward label/goto (plan:
 * "local labels/gotos"): delivering a value to join j assigns the join
 * parameter and `goto L<j>`.  This is sufficient for every join whose
 * continuation stays on the C stack (if-merges and the result of a synchronous
 * call).  A join that would have to survive a cps->cps call needs a real
 * dk_frame; that is deliberately out of the C1 subset -- such functions fall
 * back -- and lands in a later phase.
 * ========================================================================= */

/* ---- the C1 emittable type set --------------------------------------- */

static bool tk_scalar(TypeKind k) {
    return k == TY_INT || k == TY_BOOL || k == TY_INT64;
}

/* Atom types we can materialize as a C scalar. */
static bool atom_ok(const CAtom *a) {
    switch (a->kind) {
        case CA_INT:  return a->ty == TY_INT || a->ty == TY_INT64 || a->ty == TY_UNKNOWN;
        case CA_BOOL: return true;
        case CA_UNIT: return true;   /* nil placeholder (0) */
        case CA_VAR:
        case CA_CVAR: return tk_scalar(a->ty) || a->ty == TY_NIL;
        default:      return false;  /* CA_OTHER: float/str/etc. */
    }
}

static bool is_println_shape(BuiltinShape s) {
    return s == BS_PRINTLN_INT || s == BS_PRINTLN_BOOL || s == BS_PRINTLN_UINT;
}

static bool shape_supported(const BuiltinSpec *sp) {
    if (!sp) return false;
    switch (sp->shape) {
        case BS_BIN_INFIX:
        case BS_VARIADIC_FOLD:
        case BS_DIV_CHECK:
        case BS_PREFIX_UNARY:
        case BS_AND_SC:
        case BS_OR_SC:
        case BS_PRINTLN_INT:
        case BS_PRINTLN_BOOL:
        case BS_PRINTLN_UINT:
            return true;
        default:
            return false;
    }
}

/* Recursively check that every node lies in the C1 core subset.  Does not
 * consult the emittable set -- the cps->cps join clause is handled separately
 * by the fixpoint (needs_heap_join). */
static bool term_core_ok(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            return t->as.appcont.kont.kind != KK_PROMPT && atom_ok(&t->as.appcont.v);
        case CT_LETVAL:
            return (tk_scalar(t->as.letval.x.ty) || t->as.letval.x.ty == TY_NIL)
                && atom_ok(&t->as.letval.v)
                && term_core_ok(t->as.letval.body);
        case CT_LETPRIM: {
            if (!shape_supported(t->as.letprim.spec)) return false;
            for (uint32_t i = 0; i < t->as.letprim.n; i++)
                if (!atom_ok(&t->as.letprim.args[i])) return false;
            return term_core_ok(t->as.letprim.body);
        }
        case CT_LETCALL: {
            if (!(tk_scalar(t->as.letcall.x.ty) || t->as.letcall.x.ty == TY_NIL))
                return false;
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!atom_ok(&t->as.letcall.args[i])) return false;
            return term_core_ok(t->as.letcall.body);
        }
        case CT_TAILCALL: {
            if (t->as.tailcall.kont.kind == KK_PROMPT) return false;
            for (uint32_t i = 0; i < t->as.tailcall.n; i++)
                if (!atom_ok(&t->as.tailcall.args[i])) return false;
            return true;
        }
        case CT_LETCONT:
            return (tk_scalar(t->as.letcont.param.ty) || t->as.letcont.param.ty == TY_NIL)
                && term_core_ok(t->as.letcont.jbody)
                && term_core_ok(t->as.letcont.body);
        case CT_IF:
            return atom_ok(&t->as.if_.cond)
                && term_core_ok(t->as.if_.then_)
                && term_core_ok(t->as.if_.else_);
        default: /* CT_RESET, CT_SHIFT, CT_UNSUPPORTED */
            return false;
    }
}

static bool fn_sig_ok(const FnDef *fd) {
    if (!tk_scalar(fd->return_type.kind)) return false;   /* nil-return excluded in C1 */
    for (uint8_t i = 0; i < fd->n_params; i++)
        if (!tk_scalar(fd->params[i]->type.kind)) return false;
    return true;
}

/* ---- whole-program classification cache ------------------------------ */

typedef struct { const FnDef *fd; const Binding *bind; CTerm *term; bool in_s; } SEnt;

static const Expr *g_prog;      /* program the cache is keyed on */
static Arena       g_arena;     /* owns the cached CTerms (+ coloring) */
static bool        g_arena_live;
static SEnt       *g_ents;
static size_t      g_ents_n;
static bool        g_fwd_done;  /* __cps forward decls emitted for g_prog */

static bool binding_in_s(const Binding *b) {
    if (!b) return false;
    for (size_t i = 0; i < g_ents_n; i++)
        if (g_ents[i].bind == b) return g_ents[i].in_s;
    return false;
}

/* Does `t` contain a non-tail cps->cps call -- a CT_TAILCALL whose result is
 * bound by a join (kont is a KK_VAR) and whose callee is currently emittable?
 * Such a call must reify the join onto the heap chain, which C1 does not do. */
static bool needs_heap_join(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT: return false;
        case CT_LETVAL:  return needs_heap_join(t->as.letval.body);
        case CT_LETPRIM: return needs_heap_join(t->as.letprim.body);
        case CT_LETCALL: return needs_heap_join(t->as.letcall.body);
        case CT_TAILCALL:
            return t->as.tailcall.kont.kind == KK_VAR
                && binding_in_s(t->as.tailcall.fn);
        case CT_LETCONT:
            return needs_heap_join(t->as.letcont.jbody)
                || needs_heap_join(t->as.letcont.body);
        case CT_IF:
            return needs_heap_join(t->as.if_.then_)
                || needs_heap_join(t->as.if_.else_);
        default: return false;
    }
}

static void ensure_S(const Expr *program) {
    if (g_prog == program && g_ents) return;   /* cached */

    if (g_arena_live) { arena_free(&g_arena); g_arena_live = false; }
    free(g_ents); g_ents = NULL; g_ents_n = 0;
    g_prog = program;
    g_fwd_done = false;
    if (!program || program->kind != EX_PROGRAM) return;

    arena_init(&g_arena, 0);
    g_arena_live = true;

    /* Coloring is idempotent (cps.c resets first); guarantee it ran. */
    cps_color_program(&g_arena, (Expr *)program);

    /* Collect candidates: colored top-level fns with a core-only CTerm and a
     * scalar signature. */
    uint32_t np = program->as.program.n;
    g_ents = (SEnt *)calloc(np ? np : 1, sizeof(SEnt));
    for (uint32_t i = 0; i < np; i++) {
        Expr *it = program->as.program.items[i];
        if (!it || it->kind != EX_FN_DEF || !it->as.fn_def_.fn) continue;
        FnDef *fd = it->as.fn_def_.fn;
        if (!fd->cps_colored || !fd->binding || !fd->body) continue;
        /* `main` is the program entry point (int main(int, char**)); an exported
         * binding names a fixed C symbol/linkage.  Neither can take the
         * static int64_t entry-wrapper shape, so keep them direct-style. */
        if (fd->binding->c_export_name) continue;
        if (fd->binding->name && fd->binding->name->len == 4 &&
            memcmp(fd->binding->name->name, "main", 4) == 0) continue;
        if (!fn_sig_ok(fd)) continue;
        CTerm *t = cps_ir_translate_fn(&g_arena, (Expr *)program, fd);
        if (!term_core_ok(t)) continue;
        g_ents[g_ents_n].fd = fd;
        g_ents[g_ents_n].bind = fd->binding;
        g_ents[g_ents_n].term = t;
        g_ents[g_ents_n].in_s = true;
        g_ents_n++;
    }

    /* Fixpoint: drop any candidate that needs a heap-reified join.  Monotone
     * (only removals), so it converges. */
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < g_ents_n; i++) {
            if (g_ents[i].in_s && needs_heap_join(g_ents[i].term)) {
                g_ents[i].in_s = false;
                changed = true;
            }
        }
    }
}

static SEnt *ent_of(const FnDef *fd) {
    for (size_t i = 0; i < g_ents_n; i++)
        if (g_ents[i].fd == fd) return &g_ents[i];
    return NULL;
}

/* ---- emission -------------------------------------------------------- */

#define MAX_JOINS 64

typedef struct {
    EmitCtx *ctx;
    Buf     *out;
    int      indent;
    /* active inline joins: KK_VAR id -> join-parameter C name */
    struct { uint32_t id; const char *param; } joins[MAX_JOINS];
    int      n_joins;
} CE;

static void ce_line(CE *ce, const char *fmt, ...) {
    indent_buf(ce->out, ce->indent);
    va_list ap; va_start(ap, fmt);
    buf_vprintf(ce->out, fmt, ap);
    va_end(ap);
    buf_putc(ce->out, '\n');
}

static const char *join_param(CE *ce, uint32_t id) {
    for (int i = ce->n_joins - 1; i >= 0; i--)
        if (ce->joins[i].id == id) return ce->joins[i].param;
    return "0";  /* unreachable when the term is well-formed */
}

/* Materialize an atom as a malloc'd C expression string. */
static char *atom_str(CE *ce, const CAtom *a) {
    switch (a->kind) {
        case CA_INT:  return atom_int_typed(a->i, TY_INT);
        case CA_BOOL: return atom_bool(a->b);
        case CA_UNIT: return strdup("0");
        case CA_VAR:  return atom_var(ce->ctx, a->var);
        case CA_CVAR: return strdup(a->cvar_name ? a->cvar_name : "0");
        default:      return strdup("0");
    }
}

/* Build the RHS C expression for a supported non-println builtin. */
static char *prim_expr(const BuiltinSpec *sp, char **as, uint32_t n) {
    Buf b; buf_init(&b);
    switch (sp->shape) {
        case BS_BIN_INFIX:
            buf_printf(&b, "(%s) %s (%s)", as[0], sp->c_op, as[1]);
            break;
        case BS_VARIADIC_FOLD: {
            uint32_t opens = (n >= 2) ? n - 2 : 0;
            for (uint32_t i = 0; i < opens; i++) buf_putc(&b, '(');
            buf_printf(&b, "(%s)", as[0]);
            for (uint32_t i = 1; i < n; i++) {
                buf_printf(&b, " %s (%s)", sp->c_op, as[i]);
                if (i < n - 1) buf_putc(&b, ')');
            }
            break;
        }
        case BS_DIV_CHECK:
            buf_printf(&b,
                "((%s) ? ((%s) / (%s)) : "
                "(fprintf(stderr, \"division by zero\\n\"), abort(), 0))",
                as[1], as[0], as[1]);
            break;
        case BS_PREFIX_UNARY:
            buf_printf(&b, "%s(%s)", sp->c_op, as[0]);
            break;
        case BS_AND_SC:
        case BS_OR_SC: {
            const char *op = (sp->shape == BS_AND_SC) ? "&&" : "||";
            for (uint32_t i = 0; i < n; i++) {
                if (i) buf_printf(&b, " %s ", op);
                buf_printf(&b, "(%s)", as[i]);
            }
            if (n == 0) buf_puts(&b, (sp->shape == BS_AND_SC) ? "1" : "0");
            break;
        }
        default:
            buf_puts(&b, "0");
            break;
    }
    buf_putc(&b, '\0');
    char *s = strdup(b.data);
    buf_free(&b);
    return s;
}

static char *callee_name(const Binding *fn) {
    return raw_name_for_binding(fn);   /* malloc'd */
}

/* Join a term's atom arguments into a malloc'd "a0, a1, ..." string. */
static char *atoms_csv(CE *ce, const CAtom *args, uint32_t n) {
    Buf b; buf_init(&b);
    for (uint32_t i = 0; i < n; i++) {
        if (i) buf_puts(&b, ", ");
        char *a = atom_str(ce, &args[i]);
        buf_puts(&b, a);
        free(a);
    }
    buf_putc(&b, '\0');
    char *s = strdup(b.data);
    buf_free(&b);
    return s;
}

static void emit_term(CE *ce, const CTerm *t);

/* Deliver value-string `v` to continuation `kont`. */
static void emit_deliver(CE *ce, const CKont *kont, const char *v) {
    if (kont->kind == KK_RET) {
        ce_line(ce, "return dk_run(k, (intptr_t)(%s));", v);
    } else { /* KK_VAR: an inline join */
        ce_line(ce, "%s = %s;", join_param(ce, kont->id), v);
        ce_line(ce, "goto L%u;", kont->id);
    }
}

static void emit_println(CE *ce, BuiltinShape shape, const char *arg) {
    switch (shape) {
        case BS_PRINTLN_INT:
            ce_line(ce, "printf(\"%%lld\\n\", (long long)(%s));", arg);
            break;
        case BS_PRINTLN_UINT:
            ce_line(ce, "printf(\"%%llu\\n\", (unsigned long long)(%s));", arg);
            break;
        case BS_PRINTLN_BOOL:
            ce_line(ce, "puts((%s) ? \"true\" : \"false\");", arg);
            break;
        default: break;
    }
}

static void emit_term(CE *ce, const CTerm *t) {
    switch (t->kind) {
        case CT_APPCONT: {
            char *v = atom_str(ce, &t->as.appcont.v);
            emit_deliver(ce, &t->as.appcont.kont, v);
            free(v);
            break;
        }
        case CT_LETVAL: {
            char *v = atom_str(ce, &t->as.letval.v);
            ce_line(ce, "%s = %s;", t->as.letval.x.name, v);
            free(v);
            emit_term(ce, t->as.letval.body);
            break;
        }
        case CT_LETPRIM: {
            uint32_t n = t->as.letprim.n;
            char **as = (char **)calloc(n ? n : 1, sizeof(char *));
            for (uint32_t i = 0; i < n; i++) as[i] = atom_str(ce, &t->as.letprim.args[i]);
            const BuiltinSpec *sp = t->as.letprim.spec;
            if (is_println_shape(sp->shape)) {
                emit_println(ce, sp->shape, n ? as[0] : "0");
                ce_line(ce, "%s = 0;", t->as.letprim.x.name);  /* nil result */
            } else {
                char *rhs = prim_expr(sp, as, n);
                ce_line(ce, "%s = %s;", t->as.letprim.x.name, rhs);
                free(rhs);
            }
            for (uint32_t i = 0; i < n; i++) free(as[i]);
            free(as);
            emit_term(ce, t->as.letprim.body);
            break;
        }
        case CT_LETCALL: {
            char *fn = callee_name(t->as.letcall.fn);
            char *argv = atoms_csv(ce, t->as.letcall.args, t->as.letcall.n);
            ce_line(ce, "%s = %s(%s);", t->as.letcall.x.name, fn, argv);
            free(fn); free(argv);
            emit_term(ce, t->as.letcall.body);
            break;
        }
        case CT_TAILCALL: {
            char *fn = callee_name(t->as.tailcall.fn);
            char *argv = atoms_csv(ce, t->as.tailcall.args, t->as.tailcall.n);
            if (binding_in_s(t->as.tailcall.fn)) {
                /* cps->cps: thread the continuation.  In the C1 subset the kont
                 * is always KK_RET here (a KK_VAR would need a heap join, which
                 * excludes the caller from the emittable set). */
                if (t->as.tailcall.n)
                    ce_line(ce, "return %s__cps(%s, k);", fn, argv);
                else
                    ce_line(ce, "return %s__cps(k);", fn);
            } else {
                /* Synchronous call to a direct/fallback function, then deliver
                 * the result to the continuation (cps->direct bridge). */
                char *tmp = fresh_tmp(ce->ctx);
                ce_line(ce, "int64_t %s = %s(%s);", tmp, fn, argv);
                emit_deliver(ce, &t->as.tailcall.kont, tmp);
                free(tmp);
            }
            free(fn); free(argv);
            break;
        }
        case CT_LETCONT: {
            if (ce->n_joins < MAX_JOINS) {
                ce->joins[ce->n_joins].id = t->as.letcont.j.id;
                ce->joins[ce->n_joins].param = t->as.letcont.param.name;
                ce->n_joins++;
            }
            emit_term(ce, t->as.letcont.body);
            if (ce->n_joins > 0) ce->n_joins--;
            /* the join landing pad */
            indent_buf(ce->out, ce->indent);
            buf_printf(ce->out, "L%u:;\n", t->as.letcont.j.id);
            emit_term(ce, t->as.letcont.jbody);
            break;
        }
        case CT_IF: {
            char *c = atom_str(ce, &t->as.if_.cond);
            ce_line(ce, "if (%s) {", c);
            free(c);
            ce->indent += 4;
            emit_term(ce, t->as.if_.then_);
            ce->indent -= 4;
            ce_line(ce, "} else {");
            ce->indent += 4;
            emit_term(ce, t->as.if_.else_);
            ce->indent -= 4;
            ce_line(ce, "}");
            break;
        }
        default:
            ce_line(ce, "abort(); /* cps-backend: unreachable */");
            break;
    }
}

/* ---- binder pre-declaration ------------------------------------------ *
 * Every let-bound C variable is declared (uninitialized) at the top of the
 * function so that forward `goto`s into a join never jump past a declaration.
 * All values thread as machine scalars, so int64_t is the uniform storage. */
static void emit_binder_decls(CE *ce, const CTerm *t) {
    if (!t) return;
    switch (t->kind) {
        case CT_APPCONT: break;
        case CT_LETVAL:
            ce_line(ce, "int64_t %s;", t->as.letval.x.name);
            emit_binder_decls(ce, t->as.letval.body);
            break;
        case CT_LETPRIM:
            ce_line(ce, "int64_t %s;", t->as.letprim.x.name);
            emit_binder_decls(ce, t->as.letprim.body);
            break;
        case CT_LETCALL:
            ce_line(ce, "int64_t %s;", t->as.letcall.x.name);
            emit_binder_decls(ce, t->as.letcall.body);
            break;
        case CT_TAILCALL: break;
        case CT_LETCONT:
            ce_line(ce, "int64_t %s;", t->as.letcont.param.name);
            emit_binder_decls(ce, t->as.letcont.body);
            emit_binder_decls(ce, t->as.letcont.jbody);
            break;
        case CT_IF:
            emit_binder_decls(ce, t->as.if_.then_);
            emit_binder_decls(ce, t->as.if_.else_);
            break;
        default: break;
    }
}

/* ---- signatures ------------------------------------------------------ */

static void emit_params(EmitCtx *ctx, Buf *file, const FnDef *fd) {
    for (uint8_t i = 0; i < fd->n_params; i++) {
        if (i) buf_puts(file, ", ");
        const char *pty = emit_type_c_name(ctx, emit_type_from_kind(fd->params[i]->type.kind));
        char *pn = name_for_binding(ctx, fd->params[i]);
        buf_printf(file, "%s %s", pty, pn);
        free(pn);
    }
}

/* Emit forward declarations for every emittable __cps function, once per
 * program.  Goes into `file` (appended after the DK prelude in `out`). */
static void emit_forward_decls(EmitCtx *ctx, Buf *file) {
    for (size_t i = 0; i < g_ents_n; i++) {
        if (!g_ents[i].in_s) continue;
        const FnDef *fd = g_ents[i].fd;
        char *cn = raw_name_for_binding(fd->binding);
        buf_printf(file, "static int64_t %s__cps(", cn);
        emit_params(ctx, file, fd);
        if (fd->n_params) buf_puts(file, ", ");
        buf_puts(file, "DK *k);\n");
        free(cn);
    }
}

bool emit_cps_ir_program_has_emittable(const Expr *program) {
    if (!g_opt_cps_backend) return false;
    ensure_S(program);
    for (size_t i = 0; i < g_ents_n; i++)
        if (g_ents[i].in_s) return true;
    return false;
}

bool emit_cps_ir_try_fn(EmitCtx *ctx, Buf *file, const Expr *e) {
    if (!g_opt_cps_backend) return false;
    if (!e || e->kind != EX_FN_DEF || !e->as.fn_def_.fn) return false;
    FnDef *fd = e->as.fn_def_.fn;
    if (!fd->binding) return false;
    const Expr *program = ctx->program_root;
    if (!program) return false;

    /* ensure_S colors the program (idempotently) and classifies it; the
     * pipeline has not colored by emit time, so this must precede any read of
     * fd->cps_colored / g_ents. */
    ensure_S(program);
    SEnt *se = ent_of(fd);
    if (!se || !se->in_s) return false;   /* not emittable -> caller falls back */

    experiment_warn_if_used("cps-backend");

    if (!g_fwd_done) { emit_forward_decls(ctx, file); g_fwd_done = true; }

    char *cn = raw_name_for_binding(fd->binding);

    /* ---- CPS body: int64_t <name>__cps(<params>, DK *k) ---- */
    buf_printf(file, "static int64_t %s__cps(", cn);
    emit_params(ctx, file, fd);
    if (fd->n_params) buf_puts(file, ", ");
    buf_puts(file, "DK *k) {\n");

    CE ce; memset(&ce, 0, sizeof(ce));
    ce.ctx = ctx; ce.out = file; ce.indent = 4;
    emit_binder_decls(&ce, se->term);
    emit_term(&ce, se->term);
    buf_puts(file, "}\n");

    /* ---- direct-entry wrapper: int64_t <name>(<params>) ----
     * Seeds a dk_done()-terminated root continuation so uncolored/direct
     * callers reach the CPS body by the plain name unchanged. */
    buf_printf(file, "__attribute__((unused)) static int64_t %s(", cn);
    emit_params(ctx, file, fd);
    buf_puts(file, ") {\n    return ");
    buf_printf(file, "%s__cps(", cn);
    for (uint8_t i = 0; i < fd->n_params; i++) {
        if (i) buf_puts(file, ", ");
        char *pn = name_for_binding(ctx, fd->params[i]);
        buf_puts(file, pn);
        free(pn);
    }
    if (fd->n_params) buf_puts(file, ", ");
    buf_puts(file, "dk_done());\n}\n");

    free(cn);
    return true;
}
