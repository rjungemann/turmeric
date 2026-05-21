/* emit_effects.c -- expression-position emission for algebraic effects and
 * CPS (shift/reset) constructs.  Covers the three regions of emit_value that
 * were extracted from emit_expr.c (EE1--EE3):
 *
 *   Region C  EX_DEFECT / EX_PERFORM / EX_HANDLE / EX_RESUME / EX_DISCONTINUE
 *   Region A  EX_RESET / EX_CLONEABLE_RESET / EX_SHIFT / EX_SHIFT0 /
 *             EX_CLONEABLE_SHIFT / EX_SERIAL_RESET / EX_SERIAL_SHIFT
 *   Region B  EX_CONT_PRED
 *
 * The emit_program runtime prelude fragments (fiber struct fields,
 * global_effect_handler_chain, tur_effect_perform definition) remain in
 * emit_module.c -- see emit-effects-extraction-plan.md §EE4 for rationale.
 */
#include "emit_internal.h"

/* =========================================================================
 * Region C -- algebraic effects
 * ========================================================================= */

char *emit_effects_defect(EmitCtx *ctx, Buf *body, const Expr *e) {
    (void)ctx; (void)body; (void)e;
    /* Effect definitions are compile-time only - no runtime code */
    return atom_nil();
}

char *emit_effects_perform(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (perform (EffectName args...)) - perform an effect.
     * Emit: tur_effect_perform("Name", args_array, n_args)
     */
    PerformExpr *perf = e->as.perform_.perform;
    if (!perf) return atom_nil();

    bool nil_result = (e->type.kind == TY_NIL);

    /* Emit arguments into a temporary array */
    char *args_var_str = NULL;
    if (perf->n_args > 0) {
        args_var_str = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        buf_printf(body, "int64_t %s[%d];\n", args_var_str, perf->n_args);
        for (uint8_t i = 0; i < perf->n_args; i++) {
            char *av = emit_value(ctx, body, perf->args[i]);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s[%d] = (int64_t)%s;\n", args_var_str, i, av);
            free(av);
        }
    }
    const char *args_arg  = args_var_str ? args_var_str : "NULL";
    int         n_args    = perf->n_args;

    if (nil_result) {
        /* Void-result effect: emit as statement, return nil placeholder */
        indent_buf(body, ctx->indent);
        buf_printf(body, "tur_effect_perform(\"%s\", %s, %d);\n",
                   perf->effect_name->name, args_arg, n_args);
        free(args_var_str);
        return atom_nil();
    } else {
        char *result = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        const char *res_ctype = type_c_name(e->type);
        bool needs_cast = (e->type.kind != TY_INT && e->type.kind != TY_UNKNOWN);
        if (needs_cast) {
            buf_printf(body, "%s %s = (%s)tur_effect_perform(\"%s\", %s, %d);\n",
                       res_ctype, result, res_ctype,
                       perf->effect_name->name, args_arg, n_args);
        } else {
            buf_printf(body, "int64_t %s = tur_effect_perform(\"%s\", %s, %d);\n",
                       result, perf->effect_name->name, args_arg, n_args);
        }
        free(args_var_str);
        return result;
    }
}

