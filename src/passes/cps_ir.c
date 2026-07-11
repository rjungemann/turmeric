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

static CVar fresh_cvar(CpsB *b, const Type *ty) {
    CVar v;
    v.id = b->counter++;
    char buf[24];
    snprintf(buf, sizeof(buf), "t%u", v.id);
    v.name = arena_strdup(b->a, buf, strlen(buf));
    v.ty = ty ? ty->kind : TY_UNKNOWN;
    v.type = ty;
    v.bind = NULL;
    return v;
}

static CVar cvar_of_binding(const Binding *bd) {
    CVar v;
    v.id = bd->id;
    v.name = (bd->name ? bd->name->name : "_");
    v.ty = bd->type.kind;
    v.type = &bd->type;
    v.bind = bd;
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

/* A Tier A scalar kind: an integer/bool/pointer that occupies its C width with a
 * direct bit pattern.  A same-size reinterpret between two such kinds (e.g. the
 * `int -> uint64` the frontend emits for `(:: 10 :uint64)`, since both are 64
 * bits) is bit-identical, so it is a value-preserving retype the CPS translator
 * can see through.  Floats are excluded -- an int<->double reinterpret changes
 * bit meaning and is a Tier B concern, left to the fallback. */
static bool tierA_scalar_kind(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_BOOL: case TY_CSTR: case TY_PTR_VOID:
            return true;
        default:
            return false;
    }
}

/* True if `e` is a same-size Tier A reinterpret we can peel like an ascription. */
static bool is_tierA_reinterp(const Expr *e) {
    return e && e->kind == EX_REINTERPRET
        && tierA_scalar_kind(e->as.reinterpret_.source_kind)
        && tierA_scalar_kind(e->as.reinterpret_.target_kind);
}

static bool is_atomic(const Expr *e) {
    e = ascribe_peel(e);
    if (!e) return false;
    if (is_tierA_reinterp(e)) return is_atomic(e->as.reinterpret_.expr);
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
    a.type = e ? &e->type : NULL;
    if (!e) { a.kind = CA_UNIT; return a; }
    /* A Tier A reinterpret is a bit-identical retype: take the inner atom's
     * value but keep the reinterpret's (retyped) result type. */
    if (is_tierA_reinterp(e)) {
        TypeKind tgt = e->as.reinterpret_.target_kind;
        CAtom inner = atom_of(e->as.reinterpret_.expr);
        inner.ty = tgt;
        return inner;
    }
    switch (e->kind) {
        case EX_INT_LIT:   a.kind = CA_INT;  a.i = e->as.i; break;
        case EX_BOOL_LIT:  a.kind = CA_BOOL; a.b = e->as.b; break;
        case EX_NIL_LIT:   a.kind = CA_UNIT; break;
        case EX_FLOAT_LIT: a.kind = CA_FLOAT; a.f = e->as.f; break;
        case EX_CSTR_LIT:  a.kind = CA_STR;  a.str = e->as.s; break;
        case EX_VAR:       a.kind = CA_VAR;  a.var = e->as.var.binding; break;
        default:           a.kind = CA_OTHER; break;
    }
    return a;
}

static CAtom atom_cvar(CVar v) {
    CAtom a; memset(&a, 0, sizeof(a));
    a.kind = CA_CVAR; a.ty = v.ty; a.type = v.type; a.cvar_id = v.id; a.cvar_name = v.name;
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

/* An owning-value operation on a local rc handle (`rc/of`, `rc/clone`, `rc/drop`,
 * `rc/strong-count`, `rc->ptr`).  These do not cross a DK slot -- the rc stays a
 * local -- so the CPS backend delegates their emission to the direct emitter
 * (`emit_value`), which already carries the correct control-block / refcount /
 * drop-glue discipline.  Only a form whose operand is atomic (a var or literal)
 * is delegated, so no control operator (`perform` / `shift`) can hide in an
 * operand and get emitted in direct style inside a CPS function. */
static const Expr *owning_operand(const Expr *e) {
    if (!e) return NULL;
    switch (e->kind) {
        case EX_RC_OF:    return e->as.rc_of_.expr;
        case EX_RC_CLONE: return e->as.rc_clone_.expr;
        case EX_RC_DROP:  return e->as.rc_drop_.expr;
        case EX_RC_COUNT: return e->as.rc_count_.expr;
        case EX_RC_PTR:   return e->as.rc_ptr_.expr;
        default:          return NULL;
    }
}

static bool is_delegatable_owning(const Expr *e) {
    const Expr *arg = owning_operand(e);
    return arg != NULL && is_atomic(arg);
}

/* A struct/ADT value op that stays a local -- construct (make-struct), read a
 * field (.field), or a default value.  Like the owning ops, these do not need
 * CPS lowering: the aggregate is a local (never crosses the DK slot -- a crossing
 * use is rejected by atom_ok / fn_sig_ok), so emission is delegated to the direct
 * emitter.  Delegated only when every operand is atomic, so no control operator
 * hides in an operand. */
static bool is_delegatable_struct(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_GET_FIELD:
            return is_atomic(e->as.get_field_.struct_expr);
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
                if (!is_atomic(e->as.make_struct_.field_values[i])) return false;
            return true;
        case EX_DEFAULT_OF:
            return true;
        default:
            return false;
    }
}

/* A capture-free fn-value wrapper -- a bare fn coerced to a fat/poly callable, or
 * a closure literal with no captured free variables.  Such a value is not a call
 * and not a control op, and (being capture-free) references no enclosing local,
 * so it is safe to delegate to the direct emitter via CT_LETRAW anywhere,
 * including inside a lifted zero-capture body.  A CAPTURING closure is NOT
 * delegated here: its free vars would need the has_capture cut to see them, so it
 * falls back conservatively.  The wrapped fn body is a call-graph boundary
 * (colored on its own merits), never executed at this position. (N6.1) */
static bool is_delegatable_value(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_FN_TO_FAT:
        case EX_POLY_WRAP:
        case EX_FN:
            return true;
        case EX_CLOSURE:
            return e->as.closure_.closure && e->as.closure_.closure->n_captures == 0;
        default:
            return false;
    }
}

/* A CAPTURING closure is NOT a bare "direct entry point" value.  The direct
 * emitter's indirect-call path (emit_expr.c, the `fn_expr` block ~2950) casts
 * the callee VALUE to a bare function pointer and calls it -- but a fat
 * closure's value is its ENV pointer, so the cast jumps into the heap-allocated
 * env struct as if it were code (segfault).  Only a bare fn / 0-capture closure
 * / fn-pointer value is a valid indirect callee there.  A `shift`/`shift0` whose
 * receiver is a capturing closure lowers via a synthesized `(recv val)` that
 * hits exactly this path (cps_shift_body_kf), so such a shift must EVICT to the
 * direct shift lowering (emit_cps.c), which handles a capturing receiver
 * correctly (verified: continuation-substrate t-deep).  Returns true for a
 * callee the indirect delegation can safely emit. */
static bool indirect_callee_ok(const Expr *fe) {
    return !(fe && fe->kind == EX_CLOSURE && fe->as.closure_.closure
             && fe->as.closure_.closure->n_captures > 0);
}

/* True if every argument of an EX_CALL is atomic (so the whole call can be
 * delegated to the direct emitter without control ops hiding in an arg). */
static bool call_args_atomic(const Expr *e) {
    for (uint32_t i = 0; i < e->as.call_.n_args; i++)
        if (!is_atomic(e->as.call_.args[i])) return false;
    return true;
}

static CTerm *build_letraw(CpsB *b, Expr *e, CVar x, CTerm *rest) {
    CTerm *t = new_term(b, CT_LETRAW);
    t->as.letraw.x = x;
    t->as.letraw.e = e;
    t->as.letraw.body = rest;
    return t;
}

/* U7 callcc: a (call/cc f)/(escape f) receiver `f` the native escape landing
 * (emit_callcc) can emit as a value -- a named top-level fn, a fat/poly/fn value,
 * or a closure literal (capturing OR not).  A CAPTURING closure's free vars are
 * surfaced by collect_free_vars (through the EX_CALLCC receiver) and ride the
 * lifted continuation's env exactly like the CT_LETRAW path: in a lifted body a
 * non-scalar capture bails to fallback (the enclosing continuation's collect_caps
 * sets cs.ok=false and evicts), and in the main body emit_value captures the
 * visible local directly.  A non-fn-value receiver returns NULL so the caller
 * keeps the CT_LETRAW delegation. */
static bool callcc_native_recv(const Expr *f) {
    f = ascribe_peel(f);
    if (!f) return false;
    if (is_delegatable_value(f)) return true;          /* fat/poly/fn/zero-cap closure */
    if (f->kind == EX_CLOSURE) return true;            /* a closure literal (capturing too) */
    if (f->kind == EX_VAR && f->as.var.binding
        && f->as.var.binding->is_global
        && f->as.var.binding->type.kind == TY_FN)
        return true;                                    /* a named top-level fn */
    return false;
}

