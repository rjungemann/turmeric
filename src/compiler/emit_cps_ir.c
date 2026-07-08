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

/* ---- the emittable value set (non-scalar Tier A) --------------------- *
 * A type is "slot-representable at Tier A" when its value fits the one-word DK
 * slot by a plain cast: any <=64-bit integer/bool, or a cstr/pointer.  See
 * docs/upcoming/v1/cps-backend-non-scalar-values-plan.md.  Tier B (float, bit-
 * reinterpret) and Tier C (wide by-value aggregates, boxed) are not yet here. */
static bool slot_ty(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_INT64: case TY_BOOL:
        case TY_INT8: case TY_INT16: case TY_INT32:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_CSTR:
            return true;
        default:
            return false;
    }
}

/* The C type used to declare a let-binder of type kind `ty` (TY_NIL binders are
 * unit placeholders stored as int64_t). */
static const char *binder_ctype(EmitCtx *ctx, TypeKind ty) {
    if (ty == TY_NIL || ty == TY_UNKNOWN) return "int64_t";
    return emit_type_c_name(ctx, emit_type_from_kind(ty));
}

/* Atom types we can materialize as a slot-representable value. */
static bool atom_ok(const CAtom *a) {
    switch (a->kind) {
        case CA_INT:  return a->ty == TY_INT || a->ty == TY_INT64 || a->ty == TY_UNKNOWN;
        case CA_BOOL: return true;
        case CA_UNIT: return true;   /* nil placeholder (0) */
        case CA_STR:  return true;   /* cstr literal (Tier A pointer) */
        case CA_VAR:
        case CA_CVAR: return slot_ty(a->ty) || a->ty == TY_NIL;
        default:      return false;  /* CA_OTHER: float/etc. */
    }
}

static bool is_println_shape(BuiltinShape s) {
    return s == BS_PRINTLN_INT || s == BS_PRINTLN_BOOL || s == BS_PRINTLN_UINT
        || s == BS_PRINTLN_CSTR;
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
        case BS_PRINTLN_CSTR:
            return true;
        default:
            return false;
    }
}

static bool term_core_ok(const CTerm *t);

/* ---- C3 delimited-control subset predicates -------------------------- *
 * The C3 backend lowers a reset/shift only in a restricted, zero-capture,
 * identity-receiver subset; anything else keeps its direct-style emission.
 * These predicates gate that subset. */

#define CC_MAX_BOUND 128

/* Source bindings that are NOT captures inside the lifted body being checked
 * (an effect handler case's params and continuation `k`, bound by the DKHandler
 * signature).  Set by has_capture() before each walk. */
static const Binding *g_excl_b[8];
static int            g_excl_n;

static bool binding_excluded(const Binding *b) {
    for (int i = 0; i < g_excl_n; i++) if (g_excl_b[i] == b) return true;
    return false;
}

/* True if `t` references a free variable that a lifted continuation / shift /
 * handler-case helper would have to capture: a non-global, non-excluded source
 * var, or a CPS var not bound within `t` and not the excluded incoming value
 * `exclude` (UINT32_MAX = none).  The zero-capture cut falls back when it holds. */
static bool has_capture_rec(const CTerm *t, uint32_t exclude,
                            uint32_t *bound, int nb) {
    if (!t || nb >= CC_MAX_BOUND) return t ? true : false;
    #define CC_ATOM(a) do { const CAtom *_a = (a); \
        if (_a->kind == CA_VAR) { if (_a->var && !_a->var->is_global && !binding_excluded(_a->var)) return true; } \
        else if (_a->kind == CA_CVAR) { \
            if (_a->cvar_id != exclude) { bool _f = true; \
                for (int _i = 0; _i < nb; _i++) if (bound[_i] == _a->cvar_id) { _f = false; break; } \
                if (_f) return true; } } } while (0)
    switch (t->kind) {
        case CT_APPCONT: CC_ATOM(&t->as.appcont.v); return false;
        case CT_LETVAL:
            CC_ATOM(&t->as.letval.v);
            bound[nb] = t->as.letval.x.id;
            return has_capture_rec(t->as.letval.body, exclude, bound, nb + 1);
        case CT_LETPRIM:
            for (uint32_t i = 0; i < t->as.letprim.n; i++) CC_ATOM(&t->as.letprim.args[i]);
            bound[nb] = t->as.letprim.x.id;
            return has_capture_rec(t->as.letprim.body, exclude, bound, nb + 1);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++) CC_ATOM(&t->as.letcall.args[i]);
            bound[nb] = t->as.letcall.x.id;
            return has_capture_rec(t->as.letcall.body, exclude, bound, nb + 1);
        case CT_TAILCALL:
            for (uint32_t i = 0; i < t->as.tailcall.n; i++) CC_ATOM(&t->as.tailcall.args[i]);
            return false;
        case CT_IF:
            CC_ATOM(&t->as.if_.cond);
            return has_capture_rec(t->as.if_.then_, exclude, bound, nb)
                || has_capture_rec(t->as.if_.else_, exclude, bound, nb);
        case CT_LETCONT:
            bound[nb] = t->as.letcont.param.id;
            return has_capture_rec(t->as.letcont.jbody, exclude, bound, nb + 1)
                || has_capture_rec(t->as.letcont.body, exclude, bound, nb + 1);
        case CT_RESUME:
            CC_ATOM(&t->as.resume.k);   /* k is the handler's excluded continuation */
            CC_ATOM(&t->as.resume.v);
            bound[nb] = t->as.resume.x.id;
            return has_capture_rec(t->as.resume.body, exclude, bound, nb + 1);
        default: return true;   /* nested reset/shift/handle/perform in a lifted body: bail */
    }
    #undef CC_ATOM
}