char *emit_effects_handle(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (handle body (E [params] k) handler ...)
     *
     * Phase 19D: Deep-handler continuation capture semantics.
     * The body runs in a new fiber. When an effect is performed,
     * the fiber yields to the parent dispatch loop, which calls
     * the matching handler case function with k = (int64_t)fiber.
     * (resume k v) resumes the fiber via tur_effect_cont_resume.
     * Handler case functions receive outer captures via __env.
     */
    HandleExpr *h = e->as.handle_.handle;
    bool returns_value = (e->type.kind != TY_NIL);

    /* Allocate unique IDs for this handle expression */
    int handle_id = ctx->tmp_n++;

    /* ----------------------------------------------------------------
     * Step 1: Collect outer variables captured by handler case bodies.
     * ---------------------------------------------------------------- */
    /* Collect captures for each case body plus the handle body */
    Binding **all_caps = NULL;
    uint32_t n_all_caps = 0, c_all_caps = 0;

    /* Collect captures from handle body (for body fiber function) */
    uint32_t n_body_caps = 0;
    Binding **body_caps = collect_handle_captures(h->body, &n_body_caps);
    for (uint32_t ci = 0; ci < n_body_caps; ci++) {
        bool already = false;
        for (uint32_t ai = 0; ai < n_all_caps; ai++)
            if (all_caps[ai] == body_caps[ci]) { already = true; break; }
        if (!already) {
            if (n_all_caps >= c_all_caps) {
                c_all_caps = (c_all_caps == 0) ? 8 : c_all_caps * 2;
                all_caps = (Binding **)realloc(all_caps, c_all_caps * sizeof(Binding *));
            }
            all_caps[n_all_caps++] = body_caps[ci];
        }
    }
    free(body_caps);

    /* Collect captures from handler case bodies (they use outer vars + k + effect params) */
    /* Handler case bodies are emitted as separate static fns; outer vars are captured via __env */
    Binding ***case_caps = (Binding ***)malloc(h->n_cases * sizeof(Binding **));
    uint32_t *case_ncaps = (uint32_t *)malloc(h->n_cases * sizeof(uint32_t));
    for (uint8_t ci = 0; ci < h->n_cases; ci++) {
        HandleCase *c = &h->cases[ci];
        case_caps[ci] = collect_handle_captures(c->body, &case_ncaps[ci]);
        /* Filter out k_binding and param_bindings (those are local to the handler fn) */
        uint32_t filtered = 0;
        for (uint32_t j = 0; j < case_ncaps[ci]; j++) {
            Binding *b = case_caps[ci][j];
            bool is_local = false;
            if (c->k_binding && b == c->k_binding) is_local = true;
            for (uint8_t p = 0; p < c->n_params && !is_local; p++)
                if (c->param_bindings && c->param_bindings[p] == b) is_local = true;
            if (!is_local) case_caps[ci][filtered++] = b;
        }
        case_ncaps[ci] = filtered;
        /* Add to all_caps */
        for (uint32_t j = 0; j < case_ncaps[ci]; j++) {
            bool already = false;
            for (uint32_t ai = 0; ai < n_all_caps; ai++)
                if (all_caps[ai] == case_caps[ci][j]) { already = true; break; }
            if (!already) {
                if (n_all_caps >= c_all_caps) {
                    c_all_caps = (c_all_caps == 0) ? 8 : c_all_caps * 2;
                    all_caps = (Binding **)realloc(all_caps, c_all_caps * sizeof(Binding *));
                }
                all_caps[n_all_caps++] = case_caps[ci][j];
            }
        }
    }

    bool has_captures = (n_all_caps > 0);

    /* ----------------------------------------------------------------
     * Step 2: Emit env struct type (if captures exist).
     * ---------------------------------------------------------------- */
    char env_type_name[64];
    snprintf(env_type_name, sizeof(env_type_name), "__HEnv_%d", handle_id);
    char env_var_name[64];
    snprintf(env_var_name, sizeof(env_var_name), "__henv_%d", handle_id);

    Buf *hbuf = ctx->pending_handler_fns;

    if (has_captures) {
        buf_printf(hbuf, "typedef struct %s %s;\n", env_type_name, env_type_name);
        buf_printf(hbuf, "struct %s {\n", env_type_name);
        for (uint32_t ci = 0; ci < n_all_caps; ci++) {
            Binding *b = all_caps[ci];
            char *raw = raw_name_for_binding(b);
            /* ET2: poly fn params (TY_FORALL) must be stored as tur_poly_fn_t */
            const char *field_ctype = b->is_poly_fn ? "tur_poly_fn_t"
                                                    : type_c_name(b->type);
            buf_printf(hbuf, "    %s %s;\n", field_ctype, raw);
            free(raw);
        }
        buf_printf(hbuf, "};\n\n");
    }

    /* ----------------------------------------------------------------
     * Step 3: Allocate names for handler functions, body fn, dispatch fn.
     * ---------------------------------------------------------------- */
    char **hfn_names = (char **)malloc(h->n_cases * sizeof(char *));
    for (uint8_t i = 0; i < h->n_cases; i++) {
        hfn_names[i] = (char *)malloc(48);
        snprintf(hfn_names[i], 48, "__effect_handler_%d", ctx->tmp_n++);
    }
    char body_fn_name[64];
    snprintf(body_fn_name, sizeof(body_fn_name), "__handle_body_%d", handle_id);
    char dispatch_fn_name[64];
    snprintf(dispatch_fn_name, sizeof(dispatch_fn_name), "__dispatch_%d", handle_id);

    /* ----------------------------------------------------------------
     * Step 4: Emit handler case functions (static, file-scope).
     * Handler receives outer captures via __env (cast to HEnv*).
     * ---------------------------------------------------------------- */
    for (uint8_t i = 0; i < h->n_cases; i++) {
        HandleCase *c = &h->cases[i];

        /* Emit handler body with handle_captures context for outer var access.
         * Use temp buffers so nested handles emit their static fns at file scope
         * (before this function), not inside it. Pattern mirrors emit_fn_def. */
        {
            Buf hfn_buf; buf_init(&hfn_buf);
            Buf hfn_pending; buf_init(&hfn_pending);

            /* Cast __env to typed env pointer if we have captures */
            if (has_captures) {
                buf_printf(&hfn_buf,
                    "    %s *%s = (%s *)__env;\n",
                    env_type_name, env_var_name, env_type_name);
            }

            /* Unpack effect arguments into named locals */
            for (uint8_t j = 0; j < c->n_params; j++) {
                if (c->param_bindings && c->param_bindings[j]) {
                    const char *ctype = type_c_name(c->param_bindings[j]->type);
                    char *raw = raw_name_for_binding(c->param_bindings[j]);
                    buf_printf(&hfn_buf,
                        "    %s %s_%u = (%s)__effect_args[%d];\n",
                        ctype, raw, (unsigned)c->param_bindings[j]->id, ctype, j);
                    free(raw);
                }
            }

            /* Bind k - now it's a fiber pointer cast to int64_t */
            if (c->k_binding) {
                char *raw = raw_name_for_binding(c->k_binding);
                buf_printf(&hfn_buf,
                    "    int64_t %s_%u = __k;\n",
                    raw, (unsigned)c->k_binding->id);
                free(raw);
            }

            EmitCtx hctx = *ctx;
            hctx.file = &hfn_buf;
            hctx.pending_handler_fns = &hfn_pending;
            hctx.indent = 4;
            hctx.fn_params = NULL;
            hctx.n_fn_params = 0;
            hctx.closure = NULL;
            hctx.env_var_name = has_captures ? env_var_name : NULL;
            hctx.defer_captures = NULL;
            hctx.n_defer_captures = 0;
            hctx.frame_var = NULL;
            hctx.return_emitted = false;
            hctx.handle_captures = has_captures ? all_caps : NULL;
            hctx.n_handle_captures = has_captures ? n_all_caps : 0;
            hctx.handle_env_name = has_captures ? env_var_name : NULL;

            /* Emit handler case body.
             * Phase 19D: for non-never, non-nil bodies, or nil bodies ending in
             * fiber-resume, use emit_value so the fiber result is returned.
             * For never-typed or nil bodies that don't resume, use emit_stmt + return 0. */
            if (!c->body) {
                buf_puts(&hfn_buf, "    return 0;\n");
            } else if (c->body->type.kind == TY_NEVER) {
                /* Never-returning body: emit as stmt (panic, discontinue, etc.) */
                emit_stmt(&hctx, &hfn_buf, c->body);
                buf_puts(&hfn_buf, "    return 0; /* unreachable */\n");
            } else {
                char *hret = emit_value(&hctx, &hfn_buf, c->body);
                /* Check if the value is nil placeholder (atom_nil = ((void)0)) */
                bool is_nil_placeholder = (strcmp(hret, "((void)0)") == 0);
                if (is_nil_placeholder) {
                    /* Body is nil-typed and doesn't produce a fiber result */
                    free(hret);
                    buf_puts(&hfn_buf, "    return 0;\n");
                } else {
                    buf_printf(&hfn_buf, "    return (int64_t)%s;\n", hret);
                    free(hret);
                }
            }

            ctx->tmp_n = hctx.tmp_n;

            /* Flush inner pending fns BEFORE this handler function */
            if (hfn_pending.len > 0) {
                buf_write(hbuf, hfn_pending.data, hfn_pending.len);
            }
            buf_free(&hfn_pending);

            /* Now emit forward decl + function definition */
            buf_printf(hbuf,
                "static int64_t %s(int64_t *__effect_args, int __n_effect_args,"
                " int64_t __k, void *__env);\n", hfn_names[i]);
            buf_printf(hbuf,
                "static int64_t %s(int64_t *__effect_args, int __n_effect_args,"
                " int64_t __k, void *__env) {\n", hfn_names[i]);
            buf_write(hbuf, hfn_buf.data, hfn_buf.len);
            buf_puts(hbuf, "}\n\n");
            buf_free(&hfn_buf);
        }
    }

    /* ----------------------------------------------------------------
     * Step 5: Emit handle body fiber function.
     * This is called as the fiber's entry_fn. It accesses the env
     * via tur_current_fiber->eff_ctx->body_env.
     *
     * Use a temporary buffer + nested pending_hfns so that inner
     * handles write their static functions BEFORE this function
     * (not inside it). Pattern mirrors emit_fn_def.
     * ---------------------------------------------------------------- */
    {
        Buf fn_buf; buf_init(&fn_buf);
        Buf inner_pending; buf_init(&inner_pending);

        if (has_captures) {
            buf_printf(&fn_buf,
                "    TurEffectCaptureCtx *__cap = (TurEffectCaptureCtx *)tur_current_fiber->eff_ctx;\n");
            buf_printf(&fn_buf,
                "    %s *%s = (%s *)__cap->body_env;\n",
                env_type_name, env_var_name, env_type_name);
        }

        EmitCtx bctx = *ctx;
        bctx.file = &fn_buf;
        bctx.pending_handler_fns = &inner_pending;
        bctx.indent = 4;
        bctx.fn_params = NULL;
        bctx.n_fn_params = 0;
        bctx.closure = NULL;
        bctx.env_var_name = has_captures ? env_var_name : NULL;
        bctx.defer_captures = NULL;
        bctx.n_defer_captures = 0;
        bctx.frame_var = NULL;
        bctx.return_emitted = false;
        bctx.handle_captures = has_captures ? all_caps : NULL;
        bctx.n_handle_captures = has_captures ? n_all_caps : 0;
        bctx.handle_env_name = has_captures ? env_var_name : NULL;

        if (h->body->type.kind == TY_NIL || h->body->type.kind == TY_NEVER) {
            emit_stmt(&bctx, &fn_buf, h->body);
            buf_puts(&fn_buf, "    tur_current_fiber->result = 0;\n");
        } else {
            char *bret = emit_value(&bctx, &fn_buf, h->body);
            buf_printf(&fn_buf, "    tur_current_fiber->result = (int64_t)%s;\n", bret);
            free(bret);
        }

        ctx->tmp_n = bctx.tmp_n;

        /* Emit defer thunks registered during handle body emission.
         * They were queued on bctx but not yet flushed. Emit them to
         * inner_pending so they appear before the body function in hbuf. */
        if (bctx.pending_defer_thunks != ctx->pending_defer_thunks) {
            /* Extract only the NEW thunks (those prepended during body emit) */
            DeferThunk *new_thunks = bctx.pending_defer_thunks;
            if (new_thunks) {
                DeferThunk *p = new_thunks;
                while (p->next != ctx->pending_defer_thunks) p = p->next;
                p->next = NULL;  /* terminate new-thunks list */
                /* Use bctx so name_for_binding resolves handle captures correctly */
                EmitCtx thunk_ctx = bctx;
                thunk_ctx.pending_defer_thunks = new_thunks;
                emit_pending_defer_thunks(&thunk_ctx, &inner_pending);
                /* emit_pending_defer_thunks freed the new thunks; ctx unchanged */
            }
        }

        /* Flush nested pending handler fns before the body function */
        if (inner_pending.len > 0) {
            buf_write(hbuf, inner_pending.data, inner_pending.len);
        }
        buf_free(&inner_pending);

        /* Forward declaration */
        buf_printf(hbuf, "static void %s(void);\n", body_fn_name);
        /* Function definition */
        buf_printf(hbuf, "static void %s(void) {\n", body_fn_name);
        /* Emit body content */
        buf_write(hbuf, fn_buf.data, fn_buf.len);
        buf_puts(hbuf, "}\n\n");
        buf_free(&fn_buf);
    }

    /* ----------------------------------------------------------------
     * Step 6: Emit dispatch function.
     * ---------------------------------------------------------------- */
    buf_printf(hbuf, "static int64_t %s(void *__ctx_void, int64_t __k_int, int64_t __resume_val);\n",
               dispatch_fn_name);

    /* MS1: Emit multishot wrapper helpers (env struct + cont_fn + clone_fn).
     * These are emitted after the dispatch fn forward declaration so they can
     * call dispatch_fn_name, and before the dispatch fn body that calls them. */
    for (uint8_t i = 0; i < h->n_cases; i++) {
        HandleCase *ms_c = &h->cases[i];
        if (ms_c->cont_kind != CK_MULTISHOT) continue;
        /* Env struct: captures the dispatch context and fiber pointer */
        buf_printf(hbuf, "struct __ms_env_%d_%d { void *__ctx; int64_t __k_int; };\n",
                   handle_id, (int)i);
        /* Continuation function: calls dispatch_fn_name(ctx, k_int, v) */
        buf_printf(hbuf,
            "static int64_t __ms_cont_fn_%d_%d(void *__env, int64_t __v) {\n",
            handle_id, (int)i);
        buf_printf(hbuf,
            "    struct __ms_env_%d_%d *__e = (struct __ms_env_%d_%d *)__env;\n",
            handle_id, (int)i, handle_id, (int)i);
        buf_printf(hbuf,
            "    return %s(__e->__ctx, __e->__k_int, __v);\n", dispatch_fn_name);
        buf_puts(hbuf, "}\n");
        /* Clone function: shallow copy of env (ctx and k_int are both plain pointers) */
        buf_printf(hbuf,
            "static void *__ms_env_clone_%d_%d(const void *__env) {\n",
            handle_id, (int)i);
        buf_printf(hbuf,
            "    struct __ms_env_%d_%d *__orig = (struct __ms_env_%d_%d *)__env;\n",
            handle_id, (int)i, handle_id, (int)i);
        buf_printf(hbuf,
            "    struct __ms_env_%d_%d *__copy = (struct __ms_env_%d_%d *)malloc(sizeof(struct __ms_env_%d_%d));\n",
            handle_id, (int)i, handle_id, (int)i, handle_id, (int)i);
        buf_puts(hbuf, "    if (__copy) *__copy = *__orig;\n");
        buf_puts(hbuf, "    return __copy;\n");
        buf_puts(hbuf, "}\n");
    }

    buf_printf(hbuf, "static int64_t %s(void *__ctx_void, int64_t __k_int, int64_t __resume_val) {\n",
               dispatch_fn_name);
    buf_puts(hbuf, "    TurEffectCaptureCtx *__dcap = (TurEffectCaptureCtx *)__ctx_void;\n");
    buf_puts(hbuf, "    FiberBlock *__fiber = (FiberBlock *)(intptr_t)__k_int;\n");
    buf_puts(hbuf, "    int64_t __r = tur_fiber_block_resume(__fiber, __resume_val);\n");
    buf_puts(hbuf, "    if (__fiber->done) { return __fiber->result; }\n");
    buf_puts(hbuf, "    if (!__dcap->has_pending_effect) return __r;\n");
    /* Dispatch to matching handler case */
    for (uint8_t i = 0; i < h->n_cases; i++) {
        HandleCase *c = &h->cases[i];
        if (i == 0)
            buf_printf(hbuf, "    if (strcmp(__dcap->eff_name, \"%s\") == 0) {\n",
                       c->effect_name->name);
        else
            buf_printf(hbuf, "    } else if (strcmp(__dcap->eff_name, \"%s\") == 0) {\n",
                       c->effect_name->name);
        if (c->cont_kind == CK_MULTISHOT) {
            /* MS1: wrap fiber k in a tur_cloneable_cont for snapshot-safe multi-shot. */
            buf_printf(hbuf,
                "        struct __ms_env_%d_%d *__ms_e = (struct __ms_env_%d_%d *)malloc(sizeof(struct __ms_env_%d_%d));\n",
                handle_id, (int)i, handle_id, (int)i, handle_id, (int)i);
            buf_puts(hbuf, "        if (!__ms_e) abort();\n");
            buf_printf(hbuf, "        __ms_e->__ctx = __ctx_void; __ms_e->__k_int = __k_int;\n");
            buf_printf(hbuf,
                "        int64_t __k_ms = (int64_t)(intptr_t)tur_cloneable_cont_alloc(__ms_cont_fn_%d_%d, __ms_e, __ms_env_clone_%d_%d, free);\n",
                handle_id, (int)i, handle_id, (int)i);
            if (has_captures)
                buf_printf(hbuf,
                    "        return %s(__dcap->eff_args, __dcap->eff_n_args, __k_ms, __dcap->body_env);\n",
                    hfn_names[i]);
            else
                buf_printf(hbuf,
                    "        return %s(__dcap->eff_args, __dcap->eff_n_args, __k_ms, NULL);\n",
                    hfn_names[i]);
        } else if (has_captures)
            buf_printf(hbuf,
                "        return %s(__dcap->eff_args, __dcap->eff_n_args, __k_int, __dcap->body_env);\n",
                hfn_names[i]);
        else
            buf_printf(hbuf,
                "        return %s(__dcap->eff_args, __dcap->eff_n_args, __k_int, NULL);\n",
                hfn_names[i]);
    }
    if (h->n_cases > 0) {
        buf_puts(hbuf, "    } else {\n");
        /* Phase 19D: bubble unhandled effect to outer fiber if possible */
        buf_puts(hbuf, "        /* Phase 19D: bubble up unhandled effect to outer fiber */\n");
        buf_puts(hbuf, "        FiberBlock *__outer_f = tur_current_fiber;\n");
        buf_puts(hbuf, "        if (__outer_f && __outer_f->eff_ctx) {\n");
        buf_puts(hbuf, "            TurEffectCaptureCtx *__outer_cap = (TurEffectCaptureCtx *)__outer_f->eff_ctx;\n");
        buf_puts(hbuf, "            __outer_cap->eff_name = __dcap->eff_name;\n");
        buf_puts(hbuf, "            int __bun = __dcap->eff_n_args < 8 ? __dcap->eff_n_args : 8;\n");
        buf_puts(hbuf, "            for (int __bi = 0; __bi < __bun; __bi++) __outer_cap->eff_args[__bi] = __dcap->eff_args[__bi];\n");
        buf_puts(hbuf, "            __outer_cap->eff_n_args = __dcap->eff_n_args;\n");
        buf_puts(hbuf, "            __outer_cap->has_pending_effect = true;\n");
        buf_puts(hbuf, "            tur_fiber_block_yield(0);\n");
        buf_puts(hbuf, "            __outer_cap->has_pending_effect = false;\n");
        /* Recursively dispatch inner fiber with the value the outer handler provided */
        buf_printf(hbuf, "            return %s(__ctx_void, __k_int, __outer_f->arg);\n",
                   dispatch_fn_name);
        buf_puts(hbuf, "        }\n");
        buf_puts(hbuf, "        fprintf(stderr, \"dispatch: unhandled effect: %s\\n\", __dcap->eff_name);\n");
        buf_puts(hbuf, "        abort();\n");
        buf_puts(hbuf, "    }\n");
    }
    buf_puts(hbuf, "    return 0;\n");
    buf_puts(hbuf, "}\n\n");

    /* ----------------------------------------------------------------
     * Step 7: Inline code -- set up fiber and run dispatch.
     * ---------------------------------------------------------------- */
    char *result = fresh_tmp(ctx);

    /* Alloc env struct if needed */
    if (has_captures) {
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s *%s = (%s *)malloc(sizeof(%s));\n",
                   env_type_name, env_var_name, env_type_name, env_type_name);
        /* Populate env fields */
        for (uint32_t ci = 0; ci < n_all_caps; ci++) {
            Binding *b = all_caps[ci];
            char *raw = raw_name_for_binding(b);
            char *cur_name = name_for_binding(ctx, b);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s->%s = %s;\n", env_var_name, raw, cur_name);
            free(raw);
            free(cur_name);
        }
    }

    /* Alloc TurEffectCaptureCtx */
    char cap_var[64];
    snprintf(cap_var, sizeof(cap_var), "__cap_%d", handle_id);
    indent_buf(body, ctx->indent);
    buf_printf(body, "TurEffectCaptureCtx %s;\n", cap_var);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.has_pending_effect = false;\n", cap_var);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.eff_name = NULL;\n", cap_var);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.eff_n_args = 0;\n", cap_var);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.dispatch = %s;\n", cap_var, dispatch_fn_name);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.body_env = %s;\n", cap_var,
               has_captures ? env_var_name : "NULL");

    /* Create the body fiber */
    char fiber_var[64];
    snprintf(fiber_var, sizeof(fiber_var), "__fiber_%d", handle_id);
    indent_buf(body, ctx->indent);
    buf_printf(body, "FiberBlock *%s = tur_fiber_block_new(%s, 0);\n",
               fiber_var, body_fn_name);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s->eff_ctx = &%s;\n", fiber_var, cap_var);

    /* Set up intercept frame (handler_fn == NULL means fiber-intercept) */
    char frame_var[64];
    snprintf(frame_var, sizeof(frame_var), "__eff_frame_%d", handle_id);
    char chain_var[64];
    snprintf(chain_var, sizeof(chain_var), "__eff_chain_%d", handle_id);

    /* The intercept frame must be installed in the FIBER's effect handler chain,
     * not the current chain.  We install it before starting the fiber by setting
     * fiber->effect_handler_chain directly.
     * parent is set to the current chain so effects not handled here can bubble up. */
    char parent_chain_var[64];
    snprintf(parent_chain_var, sizeof(parent_chain_var), "__parent_chain_%d", handle_id);
    indent_buf(body, ctx->indent);
    buf_printf(body,
        "EffectHandlerFrame *%s = (tur_current_fiber"
        " ? (EffectHandlerFrame *)tur_current_fiber->effect_handler_chain"
        " : global_effect_handler_chain);\n", parent_chain_var);
    indent_buf(body, ctx->indent);
    buf_printf(body, "EffectHandlerFrame %s;\n", frame_var);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.parent = %s;\n", frame_var, parent_chain_var);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.n_cases = %d;\n", frame_var, h->n_cases);
    for (uint8_t i = 0; i < h->n_cases; i++) {
        HandleCase *c = &h->cases[i];
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s.cases[%d].effect_name = \"%s\";\n",
                   frame_var, i, c->effect_name->name);
        indent_buf(body, ctx->indent);
        /* NULL handler_fn = intercept case (fiber yields to dispatch loop) */
        buf_printf(body, "%s.cases[%d].handler_fn = NULL;\n", frame_var, i);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s.cases[%d].env = NULL;\n", frame_var, i);
    }
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s->effect_handler_chain = &%s;\n", fiber_var, frame_var);

    /* Start the dispatch loop -- first call resumes with 0 to start body */
    indent_buf(body, ctx->indent);
    if (returns_value) {
        buf_printf(body, "%s %s = (%s)%s(&%s, (int64_t)(intptr_t)%s, 0);\n",
                   type_c_name(e->type), result, type_c_name(e->type),
                   dispatch_fn_name, cap_var, fiber_var);
    } else {
        buf_printf(body, "%s(&%s, (int64_t)(intptr_t)%s, 0);\n",
                   dispatch_fn_name, cap_var, fiber_var);
    }

    /* Phase 19D: Write env fields back to outer locals (so mutations in handler
     * are visible in the outer scope after dispatch). Must be done BEFORE freeing
     * the env, so do this unconditionally while env is still valid. */
    if (has_captures) {
        indent_buf(body, ctx->indent);
        buf_puts(body, "/* Phase 19D: write-back env captures to outer locals */\n");
        for (uint32_t ci = 0; ci < n_all_caps; ci++) {
            Binding *b = all_caps[ci];
            char *raw = raw_name_for_binding(b);
            char *cur_name = name_for_binding(ctx, b);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s = %s->%s;\n", cur_name, env_var_name, raw);
            free(raw);
            free(cur_name);
        }
    }

    /* If fiber is done, free it.  If not (k was stored), it lives on. */
    indent_buf(body, ctx->indent);
    buf_printf(body, "if (%s->done) { free(%s->stack); free(%s); }\n",
               fiber_var, fiber_var, fiber_var);

    /* Free env if we allocated it and fiber is done (for immediate-resume case).
     * If fiber is NOT done (k stored), leak env for v1 -- it must stay alive
     * as long as the fiber might be resumed. */
    if (has_captures) {
        indent_buf(body, ctx->indent);
        buf_printf(body, "if (%s->done) { free(%s); }\n",
                   fiber_var, env_var_name);
    }

    /* Cleanup */
    for (uint8_t i = 0; i < h->n_cases; i++) free(hfn_names[i]);
    free(hfn_names);
    for (uint8_t i = 0; i < h->n_cases; i++) free(case_caps[i]);
    free(case_caps);
    free(case_ncaps);
    free(all_caps);

    (void)chain_var;  /* named but unused; kept for symmetry with frame_var */

    if (returns_value) return result;
    free(result);
    return atom_nil();
}

