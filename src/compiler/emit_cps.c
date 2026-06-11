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
#include "typeclass.h"
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
    const char    *c_op;     /* builtin frame: "+","-","*","/"; NULL for a call frame */
    const Binding *call_fn;  /* call frame: target top-level fn binding; else NULL */
    bool           hole_left;/* true iff the hole is the left operand/arg */
    const Expr    *other;    /* the non-hole operand (captured into the frame env) */
    TypeKind       env_kind; /* type kind of `other` -- for typed serial marshaling */
    /* cps-transform-plan (a): when the env is a nominal type with a Serializable
     * instance (not the inline int/cstr kinds), the instance's method C names;
     * NULL for inline-marshaled envs. */
    const char    *env_ser;
    const char    *env_deser;
} ClFrame;

/* cps-transform-plan (grammar extension): a pure `let` binding the context walk
 * descended through. Its init does not reach the shift, so it is emitted once at
 * the reset site as a plain C local ahead of the captured-operand evaluation;
 * the context frames' non-hole operands may then reference it by name. */
#define CL_MAX_CTX_LETS 16
typedef struct {
    const Binding *binding;  /* the let-bound variable */
    const Expr    *init;     /* its pure initializer */
} CtxLet;

/* cps-transform-plan (a): find a Serializable instance for `t` and return its
 * serialize/deserialize method C names. Restricted to nominal (TY_STRUCT, incl.
 * opaque) types -- primitive int/cstr envs use the inline codec. Scans the
 * program's instance definitions (codegen has no typeclass env). */
static bool sk_find_serializable(const Expr *program, Type t,
                                 const char **ser_out, const char **deser_out) {
    if (!program || program->kind != EX_PROGRAM) return false;
    if (t.kind != TY_STRUCT || !t.as.struct_.def || !t.as.struct_.def->name)
        return false;
    const char *tname = t.as.struct_.def->name;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *it = program->as.program.items[i];
        if (!it || it->kind != EX_INSTANCE_DEF) continue;
        const TypeClassInstance *inst = it->as.instance_def_.instance;
        if (!inst || !inst->typeclass || !inst->typeclass->name) continue;
        if (strcmp(inst->typeclass->name->name, "Serializable") != 0) continue;
        if (inst->n_type_args < 1) continue;
        Type at = inst->type_args[0];
        if (at.kind != TY_STRUCT || !at.as.struct_.def || !at.as.struct_.def->name)
            continue;
        if (strcmp(at.as.struct_.def->name, tname) != 0) continue;
        /* Found the matching instance. When the caller only wants existence
         * (NULL out-params, e.g. a feasibility check), don't allocate names. */
        if (!ser_out || !deser_out) return true;
        const char *ser = NULL, *deser = NULL;
        const TypeClass *tc = inst->typeclass;
        for (uint8_t j = 0; j < tc->n_methods && j < inst->n_method_impls; j++) {
            if (!inst->method_impls[j] || !inst->method_impls[j]->binding) continue;
            const char *mn = tc->methods[j].name ? tc->methods[j].name->name : "";
            char *cn = raw_name_for_binding(inst->method_impls[j]->binding);
            if (strcmp(mn, "serialize") == 0) ser = cn;
            else if (strcmp(mn, "deserialize") == 0) deser = cn;
            else free(cn);
        }
        if (ser && deser) { *ser_out = ser; *deser_out = deser; return true; }
        free((void *)ser); free((void *)deser);
        return false;
    }
    return false;
}

/* The C cast applied to an intptr-carried env/value when handed to a frame's
 * call target. Only int- and cstr-kinded operands are supported (both fit in an
 * intptr_t and are the kinds the serial marshaler can encode). */
static const char *c_cast_for_kind(TypeKind k) {
    return (k == TY_CSTR) ? "(const char *)" : "(int64_t)";
}

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
                  strcmp(op, "*") == 0 || strcmp(op, "/") == 0);
}

/* The C expression a context frame computes, in terms of `env` (the captured
 * non-hole operand) and `value` (the resumed value). Shared by the cloneable
 * (per-site, CPS9) and serial (fixed tagged, CPS10) frame emitters so the two
 * paths stay in lock-step. Division aborts on a zero divisor, matching the
 * BS_DIV_CHECK lowering (emit_core.c). */
static const char *frame_c_expr(const char *op, bool hole_left) {
    if (strcmp(op, "+") == 0) return "env + value";
    if (strcmp(op, "*") == 0) return "env * value";
    if (strcmp(op, "-") == 0) return hole_left ? "value - env" : "env - value";
    /* "/" : the hole is the dividend when hole_left, else the divisor. */
    if (hole_left)  /* value / env  -- divisor is the captured env */
        return "(env) ? (value / env) "
               ": (fprintf(stderr, \"division by zero\\n\"), abort(), 0)";
    /* env / value  -- divisor is the resumed value */
    return "(value) ? (env / value) "
           ": (fprintf(stderr, \"division by zero\\n\"), abort(), 0)";
}

/* Walk the reset body down to its single shift of kind `target`, recording each
 * enclosing single-hole int binop as a context frame (frames[0] = outermost).
 * Returns the shift expr and *n_out frames (>= 0), or NULL if the body is not a
 * supported context chain. Shared by the cloneable (CPS9) and serial (CPS10)
 * lowerings. */
/* Env/result kinds a context frame can carry: int and cstr (both fit an
 * intptr_t; the serial marshaler encodes each by kind -- int inline, cstr
 * length-prefixed). With value-typed cont<T> the surface can now express a
 * cstr-valued continuation, so cstr contexts (e.g. (cat "hi " [])) type-check. */
static bool env_kind_ok(TypeKind k) { return k == TY_INT || k == TY_CSTR; }

