/* emit_fns.c -- function-definition C emission (emit_fn_def). */
#include "emit_internal.h"
#include "globals.h"   /* M7: g_m7_hkt_enabled (flag-gated by-value HKT dispatch) */

/* ============================================================================
 * CF1: Self-tail-call optimization (self-TCO).
 *
 * A self-recursive call in tail position is lowered to a backedge: the argument
 * values are evaluated into temporaries, the C parameter variables are
 * reassigned, and control jumps to a `__tur_tailcall:` label at the top of the
 * function body -- turning `(let loop [...] ... (loop ...))` and equivalent
 * self-recursive `defn`s into an iterative loop that does not grow the C stack.
 *
 * Scope (1.0): self-tail-calls only, reached through `if`, `do`, and `let`/
 * `letrec` (which is where the named-let idiom and `cond`/`when` -- macro-
 * expanded to `if` -- put them).  Mutual/general tail calls and tail calls
 * inside `match` arms remain ordinary recursive calls; they are correct, just
 * not stack-optimized.  See docs/guides/generators-guide.md and
 * docs/control-flow-completeness-plan.md (Phase CF1).
 * ========================================================================== */

/* The declared C type of parameter `i`, matching the function signature's
 * default (non-pbp, non-poly) path.
 *
 * M4 follow-up (docs/upcoming/tco-in-abi-specs-for-stdlib-iteration.md):
 * when emitting an ABI spec body, the spec's per-instantiation arg type
 * is the authoritative C-level shape — the bare fn binding's full type is
 * the generic (pre-substitution) form which lowers to int64_t for TY_APP.
 * Consult `current_abi_specialization->arg_types[i]` first when set; this
 * lets `tco_params_simple` accept by-value-struct params (e.g.
 * `Vec__int`) that the generic form would reject as carrier-ABI. */
static Type tco_param_type(EmitCtx *ctx, const Expr *fn_e, FnDef *fd, uint8_t i) {
    if (ctx && ctx->current_abi_specialization
        && i < ctx->current_abi_specialization->n_args) {
        return ctx->current_abi_specialization->arg_types[i];
    }
    if (fn_e->type.kind == TY_FN && fn_e->type.as.fn.arg_full_types &&
        fn_e->type.as.fn.arg_full_types[i])
        return *fn_e->type.as.fn.arg_full_types[i];
    return fd->param_types[i];
}

/* A function is TCO-eligible only if every parameter is a plain scalar we can
 * reassign with `p = tmp;`.  Pass-by-pointer structs, poly-fn, fn-typed, and
 * carrier-ABI parameters are excluded so the backedge temporary's C type is
 * unambiguous.
 *
 * M4 follow-up: a param that resolves to a concrete by-value struct (post-
 * Path A spec substitution: e.g. `Vec__int` from `(Vec int)` at a spec
 * call site) is OK to reassign — `Vec__int new = ...; x = new;` is a valid
 * C struct copy.  The carrier-ABI rejection now checks the EMIT C name
 * rather than the type-kind: only reject when the emitted name is
 * "int64_t" (the carrier fallback), which indicates the type would need
 * a bridge to/from the carrier on reassignment. */
static bool tco_params_simple(EmitCtx *ctx, const Expr *fn_e, FnDef *fd) {
    if (fd->is_variadic || fd->closure) return false;
    for (uint8_t i = 0; i < fd->n_params; i++) {
        if (fd->params[i]->is_poly_fn) return false;
        Type pty = tco_param_type(ctx, fn_e, fd, i);
        if (pty.kind == TY_FN) return false;
        Type rpty = emit_resolve_type(ctx, pty);
        if (type_struct_pass_by_ptr(rpty)) return false;
        if (type_uses_carrier_abi(rpty)) {
            /* M4 follow-up: a TY_APP/TY_STRUCT-with-tparams that emits as
             * a concrete struct name is by-value at this call site (Path A
             * spec) — TCO is safe.  Only reject when the C emit collapses
             * to the int64 carrier. */
            const char *c = emit_type_c_name(ctx, rpty);
            if (!c || strcmp(c, "int64_t") == 0) return false;
        }
    }
    return true;
}

/* True if `call` is a direct, arity-matching self-call of the function being
 * emitted (whose C name is `fn_cname`).  Identity is by resolved C name, not
 * Binding pointer: named-let desugars `(let loop ...)` to a `letrec` whose loop
 * binding (the call's target) is a *different* Binding object than the
 * anonymous `fn`'s own FnDef binding, yet both mangle to the same C function
 * name (e.g. __fn_691). */
static bool tco_is_self_call(FnDef *fd, const char *fn_cname, const Expr *call) {
    if (!call || call->kind != EX_CALL) return false;
    if (!call->as.call_.fn_binding) return false;     /* must be a direct call */
    if (call->as.call_.fn_expr) return false;          /* indirect field call */
    /* M4 follow-up (docs/upcoming/tco-in-abi-specs-for-stdlib-iteration.md):
     * Path A's elab resolves typeclass dispatch directly to the instance
     * method's binding (`fn_binding = method->binding; fn_expr = NULL`).
     * `dict_arg` is still set as an annotation, but the actual call IS a
     * direct call — the fn_binding identity check below already handles
     * recursive Path A specs correctly.  The original `dict_arg` reject
     * existed for pre-Path-A indirect dispatch through the dict slot
     * cast, which `fn_expr != NULL` already filters out. */
    if (call->as.call_.is_poly_call) return false;     /* rank-2 poly call */
    if (call->as.call_.n_args != fd->n_params) return false;
    if (call->as.call_.fn_binding == fd->binding) return true;  /* fast path */
    char *cn = raw_name_for_binding(call->as.call_.fn_binding);
    bool same = fn_cname && cn && strcmp(cn, fn_cname) == 0;
    free(cn);
    return same;
}

/* A let/letrec is tail-transparent for TCO only if every binding is a plain
 * scalar we can declare with `T name = init;`.  fn-typed (incl. letrec global
 * fns), poly-fn, and carrier-ABI bindings force the whole let onto the default
 * (emit_value + return) path. */
static bool tco_let_simple(EmitCtx *ctx, const Expr *e) {
    for (uint32_t i = 0; i < e->as.let_.n; i++) {
        const Binding *b = e->as.let_.bindings[i].binding;
        if (!b) return false;
        if (b->type.kind == TY_FN || b->is_poly_fn) return false;
        if (type_uses_carrier_abi(emit_resolve_type(ctx, b->type))) return false;
    }
    return true;
}

/* Mark self-tail-calls in tail position, recursing through if/do/let/letrec.
 * This mirrors emit_tail's structural recursion exactly so that "marked >= 1"
 * predicts whether emit_tail will emit a backedge (and thus whether the
 * `__tur_tailcall:` label is used).  Returns the number of calls marked. */
static int tco_mark(EmitCtx *ctx, FnDef *fd, const char *fn_cname, Expr *e) {
    if (!e) return 0;
    switch (e->kind) {
        case EX_CALL:
            if (tco_is_self_call(fd, fn_cname, e)) {
                e->as.call_.is_tail_self_call = true;
                return 1;
            }
            return 0;
        case EX_IF: {
            if (!e->as.if_.else_or_null) return 0;  /* default path; no recursion */
            int n = tco_mark(ctx, fd, fn_cname, e->as.if_.then_);
            n += tco_mark(ctx, fd, fn_cname, e->as.if_.else_or_null);
            return n;
        }
        case EX_DO: {
            if (e->as.do_.n == 0) return 0;
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (e->as.do_.items[i]->kind == EX_DEFER) return 0; /* defers break tail */
            return tco_mark(ctx, fd, fn_cname, e->as.do_.items[e->as.do_.n - 1]);
        }
        case EX_LET:
        case EX_LETREC:
            if (!tco_let_simple(ctx, e)) return 0;
            return tco_mark(ctx, fd, fn_cname, e->as.let_.body);
        default:
            return 0;
    }
}

/* Forward decl: tail-position emitter (mutually recursive). */
static void emit_tail(EmitCtx *ctx, Buf *body, const Expr *fn_e, FnDef *fd,
                      const Expr *e, TypeKind result_kind, bool is_main);

/* Emit a self-tail-call as a backedge: evaluate all args into temporaries
 * first (so argument expressions still see the *old* parameter values, which
 * matters for reorderings/swaps), reassign the C parameter variables, then
 * `goto __tur_tailcall;`. */
static void emit_tail_backedge(EmitCtx *ctx, Buf *body, const Expr *fn_e,
                               FnDef *fd, const Expr *call) {
    uint8_t n = fd->n_params;
    char **tmps = n ? (char **)calloc(n, sizeof(char *)) : NULL;
    for (uint8_t i = 0; i < n; i++) {
        char *av = emit_value(ctx, body, call->as.call_.args[i]);
        char *t = fresh_tmp(ctx);
        Type pty = tco_param_type(ctx, fn_e, fd, i);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s = %s;\n", emit_type_c_name(ctx, pty), t, av);
        tmps[i] = t;
        free(av);
    }
    for (uint8_t i = 0; i < n; i++) {
        char *pn = raw_name_for_binding(fd->params[i]);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s = %s;\n", pn, tmps[i]);
        free(pn);
        free(tmps[i]);
    }
    free(tmps);
    indent_buf(body, ctx->indent);
    buf_puts(body, "goto __tur_tailcall;\n");
}

/* A#1 (return position): emit a tail/return value, wrapping it in EX_FN_TO_FAT
 * when the enclosing function carries the ^fat result marker and the value is a
 * bare non-capturing fn (TY_FN).  This makes the returned value a heap fat
 * closure { thunk, env } so a fat-call consumer reads a valid layout instead of
 * a bare function pointer -- symmetric with the ^fat parameter auto-shim.  A
 * capturing closure (already TY_PTR_VOID) or any non-fn value passes through. */
static char *emit_fat_return_value(EmitCtx *ctx, Buf *body, const Expr *fn_e,
                                   const Expr *e) {
    if (fn_e && fn_e->type.kind == TY_FN && fn_e->type.as.fn.result_fat &&
        e->type.kind == TY_FN) {
        Expr shim = {0};
        shim.kind = EX_FN_TO_FAT;
        shim.type = e->type;
        shim.span = e->span;
        shim.as.fn_to_fat_.inner = (Expr *)e;
        return emit_value(ctx, body, &shim);
    }
    return emit_value(ctx, body, e);
}

