/* emit_fns.c -- function-definition C emission (emit_fn_def). */
#include "emit_internal.h"

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
 * default (non-pbp, non-poly) path. */
static Type tco_param_type(const Expr *fn_e, FnDef *fd, uint8_t i) {
    if (fn_e->type.kind == TY_FN && fn_e->type.as.fn.arg_full_types &&
        fn_e->type.as.fn.arg_full_types[i])
        return *fn_e->type.as.fn.arg_full_types[i];
    return fd->param_types[i];
}

/* A function is TCO-eligible only if every parameter is a plain scalar we can
 * reassign with `p = tmp;`.  Pass-by-pointer structs, poly-fn, fn-typed, and
 * carrier-ABI parameters are excluded so the backedge temporary's C type is
 * unambiguous. */
static bool tco_params_simple(EmitCtx *ctx, const Expr *fn_e, FnDef *fd) {
    if (fd->is_variadic || fd->closure) return false;
    for (uint8_t i = 0; i < fd->n_params; i++) {
        if (fd->params[i]->is_poly_fn) return false;
        Type pty = tco_param_type(fn_e, fd, i);
        if (pty.kind == TY_FN) return false;
        Type rpty = emit_resolve_type(ctx, pty);
        if (type_struct_pass_by_ptr(rpty)) return false;
        if (type_uses_carrier_abi(rpty)) return false;
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
    if (call->as.call_.dict_arg) return false;         /* typeclass dispatch */
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
        Type pty = tco_param_type(fn_e, fd, i);
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
    indent_buf(body, ctx->indent);
    if (e->type.kind == TY_NIL) {
        free(v);
        v = strdup(result_kind == TY_BOOL ? "false" : "0");
    }
    if (is_main && result_kind == TY_INT)
        buf_printf(body, "return (int)%s;\n", v);
    else
        buf_printf(body, "return %s;\n", v);
    free(v);
}

/* ------------ Phase 2: function emission ------------ */

void emit_fn_def(EmitCtx *ctx, Buf *file, const Expr *e) {
    FnDef *fd = e->as.fn_def_.fn;
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

    /* Phase 3: Emit env struct for closure thunks */
    if (fd->closure) {
        const Symbol *env_name = fd->closure->env_name;
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
            Type *thunk_params = fd->n_params > 1 ? &fd->param_types[1] : NULL;
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
                const char *cap_ctype = captured->is_poly_fn
                                          ? "tur_poly_fn_t"
                                          : type_c_name(captured->type);
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
    bool needs_static = !is_main &&
        !ctx->fn_name_override_external &&
        !(ctx->separate_compilation && fd->binding->is_exported);
    if (needs_static) {
        buf_printf(file, "static ");
    }
    /* Return type from fn_type */
    if (e->type.kind == TY_FN) {
        TypeKind result = e->type.as.fn.result_kind;
        if (is_main) {
            buf_puts(file, "int");  /* C main must always return int */
        } else if (use_abi_spec) {
            buf_puts(file, emit_type_c_name(ctx, ctx->current_abi_specialization->result_type));
        } else if (e->type.as.fn.result_full_type) {
            bool body_is_inline_c = (fd->body && fd->body->kind == EX_INLINE_C);
            if (!body_is_inline_c) {
                buf_puts(file, emit_type_c_name(ctx, *e->type.as.fn.result_full_type));
            } else {
                buf_puts(file, "int64_t");
            }
        } else {
            buf_puts(file, emit_type_c_name(ctx, emit_type_from_kind(result)));
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
    for (uint8_t i = 0; i < fd->n_params; i++) {
        if (i > 0) buf_puts(file, ", ");
        /* Phase HRT1: poly fn params use tur_poly_fn_t in signature */
        if (fd->params[i]->is_poly_fn) {
            buf_puts(file, "tur_poly_fn_t");
        } else if (fd->param_types[i].kind == TY_FN) {
            /* ER4: function-typed parameters are passed as int64_t (opaque
             * function pointer) in Turmeric's calling convention. */
            buf_puts(file, "int64_t");
        } else if (use_abi_spec) {
            const char *pc = emit_type_c_name(ctx, ctx->current_abi_specialization->arg_types[i]);
            buf_puts(file, pc);
            /* KB-021: a concrete carrier-ABI spec param is passed by value. */
            if (fd->params[i] &&
                type_uses_carrier_abi(emit_resolve_type(ctx, ctx->current_abi_specialization->arg_types[i])) &&
                strcmp(pc, "int64_t") != 0)
                fd->params[i]->emit_byvalue_carrier_abi = true;
        } else {
            /* Phase D: large structs use const T* ABI unless in inline-C or closure. */
            Type param_ty = (e->type.as.fn.arg_full_types && e->type.as.fn.arg_full_types[i])
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
        buf_printf(file, " %s", pn);
        free((void*)pn);
    }
    buf_puts(file, ") {\n");

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
            char env_var_name_buf[64];
            snprintf(env_var_name_buf, sizeof(env_var_name_buf), "__env_%s", fd->closure->env_name->name);
            buf_printf(file, "struct %s *%s = (struct %s *)%s;\n",
                       fd->closure->env_name->name,
                       env_var_name_buf,
                       fd->closure->env_name->name,
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
     * Excludes main and ABI-specialization clones (distinct parameter ABI). */
    bool tco_eligible = !body_diverges && fd->body->kind != EX_INLINE_C &&
        !(result_kind == TY_NIL && !is_main) && !is_main && !use_abi_spec &&
        tco_params_simple(ctx, e, fd) && tco_mark(ctx, fd, fn_name, fd->body) > 0;

    if (body_diverges) {
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
        indent_buf(file, ctx->indent);
        /* Special case: if this is main and it returns int64_t, cast to int */
        if (is_main && result_kind == TY_INT) {
            buf_printf(file, "return (int)%s;\n", ret_val);
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
}

