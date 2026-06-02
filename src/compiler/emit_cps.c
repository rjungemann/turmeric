/* emit_cps.c -- CPS-substrate codegen wiring (cps-transform-plan).
 *
 * Re-points the base delimited-control lowerings (reset / shift / shift0) off
 * the bespoke inlined runtime and onto the heap-reified CPS substrate's
 * multi-prompt delimited-control machine (`DK`, src/runtime/cps_prompt.{h,c}),
 * emitted directly into the generated program. See emit_cps.h for the model.
 *
 * Turmeric's shift is abortive: `(shift f v)` applies the receiver `f` to the
 * body value `v` and delivers the result to the nearest enclosing `reset`,
 * discarding the captured context (the sub-continuation is never handed back
 * to user code). The DK machine expresses exactly this: a `reset` is a prompt,
 * a `shift` is a DK shift node whose body returns the abort value, and dk_run
 * delivers it through the prompt. Because the abort value `f(v)` can be
 * precomputed in-scope, the shift node needs no per-frame environment capture
 * codegen -- the precomputed value rides in the node's `body_env`.
 *
 * Anything outside the supported subset returns NULL from emit_cps_reset, so
 * the caller falls back to the legacy lowering and that code stays
 * byte-identical.
 */

#include "emit_cps.h"

#include <stdlib.h>
#include <string.h>

#include "emit_internal.h"
#include "builtins.h"
#include "types.h"

/* ---- program scan: does it use base delimited control? ---------------- */

static bool uses_base_delimited(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_RESET:
        case EX_SHIFT:
        case EX_SHIFT0:
            return true;
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                if (uses_base_delimited(e->as.program.items[i])) return true;
            return false;
        case EX_FN_DEF:
            return e->as.fn_def_.fn && uses_base_delimited(e->as.fn_def_.fn->body);
        case EX_FN:
            return e->as.fn_.fn && uses_base_delimited(e->as.fn_.fn->body);
        case EX_CLOSURE:
            return e->as.closure_.closure && e->as.closure_.closure->fn &&
                   uses_base_delimited(e->as.closure_.closure->fn->body);
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (uses_base_delimited(e->as.let_.bindings[i].init)) return true;
            return uses_base_delimited(e->as.let_.body);
        case EX_IF:
            return uses_base_delimited(e->as.if_.cond) ||
                   uses_base_delimited(e->as.if_.then_) ||
                   uses_base_delimited(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (uses_base_delimited(e->as.do_.items[i])) return true;
            return false;
        case EX_WHILE:
            return uses_base_delimited(e->as.while_.cond) ||
                   uses_base_delimited(e->as.while_.body);
        case EX_SET:    return uses_base_delimited(e->as.set_.value);
        case EX_DEF:    return e->as.def_.init && uses_base_delimited(e->as.def_.init);
        case EX_RETURN: return e->as.return_.value && uses_base_delimited(e->as.return_.value);
        case EX_DEFER:  return uses_base_delimited(e->as.defer_.body);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (uses_base_delimited(e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (uses_base_delimited(e->as.call_.args[i])) return true;
            return false;
        default:
            return false;
    }
}

bool emit_cps_program_uses_delimited(const Expr *program) {
    return uses_base_delimited(program);
}

/* ---- program scan: does it use (call/cc f) / (escape f)? --------------- */

static bool uses_callcc(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_CALLCC:
            return true;
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                if (uses_callcc(e->as.program.items[i])) return true;
            return false;
        case EX_FN_DEF:
            return e->as.fn_def_.fn && uses_callcc(e->as.fn_def_.fn->body);
        case EX_FN:
            return e->as.fn_.fn && uses_callcc(e->as.fn_.fn->body);
        case EX_CLOSURE:
            return e->as.closure_.closure && e->as.closure_.closure->fn &&
                   uses_callcc(e->as.closure_.closure->fn->body);
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (uses_callcc(e->as.let_.bindings[i].init)) return true;
            return uses_callcc(e->as.let_.body);
        case EX_IF:
            return uses_callcc(e->as.if_.cond) ||
                   uses_callcc(e->as.if_.then_) ||
                   uses_callcc(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (uses_callcc(e->as.do_.items[i])) return true;
            return false;
        case EX_WHILE:
            return uses_callcc(e->as.while_.cond) || uses_callcc(e->as.while_.body);
        case EX_SET:    return uses_callcc(e->as.set_.value);
        case EX_DEF:    return e->as.def_.init && uses_callcc(e->as.def_.init);
        case EX_RETURN: return e->as.return_.value && uses_callcc(e->as.return_.value);
        case EX_DEFER:  return uses_callcc(e->as.defer_.body);
        case EX_RESET:  return uses_callcc(e->as.reset_.body);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (uses_callcc(e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (uses_callcc(e->as.call_.args[i])) return true;
            return false;
        default:
            return false;
    }
}

bool emit_cps_program_uses_callcc(const Expr *program) {
    return uses_callcc(program);
}

/* ---- the abort value: f(v) must round-trip through intptr_t -------------
 * The DK node carries the precomputed abort value in its intptr_t body_env, so
 * the value must survive an intptr_t round trip. Integers, booleans, nil and
 * pointers (cstr) do; floats do not (a (intptr_t) cast truncates). */
static bool ty_intptr_safe(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_BOOL: case TY_NIL: case TY_CSTR:
            return true;
        default:
            return false;
    }
}

/* A let binding emitted ahead of the shift becomes a plain C local; restrict to
 * scalar types whose C representation is unambiguous (no carrier-ABI/struct/RC
 * subtleties). */
static bool ty_simple_local(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_BOOL: case TY_NIL: case TY_CSTR: case TY_FLOAT:
            return true;
        default:
            return false;
    }
}

/* ---- reaches_shift: can `e` dynamically reach a base shift bound to the
 * enclosing reset? Stops at nested reset / cloneable / serial / fn boundaries
 * (those self-delimit or are not our base shift). --------------------------*/
static bool reaches_shift(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_SHIFT:
        case EX_SHIFT0:
            return true;
        /* boundaries that do not propagate a shift up to our prompt */
        case EX_RESET:
        case EX_CLONEABLE_RESET:
        case EX_CLONEABLE_SHIFT:
        case EX_SERIAL_RESET:
        case EX_SERIAL_SHIFT:
        case EX_FN:
        case EX_FN_DEF:
        case EX_CLOSURE:
            return false;
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (reaches_shift(e->as.let_.bindings[i].init)) return true;
            return reaches_shift(e->as.let_.body);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (reaches_shift(e->as.do_.items[i])) return true;
            return false;
        case EX_IF:
            return reaches_shift(e->as.if_.cond) ||
                   reaches_shift(e->as.if_.then_) ||
                   reaches_shift(e->as.if_.else_or_null);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (reaches_shift(e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (reaches_shift(e->as.call_.args[i])) return true;
            return false;
        case EX_RETURN:
            return reaches_shift(e->as.return_.value);
        case EX_SET:
            return reaches_shift(e->as.set_.value);
        default:
            return false;
    }
}

/* ---- can_lower: pure feasibility check (NO emission) --------------------
 * Mirrors the structure of emit_first_shift below. Run first so that a
 * fall-back never leaves half-emitted code. */
static bool can_lower(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_SHIFT:
        case EX_SHIFT0:
            return ty_intptr_safe(e->type.kind);
        case EX_LET:
            if (expr_contains_return_or_throw(e->as.let_.body)) return false;
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                const Expr *init = e->as.let_.bindings[i].init;
                if (reaches_shift(init)) return can_lower(init);
                const Binding *b = e->as.let_.bindings[i].binding;
                if (!b || !ty_simple_local(b->type.kind)) return false;
                if (expr_contains_return_or_throw(init)) return false;
            }
            return can_lower(e->as.let_.body);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                const Expr *it = e->as.do_.items[i];
                if (reaches_shift(it)) return can_lower(it);
                if (expr_contains_return_or_throw(it)) return false;
            }
            return false;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                const Expr *a = e->as.builtin.args[i];
                if (reaches_shift(a)) return can_lower(a);
                if (expr_contains_return_or_throw(a)) return false;
            }
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                const Expr *a = e->as.call_.args[i];
                if (reaches_shift(a)) return can_lower(a);
                if (expr_contains_return_or_throw(a)) return false;
            }
            return false;
        case EX_IF:
            /* Only a shift in the condition is statically pin-pointable; a
             * shift in a branch is genuine runtime branching -- not supported
             * by the abort-value model. */
            if (reaches_shift(e->as.if_.cond)) return can_lower(e->as.if_.cond);
            return false;
        default:
            return false;
    }
}