char *emit_effects_resume(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (resume k value) - resume continuation with value.
     *
     * Phase 19D: k is a FiberBlock* cast to int64_t.
     * tur_effect_cont_resume(k, v) resumes the fiber via the dispatch fn.
     *
     * MS1: CK_MULTISHOT k is a tur_cloneable_cont* (int64_t).
     * tur_cloneable_cont_resume(tur_continuation_snapshot(k), v) clones before each resume.
     *
     * For Phase 18 tur_cont* continuations (TY_CONT), use the old path.
     */
    if (e->as.resume_.resume) {
        char *k_var = emit_value(ctx, body, e->as.resume_.resume->k);
        char *v_var = emit_value(ctx, body, e->as.resume_.resume->value);
        Type k_type = e->as.resume_.resume->k->type;
        if (k_type.copy_kind == CK_MULTISHOT) {
            /* MS1: ^multishot k -- snapshot before each resume so k stays usable. */
            char *tmp = fresh_tmp(ctx);
            Type vtype = e->as.resume_.resume->value->type;
            bool v_is_nil = (vtype.kind == TY_NIL || vtype.kind == TY_NEVER);
            indent_buf(body, ctx->indent);
            if (v_is_nil)
                buf_printf(body, "int64_t %s = tur_cloneable_cont_resume(tur_continuation_snapshot(%s), (int64_t)0);\n",
                           tmp, k_var);
            else
                buf_printf(body, "int64_t %s = tur_cloneable_cont_resume(tur_continuation_snapshot(%s), (int64_t)%s);\n",
                           tmp, k_var, v_var);
            free(k_var);
            free(v_var);
            return tmp;
        }
        if (k_type.kind == TY_INT) {
            /* Phase 19D: fiber-based k.
             * tur_effect_cont_resume always returns int64_t (the fiber's final result),
             * regardless of the effect's return type. Always capture the return value
             * so the handler can return it as the handle expression's result. */
            char *tmp = fresh_tmp(ctx);
            Type vtype = e->as.resume_.resume->value->type;
            bool v_is_nil = (vtype.kind == TY_NIL || vtype.kind == TY_NEVER);
            indent_buf(body, ctx->indent);
            if (v_is_nil)
                buf_printf(body, "int64_t %s = tur_effect_cont_resume((int64_t)(intptr_t)%s, (int64_t)0);\n",
                           tmp, k_var);
            else
                buf_printf(body, "int64_t %s = tur_effect_cont_resume((int64_t)(intptr_t)%s, (int64_t)%s);\n",
                           tmp, k_var, v_var);
            free(k_var);
            free(v_var);
            return tmp;
        }
        free(k_var);
        /* Phase 18 path: return value directly */
        return v_var;
    }
    return atom_nil();
}