static const Expr *collect_ctx(const Expr *rb, ExprKind target,
                               ClFrame *frames, uint32_t *n_out,
                               const Expr *program, bool want_names,
                               CtxLet *lets, uint32_t *n_lets_out) {
    uint32_t n = 0;
    uint32_t nl = 0;
    const Expr *cur = rb;
    for (;;) {
        if (n >= CL_MAX_CTX_FRAMES) return NULL;
        if (cur && cur->kind == EX_LET) {
            /* A pure `let` in the context: each init must not reach the shift
             * (it is prelude, evaluated once at capture time) and must be a
             * simple scalar local; the body carries the hole. The bindings are
             * recorded so the emit site can lay them down as C locals ahead of
             * the captured-operand evaluation. */
            if (!lets || !n_lets_out) return NULL;
            const Expr *body = cur->as.let_.body;
            if (!reaches_shift_kind(body, target)) return NULL;
            for (uint32_t i = 0; i < cur->as.let_.n; i++) {
                const Expr *init = cur->as.let_.bindings[i].init;
                const Binding *b = cur->as.let_.bindings[i].binding;
                if (reaches_shift_kind(init, target)) return NULL;
                if (!b || !ty_simple_local(b->type.kind)) return NULL;
                if (expr_contains_return_or_throw(init)) return NULL;
                if (nl >= CL_MAX_CTX_LETS) return NULL;
                lets[nl].binding = b;
                lets[nl].init = init;
                nl++;
            }
            cur = body;
            continue;
        }
        if (cur && cur->kind == EX_DO) {
            /* do-sequence with a statement-position shift:
             *   (do PRELUDE... (shift k v) TAIL)
             * The items before the shift are side-effecting prelude, evaluated
             * once at capture time (recorded as binding-less prelude entries).
             * The single optional tail item is the captured continuation: an
             * ignore-value frame that, on resume, runs regardless of the resume
             * value. This is the "skip expensive-init, resume into the loop"
             * shape the application-image-dumps plan targets. */
            if (!lets || !n_lets_out) return NULL;
            uint32_t N = cur->as.do_.n;
            int32_t m = -1;
            for (uint32_t i = 0; i < N; i++) {
                if (reaches_shift_kind(cur->as.do_.items[i], target)) {
                    if (m >= 0) return NULL;   /* at most one hole */
                    m = (int32_t)i;
                }
            }
            if (m < 0) return NULL;
            /* The shift must sit in statement position (be the do item itself).
             * The tail items (m, N) are the captured continuation. */
            if (cur->as.do_.items[m]->kind != target) return NULL;
            /* Prelude items [0, m): emitted for side effect at the reset site. */
            for (int32_t i = 0; i < m; i++) {
                if (nl >= CL_MAX_CTX_LETS) return NULL;
                if (expr_contains_return_or_throw(cur->as.do_.items[i])) return NULL;
                lets[nl].binding = NULL;                 /* side-effect prelude */
                lets[nl].init    = cur->as.do_.items[i];
                nl++;
            }
            /* Tail items (m, N): each a 0-arg top-level call -> an ignore-value
             * frame. They run in source order on resume, so record them in
             * reverse: the first tail item is innermost (adjacent to the shift,
             * runs first), the last is outermost (runs last, its value is the
             * reset's value). */
            for (int32_t i = (int32_t)N - 1; i > m; i--) {
                const Expr *tail = cur->as.do_.items[i];
                if (tail->kind != EX_CALL || tail->as.call_.n_args != 0 ||
                    !tail->as.call_.fn_binding || tail->as.call_.fn_expr) return NULL;
                const Binding *fb = tail->as.call_.fn_binding;
                if (fb->type.kind != TY_FN || fb->type.as.fn.arity != 0) return NULL;
                if (fb->closure_fn_binding) return NULL;
                /* The outermost tail item yields the reset's value (scalar);
                 * inner ones are run for effect and their value discarded. */
                if (i == (int32_t)N - 1 && !env_kind_ok(tail->type.kind)) return NULL;
                if (n >= CL_MAX_CTX_FRAMES) return NULL;
                frames[n].c_op = NULL;
                frames[n].call_fn = fb;
                frames[n].hole_left = true;   /* unused for a 0-arg frame */
                frames[n].other = NULL;       /* no env */
                frames[n].env_kind = TY_INT;  /* inline zero env */
                frames[n].env_ser = NULL;
                frames[n].env_deser = NULL;
                n++;
            }
            cur = cur->as.do_.items[m];   /* the shift; loop exits below */
            continue;
        }
        if (cur && cur->kind == EX_BUILTIN) {
            /* Arithmetic frame: a single-hole int binop (+, -, *, /). */
            const BuiltinSpec *spec = cur->as.builtin.spec;
            if (!spec || cur->as.builtin.n != 2) return NULL;
            if (!cl_op_supported(spec->c_op)) return NULL;
            if (cur->type.kind != TY_INT) return NULL;
            const Expr *a0 = cur->as.builtin.args[0];
            const Expr *a1 = cur->as.builtin.args[1];
            bool h0 = reaches_shift_kind(a0, target);
            bool h1 = reaches_shift_kind(a1, target);
            if (h0 == h1) return NULL;             /* need exactly one hole */
            const Expr *other = h0 ? a1 : a0;
            if (other->type.kind != TY_INT) return NULL;
            if (reaches_shift_kind(other, target)) return NULL;
            if (expr_contains_return_or_throw(other)) return NULL;
            frames[n].c_op = spec->c_op;
            frames[n].call_fn = NULL;
            frames[n].hole_left = h0;
            frames[n].other = other;
            frames[n].env_kind = TY_INT;
            frames[n].env_ser = NULL;
            frames[n].env_deser = NULL;
            n++;
            cur = h0 ? a0 : a1;
        } else if (cur && cur->kind == EX_CALL && cur->as.call_.n_args == 2 &&
                   cur->as.call_.fn_binding && !cur->as.call_.fn_expr) {
            /* Call frame: a 2-arg call to a resolved top-level function with one
             * hole arg and one pure env arg. The env may be int/cstr (inline) or
             * -- when `program` is supplied (serial path) -- a nominal type with
             * a Serializable instance (marshaled via that instance, CPS step a). */
            const Binding *fb = cur->as.call_.fn_binding;
            if (fb->type.kind != TY_FN || fb->type.as.fn.arity != 2) return NULL;
            if (fb->closure_fn_binding) return NULL;   /* needs hidden env arg */
            if (!env_kind_ok(cur->type.kind)) return NULL;   /* result: scalar */
            const Expr *a0 = cur->as.call_.args[0];
            const Expr *a1 = cur->as.call_.args[1];
            bool h0 = reaches_shift_kind(a0, target);
            bool h1 = reaches_shift_kind(a1, target);
            if (h0 == h1) return NULL;
            /* the hole slot's param must be a carrier scalar (the resume value) */
            TypeKind hole_param = h0 ? fb->type.as.fn.arg_kinds[0]
                                     : fb->type.as.fn.arg_kinds[1];
            if (!env_kind_ok(hole_param)) return NULL;
            const Expr *other = h0 ? a1 : a0;   /* the env operand */
            if (reaches_shift_kind(other, target)) return NULL;
            if (expr_contains_return_or_throw(other)) return NULL;
            const char *eser = NULL, *edeser = NULL;
            if (env_kind_ok(other->type.kind)) {
                /* int/cstr env -- inline codec */
            } else if (program &&
                       sk_find_serializable(program, other->type,
                                            want_names ? &eser : NULL,
                                            want_names ? &edeser : NULL)) {
                /* nominal env with a Serializable instance -- instance codec */
            } else {
                return NULL;
            }
            frames[n].c_op = NULL;
            frames[n].call_fn = fb;
            frames[n].hole_left = h0;
            frames[n].other = other;
            frames[n].env_kind = other->type.kind;
            frames[n].env_ser = eser;
            frames[n].env_deser = edeser;
            n++;
            cur = h0 ? a0 : a1;
        } else if (cur && cur->kind == EX_CALL && cur->as.call_.n_args == 1 &&
                   cur->as.call_.fn_binding && !cur->as.call_.fn_expr) {
            /* 1-arg call frame: (f HOLE) -- the sole argument is the hole, so
             * there is no captured env. The frame applies f to the resume
             * value; it marshals as the target name with a zero (unused) env. */
            const Binding *fb = cur->as.call_.fn_binding;
            if (fb->type.kind != TY_FN || fb->type.as.fn.arity != 1) return NULL;
            if (fb->closure_fn_binding) return NULL;   /* needs hidden env arg */
            if (!env_kind_ok(cur->type.kind)) return NULL;   /* result: scalar */
            const Expr *a0 = cur->as.call_.args[0];
            if (!reaches_shift_kind(a0, target)) return NULL;
            if (!env_kind_ok(fb->type.as.fn.arg_kinds[0])) return NULL;
            frames[n].c_op = NULL;
            frames[n].call_fn = fb;
            frames[n].hole_left = true;   /* sole arg carries the hole */
            frames[n].other = NULL;       /* no env */
            frames[n].env_kind = TY_INT;  /* marshaled as an inline zero env */
            frames[n].env_ser = NULL;
            frames[n].env_deser = NULL;
            n++;
            cur = a0;
        } else {
            break;
        }
    }
    if (cur && cur->kind == target) {
        *n_out = n;
        if (n_lets_out) *n_lets_out = nl;
        return cur;
    }
    return NULL;
}