static bool has_capture(const CTerm *t, uint32_t exclude) {
    uint32_t bound[CC_MAX_BOUND];
    g_excl_n = 0;
    return has_capture_rec(t, exclude, bound, 0);
}

/* Like has_capture, but the handler case's params + continuation k are excluded
 * (they are bound by the DKHandler signature, not captured). */
static bool has_capture_case(const CTerm *t, const CTerm *hnode) {
    uint32_t bound[CC_MAX_BOUND];
    g_excl_n = 0;
    for (uint32_t i = 0; i < hnode->as.handle.n_params && g_excl_n < 8; i++)
        g_excl_b[g_excl_n++] = hnode->as.handle.params[i];
    if (hnode->as.handle.k && g_excl_n < 8) g_excl_b[g_excl_n++] = hnode->as.handle.k;
    bool r = has_capture_rec(t, UINT32_MAX, bound, 0);
    g_excl_n = 0;
    return r;
}

/* A reset continuation must be self-contained: every KK_VAR it delivers to must
 * name a join defined within it (its exits otherwise are KK_RET, which the
 * lifted helper maps to its own k).  Rejects a body that escapes to an
 * enclosing join. */
static bool joins_closed_rec(const CTerm *t, uint32_t *def, int nd) {
    if (!t || nd >= CC_MAX_BOUND) return t == NULL;
    switch (t->kind) {
        case CT_APPCONT:
            if (t->as.appcont.kont.kind == KK_VAR) {
                for (int i = 0; i < nd; i++) if (def[i] == t->as.appcont.kont.id) return true;
                return false;
            }
            return true;
        case CT_LETVAL:  return joins_closed_rec(t->as.letval.body, def, nd);
        case CT_LETPRIM: return joins_closed_rec(t->as.letprim.body, def, nd);
        case CT_LETCALL: return joins_closed_rec(t->as.letcall.body, def, nd);
        case CT_TAILCALL:
            if (t->as.tailcall.kont.kind == KK_VAR) {
                for (int i = 0; i < nd; i++) if (def[i] == t->as.tailcall.kont.id) return true;
                return false;
            }
            return true;
        case CT_IF:
            return joins_closed_rec(t->as.if_.then_, def, nd)
                && joins_closed_rec(t->as.if_.else_, def, nd);
        case CT_LETCONT:
            def[nd] = t->as.letcont.j.id;
            return joins_closed_rec(t->as.letcont.jbody, def, nd + 1)
                && joins_closed_rec(t->as.letcont.body, def, nd + 1);
        case CT_RESUME: return joins_closed_rec(t->as.resume.body, def, nd);
        default: return false;
    }
}

static bool reset_body_ok(const CTerm *t) {
    if (!term_core_ok(t)) return false;
    uint32_t def[CC_MAX_BOUND];
    return joins_closed_rec(t, def, 0);
}

/* A shift body in the C3 subset: straight-line (letval/letprim/letcall) ending
 * in delivery of a scalar atom to the prompt.  No branches, tail calls, joins,
 * or nested delimiters. */
static bool shift_body_ok(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            return t->as.appcont.kont.kind == KK_PROMPT && atom_ok(&t->as.appcont.v);
        case CT_LETVAL:
            return atom_ok(&t->as.letval.v) && shift_body_ok(t->as.letval.body);
        case CT_LETPRIM:
            if (!shape_supported(t->as.letprim.spec) || is_println_shape(t->as.letprim.spec->shape))
                return false;
            for (uint32_t i = 0; i < t->as.letprim.n; i++)
                if (!atom_ok(&t->as.letprim.args[i])) return false;
            return shift_body_ok(t->as.letprim.body);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!atom_ok(&t->as.letcall.args[i])) return false;
            return shift_body_ok(t->as.letcall.body);
        default: return false;
    }
}