char *emit_effects_discontinue(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (discontinue k exception) - discontinue with exception.
     * Phase 19D: mark fiber as done and throw the exception.
     */
    if (e->as.discontinue_.discontinue) {
        char *k_var = emit_value(ctx, body, e->as.discontinue_.discontinue->k);
        if (e->as.discontinue_.discontinue->k->type.kind == TY_INT) {
            indent_buf(body, ctx->indent);
            buf_printf(body, "((FiberBlock*)(intptr_t)%s)->done = 1;\n", k_var);
        }
        free(k_var);
        char *e_var = emit_value(ctx, body, e->as.discontinue_.discontinue->exception);
        (void)e_var;  /* exception payload evaluated for side effects; discontinue aborts */
        free(e_var);
        indent_buf(body, ctx->indent);
        buf_puts(body, "abort();\n");
    }
    return atom_nil();
}

/* =========================================================================
 * Region A -- delimited / cloneable / serial continuations
 * ========================================================================= */

char *emit_effects_reset(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (reset body) - establish a continuation boundary and run body
     *
     * For v1 without full CPS: reset just evaluates and returns its body.
     * This is correct for the case where body doesn't contain shift.
     * When body contains shift, the CPS pass should have transformed it.
     */
    return emit_value(ctx, body, e->as.reset_.body);
}