/* ---- emit_first_shift: emit preceding effects, then the abort value -----
 * Returns the C variable holding the abort value f(v); sets *is_shift0. The
 * caller guarantees can_lower(e) was true, so this never falls through. */
static char *emit_first_shift(EmitCtx *ctx, Buf *body, const Expr *e,
                              bool *is_shift0) {
    switch (e->kind) {
        case EX_SHIFT:
            *is_shift0 = false;
            return emit_effects_shift(ctx, body, e);
        case EX_SHIFT0:
            *is_shift0 = true;
            return emit_effects_shift0(ctx, body, e);
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                const Expr *init = e->as.let_.bindings[i].init;
                if (reaches_shift(init))
                    return emit_first_shift(ctx, body, init, is_shift0);
                /* Emit the binding as a plain C local so a later shift body
                 * (e.g. a closure receiver) can reference it. */
                const Binding *b = e->as.let_.bindings[i].binding;
                char *bn = name_for_binding(ctx, b);
                char *iv = emit_value(ctx, body, init);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s = %s;\n", emit_type_c_name(ctx, b->type), bn, iv);
                indent_buf(body, ctx->indent);
                buf_printf(body, "(void)%s;\n", bn);
                free(bn);
                free(iv);
            }
            return emit_first_shift(ctx, body, e->as.let_.body, is_shift0);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                const Expr *it = e->as.do_.items[i];
                if (reaches_shift(it))
                    return emit_first_shift(ctx, body, it, is_shift0);
                emit_stmt(ctx, body, it);   /* preceding effect */
            }
            break;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                const Expr *a = e->as.builtin.args[i];
                if (reaches_shift(a))
                    return emit_first_shift(ctx, body, a, is_shift0);
                char *v = emit_value(ctx, body, a);   /* preceding effect; result discarded (abortive) */
                free(v);
            }
            break;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                const Expr *a = e->as.call_.args[i];
                if (reaches_shift(a))
                    return emit_first_shift(ctx, body, a, is_shift0);
                char *v = emit_value(ctx, body, a);
                free(v);
            }
            break;
        case EX_IF:
            if (reaches_shift(e->as.if_.cond))
                return emit_first_shift(ctx, body, e->as.if_.cond, is_shift0);
            break;
        default:
            break;
    }
    /* Unreachable when can_lower(e) held. */
    return NULL;
}

char *emit_cps_reset(EmitCtx *ctx, Buf *body, const Expr *e) {
    const Expr *rb = e->as.reset_.body;
    /* No shift bound to this reset -> nothing for the substrate to do. */
    if (!reaches_shift(rb)) return NULL;
    /* Outside the supported subset -> let the caller fall back cleanly. */
    if (!can_lower(rb)) return NULL;

    bool is_shift0 = false;
    char *fv = emit_first_shift(ctx, body, rb, &is_shift0);
    if (!fv) return NULL;   /* defensive: should not happen after can_lower */

    /* Run the delimited computation on the substrate: a single prompt and a
     * shift node whose body returns the precomputed abort value. */
    const char *shift_ctor = is_shift0 ? "dk_shift0" : "dk_shift";
    char *result = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body,
        "%s %s = (%s)dk_run(%s(1, __dk_abort_body, (intptr_t)(%s), "
        "dk_prompt(1, dk_done())), 0);\n",
        emit_type_c_name(ctx, e->type), result, emit_type_c_name(ctx, e->type),
        shift_ctor, fv);
    free(fv);
    return result;
}

/* ---- (call/cc f) / (escape f): undelimited upward escape -------------- */