/* cps-transform-plan (grammar extension): the one runtime branch point the
 * resumable lowering supports.  If `body` is an `(if cond THEN ELSE)` whose
 * condition is pure (does not reach the shift) and whose arms split cleanly
 * into exactly one shift-bearing arm and one pure arm, return the shift-bearing
 * arm (the context body to walk) and set *cond_out / *else_out (the pure arm) /
 * *when_out (true iff the shift arm is the `then` arm, so the emitted C test is
 * `if (cond)` vs `if (!(cond))`).  Otherwise return NULL.  The condition is
 * evaluated once at the reset site; the shift path runs the DK chain built from
 * the shift-bearing arm, the other path yields the pure arm's value directly --
 * the frame-chain model cannot express a shift reached only on one runtime
 * branch any other way. */
static const Expr *ctx_if_branch(const Expr *body, ExprKind shift_kind,
                                 const Expr **cond_out, const Expr **else_out,
                                 bool *when_out) {
    if (!body || body->kind != EX_IF) return NULL;
    const Expr *cond = body->as.if_.cond;
    const Expr *thn  = body->as.if_.then_;
    const Expr *els  = body->as.if_.else_or_null;
    if (!cond || !thn || !els) return NULL;            /* need both arms */
    if (reaches_shift_kind(cond, shift_kind)) return NULL;
    if (expr_contains_return_or_throw(cond)) return NULL;
    bool ht = reaches_shift_kind(thn, shift_kind);
    bool he = reaches_shift_kind(els, shift_kind);
    if (ht == he) return NULL;                         /* exactly one shift arm */
    const Expr *shift_arm = ht ? thn : els;
    const Expr *pure_arm  = ht ? els : thn;
    if (reaches_shift_kind(pure_arm, shift_kind)) return NULL;   /* defensive */
    if (expr_contains_return_or_throw(pure_arm)) return NULL;
    *cond_out = cond;
    *else_out = pure_arm;
    *when_out = ht;
    return shift_arm;
}

/* cps-transform-plan (grammar extension, last shape): an `if` branch point
 * reached *through* outer context frames, e.g. (reset (+ 5 (if c THEN[shift]
 * ELSE))). ctx_if_branch only matches an `if` that is the whole reset body; the
 * helpers below locate an `if` sitting *under* a chain of context frames and let
 * the lowering split it into a shift-bearing context body and a pure-arm context
 * body, each still a flat frame chain the existing emitters handle. */

/* The unique index of the arg in `args[0..n)` that reaches a shift of kind
 * `target`, or -1 if zero or more than one arg reaches it (so the descent is
 * unambiguous; collect_ctx enforces the "exactly one hole" rule later). */
static int shift_child_index(Expr *const *args, uint32_t n, ExprKind target) {
    int found = -1;
    for (uint32_t i = 0; i < n; i++) {
        if (reaches_shift_kind(args[i], target)) {
            if (found >= 0) return -1;   /* ambiguous */
            found = (int)i;
        }
    }
    return found;
}

/* Walk down the context spine (single-hole binops, 2-arg calls, pure `let`
 * bodies) from `cur` following the unique shift-reaching child. Return the first
 * enclosing `if` reached this way, or NULL if the spine reaches the shift itself
 * (a flat context, no branch point) or hits an unsupported node. The frame
 * validity of the spine (op support, operand purity/type) is *not* checked here;
 * the caller defers that to collect_ctx run over the substituted shift body. */
static const Expr *find_ctx_if(const Expr *cur, ExprKind target) {
    for (int guard = 0; cur && guard < 4096; guard++) {
        if (cur->kind == EX_IF)   return cur;     /* the branch point */
        if (cur->kind == target)  return NULL;    /* flat: shift, no `if` */
        const Expr *next = NULL;
        if (cur->kind == EX_BUILTIN && cur->as.builtin.n == 2) {
            int h = shift_child_index(cur->as.builtin.args, 2, target);
            if (h < 0) return NULL;
            next = cur->as.builtin.args[h];
        } else if (cur->kind == EX_CALL && cur->as.call_.n_args == 2 &&
                   cur->as.call_.fn_binding && !cur->as.call_.fn_expr) {
            int h = shift_child_index(cur->as.call_.args, 2, target);
            if (h < 0) return NULL;
            next = cur->as.call_.args[h];
        } else if (cur->kind == EX_LET || cur->kind == EX_LETREC) {
            if (!reaches_shift_kind(cur->as.let_.body, target)) return NULL;
            next = cur->as.let_.body;
        } else {
            return NULL;
        }
        cur = next;
    }
    return NULL;
}

/* A small pool of compile-time-only allocations (spine clones + their cloned
 * arg arrays) freed in bulk once the substituted bodies have been emitted. */
typedef struct { void *items[128]; uint32_t n; } ClonePool;
static void *pool_take(ClonePool *p, size_t sz) {
    void *m = malloc(sz);
    if (p->n < 128) p->items[p->n++] = m;   /* spine is tiny; cap defensively */
    return m;
}
static void pool_free(ClonePool *p) {
    for (uint32_t i = 0; i < p->n; i++) free(p->items[i]);
    p->n = 0;
}