/* Build a native CT_CALLCC (binds x = the call/cc value, continues body) for a
 * receiver emit_callcc can emit; NULL otherwise so the caller falls back to the
 * CT_LETRAW delegation. */
static CTerm *build_callcc(CpsB *b, Expr *e, CVar x, CTerm *body) {
    if (!callcc_native_recv(e->as.callcc_.fn)) return NULL;
    CTerm *t = new_term(b, CT_CALLCC);
    t->as.callcc.x = x;
    t->as.callcc.e = e;
    t->as.callcc.body = body;
    return t;
}

/* U3 Shape 1: try to build a native (identity-continuation) cloneable node for
 * (cloneable-reset (cloneable-shift receiver val)) -- the shift IS the whole
 * reset body, so the captured continuation is the identity (no dk_copy_range).
 * Restricted to a named, uncolored top-level fn receiver; returns NULL for any
 * other shape (a non-trivial continuation, a closure/indirect receiver, a
 * colored receiver), so the caller falls back to the CT_LETRAW delegation. */
/* A named, uncolored top-level fn receiver of a cloneable-shift; NULL otherwise. */
static const Binding *cloneable_named_receiver(CpsB *b, const Expr *shift) {
    const Expr *kf = ascribe_peel(shift->as.cloneable_shift_.k_fn);
    if (!kf || kf->kind != EX_VAR || !kf->as.var.binding) return NULL;
    const Binding *recv = kf->as.var.binding;
    if (!recv->is_global) return NULL;          /* a top-level fn */
    if (callee_colored(b, recv)) return NULL;   /* uncolored receiver only */
    return recv;
}

static bool cloneable_op_supported(const char *op) {
    return op && (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                  strcmp(op, "*") == 0 || strcmp(op, "/") == 0);
}

static bool safe_to_delegate(CpsB *b, const Expr *e);   /* fwd (defined below) */

/* True if `e` contains a cloneable-shift reachable through the supported context
 * spine -- arithmetic binops, calls, pure lets, and an if branch point --
 * mirroring the direct emitter's reaches_shift_kind for EX_CLONEABLE_SHIFT.  Any
 * nested control form (a further reset/shift, an fn/closure body) self-delimits
 * and stops the descent, so a nested cloneable-reset is not descended. */