/* A perform continuation (C4): straight-line, delivered to KK_RET as a scalar
 * (the lifted DKFrame returns it).  No branches / tail calls / joins / nested
 * control. */
static bool perform_body_ok(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            /* Delivered to the function's return continuation (KK_RET) or, when
             * the perform is the tail of a delimited body, to the enclosing
             * prompt (KK_PROMPT); the lifted frame returns the value either way. */
            return (t->as.appcont.kont.kind == KK_RET || t->as.appcont.kont.kind == KK_PROMPT)
                && atom_ok(&t->as.appcont.v);
        case CT_LETVAL:
            return atom_ok(&t->as.letval.v) && perform_body_ok(t->as.letval.body);
        case CT_LETPRIM:
            if (!shape_supported(t->as.letprim.spec) || is_println_shape(t->as.letprim.spec->shape))
                return false;
            for (uint32_t i = 0; i < t->as.letprim.n; i++)
                if (!atom_ok(&t->as.letprim.args[i])) return false;
            return perform_body_ok(t->as.letprim.body);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!atom_ok(&t->as.letcall.args[i])) return false;
            return perform_body_ok(t->as.letcall.body);
        default: return false;
    }
}

/* A handler case body (C4): straight-line with `resume` (dk_invoke), delivered
 * to the prompt (KK_PROMPT) as a scalar.  resume.k is the continuation binding
 * (not scalar-checked); resume.v and other atoms are scalar. */
static bool handle_case_ok(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            return t->as.appcont.kont.kind == KK_PROMPT && atom_ok(&t->as.appcont.v);
        case CT_LETVAL:
            return atom_ok(&t->as.letval.v) && handle_case_ok(t->as.letval.body);
        case CT_LETPRIM:
            if (!shape_supported(t->as.letprim.spec) || is_println_shape(t->as.letprim.spec->shape))
                return false;
            for (uint32_t i = 0; i < t->as.letprim.n; i++)
                if (!atom_ok(&t->as.letprim.args[i])) return false;
            return handle_case_ok(t->as.letprim.body);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!atom_ok(&t->as.letcall.args[i])) return false;
            return handle_case_ok(t->as.letcall.body);
        case CT_RESUME:
            return t->as.resume.k.kind == CA_VAR && atom_ok(&t->as.resume.v)
                && handle_case_ok(t->as.resume.body);
        default: return false;
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
            return (slot_ty(t->as.letval.x.ty) || t->as.letval.x.ty == TY_NIL)
                && atom_ok(&t->as.letval.v)
                && term_core_ok(t->as.letval.body);
        case CT_LETPRIM: {
            if (!shape_supported(t->as.letprim.spec)) return false;
            for (uint32_t i = 0; i < t->as.letprim.n; i++)
                if (!atom_ok(&t->as.letprim.args[i])) return false;
            return term_core_ok(t->as.letprim.body);
        }
        case CT_LETCALL: {
            if (!(slot_ty(t->as.letcall.x.ty) || t->as.letcall.x.ty == TY_NIL))
                return false;
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!atom_ok(&t->as.letcall.args[i])) return false;
            return term_core_ok(t->as.letcall.body);
        }
        case CT_TAILCALL: {
            /* KK_RET / KK_VAR / KK_PROMPT are all valid here: a KK_PROMPT tail
             * call threads the enclosing prompt chain (a reset's delimited
             * body); a KK_VAR is only reifiable when the callee is not emittable
             * (handled by needs_heap_join). */
            for (uint32_t i = 0; i < t->as.tailcall.n; i++)
                if (!atom_ok(&t->as.tailcall.args[i])) return false;
            return true;
        }
        case CT_LETCONT:
            return (slot_ty(t->as.letcont.param.ty) || t->as.letcont.param.ty == TY_NIL)
                && term_core_ok(t->as.letcont.jbody)
                && term_core_ok(t->as.letcont.body);
        case CT_IF:
            return atom_ok(&t->as.if_.cond)
                && term_core_ok(t->as.if_.then_)
                && term_core_ok(t->as.if_.else_);
        case CT_RESET:
            /* C3: delimited body + a self-contained, zero-capture continuation. */
            return (slot_ty(t->as.reset.x.ty) || t->as.reset.x.ty == TY_NIL)
                && term_core_ok(t->as.reset.delim)
                && reset_body_ok(t->as.reset.body)
                && !has_capture(t->as.reset.body, t->as.reset.x.id);
        case CT_SHIFT:
            /* C3: straight-line, zero-capture shift body (which already applies
             * the receiver -- cps_shift_body).  The captured continuation is
             * discarded here (abortive); resume lands in C4. */
            return shift_body_ok(t->as.shift.body)
                && !has_capture(t->as.shift.body, UINT32_MAX);
        case CT_HANDLE:
            /* C4: single-case, <=1 effect param, self-contained zero-capture
             * continuation, straight-line handler case. */
            return t->as.handle.n_params <= 1
                && (slot_ty(t->as.handle.x.ty) || t->as.handle.x.ty == TY_NIL)
                && term_core_ok(t->as.handle.delim)
                && reset_body_ok(t->as.handle.body)
                && !has_capture(t->as.handle.body, t->as.handle.x.id)
                && handle_case_ok(t->as.handle.case_body)
                && !has_capture_case(t->as.handle.case_body, t);
        case CT_PERFORM:
            /* C4: <=1 scalar arg; zero-capture straight-line continuation. */
            return t->as.perform.n <= 1
                && (t->as.perform.n == 0 || atom_ok(&t->as.perform.args[0]))
                && perform_body_ok(t->as.perform.body)
                && !has_capture(t->as.perform.body, t->as.perform.x.id);
        case CT_RESUME:
            return t->as.resume.k.kind == CA_VAR && atom_ok(&t->as.resume.v)
                && term_core_ok(t->as.resume.body);
        default: /* CT_UNSUPPORTED */
            return false;
    }
}