/* M5 straddle (root cause C of m5-suite-residual-6-failures): true when every
 * tail leaf of `e` is a call that emits an int64 carrier value -- a
 * #{Construct} helper (some/ok/err/none) or a typeclass-method impl
 * (__inst_*), both of which return the carrier handle regardless of their
 * declared (Option A)/(Result A B) type.  Used to decide whether a by-value
 * carrier-aggregate return needs a carrier->concrete deref bridge.  Shared
 * with emit_module.c's forward-decl mirror via emit_internal.h. */
bool fn_body_tail_is_carrier_producer(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_ASCRIBE:
            return fn_body_tail_is_carrier_producer(e->as.ascribe_.inner);
        case EX_DO:
            return e->as.do_.n > 0 &&
                   fn_body_tail_is_carrier_producer(e->as.do_.items[e->as.do_.n - 1]);
        case EX_IF:
            return e->as.if_.else_or_null &&
                   fn_body_tail_is_carrier_producer(e->as.if_.then_) &&
                   fn_body_tail_is_carrier_producer(e->as.if_.else_or_null);
        case EX_LET:
        case EX_LETREC:
            return fn_body_tail_is_carrier_producer(e->as.let_.body);
        case EX_CALL: {
            const Binding *b = e->as.call_.fn_binding;
            if (!b) return false;
            if (b->is_construct_template) return true;
            if (b->name && b->name->name &&
                strncmp(b->name->name, "__inst_", 7) == 0) return true;
            /* result-bridge-tail-call-from-pure-tur-to-inline-c: an inline-C
             * body whose declared return type uses the carrier ABI is lowered
             * with an int64_t C return type, so a tail call to it yields the
             * carrier handle.  A pure-Turmeric wrapper around such a helper
             * needs the same carrier->by-value bridge that the #{Construct}
             * and __inst_ producers above already get.
             * See docs/archive/tail-call-inline-c-carrier-bridge.md. */
            if (b->body_is_inline_c && b->type.kind == TY_FN &&
                b->type.as.fn.result_full_type &&
                type_uses_carrier_abi(*b->type.as.fn.result_full_type))
                return true;
            /* CONV-S1 seam 4 (return-bridge): under the defstruct-as-defadt
             * lowering a parametric ADT app (`(Result cstr cstr)`) is a BY-VALUE
             * type, so `type_uses_carrier_abi` reports false -- yet an inline-C
             * body is still EMITTED with an int64 carrier return (emit_fns.c's
             * signature path lowers an inline-C TY_APP result to int64_t, since it
             * is neither a typed pointer nor a TY_STRUCT).  A pure-Turmeric wrapper
             * whose own return is the by-value aggregate (`capture` -> the int64
             * `captures-get`) therefore still needs the carrier->by-value bridge.
             * Recognise a non-heap TY_APP inline-C result as a carrier producer. */
            if (b->body_is_inline_c && b->type.kind == TY_FN &&
                b->type.as.fn.result_full_type &&
                b->type.as.fn.result_full_type->kind == TY_APP &&
                !type_is_heap_struct(*b->type.as.fn.result_full_type) &&
                !type_is_heap_adt(*b->type.as.fn.result_full_type))
                return true;
            return false;
        }
        default:
            return false;
    }
}

/* CONV-S1 seam 4 (return-bridge): true when the body tail is a direct call to an
 * INLINE-C function whose by-value ADT-app result is nonetheless EMITTED as the
 * int64 carrier.  Under the defstruct-as-defadt lowering a parametric ADT app
 * (`(Result cstr cstr)`) is a by-value type, so `type_uses_carrier_abi` reports
 * false -- but emit_fns.c still lowers an inline-C body with a TY_APP result to an
 * int64_t C return, so a pure-Turmeric wrapper whose own return is the by-value
 * aggregate returns that int64 into the aggregate slot.  This narrowly widens the
 * M5 straddle gate (whose `type_uses_carrier_abi(body type)` test misses the
 * lowered case) to exactly that shape -- NOT to an if/construct merge tail (which
 * the (a) fix already lowers by value, so unboxing it would be wrong).  At default
 * (no lowering) the inline-C result is the carrier rep and the M5 gate already
 * handles it, so this stays inert. */
static bool fn_body_tail_returns_carrier_value(EmitCtx *ctx, const Expr *e) {
    (void)ctx;
    if (!e) return false;
    switch (e->kind) {
        case EX_ASCRIBE:
            return fn_body_tail_returns_carrier_value(ctx, e->as.ascribe_.inner);
        /* NOTE: do NOT descend into EX_DO/EX_LET/EX_LETREC here.  Under the
         * defstruct-as-defadt lowering those control forms now bridge their own
         * tail value into a by-value merge temp (emit_expr.c's
         * bridge_control_value_to_byvalue_temp), so the merge result is already
         * the by-value aggregate and `return <temp>` needs no further unbox.
         * Descending would re-fire the M5 carrier->concrete deref on a value that
         * is no longer the carrier (the assignment-straddle double-bridge). */
        case EX_CALL: {
            const Binding *b = e->as.call_.fn_binding;
            if (!b || b->type.kind != TY_FN || !b->type.as.fn.result_full_type)
                return false;
            Type rft = *b->type.as.fn.result_full_type;
            return b->body_is_inline_c && rft.kind == TY_APP
                && !type_uses_carrier_abi(rft)
                && !type_is_heap_struct(rft) && !type_is_heap_adt(rft);
        }
        default:
            return false;
    }
}

/* Emit `e` in tail position: every path ends in `return <v>;` or a backedge
 * `goto __tur_tailcall;`.  Only invoked for functions tco_mark flagged. */
static void emit_tail(EmitCtx *ctx, Buf *body, const Expr *fn_e, FnDef *fd,
                      const Expr *e, TypeKind result_kind, bool is_main) {
    switch (e->kind) {
        case EX_CALL:
            if (e->as.call_.is_tail_self_call) {
                emit_tail_backedge(ctx, body, fn_e, fd, e);
                return;
            }
            break;  /* non-self call -> default return path */
        case EX_IF:
            if (e->as.if_.else_or_null) {
                char *cond = emit_value(ctx, body, e->as.if_.cond);
                indent_buf(body, ctx->indent);
                buf_printf(body, "if (%s) {\n", cond);
                free(cond);
                ctx->indent += 4;
                emit_tail(ctx, body, fn_e, fd, e->as.if_.then_, result_kind, is_main);
                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "} else {\n");
                ctx->indent += 4;
                emit_tail(ctx, body, fn_e, fd, e->as.if_.else_or_null, result_kind, is_main);
                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");
                return;
            }
            break;
        case EX_DO: {
            bool has_defer = false;
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (e->as.do_.items[i]->kind == EX_DEFER) { has_defer = true; break; }
            if (!has_defer && e->as.do_.n > 0) {
                for (uint32_t i = 0; i + 1 < e->as.do_.n; i++)
                    emit_stmt(ctx, body, e->as.do_.items[i]);
                emit_tail(ctx, body, fn_e, fd, e->as.do_.items[e->as.do_.n - 1],
                          result_kind, is_main);
                return;
            }
            break;
        }
        case EX_LET:
        case EX_LETREC:
            if (tco_let_simple(ctx, e)) {
                indent_buf(body, ctx->indent);
                buf_puts(body, "{\n");
                ctx->indent += 4;
                for (uint32_t i = 0; i < e->as.let_.n; i++) {
                    const Binding *b = e->as.let_.bindings[i].binding;
                    char *bn = name_for_binding(ctx, b);
                    char *iv = emit_value(ctx, body, e->as.let_.bindings[i].init);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s %s = %s;\n", emit_type_c_name(ctx, b->type), bn, iv);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "(void)%s;\n", bn);
                    free(bn);
                    free(iv);
                }
                emit_tail(ctx, body, fn_e, fd, e->as.let_.body, result_kind, is_main);
                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");
                return;
            }
            break;
        default:
            break;
    }

    /* Default: emit as a value and return it. */
    char *v = emit_fat_return_value(ctx, body, fn_e, e);
    if (e->type.kind == TY_NIL) {
        free(v);
        v = strdup(result_kind == TY_BOOL ? "false" : "0");
    }
    /* Phase 5 carrier-bridge deletion (concrete->carrier tail return): this
     * tail is emitted in a function/closure whose C return is the uniform int64
     * carrier but the tail value is now a by-value Option/Result struct (a
     * monomorphized #{Construct} spec).  Heap-spill it back to the int64 carrier
     * (a stack spill would return a dangling address).  Guard fires only for an
     * actual by-value carrier producer, so plain int tails and not-yet-
     * monomorphized carrier producers are untouched. */
    Type tail_bv = (!is_main && result_kind == TY_INT && e->type.kind != TY_NIL &&
                    e->type.kind != TY_NEVER &&
                    fn_body_tail_emits_byvalue_carrier_abi(ctx, e))
        ? fn_body_tail_byvalue_carrier_type(ctx, e)
        : type_simple(TY_UNKNOWN, CK_COPY);
    if (tail_bv.kind != TY_UNKNOWN) {
        indent_buf(body, ctx->indent);
        if (type_is_heap_struct(tail_bv)) {
            /* constrained-defn-monomorphize: a `:heap` struct tail (`Cons__A *`)
             * already fits the int64 carrier as a pointer; cast it rather than
             * malloc-boxing it (which would double-box into a `Cons__A **`).  See
             * the matching guard on the non-fat return path in this file. */
            buf_printf(body, "return (int64_t)(intptr_t)%s;\n", v);
        } else {
            const char *cty = emit_type_c_name(ctx, tail_bv);
            buf_printf(body,
                "{ %s *__tur_ret_p = (%s *)malloc(sizeof(%s)); "
                "*__tur_ret_p = %s; "
                "return (int64_t)(intptr_t)__tur_ret_p; }\n",
                cty, cty, cty, v);
        }
        free(v);
        return;
    }
    /* RSP1: returning a pass-by-ptr struct *parameter* directly.  The param is
     * held in C as `const T *`, but the function returns the struct by value
     * (pass-by-ptr applies to parameters, not returns), so `return x;` would
     * return a pointer where a `T` is expected.  Dereference to copy it out.
     * Mirrors the if-branch deref in emit_expr.c (emit_if_value). */
    {
        const Expr *re = e;
        while (re && re->kind == EX_ASCRIBE) re = re->as.ascribe_.inner;
        if (re && re->kind == EX_VAR && re->as.var.binding &&
            tail_bv.kind == TY_UNKNOWN && !(is_main && result_kind == TY_INT)) {
            for (uint8_t _i = 0; _i < ctx->n_pbp_params; _i++) {
                if (ctx->pbp_param_ptrs[_i] == re->as.var.binding) {
                    char *deref = (char *)malloc(strlen(v) + 4);
                    sprintf(deref, "*(%s)", v);
                    free(v);
                    v = deref;
                    break;
                }
            }
        }
    }
    indent_buf(body, ctx->indent);
    if (is_main && result_kind == TY_INT)
        buf_printf(body, "return (int)%s;\n", v);
    else
        buf_printf(body, "return %s;\n", v);
    free(v);
}