char *emit_effects_cloneable_reset(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (cloneable-reset body) - evaluate body within a cloneable reset boundary.
     *
     * CPS-CL3 + CPS-CL5: When the body IS a cloneable-shift (Case 1), use the
     * full CPS path: emit a trivial __cont_fn, alloc a tur_cloneable_cont, and
     * call k_fn with the continuation.  The reset's value = k_fn's return value.
     *
     * Fallback: body contains no shift -- just evaluate and return body value.
     */
    const Expr *rb = e->as.cloneable_reset_.body;
    if (rb->kind == EX_CLONEABLE_SHIFT) {
        /* Full CPS: shift is the entire reset body (Case 1 -- trivial continuation). */
        const Expr *shift = rb;
        int cont_id = ctx->tmp_n++;

        /* CPS-CL3: Emit trivial continuation function to pending_handler_fns.
         * Appears at file scope before the enclosing function definition. */
        if (ctx->pending_handler_fns) {
            buf_printf(ctx->pending_handler_fns,
                "static int64_t __cont_fn_%d(void *__env, int64_t __value) {"
                " (void)__env; return __value; }\n\n", cont_id);
        }

        /* CPS-CL2: Emit env struct + alloc if there are live captures.
         * (For the trivial continuation the env is unused; emit placeholder.) */
        char *env_val_str  = NULL;   /* NULL -> "NULL" in format */
        char *clone_fn_str = NULL;
        char *drop_fn_str  = NULL;
        char *env_var = NULL;
        /* Case 1: the continuation body is trivially `return __value` and ignores
         * __env.  Never allocate an env struct here: the live_captures list may
         * contain the very binding that is being introduced by the enclosing let
         * (i.e., the shift result), which is not yet declared in C at this point.
         * Passing NULL for the env is correct -- __cont_fn_N ignores it. */
        bool has_env = false;
        if (has_env) {
            /* Emit a struct with one field per live capture */
            Buf *hb = ctx->pending_handler_fns;
            buf_printf(hb, "typedef struct { ");
            for (uint32_t ci = 0;
                 ci < shift->as.cloneable_shift_.n_live_captures; ci++) {
                Binding *cap = shift->as.cloneable_shift_.live_captures[ci];
                char *rn = raw_name_for_binding(cap);
                buf_printf(hb, "%s %s; ", type_c_name(cap->type), rn);
                free(rn);
            }
            buf_printf(hb, "} __clenv_%d;\n", cont_id);
            /* CPS-CL11: per-field deep clone using each binding's
             * Clone instance method (recorded by
             * cps_emit_capture_environment in src/cps.c).  Falls
             * back to bitwise copy for fields whose binding has no
             * recorded clone fn (e.g. primitives without a Clone
             * instance, or when tc_env was not threaded). */
            buf_printf(hb,
                "static void *__clenv_%d_clone(const void *src) {\n"
                "    __clenv_%d *copy = malloc(sizeof(__clenv_%d));\n"
                "    if (!copy) abort();\n"
                "    const __clenv_%d *s = (const __clenv_%d *)src;\n",
                cont_id, cont_id, cont_id, cont_id, cont_id);
            for (uint32_t ci = 0;
                 ci < shift->as.cloneable_shift_.n_live_captures; ci++) {
                Binding *cap = shift->as.cloneable_shift_.live_captures[ci];
                char *rn = raw_name_for_binding(cap);
                const char *clone_fn =
                    shift->as.cloneable_shift_.capture_clone_fns
                    ? shift->as.cloneable_shift_.capture_clone_fns[ci]
                    : NULL;
                if (clone_fn) {
                    buf_printf(hb,
                        "    copy->%s = %s(s->%s);\n",
                        rn, clone_fn, rn);
                } else {
                    buf_printf(hb,
                        "    copy->%s = s->%s;\n", rn, rn);
                }
                free(rn);
            }
            buf_printf(hb,
                "    return copy;\n"
                "}\n"
                "static void __clenv_%d_drop(void *p) { free(p); }\n\n",
                cont_id);

            /* Alloc env in the function body */
            env_var = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "__clenv_%d *%s = malloc(sizeof(__clenv_%d));\n",
                       cont_id, env_var, cont_id);
            indent_buf(body, ctx->indent);
            buf_printf(body, "if (!%s) abort();\n", env_var);
            for (uint32_t ci = 0;
                 ci < shift->as.cloneable_shift_.n_live_captures; ci++) {
                Binding *cap = shift->as.cloneable_shift_.live_captures[ci];
                char *rn = raw_name_for_binding(cap);
                char *cn = name_for_binding(ctx, cap);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s->%s = %s;\n", env_var, rn, cn);
                free(rn);
                free(cn);
            }
            clone_fn_str = malloc(32);
            drop_fn_str  = malloc(32);
            snprintf(clone_fn_str, 32, "__clenv_%d_clone", cont_id);
            snprintf(drop_fn_str,  32, "__clenv_%d_drop",  cont_id);
            env_val_str = env_var;
        }

        /* CPS-CL5: Alloc the continuation */
        char *cont_var = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        buf_printf(body,
            "tur_cloneable_cont *%s = tur_cloneable_cont_alloc("
            "__cont_fn_%d, %s, %s, %s);\n",
            cont_var, cont_id,
            env_val_str  ? env_val_str  : "NULL",
            clone_fn_str ? clone_fn_str : "NULL",
            drop_fn_str  ? drop_fn_str  : "NULL");

        /* CPS-CL5: Evaluate k_fn, then call k_fn(cont_as_int64) */
        char *k_fn_val = emit_value(ctx, body, shift->as.cloneable_shift_.k_fn);
        char *result   = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        if (shift->as.cloneable_shift_.k_fn->kind == EX_CLOSURE) {
            struct Closure *closure =
                shift->as.cloneable_shift_.k_fn->as.closure_.closure;
            char *thunk_name;
            if (closure->fn->binding) {
                thunk_name = raw_name_for_binding(closure->fn->binding);
            } else {
                thunk_name = malloc(64);
                snprintf(thunk_name, 64, "__fn_anon_%d",
                         closure->fn->n_params > 0
                             ? closure->fn->params[0]->id : 0);
            }
            buf_printf(body, "%s %s = %s(%s, (int64_t)(intptr_t)%s);\n",
                       type_c_name(e->type), result,
                       thunk_name, k_fn_val, cont_var);
            free(thunk_name);
        } else {
            buf_printf(body, "%s %s = %s((int64_t)(intptr_t)%s);\n",
                       type_c_name(e->type), result, k_fn_val, cont_var);
        }
        free(k_fn_val);
        if (env_var)      free(env_var);
        if (clone_fn_str) free(clone_fn_str);
        if (drop_fn_str)  free(drop_fn_str);
        return result;
    }
    /* CPS-CL4 Case 2: body contains cloneable-shift somewhere inside
     * (not as the direct body).  Emit a setjmp boundary so that
     * cloneable-shift can longjmp back to return k_fn's result. */
    if (cps_expr_contains_cloneable_shift(rb)) {
        char *rctx_var = fresh_tmp(ctx);
        char *result   = fresh_tmp(ctx);

        /* Push a new reset context */
        indent_buf(body, ctx->indent);
        buf_printf(body, "tur_cloneable_reset_ctx %s;\n", rctx_var);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s.result = 0;\n", rctx_var);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s.prev = tur_current_reset_ctx;\n", rctx_var);
        indent_buf(body, ctx->indent);
        buf_printf(body, "tur_current_reset_ctx = &%s;\n", rctx_var);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s;\n", type_c_name(e->type), result);

        indent_buf(body, ctx->indent);
        buf_printf(body, "if (setjmp(%s.jmp) == 0) {\n", rctx_var);
        ctx->indent++;
        /* Normal path: evaluate body */
        char *body_val = emit_value(ctx, body, rb);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s = %s;\n", result, body_val);
        free(body_val);
        ctx->indent--;
        indent_buf(body, ctx->indent);
        buf_puts(body, "} else {\n");
        ctx->indent++;
        /* Shift fired -- k_fn's return value is in ctx.result */
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s = %s.result;\n", result, rctx_var);
        ctx->indent--;
        indent_buf(body, ctx->indent);
        buf_puts(body, "}\n");
        /* Pop reset context */
        indent_buf(body, ctx->indent);
        buf_printf(body, "tur_current_reset_ctx = %s.prev;\n", rctx_var);

        free(rctx_var);
        return result;
    }

    /* Fallback: no shift in body -- just evaluate and return */
    return emit_value(ctx, body, rb);
}