/* Return a copy of `orig` in which the spine node `if_node` (found by
 * find_ctx_if) is replaced by `replacement`, following the same unique
 * shift-reaching child path. Only the spine nodes (and the binop/call arg arrays
 * carrying the hole) are freshly allocated -- every off-spine sub-expression is
 * shared with `orig`. The clones live until pool_free. */
static const Expr *clone_spine(const Expr *orig, const Expr *if_node,
                               const Expr *replacement, ExprKind target,
                               ClonePool *pool) {
    if (orig == if_node) return replacement;
    Expr *c = pool_take(pool, sizeof(Expr));
    *c = *orig;
    if (orig->kind == EX_BUILTIN && orig->as.builtin.n == 2) {
        uint32_t n = orig->as.builtin.n;
        int h = shift_child_index(orig->as.builtin.args, n, target);
        Expr **na = pool_take(pool, n * sizeof(Expr *));
        for (uint32_t i = 0; i < n; i++) na[i] = orig->as.builtin.args[i];
        na[h] = (Expr *)clone_spine(orig->as.builtin.args[h], if_node,
                                    replacement, target, pool);
        c->as.builtin.args = na;
    } else if (orig->kind == EX_CALL && orig->as.call_.n_args == 2) {
        uint32_t n = orig->as.call_.n_args;
        int h = shift_child_index(orig->as.call_.args, n, target);
        Expr **na = pool_take(pool, n * sizeof(Expr *));
        for (uint32_t i = 0; i < n; i++) na[i] = orig->as.call_.args[i];
        na[h] = (Expr *)clone_spine(orig->as.call_.args[h], if_node,
                                    replacement, target, pool);
        c->as.call_.args = na;
    } else if (orig->kind == EX_LET || orig->kind == EX_LETREC) {
        c->as.let_.body = (Expr *)clone_spine(orig->as.let_.body, if_node,
                                              replacement, target, pool);
    }
    return c;
}

/* Pure feasibility check (NO emission): is this cloneable-reset lowerable onto
 * the DK machine? Requires a non-empty supported context (so existing
 * empty-context fixtures stay on the legacy path, byte-identical) -- except for
 * the if-shaped body, which never matched the legacy path, so an empty context
 * in the shift arm is admitted there. The `if` may be the whole body or sit
 * beneath outer context frames (the last grammar shape). */
static bool cl_can_lower(const Expr *e) {
    if (!e || e->kind != EX_CLONEABLE_RESET) return false;
    if (!ty_intptr_safe(e->type.kind)) return false;
    const Expr *inner = e->as.cloneable_reset_.body;
    const Expr *if_node = find_ctx_if(inner, EX_CLONEABLE_SHIFT);
    bool has_if = (if_node != NULL);

    /* The context body to walk down to the shift: the whole reset body for a
     * flat context, or -- when there is an `if` branch point (at the root or
     * beneath outer frames) -- the reset body with the `if` replaced by its
     * shift-bearing arm, so the outer frames + the shift arm form one flat
     * chain. */
    const Expr *shift_body = inner;
    ClonePool pool = {0};
    if (has_if) {
        const Expr *cond = NULL, *els = NULL; bool when = true;
        const Expr *branch = ctx_if_branch(if_node, EX_CLONEABLE_SHIFT,
                                           &cond, &els, &when);
        if (!branch) return false;
        shift_body = clone_spine(inner, if_node, branch, EX_CLONEABLE_SHIFT, &pool);
    }

    ClFrame frames[CL_MAX_CTX_FRAMES];
    CtxLet lets[CL_MAX_CTX_LETS];
    uint32_t nf = 0, nl = 0;
    const Expr *shift = collect_ctx(shift_body, EX_CLONEABLE_SHIFT, frames, &nf,
                                    NULL, false, lets, &nl);
    bool ok = shift &&
              (has_if || nf != 0) &&     /* empty flat context -> legacy */
              shift->as.cloneable_shift_.n_live_captures == 0 &&
              shift->as.cloneable_shift_.k_fn != NULL;
    pool_free(&pool);
    return ok;
}

/* Emit the body expression of a context frame: either a builtin arithmetic op
 * or a 2-arg call to the frame's top-level target. The frame receives the
 * captured non-hole operand as `env` and the resumed value as `value`. */
static void cl_emit_frame_body(Buf *hb, const char *name, const ClFrame *f) {
    if (!f->call_fn) {
        buf_printf(hb,
            "static intptr_t %s(intptr_t env, intptr_t value) { return %s; }\n",
            name, frame_c_expr(f->c_op, f->hole_left));
        return;
    }
    /* Call frame: invoke target(arg0, arg1), casting env/value to the param
     * types. The hole flows in as `value`; `env` is the captured other arg. */
    char *rn = raw_name_for_binding(f->call_fn);
    if (f->call_fn->type.as.fn.arity == 0) {
        /* 0-arg call: ignore-value continuation frame -- run target() on resume
         * regardless of the resume value (a do-sequence tail). */
        buf_printf(hb,
            "static intptr_t %s(intptr_t env, intptr_t value) { (void)env; (void)value; return (intptr_t)%s(); }\n",
            name, rn);
        free(rn);
        return;
    }
    if (f->call_fn->type.as.fn.arity == 1) {
        /* 1-arg call: no env -- apply target to the resume value alone. */
        TypeKind k0 = f->call_fn->type.as.fn.arg_kinds[0];
        buf_printf(hb,
            "static intptr_t %s(intptr_t env, intptr_t value) { (void)env; return (intptr_t)%s(%svalue); }\n",
            name, rn, c_cast_for_kind(k0));
        free(rn);
        return;
    }
    TypeKind k0 = f->call_fn->type.as.fn.arg_kinds[0];
    TypeKind k1 = f->call_fn->type.as.fn.arg_kinds[1];
    const char *a0 = f->hole_left ? "value" : "env";   /* arg0 */
    const char *a1 = f->hole_left ? "env" : "value";   /* arg1 */
    buf_printf(hb,
        "static intptr_t %s(intptr_t env, intptr_t value) { return (intptr_t)%s(%s%s, %s%s); }\n",
        name, rn, c_cast_for_kind(k0), a0, c_cast_for_kind(k1), a1);
    free(rn);
}

static void cl_emit_frame_fn(Buf *hb, const char *name, const ClFrame *f) {
    cl_emit_frame_body(hb, name, f);
}

/* Emit the cloneable DK lowering for a single context body `ctx_body` (the
 * reset body, or -- under ctx_if_branch -- the shift-bearing arm), producing a
 * fresh result var holding the reset's value. The caller guarantees feasibility
 * (cl_can_lower) and file scope (pending_handler_fns). */