char *emit_cps_callcc(EmitCtx *ctx, Buf *body, const Expr *e) {
    const Expr *f = e->as.callcc_.fn;
    const char *rty = emit_type_c_name(ctx, e->type);

    char *cc  = fresh_tmp(ctx);   /* the escape-continuation landing record */
    char *res = fresh_tmp(ctx);   /* the call/cc result */

    /* Landing: capture is O(1) and depth-unbounded (no 16-frame ceiling) --
     * f may invoke the continuation from arbitrarily deep, longjmp returns
     * straight here. The setjmp pattern mirrors the cloneable-reset boundary. */
    indent_buf(body, ctx->indent);
    buf_printf(body, "tur_escape_cont %s;\n", cc);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.valid = 1;\n", cc);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s %s;\n", rty, res);
    indent_buf(body, ctx->indent);
    buf_printf(body, "if (setjmp(%s.buf) == 0) {\n", cc);
    ctx->indent++;

    /* Normal path: run f with the landing handle as its continuation. */
    char *fval = emit_value(ctx, body, f);
    char *fr   = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    if (f->kind == EX_CLOSURE) {
        struct Closure *closure = f->as.closure_.closure;
        char *thunk_name;
        if (closure->fn->binding) {
            thunk_name = raw_name_for_binding(closure->fn->binding);
        } else {
            thunk_name = malloc(64);
            snprintf(thunk_name, 64, "__fn_anon_%d",
                     closure->fn->n_params > 0 ? closure->fn->params[0]->id : 0);
        }
        buf_printf(body, "%s %s = %s(%s, (int64_t)(intptr_t)&%s);\n",
                   rty, fr, thunk_name, fval, cc);
        free(thunk_name);
    } else {
        buf_printf(body, "%s %s = %s((int64_t)(intptr_t)&%s);\n",
                   rty, fr, fval, cc);
    }
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s = %s;\n", res, fr);
    /* f returned normally: the captured continuation is now dead. */
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.valid = 0;\n", cc);
    free(fr);
    free(fval);
    ctx->indent--;
    indent_buf(body, ctx->indent);
    buf_puts(body, "} else {\n");
    ctx->indent++;
    /* Resumed path: an upward (tur_escape_resume %s v) delivered v here. */
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s = (%s)%s.result;\n", res, rty, cc);
    ctx->indent--;
    indent_buf(body, ctx->indent);
    buf_puts(body, "}\n");

    free(cc);
    return res;
}

void emit_cps_callcc_prelude(Buf *out) {
    buf_puts(out,
"/* call-cc-completion: undelimited escape-continuation runtime.\n"
" * (call/cc f)/(escape f) set up a landing here; the continuation handle f\n"
" * receives is &tur_escape_cont. Invoking it (tur_escape_resume) is a one-shot\n"
" * upward escape: it longjmps back to the call/cc site delivering the value.\n"
" * Capture is O(1) and unbounded -- no TUR_CONT_MAX_CAPTURED_FRAMES ceiling. */\n"
"typedef struct tur_escape_cont {\n"
"    jmp_buf buf;\n"
"    int64_t result;  /* value delivered by tur_escape_resume */\n"
"    bool    valid;   /* false once the call/cc prompt has returned */\n"
"} tur_escape_cont;\n"
"static int64_t tur_escape_resume(int64_t k, int64_t v) {\n"
"    tur_escape_cont *cc = (tur_escape_cont *)(intptr_t)k;\n"
"    if (!cc || !cc->valid) {\n"
"        fprintf(stderr, \"tur: continuation invoked after its call/cc prompt returned\\n\");\n"
"        abort();\n"
"    }\n"
"    cc->result = v;\n"
"    longjmp(cc->buf, 1);\n"
"    return 0; /* unreachable */\n"
"}\n"
"\n");
}

/* =========================================================================
 * CPS9 (cps-transform-plan): cloneable continuations on the DK machine.
 *
 * The headline "next increment" called out in CPS8: re-point cloneable
 * continuations (cloneable-reset / cloneable-shift, the substrate behind
 * call/cc*) onto the multi-prompt DK machine so a captured continuation
 * actually reifies the *delimited context* (the frames between the shift and
 * its reset) and replays it -- multi-shot -- via dk_invoke.
 *
 * Before this, the emitted cloneable continuation was trivial: the captured
 * "rest of the computation" was a no-op `return __value`, so a context like
 * (cloneable-reset (+ 10 (cloneable-shift k ...))) lost the `+ 10` on resume
 * (and, with the shift nested inside an operand, did not even compile). Now
 * the context is lambda-lifted into DK frames; the cloneable continuation
 * stores the reified sub-continuation chain as its env, with a DK-invoking
 * cont_fn -- reusing the existing tur_cloneable_cont machinery wholesale
 * (resume/clone/drop are unchanged; multi-shot is dk_invoke's internal copy).
 *
 * Supported context subset (anything else falls back to the legacy lowering,
 * keeping its emitted C byte-identical): a non-empty chain of single-hole
 * integer binary ops (+, -, *) wrapping exactly one cloneable-shift whose
 * receiver has no live captures, with an intptr-safe (int) result. The
 * non-hole operands are pure int expressions, evaluated once at capture time
 * and reused on every resume (the standard delimited-control semantics).
 * ========================================================================= */

#define CL_MAX_CTX_FRAMES 32

typedef struct {
    const char *c_op;     /* "+", "-", "*" */
    bool        hole_left;/* true iff args[0] is the hole (reaches the shift) */
    const Expr *other;    /* the non-hole operand (captured into the frame env) */
} ClFrame;

/* Boundary-aware: can `e` dynamically reach a shift of kind `target` bound to
 * the enclosing reset? Stops at nested resets/shifts of any other flavor and at
 * fn boundaries (those self-delimit or are not our shift). Shared by the
 * cloneable (CPS9) and serial (CPS10) context walks. */