char *emit_effects_shift(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (shift k body) - shift with continuation handler k and value body
     *
     * Semantics: evaluate body to get value v, call k(v), return result.
     * Note: Without CPS, we can't capture the continuation, so this is a
     * simplified version that just calls k with body's value.
     * Full implementation requires CPS transformation.
     */
    char *body_val = emit_value(ctx, body, e->as.shift_.body);
    char *result = fresh_tmp(ctx);

    /* Emit the call: k_fn(body_val) */
    /* Use emit_call_helper to handle both functions and closures */
    /* For now, just emit as a regular call */
    char *k_fn = emit_value(ctx, body, e->as.shift_.k_fn);

    /* Check if this is a closure call by looking at the expression */
    if (e->as.shift_.k_fn->kind == EX_CLOSURE) {
        /* For closures, k_fn is the env pointer, need to call thunk */
        struct Closure *closure = e->as.shift_.k_fn->as.closure_.closure;
        /* The thunk function name is based on the closure's function binding */
        char *thunk_name = (char *)malloc(64);
        if (closure->fn->binding) {
            snprintf(thunk_name, 64, "%s", closure->fn->binding->name->name);
        } else {
            /* Anonymous function - this shouldn't happen for closures */
            snprintf(thunk_name, 64, "__fn_anon_%d", closure->fn->params[0]->id);
        }
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s = %s(%s, %s);\n",
                  type_c_name(e->type), result, thunk_name, k_fn, body_val);
        free(thunk_name);
    } else {
        /* Regular function call */
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s = %s(%s);\n",
                  type_c_name(e->type), result, k_fn, body_val);
    }
    free(k_fn);
    free(body_val);
    return result;
}