static char *emit_cloneable_ctx(EmitCtx *ctx, Buf *body, const Expr *e,
                                const Expr *ctx_body) {
    ClFrame frames[CL_MAX_CTX_FRAMES];
    CtxLet lets[CL_MAX_CTX_LETS];
    uint32_t nf = 0, nl = 0;
    const Expr *shift = collect_ctx(ctx_body, EX_CLONEABLE_SHIFT, frames, &nf, NULL, false, lets, &nl);
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

    /* 3. At the reset site: emit any pure prelude `let` bindings as C locals so
     *    the captured operands (which may reference them) resolve, then evaluate
     *    the non-hole operands (once, in source order) and the receiver's env,
     *    build the DK chain, and run it. */
    for (uint32_t i = 0; i < nl; i++) {
        char *iv = emit_value(ctx, body, lets[i].init);
        if (!lets[i].binding) {           /* side-effect prelude (do prefix) */
            indent_buf(body, ctx->indent);
            buf_printf(body, "(void)(%s);\n", iv);
            free(iv);
            continue;
        }
        char *bn = name_for_binding(ctx, lets[i].binding);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s = %s;\n",
                   emit_type_c_name(ctx, lets[i].binding->type), bn, iv);
        indent_buf(body, ctx->indent);
        buf_printf(body, "(void)%s;\n", bn);
        free(bn);
        free(iv);
    }
    char *op_vals[CL_MAX_CTX_FRAMES];
    for (uint32_t i = 0; i < nf; i++)
        op_vals[i] = frames[i].other ? emit_value(ctx, body, frames[i].other)
                                     : strdup("0");  /* no-env (0/1-arg) frame */
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

char *emit_cps_cloneable_reset(EmitCtx *ctx, Buf *body, const Expr *e) {
    if (!cl_can_lower(e)) return NULL;
    if (!ctx->pending_handler_fns) return NULL;   /* need file scope for helpers */

    const Expr *inner = e->as.cloneable_reset_.body;
    const Expr *if_node = find_ctx_if(inner, EX_CLONEABLE_SHIFT);
    if (!if_node)
        return emit_cloneable_ctx(ctx, body, e, inner);   /* flat context */

    /* if-shaped context: the condition is pure, so evaluate it once and branch
     * in the emitted C. The shift path runs the DK chain built from the reset
     * body with the `if` replaced by its shift-bearing arm (so any *outer*
     * frames above the `if` ride in the same chain); the other path yields the
     * reset body with the `if` replaced by its pure arm, evaluated directly. */
    const Expr *cond = NULL, *els = NULL; bool when = true;
    const Expr *branch = ctx_if_branch(if_node, EX_CLONEABLE_SHIFT,
                                       &cond, &els, &when);
    ClonePool pool = {0};
    const Expr *shift_body = clone_spine(inner, if_node, branch,
                                         EX_CLONEABLE_SHIFT, &pool);
    const Expr *pure_body  = clone_spine(inner, if_node, els,
                                         EX_CLONEABLE_SHIFT, &pool);

    const char *rty = emit_type_c_name(ctx, e->type);
    char *result = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s %s;\n", rty, result);
    char *cv = emit_value(ctx, body, cond);
    indent_buf(body, ctx->indent);
    buf_printf(body, "if (%s(%s)) {\n", when ? "" : "!", cv);
    free(cv);
    ctx->indent++;
    char *sub = emit_cloneable_ctx(ctx, body, e, shift_body);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s = %s;\n", result, sub);
    free(sub);
    ctx->indent--;
    indent_buf(body, ctx->indent);
    buf_puts(body, "} else {\n");
    ctx->indent++;
    char *ev = emit_value(ctx, body, pure_body);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s = %s;\n", result, ev);
    free(ev);
    ctx->indent--;
    indent_buf(body, ctx->indent);
    buf_puts(body, "}\n");
    pool_free(&pool);
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
static bool sk_can_lower(const Expr *e, const Expr *program) {
    if (!e || e->kind != EX_SERIAL_RESET) return false;
    if (!ty_intptr_safe(e->type.kind)) return false;
    const Expr *inner = e->as.serial_reset_.body;
    const Expr *if_node = find_ctx_if(inner, EX_SERIAL_SHIFT);

    /* See cl_can_lower: walk the shift-bearing context body, which is the whole
     * reset body for a flat context or -- with an `if` branch point at the root
     * or beneath outer frames -- the body with the `if` replaced by its
     * shift arm. */
    const Expr *shift_body = inner;
    ClonePool pool = {0};
    if (if_node) {
        const Expr *cond = NULL, *els = NULL; bool when = true;
        const Expr *branch = ctx_if_branch(if_node, EX_SERIAL_SHIFT,
                                           &cond, &els, &when);
        if (!branch) return false;
        shift_body = clone_spine(inner, if_node, branch, EX_SERIAL_SHIFT, &pool);
    }

    ClFrame frames[CL_MAX_CTX_FRAMES];
    CtxLet lets[CL_MAX_CTX_LETS];
    uint32_t nf = 0, nl = 0;
    const Expr *shift = collect_ctx(shift_body, EX_SERIAL_SHIFT,
                                    frames, &nf, program, /*want_names=*/false,
                                    lets, &nl);
    bool ok = shift && shift->as.serial_shift_.k_fn != NULL;
    pool_free(&pool);
    return ok;
}

/* The stable tag for a context frame (op + hole side), shared by the emitted
 * frame table. Mirrors the SK_TAG_* enum emitted in the prelude. */
static int sk_tag_for_frame(const ClFrame *f) {
    if (!f->c_op) return 0;                         /* call frame: not arithmetic */
    if (strcmp(f->c_op, "+") == 0) return 1;       /* SK_TAG_ADD:  env + v */
    if (strcmp(f->c_op, "*") == 0) return 2;       /* SK_TAG_MUL:  env * v */
    if (strcmp(f->c_op, "-") == 0)
        return f->hole_left ? 4                    /* SK_TAG_SUBL: v - env */
                            : 3;                   /* SK_TAG_SUBR: env - v */
    /* "/" */ return f->hole_left ? 6              /* SK_TAG_DIVL: v / env */
                                  : 5;             /* SK_TAG_DIVR: env / v */
}

/* Emit the serial DK lowering for a single context body `ctx_body` (the reset
 * body, or -- under ctx_if_branch -- the shift-bearing arm), producing a fresh
 * result var holding the reset's value. Caller guarantees feasibility. */
static char *emit_serial_ctx(EmitCtx *ctx, Buf *body, const Expr *e,
                             const Expr *ctx_body) {
    ClFrame frames[CL_MAX_CTX_FRAMES];
    CtxLet lets[CL_MAX_CTX_LETS];
    uint32_t nf = 0, nl = 0;
    const Expr *shift = collect_ctx(ctx_body, EX_SERIAL_SHIFT,
                                    frames, &nf, ctx->program_root, /*want_names=*/true,
                                    lets, &nl);
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

    /* Per-frame: an arithmetic frame uses a fixed tagged fn; a call frame gets a
     * per-site wrapper that self-registers (name -> fn) so the marshaler can map
     * the frame to a stable name and back. frame_fn[i] is the C expression naming
     * the DKFrame to install. */
    /* Emit any pure prelude `let` bindings as C locals first, so captured
     * operands that reference them resolve (mirrors the cloneable path). A
     * binding-less entry is a do-prefix statement, emitted for side effect. */
    for (uint32_t i = 0; i < nl; i++) {
        char *iv = emit_value(ctx, body, lets[i].init);
        if (!lets[i].binding) {           /* side-effect prelude (do prefix) */
            indent_buf(body, ctx->indent);
            buf_printf(body, "(void)(%s);\n", iv);
            free(iv);
            continue;
        }
        char *bn = name_for_binding(ctx, lets[i].binding);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s = %s;\n",
                   emit_type_c_name(ctx, lets[i].binding->type), bn, iv);
        indent_buf(body, ctx->indent);
        buf_printf(body, "(void)%s;\n", bn);
        free(bn);
        free(iv);
    }
    char *op_vals[CL_MAX_CTX_FRAMES];
    char  frame_fn[CL_MAX_CTX_FRAMES][48];
    for (uint32_t i = 0; i < nf; i++) {
        op_vals[i] = frames[i].other ? emit_value(ctx, body, frames[i].other)
                                     : strdup("0");  /* no-env (1-arg) frame */
        if (!frames[i].call_fn) {
            snprintf(frame_fn[i], sizeof frame_fn[i],
                     "__sk_frame_for_tag(%d)", sk_tag_for_frame(&frames[i]));
        } else {
            /* Emit the wrapper fn + a registry entry keyed by target name + side. */
            snprintf(frame_fn[i], sizeof frame_fn[i], "__sk_call_%d_%u", id, i);
            cl_emit_frame_body(hb, frame_fn[i], &frames[i]);
            char *rn = raw_name_for_binding(frames[i].call_fn);
            /* env_kind code + Serializable instance fn pointers (SER envs only). */
            int ekc = frames[i].env_ser ? 2                       /* SK_ENV_SER  */
                    : (frames[i].env_kind == TY_CSTR) ? 1         /* SK_ENV_CSTR */
                    : 0;                                          /* SK_ENV_INT  */
            if (frames[i].env_ser) {
                buf_printf(hb,
                    "static SkReg __sk_reg_%d_%u = { \"%s%s\", %s, %d,"
                    " (void *(*)(int64_t))%s, (int64_t (*)(void *))%s, 0 };\n"
                    "__attribute__((constructor)) static void __sk_reginit_%d_%u(void) "
                    "{ __sk_register(&__sk_reg_%d_%u); }\n",
                    id, i, rn, frames[i].hole_left ? "$L" : "$R", frame_fn[i], ekc,
                    frames[i].env_ser, frames[i].env_deser,
                    id, i, id, i);
            } else {
                buf_printf(hb,
                    "static SkReg __sk_reg_%d_%u = { \"%s%s\", %s, %d, 0, 0, 0 };\n"
                    "__attribute__((constructor)) static void __sk_reginit_%d_%u(void) "
                    "{ __sk_register(&__sk_reg_%d_%u); }\n",
                    id, i, rn, frames[i].hole_left ? "$L" : "$R", frame_fn[i], ekc,
                    id, i, id, i);
            }
            free(rn);
            free((void *)frames[i].env_ser);
            free((void *)frames[i].env_deser);
        }
    }
    char *k_fn_val = emit_value(ctx, body, k_fn);

    char *chain = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body, "DK *%s = dk_prompt(1, dk_done());\n", chain);
    for (uint32_t i = 0; i < nf; i++) {       /* outermost-first -> innermost at front */
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s = dk_frame(%s, (intptr_t)(%s), %s);\n",
                   chain, frame_fn[i], op_vals[i], chain);
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