static bool reaches_shift_kind(const Expr *e, ExprKind target) {
    if (!e) return false;
    if (e->kind == target) return true;
    switch (e->kind) {
        case EX_CLONEABLE_SHIFT:
        case EX_SERIAL_SHIFT:
        case EX_CLONEABLE_RESET:
        case EX_RESET:
        case EX_SERIAL_RESET:
        case EX_SHIFT:
        case EX_SHIFT0:
        case EX_CALLCC:
        case EX_FN:
        case EX_FN_DEF:
        case EX_CLOSURE:
            return false;   /* target already matched above; these self-delimit */
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (reaches_shift_kind(e->as.builtin.args[i], target)) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (reaches_shift_kind(e->as.call_.args[i], target)) return true;
            return false;
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (reaches_shift_kind(e->as.let_.bindings[i].init, target)) return true;
            return reaches_shift_kind(e->as.let_.body, target);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (reaches_shift_kind(e->as.do_.items[i], target)) return true;
            return false;
        case EX_IF:
            return reaches_shift_kind(e->as.if_.cond, target) ||
                   reaches_shift_kind(e->as.if_.then_, target) ||
                   reaches_shift_kind(e->as.if_.else_or_null, target);
        case EX_RETURN:
            return reaches_shift_kind(e->as.return_.value, target);
        case EX_SET:
            return reaches_shift_kind(e->as.set_.value, target);
        default:
            return false;
    }
}

static bool cl_op_supported(const char *op) {
    return op && (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                  strcmp(op, "*") == 0);
}

/* Walk the reset body down to its single shift of kind `target`, recording each
 * enclosing single-hole int binop as a context frame (frames[0] = outermost).
 * Returns the shift expr and *n_out frames (>= 0), or NULL if the body is not a
 * supported context chain. Shared by the cloneable (CPS9) and serial (CPS10)
 * lowerings. */
static const Expr *collect_ctx(const Expr *rb, ExprKind target,
                               ClFrame *frames, uint32_t *n_out) {
    uint32_t n = 0;
    const Expr *cur = rb;
    while (cur && cur->kind == EX_BUILTIN) {
        const BuiltinSpec *spec = cur->as.builtin.spec;
        if (!spec || cur->as.builtin.n != 2) return NULL;
        if (!cl_op_supported(spec->c_op)) return NULL;
        if (cur->type.kind != TY_INT) return NULL;
        const Expr *a0 = cur->as.builtin.args[0];
        const Expr *a1 = cur->as.builtin.args[1];
        bool h0 = reaches_shift_kind(a0, target);
        bool h1 = reaches_shift_kind(a1, target);
        if (h0 == h1) return NULL;                 /* need exactly one hole */
        const Expr *hole  = h0 ? a0 : a1;
        const Expr *other = h0 ? a1 : a0;
        if (other->type.kind != TY_INT) return NULL;
        if (reaches_shift_kind(other, target)) return NULL;
        if (expr_contains_return_or_throw(other)) return NULL;
        if (n >= CL_MAX_CTX_FRAMES) return NULL;
        frames[n].c_op = spec->c_op;
        frames[n].hole_left = h0;
        frames[n].other = other;
        n++;
        cur = hole;
    }
    if (cur && cur->kind == target) {
        *n_out = n;
        return cur;
    }
    return NULL;
}

/* Pure feasibility check (NO emission): is this cloneable-reset lowerable onto
 * the DK machine? Requires a non-empty supported context (so existing
 * empty-context fixtures stay on the legacy path, byte-identical). */
static bool cl_can_lower(const Expr *e) {
    if (!e || e->kind != EX_CLONEABLE_RESET) return false;
    if (!ty_intptr_safe(e->type.kind)) return false;
    ClFrame frames[CL_MAX_CTX_FRAMES];
    uint32_t nf = 0;
    const Expr *shift = collect_ctx(e->as.cloneable_reset_.body, EX_CLONEABLE_SHIFT, frames, &nf);
    if (!shift || nf == 0) return false;
    if (shift->as.cloneable_shift_.n_live_captures != 0) return false;
    if (!shift->as.cloneable_shift_.k_fn) return false;
    return true;
}

/* Emit a context frame fn for op/hole-side. The frame receives the captured
 * non-hole operand as `env` and the resumed value as `value`. */
static void cl_emit_frame_fn(Buf *hb, const char *name, const ClFrame *f) {
    const char *expr;
    if (strcmp(f->c_op, "+") == 0)      expr = "env + value";
    else if (strcmp(f->c_op, "*") == 0) expr = "env * value";
    else /* "-" */                      expr = f->hole_left ? "value - env"
                                                            : "env - value";
    buf_printf(hb,
        "static intptr_t %s(intptr_t env, intptr_t value) { return %s; }\n",
        name, expr);
}