char *emit_effects_shift0(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (shift0 k body) - one-shot shift
     * Similar to shift but k cannot resume the continuation.
     * For now, same implementation as shift.
     */
    char *body_val = emit_value(ctx, body, e->as.shift0_.body);
    char *result = fresh_tmp(ctx);

    char *k_fn = emit_value(ctx, body, e->as.shift0_.k_fn);

    /* Check if this is a closure call by looking at the expression */
    if (e->as.shift0_.k_fn->kind == EX_CLOSURE) {
        /* For closures, k_fn is the env pointer, need to call thunk */
        struct Closure *closure = e->as.shift0_.k_fn->as.closure_.closure;
        char *thunk_name = (char *)malloc(64);
        if (closure->fn->binding) {
            snprintf(thunk_name, 64, "%s", closure->fn->binding->name->name);
        } else {
            snprintf(thunk_name, 64, "__fn_anon_%d", closure->fn->params[0]->id);
        }
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s = %s(%s, %s);\n",
                  type_c_name(e->type), result, thunk_name, k_fn, body_val);
        free(thunk_name);
    } else {
        /* Regular function call */
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s = %s(%s);\n",
                  type_c_name(e->type), result, k_fn, body_val);
    }
    free(k_fn);
    free(body_val);
    return result;
}