static bool fn_sig_ok(const FnDef *fd) {
    if (!slot_ty(fd->return_type.kind)) return false;   /* nil-return excluded in C1 */
    for (uint8_t i = 0; i < fd->n_params; i++)
        if (!slot_ty(fd->params[i]->type.kind)) return false;
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

/* Effect -> prompt-tag registry.  Tags start at 2 (reset/shift use 1); the same
 * interned effect Symbol always maps to the same tag, so a handle and a matching
 * perform agree.  Reset per program. */
#define CPS_MAX_EFFECTS 128
static const Symbol *g_eff_syms[CPS_MAX_EFFECTS];
static int           g_eff_n;

static int effect_tag(const Symbol *eff) {
    for (int i = 0; i < g_eff_n; i++)
        if (g_eff_syms[i] == eff) return i + 2;
    if (g_eff_n < CPS_MAX_EFFECTS) { g_eff_syms[g_eff_n] = eff; return (g_eff_n++) + 2; }
    return 2;
}

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
        case CT_RESET:
            return needs_heap_join(t->as.reset.delim)
                || needs_heap_join(t->as.reset.body);
        case CT_SHIFT:
            return needs_heap_join(t->as.shift.body);
        case CT_HANDLE:
            return needs_heap_join(t->as.handle.delim)
                || needs_heap_join(t->as.handle.body)
                || needs_heap_join(t->as.handle.case_body);
        case CT_PERFORM:
            return needs_heap_join(t->as.perform.body);
        case CT_RESUME:
            return needs_heap_join(t->as.resume.body);
        default: return false;
    }
}