char *emit_cps_cloneable_reset(EmitCtx *ctx, Buf *body, const Expr *e) {
    if (!cl_can_lower(e)) return NULL;
    if (!ctx->pending_handler_fns) return NULL;   /* need file scope for helpers */

    ClFrame frames[CL_MAX_CTX_FRAMES];
    uint32_t nf = 0;
    const Expr *shift = collect_ctx(e->as.cloneable_reset_.body, EX_CLONEABLE_SHIFT, frames, &nf);
    const Expr *k_fn = shift->as.cloneable_shift_.k_fn;
    Buf *hb = ctx->pending_handler_fns;
    int id = ctx->tmp_n++;

    /* 1. Context frame fns (file scope). */
    char frame_names[CL_MAX_CTX_FRAMES][48];
    for (uint32_t i = 0; i < nf; i++) {
        snprintf(frame_names[i], sizeof frame_names[i], "__cc_ctx_%d_%u", id, i);
        cl_emit_frame_fn(hb, frame_names[i], &frames[i]);
    }

    /* 2. The shift body (file scope): wrap the captured sub-continuation in a
     *    cloneable_cont (env = an owned DK copy) and hand it to the receiver f.
     *    f's return value becomes the reset's value. */
    char body_name[48];
    snprintf(body_name, sizeof body_name, "__cc_body_%d", id);
    buf_printf(hb,
        "static intptr_t %s(intptr_t env, DK *subk) {\n"
        "    DK *__cap = dk_copy_range(subk, NULL);\n"
        "    tur_cloneable_cont *__k = tur_cloneable_cont_alloc("
        "__dk_cont_fn, __cap, __dk_env_clone, __dk_env_drop);\n",
        body_name);
    if (k_fn->kind == EX_CLOSURE) {
        struct Closure *closure = k_fn->as.closure_.closure;
        char *thunk_name;
        if (closure->fn->binding) {
            thunk_name = raw_name_for_binding(closure->fn->binding);
        } else {
            thunk_name = malloc(64);
            snprintf(thunk_name, 64, "__fn_anon_%d",
                     closure->fn->n_params > 0 ? closure->fn->params[0]->id : 0);
        }
        buf_printf(hb,
            "    return (intptr_t)%s((int64_t)env, (int64_t)(intptr_t)__k);\n}\n",
            thunk_name);
        free(thunk_name);
    } else {
        buf_printf(hb,
            "    return (intptr_t)((int64_t (*)(int64_t))(intptr_t)env)"
            "((int64_t)(intptr_t)__k);\n}\n");
    }

    /* 3. At the reset site: evaluate the non-hole operands (once, in source
     *    order) and the receiver's env, build the DK chain, and run it. */
    char *op_vals[CL_MAX_CTX_FRAMES];
    for (uint32_t i = 0; i < nf; i++)
        op_vals[i] = emit_value(ctx, body, frames[i].other);
    char *k_fn_val = emit_value(ctx, body, k_fn);

    char *chain = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body, "DK *%s = dk_prompt(1, dk_done());\n", chain);
    /* Wrap outermost-first so the innermost frame ends up adjacent to the
     * shift and runs first on resume (inside-out evaluation). */
    for (uint32_t i = 0; i < nf; i++) {
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s = dk_frame(%s, (intptr_t)(%s), %s);\n",
                   chain, frame_names[i], op_vals[i], chain);
        free(op_vals[i]);
    }
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s = dk_shift(1, %s, (intptr_t)(%s), %s);\n",
               chain, body_name, k_fn_val, chain);
    free(k_fn_val);

    const char *rty = emit_type_c_name(ctx, e->type);
    char *result = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s %s = (%s)dk_run(%s, 0);\n", rty, result, rty, chain);
    indent_buf(body, ctx->indent);
    buf_printf(body, "dk_free(%s);\n", chain);
    free(chain);
    return result;
}

/* Program scan: does any cloneable-reset lower onto the DK machine? Gates the
 * DK runtime prelude + the cloneable<->DK bridge. Mirrors cl_can_lower. */