char *emit_cps_serial_reset(EmitCtx *ctx, Buf *body, const Expr *e) {
    if (!sk_can_lower(e, ctx->program_root)) return NULL;
    if (!ctx->pending_handler_fns) return NULL;

    const Expr *inner = e->as.serial_reset_.body;
    const Expr *if_node = find_ctx_if(inner, EX_SERIAL_SHIFT);
    if (!if_node)
        return emit_serial_ctx(ctx, body, e, inner);   /* flat context */

    /* if-shaped context: branch in the emitted C on the pure condition. The
     * shift path captures + marshals the chain built from the reset body with
     * the `if` replaced by its shift arm (outer frames above the `if` included);
     * the other path yields the body with the `if` replaced by its pure arm. */
    const Expr *cond = NULL, *els = NULL; bool when = true;
    const Expr *branch = ctx_if_branch(if_node, EX_SERIAL_SHIFT,
                                       &cond, &els, &when);
    ClonePool pool = {0};
    const Expr *shift_body = clone_spine(inner, if_node, branch,
                                         EX_SERIAL_SHIFT, &pool);
    const Expr *pure_body  = clone_spine(inner, if_node, els,
                                         EX_SERIAL_SHIFT, &pool);

    const char *rty = emit_type_c_name(ctx, e->type);
    char *result = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s %s;\n", rty, result);
    char *cv = emit_value(ctx, body, cond);
    indent_buf(body, ctx->indent);
    buf_printf(body, "if (%s(%s)) {\n", when ? "" : "!", cv);
    free(cv);
    ctx->indent++;
    char *sub = emit_serial_ctx(ctx, body, e, shift_body);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s = %s;\n", result, sub);
    free(sub);
    ctx->indent--;
    indent_buf(body, ctx->indent);
    buf_puts(body, "} else {\n");
    ctx->indent++;
    char *ev = emit_value(ctx, body, pure_body);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s = %s;\n", result, ev);
    free(ev);
    ctx->indent--;
    indent_buf(body, ctx->indent);
    buf_puts(body, "}\n");
    pool_free(&pool);
    return result;
}

/* Program scan: does any serial-reset lower onto the DK machine? Gates the DK
 * runtime prelude + the serial marshaling runtime. Mirrors sk_can_lower. */