/* ------------ Phase 2: function emission ------------ */

void emit_fn_def(EmitCtx *ctx, Buf *file, const Expr *e) {
    FnDef *fd = e->as.fn_def_.fn;
    /* SYM5: detect the opt-in str->sym definition (from sym-dynamic.tur).  Its
     * presence is what links the runtime intern table, so it gates the
     * static-record seeding constructor emitted by sym_codegen_emit. */
    if (fd->binding && fd->binding->name && fd->binding->name->name &&
        strcmp(fd->binding->name->name, "str->sym") == 0) {
        sym_codegen_note_intern_used();
    }
    bool use_abi_spec = ctx->current_abi_specialization &&
        ctx->current_abi_specialization->fn == fd;
    /* Use raw name (without ID suffix) for function name */
    const char *fn_name = ctx->fn_name_override
        ? ctx->fn_name_override
        : raw_name_for_binding(fd->binding);

    /* Phase 19: Drain any pending effect handler functions accumulated while
     * elaborating the PREVIOUS top-level expression.  They must appear before
     * this function definition so they are at file scope. */
    if (ctx->pending_handler_fns && ctx->pending_handler_fns->len > 0) {
        buf_write(file, ctx->pending_handler_fns->data, ctx->pending_handler_fns->len);
        buf_free(ctx->pending_handler_fns);
        buf_init(ctx->pending_handler_fns);
    }


    /* Phase 19 (per-fiber): Route THIS function's body through a temp buffer so
     * that any handler functions accumulated during body emission are flushed to
     * file-scope BEFORE this function's definition. Without this, a handle
     * expression inside a non-final function generates a forward reference: the
     * function body is written to `file` first, then the handler appears after
     * the next function drains the pending buffer.
     *
     * Steps:
     *   1. Redirect ctx->file to a temp buf
     *   2. Emit the function definition into the temp buf
     *   3. Drain any newly-added handlers to the real file (file scope, before def)
     *   4. Append the temp buf to the real file
     */
    Buf fn_tmp; buf_init(&fn_tmp);
    Buf *real_file = file;
    ctx->file = &fn_tmp;
    file = &fn_tmp;

    /* poly-closure-result-specialization: an inner-closure-body spec emits its
     * env struct + cast under a suffixed env name so the float layout does not
     * collide with the base int64-carrier struct.  NULL/identical for ordinary
     * specs and non-spec closures. */
    const Symbol *env_name_eff = fd->closure ? fd->closure->env_name : NULL;
    if (use_abi_spec && ctx->current_abi_specialization->env_name_override)
        env_name_eff = ctx->current_abi_specialization->env_name_override;

    /* Phase 3: Emit env struct for closure thunks */
    if (fd->closure) {
        const Symbol *env_name = env_name_eff;
        /* Check if we've already emitted this env struct */
        bool already_emitted = false;
        if (ctx->env_struct_names) {
            for (uint8_t i = 0; i < ctx->n_env_struct_names; i++) {
                if (ctx->env_struct_names[i] == env_name) {
                    already_emitted = true;
                    break;
                }
            }
        }
        if (!already_emitted) {
            Type thunk_result = (e->type.kind == TY_FN && e->type.as.fn.result_full_type)
                ? *e->type.as.fn.result_full_type
                : emit_type_from_kind(e->type.kind == TY_FN ? e->type.as.fn.result_kind : TY_NIL);
            /* poly-closure-result-specialization: resolve the inner result tyvar
             * to its concrete (float) type so the typed thunk slot is xmm0-correct. */
            thunk_result = emit_resolve_type(ctx, thunk_result);
            Type *thunk_params = fd->n_params > 1 ? &fd->param_types[1] : NULL;
            Type resolved_thunk_params[MAX_FN_ARITY];
            if (thunk_params && use_abi_spec) {
                uint8_t tp_n = (uint8_t)(fd->n_params - 1);
                for (uint8_t _t = 0; _t < tp_n; _t++)
                    resolved_thunk_params[_t] = emit_resolve_type(ctx, fd->param_types[_t + 1]);
                thunk_params = resolved_thunk_params;
            }
            uint8_t thunk_arity = fd->n_params > 0 ? (uint8_t)(fd->n_params - 1) : 0;
            char *thunk_typedef = ensure_typed_thunk_typedef(ctx, file, thunk_result, thunk_params, thunk_arity);
            if (ctx->n_env_struct_names >= ctx->cap_env_struct_names) {
                ctx->cap_env_struct_names = ctx->cap_env_struct_names ? ctx->cap_env_struct_names * 2 : 8;
                ctx->env_struct_names = (const Symbol **)realloc(ctx->env_struct_names,
                    ctx->cap_env_struct_names * sizeof(const Symbol *));
            }
            ctx->env_struct_names[ctx->n_env_struct_names++] = env_name;
            
            /* Phase HKT §5: fat closure layout — __fn (int64_t) first, then
             * captures.  The fat pointer doubles as the env ptr for the thunk:
             * thunk receives fat_ptr as self, accesses captures via ->field
             * (which are at the correct offsets after __fn). */
            if (thunk_typedef) {
                buf_printf(file, "struct %s { %s __fn; ", env_name->name, thunk_typedef);
            } else {
                buf_printf(file, "struct %s { int64_t __fn; ", env_name->name);
            }
            for (uint8_t i = 0; i < fd->closure->n_captures; i++) {
                Binding *captured = fd->closure->captures[i];
                /* CY4: rank-2 captures must live as tur_poly_fn_t. */
                /* A captured fn value (fat closure or bare fn ptr) is carried as
                 * the int64_t fn-ABI carrier -- the same C type fn-typed
                 * parameters use.  type_c_name(TY_FN) returns the *result*
                 * type's C name (e.g. "double" for a :float-returning closure),
                 * which would alias the closure pointer through a floating-point
                 * field and reinterpret it via a double on read -- a latent
                 * miscompile that only survives because valid pointers fit
                 * exactly in a double's 53-bit integer range. */
                const char *cap_ctype = captured->is_poly_fn
                                          ? "tur_poly_fn_t"
                                          : (captured->type.kind == TY_FN
                                               ? "int64_t"
                                               : emit_type_c_name(ctx, captured->type));
                char *field = raw_name_for_binding(captured);
                buf_printf(file, "%s %s; ", cap_ctype, field);
                free(field);
            }
            buf_puts(file, "};\n");
            free(thunk_typedef);
        }
    }

    /* Emit function signature */
    /* Special case: C's main() must return int, not int64_t.
     * fn_name is already the mangled name; main is never module-prefixed. */
    bool is_main = (strcmp(fn_name, "main") == 0);
    /* Phase M3: In separate compilation mode, exported functions get extern
     * linkage (no static) so they can be called from other compilation units.
     * J3: ABI-specialization clone bodies emitted with external_linkage also
     * drop 'static' so they are visible to borrower TUs at link time. */
    /* spice-defn-return-result-kind-mismatch: stdlib defns preloaded into
     * every project-mode TU stay `static` -- otherwise every TU emits an
     * external copy of e.g. `ok-val` and the linker rejects the duplicates. */
    /* #[used]: a defn reachable only via its mangled C symbol (cross-module
     * inline-C bridge or by-address C-ABI callback) must keep external linkage
     * so the raw `extern <mangled>` reference in another TU resolves. */
    bool needs_static = !is_main &&
        !ctx->fn_name_override_external &&
        !(ctx->separate_compilation
          && (fd->binding->is_exported || fd->binding->retain_c_linkage)
          && !fd->binding->is_from_stdlib);
    if (needs_static) {
        buf_printf(file, "static ");
    }
    /* Return type from fn_type */
    if (e->type.kind == TY_FN) {
        TypeKind result = e->type.as.fn.result_kind;
        /* RT/SC5: carrier-return bridge for instance methods that resolve a
         * dispatch tyvar to a non-carrier by-value struct. */
        Type carrier_override = is_main ? (Type){0} : emit_carrier_return_override(fd);
        if (is_main) {
            buf_puts(file, "int");  /* C main must always return int */
        } else if (use_abi_spec) {
            /* Direction (1) of the open report: a typeclass instance
             * method (__inst_*) whose return type uses the carrier ABI
             * (parameterized struct like (Result A B)) emits int64_t
             * return -- the dispatch dict expects a uniform
             * `int64_t (*)(int64_t, ...)` shape, and the body's
             * by-value Result__T__B struct gets heap-spilled to a
             * pointer-as-int64 at the body emit.
             *
             * M4c Path A result-side: per-instantiation specs on non-HKT
             * classes (spec->typeclass_inst set) bypass the dispatch dict
             * — Path A's call site invokes the spec directly with no
             * carrier indirection.  Skip the int64_t override for those so
             * the spec's signature matches its return value and the
             * caller's bridge becomes unnecessary.  HKT-class specs keep
             * the carrier override per the M6/M7 carve-out. */
            Type rt = ctx->current_abi_specialization->result_type;
            bool is_instance_method =
                fd->binding && fd->binding->name && fd->binding->name->name &&
                strncmp(fd->binding->name->name, "__inst_", 7) == 0;
            Type rt_resolved = emit_resolve_type(ctx, rt);
            /* M7 layer-4 (flag-gated): a per-(f, A) by-value HKT instance-method
             * spec returns the resolved struct by value (keep in sync with the
             * body-side return-type branch and the forward decl in
             * emit_module.c). */
            if (g_m7_hkt_enabled && is_instance_method &&
                rt_resolved.kind == TY_APP &&
                !type_is_heap_struct(rt_resolved) &&
                type_has_concrete_codegen_layout(&rt_resolved)) {
                buf_puts(file, emit_type_c_name(ctx, rt));
            } else if (is_instance_method &&
                type_uses_carrier_abi(rt_resolved)
                && ctx->current_abi_specialization->typeclass_inst == NULL) {
                buf_puts(file, "int64_t");
            } else {
                buf_puts(file, emit_type_c_name(ctx, rt));
            }
        } else if (carrier_override.kind == TY_STRUCT) {
            buf_puts(file, emit_type_c_name(ctx, carrier_override));
        } else if (!is_main && e->type.as.fn.result_full_type &&
                   fd->binding && fd->binding->name && fd->binding->name->name &&
                   strncmp(fd->binding->name->name, "__inst_", 7) == 0 &&
                   type_uses_carrier_abi(emit_resolve_type(ctx,
                       *e->type.as.fn.result_full_type))) {
            /* Direction (1) of polymorphic-ok-in-typeclass-instance-method-...md:
             * non-spec instance method whose declared return is a carrier-ABI
             * parameterized struct (e.g. (Result T E)).  The dispatch dict's
             * uniform `int64_t (*)(...)` signature requires an int64 return; the
             * body's by-value struct gets heap-spilled at the return-emit below. */
            buf_puts(file, "int64_t");
        } else if (!is_main && e->type.as.fn.result_full_type &&
                   emit_inst_fn_return_carrier(fd, e->type.as.fn.result_full_type)) {
            /* instance-method-closure-return: a typeclass-method impl whose
             * declared return is a function value uses the thin fn-ptr typedef
             * (non-capturing) or the int64_t closure carrier (fat box) -- never
             * the function's result type.  Must agree with the dict field type
             * in emit_stmt.c and the forward decl in emit_module.c. */
            buf_puts(file, emit_inst_fn_return_carrier(fd, e->type.as.fn.result_full_type));
        } else if (e->type.as.fn.result_full_type) {
            bool body_is_inline_c = (fd->body && fd->body->kind == EX_INLINE_C);
            Type rft = *e->type.as.fn.result_full_type;
            /* fn-typed-return: a declared function-value return lowers to its
             * matching fn-ptr typedef (not the function's result type).  Skipped
             * for ^fat returns, which are wrapped into a void* heap fat box. */
            const char *fn_ret_td = e->type.as.fn.result_fat
                ? NULL : emit_fn_return_typedef(fd, &rft);
            /* ptr-generic-parameterised-type: a typed ptr<T> return lowers to
             * `T *` even from an inline-C body, so the C body's `return ptr;`
             * type-checks against the declared pointer type with no cast. */
            bool typed_ptr = (rft.kind == TY_PTR_VOID && rft.as.ptr.inner);
            /* inline-c-struct-return: a by-value struct return (e.g. the linear
             * FileHandle from io's file-open) lowers to the struct's C name even
             * from an inline-C body, so `return fh;` agrees with the declared
             * type.  Carrier-ABI structs are handled by carrier_override above. */
            bool typed_struct = (rft.kind == TY_STRUCT);
            /* typed-c-abi-function-pointers: a cfnptr return lowers to its
             * concrete `R (*)(A...)` typedef even from an inline-C body, so
             * the header prototype (which uses type_c_name) and the .c
             * definition agree -- and so an inline-C body returning the bare
             * function pointer type-checks against the declared return.  */
            bool typed_cfnptr = (rft.kind == TY_FN && rft.as.fn.cfnptr);
            /* end-to-end-monomorphization (bucket A): a `:heap` result on an
             * inline-C producer (e.g. `vec-new : (Vec A)`) lowers to the typed
             * pointer (`Vec__int *`) in an active ABI spec, so the spec binds a
             * typed-pointer local at the call site (no `(Vec__int *)(intptr_t)h`
             * cast at downstream typed consumers).  The carrier base (no active
             * spec) keeps int64.  The body returns via `__TUR_RET__`, which
             * matches: int64_t for the base, the typed pointer for the spec. */
            bool typed_heap_spec = body_is_inline_c && use_abi_spec &&
                                   type_is_heap_struct(rft);
            if (fn_ret_td && !body_is_inline_c) {
                buf_puts(file, fn_ret_td);
            } else if (!body_is_inline_c || typed_ptr || typed_struct ||
                       typed_cfnptr || typed_heap_spec) {
                buf_puts(file, emit_type_c_name(ctx, rft));
            } else {
                buf_puts(file, "int64_t");
            }
        } else {
            /* SF-application carrier bridge (struct/app closure return):
             * a lifted closure body whose declared return is a typed application
             * (e.g. (Pair float float)) often has result_full_type=NULL on the
             * outer TY_FN type, so the bare result_kind (TY_APP / TY_STRUCT)
             * here would lower to the int64_t carrier and mismatch the body's
             * actual struct return.  When the body's type has the concrete
             * codegen layout (Pair__float__float), prefer it so the C return
             * type matches the value the body produces. */
            const char *_body_c = (fd->body && (fd->body->type.kind == TY_APP
                                                || fd->body->type.kind == TY_STRUCT))
                ? emit_type_c_name(ctx, fd->body->type) : NULL;
            /* Direction (1) of polymorphic-ok-in-typeclass-instance-method-...md:
             * a non-spec instance method whose body codegen produces a
             * by-value carrier-ABI struct (e.g. Result__User__cstr) must
             * return int64_t to honor the dispatch dict's uniform pointer
             * signature; the body return is heap-spilled below.  If the
             * body codegen is already int64_t (TY_APP that lowered to a
             * carrier handle), the existing int64_t path is correct. */
            bool inst_method_struct_body =
                fd->binding && fd->binding->name && fd->binding->name->name &&
                strncmp(fd->binding->name->name, "__inst_", 7) == 0 &&
                _body_c && strcmp(_body_c, "int64_t") != 0 &&
                type_uses_carrier_abi(fd->body->type);
            /* M5 straddle (root cause C): a lifted lambda with no
             * result_full_type whose body's tail value comes from a
             * carrier-int64 producer (some/ok/err/none or an __inst_ method)
             * emits the int64 carrier -- and it is dispatched through the
             * int64-in/int64-out fat/poly thunk, so its signature must be
             * int64 too, NOT the by-value `_body_c` aggregate.  Without this
             * the signature (Option__int) and the body-return block (which
             * lowers the bare result_kind to int64) disagree -> cc error.  A
             * genuine by-value aggregate body (e.g. (Pair float float) via
             * make-struct) is not a carrier producer and keeps `_body_c`. */
            bool body_is_carrier_producer =
                _body_c && strcmp(_body_c, "int64_t") != 0 &&
                type_uses_carrier_abi(emit_resolve_type(ctx, fd->body->type)) &&
                fn_body_tail_is_carrier_producer(fd->body);
            if (inst_method_struct_body || body_is_carrier_producer) {
                buf_puts(file, "int64_t");
            } else if (_body_c && strcmp(_body_c, "int64_t") != 0) {
                buf_puts(file, _body_c);
            } else {
                buf_puts(file, emit_type_c_name(ctx, emit_type_from_kind(result)));
            }
        }
    } else {
        buf_puts(file, "void");
    }
    buf_printf(file, " %s(", fn_name);

    /* CLI-ARGS: a user-defined zero-arg `main` must still take (argc, argv)
     * at the C level so g_tur_args can be populated from the process argv.
     * Without this, `*args*` is always empty in user-main programs. */
    bool emit_main_argv = (is_main && fd->n_params == 0);
    if (emit_main_argv) {
        buf_puts(file, "int argc, char **argv");
    }

    /* Emit parameters - use raw names (without ID suffix) */
    bool body_is_inline_c = (fd->body && fd->body->kind == EX_INLINE_C);
    /* Prereq 6: when a polymorphic stdlib constructor's inline-C body
     * (which assumes int64-castable `x`) is monomorphized for a value-
     * struct A, the source `(int64_t)(intptr_t)x` cast on the struct
     * rvalue is invalid C. The boxing-needed shape is three existing
     * predicates: body_is_inline_c + return type uses carrier ABI + the
     * spec arg type is a non-parametric value-struct. When all three
     * hold for param i, we rename its C identifier to `__tur_inbox_<orig>`
     * here and emit a heap-spill stanza after the param list closer so
     * `<orig>` shadows the parameter as a `T *` heap pointer -- the
     * inline-C body's `(int64_t)(intptr_t)<orig>` cast then operates on
     * a pointer and produces the int64 carrier the call site expects.
     * No source-level annotation: the recognizer is purely structural.
     * See docs/reported/polymorphic-ok-fails-for-value-struct-payload.md. */
    bool needs_box_spill[16];
    for (uint8_t i = 0; i < 16; i++) needs_box_spill[i] = false;
    /* M2b: the box-spill recognizer was originally inline-C only (the body's
     * `(int64_t)(intptr_t)x` cast required `x` to be a pointer).  Now that
     * `#{Construct}` bodies can be written as `(make-struct …)` instead, the
     * same monomorphization shape (value-struct payload through a carrier-ABI
     * result) still needs the box-spill — the prereq-6 synthesis below
     * heap-spills the param value into the struct's pointer-typed payload
     * slot.  Enable the recognizer for `#{Construct}` make-struct bodies as
     * well as inline-C ones.  See
     * docs/reported/m2b-stdlib-migration-blocked-on-carrier-fallback.md. */
    bool body_is_make_struct = fd->body && fd->body->kind == EX_MAKE_STRUCT;
    bool boxspill_eligible_body =
        body_is_inline_c ||
        (body_is_make_struct && fd->binding && fd->binding->is_construct_template);
    if (use_abi_spec && boxspill_eligible_body) {
        Type ret = ctx->current_abi_specialization->result_type;
        bool return_uses_carrier =
            type_uses_carrier_abi(emit_resolve_type(ctx, ret));
        if (return_uses_carrier) {
            for (uint8_t i = 0; i < fd->n_params && i < 16; i++) {
                Type pty = ctx->current_abi_specialization->arg_types[i];
                bool is_value_struct =
                    pty.kind == TY_STRUCT && pty.as.struct_.def &&
                    !pty.as.struct_.def->is_opaque &&
                    pty.as.struct_.def->n_type_params == 0 &&
                    !type_uses_carrier_abi(emit_resolve_type(ctx, pty));
                if (is_value_struct) needs_box_spill[i] = true;
            }
        }
    }
    /* B4 (byvalue-recursive-carrier, slice 2): the inverse of box-spill.  A
     * CLOSURE thunk whose parameter is a WIDE (>8 byte) by-value ADT receives it
     * across the fat-closure boundary as an int64 heap-box POINTER (the value
     * cannot ride the uniform int64 carrier slot directly).  Emit that param as
     * int64_t under a `__tur_b4box_<orig>` alias and deref+copy it back into the
     * by-value aggregate `<orig>` at body entry.  The box is owned by the
     * enclosing carrier node (allocated in its monomorph ctor), so the thunk
     * only borrows it -- no free here.  Restricted to closures: a top-level fn
     * taking a wide by-value ADT uses the ordinary pass-by-ptr ABI, not the fat
     * carrier. */
    bool needs_box_load[16];
    for (uint8_t i = 0; i < 16; i++) needs_box_load[i] = false;
    if (fd->closure) {
        for (uint8_t i = 0; i < fd->n_params && i < 16; i++) {
            if (fd->params[i]->is_poly_fn ||
                fd->param_types[i].kind == TY_FN) continue;
            Type pty = use_abi_spec
                ? ctx->current_abi_specialization->arg_types[i]
                : ((e->type.as.fn.arg_full_types && e->type.as.fn.arg_full_types[i])
                       ? *e->type.as.fn.arg_full_types[i] : fd->param_types[i]);
            if (type_is_wide_byval_adt(emit_resolve_type(ctx, pty)))
                needs_box_load[i] = true;
        }
    }
    for (uint8_t i = 0; i < fd->n_params; i++) {
        if (i > 0) buf_puts(file, ", ");
        /* B4 slice 2: wide by-value ADT closure param arrives as int64 box ptr. */
        if (i < 16 && needs_box_load[i]) {
            const char *pn = raw_name_for_binding(fd->params[i]);
            buf_printf(file, "int64_t __tur_b4box_%s", pn);
            free((void*)pn);
            continue;
        }
        /* Phase HRT1: poly fn params use tur_poly_fn_t in signature */
        if (fd->params[i]->is_poly_fn) {
            buf_puts(file, "tur_poly_fn_t");
        } else if (fd->param_types[i].kind == TY_FN
                   && fd->param_types[i].as.fn.cfnptr) {
            /* typed-c-abi-function-pointers: a cfnptr parameter is a bare C
             * function pointer `R (*)(A...)`, declared via the concrete
             * fn-ptr typedef so the (possibly inline-C) body can invoke it
             * directly and external C libraries see the exact ABI shape.
             * Falls back to int64_t only when the signature is non-concrete. */
            const char *td = register_fn_ptr_typedef(&fd->param_types[i]);
            buf_puts(file, td ? td : "int64_t");
        } else if (fd->param_types[i].kind == TY_FN) {
            /* ER4: function-typed parameters are passed as int64_t (opaque
             * function pointer) in Turmeric's calling convention. */
            buf_puts(file, "int64_t");
        } else if (fd->params[i]->is_fat && body_is_inline_c) {
            /* fat-param-emitted-as-void-ptr-warns-in-inline-c.md: a ^fat param
             * is a fat-closure *carrier* handle.  In an inline-C body, emit it as
             * int64_t -- the same carrier ABI every other Turmeric value uses --
             * so the hand-written C can treat it as a handle without a
             * void*->int64_t -Wint-conversion warning.  The C ABI is identical
             * (8-byte register arg); the call site coerces with (int64_t)(intptr_t).
             * Ordinary (compiler-generated) bodies keep the void* fat carrier so
             * the fat-call dispatch and typeclass-dictionary slot types are
             * unchanged. */
            buf_puts(file, "int64_t");
        } else if (use_abi_spec) {
            const char *pc = emit_type_c_name(ctx, ctx->current_abi_specialization->arg_types[i]);
            buf_puts(file, pc);
            /* KB-021: a concrete carrier-ABI spec param is passed by value.
             * CONV-S1: a by-value parametric record-ADT monomorph param
             * (`tur_adt_Option__int`) is ALSO passed by value but reports
             * NON-carrier from type_uses_carrier_abi (it is genuinely by-value,
             * P2-P4), so it was not flagged -- leaving the ACB carrier->concrete
             * bridge to wrongly deref it (`(*(tur_adt_Option__int *)(intptr_t)
             * container)`) inside an HKT instance body whose param's generic elab
             * type `(Option A)` still reads as the carrier.  Flag a concrete ADT-app
             * spec param too so it is recognised as already by-value. */
            Type resolved_spec_arg =
                emit_resolve_type(ctx, ctx->current_abi_specialization->arg_types[i]);
            if (fd->params[i] && strcmp(pc, "int64_t") != 0 &&
                (type_uses_carrier_abi(resolved_spec_arg) ||
                 type_app_is_concrete_adt(&resolved_spec_arg)))
                fd->params[i]->emit_byvalue_carrier_abi = true;
        } else {
            /* Phase D: large structs use const T* ABI unless in inline-C or closure.
             * bare-fat-sink-poly-box-slot0-int64-mismatch.md: a ^fat param is a
             * fat-closure *carrier* handle, never a by-value fn; its arg_full_types
             * slot may now hold a synthesized (fn ...) signature purely so a caller
             * boxing a tur_poly_fn_t can pick the right slot-0 shim.  Emit the
             * carrier type from param_types here so that signature does not leak
             * into the param's own C type (type_c_name(fn) -> the result type). */
            Type param_ty = (!fd->params[i]->is_fat &&
                             e->type.as.fn.arg_full_types && e->type.as.fn.arg_full_types[i])
                ? *e->type.as.fn.arg_full_types[i]
                : fd->param_types[i];
            if (!fd->closure && !body_is_inline_c && type_struct_pass_by_ptr(param_ty)) {
                buf_printf(file, "const %s *", emit_type_c_name(ctx, param_ty));
            } else {
                const char *pc = emit_type_c_name(ctx, param_ty);
                buf_puts(file, pc);
                /* KB-021: a by-value concrete carrier-ABI parameter must be
                 * bridged to the carrier when used at a dispatch callsite. */
                if (fd->params[i] &&
                    type_uses_carrier_abi(emit_resolve_type(ctx, param_ty)) &&
                    strcmp(pc, "int64_t") != 0)
                    fd->params[i]->emit_byvalue_carrier_abi = true;
            }
        }
        const char *pn = raw_name_for_binding(fd->params[i]);
        if (i < 16 && needs_box_spill[i]) {
            /* Prereq 6: rename the parameter so the inline-C body's
             * `<orig>` identifier can be redeclared as a heap pointer
             * inside the function. */
            buf_printf(file, " __tur_inbox_%s", pn);
        } else {
            buf_printf(file, " %s", pn);
        }
        free((void*)pn);
    }
    buf_puts(file, ") {\n");

    /* Debugger Phase 4 (--debug): anchor the function body to the defn's source
     * line so a gdb/lldb frame for this function resolves to its `.tur` site.
     * Reset the dedup tracker first: this body is a fresh statement stream
     * (possibly into a temp buffer), so the first statement must re-anchor. */
    emit_line_reset(ctx);
    emit_line_directive(ctx, file, e->span);

    /* B4 (byvalue-recursive-carrier, slice 2): materialize each wide by-value
     * ADT closure param from its int64 heap box at entry -- deref + copy into
     * the by-value aggregate `<orig>` the body expects.  Borrow only: the box is
     * owned by the carrier node that produced it, so no free here. */
    for (uint8_t i = 0; i < fd->n_params && i < 16; i++) {
        if (!needs_box_load[i]) continue;
        Type pty = use_abi_spec
            ? ctx->current_abi_specialization->arg_types[i]
            : ((e->type.as.fn.arg_full_types && e->type.as.fn.arg_full_types[i])
                   ? *e->type.as.fn.arg_full_types[i] : fd->param_types[i]);
        const char *ctype = emit_type_c_name(ctx, pty);
        const char *pn = raw_name_for_binding(fd->params[i]);
        indent_buf(file, ctx->indent + 4);
        buf_printf(file, "%s %s = *(%s *)(intptr_t)__tur_b4box_%s;\n",
                   ctype, pn, ctype, pn);
        free((void*)pn);
    }

    /* M2b RETIRED M2a inference (2026-06-13).
     *
     * Background: M2a (generalized from Prereq 6) used shape inference to
     * synthesize a direct by-value constructor body for `#{Construct}`-
     * annotated polymorphic constructors with value-struct payloads.  It
     * reconstructed the discriminator-tag + payload-field assignment by
     * matching the param's C type against StructDef type-params, then
     * emitted `__tur_p6_r.is_ok = true; memset(...); __tur_p6_r.ok_val =
     * payload;` style code.
     *
     * Why retired: M2b shipped `(make-struct StructName :field val ...)`
     * and `(default-of T)` body forms in elab_structs.c / elab_types.c.
     * stdlib's ok/err/some/none/pair/tcons-of now have explicit make-struct
     * bodies; the normal EX_MAKE_STRUCT emit at emit_expr.c:3700-3731
     * handles the field-by-field construction including heap-spill for
     * value-struct payload fields via struct_field_c_type's pointer
     * lowering.  Empirically, disabling the inference branch produced
     * cleaner C (designated initializer instead of memset-then-assign)
     * with zero fixture-snapshot diffs across the suite (1564/86 baseline)
     * because no fixture's snapshot exercised the inference path -- the
     * stdlib migration to make-struct made it dead code.
     *
     * The variable name `prereq6_synthesized_body` stays for the symmetric
     * m2b_carrier_synth control flow below; this flag remains false
     * because the normal body-emit path handles the by-value case now. */
    bool prereq6_synthesized_body = false;

    /* M2b (end-to-end-monomorphization-plan, Plan M2): synthesize a CARRIER
     * body for any `#{Construct}` polymorphic constructor whose user body is
     * a `(make-struct …)` expression and which is being emitted in the
     * generic carrier-emit path (no `current_abi_specialization`).  Without
     * this, the make-struct body lowers to `(int64_t){.field = …}` — invalid
     * C (designated initializer on a non-aggregate) — because the result
     * type's tyvars never get bound.
     *
     * The synthesized body replays the legacy tur_box_ok / tur_box_err / tur_box_some /
     * TUR_NONE pattern: identify (a) the bool discriminator field's literal
     * value in the make-struct, and (b) the payload field whose value is a
     * direct parameter reference; then emit
     *     return tur_<helper>((int64_t)(intptr_t)<payload>);
     * The helper is chosen from the StructDef name + discriminator value.
     * Currently supports Result (ok / err) and Option (some / none).
     *
     * Out of scope: structs without a bool discriminator (Pair, Cons) — they
     * have no carrier helper and never produce carrier values, so the
     * generic body for them is unreachable from carrier call sites.
     *
     * See docs/reported/m2b-stdlib-migration-blocked-on-carrier-fallback.md
     * for the rationale; this implements that report's option (a). */
    bool m2b_carrier_synth = false;
    /* Fire in TWO contexts:
     *   (a) generic carrier-only emit (no spec at all) — every #{Construct}
     *       polymorphic defn must always emit a callable fallback symbol;
     *   (b) a spec whose declared result type lowers to the int64 carrier
     *       (typeclass-method dispatch context: the method's signature is
     *       int64-in/int64-out so the dict slot is uniformly typed). */
    bool carrier_spec_return = false;
    if (use_abi_spec) {
        const char *rc = emit_type_c_name(ctx,
            ctx->current_abi_specialization->result_type);
        carrier_spec_return = rc && strcmp(rc, "int64_t") == 0;
    }
    if ((!use_abi_spec || carrier_spec_return)
        && fd->binding && fd->binding->is_construct_template
        && fd->body && fd->body->kind == EX_MAKE_STRUCT) {
        StructDef *def = fd->body->as.make_struct_.def;
        if (def && def->name) {
            int disc_field_idx = -1;
            for (uint32_t fi = 0; fi < def->n_fields; fi++) {
                if (def->fields[fi].kind == TY_BOOL) {
                    disc_field_idx = (int)fi;
                    break;
                }
            }
            bool disc_value = false;
            int payload_param_idx = -1;
            bool disc_known = false;
            if (disc_field_idx >= 0
                && (uint32_t)disc_field_idx < fd->body->as.make_struct_.n_fields) {
                Expr *dv = fd->body->as.make_struct_.field_values[disc_field_idx];
                if (dv && dv->kind == EX_BOOL_LIT) {
                    disc_value = dv->as.b;
                    disc_known = true;
                    /* Find a non-discriminator field whose value is a direct
                     * parameter reference (i.e. the carrier payload). */
                    for (uint32_t fi = 0; fi < fd->body->as.make_struct_.n_fields; fi++) {
                        if ((int)fi == disc_field_idx) continue;
                        Expr *fv = fd->body->as.make_struct_.field_values[fi];
                        if (!fv || fv->kind != EX_VAR) continue;
                        Binding *b = fv->as.var.binding;
                        for (uint8_t pi = 0; pi < fd->n_params; pi++) {
                            if (fd->params[pi] == b) {
                                payload_param_idx = (int)pi;
                                break;
                            }
                        }
                        if (payload_param_idx >= 0) break;
                    }
                }
            }

            const char *helper = NULL;
            if (disc_known) {
                if (strcmp(def->name, "Result") == 0)
                    helper = disc_value ? "tur_box_ok" : "tur_box_err";
                else if (strcmp(def->name, "Option") == 0)
                    helper = disc_value ? "tur_box_some" : NULL;
            }

            if (disc_known && (helper || (!disc_value
                                          && strcmp(def->name, "Option") == 0))) {
                indent_buf(file, ctx->indent + 4);
                if (!helper) {
                    /* none: zero-payload, returns NULL Option. */
                    buf_puts(file, "return 0;\n");
                } else if (payload_param_idx >= 0) {
                    const char *pn = raw_name_for_binding(fd->params[payload_param_idx]);
                    buf_printf(file, "return %s((int64_t)(intptr_t)%s);\n", helper, pn);
                    free((void*)pn);
                } else {
                    /* No payload param found (shouldn't happen for ok/err/some);
                     * fall back to a NULL carrier so the emitted C compiles. */
                    buf_puts(file, "return 0;\n");
                }
                m2b_carrier_synth = true;
            }
        }
    }

    {
        /* M2b retirement of the M2a inference path: the shape-inference
         * code that used to live here (Result/Option/discriminator field
         * detection + tyvar-position matching + memset-spill emit) is gone.
         * ok/err/some/none now have explicit (make-struct ...) bodies in
         * stdlib; the normal EX_MAKE_STRUCT emit handles their construction
         * including value-struct payload heap-spill via struct_field_c_type.
         * Empirically the inference branch was dead code -- disabling it
         * produced zero fixture-snapshot diffs and ran the suite clean
         * (1564/86 baseline).  See the M2b design doc and audit Section 10
         * for the migration history.
         *
         * The heap-spill stanza below stays: it sets up the `__tur_inbox_X`
         * pointer for the few inline-C `#{Construct}` bodies that may still
         * cast their params via `(int64_t)(intptr_t)x`.  Stdlib has none,
         * but the safety net is cheap and future user code can rely on it. */
        {
            for (uint8_t i = 0; i < fd->n_params && i < 16; i++) {
                if (!needs_box_spill[i]) continue;
                Type pty = ctx->current_abi_specialization->arg_types[i];
                const char *ctype = emit_type_c_name(ctx, pty);
                const char *pn = raw_name_for_binding(fd->params[i]);
                indent_buf(file, ctx->indent + 4);
                buf_printf(file,
                           "%s *%s = (%s *)malloc(sizeof(%s)); *%s = __tur_inbox_%s;\n",
                           ctype, pn, ctype, ctype, pn, pn);
                free((void*)pn);
            }
        }
    }

    /* Phase R6: Inject g_panic_trace initialization at start of main */
    if (is_main && g_panic_trace) {
        ctx->indent += 4;
        indent_buf(file, ctx->indent);
        buf_puts(file, "g_panic_trace = 1;\n");
        ctx->indent -= 4;
    }

    /* CLI-ARGS: build *args* cons list from argv[1..argc-1] before user code runs.
     * Mirrors the synthesized-main path in emit_module.c so user-main and
     * synthesized-main programs see *args* identically. */
    if (emit_main_argv) {
        ctx->indent += 4;
        indent_buf(file, ctx->indent);
        buf_puts(file, "/* *args*: build cons list from argv[1..argc-1] */\n");
        indent_buf(file, ctx->indent);
        buf_puts(file, "g_tur_args = 0;\n");
        indent_buf(file, ctx->indent);
        buf_puts(file, "for (int _ai = argc - 1; _ai >= 1; _ai--) {\n");
        indent_buf(file, ctx->indent);
        buf_puts(file, "    typedef struct { int64_t value; int64_t next; } __tur_args_cell;\n");
        indent_buf(file, ctx->indent);
        buf_puts(file, "    __tur_args_cell *_c = (__tur_args_cell *)malloc(sizeof(__tur_args_cell));\n");
        indent_buf(file, ctx->indent);
        buf_puts(file, "    _c->value = (int64_t)(intptr_t)argv[_ai];\n");
        indent_buf(file, ctx->indent);
        buf_puts(file, "    _c->next = g_tur_args;\n");
        indent_buf(file, ctx->indent);
        buf_puts(file, "    g_tur_args = (int64_t)(intptr_t)_c;\n");
        indent_buf(file, ctx->indent);
        buf_puts(file, "}\n");
        ctx->indent -= 4;
    }

    /* Phase 9 follow-up: Run last-use elision analysis on the function body
     * before emitting.  This marks eligible EX_RC_CLONE/EX_RC_DROP nodes so
     * the emitter can skip redundant rc_strong_increment/decrement pairs. */
    rc_elision_analyze_fn(fd->body);

    /* Emit function body */
    ctx->indent += 4;

    /* Set up function parameter context so that parameter references
     * in the body use raw names (without ID suffix) */
    Binding **saved_params = ctx->fn_params;
    uint8_t saved_n_params = ctx->n_fn_params;
    ctx->fn_params = fd->params;
    ctx->n_fn_params = fd->n_params;

    /* Phase D: record which params are pbp for field-access and call-site handling. */
    uint8_t saved_n_pbp = ctx->n_pbp_params;
    ctx->n_pbp_params = 0;
    if (!fd->closure && !body_is_inline_c) {
        for (uint8_t _pi = 0; _pi < fd->n_params && ctx->n_pbp_params < 16; _pi++) {
            Type pty = (e->type.as.fn.arg_full_types && e->type.as.fn.arg_full_types[_pi])
                ? *e->type.as.fn.arg_full_types[_pi] : fd->param_types[_pi];
            if (type_struct_pass_by_ptr(pty))
                ctx->pbp_param_ptrs[ctx->n_pbp_params++] = fd->params[_pi];
        }
    }

    /* Phase R5: Set no_unwind context from the function's binding attribute */
    bool saved_no_unwind = ctx->no_unwind;
    ctx->no_unwind = fd->binding->no_unwind;

    /* Phase 3: Set up closure context if this is a closure thunk */
    struct Closure *saved_closure = ctx->closure;
    const char *saved_env_var_name = ctx->env_var_name;
    if (fd->closure) {
        ctx->closure = fd->closure;
        /* First parameter is the env */
        if (fd->n_params > 0) {
            Binding *env_param = fd->params[0];
            /* Emit cast of env parameter to env struct type */
            indent_buf(file, ctx->indent);
            char *env_param_name = raw_name_for_binding(env_param);
            /* Create a local variable name for the casted env */
            char env_var_name_buf[80];
            snprintf(env_var_name_buf, sizeof(env_var_name_buf), "__env_%s", env_name_eff->name);
            buf_printf(file, "struct %s *%s = (struct %s *)%s;\n",
                       env_name_eff->name,
                       env_var_name_buf,
                       env_name_eff->name,
                       env_param_name);
            free(env_param_name);
            /* Store the env variable name for use in name_for_binding */
            ctx->env_var_name = strdup(env_var_name_buf);
        }
    }

    /* Get the return type */
    TypeKind result_kind = use_abi_spec
        ? ctx->current_abi_specialization->result_type.kind
        : (e->type.kind == TY_FN ? e->type.as.fn.result_kind : TY_NIL);

    /* Phase 3/4: If the body diverges on every path (return/panic), emit it as
     * statements only -- a trailing `return` would be dead code.  A body that
     * may still fall through with a value (e.g. an early `return` in only one
     * `if` branch) must go through the emit_value path below so the
     * fall-through value is returned. */
    bool body_diverges = expr_tail_diverges(fd->body);

    /* CF1: detect self-tail-calls so a value-returning, simple-parameter
     * function can be emitted as an iterative loop instead of self-recursion.
     * Excludes main (different signature handling).
     *
     * M4 follow-up (docs/upcoming/tco-in-abi-specs-for-stdlib-iteration.md):
     * lifted the `!use_abi_spec` restriction.  TCO inside an ABI spec is
     * safe: the `__tur_tailcall:` label and the param-reassign loop are
     * pure C-level constructs, and `tco_params_simple` reads the spec's
     * resolved param types (post-substitution) so the backedge declarations
     * match the spec's signature.  Carrier-ABI params still reject in
     * `tco_params_simple` so a backedge's reassignment never crosses an
     * int64→struct boundary unintentionally. */
    bool tco_eligible = !body_diverges && fd->body->kind != EX_INLINE_C &&
        !(result_kind == TY_NIL && !is_main) && !is_main &&
        tco_params_simple(ctx, e, fd) && tco_mark(ctx, fd, fn_name, fd->body) > 0;

    if (prereq6_synthesized_body) {
        /* Prereq 6: the function body was already emitted as a synthesized
         * wrapper above (heap-spill + carrier helper call + cast back to
         * the by-value struct). Skip the normal body-emit paths; the
         * function's closing brace at the end of emit_fn_def still runs. */
    } else if (m2b_carrier_synth) {
        /* M2b carrier-emit synth (above) already wrote a `return tur_*(...)`
         * line; skip the make-struct body so it doesn't double-emit. */
    } else if (body_diverges) {
        /* Body diverges on every path - emit as statements only */
        emit_stmt(ctx, file, fd->body);
    } else if (fd->body->kind == EX_INLINE_C) {
        /* Inline C body - emit as-is (it contains its own return statements) */
        emit_stmt(ctx, file, fd->body);
    } else if (result_kind == TY_NIL && !is_main) {
        /* void function - emit body as statements */
        emit_stmt(ctx, file, fd->body);
    } else if (tco_eligible) {
        /* CF1: self-tail-call loop. Label the top of the body; emit_tail turns
         * each self-tail-call into a parameter-reassignment + goto backedge. */
        indent_buf(file, ctx->indent);
        buf_puts(file, "__tur_tailcall:;\n");
        emit_tail(ctx, file, e, fd, fd->body, result_kind, is_main);
    } else if (fd->body->type.kind == TY_NIL) {
        /* Body is nil-typed (e.g. a bare void call like (show 99)) but the
         * function expects a return value -- typically (defn main [] :int
         * (some-void-call)).  Emit the body as a STATEMENT so its side effects
         * (and the call itself) actually execute; emit_value would only return
         * the call string for a caller to consume, and the discard below would
         * drop the call.  Then return the default for result_kind. */
        emit_stmt(ctx, file, fd->body);
        indent_buf(file, ctx->indent);
        const char *dflt = (result_kind == TY_BOOL) ? "false" : "0";
        if (is_main && result_kind == TY_INT) {
            buf_printf(file, "return (int)%s;\n", dflt);
        } else {
            buf_printf(file, "return %s;\n", dflt);
        }
    } else {
        /* Function with return value */
        char *ret_val = emit_fat_return_value(ctx, file, e, fd->body);
        /* RSP1: returning a pass-by-ptr struct *parameter* directly.  The param
         * is held in C as `const T *` but the function returns the struct by
         * value, so `return x;` returns a pointer where a `T` is expected.
         * Dereference to copy it out.  A bare struct param is a concrete sized
         * value, never an Option/Result carrier aggregate, so it never collides
         * with the carrier-spill return paths below.  Mirrors emit_tail. */
        if (fd->body) {
            const Expr *re = fd->body;
            while (re && re->kind == EX_ASCRIBE) re = re->as.ascribe_.inner;
            if (re && re->kind == EX_VAR && re->as.var.binding &&
                !(is_main && result_kind == TY_INT)) {
                for (uint8_t _i = 0; _i < ctx->n_pbp_params; _i++) {
                    if (ctx->pbp_param_ptrs[_i] == re->as.var.binding) {
                        char *deref = (char *)malloc(strlen(ret_val) + 4);
                        sprintf(deref, "*(%s)", ret_val);
                        free(ret_val);
                        ret_val = deref;
                        break;
                    }
                }
            }
        }
        indent_buf(file, ctx->indent);
        /* closure-carrier-return-and-arg-int-pointer-warnings: determine the
         * function's actual C return type so a fn-typed body returned through
         * the int64_t/void* closure carrier gets the matching bridge cast.
         * This mirrors the signature-emission branches above; the body is
         * guaranteed non-inline-c here (inline-c is handled earlier), so the
         * inline-c-only sub-branches there do not apply. */
        const char *ret_ctype = NULL;
        bool inst_method_carrier_spill = false;
        if (e->type.kind == TY_FN && !is_main) {
            Type carrier_override = emit_carrier_return_override(fd);
            if (use_abi_spec) {
                Type rt = ctx->current_abi_specialization->result_type;
                bool is_inst = fd->binding && fd->binding->name &&
                    fd->binding->name->name &&
                    strncmp(fd->binding->name->name, "__inst_", 7) == 0;
                /* M4c Path A result-side: non-HKT instance method specs
                 * (typeclass_inst set) skip the carrier-int64 override.
                 * The signature emit at L412 + the forward decl at
                 * emit_module.c:1853 use the same gate; keep them in sync. */
                Type rt_resolved = emit_resolve_type(ctx, rt);
                /* M7 layer-4 (flag-gated): a per-(f, A) by-value HKT
                 * instance-method spec returns the resolved struct (`Option__int`)
                 * BY VALUE, not the int64 carrier -- this is the whole point of
                 * the spec.  Detect it by the concrete by-value TY_APP result
                 * (same guards as construct_recovered_byvalue) and skip the
                 * carrier spill below. */
                if (g_m7_hkt_enabled && is_inst &&
                    rt_resolved.kind == TY_APP &&
                    !type_is_heap_struct(rt_resolved) &&
                    type_has_concrete_codegen_layout(&rt_resolved)) {
                    ret_ctype = emit_type_c_name(ctx, rt);
                } else if (is_inst && type_uses_carrier_abi(rt_resolved)
                    && ctx->current_abi_specialization->typeclass_inst == NULL) {
                    ret_ctype = "int64_t";
                    inst_method_carrier_spill = true;
                } else {
                    ret_ctype = emit_type_c_name(ctx, rt);
                }
            } else if (carrier_override.kind == TY_STRUCT) {
                ret_ctype = emit_type_c_name(ctx, carrier_override);
            } else if (e->type.as.fn.result_full_type &&
                       fd->binding && fd->binding->name && fd->binding->name->name &&
                       strncmp(fd->binding->name->name, "__inst_", 7) == 0 &&
                       type_uses_carrier_abi(emit_resolve_type(ctx,
                           *e->type.as.fn.result_full_type))) {
                ret_ctype = "int64_t";
                inst_method_carrier_spill = true;
            } else if (e->type.as.fn.result_full_type &&
                       emit_inst_fn_return_carrier(fd,
                           e->type.as.fn.result_full_type)) {
                ret_ctype = emit_inst_fn_return_carrier(fd,
                    e->type.as.fn.result_full_type);
            } else if (e->type.as.fn.result_full_type) {
                Type rft = *e->type.as.fn.result_full_type;
                const char *fn_ret_td = e->type.as.fn.result_fat
                    ? NULL : emit_fn_return_typedef(fd, &rft);
                ret_ctype = fn_ret_td ? fn_ret_td : emit_type_c_name(ctx, rft);
            } else if (fd->binding && fd->binding->name && fd->binding->name->name &&
                       strncmp(fd->binding->name->name, "__inst_", 7) == 0 &&
                       fd->body && type_uses_carrier_abi(fd->body->type)) {
                /* Direction (1): non-spec instance method, result_full_type
                 * absent.  Spill only when body codegen is by-value struct
                 * (matching the signature fallback above). */
                const char *_body_c2 = emit_type_c_name(ctx, fd->body->type);
                if (_body_c2 && strcmp(_body_c2, "int64_t") != 0) {
                    ret_ctype = "int64_t";
                    inst_method_carrier_spill = true;
                } else {
                    ret_ctype = emit_type_c_name(ctx,
                        emit_type_from_kind(e->type.as.fn.result_kind));
                }
            } else {
                ret_ctype = emit_type_c_name(ctx,
                    emit_type_from_kind(e->type.as.fn.result_kind));
            }
        }
        bool ret_is_int64_carrier = ret_ctype &&
            strcmp(ret_ctype, "int64_t") == 0;
        /* Special case: if this is main and it returns int64_t, cast to int */
        if (is_main && result_kind == TY_INT) {
            buf_printf(file, "return (int)%s;\n", ret_val);
        } else if (inst_method_carrier_spill) {
            /* Direction (1): instance method whose declared result is a
             * parameterized struct (e.g. (Result T E)) returns the carrier
             * int64 handle.  Heap-spill the by-value struct and cast its
             * pointer as int64_t so the dispatch dict's uniform
             * `int64_t (*)(...)` signature is honored. */
            Type rt;
            if (use_abi_spec) {
                rt = ctx->current_abi_specialization->result_type;
            } else if (e->type.as.fn.result_full_type) {
                rt = emit_resolve_type(ctx, *e->type.as.fn.result_full_type);
            } else {
                rt = fd->body->type;
            }
            const char *struct_cty = emit_type_c_name(ctx, rt);
            /* M7 (flag-gated): in a GENERIC carrier base instance method whose
             * declared result is a parameterized struct with a free element
             * tyvar -- e.g. `Decode`'s `(Result a cstr)` -- `rt` resolves to the
             * int64 carrier (no instance specialization active here).  But under
             * the flag the concrete instance body recovers its construct BY VALUE
             * (`(ok (make-struct Point ...))` -> `ok__spec` returning
             * `Result__Point__cstr`), so `ret_val` is a by-value aggregate.
             * Spilling it as `sizeof(int64_t)` + `*(int64_t*)p = <struct>` is a
             * hard cc error (aggregate into integer).  When the body's own type
             * is a concrete non-carrier aggregate, spill THAT type so the malloc
             * size and the cast match the actual value.  Flag-off the body stays
             * a carrier int64 here, so this is inert (byte-identical) flag-off.
             * See docs/reported/m7-hkt-bimap-... family / instance-method-
             * return-carrier-bridge fixture. */
            if (g_m7_hkt_enabled && struct_cty &&
                strcmp(struct_cty, "int64_t") == 0 && fd->body) {
                const char *body_cty = emit_type_c_name(ctx, fd->body->type);
                if (body_cty && strcmp(body_cty, "int64_t") != 0)
                    struct_cty = body_cty;
            }
            if (g_m7_hkt_enabled && struct_cty &&
                strcmp(struct_cty, "int64_t") == 0) {
                /* M7 (flag-gated): the body already produced the carrier int64
                 * handle -- e.g. a partial-application `(Result _ E)` instance
                 * whose pure-Turmeric body lowered to the carrier `ok`/`err` (the
                 * spill type stays int64 because the result element doesn't ground
                 * by value).  There is nothing to box: malloc'ing another int64
                 * and storing the handle into it DOUBLE-boxes (the by-value
                 * consumer then reads a pointer-to-handle as the struct -> garbage,
                 * the `(Result _ E)` fmr returning 0 for 42).  Return the carrier
                 * value directly.  Gated on the flag because the legacy carrier
                 * path (flag-off) relies on the existing spill shape (range-*
                 * GADT fixtures). */
                buf_printf(file, "return (int64_t)(intptr_t)%s;\n", ret_val);
            } else {
                buf_printf(file,
                    "{ %s *__tur_ret_p = (%s *)malloc(sizeof(%s)); "
                    "*__tur_ret_p = %s; "
                    "return (int64_t)(intptr_t)__tur_ret_p; }\n",
                    struct_cty, struct_cty, struct_cty, ret_val);
            }
        } else if (fd->body->type.kind == TY_FN &&
                   (result_kind == TY_INT || ret_is_int64_carrier)) {
            /* A function-typed body returned through the int64_t closure
             * carrier: a bare non-capturing fn reference is a `void *(...)`
             * function pointer, and a fat box is declared `void *`, but the C
             * return type is the int64_t carrier.  Without the (int64_t)(intptr_t)
             * bridge, clang trips -Wint-conversion / -Wincompatible-function-
             * pointer-types on the implicit pointer-to-int conversion.  (A void*
             * carrier return needs no cast: the body value is already a pointer.) */
            buf_printf(file, "return (int64_t)(intptr_t)%s;\n", ret_val);
        } else if (use_abi_spec
                   && ctx->current_abi_specialization->typeclass_inst != NULL
                   && ret_ctype && strcmp(ret_ctype, "int64_t") != 0
                   && fd->body->type.kind != TY_NEVER
                   && type_uses_carrier_abi(emit_resolve_type(ctx, fd->body->type))
                   && strcmp(emit_type_c_name(ctx,
                                emit_resolve_type(ctx, fd->body->type)),
                             "int64_t") == 0
                   && !fn_body_tail_emits_byvalue_carrier_abi(ctx, fd->body)) {
            /* M4c Path A result-side: the spec's declared return is a
             * concrete by-value struct (e.g. `Result__int__cstr`), but the
             * body's last expression elaborates to a carrier-ABI value
             * (`int64_t` at C level) — e.g. `(ok v)` where ok still emits
             * its bare carrier symbol.  Route through emit_carrier_bridge so
             * the canonical-carrier field-by-field unbox kicks in for
             * Result/Option sinks with sub-word payloads
             * (decode-bool-carrier-instance-ascription); the legacy
             * `*(T *)(intptr_t)x` deref stays for other parametric sinks.
             *
             * instance-method-return-carrier-bridge: also skip the deref when
             * the body tail already emits the struct by value (a post-M2
             * #{Construct} spec like `(ok (make-struct ...))` lowers to its
             * by-value `*__spec__*` clone). */
            Type sink_rt = ctx->current_abi_specialization->result_type;
            char *bridged = emit_carrier_bridge(ctx, file, strdup(ret_val),
                                                CK_CARRIER, CK_CONCRETE, sink_rt);
            indent_buf(file, ctx->indent);
            buf_printf(file, "return %s;\n", bridged);
            free(bridged);
        } else if (!ret_is_int64_carrier && ret_ctype
                   && fd->body->type.kind != TY_NEVER
                   && (type_uses_carrier_abi(emit_resolve_type(ctx, fd->body->type))
                       || fn_body_tail_returns_carrier_value(ctx, fd->body))
                   && fn_body_tail_is_carrier_producer(fd->body)
                   && !fn_body_tail_emits_byvalue_carrier_abi(ctx, fd->body)) {
            /* M5 straddle (root cause C): an ordinary function or lifted lambda
             * whose declared return is a by-value carrier aggregate
             * (Option__int / Result__int__int) but whose tail value comes from
             * a carrier-int64 producer (some/ok/err/none or an __inst_ method).
             * Same carrier->concrete unbox as the M4c spec branch above. */
            Type sink_rt = (e->type.kind == TY_FN && e->type.as.fn.result_full_type)
                ? emit_resolve_type(ctx, *e->type.as.fn.result_full_type)
                : emit_resolve_type(ctx, fd->body->type);
            char *bridged = emit_carrier_bridge(ctx, file, strdup(ret_val),
                                                CK_CARRIER, CK_CONCRETE, sink_rt);
            indent_buf(file, ctx->indent);
            buf_printf(file, "return %s;\n", bridged);
            free(bridged);
        } else if (ret_is_int64_carrier && fd->body
                   && fd->body->type.kind != TY_NEVER
                   && fn_body_tail_emits_byvalue_carrier_abi(ctx, fd->body)
                   && fn_body_tail_byvalue_carrier_type(ctx, fd->body).kind != TY_UNKNOWN) {
            /* Phase 5 carrier-bridge deletion (concrete->carrier return): the C
             * return is the uniform int64 carrier (a lifted lambda thunk in a
             * poly_fn slot, or a generic carrier base) but the body tail now
             * produces a by-value Option/Result struct (a monomorphized
             * #{Construct} spec like `some__spec`).  Heap-spill the struct and
             * return its pointer as int64 -- the SAME malloc spill the
             * inst_method_carrier_spill path uses, so the carrier consumer
             * (which derefs a {is_some,value}/{is_ok,...} layout) reads it
             * correctly.  A stack spill (emit_carrier_bridge concrete->carrier)
             * would return a dangling address, so it is NOT used here.  The
             * concrete type comes from the matched spec, since the construct's
             * own e->type is collapsed to the int64 carrier. */
            Type src = fn_body_tail_byvalue_carrier_type(ctx, fd->body);
            if (type_is_heap_struct(src) || type_is_heap_adt(src)) {
                /* constrained-defn-monomorphize: a `:heap` struct tail (e.g.
                 * `(Cons A)` built by `tcons-of`) is ALREADY a pointer (`Cons__A *`)
                 * that fits the int64 carrier directly.  Malloc-boxing it
                 * double-boxes (the carrier consumer then reads a pointer-to-pointer
                 * as the cell -> a length-1 / garbage list), the heap-struct mirror
                 * of the M7 double-box guard above.  Cast the pointer to the carrier
                 * instead of spilling.  seam 3: a lowered `:heap` ADT tail
                 * (`type_is_heap_adt`) is the same typed-pointer shape. */
                buf_printf(file, "return (int64_t)(intptr_t)%s;\n", ret_val);
            } else {
                const char *cty = emit_type_c_name(ctx, src);
                buf_printf(file,
                    "{ %s *__tur_ret_p = (%s *)malloc(sizeof(%s)); "
                    "*__tur_ret_p = %s; "
                    "return (int64_t)(intptr_t)__tur_ret_p; }\n",
                    cty, cty, cty, ret_val);
            }
        } else if (ret_is_int64_carrier && fd->body &&
                   fd->body->type.kind != TY_NEVER &&
                   type_is_heap_adt(emit_resolve_type(ctx, fd->body->type))) {
            /* seam 3: the function returns the int64 carrier (an abstract
             * parametric base like `tcons : (Cons A)`), but the body tail is a
             * lowered `:heap` ADT value -- a typed pointer (`ctor_Cons(..)` ->
             * `tur_adt_Cons *`) whose bit pattern IS the carrier.  Reinterpret-cast
             * it, never return it uncast (-Wint-conversion) and never malloc-box
             * (double-box).  The non-parametric mirror is the `type_is_heap_adt`
             * arm in `type_uses_carrier_abi` / the heap-struct return cast above. */
            buf_printf(file, "return (int64_t)(intptr_t)%s;\n", ret_val);
        } else {
            buf_printf(file, "return %s;\n", ret_val);
        }
        free(ret_val);
    }

    /* Restore previous context */
    ctx->fn_params = saved_params;
    ctx->n_fn_params = saved_n_params;
    ctx->no_unwind = saved_no_unwind;  /* Phase R5 */
    ctx->n_pbp_params = saved_n_pbp;   /* Phase D */
    ctx->closure = saved_closure;
    free((void*)ctx->env_var_name);
    ctx->env_var_name = saved_env_var_name;

    ctx->indent -= 4;
    buf_printf(file, "}\n\n");
    if (!ctx->fn_name_override) free((void*)fn_name);

    /* Phase 19 (per-fiber): flush handlers accumulated during this body, then
     * commit the temp function buffer to the real file. */
    if (ctx->pending_handler_fns && ctx->pending_handler_fns->len > 0) {
        buf_write(real_file, ctx->pending_handler_fns->data, ctx->pending_handler_fns->len);
        buf_free(ctx->pending_handler_fns);
        buf_init(ctx->pending_handler_fns);
    }

    buf_write(real_file, fn_tmp.data, fn_tmp.len);
    buf_free(&fn_tmp);
    ctx->file = real_file;

    /* CPS3: emit __cps wrapper for colored non-closure functions when --cps-path */
    if (fd->is_cps && g_cps_path && !is_main && !fd->closure) {
        char *wrap_name = raw_name_for_binding(fd->binding);
        buf_printf(real_file, "static void %s__cps(tur_cps_cont_t *__k", wrap_name);
        for (uint8_t i = 0; i < fd->n_params; i++) {
            buf_puts(real_file, ", ");
            if (fd->params[i]->is_poly_fn) {
                buf_puts(real_file, "tur_poly_fn_t");
            } else if (fd->param_types[i].kind == TY_FN) {
                buf_puts(real_file, "int64_t");
            } else if (fd->params[i]->is_fat && body_is_inline_c) {
                /* fat-param-emitted-as-void-ptr-warns-in-inline-c.md: ^fat
                 * carrier handle -> int64_t (matches the direct signature).
                 * Inline-C bodies are never CPS, so this stays void* in
                 * practice; gated for parity with the direct signature. */
                buf_puts(real_file, "int64_t");
            } else {
                Type param_ty = (!fd->params[i]->is_fat &&
                                 e->type.as.fn.arg_full_types && e->type.as.fn.arg_full_types[i])
                    ? *e->type.as.fn.arg_full_types[i]
                    : fd->param_types[i];
                buf_puts(real_file, emit_type_c_name(ctx, param_ty));
            }
            const char *pn = raw_name_for_binding(fd->params[i]);
            buf_printf(real_file, " %s", pn);
            free((void*)pn);
        }
        buf_puts(real_file, ") {\n");
        /* Call the direct function and apply the continuation with the result.
         * nil-returning functions lower to `void` in C, so pass 0 to the
         * continuation instead of casting a void expression. */
        if (result_kind == TY_NIL) {
            buf_printf(real_file, "    %s(", wrap_name);
            for (uint8_t i = 0; i < fd->n_params; i++) {
                if (i > 0) buf_puts(real_file, ", ");
                const char *pn = raw_name_for_binding(fd->params[i]);
                buf_puts(real_file, pn);
                free((void*)pn);
            }
            buf_puts(real_file, ");\n");
            buf_puts(real_file, "    tur_cps_apply(__k, 0);\n");
        } else {
            buf_printf(real_file, "    int64_t __result = (int64_t)%s(", wrap_name);
            for (uint8_t i = 0; i < fd->n_params; i++) {
                if (i > 0) buf_puts(real_file, ", ");
                const char *pn = raw_name_for_binding(fd->params[i]);
                buf_puts(real_file, pn);
                free((void*)pn);
            }
            buf_puts(real_file, ");\n");
            buf_puts(real_file, "    tur_cps_apply(__k, __result);\n");
        }
        buf_puts(real_file, "}\n\n");
        free(wrap_name);
    }
}