static bool uses_cloneable_dk(const Expr *e) {
    if (!e) return false;
    if (e->kind == EX_CLONEABLE_RESET && cl_can_lower(e)) return true;
    switch (e->kind) {
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                if (uses_cloneable_dk(e->as.program.items[i])) return true;
            return false;
        case EX_FN_DEF:
            return e->as.fn_def_.fn && uses_cloneable_dk(e->as.fn_def_.fn->body);
        case EX_FN:
            return e->as.fn_.fn && uses_cloneable_dk(e->as.fn_.fn->body);
        case EX_CLOSURE:
            return e->as.closure_.closure && e->as.closure_.closure->fn &&
                   uses_cloneable_dk(e->as.closure_.closure->fn->body);
        case EX_CLONEABLE_RESET:
            return uses_cloneable_dk(e->as.cloneable_reset_.body);
        case EX_RESET:
            return uses_cloneable_dk(e->as.reset_.body);
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (uses_cloneable_dk(e->as.let_.bindings[i].init)) return true;
            return uses_cloneable_dk(e->as.let_.body);
        case EX_IF:
            return uses_cloneable_dk(e->as.if_.cond) ||
                   uses_cloneable_dk(e->as.if_.then_) ||
                   uses_cloneable_dk(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (uses_cloneable_dk(e->as.do_.items[i])) return true;
            return false;
        case EX_WHILE:
            return uses_cloneable_dk(e->as.while_.cond) ||
                   uses_cloneable_dk(e->as.while_.body);
        case EX_SET:    return uses_cloneable_dk(e->as.set_.value);
        case EX_DEF:    return e->as.def_.init && uses_cloneable_dk(e->as.def_.init);
        case EX_RETURN: return e->as.return_.value && uses_cloneable_dk(e->as.return_.value);
        case EX_DEFER:  return uses_cloneable_dk(e->as.defer_.body);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (uses_cloneable_dk(e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (uses_cloneable_dk(e->as.call_.args[i])) return true;
            return false;
        default:
            return false;
    }
}

bool emit_cps_program_uses_cloneable_dk(const Expr *program) {
    return uses_cloneable_dk(program);
}

/* Bridge: a cloneable continuation whose captured context is a DK chain.
 * Reuses the emitted tur_cloneable_cont struct -- the DK chain rides in `env`,
 * resume dispatches to dk_invoke (multi-shot), clone copies the chain, drop
 * frees it. Emitted after both the cloneable runtime and the DK machine. */
void emit_cps_cloneable_bridge_prelude(Buf *out) {
    buf_puts(out,
"/* cps-transform-plan (CPS9): cloneable continuation <-> DK bridge.\n"
" * The reified delimited context (a DK chain) rides in the cloneable cont's\n"
" * env; resume = dk_invoke (multi-shot, copies internally), clone = chain\n"
" * copy, drop = dk_free. Reuses the existing tur_cloneable_cont machinery. */\n"
"static int64_t __dk_cont_fn(void *env, int64_t value) {\n"
"    return (int64_t)dk_invoke((DK *)env, (intptr_t)value);\n"
"}\n"
"static void *__dk_env_clone(const void *env) {\n"
"    return (void *)dk_copy_range((const DK *)env, NULL);\n"
"}\n"
"static void __dk_env_drop(void *env) { dk_free((DK *)env); }\n"
"\n");
}

/* =========================================================================
 * CPS10 (cps-transform-plan, CPS5.4): serializable continuations on the DK
 * machine.
 *
 * serial-shift captures the delimited context as a DK chain, exactly like the
 * cloneable path (CPS9) -- but the captured continuation is *marshalable*. A
 * heap-reified sub-continuation is a flat list of (frame, env) nodes, so it can
 * be serialized by writing each frame's stable TAG plus its env, and rebuilt by
 * mapping the tag back to a frame function -- no fiber/stack snapshot to walk
 * (the CPS5.4 insight). This is strictly simpler than snapshotting a native
 * stack and is portable: tags are stable integers, not code addresses.
 *
 * To make the tags stable the context frames are a FIXED set emitted once into
 * the preamble (not per-site lambda-lifted as in CPS9): +, *, and - (with the
 * hole on either side), keyed by SK_TAG_*. The env (the captured non-hole
 * operand) is an int and is marshaled inline; richer env types via the
 * Serializable typeclass are a follow-on. The supported context subset is the
 * same single-hole integer (+, -, *) chain as CPS9; serial-shift may also be
 * the whole reset body (empty context).
 *
 * Surface runtime (emitted builtins, registered in builtins.c):
 *   tur_serial_cont_resume(k, v)      -- run the captured chain on v
 *   tur_serial_cont_serialize(k)      -- marshal to a length-prefixed buffer
 *   tur_serial_cont_deserialize(bytes)-- rebuild a runnable chain from a buffer
 * ========================================================================= */

/* Pure feasibility check (NO emission) for a serial-reset. Unlike the cloneable
 * path, an empty context (serial-shift IS the whole body) is allowed -- serial
 * continuations are an all-new feature with no legacy snapshots to preserve. */
static bool sk_can_lower(const Expr *e) {
    if (!e || e->kind != EX_SERIAL_RESET) return false;
    if (!ty_intptr_safe(e->type.kind)) return false;
    ClFrame frames[CL_MAX_CTX_FRAMES];
    uint32_t nf = 0;
    const Expr *shift = collect_ctx(e->as.serial_reset_.body, EX_SERIAL_SHIFT,
                                    frames, &nf);
    if (!shift) return false;
    if (!shift->as.serial_shift_.k_fn) return false;
    return true;
}

/* The stable tag for a context frame (op + hole side), shared by the emitted
 * frame table. Mirrors the SK_TAG_* enum emitted in the prelude. */
static int sk_tag_for_frame(const ClFrame *f) {
    if (strcmp(f->c_op, "+") == 0) return 1;       /* SK_TAG_ADD:  env + v */
    if (strcmp(f->c_op, "*") == 0) return 2;       /* SK_TAG_MUL:  env * v */
    /* "-" */ return f->hole_left ? 4              /* SK_TAG_SUBL: v - env */
                                  : 3;             /* SK_TAG_SUBR: env - v */
}

char *emit_cps_serial_reset(EmitCtx *ctx, Buf *body, const Expr *e) {
    if (!sk_can_lower(e)) return NULL;
    if (!ctx->pending_handler_fns) return NULL;

    ClFrame frames[CL_MAX_CTX_FRAMES];
    uint32_t nf = 0;
    const Expr *shift = collect_ctx(e->as.serial_reset_.body, EX_SERIAL_SHIFT,
                                    frames, &nf);
    const Expr *k_fn = shift->as.serial_shift_.k_fn;
    Buf *hb = ctx->pending_handler_fns;
    int id = ctx->tmp_n++;

    /* The shift body (file scope): own a copy of the captured sub-continuation
     * (a DK chain of tagged frames) and hand it to the receiver f as the serial
     * continuation handle. f's return value becomes the reset's value. */
    char body_name[48];
    snprintf(body_name, sizeof body_name, "__sk_body_%d", id);
    buf_printf(hb,
        "static intptr_t %s(intptr_t env, DK *subk) {\n"
        "    DK *__cap = dk_copy_range(subk, NULL);\n",
        body_name);
    if (k_fn->kind == EX_CLOSURE) {
        struct Closure *closure = k_fn->as.closure_.closure;
        char *thunk_name;
        if (closure->fn->binding) {
            thunk_name = raw_name_for_binding(closure->fn->binding);
        } else {
            thunk_name = malloc(64);
            snprintf(thunk_name, 64, "__fn_anon_%d",
                     closure->fn->n_params > 0 ? closure->fn->params[0]->id : 0);
        }
        buf_printf(hb,
            "    return (intptr_t)%s((int64_t)env, (int64_t)(intptr_t)__cap);\n}\n",
            thunk_name);
        free(thunk_name);
    } else {
        buf_printf(hb,
            "    return (intptr_t)((int64_t (*)(int64_t))(intptr_t)env)"
            "((int64_t)(intptr_t)__cap);\n}\n");
    }

    /* At the reset site: evaluate the non-hole operands and the receiver env,
     * build the DK chain from the fixed tagged frames, and run it. */
    char *op_vals[CL_MAX_CTX_FRAMES];
    int   op_tags[CL_MAX_CTX_FRAMES];
    for (uint32_t i = 0; i < nf; i++) {
        op_vals[i] = emit_value(ctx, body, frames[i].other);
        op_tags[i] = sk_tag_for_frame(&frames[i]);
    }
    char *k_fn_val = emit_value(ctx, body, k_fn);

    char *chain = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body, "DK *%s = dk_prompt(1, dk_done());\n", chain);
    for (uint32_t i = 0; i < nf; i++) {       /* outermost-first -> innermost at front */
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s = dk_frame(__sk_frame_for_tag(%d), (intptr_t)(%s), %s);\n",
                   chain, op_tags[i], op_vals[i], chain);
        free(op_vals[i]);
    }
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s = dk_shift(1, %s, (intptr_t)(%s), %s);\n",
               chain, body_name, k_fn_val, chain);
    free(k_fn_val);

    const char *rty = emit_type_c_name(ctx, e->type);
    char *result = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s %s = (%s)dk_run(%s, 0);\n", rty, result, rty, chain);
    indent_buf(body, ctx->indent);
    buf_printf(body, "dk_free(%s);\n", chain);
    free(chain);
    return result;
}

/* Program scan: does any serial-reset lower onto the DK machine? Gates the DK
 * runtime prelude + the serial marshaling runtime. Mirrors sk_can_lower. */