static const Expr *g_sk_scan_root = NULL;  /* whole program, for the instance scan */
/* When true, the walk reports mere *presence* of serial syntax (any
 * serial-shift), not just lowerable resets. The serial runtime prelude
 * (tur_serial_cont_*) must be emitted whenever stdlib save-cont!/resume-cont!
 * could reference it -- i.e. whenever a serial-shift exists -- so an
 * unsupported context degrades cleanly instead of emitting an implicit
 * declaration that miscompiles. See
 * docs/reported/serial-shift-unsupported-context-miscompile.md. */
static bool g_sk_any_serial = false;

static bool uses_serial_dk(const Expr *e) {
    if (!e) return false;
    if (e->kind == EX_SERIAL_SHIFT && g_sk_any_serial) return true;
    if (e->kind == EX_SERIAL_RESET &&
        (g_sk_any_serial || sk_can_lower(e, g_sk_scan_root))) return true;
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
    g_sk_scan_root = program;   /* so sk_can_lower can scan for Serializable instances */
    bool r = uses_serial_dk(program);
    g_sk_scan_root = NULL;
    return r;
}

/* True if the program contains any serial-shift/serial-reset, lowerable or not.
 * Gates emission of the DK machine + serial marshaling runtime so the stdlib
 * save-cont!/resume-cont! references never dangle. */