char *emit_effects_cloneable_shift(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* CPS-CL2+CL3+CL5: Full cloneable-shift emission.
     *
     * When reached outside of the direct-reset-body Case 1 (which is
     * handled in emit_effects_cloneable_reset above), this is a standalone
     * shift inside a setjmp-based reset boundary (Case 2).
     *
     * Flow:
     *   1. Emit env struct + clone/drop helpers (CPS-CL2)
     *   2. Emit trivial continuation function (CPS-CL3)
     *   3. Pack live captures, alloc tur_cloneable_cont (CPS-CL5)
     *   4. Call k_fn with continuation
     *   5. Store k_fn result in reset ctx and longjmp
     */
    int cont_id = ctx->tmp_n++;

    /* CPS-CL3: Emit trivial continuation function.
     * For the standalone shift case, the continuation function is identity
     * (the post-shift code is handled by the setjmp return path in reset). */
    if (ctx->pending_handler_fns) {
        buf_printf(ctx->pending_handler_fns,
            "static int64_t __cont_fn_%d(void *__env, int64_t __value) {"
            " (void)__env; return __value; }\n\n", cont_id);
    }

    /* CPS-CL2: Emit env struct + clone/drop if live captures exist */
    char *env_val_str  = NULL;
    char *clone_fn_str = NULL;
    char *drop_fn_str  = NULL;
    char *env_var = NULL;
    bool has_env = (e->as.cloneable_shift_.n_live_captures > 0
                    && ctx->pending_handler_fns != NULL);
    if (has_env) {
        Buf *hb = ctx->pending_handler_fns;
        buf_printf(hb, "typedef struct { ");
        for (uint32_t ci = 0;
             ci < e->as.cloneable_shift_.n_live_captures; ci++) {
            Binding *cap = e->as.cloneable_shift_.live_captures[ci];
            char *rn = raw_name_for_binding(cap);
            buf_printf(hb, "%s %s; ", type_c_name(cap->type), rn);
            free(rn);
        }
        buf_printf(hb, "} __clenv_%d;\n", cont_id);
        /* CPS-CL11: per-field deep clone using recorded Clone fns. */
        buf_printf(hb,
            "static void *__clenv_%d_clone(const void *src) {\n"
            "    __clenv_%d *copy = malloc(sizeof(__clenv_%d));\n"
            "    if (!copy) abort();\n"
            "    const __clenv_%d *s = (const __clenv_%d *)src;\n",
            cont_id, cont_id, cont_id, cont_id, cont_id);
        for (uint32_t ci = 0;
             ci < e->as.cloneable_shift_.n_live_captures; ci++) {
            Binding *cap = e->as.cloneable_shift_.live_captures[ci];
            char *rn = raw_name_for_binding(cap);
            const char *clone_fn =
                e->as.cloneable_shift_.capture_clone_fns
                ? e->as.cloneable_shift_.capture_clone_fns[ci]
                : NULL;
            if (clone_fn) {
                buf_printf(hb,
                    "    copy->%s = %s(s->%s);\n",
                    rn, clone_fn, rn);
            } else {
                buf_printf(hb,
                    "    copy->%s = s->%s;\n", rn, rn);
            }
            free(rn);
        }
        buf_printf(hb,
            "    return copy;\n"
            "}\n"
            "static void __clenv_%d_drop(void *p) { free(p); }\n\n",
            cont_id);

        /* Alloc env in function body */
        env_var = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        buf_printf(body, "__clenv_%d *%s = malloc(sizeof(__clenv_%d));\n",
                   cont_id, env_var, cont_id);
        indent_buf(body, ctx->indent);
        buf_printf(body, "if (!%s) abort();\n", env_var);
        for (uint32_t ci = 0;
             ci < e->as.cloneable_shift_.n_live_captures; ci++) {
            Binding *cap = e->as.cloneable_shift_.live_captures[ci];
            char *rn = raw_name_for_binding(cap);
            char *cn = name_for_binding(ctx, cap);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s->%s = %s;\n", env_var, rn, cn);
            free(rn);
            free(cn);
        }
        clone_fn_str = malloc(32);
        drop_fn_str  = malloc(32);
        snprintf(clone_fn_str, 32, "__clenv_%d_clone", cont_id);
        snprintf(drop_fn_str,  32, "__clenv_%d_drop",  cont_id);
        env_val_str = env_var;
    }

    /* CPS-CL5: Alloc the continuation */
    char *cont_var = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body,
        "tur_cloneable_cont *%s = tur_cloneable_cont_alloc("
        "__cont_fn_%d, %s, %s, %s);\n",
        cont_var, cont_id,
        env_val_str  ? env_val_str  : "NULL",
        clone_fn_str ? clone_fn_str : "NULL",
        drop_fn_str  ? drop_fn_str  : "NULL");

    /* CPS-CL5: Call k_fn with the continuation */
    char *k_fn_val = emit_value(ctx, body, e->as.cloneable_shift_.k_fn);
    char *k_result = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    if (e->as.cloneable_shift_.k_fn->kind == EX_CLOSURE) {
        struct Closure *closure =
            e->as.cloneable_shift_.k_fn->as.closure_.closure;
        char *thunk_name;
        if (closure->fn->binding) {
            thunk_name = raw_name_for_binding(closure->fn->binding);
        } else {
            thunk_name = malloc(64);
            snprintf(thunk_name, 64, "__fn_anon_%d",
                     closure->fn->n_params > 0
                         ? closure->fn->params[0]->id : 0);
        }
        buf_printf(body, "int64_t %s = %s(%s, (int64_t)(intptr_t)%s);\n",
                   k_result, thunk_name, k_fn_val, cont_var);
        free(thunk_name);
    } else {
        buf_printf(body, "int64_t %s = %s((int64_t)(intptr_t)%s);\n",
                   k_result, k_fn_val, cont_var);
    }

    /* CPS-CL5: Store k_fn result in reset context and longjmp back */
    indent_buf(body, ctx->indent);
    buf_printf(body, "tur_current_reset_ctx->result = %s;\n", k_result);
    indent_buf(body, ctx->indent);
    buf_puts(body, "longjmp(tur_current_reset_ctx->jmp, 1);\n");

    /* The shift never "returns" normally (longjmp jumps past it).
     * Return k_result as the nominal value for type-checking purposes. */
    free(k_fn_val);
    if (env_var)      free(env_var);
    if (clone_fn_str) free(clone_fn_str);
    if (drop_fn_str)  free(drop_fn_str);
    return k_result;
}

char *emit_effects_serial_reset(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* serial-reset lowers to reset semantics: just evaluate body. */
    return emit_value(ctx, body, e->as.serial_reset_.body);
}

char *emit_effects_serial_shift(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* serial-shift: emit k_fn call with a NULL continuation placeholder.
     * This will be replaced by proper frame-serialization codegen once
     * the CPS pass handles EX_SERIAL_SHIFT nodes. */
    (void)e;
    char *tmp = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body, "/* serial-shift: full codegen not yet implemented */\n");
    indent_buf(body, ctx->indent);
    buf_printf(body, "int64_t %s = 0; /* serial-shift placeholder */\n", tmp);
    return tmp;
}

/* =========================================================================
 * Region B -- continuation predicate
 * ========================================================================= */

char *emit_effects_cont_pred(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* (cont? k) - check if a continuation is still valid (not done).
     * Phase 19D: for Phase-19 TY_INT k values (FiberBlock*), check !fiber->done.
     * For Phase 18 tur_cont* continuations, call tur_cont_consumed(). */
    char *inner = emit_value(ctx, body, e->as.cont_pred_.expr);
    char *tmp = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    if (e->as.cont_pred_.expr->type.kind == TY_CONT) {
        buf_printf(body, "bool %s = !tur_cont_consumed((tur_cont *)(intptr_t)%s);\n", tmp, inner);
    } else {
        /* Phase 19D: k is a FiberBlock* cast to int64_t */
        buf_printf(body, "bool %s = tur_effect_cont_valid(%s);\n", tmp, inner);
    }
    free(inner);
    return tmp;
}
