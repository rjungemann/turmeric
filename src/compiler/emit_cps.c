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

#include "emit_internal.h"
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