bool emit_cps_program_contains_serial(const Expr *program) {
    g_sk_scan_root = program;
    g_sk_any_serial = true;
    bool r = uses_serial_dk(program);
    g_sk_any_serial = false;
    g_sk_scan_root = NULL;
    return r;
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
"enum { SK_TAG_ADD = 1, SK_TAG_MUL = 2, SK_TAG_SUBR = 3, SK_TAG_SUBL = 4,\n"
"       SK_TAG_DIVR = 5, SK_TAG_DIVL = 6 };\n"
"static intptr_t __sk_add (intptr_t env, intptr_t v) { return env + v; }\n"
"static intptr_t __sk_mul (intptr_t env, intptr_t v) { return env * v; }\n"
"static intptr_t __sk_subr(intptr_t env, intptr_t v) { return env - v; } /* other - hole */\n"
"static intptr_t __sk_subl(intptr_t env, intptr_t v) { return v - env; } /* hole - other */\n"
"static intptr_t __sk_divr(intptr_t env, intptr_t v) {  /* other / hole */\n"
"    return (v) ? (env / v) : (fprintf(stderr, \"division by zero\\n\"), abort(), 0);\n"
"}\n"
"static intptr_t __sk_divl(intptr_t env, intptr_t v) {  /* hole / other */\n"
"    return (env) ? (v / env) : (fprintf(stderr, \"division by zero\\n\"), abort(), 0);\n"
"}\n"
"static DKFrame __sk_frame_for_tag(int tag) {\n"
"    switch (tag) {\n"
"        case SK_TAG_ADD:  return __sk_add;\n"
"        case SK_TAG_MUL:  return __sk_mul;\n"
"        case SK_TAG_SUBR: return __sk_subr;\n"
"        case SK_TAG_SUBL: return __sk_subl;\n"
"        case SK_TAG_DIVR: return __sk_divr;\n"
"        case SK_TAG_DIVL: return __sk_divl;\n"
"        default: return NULL;\n"
"    }\n"
"}\n"
"static int __sk_tag_for_frame(DKFrame f) {\n"
"    if (f == __sk_add)  return SK_TAG_ADD;\n"
"    if (f == __sk_mul)  return SK_TAG_MUL;\n"
"    if (f == __sk_subr) return SK_TAG_SUBR;\n"
"    if (f == __sk_subl) return SK_TAG_SUBL;\n"
"    if (f == __sk_divr) return SK_TAG_DIVR;\n"
"    if (f == __sk_divl) return SK_TAG_DIVL;\n"
"    return 0;  /* not an arithmetic frame -- a call frame (see the registry) */\n"
"}\n"
"/* Call-frame registry: each per-site call frame self-registers (via a C\n"
" * constructor) a stable name <-> DKFrame mapping, so a call frame marshals as\n"
" * its name rather than a code address -- the same name-keyed scheme the\n"
" * interpreter's serial.c uses, here with the emitted C function name. */\n"
"/* env_kind: 0 = int (inline int64), 1 = cstr (length-prefixed bytes),\n"
" * 2 = serializable (marshaled via the env type's Serializable instance). For a\n"
" * serializable env, ser/deser point at __inst_Serializable_{serialize,\n"
" * deserialize}_<T>: ser(env) -> a `bytes` {int64 len; data} buffer; deser(bytes)\n"
" * -> the env value. */\n"
"typedef struct SkReg {\n"
"    const char *name; DKFrame fn; int env_kind;\n"
"    void *(*ser)(int64_t); int64_t (*deser)(void *);\n"
"    struct SkReg *next;\n"
"} SkReg;\n"
"static SkReg *__sk_registry = NULL;\n"
"static void __sk_register(SkReg *r) { r->next = __sk_registry; __sk_registry = r; }\n"
"static DKFrame __sk_call_for_name(const char *name) {\n"
"    for (SkReg *r = __sk_registry; r; r = r->next)\n"
"        if (strcmp(r->name, name) == 0) return r->fn;\n"
"    return NULL;\n"
"}\n"
"static SkReg *__sk_reg_by_name(const char *name) {\n"
"    for (SkReg *r = __sk_registry; r; r = r->next)\n"
"        if (strcmp(r->name, name) == 0) return r;\n"
"    return NULL;\n"
"}\n"
"static SkReg *__sk_reg_for_frame(DKFrame f) {\n"
"    for (SkReg *r = __sk_registry; r; r = r->next)\n"
"        if (r->fn == f) return r;\n"
"    return NULL;\n"
"}\n"
"#define SK_TAG_CALL 100\n"
"#define SK_ENV_INT  0\n"
"#define SK_ENV_CSTR 1\n"
"#define SK_ENV_SER  2\n");
    buf_puts(out,
"/* Resume a (possibly deserialized) serial continuation on a value. */\n"
"static int64_t tur_serial_cont_resume(int64_t k, int64_t v) {\n"
"    return (int64_t)dk_invoke((DK *)(intptr_t)k, (intptr_t)v);\n"
"}\n"
"/* Marshal to a length-prefixed buffer ([int64 payload_len][int64 n][records]).\n"
" * Each record is self-describing. An arithmetic frame is [tag(1..6)][int64 env].\n"
" * A call frame is [SK_TAG_CALL][name_len][name][env_kind][env]: an int env is an\n"
" * inline int64; a cstr env is [int64 len][bytes] -- so a non-int (cstr) env is\n"
" * marshaled by value, not as a code/heap address. All scalars are memcpy'd. */\n"
"static int64_t tur_serial_cont_serialize(int64_t k) {\n"
"    DK *p = (DK *)(intptr_t)k;\n"
"    int64_t n = 0, sz = 8;  /* 8 bytes for the frame count */\n"
"    for (DK *q = p; q && q->kind == DKK_FRAME; q = q->next) {\n"
"        n++;\n"
"        if (__sk_tag_for_frame(q->fn)) { sz += 16; continue; }\n"
"        SkReg *r = __sk_reg_for_frame(q->fn);\n"
"        const char *nm = r ? r->name : \"\";\n"
"        int64_t L = (int64_t)strlen(nm);\n"
"        sz += 8 + 8 + L + 8;  /* tag + name_len + name + env_kind */\n"
"        if (r && r->env_kind == SK_ENV_CSTR) {\n"
"            const char *s = (const char *)(intptr_t)q->env;\n"
"            sz += 8 + (int64_t)(s ? strlen(s) : 0);  /* str_len + bytes */\n"
"        } else if (r && r->env_kind == SK_ENV_SER && r->ser) {\n"
"            void *b = r->ser((int64_t)q->env);       /* instance bytes {len;data} */\n"
"            int64_t bl = b ? ((int64_t *)b)[0] : 0;\n"
"            sz += 8 + bl; free(b);                    /* len + data */\n"
"        } else { sz += 8; }                            /* inline int64 */\n"
"    }\n"
"    uint8_t *buf = (uint8_t *)malloc((size_t)(8 + sz));\n"
"    if (!buf) abort();\n"
"    memcpy(buf, &sz, 8);\n"
"    uint8_t *c = buf + 8;\n"
"    memcpy(c, &n, 8); c += 8;\n"
"    for (DK *q = p; q && q->kind == DKK_FRAME; q = q->next) {\n"
"        int t = __sk_tag_for_frame(q->fn);\n"
"        if (t) {\n"
"            int64_t tag = t, env = (int64_t)q->env;\n"
"            memcpy(c, &tag, 8); c += 8; memcpy(c, &env, 8); c += 8;\n"
"            continue;\n"
"        }\n"
"        SkReg *r = __sk_reg_for_frame(q->fn);\n"
"        const char *nm = r ? r->name : \"\";\n"
"        int64_t tag = SK_TAG_CALL, L = (int64_t)strlen(nm);\n"
"        int64_t ek = r ? r->env_kind : SK_ENV_INT;\n"
"        memcpy(c, &tag, 8); c += 8;\n"
"        memcpy(c, &L, 8); c += 8;\n"
"        if (L) { memcpy(c, nm, (size_t)L); c += L; }\n"
"        memcpy(c, &ek, 8); c += 8;\n"
"        if (ek == SK_ENV_CSTR) {\n"
"            const char *s = (const char *)(intptr_t)q->env;\n"
"            int64_t sl = (int64_t)(s ? strlen(s) : 0);\n"
"            memcpy(c, &sl, 8); c += 8;\n"
"            if (sl) { memcpy(c, s, (size_t)sl); c += sl; }\n"
"        } else if (ek == SK_ENV_SER && r && r->ser) {\n"
"            void *b = r->ser((int64_t)q->env);\n"
"            int64_t bl = b ? ((int64_t *)b)[0] : 0;\n"
"            memcpy(c, &bl, 8); c += 8;\n"
"            if (bl) { memcpy(c, (int64_t *)b + 1, (size_t)bl); c += bl; }\n"
"            free(b);\n"
"        } else {\n"
"            int64_t env = (int64_t)q->env;\n"
"            memcpy(c, &env, 8); c += 8;\n"
"        }\n"
"    }\n"
"    return (int64_t)(intptr_t)buf;\n"
"}\n");
    buf_puts(out,
"/* Rebuild a runnable chain [frames..., prompt, done] from a marshaled buffer.\n"
" * Records are parsed forward, then prepended in reverse so the first-written\n"
" * frame stays at the front of the chain. A cstr env is rematerialized as a\n"
" * fresh heap string. */\n"
"static int64_t tur_serial_cont_deserialize(int64_t bytes) {\n"
"    uint8_t *c = (uint8_t *)(intptr_t)bytes + 8;  /* skip the length prefix */\n"
"    int64_t n; memcpy(&n, c, 8); c += 8;\n"
"    DKFrame *fns = (DKFrame *)malloc(sizeof(DKFrame) * (size_t)(n > 0 ? n : 1));\n"
"    intptr_t *envs = (intptr_t *)malloc(sizeof(intptr_t) * (size_t)(n > 0 ? n : 1));\n"
"    if (!fns || !envs) abort();\n"
"    for (int64_t i = 0; i < n; i++) {\n"
"        int64_t tag; memcpy(&tag, c, 8); c += 8;\n"
"        if (tag == SK_TAG_CALL) {\n"
"            int64_t L; memcpy(&L, c, 8); c += 8;\n"
"            char *nm = (char *)malloc((size_t)L + 1);\n"
"            if (!nm) abort();\n"
"            if (L) memcpy(nm, c, (size_t)L);\n"
"            nm[L] = 0; c += L;\n"
"            fns[i] = __sk_call_for_name(nm);\n"
"            SkReg *reg = __sk_reg_by_name(nm); free(nm);\n"
"            int64_t ek; memcpy(&ek, c, 8); c += 8;\n"
"            if (ek == SK_ENV_CSTR) {\n"
"                int64_t sl; memcpy(&sl, c, 8); c += 8;\n"
"                char *s = (char *)malloc((size_t)sl + 1);\n"
"                if (!s) abort();\n"
"                if (sl) memcpy(s, c, (size_t)sl);\n"
"                s[sl] = 0; c += sl;\n"
"                envs[i] = (intptr_t)s;\n"
"            } else if (ek == SK_ENV_SER) {\n"
"                int64_t bl; memcpy(&bl, c, 8); c += 8;\n"
"                int64_t *bb = (int64_t *)malloc(sizeof(int64_t) + (size_t)bl);\n"
"                if (!bb) abort();\n"
"                bb[0] = bl;\n"
"                if (bl) memcpy(bb + 1, c, (size_t)bl);\n"
"                c += bl;\n"
"                envs[i] = (reg && reg->deser) ? (intptr_t)reg->deser(bb) : 0;\n"
"                free(bb);\n"
"            } else {\n"
"                int64_t env; memcpy(&env, c, 8); c += 8;\n"
"                envs[i] = (intptr_t)env;\n"
"            }\n"
"        } else {\n"
"            fns[i] = __sk_frame_for_tag((int)tag);\n"
"            int64_t env; memcpy(&env, c, 8); c += 8;\n"
"            envs[i] = (intptr_t)env;\n"
"        }\n"
"    }\n"
"    DK *chain = dk_prompt(1, dk_done());\n"
"    for (int64_t i = n - 1; i >= 0; i--) chain = dk_frame(fns[i], envs[i], chain);\n"
"    free(fns); free(envs);\n"
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