static bool uses_serial_dk(const Expr *e) {
    if (!e) return false;
    if (e->kind == EX_SERIAL_RESET && sk_can_lower(e)) return true;
    switch (e->kind) {
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                if (uses_serial_dk(e->as.program.items[i])) return true;
            return false;
        case EX_FN_DEF:
            return e->as.fn_def_.fn && uses_serial_dk(e->as.fn_def_.fn->body);
        case EX_FN:
            return e->as.fn_.fn && uses_serial_dk(e->as.fn_.fn->body);
        case EX_CLOSURE:
            return e->as.closure_.closure && e->as.closure_.closure->fn &&
                   uses_serial_dk(e->as.closure_.closure->fn->body);
        case EX_SERIAL_RESET:
            return uses_serial_dk(e->as.serial_reset_.body);
        case EX_CLONEABLE_RESET:
            return uses_serial_dk(e->as.cloneable_reset_.body);
        case EX_RESET:
            return uses_serial_dk(e->as.reset_.body);
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (uses_serial_dk(e->as.let_.bindings[i].init)) return true;
            return uses_serial_dk(e->as.let_.body);
        case EX_IF:
            return uses_serial_dk(e->as.if_.cond) ||
                   uses_serial_dk(e->as.if_.then_) ||
                   uses_serial_dk(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (uses_serial_dk(e->as.do_.items[i])) return true;
            return false;
        case EX_WHILE:
            return uses_serial_dk(e->as.while_.cond) ||
                   uses_serial_dk(e->as.while_.body);
        case EX_SET:    return uses_serial_dk(e->as.set_.value);
        case EX_DEF:    return e->as.def_.init && uses_serial_dk(e->as.def_.init);
        case EX_RETURN: return e->as.return_.value && uses_serial_dk(e->as.return_.value);
        case EX_DEFER:  return uses_serial_dk(e->as.defer_.body);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (uses_serial_dk(e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (uses_serial_dk(e->as.call_.args[i])) return true;
            return false;
        default:
            return false;
    }
}

bool emit_cps_program_uses_serial_dk(const Expr *program) {
    return uses_serial_dk(program);
}

/* Serial marshaling runtime: the fixed tagged context frames + the resume /
 * serialize / deserialize builtins. Emitted after the DK machine prelude,
 * gated on emit_cps_program_uses_serial_dk. */
void emit_cps_serial_runtime_prelude(Buf *out) {
    buf_puts(out,
"/* cps-transform-plan (CPS10 / CPS5.4): serializable continuations.\n"
" * The captured continuation is a DK chain of tagged context frames; it is\n"
" * marshaled by writing each frame's stable tag + env (no code addresses), and\n"
" * rebuilt by mapping the tag back to a frame fn. Buffer layout matches stdlib\n"
" * `bytes` (int64 length prefix, then payload). */\n"
"enum { SK_TAG_ADD = 1, SK_TAG_MUL = 2, SK_TAG_SUBR = 3, SK_TAG_SUBL = 4 };\n"
"static intptr_t __sk_add (intptr_t env, intptr_t v) { return env + v; }\n"
"static intptr_t __sk_mul (intptr_t env, intptr_t v) { return env * v; }\n"
"static intptr_t __sk_subr(intptr_t env, intptr_t v) { return env - v; } /* other - hole */\n"
"static intptr_t __sk_subl(intptr_t env, intptr_t v) { return v - env; } /* hole - other */\n"
"static DKFrame __sk_frame_for_tag(int tag) {\n"
"    switch (tag) {\n"
"        case SK_TAG_ADD:  return __sk_add;\n"
"        case SK_TAG_MUL:  return __sk_mul;\n"
"        case SK_TAG_SUBR: return __sk_subr;\n"
"        case SK_TAG_SUBL: return __sk_subl;\n"
"        default: return NULL;\n"
"    }\n"
"}\n"
"static int __sk_tag_for_frame(DKFrame f) {\n"
"    if (f == __sk_add)  return SK_TAG_ADD;\n"
"    if (f == __sk_mul)  return SK_TAG_MUL;\n"
"    if (f == __sk_subr) return SK_TAG_SUBR;\n"
"    if (f == __sk_subl) return SK_TAG_SUBL;\n"
"    return 0;\n"
"}\n"
"/* Resume a (possibly deserialized) serial continuation on a value. */\n"
"static int64_t tur_serial_cont_resume(int64_t k, int64_t v) {\n"
"    return (int64_t)dk_invoke((DK *)(intptr_t)k, (intptr_t)v);\n"
"}\n"
"/* Marshal: [int64 payload_len][int64 n][ (int64 tag, int64 env) * n ]. The\n"
" * captured frames run front-to-back, so they are written in chain order. */\n"
"static int64_t tur_serial_cont_serialize(int64_t k) {\n"
"    DK *p = (DK *)(intptr_t)k;\n"
"    int64_t n = 0;\n"
"    for (DK *q = p; q && q->kind == DKK_FRAME; q = q->next) n++;\n"
"    int64_t payload = (int64_t)sizeof(int64_t) * (1 + 2 * n);\n"
"    int64_t *buf = (int64_t *)malloc(sizeof(int64_t) + (size_t)payload);\n"
"    if (!buf) abort();\n"
"    buf[0] = payload; buf[1] = n;\n"
"    int64_t i = 2;\n"
"    for (DK *q = p; q && q->kind == DKK_FRAME; q = q->next) {\n"
"        buf[i++] = (int64_t)__sk_tag_for_frame(q->fn);\n"
"        buf[i++] = (int64_t)q->env;\n"
"    }\n"
"    return (int64_t)(intptr_t)buf;\n"
"}\n"
"/* Rebuild a runnable chain [frames..., prompt, done] from a marshaled buffer.\n"
" * Frames are prepended in reverse so the first-written frame stays at front. */\n"
"static int64_t tur_serial_cont_deserialize(int64_t bytes) {\n"
"    int64_t *buf = (int64_t *)(intptr_t)bytes;\n"
"    int64_t n = buf[1];\n"
"    DK *chain = dk_prompt(1, dk_done());\n"
"    for (int64_t i = n - 1; i >= 0; i--) {\n"
"        int64_t tag = buf[2 + 2 * i];\n"
"        int64_t env = buf[2 + 2 * i + 1];\n"
"        chain = dk_frame(__sk_frame_for_tag((int)tag), (intptr_t)env, chain);\n"
"    }\n"
"    return (int64_t)(intptr_t)chain;\n"
"}\n"
"\n");
}

/* ---- runtime prelude: a faithful C port of src/runtime/cps_prompt.c ----- */

void emit_cps_runtime_prelude(Buf *out) {
    buf_puts(out,
"/* CPS substrate (cps-transform-plan): multi-prompt delimited-control machine.\n"
" * Heap-reified continuation chains (DK); a reset is a prompt, a shift slices\n"
" * the chain up to the nearest prompt. Faithful port of src/runtime/cps_prompt.c.\n"
" * Capture is O(depth-of-slice) and unbounded -- no 16-frame ceiling. */\n"
"#define DK_ROOT_TAG 0\n"
"typedef struct DK DK;\n"
"typedef intptr_t (*DKFrame)(intptr_t env, intptr_t value);\n"
"typedef intptr_t (*DKBody)(intptr_t env, DK *subk);\n"
"typedef enum { DKK_DONE, DKK_FRAME, DKK_PROMPT, DKK_SHIFT, DKK_SHIFT0 } DKKind;\n"
"struct DK {\n"
"    DKKind kind; DKFrame fn; intptr_t env; int tag;\n"
"    DKBody body; intptr_t body_env; DK *next;\n"
"};\n"
"static DK *dk_new(DKKind kind, DK *next) {\n"
"    DK *k = (DK *)calloc(1, sizeof(DK)); k->kind = kind; k->next = next; return k;\n"
"}\n"
"static DK *dk_done(void) { return dk_new(DKK_DONE, NULL); }\n"
"static DK *dk_frame(DKFrame fn, intptr_t env, DK *next) {\n"
"    DK *k = dk_new(DKK_FRAME, next); k->fn = fn; k->env = env; return k;\n"
"}\n"
"static DK *dk_prompt(int tag, DK *next) {\n"
"    DK *k = dk_new(DKK_PROMPT, next); k->tag = tag; return k;\n"
"}\n"
"static DK *dk_shift_impl(DKKind kind, int tag, DKBody body, intptr_t env, DK *next) {\n"
"    DK *k = dk_new(kind, next); k->tag = tag; k->body = body; k->body_env = env; return k;\n"
"}\n"
"static DK *dk_shift(int tag, DKBody body, intptr_t env, DK *next) {\n"
"    return dk_shift_impl(DKK_SHIFT, tag, body, env, next);\n"
"}\n"
"static DK *dk_shift0(int tag, DKBody body, intptr_t env, DK *next) {\n"
"    return dk_shift_impl(DKK_SHIFT0, tag, body, env, next);\n"
"}\n"
"static DK *dk_copy_node(const DK *n) {\n"
"    DK *c = dk_new(n->kind, NULL); c->fn = n->fn; c->env = n->env; c->tag = n->tag;\n"
"    c->body = n->body; c->body_env = n->body_env; return c;\n"
"}\n"
"static DK *dk_copy_range(const DK *from, const DK *stop) {\n"
"    DK *head = NULL, *tail = NULL;\n"
"    for (const DK *p = from; p && p != stop; p = p->next) {\n"
"        DK *c = dk_copy_node(p);\n"
"        if (!head) head = tail = c; else { tail->next = c; tail = c; }\n"
"    }\n"
"    return head;\n"
"}\n"
"static DK *dk_append(DK *a, DK *b) {\n"
"    if (!a) return b;\n"
"    DK *p = a;\n"
"    while (p->next) p = p->next;\n"
"    p->next = b;\n"
"    return a;\n"
"}\n"
"static void dk_free(DK *k) { while (k) { DK *n = k->next; free(k); k = n; } }\n"
"static intptr_t dk_run_impl(DK *k, intptr_t v, bool root) {\n"
"    while (k) {\n"
"        switch (k->kind) {\n"
"            case DKK_DONE: return v;\n"
"            case DKK_PROMPT: k = k->next; break;\n"
"            case DKK_FRAME: v = k->fn(k->env, v); k = k->next; break;\n"
"            case DKK_SHIFT:\n"
"            case DKK_SHIFT0: {\n"
"                DK *P = k->next;\n"
"                while (P && !(P->kind == DKK_PROMPT && P->tag == k->tag)\n"
"                         && P->kind != DKK_DONE) P = P->next;\n"
"                bool to_root = (!P || P->kind == DKK_DONE);\n"
"                bool reinstall = (k->kind == DKK_SHIFT);\n"
"                DK *sub = dk_copy_range(k->next, P);\n"
"                DK *tail = reinstall\n"
"                    ? dk_prompt(to_root ? DK_ROOT_TAG : k->tag, dk_done()) : dk_done();\n"
"                sub = dk_append(sub, tail);\n"
"                intptr_t bodyval = k->body(k->body_env, sub);\n"
"                dk_free(sub);\n"
"                if (to_root) return bodyval;\n"
"                k = P->next; v = bodyval; break;\n"
"            }\n"
"        }\n"
"    }\n"
"    (void)root; return v;\n"
"}\n"
"static intptr_t dk_run(DK *k, intptr_t v)      { return dk_run_impl(k, v, false); }\n"
"static intptr_t dk_run_root(DK *k, intptr_t v) { return dk_run_impl(k, v, true); }\n"
"static intptr_t dk_invoke(DK *sub, intptr_t w) {\n"
"    DK *c = dk_copy_range(sub, NULL); intptr_t r = dk_run_impl(c, w, false);\n"
"    dk_free(c); return r;\n"
"}\n"
"/* Abortive shift body: deliver the precomputed receiver result f(v),\n"
" * ignoring the captured sub-continuation (Turmeric shift never resumes it). */\n"
"static intptr_t __dk_abort_body(intptr_t env, DK *subk) { (void)subk; return env; }\n"
"\n");
}
