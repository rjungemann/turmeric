/* emit_fns.c -- function-definition C emission (emit_fn_def). */
#include "emit_internal.h"

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
                buf_printf(file, "%s %s; ",
                           type_c_name(captured->type), captured->name->name);
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
     * linkage (no static) so they can be called from other compilation units. */
    bool needs_static = !is_main &&
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

    /* Emit parameters - use raw names (without ID suffix) */
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
            buf_puts(file, emit_type_c_name(ctx, ctx->current_abi_specialization->arg_types[i]));
        } else if (e->type.as.fn.arg_full_types && e->type.as.fn.arg_full_types[i]) {
            buf_puts(file, emit_type_c_name(ctx, *e->type.as.fn.arg_full_types[i]));
        } else {
            buf_puts(file, emit_type_c_name(ctx, fd->param_types[i]));
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

    if (body_diverges) {
        /* Body diverges on every path - emit as statements only */
        emit_stmt(ctx, file, fd->body);
    } else if (fd->body->kind == EX_INLINE_C) {
        /* Inline C body - emit as-is (it contains its own return statements) */
        emit_stmt(ctx, file, fd->body);
    } else if (result_kind == TY_NIL && !is_main) {
        /* void function - emit body as statements */
        emit_stmt(ctx, file, fd->body);
    } else {
        /* Function with return value */
        char *ret_val = emit_value(ctx, file, fd->body);
        indent_buf(file, ctx->indent);
        /* If the body returns nil but the function expects a non-nil type,
         * emit a default value. This can happen with e.g. (defn main [] :int (println 42)) */
        if (fd->body->type.kind == TY_NIL) {
            /* Body is nil-typed, but function expects a return value.
             * Emit default based on return type. */
            free(ret_val);
            switch (result_kind) {
                case TY_INT:   ret_val = strdup("0"); break;
                case TY_BOOL:  ret_val = strdup("false"); break;
                default:       ret_val = strdup("0"); break;
            }
        }
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