static void ensure_S(const Expr *program) {
    if (g_prog == program && g_ents) return;   /* cached */

    if (g_arena_live) { arena_free(&g_arena); g_arena_live = false; }
    free(g_ents); g_ents = NULL; g_ents_n = 0;
    g_prog = program;
    g_fwd_done = false;
    g_eff_n = 0;
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
    EmitCtx    *ctx;
    Buf        *out;         /* current function/helper body buffer */
    Buf        *helpers;     /* file-scope accumulator for lifted reset/shift helpers */
    int         indent;
    /* active inline joins: KK_VAR id -> join-parameter C name */
    struct { uint32_t id; const char *param; } joins[MAX_JOINS];
    int         n_joins;
    const char *cur_k;       /* C expr for the innermost prompt chain (KK_PROMPT target) */
    const char *fn_cn;       /* enclosing function's mangled name (helper naming) */
    int        *helper_ctr;  /* per-function unique-helper counter */
    bool        shift_mode;  /* true inside a shift-body / handler-case helper: KK_PROMPT -> return */
    bool        ret_mode;    /* true inside a perform-continuation frame: KK_RET -> return value */
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
        case CA_STR:  return atom_cstr(a->str);
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
static void emit_binder_decls(CE *ce, const CTerm *t);
static void emit_reset(CE *ce, const CTerm *t);
static void emit_shift(CE *ce, const CTerm *t);
static void emit_handle(CE *ce, const CTerm *t);
static void emit_perform(CE *ce, const CTerm *t);
static void emit_resume(CE *ce, const CTerm *t);

/* Deliver value-string `v` to continuation `kont`. */
static void emit_deliver(CE *ce, const CKont *kont, const char *v) {
    if (kont->kind == KK_RET) {
        /* In a perform-continuation frame the delivered value IS the result --
         * return it (dk_perform routes it to the handler's outer continuation);
         * otherwise run the function's return continuation. */
        if (ce->ret_mode)
            ce_line(ce, "return (intptr_t)(%s);", v);
        else
            ce_line(ce, "return dk_run(k, (intptr_t)(%s));", v);
    } else if (kont->kind == KK_PROMPT) {
        /* Deliver to the innermost prompt.  In a shift-body helper the delivered
         * value IS the shift result -- return it; otherwise run the prompt chain. */
        if (ce->shift_mode)
            ce_line(ce, "return (intptr_t)(%s);", v);
        else
            ce_line(ce, "return dk_run(%s, (intptr_t)(%s));", ce->cur_k, v);
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
        case BS_PRINTLN_CSTR:
            ce_line(ce, "puts(%s);", arg);
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
            /* cps->direct: an uncolored callee runs to completion and returns an
             * ordinary value; no continuation is threaded in. */
            char *fn = callee_name(t->as.letcall.fn);
            char *argv = atoms_csv(ce, t->as.letcall.args, t->as.letcall.n);
            ce_line(ce, "%s = %s(%s); /* cps->direct */", t->as.letcall.x.name, fn, argv);
            free(fn); free(argv);
            emit_term(ce, t->as.letcall.body);
            break;
        }
        case CT_TAILCALL: {
            char *fn = callee_name(t->as.tailcall.fn);
            char *argv = atoms_csv(ce, t->as.tailcall.args, t->as.tailcall.n);
            if (binding_in_s(t->as.tailcall.fn)) {
                /* cps->cps: both colored and emitted -- thread the continuation
                 * straight through, no trampoline.  The threaded continuation is
                 * the function's own k (KK_RET) or, inside a reset's delimited
                 * body, the enclosing prompt chain (KK_PROMPT).  A KK_VAR here
                 * would need a heap join, which excludes the caller. */
                const char *thread = (t->as.tailcall.kont.kind == KK_PROMPT)
                    ? ce->cur_k : "k";
                if (t->as.tailcall.n)
                    ce_line(ce, "return %s__cps(%s, %s); /* cps->cps */", fn, argv, thread);
                else
                    ce_line(ce, "return %s__cps(%s); /* cps->cps */", fn, thread);
            } else {
                /* cps->direct: the callee is uncolored or a colored function that
                 * fell back to direct-style; call it synchronously and deliver
                 * the ordinary result value to the continuation. */
                char *tmp = fresh_tmp(ce->ctx);
                /* __auto_type keeps the callee's real return type (int, cstr,
                 * ...); the slot cast at delivery narrows it to the word. */
                ce_line(ce, "__auto_type %s = %s(%s); /* cps->direct */", tmp, fn, argv);
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
        case CT_RESET:   emit_reset(ce, t); break;
        case CT_SHIFT:   emit_shift(ce, t); break;
        case CT_HANDLE:  emit_handle(ce, t); break;
        case CT_PERFORM: emit_perform(ce, t); break;
        case CT_RESUME:  emit_resume(ce, t); break;
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
            ce_line(ce, "%s %s;", binder_ctype(ce->ctx, t->as.letval.x.ty), t->as.letval.x.name);
            emit_binder_decls(ce, t->as.letval.body);
            break;
        case CT_LETPRIM:
            ce_line(ce, "%s %s;", binder_ctype(ce->ctx, t->as.letprim.x.ty), t->as.letprim.x.name);
            emit_binder_decls(ce, t->as.letprim.body);
            break;
        case CT_LETCALL:
            ce_line(ce, "%s %s;", binder_ctype(ce->ctx, t->as.letcall.x.ty), t->as.letcall.x.name);
            emit_binder_decls(ce, t->as.letcall.body);
            break;
        case CT_TAILCALL: break;
        case CT_LETCONT:
            ce_line(ce, "%s %s;", binder_ctype(ce->ctx, t->as.letcont.param.ty), t->as.letcont.param.name);
            emit_binder_decls(ce, t->as.letcont.body);
            emit_binder_decls(ce, t->as.letcont.jbody);
            break;
        case CT_IF:
            emit_binder_decls(ce, t->as.if_.then_);
            emit_binder_decls(ce, t->as.if_.else_);
            break;
        case CT_RESET:
            /* Only the delimited body lives in this function; reset.body is
             * lifted into a helper with its own decls. */
            emit_binder_decls(ce, t->as.reset.delim);
            break;
        case CT_SHIFT: break;   /* shift.body is lifted into a helper */
        case CT_HANDLE:
            /* Only the handled body lives here; the continuation and case are
             * lifted into helpers. */
            emit_binder_decls(ce, t->as.handle.delim);
            break;
        case CT_PERFORM: break;   /* terminal; the continuation is lifted */
        case CT_RESUME:
            ce_line(ce, "%s %s;", binder_ctype(ce->ctx, t->as.resume.x.ty), t->as.resume.x.name);
            emit_binder_decls(ce, t->as.resume.body);
            break;
        default: break;
    }
}

/* ---- C3: lifted reset/shift helpers ---------------------------------- *
 * A reset's continuation and a shift's body are reified as file-scope helper
 * functions (matching the DKFrame / DKBody signatures) so the DK machine can
 * invoke them.  The zero-capture cut passes the enclosing continuation `k`
 * directly as the frame env; a body needing to capture other locals is rejected
 * at classification time (has_capture) and falls back. */

/* The shape of a lifted helper. */
typedef enum {
    LH_RESET_CONT,    /* DKFrame (env=k, xval): reset continuation, KK_RET -> dk_run(k,..) */
    LH_SHIFT_BODY,    /* DKBody  (env, subk):   shift body, KK_PROMPT -> return value */
    LH_PERFORM_CONT,  /* DKFrame (env, xval):   perform continuation, KK_RET -> return value */
    LH_HANDLER_CASE,  /* DKHandler (env, arg, subk): binds params+k, KK_PROMPT -> return */
} LHMode;

/* Emit one lifted helper into ce->helpers.  `xname` is the incoming value
 * parameter name (reset/perform continuation result var).  `hnode` is the
 * CT_HANDLE (for LH_HANDLER_CASE param/k binding), else NULL. */
static void emit_lifted(CE *ce, const char *name, LHMode mode,
                        const char *xname, TypeKind xty,
                        const CTerm *body, const CTerm *hnode) {
    /* Emit the body into a temporary buffer first, so any nested reset/shift/
     * effect appends its own (inner) helpers ahead of this one in ce->helpers. */
    Buf tmp; buf_init(&tmp);
    CE hc = *ce;
    hc.out = &tmp;
    hc.indent = 4;
    hc.n_joins = 0;
    hc.cur_k = "k";
    /* A perform continuation returns its value however it is delivered (KK_RET
     * or KK_PROMPT), so it sets both return modes. */
    hc.shift_mode = (mode == LH_SHIFT_BODY || mode == LH_HANDLER_CASE || mode == LH_PERFORM_CONT);
    hc.ret_mode   = (mode == LH_PERFORM_CONT);

    /* Load the incoming value out of the one-word slot into a real-typed local
     * (reset/perform continuation), or bind the handler case's params/k. */
    if (mode == LH_RESET_CONT || mode == LH_PERFORM_CONT) {
        indent_buf(&tmp, 4);
        buf_printf(&tmp, "%s %s = (%s)(%s__slot);\n",
                   binder_ctype(ce->ctx, xty), xname, binder_ctype(ce->ctx, xty), xname);
    }
    if (mode == LH_HANDLER_CASE && hnode) {
        if (hnode->as.handle.n_params >= 1) {
            const Binding *pb = hnode->as.handle.params[0];
            char *pn = name_for_binding(ce->ctx, pb);
            const char *pty = binder_ctype(ce->ctx, pb->type.kind);
            indent_buf(&tmp, 4);
            buf_printf(&tmp, "%s %s = (%s)arg;\n", pty, pn, pty);
            free(pn);
        }
        if (hnode->as.handle.k) {
            char *kn = name_for_binding(ce->ctx, hnode->as.handle.k);
            indent_buf(&tmp, 4);
            buf_printf(&tmp, "DK *%s = subk;\n", kn);
            free(kn);
        }
    }
    emit_binder_decls(&hc, body);
    emit_term(&hc, body);
    buf_putc(&tmp, '\0');

    switch (mode) {
        case LH_SHIFT_BODY:
            buf_printf(ce->helpers, "static intptr_t %s(intptr_t env, DK *subk) {\n", name);
            buf_puts(ce->helpers, "    (void)env; (void)subk;\n");
            break;
        case LH_HANDLER_CASE:
            buf_printf(ce->helpers, "static intptr_t %s(intptr_t env, intptr_t arg, DK *subk) {\n", name);
            buf_puts(ce->helpers, "    (void)env; (void)arg; (void)subk;\n");
            break;
        case LH_PERFORM_CONT:
            buf_printf(ce->helpers, "static intptr_t %s(intptr_t env, intptr_t %s__slot) {\n", name, xname);
            buf_puts(ce->helpers, "    (void)env;\n");
            break;
        case LH_RESET_CONT:
            buf_printf(ce->helpers, "static intptr_t %s(intptr_t env, intptr_t %s__slot) {\n", name, xname);
            buf_puts(ce->helpers, "    DK *k = (DK *)env;\n");
            break;
    }
    buf_puts(ce->helpers, tmp.data);
    buf_puts(ce->helpers, "}\n");
    buf_free(&tmp);
}

/* CT_RESET: install a prompt whose outer continuation is the lifted reset body
 * (carrying k), then emit the delimited body threading that prompt chain.  The
 * per-reset DK nodes are leaked (DK is opaque; see
 * docs/reported/cps-delimited-dk-node-leak.md), matching the abortive path. */
static void emit_reset(CE *ce, const CTerm *t) {
    int id = (*ce->helper_ctr)++;
    char hname[256];
    snprintf(hname, sizeof(hname), "%s_k%d", ce->fn_cn, id);
    emit_lifted(ce, hname, LH_RESET_CONT, t->as.reset.x.name, t->as.reset.x.ty, t->as.reset.body, NULL);

    char pchain[64];
    snprintf(pchain, sizeof(pchain), "__p%d", id);
    ce_line(ce, "DK *%s = dk_prompt(1, dk_frame(%s, (intptr_t)%s, dk_done()));",
            pchain, hname, ce->cur_k);

    const char *save = ce->cur_k;
    ce->cur_k = arena_strdup(&g_arena, pchain, strlen(pchain));
    emit_term(ce, t->as.reset.delim);   /* delivers to the prompt chain, tail */
    ce->cur_k = save;
}

/* CT_SHIFT: capture the sub-continuation up to the nearest enclosing prompt and
 * run the shift body (abortive: the identity receiver delivers the body value,
 * ignoring the captured continuation). */
static void emit_shift(CE *ce, const CTerm *t) {
    int id = (*ce->helper_ctr)++;
    char hname[256];
    snprintf(hname, sizeof(hname), "%s_s%d", ce->fn_cn, id);
    emit_lifted(ce, hname, LH_SHIFT_BODY, NULL, TY_INT, t->as.shift.body, NULL);
    ce_line(ce, "return dk_run(dk_shift(1, %s, 0, %s), 0);", hname, ce->cur_k);
}

/* ---- C4: algebraic effects (handle / perform / resume) --------------- *
 * handle mirrors reset: lift the handle continuation to a DKFrame, install a
 * dk_handler (carrying the case as a DKHandler), and run the handled body
 * threading that handler prompt.  perform mirrors shift: lift the perform
 * continuation (a pure value transform) onto the chain and dk_perform against
 * the effect's tag; the handler runs the case, and resume = dk_invoke. */
static void emit_handle(CE *ce, const CTerm *t) {
    int id = (*ce->helper_ctr)++;
    char kname[256], cname[256];
    snprintf(kname, sizeof(kname), "%s_hk%d", ce->fn_cn, id);
    snprintf(cname, sizeof(cname), "%s_hc%d", ce->fn_cn, id);
    /* lift the handle continuation (like a reset continuation) */
    emit_lifted(ce, kname, LH_RESET_CONT, t->as.handle.x.name, t->as.handle.x.ty, t->as.handle.body, NULL);
    /* lift the handler case as a DKHandler */
    emit_lifted(ce, cname, LH_HANDLER_CASE, NULL, TY_INT, t->as.handle.case_body, t);

    int tag = effect_tag(t->as.handle.effect);
    char hchain[64];
    snprintf(hchain, sizeof(hchain), "__h%d", id);
    ce_line(ce,
        "DK *%s = dk_handler(%d, %s, 0, dk_frame(%s, (intptr_t)%s, dk_done()));",
        hchain, tag, cname, kname, ce->cur_k);

    const char *save = ce->cur_k;
    ce->cur_k = arena_strdup(&g_arena, hchain, strlen(hchain));
    emit_term(ce, t->as.handle.delim);   /* body runs threading the handler prompt */
    ce->cur_k = save;
}

/* Is the perform continuation trivial -- deliver the result straight to the
 * function's return continuation (a tail perform)?  Then no frame is needed. */
static bool perform_cont_trivial(const CTerm *t) {
    const CTerm *b = t->as.perform.body;
    return b && b->kind == CT_APPCONT
        && (b->as.appcont.kont.kind == KK_RET || b->as.appcont.kont.kind == KK_PROMPT)
        && b->as.appcont.v.kind == CA_CVAR
        && b->as.appcont.v.cvar_id == t->as.perform.x.id;
}

static void emit_perform(CE *ce, const CTerm *t) {
    int tag = effect_tag(t->as.perform.effect);
    /* argument: 0-arg effects pass 0; 1-arg pass the arg (subset). */
    char *arg = (t->as.perform.n >= 1)
        ? atom_str(ce, &t->as.perform.args[0]) : strdup("0");

    if (perform_cont_trivial(t)) {
        ce_line(ce, "return dk_perform(%d, (intptr_t)(%s), %s);", tag, arg, ce->cur_k);
    } else {
        int id = (*ce->helper_ctr)++;
        char pname[256];
        snprintf(pname, sizeof(pname), "%s_pf%d", ce->fn_cn, id);
        emit_lifted(ce, pname, LH_PERFORM_CONT, t->as.perform.x.name, t->as.perform.x.ty, t->as.perform.body, NULL);
        ce_line(ce, "return dk_perform(%d, (intptr_t)(%s), dk_frame(%s, 0, %s));",
                tag, arg, pname, ce->cur_k);
    }
    free(arg);
}

static void emit_resume(CE *ce, const CTerm *t) {
    /* resume k v = dk_invoke((DK*)k, v); slot-load the result and continue. */
    char *kk = atom_str(ce, &t->as.resume.k);
    char *vv = atom_str(ce, &t->as.resume.v);
    const char *rty = binder_ctype(ce->ctx, t->as.resume.x.ty);
    ce_line(ce, "%s = (%s)dk_invoke((DK *)(%s), (intptr_t)(%s));",
            t->as.resume.x.name, rty, kk, vv);
    free(kk); free(vv);
    emit_term(ce, t->as.resume.body);
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

    /* Emit the body into a temporary buffer, accumulating any lifted reset/shift
     * helpers into `helpers`; then write helpers (which must precede their uses),
     * the signature, and the body. */
    Buf body_buf; buf_init(&body_buf);
    Buf helpers;  buf_init(&helpers);
    int helper_ctr = 0;
    CE ce; memset(&ce, 0, sizeof(ce));
    ce.ctx = ctx; ce.out = &body_buf; ce.helpers = &helpers; ce.indent = 4;
    ce.cur_k = "k"; ce.fn_cn = cn; ce.helper_ctr = &helper_ctr;
    emit_binder_decls(&ce, se->term);
    emit_term(&ce, se->term);
    buf_putc(&body_buf, '\0');
    buf_putc(&helpers, '\0');

    if (helpers.len > 1) buf_puts(file, helpers.data);

    /* ---- CPS body: int64_t <name>__cps(<params>, DK *k) ---- */
    buf_printf(file, "static int64_t %s__cps(", cn);
    emit_params(ctx, file, fd);
    if (fd->n_params) buf_puts(file, ", ");
    buf_puts(file, "DK *k) {\n");
    buf_puts(file, body_buf.data);
    buf_puts(file, "}\n");
    buf_free(&body_buf);
    buf_free(&helpers);

    /* ---- direct->cps entry wrapper: int64_t <name>(<params>) ----
     * The boundary trampoline for an uncolored/direct caller (including `main`)
     * entering a colored function by its plain name unchanged.  It seeds the
     * initial continuation as the implicit root prompt (CPS5.3, DK_ROOT_TAG) on
     * a dk_done()-terminated chain -- the structural equivalent of dk_run_root,
     * so an undelimited capture inside the CPS body reaches program entry -- runs
     * the CPS body, and returns (unwraps) the value delivered to the root.  The
     * seed chain is freed after the body returns; the CPS body never retains it
     * (a captured sub-continuation is always a copy). */
    const char *rety = binder_ctype(ctx, fd->return_type.kind);
    buf_printf(file, "__attribute__((unused)) static %s %s(", rety, cn);
    emit_params(ctx, file, fd);
    buf_puts(file, ") {\n");
    buf_puts(file, "    DK *__root = dk_prompt(DK_ROOT_TAG, dk_done());\n");
    buf_printf(file, "    int64_t __r = %s__cps(", cn);
    for (uint8_t i = 0; i < fd->n_params; i++) {
        if (i) buf_puts(file, ", ");
        char *pn = name_for_binding(ctx, fd->params[i]);
        buf_puts(file, pn);
        free(pn);
    }
    if (fd->n_params) buf_puts(file, ", ");
    buf_puts(file, "__root);\n");
    buf_puts(file, "    dk_free(__root);\n");
    /* slot-load the final delivered value back to the real return type. */
    buf_printf(file, "    return (%s)(__r);\n}\n", rety);

    free(cn);
    return true;
}