static bool cloneable_ctx_reaches_shift(const Expr *e) {
    e = ascribe_peel(e);
    if (!e) return false;
    switch (e->kind) {
        case EX_CLONEABLE_SHIFT:
            return true;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (cloneable_ctx_reaches_shift(e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (cloneable_ctx_reaches_shift(e->as.call_.args[i])) return true;
            return false;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (cloneable_ctx_reaches_shift(e->as.let_.bindings[i].init)) return true;
            return cloneable_ctx_reaches_shift(e->as.let_.body);
        case EX_IF:
            return cloneable_ctx_reaches_shift(e->as.if_.cond)
                || cloneable_ctx_reaches_shift(e->as.if_.then_)
                || cloneable_ctx_reaches_shift(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (cloneable_ctx_reaches_shift(e->as.do_.items[i])) return true;
            return false;
        default:
            return false;
    }
}

#define CL_IR_MAX_FRAMES 8
#define CL_IR_MAX_LETS   8

/* A pure `let` binding is admissible as cloneable-context prelude iff its type is
 * a simple scalar local (int/bool/nil/cstr/float) -- matching the direct
 * emitter's ty_simple_local gate. */
static bool clone_let_ty_ok(TypeKind k) {
    return k == TY_INT || k == TY_BOOL || k == TY_NIL ||
           k == TY_CSTR || k == TY_FLOAT;
}

/* U3 native cloneable: (cloneable-reset <ctx>) where <ctx> is a context spine
 * bottoming out in a (cloneable-shift receiver val) with a named uncolored
 * receiver.  The spine may contain, in any nesting:
 *   - arithmetic frames `(<op> <operand> ... [])` reified as DK frames
 *     (outermost-first); n_frames == 0 is Shape 1 (identity continuation, no
 *     dk_copy_range), >= 1 is Shape 2 (dk_copy_range);
 *   - pure `let` bindings (shift-free scalar inits, direct-emitted at the reset
 *     site as C locals so captured frame operands referencing them resolve);
 *   - one `if` branch point (pure condition; the shift-bearing arm rides the
 *     frame chain, the other arm is direct-emitted on the opposite branch).
 * Returns NULL for any richer shape (closure/colored receiver, non-atom or
 * non-int operand, a second `if`, a non-delegatable init/cond/pure arm, or
 * overflow of the frame/let caps) -- the caller then falls back to the CT_LETRAW
 * delegation. */
static CTerm *build_cloneable(CpsB *b, Expr *e, CVar x, CTerm *rest) {
    const Expr *cur = ascribe_peel(e->as.cloneable_reset_.body);
    CloneFrame frames[CL_IR_MAX_FRAMES];
    CloneLet   lets[CL_IR_MAX_LETS];
    uint32_t nf = 0, nl = 0;
    uint32_t n_outer = 0;   /* frames collected before the `if` (outer context) */
    const Expr *if_cond = NULL, *if_pure = NULL;
    bool if_when = true, saw_if = false;

    for (;;) {
        cur = ascribe_peel(cur);
        if (!cur) return NULL;

        /* Arithmetic frame: single-hole int binop. */
        if (cur->kind == EX_BUILTIN && cur->as.builtin.n == 2
            && cur->as.builtin.spec && cur->type.kind == TY_INT
            && cloneable_op_supported(cur->as.builtin.spec->c_op)) {
            const Expr *a0 = ascribe_peel(cur->as.builtin.args[0]);
            const Expr *a1 = ascribe_peel(cur->as.builtin.args[1]);
            bool h0 = cloneable_ctx_reaches_shift(a0);
            bool h1 = cloneable_ctx_reaches_shift(a1);
            if (h0 == h1) return NULL;               /* need exactly one hole side */
            const Expr *other = h0 ? a1 : a0;
            if (!other || !is_atomic(other) || other->type.kind != TY_INT) return NULL;
            if (nf >= CL_IR_MAX_FRAMES) return NULL;
            frames[nf].op           = cur->as.builtin.spec->c_op;  /* stable string */
            frames[nf].call_fn      = NULL;
            frames[nf].ignore_value = false;
            frames[nf].operand      = atom_of(other);
            frames[nf].hole_left    = h0;
            nf++;
            cur = h0 ? a0 : a1;                       /* descend the hole side */
            continue;
        }

        /* Call frame: a 1-arg call `(f [])` to a top-level uncolored int->int fn;
         * the hole is the sole argument, so there is no captured env. */
        if (cur->kind == EX_CALL && cur->as.call_.n_args == 1
            && cur->as.call_.fn_binding && !cur->as.call_.fn_expr) {
            const Binding *fb = cur->as.call_.fn_binding;
            if (fb->type.kind != TY_FN || fb->type.as.fn.arity != 1) return NULL;
            if (fb->closure_fn_binding) return NULL;         /* not a fat closure */
            if (callee_colored(b, fb)) return NULL;          /* uncolored target */
            if (cur->type.kind != TY_INT) return NULL;       /* result: int */
            if (fb->type.as.fn.arg_kinds[0] != TY_INT) return NULL;  /* arg: int */
            const Expr *a0 = ascribe_peel(cur->as.call_.args[0]);
            if (!cloneable_ctx_reaches_shift(a0)) return NULL;   /* sole arg is the hole */
            if (nf >= CL_IR_MAX_FRAMES) return NULL;
            memset(&frames[nf], 0, sizeof(CloneFrame));
            frames[nf].op        = NULL;
            frames[nf].call_fn   = fb;
            frames[nf].operand.kind = CA_INT;   /* unused placeholder (env passed as 0) */
            frames[nf].operand.ty   = TY_INT;
            frames[nf].hole_left = true;         /* the hole is the sole arg */
            nf++;
            cur = a0;                            /* descend into the hole arg */
            continue;
        }

        /* Call frame (2-arg): `(f other [])` / `(f [] other)` to a top-level
         * uncolored (int,int)->int fn; the hole is one arg, the other is a captured
         * INT env.  The env rides the frame's dk_frame slot -- deep-cloned with the
         * chain on each resume -- so multi-shot is correct with no marshaling (unlike
         * serial, which registers a per-site skcall for save/restore).  (Non-int
         * envs -- cstr / Serializable -- still delegate.) */
        if (cur->kind == EX_CALL && cur->as.call_.n_args == 2
            && cur->as.call_.fn_binding && !cur->as.call_.fn_expr) {
            const Binding *fb = cur->as.call_.fn_binding;
            if (fb->type.kind != TY_FN || fb->type.as.fn.arity != 2) return NULL;
            if (fb->closure_fn_binding) return NULL;         /* not a fat closure */
            if (callee_colored(b, fb)) return NULL;          /* uncolored target */
            if (cur->type.kind != TY_INT) return NULL;       /* result: int */
            if (fb->type.as.fn.arg_kinds[0] != TY_INT ||
                fb->type.as.fn.arg_kinds[1] != TY_INT) return NULL;  /* both args int */
            const Expr *a0 = ascribe_peel(cur->as.call_.args[0]);
            const Expr *a1 = ascribe_peel(cur->as.call_.args[1]);
            bool h0 = cloneable_ctx_reaches_shift(a0);
            bool h1 = cloneable_ctx_reaches_shift(a1);
            if (h0 == h1) return NULL;               /* exactly one hole side */
            const Expr *other = h0 ? a1 : a0;        /* the captured env operand */
            if (!other || !is_atomic(other) || other->type.kind != TY_INT) return NULL;
            if (nf >= CL_IR_MAX_FRAMES) return NULL;
            memset(&frames[nf], 0, sizeof(CloneFrame));
            frames[nf].op        = NULL;
            frames[nf].call_fn   = fb;
            frames[nf].operand   = atom_of(other);   /* real captured int env */
            frames[nf].hole_left = h0;                /* hole is the left arg? */
            nf++;
            cur = h0 ? a0 : a1;                        /* descend into the hole side */
            continue;
        }

        /* do-sequence with a statement-position shift:
         *   (do PRELUDE... (cloneable-shift receiver v) TAIL...)
         * The prelude items run once at capture time (side-effect-only, binding-
         * less lets); the shift must be the do-item itself; each tail item is a
         * 0-arg call to a top-level uncolored int fn, reified as an ignore-value
         * frame (runs `f()` on resume regardless of the resumed value).  Restricted
         * to the whole reset body (no outer frames yet) and no `if`, matching the
         * shape the direct emitter supports; the 1-arg ignore-value tail crashes
         * the direct backend, so it stays on delegation. */
        if (cur->kind == EX_DO && nf == 0 && !saw_if) {
            uint32_t N = cur->as.do_.n;
            int32_t m = -1;
            for (uint32_t i = 0; i < N; i++) {
                if (cloneable_ctx_reaches_shift(cur->as.do_.items[i])) {
                    if (m >= 0) return NULL;             /* at most one hole */
                    m = (int32_t)i;
                }
            }
            if (m < 0) return NULL;
            const Expr *shift_item = ascribe_peel(cur->as.do_.items[m]);
            if (!shift_item || shift_item->kind != EX_CLONEABLE_SHIFT) return NULL;
            /* Prelude items [0, m): direct-emitted for side effect at the reset site. */
            for (int32_t i = 0; i < m; i++) {
                const Expr *pre = cur->as.do_.items[i];
                if (cloneable_ctx_reaches_shift(pre)) return NULL;
                if (!safe_to_delegate(b, pre)) return NULL;
                if (nl >= CL_IR_MAX_LETS) return NULL;
                lets[nl].binding = NULL;                 /* side-effect prelude */
                lets[nl].init    = pre;
                nl++;
            }
            /* Tail items (m, N): 0-arg ignore-value frames.  Record in reverse so
             * the first tail item is innermost (runs first on resume) and the last
             * is outermost (runs last, its value is the reset's value). */
            for (int32_t i = (int32_t)N - 1; i > m; i--) {
                const Expr *tail = ascribe_peel(cur->as.do_.items[i]);
                if (!tail || tail->kind != EX_CALL ||
                    !tail->as.call_.fn_binding || tail->as.call_.fn_expr) return NULL;
                const Binding *fb = tail->as.call_.fn_binding;
                if (fb->type.kind != TY_FN || fb->type.as.fn.arity != 0) return NULL;
                if (tail->as.call_.n_args != 0) return NULL;
                if (fb->closure_fn_binding) return NULL;    /* not a fat closure */
                if (callee_colored(b, fb)) return NULL;     /* uncolored target */
                if (tail->type.kind != TY_INT) return NULL; /* result: int */
                if (nf >= CL_IR_MAX_FRAMES) return NULL;
                memset(&frames[nf], 0, sizeof(CloneFrame));
                frames[nf].op           = NULL;
                frames[nf].call_fn      = fb;
                frames[nf].ignore_value = true;
                frames[nf].operand.kind = CA_INT;   /* unused (env passed as 0) */
                frames[nf].operand.ty   = TY_INT;
                frames[nf].hole_left    = true;
                nf++;
            }
            cur = shift_item;                       /* the shift; loop exits below */
            continue;
        }

        /* Pure `let` prelude: inits shift-free + scalar, body carries the hole. */
        if (cur->kind == EX_LET) {
            /* emit_cloneable lays every prelude local down at the reset site,
             * ahead of the `if` branch and the frame-operand pushes, so a `let`
             * either ABOVE the `if` (referenced by outer frames + the pure arm) or
             * nested in the shift-bearing arm (referenced by inner frames) is in
             * scope where it is used.  Hoisting a shift-arm let out of its branch
             * is sound because its init is pure and scalar (shift-free,
             * safe_to_delegate below); a binding the shift BODY itself needs would
             * be a live capture, which the shift admission rejects (n_live_captures
             * != 0), so it never reaches here. */
            const Expr *lbody = cur->as.let_.body;
            if (!cloneable_ctx_reaches_shift(lbody)) return NULL;
            for (uint32_t i = 0; i < cur->as.let_.n; i++) {
                const Expr    *init = cur->as.let_.bindings[i].init;
                const Binding *bd   = cur->as.let_.bindings[i].binding;
                if (cloneable_ctx_reaches_shift(init)) return NULL;
                if (!bd || !clone_let_ty_ok(bd->type.kind)) return NULL;
                if (!safe_to_delegate(b, init)) return NULL;   /* pure, emit_value-able */
                if (nl >= CL_IR_MAX_LETS) return NULL;
                lets[nl].binding = bd;
                lets[nl].init    = init;
                nl++;
            }
            cur = lbody;
            continue;
        }

        /* One `if` branch point: pure condition, exactly one shift-bearing arm. */
        if (cur->kind == EX_IF) {
            if (saw_if) return NULL;                  /* only one branch point */
            const Expr *cond = cur->as.if_.cond;
            const Expr *thn  = cur->as.if_.then_;
            const Expr *els  = cur->as.if_.else_or_null;
            if (!cond || !thn || !els) return NULL;   /* need both arms */
            if (cloneable_ctx_reaches_shift(cond)) return NULL;
            if (!safe_to_delegate(b, cond)) return NULL;
            bool ht = cloneable_ctx_reaches_shift(thn);
            bool he = cloneable_ctx_reaches_shift(els);
            if (ht == he) return NULL;                /* exactly one shift arm */
            const Expr *shift_arm = ht ? thn : els;
            const Expr *pure_arm  = ht ? els : thn;
            if (cloneable_ctx_reaches_shift(pure_arm)) return NULL;   /* defensive */
            if (!safe_to_delegate(b, pure_arm)) return NULL;
            if_cond = cond; if_pure = pure_arm; if_when = ht; saw_if = true;
            n_outer = nf;   /* frames so far are OUTSIDE the if; later frames are inner */
            cur = shift_arm;
            continue;
        }

        break;
    }

    if (!cur || cur->kind != EX_CLONEABLE_SHIFT) return NULL;
    /* The shared DK runtime prelude (__dk_cont_fn / __dk_env_clone / __dk_env_drop,
     * dk_copy_range) that native Shape 2 emits is gated by the direct emitter's
     * cl_can_lower, which requires the shift to have no live captures at its site.
     * Match that constraint: lowering a shift with live captures natively would
     * reference prelude helpers the gate never emits (undeclared C names).  Such a
     * shape stays on the delegation path, which matches the direct backend. */
    if (cur->as.cloneable_shift_.n_live_captures != 0) return NULL;
    const Binding *recv = cloneable_named_receiver(b, cur);
    const Expr *recv_expr = NULL;
    if (!recv) {
        /* U7: a CLOSURE receiver (capturing or not).  Shape 1 (nf == 0) calls it
         * directly at the reset site; Shape 2 (nf >= 1) threads it through the
         * dk_shift body env -- emit_cloneable bakes the closure's thunk into the
         * per-site body fn and passes the closure env as the shift env.  A
         * zero-capture closure is already lifted to a named global (handled above);
         * this is the capturing case. */
        const Expr *kf = ascribe_peel(cur->as.cloneable_shift_.k_fn);
        if (kf && kf->kind == EX_CLOSURE) recv_expr = kf;
        else return NULL;
    }

    CTerm *t = new_term(b, CT_CLONEABLE);
    t->as.cloneable.x = x;
    t->as.cloneable.receiver = recv;
    t->as.cloneable.receiver_expr = recv_expr;
    t->as.cloneable.n_frames = nf;
    if (nf) {
        CloneFrame *fa = arena_alloc(b->a, nf * sizeof(CloneFrame));
        memcpy(fa, frames, nf * sizeof(CloneFrame));
        t->as.cloneable.frames = fa;
    } else {
        t->as.cloneable.frames = NULL;
    }
    t->as.cloneable.n_lets = nl;
    if (nl) {
        CloneLet *la = arena_alloc(b->a, nl * sizeof(CloneLet));
        memcpy(la, lets, nl * sizeof(CloneLet));
        t->as.cloneable.lets = la;
    } else {
        t->as.cloneable.lets = NULL;
    }
    t->as.cloneable.n_outer_frames = n_outer;
    t->as.cloneable.if_cond = if_cond;
    t->as.cloneable.if_pure = if_pure;
    t->as.cloneable.if_when = if_when;
    t->as.cloneable.body = rest;
    return t;
}

/* Pure existence check: does `program` declare a Serializable instance for the
 * nominal (TY_ADT) type `t`?  Lets build_serial admit a SER-env frame; the
 * emitter (emit_cps_ir.c) resolves the instance's serialize/deserialize method
 * C names.  Kept in the pass (no emit helpers), mirroring the existence path of
 * emit_cps.c's sk_find_serializable. */
static bool cps_serializable_exists(const Expr *program, Type t) {
    if (!program || program->kind != EX_PROGRAM) return false;
    if (t.kind != TY_ADT || !t.as.adt_.def || !t.as.adt_.def->name) return false;
    const char *tname = t.as.adt_.def->name;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *it = program->as.program.items[i];
        if (!it || it->kind != EX_INSTANCE_DEF) continue;
        const TypeClassInstance *inst = it->as.instance_def_.instance;
        if (!inst || !inst->typeclass || !inst->typeclass->name) continue;
        if (strcmp(inst->typeclass->name->name, "Serializable") != 0) continue;
        if (inst->n_type_args < 1) continue;
        Type at = inst->type_args[0];
        if (at.kind == TY_ADT && at.as.adt_.def && at.as.adt_.def->name &&
            strcmp(at.as.adt_.def->name, tname) == 0) return true;
    }
    return false;
}

/* U4 native serial: (serial-reset <arith-ctx> (serial-shift receiver v)) with a
 * named uncolored fn receiver, arithmetic context frames only.  Structurally
 * identical to the cloneable arithmetic Shape 2, but marshalable: the frames use
 * the shared tagged marshaler and the shift hands the receiver the copied DK
 * chain so save-cont!/resume-cont! round-trips.  Returns NULL for any richer
 * shape (let/if/call/do context, non-atom or non-int operand, Shape 1 identity,
 * a closure/colored receiver) -- the caller then falls back to the CT_LETRAW
 * delegation, which owns the marshaling-registry (call-frame) cases. */
static bool serial_reaches_shift(const Expr *e) {
    e = ascribe_peel(e);
    if (!e) return false;
    switch (e->kind) {
        case EX_SERIAL_SHIFT:
            return true;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (serial_reaches_shift(e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (serial_reaches_shift(e->as.call_.args[i])) return true;
            return false;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (serial_reaches_shift(e->as.let_.bindings[i].init)) return true;
            return serial_reaches_shift(e->as.let_.body);
        case EX_IF:
            return serial_reaches_shift(e->as.if_.cond)
                || serial_reaches_shift(e->as.if_.then_)
                || serial_reaches_shift(e->as.if_.else_or_null);
        default:
            return false;
    }
}

static const Binding *serial_named_receiver(CpsB *b, const Expr *shift) {
    const Expr *kf = ascribe_peel(shift->as.serial_shift_.k_fn);
    if (!kf || kf->kind != EX_VAR || !kf->as.var.binding) return NULL;
    const Binding *recv = kf->as.var.binding;
    if (recv->type.kind != TY_FN) return NULL;       /* a function value */
    if (recv->closure_fn_binding) return NULL;       /* not a fat closure */
    if (recv->is_global && callee_colored(b, recv)) return NULL;  /* uncolored global */
    return recv;
}

static CTerm *build_serial(CpsB *b, Expr *e, CVar x, CTerm *rest) {
    const Expr *cur = ascribe_peel(e->as.serial_reset_.body);
    CloneFrame frames[CL_IR_MAX_FRAMES];
    CloneLet   lets[CL_IR_MAX_LETS];
    uint32_t nf = 0, nl = 0;
    uint32_t n_outer = 0;   /* frames collected before the `if` (outer context) */
    const Expr *if_cond = NULL, *if_pure = NULL;
    bool if_when = true, saw_if = false;

    for (;;) {
        cur = ascribe_peel(cur);
        if (!cur) return NULL;

        /* Arithmetic frame: single-hole int binop -> shared tagged marshaler. */
        if (cur->kind == EX_BUILTIN && cur->as.builtin.n == 2
            && cur->as.builtin.spec && cur->type.kind == TY_INT
            && cloneable_op_supported(cur->as.builtin.spec->c_op)) {
            const Expr *a0 = ascribe_peel(cur->as.builtin.args[0]);
            const Expr *a1 = ascribe_peel(cur->as.builtin.args[1]);
            bool h0 = serial_reaches_shift(a0);
            bool h1 = serial_reaches_shift(a1);
            if (h0 == h1) return NULL;               /* need exactly one hole side */
            const Expr *other = h0 ? a1 : a0;
            if (!other || !is_atomic(other) || other->type.kind != TY_INT) return NULL;
            if (nf >= CL_IR_MAX_FRAMES) return NULL;
            memset(&frames[nf], 0, sizeof(CloneFrame));
            frames[nf].op        = cur->as.builtin.spec->c_op;   /* stable string */
            frames[nf].operand   = atom_of(other);
            frames[nf].hole_left = h0;
            nf++;
            cur = h0 ? a0 : a1;                       /* descend the hole side */
            continue;
        }

        /* Call frame: a 1-arg call `(f [])` to a top-level uncolored int->int fn;
         * the hole is the sole argument (no captured env).  Emitted with a per-site
         * wrapper + SkReg registration keyed by "<fn>$L" so the marshaler round-
         * trips it.  (2-arg call frames need a serialized env operand and stay on
         * the delegation for now.) */
        if (cur->kind == EX_CALL && cur->as.call_.n_args == 1
            && cur->as.call_.fn_binding && !cur->as.call_.fn_expr) {
            const Binding *fb = cur->as.call_.fn_binding;
            if (fb->type.kind != TY_FN || fb->type.as.fn.arity != 1) return NULL;
            if (fb->closure_fn_binding) return NULL;         /* not a fat closure */
            if (callee_colored(b, fb)) return NULL;          /* uncolored target */
            if (cur->type.kind != TY_INT) return NULL;       /* result: int */
            if (fb->type.as.fn.arg_kinds[0] != TY_INT) return NULL;  /* arg: int */
            const Expr *a0 = ascribe_peel(cur->as.call_.args[0]);
            if (!serial_reaches_shift(a0)) return NULL;      /* sole arg is the hole */
            if (nf >= CL_IR_MAX_FRAMES) return NULL;
            memset(&frames[nf], 0, sizeof(CloneFrame));
            frames[nf].op        = NULL;
            frames[nf].call_fn   = fb;
            frames[nf].operand.kind = CA_INT;   /* unused placeholder (env passed as 0) */
            frames[nf].operand.ty   = TY_INT;
            frames[nf].hole_left = true;         /* the hole is the sole arg */
            nf++;
            cur = a0;                            /* descend into the hole arg */
            continue;
        }

        /* Call frame (2-arg): `(f other [])` / `(f [] other)` to a top-level
         * uncolored (int,int)->int fn; the hole is one arg, the other is a captured
         * INT env.  The per-site wrapper applies f to (value, env) in source order;
         * the env is serialized inline (SK_ENV_INT) by the marshaler, keyed by the
         * "<fn>$2L"/"<fn>$2R" side so save-cont!/resume-cont! round-trips.  (Non-int
         * envs -- cstr / Serializable -- still delegate.) */
        if (cur->kind == EX_CALL && cur->as.call_.n_args == 2
            && cur->as.call_.fn_binding && !cur->as.call_.fn_expr) {
            const Binding *fb = cur->as.call_.fn_binding;
            if (fb->type.kind != TY_FN || fb->type.as.fn.arity != 2) return NULL;
            if (fb->closure_fn_binding) return NULL;         /* not a fat closure */
            if (callee_colored(b, fb)) return NULL;          /* uncolored target */
            if (cur->type.kind != TY_INT) return NULL;       /* result: int */
            const Expr *a0 = ascribe_peel(cur->as.call_.args[0]);
            const Expr *a1 = ascribe_peel(cur->as.call_.args[1]);
            bool h0 = serial_reaches_shift(a0);
            bool h1 = serial_reaches_shift(a1);
            if (h0 == h1) return NULL;               /* exactly one hole side */
            /* the hole slot's param is the resume value -- must be int */
            if (fb->type.as.fn.arg_kinds[h0 ? 0 : 1] != TY_INT) return NULL;
            const Expr *other = h0 ? a1 : a0;        /* the captured env operand */
            if (!other || serial_reaches_shift(other)) return NULL;
            /* env: an int (inline codec) or a nominal type with a Serializable
             * instance (SER codec via its serialize/deserialize).  cstr 2-arg envs
             * still delegate (the fixed int cast on the wrapper does not carry the
             * cstr result typing they come paired with). */
            TypeKind ek = other->type.kind;
            if (ek != TY_INT && !cps_serializable_exists(b->program, other->type))
                return NULL;
            /* An atomic env rides `operand`; a non-atomic pure env (e.g. a
             * `(mk-rec ...)` constructor) is emit_value'd at the reset site. */
            bool atom = is_atomic(other);
            if (!atom && !safe_to_delegate(b, other)) return NULL;
            if (nf >= CL_IR_MAX_FRAMES) return NULL;
            memset(&frames[nf], 0, sizeof(CloneFrame));
            frames[nf].op        = NULL;
            frames[nf].call_fn   = fb;
            frames[nf].operand   = atom_of(other);   /* carries .type + atomic value */
            frames[nf].env_expr  = atom ? NULL : other;
            frames[nf].hole_left = h0;                /* hole is arg 0 iff h0 */
            nf++;
            cur = h0 ? a0 : a1;                       /* descend the hole side */
            continue;
        }

        /* Pure `let` prelude (emit shared with cloneable): inits shift-free +
         * scalar, body carries the hole.  A `let` above or nested in the shift arm
         * is hoisted to the reset site (see build_cloneable's note); a binding the
         * shift body needs is a live capture the shift admission already rejects. */
        if (cur->kind == EX_LET) {
            const Expr *lbody = cur->as.let_.body;
            if (!serial_reaches_shift(lbody)) return NULL;
            for (uint32_t i = 0; i < cur->as.let_.n; i++) {
                const Expr    *init = cur->as.let_.bindings[i].init;
                const Binding *bd   = cur->as.let_.bindings[i].binding;
                if (serial_reaches_shift(init)) return NULL;
                if (!bd || !clone_let_ty_ok(bd->type.kind)) return NULL;
                if (!safe_to_delegate(b, init)) return NULL;
                if (nl >= CL_IR_MAX_LETS) return NULL;
                lets[nl].binding = bd;
                lets[nl].init    = init;
                nl++;
            }
            cur = lbody;
            continue;
        }

        /* One `if` branch point (emit shared with cloneable): pure condition,
         * exactly one shift-bearing arm; the pure arm is direct-emitted. */
        if (cur->kind == EX_IF) {
            if (saw_if) return NULL;
            const Expr *cond = cur->as.if_.cond;
            const Expr *thn  = cur->as.if_.then_;
            const Expr *els  = cur->as.if_.else_or_null;
            if (!cond || !thn || !els) return NULL;
            if (serial_reaches_shift(cond)) return NULL;
            if (!safe_to_delegate(b, cond)) return NULL;
            bool ht = serial_reaches_shift(thn);
            bool he = serial_reaches_shift(els);
            if (ht == he) return NULL;                /* exactly one shift arm */
            const Expr *shift_arm = ht ? thn : els;
            const Expr *pure_arm  = ht ? els : thn;
            if (serial_reaches_shift(pure_arm)) return NULL;
            if (!safe_to_delegate(b, pure_arm)) return NULL;
            if_cond = cond; if_pure = pure_arm; if_when = ht; saw_if = true;
            n_outer = nf;   /* frames so far are OUTSIDE the if; later frames are inner */
            cur = shift_arm;
            continue;
        }

        /* do-sequence with a statement-position shift (mirrors the cloneable do
         * branch): prelude items run once at capture (binding-less lets), 0-arg
         * tail calls become ignore-value frames (marshaled under the "<fn>$0"
         * side).  Whole reset body, no outer frames, no `if`. */
        if (cur->kind == EX_DO && nf == 0 && !saw_if) {
            uint32_t N = cur->as.do_.n;
            int32_t m = -1;
            for (uint32_t i = 0; i < N; i++) {
                if (serial_reaches_shift(cur->as.do_.items[i])) {
                    if (m >= 0) return NULL;             /* at most one hole */
                    m = (int32_t)i;
                }
            }
            if (m < 0) return NULL;
            const Expr *shift_item = ascribe_peel(cur->as.do_.items[m]);
            if (!shift_item || shift_item->kind != EX_SERIAL_SHIFT) return NULL;
            for (int32_t i = 0; i < m; i++) {
                const Expr *pre = cur->as.do_.items[i];
                if (serial_reaches_shift(pre)) return NULL;
                if (!safe_to_delegate(b, pre)) return NULL;
                if (nl >= CL_IR_MAX_LETS) return NULL;
                lets[nl].binding = NULL;                 /* side-effect prelude */
                lets[nl].init    = pre;
                nl++;
            }
            for (int32_t i = (int32_t)N - 1; i > m; i--) {
                const Expr *tail = ascribe_peel(cur->as.do_.items[i]);
                if (!tail || tail->kind != EX_CALL ||
                    !tail->as.call_.fn_binding || tail->as.call_.fn_expr) return NULL;
                const Binding *fb = tail->as.call_.fn_binding;
                if (fb->type.kind != TY_FN) return NULL;
                if (fb->closure_fn_binding) return NULL;    /* not a fat closure */
                if (callee_colored(b, fb)) return NULL;     /* uncolored target */
                if (tail->type.kind != TY_INT) return NULL; /* result: int */
                if (nf >= CL_IR_MAX_FRAMES) return NULL;
                if (fb->type.as.fn.arity == 0 && tail->as.call_.n_args == 0) {
                    /* 0-arg ignore-value tail `(f)`: run f() on resume, no env. */
                    memset(&frames[nf], 0, sizeof(CloneFrame));
                    frames[nf].op           = NULL;
                    frames[nf].call_fn      = fb;
                    frames[nf].ignore_value = true;
                    frames[nf].operand.kind = CA_INT;   /* unused (env passed as 0) */
                    frames[nf].operand.ty   = TY_INT;
                    frames[nf].hole_left    = true;
                } else if (fb->type.as.fn.arity == 1 && tail->as.call_.n_args == 1) {
                    /* 1-arg captured-config tail `(f cap)`: cap is a pure atomic
                     * value captured at the reset site (int inline / cstr length-
                     * prefixed) and applied to f on resume, ignoring the resumed
                     * value -- what lets a real loop `(loop cfg)` receive its config
                     * through the saved continuation.  (Serializable / non-atomic
                     * caps still delegate.) */
                    const Expr *cap = ascribe_peel(tail->as.call_.args[0]);
                    if (!cap || serial_reaches_shift(cap)) return NULL;
                    if (!is_atomic(cap)) return NULL;
                    TypeKind ek = cap->type.kind;
                    bool inline_ok = (ek == TY_INT || ek == TY_CSTR);  /* inline codec */
                    bool ser_ok = !inline_ok
                        && cps_serializable_exists(b->program, cap->type);  /* SER codec */
                    if (!inline_ok && !ser_ok) return NULL;
                    if (fb->type.as.fn.arg_kinds[0] != ek) return NULL;
                    memset(&frames[nf], 0, sizeof(CloneFrame));
                    frames[nf].op           = NULL;
                    frames[nf].call_fn      = fb;
                    frames[nf].ignore_value = true;
                    frames[nf].operand      = atom_of(cap);   /* real captured env */
                    frames[nf].hole_left    = true;
                } else {
                    return NULL;
                }
                nf++;
            }
            cur = shift_item;                       /* the shift; loop exits below */
            continue;
        }

        break;
    }

    /* Shape 1 serial: an empty context (nf == 0) -- the serial-shift IS the whole
     * reset body.  The receiver gets the identity continuation (a bare
     * dk_prompt(1, dk_done()) chain, marshalable like any deeper chain), so
     * emit_cloneable's serial branch handles nf == 0 with empty frame loops.
     * Mirrors the cloneable Shape 1 already supported natively. */
    if (!cur || cur->kind != EX_SERIAL_SHIFT) return NULL;
    const Binding *recv = serial_named_receiver(b, cur);
    const Expr *recv_expr = NULL;
    if (!recv) {
        /* U7: a CLOSURE receiver -- Shape 1 (called at the reset site) or Shape 2
         * (threaded through the dk_shift body env, thunk baked into the body fn).
         * The receiver runs once at capture; only the continuation is marshaled,
         * so a capturing closure receiver keeps save-cont!/resume-cont! sound. */
        const Expr *kf = ascribe_peel(cur->as.serial_shift_.k_fn);
        if (kf && kf->kind == EX_CLOSURE) recv_expr = kf;
        else return NULL;
    }

    CTerm *t = new_term(b, CT_CLONEABLE);
    t->as.cloneable.serial = true;
    t->as.cloneable.x = x;
    t->as.cloneable.receiver = recv;
    t->as.cloneable.receiver_expr = recv_expr;
    t->as.cloneable.n_frames = nf;
    CloneFrame *fa = arena_alloc(b->a, nf * sizeof(CloneFrame));
    memcpy(fa, frames, nf * sizeof(CloneFrame));
    t->as.cloneable.frames = fa;
    t->as.cloneable.n_lets = nl;
    if (nl) {
        CloneLet *la = arena_alloc(b->a, nl * sizeof(CloneLet));
        memcpy(la, lets, nl * sizeof(CloneLet));
        t->as.cloneable.lets = la;
    } else {
        t->as.cloneable.lets = NULL;
    }
    t->as.cloneable.n_outer_frames = n_outer;
    t->as.cloneable.if_cond = if_cond;
    t->as.cloneable.if_pure = if_pure;
    t->as.cloneable.if_when = if_when;
    t->as.cloneable.body = rest;
    return t;
}

/* N6.1: a subexpression that can be emitted wholesale by the direct emitter
 * (via CT_LETRAW) because it neither threads a continuation nor could reach one:
 * it contains no syntactic control op AND no call to a colored (may-capture)
 * function AND no indirect call (unknown coloring).  Nested fn/closure bodies are
 * call-graph boundaries -- not descended (a capture-free fn value is itself a
 * delegatable leaf; a capturing one is rejected so the has_capture cut is not
 * bypassed).  The scan is SOUND by construction: it returns true only for a
 * closed set of forms whose children it fully recurses; any unrecognized form
 * (or a control op / colored / indirect call) yields false and falls back.
 *
 * Delegating such a form in a lifted (zero-capture) body would still need the
 * has_capture cut to see its free vars; has_capture conservatively rejects an
 * un-scanned CT_LETRAW operand, so a delegated composite lands only where
 * has_capture is not the gate (the main function body) -- exactly the safe set. */
static bool safe_to_delegate(CpsB *b, const Expr *e) {
    e = ascribe_peel(e);
    if (!e) return false;
    if (is_atomic(e)) return true;
    if (is_delegatable_value(e)) return true;   /* capture-free fn value */
    switch (e->kind) {
        /* control operators: never delegatable (they thread a continuation). */
        case EX_PERFORM: case EX_HANDLE: case EX_RESUME: case EX_DISCONTINUE:
        case EX_RESET: case EX_SHIFT: case EX_SHIFT0:
        case EX_CLONEABLE_SHIFT:
        case EX_SERIAL_SHIFT:
            return false;
        /* U5 (cps-backend-unification): (async f) and (await fut) are self-
         * contained runtime calls (tur_async_fiber / tur_await_future) -- they do
         * NOT thread the caller's DK continuation (the future/result is an ordinary
         * value, and the async thunk's own effects are handled in its fiber scope).
         * Delegating them via CT_LETRAW reuses the proven fiber runtime and lets a
         * colored function that ALSO awaits stay CPS-emitted instead of wholly
         * evicting.  The delegated region is emitted by the direct emitter
         * (emit_value -> EX_ASYNC/EX_AWAIT), the bound future/result becomes a local,
         * and the CPS continuation runs after. */
        case EX_ASYNC: case EX_AWAIT:
            return true;
        /* U3 (cps-backend-unification): a (cloneable-reset body) is a
         * SELF-CONTAINED multi-shot delimited region.  The bare cloneable-shift
         * captures the rest of its reset body (a continuation that escapes the
         * shift expression), so it stays non-delegatable above; but the whole
         * cloneable-reset is emitted as a unit by the direct emitter
         * (emit_effects_cloneable_reset -> emit_cps_cloneable_reset), which owns
         * the dk_copy_range deep-clone + capture clone/drop glue.  Delegating the
         * reset via CT_LETRAW therefore reuses the proven multi-shot runtime and
         * lets a colored function that contains a cloneable-reset stay CPS-emitted
         * instead of wholly evicting.  The reset's value (often a continuation
         * handle resumed later) is bound and the CPS continuation runs after. */
        case EX_CLONEABLE_RESET:
            return true;
        /* U4 (cps-backend-unification): a (serial-reset body) is a SELF-CONTAINED
         * marshalable delimited region, analogous to cloneable-reset but with the
         * continuation serialized (save-cont!/resume-cont!) rather than deep-cloned.
         * The whole region is emitted as a unit by the direct emitter
         * (emit_effects_serial_reset -> emit_cps_serial_reset), which owns the
         * serial marshaling runtime.  Delegating the reset via CT_LETRAW reuses
         * that proven runtime and lets a colored function that contains a
         * serial-reset stay CPS-emitted instead of wholly evicting (the serial
         * runtime prelude is gated on *presence* of serial syntax, so the delegated
         * helpers are always in scope).  The bare serial-shift stays non-delegatable
         * above (it captures the rest of its reset body). */
        case EX_SERIAL_RESET:
            return true;
        /* nested fn defs: call-graph boundaries, delegatable as values. */
        case EX_FN_DEF: case EX_FN: case EX_CLOSURE:
            return is_delegatable_value(e);
        /* U2 (cps-backend-unification): (call/cc f) / (escape f) is an
         * UNDELIMITED escape whose continuation is captured at a *local* setjmp
         * landing that emit_cps_callcc establishes inline -- it does NOT thread
         * the DK continuation the way shift/perform do.  So delegating it via
         * CT_LETRAW is sound: the escape's setjmp/longjmp landing sits before the
         * bound result, and "the rest of the computation" from the call/cc site
         * is exactly the CPS continuation that runs after the binding.  This lets
         * a colored function that ALSO contains a call/cc/escape stay on the
         * CT-IR path instead of wholly evicting to the direct emitter.
         *
         * The receiver `f` is emitted by emit_cps_callcc regardless.  A capture-
         * free receiver (a plain fn, a fat-boxed fn, or a zero-capture closure)
         * delegates via the normal is-delegatable-value check.  A CAPTURING
         * closure also delegates: collect_caps (CT_LETRAW) walks the receiver's
         * free vars into the lifted continuation's env, and cap_add admits a
         * scalar (Copy) capture while a non-Copy capture bails to fallback. */
        case EX_CALLCC: {
            const Expr *f = ascribe_peel(e->as.callcc_.fn);
            return safe_to_delegate(b, e->as.callcc_.fn)
                || (f && f->kind == EX_CLOSURE);
        }
        case EX_CALL: {
            const Binding *fn = e->as.call_.fn_binding;
            if (!fn) return false;                 /* indirect: unknown coloring */
            if (callee_colored(b, fn)) return false;
            if (e->as.call_.fn_expr && !safe_to_delegate(b, e->as.call_.fn_expr))
                return false;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (!safe_to_delegate(b, e->as.call_.args[i])) return false;
            return true;
        }
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (!safe_to_delegate(b, e->as.builtin.args[i])) return false;
            return true;
        case EX_IF:
            return safe_to_delegate(b, e->as.if_.cond)
                && safe_to_delegate(b, e->as.if_.then_)
                && (!e->as.if_.else_or_null || safe_to_delegate(b, e->as.if_.else_or_null));
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (!safe_to_delegate(b, e->as.do_.items[i])) return false;
            return true;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (!safe_to_delegate(b, e->as.let_.bindings[i].init)) return false;
            return safe_to_delegate(b, e->as.let_.body);
        case EX_WHILE:
            return safe_to_delegate(b, e->as.while_.cond)
                && safe_to_delegate(b, e->as.while_.body);
        case EX_MATCH:
            if (!safe_to_delegate(b, e->as.match_.scrutinee)) return false;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                if (e->as.match_.arms[i].guard
                    && !safe_to_delegate(b, e->as.match_.arms[i].guard)) return false;
                if (!safe_to_delegate(b, e->as.match_.arms[i].body)) return false;
            }
            return true;
        case EX_SET:
            return safe_to_delegate(b, e->as.set_.value);
        case EX_CAST:
            return safe_to_delegate(b, e->as.cast_.expr);
        case EX_DEREF:
            return safe_to_delegate(b, e->as.deref_.expr);
        case EX_GET_FIELD:
            return safe_to_delegate(b, e->as.get_field_.struct_expr);
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
                if (!safe_to_delegate(b, e->as.make_struct_.field_values[i])) return false;
            return true;
        case EX_DEFAULT_OF:
            return true;
        /* Owning-value ops (rc/of, rc/clone, rc/drop, rc/strong-count, rc->ptr)
         * are control-op-free leaf ops the direct emitter already emits with the
         * correct control-block / refcount / drop-glue discipline; delegating a
         * form that contains one (e.g. `(make-struct S :field (rc/of x))`, a
         * constructor call whose argument allocates) keeps the whole thing on the
         * direct emitter -- which also resolves the monomorphized ctor name. */
        case EX_RC_OF:    return safe_to_delegate(b, e->as.rc_of_.expr);
        case EX_RC_CLONE: return safe_to_delegate(b, e->as.rc_clone_.expr);
        case EX_RC_DROP:  return safe_to_delegate(b, e->as.rc_drop_.expr);
        case EX_RC_COUNT: return safe_to_delegate(b, e->as.rc_count_.expr);
        case EX_RC_PTR:   return safe_to_delegate(b, e->as.rc_ptr_.expr);
        default:
            return false;   /* conservative: unrecognized form -> not delegatable */
    }
}

/* Every argument of a call is control-op-free and colored-call-free, so the
 * WHOLE call can be handed to the direct emitter (which emits the args inline and
 * resolves the monomorphized callee name -- constructors, specialized fns).  This
 * is a superset of call_args_atomic: it also delegates a call whose argument is a
 * non-atomic-but-delegatable form (e.g. `(rc/of x)`, a nested make-struct), which
 * would otherwise be atomized into a CT_LETCALL that mis-names the callee. */
static bool call_args_delegatable(CpsB *b, const Expr *e) {
    for (uint32_t i = 0; i < e->as.call_.n_args; i++)
        if (!safe_to_delegate(b, e->as.call_.args[i])) return false;
    return true;
}

static CAtom atomize(CpsB *b, Expr *e, Pending *p) {
    if (is_atomic(e)) return atom_of(e);
    CVar x = fresh_cvar(b, e ? &e->type : NULL);
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
/* A shift / shift0 body: apply the receiver `recv` (the (fn [k] ...) that gets
 * the captured continuation) to the delimited body expression `arg`, delivered
 * to the prompt.  Shared by both the plain and shift0 lowerings (which differ
 * only in the emitted dk_shift vs dk_shift0). */
static CTerm *cps_shift_body_kf(CpsB *b, Expr *recv, Expr *arg, Type *ty) {
    Expr *call = arena_alloc(b->a, sizeof(Expr));
    memset(call, 0, sizeof(Expr));
    call->kind = EX_CALL;
    call->type = *ty;
    call->as.call_.fn_binding =
        (recv && recv->kind == EX_VAR) ? recv->as.var.binding : NULL;
    call->as.call_.fn_expr = recv;
    call->as.call_.args = arena_alloc(b->a, sizeof(Expr *));
    call->as.call_.args[0] = arg;
    call->as.call_.n_args = 1;
    return cps_tail(b, call, kont_prompt(ty->kind));
}

static CTerm *cps_shift_body(CpsB *b, Expr *e) {
    return cps_shift_body_kf(b, e->as.shift_.k_fn, e->as.shift_.body, &e->type);
}

/* ---- algebraic effects: handle / perform / resume --------------------- *
 * Lowered to CT_HANDLE / CT_PERFORM / CT_RESUME.  `cont` is the continuation
 * term (an APPCONT to the enclosing kont in tail position, or the `rest` term in
 * bind position).  perform/resume atomize their sub-expressions into `p`. */

static CTerm *build_handle(CpsB *b, Expr *e, CVar x, CTerm *cont) {
    HandleExpr *h = e->as.handle_.handle;
    if (!h || h->n_cases < 1) {
        CTerm *u = new_term(b, CT_UNSUPPORTED);
        u->as.unsupported.why = "handle: no cases";
        return u;
    }
    CTerm *t = new_term(b, CT_HANDLE);
    t->as.handle.x = x;
    t->as.handle.delim = cps_tail(b, h->body, kont_prompt(e->type.kind));
    t->as.handle.n_cases = h->n_cases;
    CHandleCase *cs = arena_alloc(b->a, h->n_cases * sizeof(CHandleCase));
    for (uint32_t ci = 0; ci < h->n_cases; ci++) {
        HandleCase *c = &h->cases[ci];
        cs[ci].effect = c->effect_name;
        cs[ci].n_params = c->n_params;
        cs[ci].params = NULL;
        if (c->n_params) {
            const Binding **ps = arena_alloc(b->a, c->n_params * sizeof(const Binding *));
            for (uint8_t i = 0; i < c->n_params; i++) ps[i] = c->param_bindings[i];
            cs[ci].params = ps;
        }
        cs[ci].k = c->k_binding;
        /* The case clause delivers its value by return (KK_PROMPT), which
         * dk_perform routes to the handler's outer continuation. */
        cs[ci].case_body = cps_tail(b, c->body, kont_prompt(c->body ? c->body->type.kind : TY_INT));
    }
    t->as.handle.cases = cs;
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
    if (is_delegatable_owning(e) || is_delegatable_struct(e) || is_delegatable_value(e)) {
        /* bind the delegated op's result, then deliver it to the continuation. */
        CVar x = fresh_cvar(b, &e->type);
        CTerm *ac = new_term(b, CT_APPCONT);
        ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
        return build_letraw(b, e, x, ac);
    }
    switch (e->kind) {
        case EX_BUILTIN: {
            Pending p = {0};
            CAtom *args = arena_alloc(b->a, (e->as.builtin.n ? e->as.builtin.n : 1) * sizeof(CAtom));
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                args[i] = atomize(b, e->as.builtin.args[i], &p);
            CVar x = fresh_cvar(b, &e->type);
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
                /* Indirect call: the fn VALUE is the callee's direct entry point
                 * (a colored fn's value points at its direct wrapper, which
                 * installs its own root prompt), so an indirect call never
                 * participates in the caller's delimited-control chain -- its
                 * behaviour is identical whether the surrounding code is CPS- or
                 * direct-emitted.  Delegate it to the direct emitter with atomic
                 * args, exactly as an uncolored direct call is delegated. */
                if (!indirect_callee_ok(e->as.call_.fn_expr)) {
                    CTerm *t = new_term(b, CT_UNSUPPORTED);
                    t->as.unsupported.why = "indirect call through capturing closure";
                    return t;
                }
                if (!call_args_atomic(e)) {
                    CTerm *t = new_term(b, CT_UNSUPPORTED);
                    t->as.unsupported.why = "indirect call (non-atomic args)";
                    return t;
                }
                CVar x = fresh_cvar(b, &e->type);
                CTerm *ac = new_term(b, CT_APPCONT);
                ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
                return build_letraw(b, e, x, ac);
            }
            /* A cps->direct call to an uncolored callee with atomic args is
             * delegated to the direct emitter, which resolves the monomorphized
             * callee name (constructors, specialized fns) the CPS backend's own
             * naming does not.  Colored callees still thread the continuation. */
            if (!callee_colored(b, fn) && call_args_delegatable(b, e)) {
                CVar x = fresh_cvar(b, &e->type);
                CTerm *ac = new_term(b, CT_APPCONT);
                ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
                return build_letraw(b, e, x, ac);
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
                CVar x = fresh_cvar(b, &e->type);
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
                CVar discard = fresh_cvar(b, &e->as.do_.items[i]->type);
                rest = cps_bind(b, e->as.do_.items[i], discard, rest);
            }
            return rest;
        }
        case EX_RETURN:
            return cps_tail(b, e->as.return_.value, b->retk);
        case EX_RESET: {
            CVar x = fresh_cvar(b, &e->type);
            CTerm *t = new_term(b, CT_RESET);
            t->as.reset.x = x;
            t->as.reset.delim = cps_tail(b, e->as.reset_.body, kont_prompt(e->type.kind));
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            t->as.reset.body = ac;
            return t;
        }
        case EX_SHIFT: {
            CVar k = fresh_cvar(b, &e->type);
            k.name = "k'";
            CTerm *t = new_term(b, CT_SHIFT);
            t->as.shift.k = k;
            t->as.shift.body = cps_shift_body(b, e);
            return t;
        }
        case EX_SHIFT0: {
            CVar k = fresh_cvar(b, &e->type);
            k.name = "k'";
            CTerm *t = new_term(b, CT_SHIFT);
            t->as.shift.k = k;
            t->as.shift.shift0 = true;
            t->as.shift.body = cps_shift_body_kf(b, e->as.shift0_.k_fn,
                                                 e->as.shift0_.body, &e->type);
            return t;
        }
        case EX_CLONEABLE_RESET: {
            /* U3 Shape 1 native, else fall back to the CT_LETRAW delegation. */
            CVar x = fresh_cvar(b, &e->type);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            CTerm *nat = build_cloneable(b, e, x, ac);
            return nat ? nat : build_letraw(b, e, x, ac);
        }
        case EX_SERIAL_RESET: {
            /* U4: native arithmetic serial context, else delegate the marshalable
             * region so a colored function containing it stays CPS-emitted rather
             * than wholly evicting. */
            CVar x = fresh_cvar(b, &e->type);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            CTerm *nat = build_serial(b, e, x, ac);
            return nat ? nat : build_letraw(b, e, x, ac);
        }
        case EX_ASYNC: case EX_AWAIT: {
            /* U5: delegate the self-contained async region (async spawn / await)
             * so a colored function containing it stays CPS-emitted rather than
             * wholly evicting. */
            CVar x = fresh_cvar(b, &e->type);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            return build_letraw(b, e, x, ac);
        }
        case EX_HANDLE: {
            CVar x = fresh_cvar(b, &e->type);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            return build_handle(b, e, x, ac);
        }
        case EX_PERFORM: {
            Pending p = {0};
            CVar x = fresh_cvar(b, &e->type);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            CTerm *core = build_perform(b, e, x, ac, &p);
            return fold_pending(b, &p, core);
        }
        case EX_RESUME: {
            Pending p = {0};
            CVar x = fresh_cvar(b, &e->type);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            CTerm *core = build_resume(b, e, x, ac, &p);
            return fold_pending(b, &p, core);
        }
        case EX_CALLCC: {
            /* U7: native local-escape landing for a capture-free receiver, else
             * fall back to the CT_LETRAW delegation (emit_cps_callcc). */
            CVar x = fresh_cvar(b, &e->type);
            CTerm *ac = new_term(b, CT_APPCONT);
            ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
            CTerm *nat = build_callcc(b, e, x, ac);
            return nat ? nat : build_letraw(b, e, x, ac);
        }
        default: {
            /* N6.1: a control-op-free, colored-call-free form -- emit it wholesale
             * via the direct emitter, then deliver its result to the continuation. */
            if (safe_to_delegate(b, e)) {
                CVar x = fresh_cvar(b, &e->type);
                CTerm *ac = new_term(b, CT_APPCONT);
                ac->as.appcont.kont = kont; ac->as.appcont.v = atom_cvar(x);
                return build_letraw(b, e, x, ac);
            }
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
    if (is_delegatable_owning(e) || is_delegatable_struct(e) || is_delegatable_value(e))
        return build_letraw(b, e, x, rest);
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
                /* Indirect call: the fn value is the callee's direct entry, which
                 * never joins the caller's delimited-control chain -- delegate to
                 * the direct emitter with atomic args (see cps_tail). */
                if (!indirect_callee_ok(e->as.call_.fn_expr)) {
                    CTerm *t = new_term(b, CT_UNSUPPORTED);
                    t->as.unsupported.why = "indirect call through capturing closure";
                    return t;
                }
                if (!call_args_atomic(e)) {
                    CTerm *t = new_term(b, CT_UNSUPPORTED);
                    t->as.unsupported.why = "indirect call (non-atomic args)";
                    return t;
                }
                return build_letraw(b, e, x, rest);
            }
            /* cps->direct call to an uncolored callee with atomic args: delegate
             * to the direct emitter (monomorphized callee names). */
            if (!callee_colored(b, fn) && call_args_delegatable(b, e))
                return build_letraw(b, e, x, rest);
            Pending p = {0};
            uint32_t n = e->as.call_.n_args;
            CAtom *args = arena_alloc(b->a, (n ? n : 1) * sizeof(CAtom));
            for (uint32_t i = 0; i < n; i++)
                args[i] = atomize(b, e->as.call_.args[i], &p);
            if (callee_colored(b, fn)) {
                CVar j = fresh_cvar(b, x.type);
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
            CVar j = fresh_cvar(b, x.type);
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
                CVar discard = fresh_cvar(b, &e->as.do_.items[i]->type);
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
            CVar k = fresh_cvar(b, &e->type);
            k.name = "k'";
            CTerm *t = new_term(b, CT_SHIFT);
            t->as.shift.k = k;
            t->as.shift.body = cps_shift_body(b, e);
            return t;
        }
        case EX_SHIFT0: {
            CVar k = fresh_cvar(b, &e->type);
            k.name = "k'";
            CTerm *t = new_term(b, CT_SHIFT);
            t->as.shift.k = k;
            t->as.shift.shift0 = true;
            t->as.shift.body = cps_shift_body_kf(b, e->as.shift0_.k_fn,
                                                 e->as.shift0_.body, &e->type);
            return t;
        }
        case EX_CLONEABLE_RESET: {
            /* U3 Shape 1 native, else fall back to the CT_LETRAW delegation. */
            CTerm *nat = build_cloneable(b, e, x, rest);
            return nat ? nat : build_letraw(b, e, x, rest);
        }
        case EX_SERIAL_RESET: {
            /* U4: native arithmetic serial context, else delegate (see cps_tail). */
            CTerm *nat = build_serial(b, e, x, rest);
            return nat ? nat : build_letraw(b, e, x, rest);
        }
        case EX_ASYNC: case EX_AWAIT:
            /* U5: delegate the self-contained async region (see cps_tail). */
            return build_letraw(b, e, x, rest);
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
        case EX_CALLCC: {
            /* U7: native local-escape landing for a capture-free receiver, else
             * fall back to the CT_LETRAW delegation (see cps_tail). */
            CTerm *nat = build_callcc(b, e, x, rest);
            return nat ? nat : build_letraw(b, e, x, rest);
        }
        default: {
            /* N6.1: delegate a control-op-free, colored-call-free form to the
             * direct emitter (binds x, continues rest). */
            if (safe_to_delegate(b, e))
                return build_letraw(b, e, x, rest);
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
        case CA_FLOAT: fprintf(out, "%g", a->f); break;
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
            fprintf(out, "%s %s.\n", t->as.shift.shift0 ? "shift0" : "shift",
                    t->as.shift.k.name);
            cps_ir_print(t->as.shift.body, out, indent + 1);
            break;
        case CT_HANDLE:
            fprintf(out, "handle %s = {\n", t->as.handle.x.name);
            cps_ir_print(t->as.handle.delim, out, indent + 1);
            print_indent(out, indent); fputs("}\n", out);
            for (uint32_t ci = 0; ci < t->as.handle.n_cases; ci++) {
                const CHandleCase *c = &t->as.handle.cases[ci];
                print_indent(out, indent);
                fprintf(out, "with %s(", c->effect ? c->effect->name : "?");
                for (uint32_t i = 0; i < c->n_params; i++) {
                    if (i) fputc(' ', out);
                    fputs(c->params[i] && c->params[i]->name ? c->params[i]->name->name : "_", out);
                }
                fprintf(out, ") %s ->\n", c->k && c->k->name ? c->k->name->name : "k");
                cps_ir_print(c->case_body, out, indent + 1);
            }
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
        case CT_LETRAW:
            fprintf(out, "let %s = <owning-op %s>  ; direct-emitted\n",
                    t->as.letraw.x.name,
                    t->as.letraw.e && owning_operand(t->as.letraw.e) ? "rc" : "?");
            cps_ir_print(t->as.letraw.body, out, indent);
            break;
        case CT_CLONEABLE:
            fprintf(out, "let %s = %s-cont -> %s(<cont>)  ; %s %s"
                         "[%u frame(s), %u let(s)%s]\n",
                    t->as.cloneable.x.name,
                    t->as.cloneable.serial ? "serial" : "cloneable",
                    t->as.cloneable.receiver_expr ? "<closure>"
                        : (t->as.cloneable.receiver && t->as.cloneable.receiver->name
                           ? t->as.cloneable.receiver->name->name : "?"),
                    t->as.cloneable.serial ? "U4" : "U3",
                    t->as.cloneable.n_frames == 0 ? "Shape 1 " : "Shape 2 ",
                    t->as.cloneable.n_frames, t->as.cloneable.n_lets,
                    t->as.cloneable.if_cond ? ", if" : "");
            cps_ir_print(t->as.cloneable.body, out, indent);
            break;
        case CT_CALLCC:
            fprintf(out, "let %s = call/cc(<recv>)  ; U7 native escape landing\n",
                    t->as.callcc.x.name);
            cps_ir_print(t->as.callcc.body, out, indent);
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
