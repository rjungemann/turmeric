/* emit_expr.c -- expression-position C emission (emit_value and friends). */
#include "emit_internal.h"

/* ------------ block-shaped emitters ------------ */

static char *emit_let_value(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* Phase 3/4: Check if body contains return or throw first */
    bool body_has_return_or_throw = expr_contains_return_or_throw(e->as.let_.body);
    
    char *tmp = NULL;
    bool nil_result = (e->type.kind == TY_NIL);
    /* Phase R1: Also create temp if body has return/throw but let has non-nil type */
    if (!nil_result && !body_has_return_or_throw) {
        tmp = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s;\n", type_c_name(e->type), tmp);
    } else if (!nil_result && body_has_return_or_throw) {
        /* Special case for ? operator: body may contain return but still produce a value */
        tmp = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s;\n", type_c_name(e->type), tmp);
    }
    
    indent_buf(body, ctx->indent);
    buf_puts(body, "{\n");
    ctx->indent += 4;

    for (uint32_t i = 0; i < e->as.let_.n; i++) {
        const Binding *b = e->as.let_.bindings[i].binding;
        char *bn = name_for_binding(ctx, b);
        char *iv = emit_value(ctx, body, e->as.let_.bindings[i].init);
        indent_buf(body, ctx->indent);
        if (b->type.kind == TY_FN) {
            /* For function pointer types, emit: <result> (*<name>)(<args...>) = <init>; */
            buf_printf(body, "%s (*%s)(", 
                       type_c_name(emit_type_from_kind(b->type.as.fn.result_kind)), bn);
            for (uint8_t j = 0; j < b->type.as.fn.arity; j++) {
                if (j > 0) buf_puts(body, ", ");
                buf_printf(body, "%s", 
                           type_c_name(emit_type_from_kind(b->type.as.fn.arg_kinds[j])));
            }
            buf_printf(body, ") = %s;\n", iv);
        } else if (b->is_poly_fn) {
            /* Phase HRT4: let-bound poly fn alias — declare as tur_poly_fn_t. */
            buf_printf(body, "tur_poly_fn_t %s = %s;\n", bn, iv);
        } else {
            buf_printf(body, "%s %s = %s;\n", type_c_name(b->type), bn, iv);
        }
        /* Suppress unused-variable warnings even if the body never refs it. */
        indent_buf(body, ctx->indent);
        buf_printf(body, "(void)%s;\n", bn);
        free(bn);
        free(iv);
    }

    if (body_has_return_or_throw) {
        /* Body contains return/throw but may still produce a value on the
         * non-diverging path (e.g. the `?` operator: an if whose then-branch
         * returns and whose else-branch yields the unwrapped ok value).
         *
         * When the let has a non-nil result type AND the body itself can
         * yield a value (i.e. is not fully divergent), evaluate it as a
         * value expression and assign the inner result to `tmp`.  Fully
         * divergent bodies (e.g. `(do (defer ...) (return 42))`) produce
         * `(void)0` from emit_value; in that case we emit as a statement
         * and leave `tmp` uninitialised — the divergent path never reads
         * it. */
        if (!nil_result && !expr_is_divergent(e->as.let_.body)) {
            char *bv = emit_value(ctx, body, e->as.let_.body);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s = %s;\n", tmp, bv);
            free(bv);
        } else {
            emit_stmt(ctx, body, e->as.let_.body);
        }
        /* Close scope */
        ctx->indent -= 4;
        indent_buf(body, ctx->indent);
        buf_puts(body, "}\n");
        return tmp;
    }
    
    if (nil_result) {
        emit_stmt(ctx, body, e->as.let_.body);
    } else {
        char *bv = emit_value(ctx, body, e->as.let_.body);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s = %s;\n", tmp, bv);
        free(bv);
    }

    ctx->indent -= 4;
    indent_buf(body, ctx->indent);
    buf_puts(body, "}\n");
    
    return nil_result ? atom_nil() : tmp;
}

static char *emit_if_value(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* Phase 3/4: Check if branches contain return or throw */
    bool then_has_return_or_throw = expr_contains_return_or_throw(e->as.if_.then_);
    bool else_has_return_or_throw = e->as.if_.else_or_null ? 
        expr_contains_return_or_throw(e->as.if_.else_or_null) : false;
    bool any_has_return_or_throw = then_has_return_or_throw || else_has_return_or_throw;
    
    char *tmp = NULL;
    bool nil_result = (e->type.kind == TY_NIL);
    /* Phase R1: Also create temp if only then-branch has return/throw and else doesn't */
    bool else_no_return = e->as.if_.else_or_null ? !else_has_return_or_throw : false;
    if (!nil_result && ( !any_has_return_or_throw || (then_has_return_or_throw && else_no_return))) {
        tmp = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s;\n", type_c_name(e->type), tmp);
    }
    char *cond = emit_value(ctx, body, e->as.if_.cond);
    indent_buf(body, ctx->indent);
    buf_printf(body, "if (%s) {\n", cond);
    free(cond);
    ctx->indent += 4;
    if (nil_result || any_has_return_or_throw) {
        emit_stmt(ctx, body, e->as.if_.then_);
    } else {
        char *t = emit_value(ctx, body, e->as.if_.then_);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s = %s;\n", tmp, t);
        free(t);
    }
    ctx->indent -= 4;
    indent_buf(body, ctx->indent);
    buf_puts(body, "} else {\n");
    ctx->indent += 4;
    if (e->as.if_.else_or_null) {
        /* Phase R1: Special handling for ? operator lowering.
         * If only the then-branch has return/throw, emit the else-branch as a value.
         * This allows the ? operator to work: (if (err? x) (return ...) (ok-val x))
         */
        if (nil_result || (then_has_return_or_throw && else_has_return_or_throw)) {
            emit_stmt(ctx, body, e->as.if_.else_or_null);
        } else {
            char *el = emit_value(ctx, body, e->as.if_.else_or_null);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s = %s;\n", tmp, el);
            free(el);
        }
    } else {
        /* missing else: nothing to do (result type is nil) */
    }
    ctx->indent -= 4;
    indent_buf(body, ctx->indent);
    buf_puts(body, "}\n");
    return nil_result ? atom_nil() : tmp;
}

/* Phase 4 v1: defer-aware do-block emission.
 *
 * v1 strategy (effects-plan.md §6.10): Each scope gets a tur_frame variable.
 * Defers are collected and fired via tur_frame_fire_lifo at scope exit.
 * This is the unified runtime-list-on-frame model.
 *
 * For v1, we still emit defer bodies inline (not as thunks yet),
 * but we use the frame infrastructure for future-proofing.
 *
 * Known limitations:
 *   - Defer bodies are still emitted inline (v0 style), not as thunks
 *   - Captures are not yet properly handled via env structs
 *   - Defers inside `while` bodies fire on every loop iteration
 *   - Early `return` is not yet a language feature
 *
 * Future: Proper thunk generation with captures (effects-plan.md §6.10). */
static char *emit_do_value(EmitCtx *ctx, Buf *body, const Expr *e) {
    uint32_t n = e->as.do_.n;
    if (n == 0) return atom_nil();

    /* Phase 3/4: Check if there's a return/throw or defer in this do block */
    bool has_return_or_throw = false;
    bool has_defers = false;
    for (uint32_t i = 0; i < n; i++) {
        const Expr *it = e->as.do_.items[i];
        if (it->kind == EX_DEFER) {
            has_defers = true;
        } else if (expr_contains_return_or_throw(it)) {
            has_return_or_throw = true;
        }
    }
    
    /* If there's a return/throw, we need frame support for defer firing */
    if (has_return_or_throw) {
        has_defers = true;
    }

    if (!has_defers) {
        /* No defers - use old path (no frame needed) */
        int last_value_idx = -1;
        for (int i = (int)n - 1; i >= 0; i--) {
            if (e->as.do_.items[i]->kind != EX_DEFER) { last_value_idx = i; break; }
        }

        for (uint32_t i = 0; i < n; i++) {
            const Expr *it = e->as.do_.items[i];
            if (it->kind == EX_DEFER) continue;
            if ((int)i == last_value_idx) continue;
            emit_stmt(ctx, body, it);
        }

        char *result = NULL;
        if (last_value_idx >= 0) {
            const Expr *last = e->as.do_.items[last_value_idx];
            /* Phase 3/4: If last item is return or throw, emit as statement only */
            if (last->kind == EX_RETURN || last->kind == EX_PANIC ||
                last->kind == EX_PANIC_WITH || last->kind == EX_THROW) {
                emit_stmt(ctx, body, last);
                return atom_nil();
            } else if (last->type.kind == TY_NIL &&
                       /* Phase 19D: fiber-based resume always produces int64_t result */
                       !(last->kind == EX_RESUME && last->as.resume_.resume &&
                         last->as.resume_.resume->k && last->as.resume_.resume->k->type.kind == TY_INT)) {
                emit_stmt(ctx, body, last);
            } else {
                result = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                /* Phase 19D: if nil-typed but is fiber resume, use int64_t to capture result */
                bool is_fiber_resume = (last->kind == EX_RESUME && last->as.resume_.resume &&
                                        last->as.resume_.resume->k &&
                                        last->as.resume_.resume->k->type.kind == TY_INT);
                const char *decl_type = (last->type.kind == TY_NIL && is_fiber_resume)
                                        ? "int64_t" : type_c_name(last->type);
                buf_printf(body, "%s %s;\n", decl_type, result);
                char *v = emit_value(ctx, body, last);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = %s;\n", result, v);
                free(v);
            }
        }
        return result ? result : atom_nil();
    }

    /* v1 lowering: Has defers - use tur_frame */
    /* Save parent frame and set up new frame */
    const char *saved_frame = ctx->frame_var;
    char *frame_var = fresh_frame(ctx);
    ctx->frame_var = frame_var;

    /* Emit frame declaration and init */
    indent_buf(body, ctx->indent);
    buf_printf(body, "tur_frame %s;\n", frame_var);
    indent_buf(body, ctx->indent);
    if (saved_frame) {
        buf_printf(body, "tur_frame_init(&%s, &%s);\n", frame_var, saved_frame);
    } else {
        buf_printf(body, "tur_frame_init(&%s, NULL);\n", frame_var);
    }

    /* Find last non-defer item to source the do-block's value from. */
    int last_value_idx = -1;
    for (int i = (int)n - 1; i >= 0; i--) {
        if (e->as.do_.items[i]->kind != EX_DEFER) { last_value_idx = i; break; }
    }

    /* Phase 3/4: If there's a return or throw, emit all items as statements */
    if (has_return_or_throw) {
        /* Phase R1: The do may still produce a value on the non-divergent
         * path (e.g. `(do (if cond (return ...) ...) (defer ...))` whose
         * last non-defer item is the if, and the if's else branch yields
         * a value).  If the last value-producing item is non-divergent
         * and the do has a non-nil result type, materialise it into a
         * result tmp; otherwise fall back to statement-only emission. */
        const Expr *last_value = (last_value_idx >= 0)
                                     ? e->as.do_.items[last_value_idx] : NULL;
        bool last_yields_value = last_value &&
                                 e->type.kind != TY_NIL &&
                                 !expr_is_divergent(last_value);
        char *result_tmp = NULL;
        if (last_yields_value) {
            result_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s %s;\n", type_c_name(e->type), result_tmp);
        }

        /* Emit all items, registering defers */
        bool emitted_diverging = false;  /* Track if we've emitted a return/throw/panic */
        for (uint32_t i = 0; i < n; i++) {
            const Expr *it = e->as.do_.items[i];

            /* If we've already emitted a diverging expression (return/throw/panic),
             * skip remaining items as they're unreachable */
            if (emitted_diverging) {
                continue;
            }

            if (it->kind == EX_DEFER) {
                /* Register defer thunks */
                const Expr *defer_expr = it->as.defer_.body;
                const uint8_t n_captures = it->as.defer_.n_captures;
                Binding **captures = it->as.defer_.captures;

                if (n_captures == 0) {
                    char *thunk_name = fresh_defer_thunk(ctx);
                    register_defer_thunk(ctx, thunk_name, defer_expr, captures, n_captures, NULL);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "tur_frame_push_defer(&%s, %s, NULL);\n", frame_var, thunk_name);
                } else {
                    char *env_name = fresh_defer_env(ctx);
                    char *thunk_name = fresh_defer_thunk(ctx);
                    register_defer_thunk(ctx, thunk_name, defer_expr, captures, n_captures, env_name);

                    char *env_tmp = fresh_tmp(ctx);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "struct %s %s = {", env_name, env_tmp);
                    for (uint8_t j = 0; j < n_captures; j++) {
                        if (j > 0) buf_puts(body, ", ");
                        char *cn = name_for_binding(ctx, captures[j]);
                        buf_printf(body, ".%s = %s", captures[j]->name->name, cn);
                        free(cn);
                    }
                    buf_puts(body, "};\n");

                    indent_buf(body, ctx->indent);
                    buf_printf(body, "tur_frame_push_defer(&%s, %s, &%s);\n", frame_var, thunk_name, env_tmp);
                    free(env_tmp);
                }
            } else if (last_yields_value && (int)i == last_value_idx) {
                char *bv = emit_value(ctx, body, it);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = %s;\n", result_tmp, bv);
                free(bv);
            } else {
                emit_stmt(ctx, body, it);
                /* Check if this statement diverges (return/throw/panic) */
                if (it->kind == EX_RETURN ||
                    it->kind == EX_PANIC || it->kind == EX_PANIC_WITH) {
                    emitted_diverging = true;
                }
            }
        }

        /* Fire frame defers on the non-divergent path before producing the
         * do's value.  Divergent paths already fired via tur_frame_fire_chain
         * in the emitted return/throw/panic statement. */
        if (last_yields_value && !emitted_diverging) {
            indent_buf(body, ctx->indent);
            buf_printf(body, "tur_frame_fire_lifo(&%s);\n", frame_var);
        }

        ctx->frame_var = saved_frame;
        free(frame_var);
        return result_tmp ? result_tmp : atom_nil();
    }

    /* Emit non-defer items and register defer thunks */
    for (uint32_t i = 0; i < n; i++) {
        const Expr *it = e->as.do_.items[i];
        if (it->kind == EX_DEFER) {
            /* v1 lowering: Generate thunk and register with frame */
            const Expr *defer_expr = it->as.defer_.body;
            const uint8_t n_captures = it->as.defer_.n_captures;
            Binding **captures = it->as.defer_.captures;
            
            if (n_captures == 0) {
                /* No captures - generate thunk */
                char *thunk_name = fresh_defer_thunk(ctx);
                register_defer_thunk(ctx, thunk_name, defer_expr, captures, n_captures, NULL);
                indent_buf(body, ctx->indent);
                buf_printf(body, "tur_frame_push_defer(&%s, %s, NULL);\n", frame_var, thunk_name);
            } else {
                /* Has captures - generate thunk with env struct */
                char *env_name = fresh_defer_env(ctx);
                char *thunk_name = fresh_defer_thunk(ctx);
                register_defer_thunk(ctx, thunk_name, defer_expr, captures, n_captures, env_name);
                
                /* Emit env struct type definition at file scope */
                /* We'll emit this in emit_pending_defer_thunks, but we need to
                 * also create the env instance here and pass its address */
                
                /* Create env instance and register with address */
                char *env_tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "struct %s %s = {", env_name, env_tmp);
                for (uint8_t j = 0; j < n_captures; j++) {
                    if (j > 0) buf_puts(body, ", ");
                    char *cn = name_for_binding(ctx, captures[j]);
                    buf_printf(body, ".%s = %s", captures[j]->name->name, cn);
                    free(cn);
                }
                buf_puts(body, "};\n");
                
                indent_buf(body, ctx->indent);
                buf_printf(body, "tur_frame_push_defer(&%s, %s, &%s);\n", frame_var, thunk_name, env_tmp);
                free(env_tmp);
            }
        } else if ((int)i == last_value_idx) {
            continue; /* emitted as value below */
        } else {
            emit_stmt(ctx, body, it);
        }
    }

    char *result = NULL;
    if (last_value_idx >= 0) {
        const Expr *last = e->as.do_.items[last_value_idx];
        if (last->type.kind == TY_NIL) {
            emit_stmt(ctx, body, last);
        } else {
            /* Hoist value into a temp so defers can fire after it's computed. */
            result = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s %s;\n", type_c_name(last->type), result);
            char *v = emit_value(ctx, body, last);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s = %s;\n", result, v);
            free(v);
        }
    }

    /* Fire all defers LIFO at scope exit */
    indent_buf(body, ctx->indent);
    buf_printf(body, "tur_frame_fire_lifo(&%s);\n", frame_var);

    /* Restore frame state */
    ctx->frame_var = saved_frame;
    free(frame_var);

    return result ? result : atom_nil();
}


char *emit_value(EmitCtx *ctx, Buf *body, const Expr *e) {
    switch (e->kind) {
        case EX_NIL_LIT:  return atom_nil();
        case EX_BOOL_LIT: return atom_bool(e->as.b);
        case EX_INT_LIT:  return atom_int_typed(e->as.i, e->type.kind);
        case EX_FLOAT_LIT:
            if (e->type.kind == TY_FLOAT32) return atom_float32(e->as.f);
            return atom_float(e->as.f);
        case EX_CSTR_LIT: return atom_cstr(e->as.s);
        case EX_VAR:      return atom_var(ctx, e->as.var.binding);
        case EX_CAST: {
            /* (as TargetType expr) — emit as C cast: (target_c_type)(inner) */
            char *inner = emit_value(ctx, body, e->as.cast_.expr);
            Type target = type_simple(e->as.cast_.target_kind, CK_COPY);
            Buf out; buf_init(&out);
            buf_printf(&out, "((%s)(%s))", type_c_name(target), inner);
            buf_putc(&out, '\0');
            free(inner);
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        case EX_UNION_INJECT: {
            /* IT4: wrap a member value into tur_tagged_t via TUR_TAG(tag_idx, val).
             * The inner value is cast to int64_t so pointer/struct payloads fit. */
            char *inner = emit_value(ctx, body, e->as.union_inject_.value);
            Buf out; buf_init(&out);
            buf_printf(&out, "TUR_TAG(%lld, (int64_t)(intptr_t)(%s))",
                       (long long)e->as.union_inject_.tag_idx, inner);
            buf_putc(&out, '\0');
            free(inner);
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        case EX_ANY_TYPE_OF: {
            /* IT4: (type-of x) — return cstr type name via __tur_any_type_name(tag) */
            char *inner = emit_value(ctx, body, e->as.any_type_of_.value);
            Buf out; buf_init(&out);
            buf_printf(&out, "__tur_any_type_name(TUR_GETTAG(%s))", inner);
            buf_putc(&out, '\0');
            free(inner);
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        case EX_ANY_CAST: {
            /* IT4: (cast x T) — unsafe unbox: interpret TUR_UNTAG(x) as target C type.
             * No runtime tag check; caller is responsible for correctness. */
            char *inner = emit_value(ctx, body, e->as.any_cast_.value);
            Type target = type_simple(e->as.any_cast_.target_kind, CK_COPY);
            Buf out; buf_init(&out);
            buf_printf(&out, "((%s)(intptr_t)TUR_UNTAG(%s))", type_c_name(target), inner);
            buf_putc(&out, '\0');
            free(inner);
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        case EX_LET:      return emit_let_value(ctx, body, e);
        case EX_IF:       return emit_if_value(ctx, body, e);
        case EX_DO:       return emit_do_value(ctx, body, e);
        case EX_BUILTIN:  return emit_builtin(ctx, body, e);
        case EX_WHILE:    emit_while_stmt(ctx, body, e); return atom_nil();
        case EX_SET:      emit_set_stmt(ctx, body, e);   return atom_nil();
        case EX_SET_DEREF: emit_set_deref_stmt(ctx, body, e); return atom_nil();
        case EX_DEF:      /* handled at top level — shouldn't appear nested. */
        case EX_PROGRAM:
        case EX_DEFMODULE: /* Phase M0: module metadata node */
        case EX_TYPECLASS_DEF:
            /* Typeclass definitions are compile-time only - no runtime code */
            return atom_nil();
        case EX_INSTANCE_DEF:
            /* Handled in emit_stmt - file scope only */
            return atom_nil();
        /* Phase H §1: dictionary passing — either load method field (for dict
         * dispatch via fn_expr) or emit singleton address (bare dict value). */
        case EX_DICT: {
            char dict_name[128];
            emit_dict_name(dict_name, sizeof(dict_name), e->as.dict_.instance);
            Buf out; buf_init(&out);
            if (e->as.dict_.method_name[0] != '\0') {
                /* Dictionary dispatch: emit function-pointer field access.
                 * The EX_CALL indirect-call path then casts and calls it:
                 *   ((ret_t (*)(...))(intptr_t)(dict_X_singleton.method))(args) */
                buf_printf(&out, "%s_singleton.%s", dict_name, e->as.dict_.method_name);
            } else {
                /* Bare dictionary value: singleton address as int64_t. */
                buf_printf(&out, "(int64_t)(intptr_t)(&%s_singleton)", dict_name);
            }
            buf_putc(&out, '\0');
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        /* Phase R2: Panic */
        case EX_PANIC: {
            /* (panic msg) - evaluate msg (cstr), call tur_panic[_abort] */
            const Expr *payload = e->as.panic_.payload;
            char *msg_val = emit_value(ctx, body, payload);
            indent_buf(body, ctx->indent);
            if (ctx->no_unwind) {
                /* Phase R5: #[no-unwind] — abort directly; skip defer chain */
                if (payload->type.kind == TY_CSTR) {
                    buf_printf(body, "tur_panic_abort(%s);\n", msg_val);
                } else {
                    buf_puts(body, "tur_panic_abort(\"(non-string panic)\");\n");
                }
            } else {
                /* Set the current frame for panic to fire defers */
                if (ctx->frame_var) {
                    buf_printf(body, "tur_panic_set_frame(&%s);\n", ctx->frame_var);
                }
                if (payload->type.kind == TY_CSTR) {
                    buf_printf(body, "tur_panic(%s);\n", msg_val);
                } else {
                    /* For non-cstr, use a generic message */
                    buf_printf(body, "tur_panic(\"(non-string panic)\");\n");
                }
            }
            free(msg_val);
            return atom_nil();
        }
        case EX_PANIC_WITH: {
            /* (panic-with payload) - panic with typed payload */
            const Expr *payload = e->as.panic_with_.payload;
            char *payload_val = emit_value(ctx, body, payload);
            indent_buf(body, ctx->indent);
            if (ctx->no_unwind) {
                /* Phase R5: #[no-unwind] — abort directly; skip defer chain */
                buf_puts(body, "tur_panic_abort(\"(panic-with)\");\n");
            } else {
                /* Set the current frame for panic to fire defers */
                if (ctx->frame_var) {
                    buf_printf(body, "tur_panic_set_frame(&%s);\n", ctx->frame_var);
                }
                /* tur_panic_with takes type_tag (int), payload (void*), file, line */
                buf_printf(body, "tur_panic_with(%d, (void*)%s, __FILE__, __LINE__);\n",
                           (int)payload->type.kind, payload_val);
            }
            free(payload_val);
            return atom_nil();
        }
        case EX_CATCH_UNWIND: {
            /* (catch-unwind thunk) - setjmp boundary, call thunk, handle panic */
            const Expr *thunk = e->as.catch_unwind_.thunk;
            indent_buf(body, ctx->indent);
            /* Generate a unique result variable name */
            char result_var[64];
            snprintf(result_var, sizeof(result_var), "__catch_result_%d", ctx->tmp_n++);
            /* Declare result variable */
            buf_printf(body, "tur_result %s;\n", result_var);
            indent_buf(body, ctx->indent);
            /* Call tur_catch_unwind with the thunk */
            char *thunk_val = emit_value(ctx, body, thunk);
            /* The thunk is a function that takes (void* env, tur_result* out) */
            /* For simplicity in v1, we use NULL as env and pass result_var as out */
            buf_printf(body, "if (tur_catch_unwind((tur_thunk_fn)%s, NULL, &%s)) {\n", thunk_val, result_var);
            free(thunk_val);
            ctx->indent++;
            indent_buf(body, ctx->indent);
            /* Panic was caught - return the err payload as a result */
            /* For v1, we return the payload pointer wrapped in err */
            buf_printf(body, "/* panic caught */\n");
            ctx->indent--;
            indent_buf(body, ctx->indent);
            buf_printf(body, "} else {\n");
            ctx->indent++;
            indent_buf(body, ctx->indent);
            buf_printf(body, "/* normal completion - extract ok value */\n");
            ctx->indent--;
            indent_buf(body, ctx->indent);
            buf_printf(body, "}\n");
            indent_buf(body, ctx->indent);
            /* Return the result - for v1, just return the ok value or panic payload */
            buf_printf(body, "/* v1: simplified - return result struct directly */\n");
            /* Return the result variable as a pointer - cast to int64_t for ptr<void> */
            return strdup(result_var);
        }
        case EX_CATCH_PANIC_OF: {
            /* (catch-panic-of Type thunk) - typed catch boundary */
            const Expr *thunk = e->as.catch_panic_of_.thunk;
            TypeKind type_kind = e->as.catch_panic_of_.type_kind;
            indent_buf(body, ctx->indent);
            /* Generate a unique result variable name */
            char result_var[64];
            snprintf(result_var, sizeof(result_var), "__catch_panic_of_result_%d", ctx->tmp_n++);
            /* Declare result variable */
            buf_printf(body, "tur_result %s;\n", result_var);
            indent_buf(body, ctx->indent);
            /* Call tur_catch_panic_of with type and thunk */
            char *thunk_val = emit_value(ctx, body, thunk);
            buf_printf(body, "if (tur_catch_panic_of(%d, (tur_thunk_fn)%s, NULL, &%s)) {\n",
                   (int)type_kind, thunk_val, result_var);
            free(thunk_val);
            ctx->indent++;
            indent_buf(body, ctx->indent);
            buf_printf(body, "/* panic of matching type caught */\n");
            ctx->indent--;
            indent_buf(body, ctx->indent);
            buf_printf(body, "} else {\n");
            ctx->indent++;
            indent_buf(body, ctx->indent);
            buf_printf(body, "/* normal completion or type mismatch (re-panicked) */\n");
            ctx->indent--;
            indent_buf(body, ctx->indent);
            buf_printf(body, "}\n");
            indent_buf(body, ctx->indent);
            /* Return the result variable as a pointer */
            return strdup(result_var);
        }
        /* Phase S4: throw / try-catch */
        case EX_THROW: {
            const Expr *thrown = e->as.throw_.value;
            char *val = emit_value(ctx, body, thrown);
            indent_buf(body, ctx->indent);
            if (ctx->no_unwind) {
                buf_printf(body, "tur_panic_abort(\"(throw)\");\n");
            } else {
                if (ctx->frame_var) {
                    buf_printf(body, "tur_panic_set_frame(&%s);\n", ctx->frame_var);
                }
                buf_printf(body, "tur_panic_with(%d, (void*)%s, __FILE__, __LINE__);\n",
                           (int)thrown->type.kind, val);
            }
            free(val);
            return atom_nil();
        }
        case EX_TRY_CATCH: {
            /* Phase S4: wrap the try body in a tur_thunk_fn, call tur_catch_unwind,
             * and dispatch the first catch clause on panic.
             *
             * Fall back to body-only emission when:
             *   - no_unwind: panics already abort at the C level, catch is unreachable.
             *   - no pending_handler_fns: we have nowhere to put the thunk.
             */
            if (ctx->no_unwind || !ctx->pending_handler_fns) {
                return emit_value(ctx, body, e->as.try_catch_.body);
            }

            const Expr    *try_body      = e->as.try_catch_.body;
            uint32_t       n_catches     = e->as.try_catch_.n_catches;
            Binding      **catch_binds   = e->as.try_catch_.catch_bindings;
            Expr         **catch_hdlrs   = e->as.try_catch_.catch_handlers;

            /* Collect free variables (captures) from the try body */
            uint32_t   n_caps = 0;
            Binding  **caps   = collect_handle_captures(try_body, &n_caps);

            int thunk_id = ctx->tmp_n++;
            Buf *pbuf    = ctx->pending_handler_fns;

            /* Emit env struct if there are captures */
            if (n_caps > 0) {
                buf_printf(pbuf, "typedef struct { ");
                for (uint32_t ci = 0; ci < n_caps; ci++) {
                    Binding *cap = caps[ci];
                    char *rn = raw_name_for_binding(cap);
                    buf_printf(pbuf, "%s %s; ", type_c_name(cap->type), rn);
                    free(rn);
                }
                buf_printf(pbuf, "} __try_env_%d;\n", thunk_id);
            }

            /* Build the thunk body in a temporary buffer */
            Buf thunk_body; buf_init(&thunk_body);

            EmitCtx tctx              = *ctx;
            tctx.file                 = pbuf;
            tctx.pending_handler_fns  = pbuf;
            tctx.indent               = 4;
            tctx.fn_params            = NULL;
            tctx.n_fn_params          = 0;
            tctx.closure              = NULL;
            tctx.env_var_name         = (n_caps > 0) ? "__e" : NULL;
            tctx.defer_captures       = caps;
            tctx.n_defer_captures     = (uint8_t)(n_caps > 255 ? 255 : n_caps);
            tctx.frame_var            = NULL;
            tctx.return_emitted       = false;

            bool body_void = (try_body->type.kind == TY_NIL ||
                              try_body->type.kind == TY_NEVER ||
                              expr_is_divergent(try_body));
            char *body_ret = NULL;
            if (body_void) {
                emit_stmt(&tctx, &thunk_body, try_body);
            } else {
                body_ret = emit_value(&tctx, &thunk_body, try_body);
            }
            ctx->tmp_n = tctx.tmp_n;

            /* Emit the thunk function to pbuf */
            buf_printf(pbuf, "static void __try_thunk_%d(void *__env, tur_result *__out) {\n", thunk_id);
            if (n_caps > 0) {
                buf_printf(pbuf, "    __try_env_%d *__e = (__try_env_%d *)__env;\n", thunk_id, thunk_id);
            } else {
                buf_puts(pbuf, "    (void)__env;\n");
            }
            if (thunk_body.len > 0)
                buf_write(pbuf, thunk_body.data, thunk_body.len);
            buf_puts(pbuf, "    __out->tag = TUR_RESULT_OK;\n");
            if (body_ret) {
                buf_printf(pbuf, "    __out->u.ok_val = (int64_t)%s;\n", body_ret);
                free(body_ret);
            } else {
                buf_puts(pbuf, "    __out->u.ok_val = 0;\n");
            }
            buf_puts(pbuf, "}\n\n");
            buf_free(&thunk_body);

            /* ---- call site ---- */

            /* Create env struct on stack (or pass NULL) */
            const char *env_arg;
            char *env_var = NULL;
            if (n_caps > 0) {
                env_var = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "__try_env_%d %s = { ", thunk_id, env_var);
                for (uint32_t ci = 0; ci < n_caps; ci++) {
                    if (ci > 0) buf_puts(body, ", ");
                    char *cn = name_for_binding(ctx, caps[ci]);
                    buf_puts(body, cn);
                    free(cn);
                }
                buf_puts(body, " };\n");
                env_arg = env_var;
            } else {
                env_arg = "NULL";
            }
            free(caps);

            /* tur_result + tur_catch_unwind call */
            char *res_var     = fresh_tmp(ctx);
            char *caught_var  = fresh_tmp(ctx);
            char *result_var  = fresh_tmp(ctx);

            indent_buf(body, ctx->indent);
            buf_printf(body, "tur_result %s;\n", res_var);
            indent_buf(body, ctx->indent);
            if (n_caps > 0) {
                buf_printf(body, "bool %s = tur_catch_unwind(__try_thunk_%d, &%s, &%s);\n",
                           caught_var, thunk_id, env_arg, res_var);
            } else {
                buf_printf(body, "bool %s = tur_catch_unwind(__try_thunk_%d, %s, &%s);\n",
                           caught_var, thunk_id, env_arg, res_var);
            }
            free(env_var);

            indent_buf(body, ctx->indent);
            buf_printf(body, "int64_t %s;\n", result_var);
            indent_buf(body, ctx->indent);
            buf_printf(body, "if (%s) {\n", caught_var);
            free(caught_var);

            ctx->indent += 4;
            if (n_catches > 0) {
                /* Bind catch variable to the panic payload value */
                Binding *cb = catch_binds[0];
                if (cb) {
                    char *cb_name = name_for_binding(ctx, cb);
                    indent_buf(body, ctx->indent);
                    buf_printf(body,
                        "int64_t %s = (int64_t)(intptr_t)tur_panic_payload_value(%s.u.err);\n",
                        cb_name, res_var);
                    free(cb_name);
                }
                char *h = emit_value(ctx, body, catch_hdlrs[0]);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = (int64_t)%s;\n", result_var, h);
                free(h);
            } else {
                /* No catch clause: re-panic with the original payload */
                indent_buf(body, ctx->indent);
                buf_printf(body,
                    "tur_panic_with(%s.u.err->type_tag, "
                    "(void*)tur_panic_payload_value(%s.u.err), __FILE__, __LINE__);\n",
                    res_var, res_var);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = 0;\n", result_var);
            }
            ctx->indent -= 4;

            indent_buf(body, ctx->indent);
            buf_puts(body, "} else {\n");
            ctx->indent += 4;
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s = %s.u.ok_val;\n", result_var, res_var);
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            free(res_var);

            return result_var;
        }
        case EX_PANIC_PAYLOAD_TYPE: {
            const Expr *payload = e->as.panic_payload_type_.payload;
            char *payload_var = emit_value(ctx, body, payload);
            char *result = malloc(strlen(payload_var) + 64);
            snprintf(result, strlen(payload_var) + 64, "(int64_t)tur_panic_payload_type((tur_panic_payload*)%s)", payload_var);
            free(payload_var);
            return result;
        }
        case EX_PANIC_PAYLOAD_VALUE: {
            const Expr *payload = e->as.panic_payload_value_.payload;
            char *payload_var = emit_value(ctx, body, payload);
            char *result = malloc(strlen(payload_var) + 64);
            snprintf(result, strlen(payload_var) + 64, "tur_panic_payload_value((tur_panic_payload*)%s)", payload_var);
            free(payload_var);
            return result;
        }
        case EX_PANIC_PAYLOAD_FILE: {
            const Expr *payload = e->as.panic_payload_file_.payload;
            char *payload_var = emit_value(ctx, body, payload);
            char *result = malloc(strlen(payload_var) + 64);
            snprintf(result, strlen(payload_var) + 64, "tur_panic_payload_file((tur_panic_payload*)%s)", payload_var);
            free(payload_var);
            return result;
        }
        case EX_PANIC_PAYLOAD_LINE: {
            const Expr *payload = e->as.panic_payload_line_.payload;
            char *payload_var = emit_value(ctx, body, payload);
            char *result = malloc(strlen(payload_var) + 64);
            snprintf(result, strlen(payload_var) + 64, "(int64_t)tur_panic_payload_line((tur_panic_payload*)%s)", payload_var);
            free(payload_var);
            return result;
        }
        case EX_PANIC_PAYLOAD_DOWNS: {
            const Expr *payload = e->as.panic_payload_downs_.payload;
            TypeKind target_type = e->as.panic_payload_downs_.target_type;
            char *payload_var = emit_value(ctx, body, payload);
            free(payload_var);
            (void)target_type; /* unused in v1 */
            return atom_nil();
        }
        /* Phase 18: Delimited continuations */
        case EX_RESET: {
            /* (reset body) - establish a continuation boundary and run body
             * 
             * For v1 without full CPS: reset just evaluates and returns its body.
             * This is correct for the case where body doesn't contain shift.
             * When body contains shift, the CPS pass should have transformed it.
             */
            /* Emit body as an expression and return its value */
            return emit_value(ctx, body, e->as.reset_.body);
        }
        /* Phase B2: Cloneable continuations */
        case EX_CLONEABLE_RESET: {
            /* (cloneable-reset body) - evaluate body within a cloneable reset boundary.
             *
             * CPS-CL3 + CPS-CL5: When the body IS a cloneable-shift (Case 1), use the
             * full CPS path: emit a trivial __cont_fn, alloc a tur_cloneable_cont, and
             * call k_fn with the continuation.  The reset's value = k_fn's return value.
             *
             * Fallback: body contains no shift — just evaluate and return body value.
             */
            const Expr *rb = e->as.cloneable_reset_.body;
            if (rb->kind == EX_CLONEABLE_SHIFT) {
                /* Full CPS: shift is the entire reset body (Case 1 — trivial continuation). */
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
                char *env_val_str  = NULL;   /* NULL → "NULL" in format */
                char *clone_fn_str = NULL;
                char *drop_fn_str  = NULL;
                char *env_var = NULL;
                /* Case 1: the continuation body is trivially `return __value` and ignores
                 * __env.  Never allocate an env struct here: the live_captures list may
                 * contain the very binding that is being introduced by the enclosing let
                 * (i.e., the shift result), which is not yet declared in C at this point.
                 * Passing NULL for the env is correct — __cont_fn_N ignores it. */
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
                /* Shift fired — k_fn's return value is in ctx.result */
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

            /* Fallback: no shift in body — just evaluate and return */
            return emit_value(ctx, body, rb);
        }
        case EX_SHIFT: {
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
                /* For now, construct the thunk name from the function */
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
        case EX_SHIFT0: {
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
        case EX_CLONEABLE_SHIFT: {
            /* CPS-CL2+CL3+CL5: Full cloneable-shift emission.
             *
             * When reached outside of the direct-reset-body Case 1 (which is
             * handled in EX_CLONEABLE_RESET above), this is a standalone shift
             * inside a setjmp-based reset boundary (Case 2).
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
        /* Phase 21: Serializable continuations
         * For now, serial-reset / serial-shift lower to their plain counterparts.
         * Full codegen (frame registration, wire-format codec calls) is a later
         * compiler pass.  These stubs keep the switch exhaustive and emit an
         * obvious runtime failure when the node is actually reached. */
        case EX_SERIAL_RESET: {
            /* serial-reset lowers to reset semantics: just evaluate body. */
            return emit_value(ctx, body, e->as.serial_reset_.body);
        }
        case EX_SERIAL_SHIFT: {
            /* serial-shift: emit k_fn call with a NULL continuation placeholder.
             * This will be replaced by proper frame-serialization codegen once
             * the CPS pass handles EX_SERIAL_SHIFT nodes. */
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "/* serial-shift: full codegen not yet implemented */\n");
            indent_buf(body, ctx->indent);
            buf_printf(body, "int64_t %s = 0; /* serial-shift placeholder */\n", tmp);
            return tmp;
        }
        /* Phase 2 */
        case EX_FN_DEF:
            /* fn should only appear at top level for phase 2 */
            fprintf(stderr, "tur: emit: EX_FN_DEF in value position (nested fn not yet supported)\n");
            abort();
        case EX_FN:
            fprintf(stderr, "tur: emit: EX_FN not yet implemented\n");
            abort();
        case EX_CALL: {
            Binding *fn_binding = e->as.call_.fn_binding;

            /* Phase 16 v2: indirect capability field call — fn_expr is EX_GET_FIELD.
             * Emit: ((ret_t (*)(arg_t, ...))(intptr_t)(struct_val).field_name)(args...)
             * Effect-row annotation is advisory (erased to a plain function pointer). */
            if (e->as.call_.fn_expr) {
                Expr *gf = e->as.call_.fn_expr;
                char *fn_ptr_val = emit_value(ctx, body, gf);
                const char *ret_c = type_c_name(e->type);

                char **arg_strs = (char **)malloc(e->as.call_.n_args * sizeof(char *));
                if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
                bool *arg_cast = (bool *)malloc(e->as.call_.n_args * sizeof(bool));
                if (!arg_cast) { fprintf(stderr, "tur: oom\n"); abort(); }
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    char *raw = emit_value(ctx, body, e->as.call_.args[i]);
                    /* Apply same function-ptr → int64_t cast as the direct-call path:
                     * C99 forbids implicit conversion from function pointer to integer.
                     * Track which args were cast so the signature uses int64_t, not
                     * void *, for those positions — passing int64_t to void * is
                     * -Wint-conversion error in C99. */
                    bool needs_fn_cast = (e->as.call_.args[i]->type.kind == TY_FN ||
                                          e->as.call_.args[i]->type.kind == TY_PTR_VOID);
                    arg_cast[i] = needs_fn_cast;
                    if (needs_fn_cast) {
                        Buf cast; buf_init(&cast);
                        buf_printf(&cast, "(int64_t)(intptr_t)(%s)", raw);
                        buf_putc(&cast, '\0');
                        free(raw);
                        raw = strdup(cast.data);
                        buf_free(&cast);
                    }
                    arg_strs[i] = raw;
                }

                Buf out; buf_init(&out);
                /* Cast function pointer to the right signature, then call. */
                buf_printf(&out, "((%s (*)(", ret_c);
                if (e->as.call_.n_args == 0) {
                    buf_puts(&out, "void");
                } else {
                    for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                        if (i > 0) buf_puts(&out, ", ");
                        /* If we cast this arg to int64_t above, the signature must
                         * say int64_t (not void *) to avoid -Wint-conversion. */
                        if (arg_cast[i]) {
                            buf_puts(&out, "int64_t");
                        } else {
                            buf_puts(&out, type_c_name(e->as.call_.args[i]->type));
                        }
                    }
                }
                free(arg_cast);
                buf_printf(&out, "))(intptr_t)(%s))(", fn_ptr_val);
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    if (i > 0) buf_puts(&out, ", ");
                    buf_puts(&out, arg_strs[i]);
                }
                buf_puts(&out, ")");
                buf_putc(&out, '\0');
                char *result = strdup(out.data);
                buf_free(&out);
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) free(arg_strs[i]);
                free(arg_strs);
                free(fn_ptr_val);
                return result;
            }
            
            /* Phase HRT1: poly call through rank-2 fn param: emit fn_name.fn(fn_name.env, arg0, ...) */
            if (e->as.call_.is_poly_call) {
                char *fn_name = raw_name_for_binding(fn_binding);
                char **arg_strs = (char **)malloc(e->as.call_.n_args * sizeof(char *));
                if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    char *raw = emit_value(ctx, body, e->as.call_.args[i]);
                    /* Phase HRT3: nested poly fn arg — take address of compound literal so it
                     * can be passed as int64_t through the poly fn dispatch layer. */
                    if (e->as.call_.poly_arg_mask & (1u << i)) {
                        Buf cast; buf_init(&cast);
                        buf_printf(&cast, "(int64_t)(intptr_t)(&(%s))", raw);
                        buf_putc(&cast, '\0');
                        free(raw);
                        raw = strdup(cast.data);
                        buf_free(&cast);
                    } else {
                        /* Poly fn expects int64_t args — cast fn ptrs and void* through intptr_t */
                        bool needs_cast = (e->as.call_.args[i]->type.kind == TY_FN ||
                                           e->as.call_.args[i]->type.kind == TY_PTR_VOID);
                        if (needs_cast) {
                            Buf cast; buf_init(&cast);
                            buf_printf(&cast, "(int64_t)(intptr_t)(%s)", raw);
                            buf_putc(&cast, '\0');
                            free(raw);
                            raw = strdup(cast.data);
                            buf_free(&cast);
                        }
                    }
                    arg_strs[i] = raw;
                }
                Buf out; buf_init(&out);
                buf_printf(&out, "%s.fn(%s.env", fn_name, fn_name);
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    buf_printf(&out, ", (int64_t)(%s)", arg_strs[i]);
                }
                buf_puts(&out, ")");
                buf_putc(&out, '\0');
                char *result = strdup(out.data);
                buf_free(&out);
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) free(arg_strs[i]);
                free(arg_strs);
                free(fn_name);
                return result;
            }

            if (fn_binding->closure_fn_binding) {
                /* This is a closure - emit call to thunk function with closure value as first arg */
                Binding *thunk_binding = fn_binding->closure_fn_binding;
                char *thunk_name = raw_name_for_binding(thunk_binding);
                
                /* Closure value is the env struct variable */
                char *closure_val = name_for_binding(ctx, fn_binding);
                
                char **arg_strs = (char **)malloc((e->as.call_.n_args + 1) * sizeof(char *));
                if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
                
                /* First arg is the closure value (env struct) - pass by address for now */
                arg_strs[0] = closure_val;
                
                /* Rest of the args */
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    arg_strs[i + 1] = emit_value(ctx, body, e->as.call_.args[i]);
                }
                
                Buf out; buf_init(&out);
                buf_printf(&out, "%s(", thunk_name);
                for (uint32_t i = 0; i <= e->as.call_.n_args; i++) {
                    if (i > 0) buf_puts(&out, ", ");
                    buf_printf(&out, "%s", arg_strs[i]);
                }
                buf_puts(&out, ")");
                buf_putc(&out, '\0');
                char *result = strdup(out.data);
                buf_free(&out);
                for (uint32_t i = 0; i <= e->as.call_.n_args; i++) free(arg_strs[i]);
                free(arg_strs);
                free(thunk_name);
                return result;
            }

            /* Callback call through ptr<void> parameter. */
            if (fn_binding->type.kind == TY_PTR_VOID) {
                char *fn_ptr = name_for_binding(ctx, fn_binding);
                uint32_t n = e->as.call_.n_args;
                if (n == 0) {
                    /* Original 0-arg path: treat fn_ptr as a function pointer directly */
                    Buf out; buf_init(&out);
                    buf_printf(&out, "((%s (*)(void))%s)()",
                               type_c_name(e->type), fn_ptr);
                    buf_putc(&out, '\0');
                    char *result = strdup(out.data);
                    buf_free(&out);
                    free(fn_ptr);
                    return result;
                } else {
                    /* CY2: Fat-closure dynamic dispatch with n_args > 0.
                     * The fat closure has layout: struct { int64_t __fn; ... }
                     * Extract __fn as a function pointer and call it with fat_ptr + args. */
                    const char *ret_c = type_c_name(e->type);
                    char **arg_strs = (char **)malloc(n * sizeof(char *));
                    if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
                    for (uint32_t i = 0; i < n; i++) {
                        arg_strs[i] = emit_value(ctx, body, e->as.call_.args[i]);
                    }
                    Buf out; buf_init(&out);
                    /* Cast the __fn field to the thunk signature and call it. */
                    buf_printf(&out, "((%s (*)(void*", ret_c);
                    for (uint32_t i = 0; i < n; i++) {
                        buf_printf(&out, ", %s", type_c_name(e->as.call_.args[i]->type));
                    }
                    buf_printf(&out, "))(intptr_t)((int64_t *)(%s))[0])(%s", fn_ptr, fn_ptr);
                    for (uint32_t i = 0; i < n; i++) {
                        buf_printf(&out, ", %s", arg_strs[i]);
                    }
                    buf_puts(&out, ")");
                    buf_putc(&out, '\0');
                    char *result = strdup(out.data);
                    buf_free(&out);
                    for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
                    free(arg_strs);
                    free(fn_ptr);
                    return result;
                }
            }
            
            /* ER2: Callback call through a local TY_FN parameter.
             * When a function parameter is annotated with :(fn [...] #{} :T), it is
             * stored as int64_t in C (a function address or closure pointer cast to
             * int).  Calling it requires the same cast-and-invoke pattern as TY_PTR_VOID
             * callbacks.  Only applies to non-global bindings (local params). */
            if (fn_binding->type.kind == TY_FN && !fn_binding->is_global) {
                char *fn_ptr = name_for_binding(ctx, fn_binding);
                uint32_t n = e->as.call_.n_args;
                const char *ret_c = type_c_name(e->type);
                if (n == 0) {
                    Buf out; buf_init(&out);
                    buf_printf(&out, "((%s (*)(void))(intptr_t)%s)()",
                               ret_c, fn_ptr);
                    buf_putc(&out, '\0');
                    char *result = strdup(out.data);
                    buf_free(&out);
                    free(fn_ptr);
                    return result;
                } else {
                    char **arg_strs = (char **)malloc(n * sizeof(char *));
                    if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
                    for (uint32_t i = 0; i < n; i++) {
                        arg_strs[i] = emit_value(ctx, body, e->as.call_.args[i]);
                    }
                    Buf out; buf_init(&out);
                    buf_printf(&out, "((%s (*)(", ret_c);
                    for (uint32_t i = 0; i < n; i++) {
                        if (i > 0) buf_puts(&out, ", ");
                        buf_puts(&out, type_c_name(e->as.call_.args[i]->type));
                    }
                    buf_printf(&out, "))(intptr_t)%s)(", fn_ptr);
                    for (uint32_t i = 0; i < n; i++) {
                        if (i > 0) buf_puts(&out, ", ");
                        buf_puts(&out, arg_strs[i]);
                    }
                    buf_puts(&out, ")");
                    buf_putc(&out, '\0');
                    char *result = strdup(out.data);
                    buf_free(&out);
                    for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
                    free(arg_strs);
                    free(fn_ptr);
                    return result;
                }
            }

            /* Phase G0: 0-arg constructor call — emit ctor_Name() */
            if (fn_binding->type.kind == TY_ADT) {
                Buf out; buf_init(&out);
                buf_printf(&out, "ctor_%s()", fn_binding->name->name);
                buf_putc(&out, '\0');
                char *result = strdup(out.data);
                buf_free(&out);
                return result;
            }

            /* Phase G0: N-arg constructor call — fn has TY_FN w/ result TY_ADT,
             * but the fn name is the constructor name so we emit ctor_Name(args).
             * Phase G3: result_full_type is set for non-constructor ADT-returning
             * functions (e.g. equal-sym) — those must fall through to regular calls. */
            if (fn_binding->type.kind == TY_FN &&
                fn_binding->type.as.fn.result_kind == TY_ADT &&
                !fn_binding->type.as.fn.result_full_type) {
                char **arg_strs = (char **)malloc(e->as.call_.n_args * sizeof(char *));
                if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    arg_strs[i] = emit_value(ctx, body, e->as.call_.args[i]);
                }
                Buf out; buf_init(&out);
                buf_printf(&out, "ctor_%s(", fn_binding->name->name);
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    if (i > 0) buf_puts(&out, ", ");
                    buf_puts(&out, arg_strs[i]);
                }
                buf_puts(&out, ")");
                buf_putc(&out, '\0');
                char *result = strdup(out.data);
                buf_free(&out);
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) free(arg_strs[i]);
                free(arg_strs);
                return result;
            }

            /* Regular function call */
            /* Emit function call: fn(arg1, arg2, ...) */
            /* Use raw name for function calls (functions are defined with raw names) */
            char *fn_name = raw_name_for_binding(fn_binding);
            char **arg_strs = (char **)malloc(e->as.call_.n_args * sizeof(char *));
            if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                char *raw = emit_value(ctx, body, e->as.call_.args[i]);
                /* Phase HKT H3/H4: When a function-reference (TY_FN) is passed to an
                 * int64_t parameter, emit an explicit (int64_t)(intptr_t) cast so the
                 * generated C99 code is valid.  Function pointers cannot be implicitly
                 * converted to integers in C99; a reinterpret via intptr_t is needed. */
                /* Phase HKT §5: also cast TY_PTR_VOID (capturing-closure env ptr) to
                 * int64_t when passing to an int64_t parameter. */
                /* Phase HRT1: EX_POLY_WRAP emits a tur_poly_fn_t struct literal;
                 * it must NOT be cast — pass directly as tur_poly_fn_t to poly params. */
                bool needs_fn_cast = (e->as.call_.args[i]->kind != EX_POLY_WRAP) &&
                                     (e->as.call_.args[i]->type.kind == TY_FN ||
                                      e->as.call_.args[i]->type.kind == TY_PTR_VOID);
                /* When param expects void * (TY_PTR_VOID), cast to void * not int64_t.
                 * Passing int64_t to void * is invalid in C99 (-Wint-conversion error). */
                bool cast_to_void_ptr = false;
                if (needs_fn_cast && fn_binding->type.kind == TY_FN) {
                    uint8_t n_fnparams = fn_binding->type.as.fn.arity;
                    uint8_t param_idx = (i < n_fnparams) ? i : (uint32_t)(n_fnparams > 0 ? n_fnparams - 1 : 0);
                    TypeKind pk = fn_binding->type.as.fn.arg_kinds[param_idx];
                    /* Apply cast when param is int64_t in C: TY_INT, opaque TY_STRUCT
                     * (HKT container). */
                    if (pk == TY_INT || pk == TY_STRUCT) {
                        needs_fn_cast = true;
                        cast_to_void_ptr = false;
                    } else if (pk == TY_PTR_VOID) {
                        /* Param is void * — cast to void * via intptr_t, not int64_t.
                         * Needed when a TY_FN (stored as int64_t) is passed as void *;
                         * harmless no-op when a TY_PTR_VOID (already void *) is passed. */
                        needs_fn_cast = true;
                        cast_to_void_ptr = true;
                    } else if (pk == TY_FN) {
                        /* ER2: TY_FN parameter — stored as int64_t in C.  A function
                         * reference (TY_FN) or closure pointer (TY_PTR_VOID) passed to it
                         * must be cast to int64_t via intptr_t. */
                        needs_fn_cast = true;
                        cast_to_void_ptr = false;
                    } else {
                        needs_fn_cast = false;
                    }
                }
                if (needs_fn_cast) {
                    Buf cast; buf_init(&cast);
                    if (cast_to_void_ptr) {
                        buf_printf(&cast, "(void *)(intptr_t)(%s)", raw);
                    } else {
                        buf_printf(&cast, "(int64_t)(intptr_t)(%s)", raw);
                    }
                    buf_putc(&cast, '\0');
                    free(raw);
                    raw = strdup(cast.data);
                    buf_free(&cast);
                }
                /* Phase HRT3: arg is a nested poly fn passed via int64_t pointer —
                 * dereference it back to tur_poly_fn_t for the direct call. */
                if (e->as.call_.poly_arg_mask & (1u << i)) {
                    Buf cast; buf_init(&cast);
                    buf_printf(&cast, "*(tur_poly_fn_t*)(intptr_t)(%s)", raw);
                    buf_putc(&cast, '\0');
                    free(raw);
                    raw = strdup(cast.data);
                    buf_free(&cast);
                }
                arg_strs[i] = raw;
            }
            Buf out; buf_init(&out);
            buf_printf(&out, "%s(", fn_name);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                if (i > 0) buf_puts(&out, ", ");
                buf_printf(&out, "%s", arg_strs[i]);
            }
            buf_puts(&out, ")");
            buf_putc(&out, '\0');
            char *result = strdup(out.data);
            buf_free(&out);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) free(arg_strs[i]);
            free(arg_strs);
            free(fn_name);
            return result;
        }
        case EX_EXTERN_C: {
            /* extern-c declarations emit nothing in value position (they're file-scope) */
            return atom_nil();
        }
        case EX_INLINE_C: {
            /* Return the inline C code as a value string (with __TUR_CAP_N__ /
             * __TUR_VAL_N__ substitution).  Statement-position use goes through
             * emit_stmt which handles EX_INLINE_C separately. */
            InlineC *ic = e->as.inline_c_.inline_c;
            return inline_c_substitute(ctx, body, ic);
        }
        /* Phase 3 */
        /* Phase 4 */
        case EX_DEFER: {
            /* Defer in value position: emit as nil (defer evaluates to nil) */
            /* The body is emitted as a statement in emit_stmt */
            return atom_nil();
        }
        case EX_RETURN: {
            /* (return expr) or (return) - in value position, just emit the value expression
             * The return statement itself will be emitted by emit_stmt */
            if (e->as.return_.value) {
                return emit_value(ctx, body, e->as.return_.value);
            } else {
                /* return with no value - this is only valid in void functions */
                /* Emit a placeholder */
                return atom_nil();
            }
        }
        /* Phase 5 */
        case EX_REF: {
            /* (ref expr) - allocate on heap and return pointer */
            /* ref<T> lowers to void* in C for simplicity in v1 */
            char *inner = emit_value(ctx, body, e->as.ref_.expr);
            
            /* Emit: malloc + store value */
            char *tmp = fresh_tmp(ctx);
            char *inner_type_c = strdup(type_c_name(e->as.ref_.expr->type));
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s %s = malloc(sizeof(%s));\n", 
                       type_c_name(e->type), tmp, inner_type_c);
            indent_buf(body, ctx->indent);
            buf_printf(body, "*((%s *)%s) = %s;\n", inner_type_c, tmp, inner);
            free(inner);
            free(inner_type_c);
            /* Mark that we need stdlib.h for malloc/free */
            /* We'll use a flag in the EmitCtx - but ctx is passed by pointer */
            /* For now, we'll just always include stdlib.h when we see a ref */
            /* This is a simplification - in production we'd track this properly */
            return tmp;
        }
        case EX_DEREF: {
            /* (@ expr) - dereference ref<T>, rc<T>, ptr<T>, &T, or &mut T */
            char *inner = emit_value(ctx, body, e->as.deref_.expr);
            
            /* For ref<T> / lref<T>, we need to cast and dereference */
            if (e->as.deref_.expr->type.kind == TY_REF
                || e->as.deref_.expr->type.kind == TY_LREF) {
                char *inner_type_c = strdup(type_c_name(e->type));
                char *tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s = *((%s *)%s);\n", 
                           inner_type_c, tmp, inner_type_c, inner);
                free(inner);
                free(inner_type_c);
                return tmp;
            } else if (e->as.deref_.expr->type.kind == TY_REF_IMMUT
                       || e->as.deref_.expr->type.kind == TY_REF_MUT) {
                /* Phase 12: &T / &mut T dereference: *((T *)ptr) */
                const char *inner_type_c = type_c_name(e->type);
                char *tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s = *((%s *)%s);\n",
                           inner_type_c, tmp, inner_type_c, inner);
                free(inner);
                return tmp;
            } else {
                /* For ptr<T>, just cast to the appropriate type and dereference */
                /* For now, ptr<void> stays as void* */
                return inner;
            }
        }
        /* Phase 3 */
        case EX_CLOSURE: {
            /* Emit closure: the closure value IS the env struct (passed by value to thunk) */
            struct Closure *closure = e->as.closure_.closure;
            const Symbol *env_name = closure->env_name;
            
            /* Emit env struct type definition at file scope if not already emitted */
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
                /* Track this env struct */
                if (ctx->n_env_struct_names >= ctx->cap_env_struct_names) {
                    ctx->cap_env_struct_names = ctx->cap_env_struct_names ? ctx->cap_env_struct_names * 2 : 8;
                    ctx->env_struct_names = (const Symbol **)realloc(ctx->env_struct_names,
                        ctx->cap_env_struct_names * sizeof(const Symbol *));
                }
                ctx->env_struct_names[ctx->n_env_struct_names++] = env_name;
                
                /* Phase HKT §5: fat closure struct — __fn first, then captures. */
                buf_printf(ctx->file, "struct %s { int64_t __fn; ", env_name->name);
                for (uint8_t i = 0; i < closure->n_captures; i++) {
                    Binding *captured = closure->captures[i];
                    buf_printf(ctx->file, "%s %s; ",
                               type_c_name(captured->type), captured->name->name);
                }
                buf_puts(ctx->file, "};\n");
            }
            
            /* Phase HKT §5: heap-allocate the fat closure struct so that the
             * fat pointer can safely escape from the local stack frame and be
             * passed to HKT helpers as an opaque int64_t.  Layout:
             *   struct __env_N { int64_t __fn; <captures...> }
             * The __fn field holds the thunk pointer (as int64_t).  Callers
             * using the generic fat-closure protocol recover the thunk as:
             *   thunk = (int64_t(*)(void*,int64_t))(intptr_t)fat->__fn
             * and invoke it as thunk(fat_ptr, arg). */
            const char *thunk_sym = closure->fn->binding->name->name;
            char *fat_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "struct %s *%s = (struct %s *)malloc(sizeof(struct %s));\n",
                       env_name->name, fat_tmp, env_name->name, env_name->name);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s->__fn = (int64_t)(intptr_t)%s;\n", fat_tmp, thunk_sym);
            for (uint8_t i = 0; i < closure->n_captures; i++) {
                Binding *captured = closure->captures[i];
                char *cn = name_for_binding(ctx, captured);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s->%s = %s;\n", fat_tmp, captured->name->name, cn);
                free(cn);
            }
            char *ptr_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "void *%s = %s;\n", ptr_tmp, fat_tmp);
            
            return ptr_tmp;
        }
        /* Phase 9: rc<T> + weak<T> operations */
        case EX_RC_OF: {
            /* (rc/of x) - allocate control block, copy value into it */
            char *inner = emit_value(ctx, body, e->as.rc_of_.expr);
            char *inner_type_c = strdup(type_c_name(e->as.rc_of_.expr->type));
            
            /* Emit: allocate value separately, then attach it to rc control block. */
            char *val_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s *%s = (%s *)malloc(sizeof(%s));\n",
                       inner_type_c, val_tmp, inner_type_c, inner_type_c);
            indent_buf(body, ctx->indent);
            buf_printf(body, "*%s = %s;\n", val_tmp, inner);

            char *cb_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            /* Phase 11: if the inner value is a struct with RC fields, pass its drop glue */
            const char *drop_fn_name = "NULL";
            char dg_name_buf[256];
            if (e->as.rc_of_.expr->type.kind == TY_STRUCT) {
                StructDef *sdef = e->as.rc_of_.expr->type.as.struct_.def;
                if (sdef && sdef->needs_drop_glue) {
                    snprintf(dg_name_buf, sizeof(dg_name_buf), "drop_glue_%s", sdef->name);
                    drop_fn_name = dg_name_buf;
                }
            }
            buf_printf(body, "RcControlBlock *%s = rc_cb_alloc(0, %d, %s);\n",
                       cb_tmp, e->as.rc_of_.expr->type.kind, drop_fn_name);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s->value = %s;\n", cb_tmp, val_tmp);

            free(inner);
            free(inner_type_c);
            free(val_tmp);
            return cb_tmp;
        }
        case EX_RC_CLONE: {
            /* (rc/clone r) - increment strong count, return same cb */
            char *inner = emit_value(ctx, body, e->as.rc_clone_.expr);
            if (e->as.rc_clone_.elide) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "/* rc-elision: skipped rc_strong_increment(%s) \xe2\x80\x94 last-use clone */\n", inner);
            } else {
                indent_buf(body, ctx->indent);
                buf_printf(body, "rc_strong_increment(%s);\n", inner);
            }
            /* Return the same pointer (now with incremented count, or elided) */
            return strdup(inner);
        }
        case EX_RC_DROP: {
            /* (rc/drop r) - decrement strong count */
            char *inner = emit_value(ctx, body, e->as.rc_drop_.expr);
            if (e->as.rc_drop_.elide) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "/* rc-elision: skipped rc_strong_decrement(%s) \xe2\x80\x94 matched elided clone */\n", inner);
            } else {
                indent_buf(body, ctx->indent);
                buf_printf(body, "rc_strong_decrement(%s);\n", inner);
                indent_buf(body, ctx->indent);
                buf_printf(body, "rc_free_queue_drain();\n");
            }
            free(inner);
            return atom_nil();
        }
        case EX_RC_PTR: {
            /* (rc->ptr r) - borrow ptr<T> from rc<T> */
            char *inner = emit_value(ctx, body, e->as.rc_ptr_.expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "void *%s = rc_get_value(%s);\n", tmp, inner);
            free(inner);
            return tmp;
        }
        case EX_RC_COUNT: {
            /* (rc/strong-count r) - get strong count */
            char *inner = emit_value(ctx, body, e->as.rc_count_.expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "int64_t %s = rc_strong_count(%s);\n", tmp, inner);
            free(inner);
            return tmp;
        }
        case EX_RC_FROM_REF: {
            /* (rc/from-ref r) - move ref<T> into rc<T> */
            char *inner = emit_value(ctx, body, e->as.rc_from_ref_.expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "RcControlBlock *%s = tur_rc_from_ref(%s, %d);\n",
                       tmp, inner, e->type.as.rc.inner);
            free(inner);
            return tmp;
        }
        case EX_REF_FROM_RC: {
            /* (ref/from-rc r) - extract unique ref<T> from rc<T> */
            char *inner = emit_value(ctx, body, e->as.ref_from_rc_.expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "void *%s = tur_ref_from_rc(%s);\n", tmp, inner);
            free(inner);
            return tmp;
        }
        case EX_WEAK: {
            /* (weak r) - create weak<T> from rc<T> */
            char *inner = emit_value(ctx, body, e->as.weak_.expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            /* weak shares the same control block, increments weak count */
            buf_printf(body, "RcControlBlock *%s = %s; rc_weak_increment(%s);\n", tmp, inner, inner);
            free(inner);
            return tmp;
        }
        case EX_WEAK_UPGRADE: {
            /* (upgrade w) - upgrade weak<T> to option<rc<T>> */
            char *inner = emit_value(ctx, body, e->as.weak_upgrade_.expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            /* Try to upgrade - returns the same cb if alive, NULL otherwise */
            buf_printf(body, "RcControlBlock *%s = rc_upgrade(%s);\n", tmp, inner);
            free(inner);
            return tmp;
        }
        case EX_WEAK_PRED: {
            /* (weak? w) - check if w is weak<T> */
            char *inner = emit_value(ctx, body, e->as.weak_pred_.expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            /* Check if the type kind is TY_WEAK - but at runtime we can't easily check this */
            /* For Phase 9, we'll use a simple approach: check if it's a RcControlBlock pointer */
            /* This is a simplification - in a proper implementation we'd need type info */
            buf_printf(body, "bool %s = true; // weak? not fully implemented in Phase 9\n", tmp);
            free(inner);
            return tmp;
        }
        case EX_REF_PRED: {
            /* (ref? x) - check if x is ref<T>: always true if elaboration accepted it */
            char *inner = emit_value(ctx, body, e->as.ref_pred_.expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            /* ref<T> is a void* in C; check non-NULL */
            buf_printf(body, "bool %s = (%s != NULL); /* ref? — non-null pointer check */\n", tmp, inner);
            free(inner);
            return tmp;
        }
        case EX_CONT_PRED: {
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
        /* Phase T21-F: async/await */
        case EX_ASYNC: {
            /* (async fn-expr) — launch fn-expr in a fiber, return TurFuture* as ptr<void>.
             *
             * Two paths:
             *  (a) fn-expr has type TY_FN (a no-arg function): use it as a function pointer
             *      directly (existing behaviour).
             *  (b) fn-expr is an expression (e.g., (handle ...) from (with-handler ...)):
             *      T25 — wrap in a static thunk emitted at file scope, then call tur_async_fiber
             *      with the thunk.  v1 limitation: the expression must not capture outer-scope
             *      variables (same limitation as effect handler bodies). */
            const Expr *fn_expr = e->as.async_.fn_expr;
            char *tmp = fresh_tmp(ctx);
            if (fn_expr->type.kind == TY_FN) {
                /* Path (a): fn-expr is already a function pointer */
                char *fn_val = emit_value(ctx, body, fn_expr);
                indent_buf(body, ctx->indent);
                buf_printf(body, "void *%s = (void *)tur_async_fiber((int64_t(*)(void))(intptr_t)%s);\n", tmp, fn_val);
                free(fn_val);
            } else {
                /* Path (b): wrap the expression in a static thunk (T25 with-handler support).
                 *
                 * Emission order is important: any nested handler functions (emitted to
                 * pending_handler_fns) must appear BEFORE the thunk's opening brace in the
                 * output.  We achieve this by:
                 *   1. Emit the forward declaration to pbuf.
                 *   2. Emit the thunk body into a separate temporary buffer (thunk_body_buf),
                 *      routing pending_handler_fns to pbuf so nested handler fns land there.
                 *   3. Write the thunk definition (brace + body buf + return + close) to pbuf.
                 */
                char *thunk_name = (char *)malloc(32);
                snprintf(thunk_name, 32, "__async_thunk_%d", ctx->tmp_n++);

                Buf *pbuf = ctx->pending_handler_fns;

                /* Step 1: forward declaration */
                buf_printf(pbuf, "static int64_t %s(void);\n", thunk_name);

                /* Step 2: emit thunk body into a separate buffer */
                Buf thunk_body_buf;
                buf_init(&thunk_body_buf);

                EmitCtx tctx = *ctx;
                tctx.file = pbuf;
                tctx.pending_handler_fns = pbuf; /* nested handler fns → pbuf (file scope) */
                tctx.indent = 4;
                tctx.fn_params = NULL;
                tctx.n_fn_params = 0;
                tctx.closure = NULL;
                tctx.env_var_name = NULL;
                tctx.defer_captures = NULL;
                tctx.n_defer_captures = 0;
                tctx.frame_var = NULL;
                tctx.return_emitted = false;

                char *ret = NULL;
                if (fn_expr->type.kind == TY_NIL || fn_expr->type.kind == TY_NEVER) {
                    emit_stmt(&tctx, &thunk_body_buf, fn_expr);
                } else {
                    ret = emit_value(&tctx, &thunk_body_buf, fn_expr);
                }
                ctx->tmp_n = tctx.tmp_n;

                /* Step 3: write thunk definition to pbuf — after nested handler fns */
                buf_printf(pbuf, "static int64_t %s(void) {\n", thunk_name);
                if (thunk_body_buf.len > 0)
                    buf_write(pbuf, thunk_body_buf.data, thunk_body_buf.len);
                if (ret) {
                    buf_printf(pbuf, "    return (int64_t)%s;\n", ret);
                    free(ret);
                } else {
                    buf_puts(pbuf, "    return 0;\n");
                }
                buf_puts(pbuf, "}\n\n");
                buf_free(&thunk_body_buf);

                indent_buf(body, ctx->indent);
                buf_printf(body, "void *%s = (void *)tur_async_fiber(%s);\n", tmp, thunk_name);
                free(thunk_name);
            }
            return tmp;
        }
        case EX_AWAIT: {
            /* (await fut) — await a future using shift + scheduler callback */
            char *fut_val = emit_value(ctx, body, e->as.await_.fut_expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "int64_t %s = tur_await_future((TurFuture*)(intptr_t)%s);\n", tmp, fut_val);
            free(fut_val);
            return tmp;
        }
        /* Phase SEL1: fair multi-channel select */
        case EX_SELECT: {
            /*
             * (select ((ch :recv v) body) ... (:default body))
             *
             * Generates:
             *   TurSelectClause __sel_N[n];
             *   __sel_N[0].chan = (void*)(intptr_t)<ch_val>;
             *   __sel_N[0].op  = 0;  // recv
             *   __sel_N[0].val = 0;
             *   ...
             *   int64_t __sel_idx_N = (int64_t)tur_select_blocking(__sel_N, n, has_default);
             *   int64_t __sel_result_N;
             *   if (__sel_idx_N == 0) { int64_t v = __sel_N[0].val; __sel_result_N = <body0>; }
             *   else if (__sel_idx_N == 1) { ... }
             *   else { __sel_result_N = <default_body>; }
             */
            uint32_t n    = e->as.select_.n_clauses;
            int has_def   = e->as.select_.has_default;
            const SelectClauseEntry *cls = e->as.select_.clauses;

            /* Allocate unique suffix for this select to avoid name collisions */
            int sel_id = ctx->tmp_n++;

            char sel_arr[32], sel_idx[32], sel_res[32];
            snprintf(sel_arr, sizeof(sel_arr), "__sel_%d",     sel_id);
            snprintf(sel_idx, sizeof(sel_idx), "__sel_idx_%d", sel_id);
            snprintf(sel_res, sizeof(sel_res), "__sel_res_%d", sel_id);

            /* Declare clause array */
            indent_buf(body, ctx->indent);
            buf_printf(body, "TurSelectClause %s[%u];\n", sel_arr, n > 0 ? n : 1);

            /* Fill clause array */
            for (uint32_t ci = 0; ci < n; ci++) {
                const SelectClauseEntry *cl = &cls[ci];

                /* Emit channel expression */
                char *ch_val = emit_value(ctx, body, cl->chan);

                indent_buf(body, ctx->indent);
                buf_printf(body, "%s[%u].chan = (void *)(intptr_t)%s;\n",
                           sel_arr, ci, ch_val);
                free(ch_val);

                indent_buf(body, ctx->indent);
                buf_printf(body, "%s[%u].op = %d;\n", sel_arr, ci, cl->op);

                if (cl->op == 1 && cl->send_val) {
                    /* send: fill val field before call */
                    char *sv = emit_value(ctx, body, cl->send_val);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s[%u].val = (int64_t)%s;\n", sel_arr, ci, sv);
                    free(sv);
                } else {
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s[%u].val = 0;\n", sel_arr, ci);
                }
            }

            /* Call tur_select_blocking */
            indent_buf(body, ctx->indent);
            buf_printf(body,
                "int64_t %s = (int64_t)tur_select_blocking(%s, %d, %d);\n",
                sel_idx, sel_arr, (int)n, has_def);

            /* Declare result variable (zero-initialized to silence -Wuninitialized) */
            const char *res_ctype = type_c_name(e->type);
            bool has_result = (e->type.kind != TY_NIL && e->type.kind != TY_NEVER);
            if (has_result) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s = 0;\n", res_ctype, sel_res);
            }

            /* Dispatch: if-else chain over clause index */
            for (uint32_t ci = 0; ci < n; ci++) {
                const SelectClauseEntry *cl = &cls[ci];

                indent_buf(body, ctx->indent);
                if (ci == 0) {
                    buf_printf(body, "if (%s == %d) {\n", sel_idx, (int)ci);
                } else {
                    buf_printf(body, "} else if (%s == %d) {\n", sel_idx, (int)ci);
                }
                ctx->indent += 4;

                if (cl->op == 0 && cl->recv_binding) {
                    /* recv: declare binding with the received value */
                    char *bname = name_for_binding(ctx, cl->recv_binding);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "int64_t %s = %s[%u].val;\n",
                               bname, sel_arr, ci);
                    free(bname);
                }

                if (has_result) {
                    char *bv = emit_value(ctx, body, cl->body);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s = (%s)%s;\n", sel_res, res_ctype, bv);
                    free(bv);
                } else {
                    emit_stmt(ctx, body, cl->body);
                }

                ctx->indent -= 4;
            }

            /* Default branch */
            if (has_def) {
                indent_buf(body, ctx->indent);
                if (n == 0) {
                    buf_printf(body, "if (1) {\n");
                } else {
                    buf_puts(body, "} else {\n");
                }
                ctx->indent += 4;
                if (has_result) {
                    char *dv = emit_value(ctx, body, e->as.select_.default_body);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s = (%s)%s;\n", sel_res, res_ctype, dv);
                    free(dv);
                } else {
                    emit_stmt(ctx, body, e->as.select_.default_body);
                }
                ctx->indent -= 4;
            }

            /* Close final brace */
            if (n > 0 || has_def) {
                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");
            }

            if (has_result) {
                char *tmp = strdup(sel_res);
                return tmp;
            } else {
                char *tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "int64_t %s = 0;\n", tmp);
                return tmp;
            }
        }
        /* Phase 20: Software Transactional Memory */
        case EX_STM: {
            /* (stm expr1 expr2 ...) - STM blocks are only valid inside atomically */
            /* The EX_ATOMICALLY case handles the emission, so we should not reach here */
            /* For now, emit as a no-op */
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "/* STM block (should be inside atomically) */ void *%s = NULL;\n", tmp);
            return tmp;
        }
        case EX_ATOMICALLY: {
            /* (atomically stm-expr) - execute the stm block atomically */
            /* Emit the transaction loop inline, with the STM expressions inside */
            char *result = fresh_tmp(ctx);
            Type result_type = e->type;
            
            /* Declare result variable - but void type is not allowed in C */
            if (result_type.kind != TY_NIL) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s;\n", type_c_name(result_type), result);
            }
            
            /* atomically block */
            indent_buf(body, ctx->indent);
            buf_printf(body, "/* atomically */\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "{\n");
            ctx->indent += 4;
            
            /* Create transaction */
            indent_buf(body, ctx->indent);
            buf_puts(body, "STM_Transaction *__tx = tur_stm_new_transaction();\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "STM_Transaction *__prev_tx = tur_stm_current_tx();\n");
            
            /* Transaction loop */
            indent_buf(body, ctx->indent);
            buf_puts(body, "while (1) {\n");
            ctx->indent += 4;
            
            indent_buf(body, ctx->indent);
            buf_puts(body, "__tx->retry_requested = false;\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "__tx->aborted = false;\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "__tx->read_count = 0;\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "__tx->write_count = 0;\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "tur_stm_set_current_tx(__tx);\n");
            
            /* Emit STM block body directly */
            const Expr *stm_expr = e->as.atomically_.stm_expr;
            char *stm_last_val = NULL;
            if (stm_expr->as.stm_.n_body > 0) {
                for (uint32_t i = 0; i < stm_expr->as.stm_.n_body; i++) {
                    const Expr *expr = stm_expr->as.stm_.body[i];
                    if (i < stm_expr->as.stm_.n_body - 1) {
                        /* Not the last expression - emit as statement */
                        emit_stmt(ctx, body, expr);
                    } else {
                        /* Last expression - emit as value */
                        if (expr->type.kind != TY_NIL && expr->type.kind != TY_NEVER) {
                            stm_last_val = emit_value(ctx, body, expr);
                        } else {
                            emit_stmt(ctx, body, expr);
                        }
                    }
                }
            }
            
            /* Check if retry is needed */
            indent_buf(body, ctx->indent);
            buf_puts(body, "if (__tur_stm_should_retry(__tx)) {\n");
            ctx->indent += 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "tur_stm_set_current_tx(__prev_tx);\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "continue;\n");
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            
            /* Commit */
            indent_buf(body, ctx->indent);
            buf_puts(body, "if (tur_stm_commit(__tx)) {\n");
            ctx->indent += 4;
            if (result_type.kind != TY_NIL && stm_last_val) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = %s;\n", result, stm_last_val);
                free(stm_last_val);
            } else {
                indent_buf(body, ctx->indent);
                buf_puts(body, "/* STM block returned nil or void */\n");
            }
            indent_buf(body, ctx->indent);
            buf_puts(body, "tur_stm_set_current_tx(__prev_tx);\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "free(__tx);\n");
            indent_buf(body, ctx->indent);
            buf_puts(body, "break;\n");
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            
            /* Retry */
            indent_buf(body, ctx->indent);
            buf_puts(body, "tur_stm_set_current_tx(__prev_tx);\n");
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            
            return result;
        }
        case EX_RETRY: {
            /* (retry) - request transaction retry */
            indent_buf(body, ctx->indent);
            buf_puts(body, "tur_stm_retry(tur_stm_current_tx());\n");
            return atom_nil();
        }
        case EX_CHECK: {
            /* (check cond) - abort if condition is false */
            char *cond_val = emit_value(ctx, body, e->as.check_.cond);
            indent_buf(body, ctx->indent);
            buf_printf(body, "tur_stm_check(%s);\n", cond_val);
            free(cond_val);
            return atom_nil();
        }
        case EX_OR_ELSE: {
            /* (or-else stm1 stm2) - try stm1, retry with stm2 on retry */
            /* Simplified: emit stm1, if retry then emit stm2 */
            /* Note: This is a simplified implementation that doesn't properly handle
             * the retry semantics. A full implementation would need to track
             * which branch was taken. */
            char *result = fresh_tmp(ctx);
            Type result_type = e->type;
            
            if (result_type.kind != TY_NIL) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s;\n", type_c_name(result_type), result);
            }
            
            indent_buf(body, ctx->indent);
            buf_puts(body, "/* or-else */\n");
            
            /* Save retry state before stm1 */
            indent_buf(body, ctx->indent);
            buf_puts(body, "bool __or_else_retry_before = tur_stm_current_tx()->retry_requested;\n");
            
            /* Emit stm1 */
            if (e->as.or_else_.stm1->type.kind != TY_NIL) {
                char *stm1_val = emit_value(ctx, body, e->as.or_else_.stm1);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = %s;\n", result, stm1_val);
                free(stm1_val);
            } else {
                emit_stmt(ctx, body, e->as.or_else_.stm1);
            }
            
            /* If stm1 caused retry, clear it and emit stm2 */
            indent_buf(body, ctx->indent);
            buf_puts(body, "if (!__or_else_retry_before && tur_stm_current_tx()->retry_requested) {\n");
            ctx->indent += 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "tur_stm_current_tx()->retry_requested = false;\n");
            if (e->as.or_else_.stm2->type.kind != TY_NIL) {
                char *stm2_val = emit_value(ctx, body, e->as.or_else_.stm2);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = %s;\n", result, stm2_val);
                free(stm2_val);
            } else {
                emit_stmt(ctx, body, e->as.or_else_.stm2);
            }
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            
            return result;
        }
        case EX_TVAR_NEW: {
            /* (TVar::new init) - create a new TVar */
            char *init_val = emit_value(ctx, body, e->as.tvar_new_.init);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            /* For now, we emit a malloc-based TVar - proper impl needs TypeInfo */
            buf_printf(body, "TVar *%s = tur_tvar_new(NULL, (void*)(intptr_t)%s);\n", tmp, init_val);
            free(init_val);
            return tmp;
        }
        case EX_TVAR_READ: {
            /* (TVar::read tvar) - read a TVar within a transaction */
            char *tvar_val = emit_value(ctx, body, e->as.tvar_read_.tvar);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "void *%s = tur_tvar_read(tur_stm_current_tx(), (TVar*)%s);\n", tmp, tvar_val);
            free(tvar_val);
            return tmp;
        }
        case EX_TVAR_WRITE: {
            /* (TVar::write tvar value) - write to a TVar within a transaction */
            char *tvar_val = emit_value(ctx, body, e->as.tvar_write_.tvar);
            char *val_val = emit_value(ctx, body, e->as.tvar_write_.value);
            indent_buf(body, ctx->indent);
            buf_printf(body, "tur_tvar_write(tur_stm_current_tx(), (TVar*)%s, (void*)(intptr_t)%s);\n", tvar_val, val_val);
            free(tvar_val);
            free(val_val);
            return atom_nil();
        }
        case EX_TVAR_MODIFY: {
            /* (TVar::modify tvar fn) - modify a TVar */
            char *tvar_val = emit_value(ctx, body, e->as.tvar_modify_.tvar);
            char *fn_val = emit_value(ctx, body, e->as.tvar_modify_.fn);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            /* Simplified: emit as read-modify-write */
            buf_printf(body, "/* TVar::modify %s %s */ void *%s = NULL;\n", tvar_val, fn_val, tmp);
            free(tvar_val);
            free(fn_val);
            return tmp;
        }
        case EX_TVAR_SWAP: {
            /* (TVar::swap tvar new) - swap value and return old */
            char *tvar_val = emit_value(ctx, body, e->as.tvar_swap_.tvar);
            char *new_val = emit_value(ctx, body, e->as.tvar_swap_.new_val);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "void *%s = tur_tvar_swap(tur_stm_current_tx(), (TVar*)%s, (void*)(intptr_t)%s);\n", tmp, tvar_val, new_val);
            free(tvar_val);
            free(new_val);
            return tmp;
        }
        case EX_TVAR_CAS: {
            /* (TVar::cas tvar old new) - compare-and-swap */
            char *tvar_val = emit_value(ctx, body, e->as.tvar_cas_.tvar);
            char *old_val = emit_value(ctx, body, e->as.tvar_cas_.old_val);
            char *new_val = emit_value(ctx, body, e->as.tvar_cas_.new_val);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "bool %s = tur_tvar_cas(tur_stm_current_tx(), (TVar*)%s, (void*)(intptr_t)%s, (void*)(intptr_t)%s);\n", tmp, tvar_val, old_val, new_val);
            free(tvar_val);
            free(old_val);
            free(new_val);
            return tmp;
        }
        /* Phase 12: Borrow traits */
        case EX_BORROW_IMMUT: {
            /* (& expr) - immutable borrow */
            Type inner_type = e->as.borrow_immut_.expr->type;
            char *inner = emit_value(ctx, body, e->as.borrow_immut_.expr);
            if (inner_type.kind == TY_REF_IMMUT || inner_type.kind == TY_REF_MUT) {
                /* Reborrow: the pointer value IS the borrow — return as-is */
                return inner;
            } else if (inner_type.kind == TY_REF) {
                /* Borrow from ref<T>: ref<T> is a void* pointing to T */
                char *result = (char *)malloc(strlen(inner) + 24);
                if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
                snprintf(result, strlen(inner) + 24, "((const void *)%s)", inner);
                free(inner);
                return result;
            } else {
                /* Plain value borrow: take address */
                char *result = (char *)malloc(strlen(inner) + 10);
                if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
                snprintf(result, strlen(inner) + 10, "&%s", inner);
                free(inner);
                return result;
            }
        }
        case EX_BORROW_MUT: {
            /* (&mut expr) - mutable borrow */
            Type inner_type = e->as.borrow_mut_.expr->type;
            char *inner = emit_value(ctx, body, e->as.borrow_mut_.expr);
            if (inner_type.kind == TY_REF_MUT) {
                /* Mutable reborrow: return pointer directly */
                return inner;
            } else if (inner_type.kind == TY_REF) {
                /* Borrow from ref<T>: ref<T> is a void* pointing to T */
                char *result = (char *)malloc(strlen(inner) + 20);
                if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
                snprintf(result, strlen(inner) + 20, "((void *)%s)", inner);
                free(inner);
                return result;
            } else {
                /* Plain value mutable borrow: take address */
                char *result = (char *)malloc(strlen(inner) + 10);
                if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
                snprintf(result, strlen(inner) + 10, "&%s", inner);
                free(inner);
                return result;
            }
        }
        /* Phase 19: Algebraic effects */
        case EX_DEFECT:
            /* Effect definitions are compile-time only - no runtime code */
            return atom_nil();
        /* DV2: Dynamic vars */
        case EX_DEFDYNAMIC:
            /* Declaration is compile-time only; root init is in emit_module.c Pass 2. */
            return atom_nil();
        case EX_DYNVAR_READ: {
            DynVarEntry *dv_entry = e->as.dynvar_read_.entry;
            char *mname = mangle_dynvar_name(dv_entry->name->name);
            const char *vctype = type_c_name(dv_entry->value_type);
            char *ftmp = fresh_tmp(ctx);
            char *rtmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "TurDynFrame *%s = (TurDynFrame *)pthread_getspecific(_dynvar_key_%s);\n",
                       ftmp, mname);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s %s = %s ? *(%s *)%s->value : _dynvar_root_%s;\n",
                       vctype, rtmp, ftmp, vctype, ftmp, mname);
            free(mname);
            free(ftmp);
            return rtmp;
        }
        case EX_DYNVAR_BINDING: {
            const DynBinding *pairs = e->as.dynvar_binding_.pairs;
            uint32_t n_pairs = e->as.dynvar_binding_.n_pairs;
            bool nil_result = (e->type.kind == TY_NIL);
            char *result_tmp = NULL;
            if (!nil_result) {
                result_tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s;\n", type_c_name(e->type), result_tmp);
            }
            indent_buf(body, ctx->indent);
            buf_puts(body, "{\n");
            ctx->indent += 4;
            for (uint32_t pi = 0; pi < n_pairs; pi++) {
                DynVarEntry *dv_entry = pairs[pi].entry;
                char *mname = mangle_dynvar_name(dv_entry->name->name);
                const char *vctype = type_c_name(dv_entry->value_type);
                /* Emit override value */
                char *val = emit_value(ctx, body, pairs[pi].override_expr);
                char *vslot = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s = %s;\n", vctype, vslot, val);
                free(val);
                /* Emit binding frame */
                char *frame = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body,
                    "TurDynFrame %s = { (TurDynFrame *)pthread_getspecific(_dynvar_key_%s), &%s };\n",
                    frame, mname, vslot);
                indent_buf(body, ctx->indent);
                buf_printf(body, "pthread_setspecific(_dynvar_key_%s, &%s);\n", mname, frame);
                /* Cleanup guard pointer -- cleanup fn pops the frame even on longjmp */
                char *fptr = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body,
                    "TurDynFrame *%s __attribute__((cleanup(_dynvar_pop_%s))) = &%s;\n",
                    fptr, mname, frame);
                indent_buf(body, ctx->indent);
                buf_printf(body, "(void)%s;\n", fptr);
                free(mname);
                free(vslot);
                free(frame);
                free(fptr);
            }
            /* Emit body */
            char *bval = emit_value(ctx, body, e->as.dynvar_binding_.body);
            if (!nil_result) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = %s;\n", result_tmp, bval);
            }
            free(bval);
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            return nil_result ? atom_nil() : result_tmp;
        }
        case EX_DYNVAR_SET: {
            DynVarEntry *dv_entry = e->as.dynvar_set_.entry;
            char *mname = mangle_dynvar_name(dv_entry->name->name);
            const char *vctype = type_c_name(dv_entry->value_type);
            char *val = emit_value(ctx, body, e->as.dynvar_set_.value);
            char *ftmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "{ TurDynFrame *%s = (TurDynFrame *)pthread_getspecific(_dynvar_key_%s);\n",
                       ftmp, mname);
            ctx->indent += 2;
            indent_buf(body, ctx->indent);
            buf_printf(body,
                "if (!%s) { fprintf(stderr, \"tur: set! on dynamic var '%s' with no active binding\\n\"); abort(); }\n",
                ftmp, dv_entry->name->name);
            indent_buf(body, ctx->indent);
            buf_printf(body, "*(%s *)%s->value = %s;\n", vctype, ftmp, val);
            ctx->indent -= 2;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            free(mname);
            free(val);
            free(ftmp);
            return atom_nil();
        }
        case EX_PERFORM: {
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
        case EX_HANDLE: {
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
             * Step 7: Inline code — set up fiber and run dispatch.
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

            /* Start the dispatch loop — first call resumes with 0 to start body */
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
             * If fiber is NOT done (k stored), leak env for v1 — it must stay alive
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

            if (returns_value) return result;
            free(result);
            return atom_nil();
        }
        case EX_RESUME: {
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
                    /* MS1: ^multishot k — snapshot before each resume so k stays usable. */
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
        case EX_DISCONTINUE: {
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
        case EX_MAKE_STRUCT: {
            /* (make-struct StructName v1 v2 ...) - emit C99 compound literal */
            StructDef *def = e->as.make_struct_.def;
            Buf lit; buf_init(&lit);
            buf_printf(&lit, "(%s){", def->name);
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++) {
                char *fv = emit_value(ctx, body, e->as.make_struct_.field_values[i]);
                bool is_fn_field = (def->fields[i].kind == TY_FN);
                bool val_is_fn = (e->as.make_struct_.field_values[i]->type.kind == TY_FN);
                if (i > 0) buf_puts(&lit, ", ");
                char *mfn = mangle_field_name(def->fields[i].name);
                if (is_fn_field && val_is_fn) {
                    /* Phase 16 v2: function pointer → int64_t field cast */
                    buf_printf(&lit, ".%s = (int64_t)(intptr_t)%s", mfn, fv);
                } else {
                    buf_printf(&lit, ".%s = %s", mfn, fv);
                }
                free(mfn);
                free(fv);
            }
            buf_puts(&lit, "}");
            buf_putc(&lit, '\0');
            char *result = strdup(lit.data);
            buf_free(&lit);
            return result;
        }
        case EX_SET_LIT: {
            /* Phase X3: #s(e1 e2 ...) -> tur_set_from_items(n, (int64_t[]){v1, v2, ...}) */
            uint32_t n = e->as.set_lit_.n;
            /* Evaluate each item into a temp so side effects happen before the call */
            char **item_vals = malloc(n * sizeof(char *));
            for (uint32_t i = 0; i < n; i++) {
                item_vals[i] = emit_value(ctx, body, e->as.set_lit_.items[i]);
            }
            Buf lit; buf_init(&lit);
            buf_printf(&lit, "tur_set_from_items(%u, (int64_t[]){", n);
            for (uint32_t i = 0; i < n; i++) {
                if (i > 0) buf_puts(&lit, ", ");
                buf_puts(&lit, item_vals[i]);
                free(item_vals[i]);
            }
            free(item_vals);
            buf_puts(&lit, "})");
            buf_putc(&lit, '\0');
            char *result = strdup(lit.data);
            buf_free(&lit);
            return result;
        }
        case EX_GET_FIELD: {
            /* (.field s) - emit s.field_name */
            char *sv = emit_value(ctx, body, e->as.get_field_.struct_expr);
            StructDef *def = e->as.get_field_.def;
            const char *fname_raw = def->fields[e->as.get_field_.field_idx].name;
            char *fname = mangle_field_name(fname_raw);
            Buf lit; buf_init(&lit);
            buf_printf(&lit, "(%s).%s", sv, fname);
            buf_putc(&lit, '\0');
            free(sv);
            free(fname);
            char *result = strdup(lit.data);
            buf_free(&lit);
            return result;
        }
        /* Phase HRT1: rank-2 polymorphic function wrapper */
        case EX_POLY_WRAP: {
            if (!e->as.poly_wrap_.wrapper_binding) {
                if (e->as.poly_wrap_.is_closure) {
                    /* Phase CCL: fat closure — pack into tur_poly_fn_t.
                     * Fat closure layout: struct { int64_t __fn; <captures...> }
                     * __fn (the first field, at offset 0) is the thunk pointer.
                     * Protocol: thunk(void *env, int64_t arg) where env = fat_ptr. */
                    char *fat = emit_value(ctx, body, e->as.poly_wrap_.inner);
                    char *tmp = fresh_tmp(ctx);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "void *%s = %s;\n", tmp, fat);
                    free(fat);
                    Buf out; buf_init(&out);
                    buf_printf(&out,
                        "(tur_poly_fn_t){ %s, "
                        "(int64_t(*)(void*,int64_t))(intptr_t)((int64_t*)%s)[0] }",
                        tmp, tmp);
                    buf_putc(&out, '\0');
                    char *result = strdup(out.data);
                    buf_free(&out);
                    free(tmp);
                    return result;
                }
                /* Phase HRT4: pass-through — inner is already a tur_poly_fn_t, emit directly. */
                return emit_value(ctx, body, e->as.poly_wrap_.inner);
            }
            /* Emit (tur_poly_fn_t){ NULL, wrapper_fn_name } */
            char *wn = raw_name_for_binding(e->as.poly_wrap_.wrapper_binding);
            Buf out; buf_init(&out);
            buf_printf(&out, "(tur_poly_fn_t){ NULL, %s }", wn);
            buf_putc(&out, '\0');
            free(wn);
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        /* Phase HRT1: type ascription — erase type, emit inner expression */
        case EX_ASCRIBE: {
            return emit_value(ctx, body, e->as.ascribe_.inner);
        }
        /* Phase HRT2: existential pack — box value as void* */
        case EX_EXISTS_PACK: {
            char *val = emit_value(ctx, body, e->as.exists_pack_.value);
            Buf out; buf_init(&out);
            TypeKind vk = e->as.exists_pack_.value->type.kind;
            if (vk == TY_PTR_VOID || vk == TY_EXISTS || vk == TY_FORALL) {
                /* Already a pointer — cast directly */
                buf_printf(&out, "(tur_exists_t)(%s)", val);
            } else {
                /* Scalar (int64_t, bool, etc.) — reinterpret via intptr_t (64-bit safe) */
                buf_printf(&out, "(tur_exists_t)(intptr_t)((int64_t)(%s))", val);
            }
            buf_putc(&out, '\0');
            free(val);
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        /* Phase HRT2: existential open — unbox void*, bind v, emit body */
        case EX_EXISTS_OPEN: {
            bool nil_result = (e->type.kind == TY_NIL);
            char *tmp = NULL;
            if (!nil_result) {
                tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s;\n", type_c_name(e->type), tmp);
            }
            indent_buf(body, ctx->indent);
            buf_puts(body, "{\n");
            ctx->indent += 4;

            /* Emit and unbox the packed value */
            char *packed_val = emit_value(ctx, body, e->as.exists_open_.packed);
            char *var_name = name_for_binding(ctx, e->as.exists_open_.var_binding);
            indent_buf(body, ctx->indent);
            TypeKind vk = e->as.exists_open_.var_binding->type.kind;
            if (vk == TY_PTR_VOID || vk == TY_EXISTS || vk == TY_FORALL || vk == TY_FN) {
                buf_printf(body, "void *%s = (void *)(%s);\n", var_name, packed_val);
            } else {
                /* Unbox scalar via intptr_t */
                buf_printf(body, "int64_t %s = (int64_t)(intptr_t)(%s);\n",
                           var_name, packed_val);
            }
            free(packed_val);

            /* Suppress unused-variable warning */
            indent_buf(body, ctx->indent);
            buf_printf(body, "(void)%s;\n", var_name);
            free(var_name);

            /* Emit body */
            if (nil_result) {
                emit_stmt(ctx, body, e->as.exists_open_.body);
            } else {
                char *bv = emit_value(ctx, body, e->as.exists_open_.body);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s = %s;\n", tmp, bv);
                free(bv);
            }

            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            return nil_result ? atom_nil() : tmp;
        }
        /* Phase G0: EX_DEFDATA — nothing to emit in value position (handled in Pass 0) */
        case EX_DEFDATA:
        /* Phase G1: EX_DEFGADT — same as EX_DEFDATA, handled in Pass 0 */
        case EX_DEFGADT:
            return atom_nil();

        /* Phase G0: EX_MATCH -- emit as statement-expression ({ ... result; }) */
        case EX_MATCH: {
            /* SS2: Session offer match -- TY_SESSION_OFFER scrutinee.
             * The scrutinee emits a tur_session_recv_tag() call (returns int64_t tag).
             * Each arm binds the channel pointer to its arm variable. */
            if (g_sessions_enabled &&
                e->as.match_.scrutinee->type.kind == TY_SESSION_OFFER) {
                const Expr *scrut = e->as.match_.scrutinee;
                bool nil_result = (e->type.kind == TY_NIL);
                char *tmp = NULL;
                if (!nil_result) {
                    tmp = fresh_tmp(ctx);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s %s = 0;\n", type_c_name(e->type), tmp);
                }
                /* Evaluate the scrutinee (returns the tag as int64_t). */
                char *tag_val = emit_value(ctx, body, scrut);
                char *tag_tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "int64_t %s = %s;\n", tag_tmp, tag_val);
                free(tag_val);

                /* Get the channel C name from the scrutinee's val_exprs[0]. */
                char *chan_name = NULL;
                if (scrut->kind == EX_INLINE_C) {
                    InlineC *sc_ic = scrut->as.inline_c_.inline_c;
                    if (sc_ic->n_val_exprs > 0 && sc_ic->val_exprs[0]
                        && sc_ic->val_exprs[0]->kind == EX_VAR) {
                        chan_name = name_for_binding(ctx, sc_ic->val_exprs[0]->as.var.binding);
                    }
                }
                if (!chan_name) chan_name = strdup("NULL");

                indent_buf(body, ctx->indent);
                buf_puts(body, "{\n");
                ctx->indent += 4;

                bool first_arm = true;
                for (uint32_t ai = 0; ai < e->as.match_.n_arms; ai++) {
                    MatchArm *arm = &e->as.match_.arms[ai];
                    MatchPattern *pat = &arm->pattern;

                    indent_buf(body, ctx->indent);
                    if (pat->is_wildcard || pat->union_member_idx < 0) {
                        buf_puts(body, first_arm ? "{\n" : "else {\n");
                    } else {
                        if (first_arm) {
                            buf_printf(body, "if (%s == %d) {\n",
                                       tag_tmp, pat->union_member_idx);
                        } else {
                            buf_printf(body, "else if (%s == %d) {\n",
                                       tag_tmp, pat->union_member_idx);
                        }
                    }
                    first_arm = false;
                    ctx->indent += 4;

                    /* Bind the arm variable to the channel pointer.
                     * Session-derived types (pair, recv-pair, offer) all lower to
                     * void* at runtime -- a TurChannel pointer. */
                    if (pat->n_bindings > 0 && pat->bindings[0]) {
                        Binding *fb = pat->bindings[0];
                        char *bname = name_for_binding(ctx, fb);
                        const char *arm_c_type =
                            (fb->type.kind == TY_SESSION ||
                             fb->type.kind == TY_SESSION_RECV_PAIR ||
                             fb->type.kind == TY_SESSION_PAIR ||
                             fb->type.kind == TY_SESSION_OFFER)
                            ? "void *"
                            : type_c_name(fb->type);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s %s = (void *)%s;\n",
                                   arm_c_type, bname, chan_name);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "(void)%s;\n", bname);
                        free(bname);
                    }

                    if (!nil_result) {
                        char *bv = emit_value(ctx, body, arm->body);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s = %s;\n", tmp, bv);
                        free(bv);
                    } else {
                        emit_stmt(ctx, body, arm->body);
                    }
                    ctx->indent -= 4;
                    indent_buf(body, ctx->indent);
                    buf_puts(body, "}\n");
                }

                free(chan_name);
                free(tag_tmp);
                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");
                return nil_result ? atom_nil() : tmp;
            }

            /* IT4: Union match -- tag-based dispatch via TUR_GETTAG / TUR_UNTAG.
             * Emits an if/else-if/else chain; each type-narrowing arm checks
             * the discriminant tag and binds the untagged value to the arm variable. */
            if (e->as.match_.scrutinee->type.kind == TY_UNION ||
                e->as.match_.scrutinee->type.kind == TY_ANY) {
                bool nil_result = (e->type.kind == TY_NIL);
                /* Use {0} for tur_tagged_t results, 0 for scalar results */
                const char *zero_init = (e->type.kind == TY_UNION ||
                                         e->type.kind == TY_ANY) ? "{0}" : "0";
                char *tmp = NULL;
                if (!nil_result) {
                    tmp = fresh_tmp(ctx);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s %s = %s;\n",
                               type_c_name(e->type), tmp, zero_init);
                }

                char *scrut_val = emit_value(ctx, body, e->as.match_.scrutinee);

                indent_buf(body, ctx->indent);
                buf_puts(body, "{\n");
                ctx->indent += 4;

                /* Store scrutinee once to avoid double-evaluation */
                char *scrut_tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "tur_tagged_t %s = %s;\n", scrut_tmp, scrut_val);
                free(scrut_val);

                bool first_arm = true;
                for (uint32_t ai = 0; ai < e->as.match_.n_arms; ai++) {
                    MatchArm *arm = &e->as.match_.arms[ai];
                    MatchPattern *pat = &arm->pattern;

                    indent_buf(body, ctx->indent);
                    if (pat->is_wildcard || pat->union_member_idx < 0) {
                        /* Wildcard or bare-variable arm — catches all remaining cases */
                        buf_puts(body, first_arm ? "{\n" : "else {\n");
                    } else {
                        /* Type-narrowing arm — check discriminant tag */
                        if (first_arm) {
                            buf_printf(body, "if (TUR_GETTAG(%s) == %d) {\n",
                                       scrut_tmp, pat->union_member_idx);
                        } else {
                            buf_printf(body, "else if (TUR_GETTAG(%s) == %d) {\n",
                                       scrut_tmp, pat->union_member_idx);
                        }
                    }
                    first_arm = false;
                    ctx->indent += 4;

                    /* Bind the narrowed variable: untagged value cast to the arm type */
                    if (pat->n_bindings > 0 && pat->bindings[0]) {
                        Binding *fb = pat->bindings[0];
                        const char *ctype = type_c_name(fb->type);
                        char *bname = name_for_binding(ctx, fb);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s %s = (%s)(intptr_t)TUR_UNTAG(%s);\n",
                                   ctype, bname, ctype, scrut_tmp);
                        free(bname);
                    }

                    /* Emit arm body */
                    if (!nil_result) {
                        char *bv = emit_value(ctx, body, arm->body);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s = %s;\n", tmp, bv);
                        free(bv);
                    } else {
                        emit_stmt(ctx, body, arm->body);
                    }

                    ctx->indent -= 4;
                    indent_buf(body, ctx->indent);
                    buf_puts(body, "}\n");
                }

                free(scrut_tmp);
                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");

                return nil_result ? atom_nil() : tmp;
            }

            /* Phase S4-lit: Literal pattern match -- scrutinee is a primitive type.
             * Emits an if/else-if chain comparing the scrutinee against literal values. */
            {
                bool _has_lit = false;
                for (uint32_t _ai = 0; _ai < e->as.match_.n_arms && !_has_lit; _ai++)
                    _has_lit = e->as.match_.arms[_ai].pattern.is_literal;
                if (_has_lit) {
                    bool nil_result = (e->type.kind == TY_NIL);
                    char *tmp = NULL;
                    if (!nil_result) {
                        tmp = fresh_tmp(ctx);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s %s = 0;\n", type_c_name(e->type), tmp);
                    }
                    char *scrut_val = emit_value(ctx, body, e->as.match_.scrutinee);
                    TypeKind _sk = e->as.match_.scrutinee->type.kind;
                    bool _is_str   = (_sk == TY_CSTR);
                    bool _is_float = (_sk == TY_FLOAT || _sk == TY_FLOAT32 || _sk == TY_FLOAT64);
                    char *scrut_tmp = fresh_tmp(ctx);
                    indent_buf(body, ctx->indent);
                    if (_is_str)
                        buf_printf(body, "const char *%s = (const char *)(intptr_t)(%s);\n",
                                   scrut_tmp, scrut_val);
                    else if (_is_float)
                        buf_printf(body, "double %s = (double)(%s);\n",
                                   scrut_tmp, scrut_val);
                    else
                        buf_printf(body, "int64_t %s = (int64_t)(intptr_t)(%s);\n",
                                   scrut_tmp, scrut_val);
                    free(scrut_val);
                    bool _first = true;
                    for (uint32_t ai = 0; ai < e->as.match_.n_arms; ai++) {
                        MatchArm *arm = &e->as.match_.arms[ai];
                        MatchPattern *pat = &arm->pattern;
                        indent_buf(body, ctx->indent);
                        if (pat->is_wildcard || pat->is_var) {
                            buf_puts(body, _first ? "{\n" : "else {\n");
                        } else {
                            const char *kw = _first ? "if" : "else if";
                            switch (pat->lit_kind) {
                            case F_INT:
                                buf_printf(body, "%s (%s == (int64_t)%lldLL) {\n",
                                           kw, scrut_tmp, (long long)pat->lit_int);
                                break;
                            case F_BOOL:
                                buf_printf(body, "%s (%s == %s) {\n",
                                           kw, scrut_tmp, pat->lit_bool ? "1" : "0");
                                break;
                            case F_FLOAT:
                                buf_printf(body, "%s (%s == %g) {\n",
                                           kw, scrut_tmp, pat->lit_float);
                                break;
                            case F_STR: {
                                Buf _slit; buf_init(&_slit);
                                StrSlice _ss; _ss.p = pat->lit_cstr ? pat->lit_cstr : "";
                                _ss.len = (uint32_t)strlen(_ss.p);
                                emit_c_string(&_slit, _ss);
                                buf_putc(&_slit, '\0');
                                buf_printf(body, "%s (%s != NULL && strcmp(%s, %s) == 0) {\n",
                                           kw, scrut_tmp, scrut_tmp, _slit.data);
                                buf_free(&_slit);
                                break;
                            }
                            case F_NIL:
                                buf_printf(body, "%s (%s == 0) {\n", kw, scrut_tmp);
                                break;
                            default:
                                buf_printf(body, "%s (0) {\n", kw);
                                break;
                            }
                        }
                        _first = false;
                        ctx->indent += 4;
                        /* Bind var capture to scrutinee value */
                        if (pat->is_var && pat->var_binding) {
                            const char *_ctype = type_c_name(e->as.match_.scrutinee->type);
                            char *_vname = name_for_binding(ctx, pat->var_binding);
                            indent_buf(body, ctx->indent);
                            if (_is_str)
                                buf_printf(body, "const char *%s = %s;\n", _vname, scrut_tmp);
                            else if (_is_float)
                                buf_printf(body, "%s %s = (%s)%s;\n", _ctype, _vname, _ctype, scrut_tmp);
                            else
                                buf_printf(body, "%s %s = (%s)(intptr_t)%s;\n",
                                           _ctype, _vname, _ctype, scrut_tmp);
                            free(_vname);
                        }
                        if (!nil_result) {
                            char *bv = emit_value(ctx, body, arm->body);
                            indent_buf(body, ctx->indent);
                            buf_printf(body, "%s = %s;\n", tmp, bv);
                            free(bv);
                        } else {
                            emit_stmt(ctx, body, arm->body);
                        }
                        ctx->indent -= 4;
                        indent_buf(body, ctx->indent);
                        buf_puts(body, "}\n");
                    }
                    free(scrut_tmp);
                    return nil_result ? atom_nil() : tmp;
                }
            }

            AdtDef *adt = e->as.match_.scrutinee->type.as.adt_.def;
            char adt_c_name[256];
            snprintf(adt_c_name, sizeof(adt_c_name), "tur_adt_%s", adt->name);

            bool nil_result = (e->type.kind == TY_NIL);
            char *tmp = NULL;
            if (!nil_result) {
                tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                /* Initialize to zero to silence -Wsometimes-uninitialized; exhaustiveness
                 * is guaranteed by the elaborator so the default branch is unreachable. */
                buf_printf(body, "%s %s = 0;\n", type_c_name(e->type), tmp);
            }

            /* Emit scrutinee */
            char *scrut_val = emit_value(ctx, body, e->as.match_.scrutinee);

            /* Phase G4: Check if any arm has a guard */
            bool has_any_guard = false;
            for (uint32_t ai = 0; ai < e->as.match_.n_arms; ai++) {
                if (e->as.match_.arms[ai].guard) { has_any_guard = true; break; }
            }

            if (has_any_guard) {
                /* Phase G4: Emit as if-chain with goto for guard fallthrough */
                char *end_label = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_puts(body, "{\n");
                ctx->indent += 4;

                indent_buf(body, ctx->indent);
                buf_printf(body, "%s *__scrut = (%s *)(intptr_t)(%s);\n",
                           adt_c_name, adt_c_name, scrut_val);
                free(scrut_val);

                for (uint32_t ai = 0; ai < e->as.match_.n_arms; ai++) {
                    MatchArm *arm = &e->as.match_.arms[ai];
                    MatchPattern *pat = &arm->pattern;

                    indent_buf(body, ctx->indent);
                    if (pat->is_wildcard || pat->is_var) {
                        buf_puts(body, "{\n");
                    } else {
                        buf_printf(body, "if (__scrut->tag == %u) {\n", pat->ctor->tag);
                    }
                    ctx->indent += 4;

                    /* Bind fields */
                    if (!pat->is_wildcard && !pat->is_var && pat->ctor) {
                        for (uint32_t bi = 0; bi < pat->n_bindings; bi++) {
                            Binding *fb = pat->bindings[bi];
                            const char *ctype = type_c_name(fb->type);
                            char *bname = name_for_binding(ctx, fb);
                            indent_buf(body, ctx->indent);
                            buf_printf(body, "%s %s = (%s)__scrut->as.%s._%u;\n",
                                       ctype, bname, ctype, pat->ctor->name, bi);
                            free(bname);
                        }
                    }

                    /* Emit guard */
                    if (arm->guard) {
                        char *gv = emit_value(ctx, body, arm->guard);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "if (%s) {\n", gv);
                        free(gv);
                        ctx->indent += 4;
                    }

                    /* Emit body */
                    if (!nil_result) {
                        char *bv = emit_value(ctx, body, arm->body);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s = %s;\n", tmp, bv);
                        free(bv);
                    } else {
                        emit_stmt(ctx, body, arm->body);
                    }
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "goto __%s;\n", end_label);

                    if (arm->guard) {
                        ctx->indent -= 4;
                        indent_buf(body, ctx->indent);
                        buf_puts(body, "}\n");
                    }

                    ctx->indent -= 4;
                    indent_buf(body, ctx->indent);
                    buf_puts(body, "}\n");
                }

                indent_buf(body, ctx->indent);
                buf_printf(body, "__%s:;\n", end_label);

                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");
                free(end_label);
            } else {
                indent_buf(body, ctx->indent);
                buf_puts(body, "{\n");
                ctx->indent += 4;

                indent_buf(body, ctx->indent);
                buf_printf(body, "%s *__scrut = (%s *)(intptr_t)(%s);\n",
                           adt_c_name, adt_c_name, scrut_val);
                free(scrut_val);

                indent_buf(body, ctx->indent);
                buf_puts(body, "switch (__scrut->tag) {\n");

                bool has_default = false;
                /* Phase G0: Track emitted constructor tags to skip redundant
                 * arms (arms whose constructor was already covered by an
                 * earlier arm).  The elaborator emits a warning for these;
                 * the emitter must not produce a duplicate case label. */
                bool emitted_tags[256] = {false};
                for (uint32_t ai = 0; ai < e->as.match_.n_arms; ai++) {
                    MatchArm *arm = &e->as.match_.arms[ai];
                    MatchPattern *pat = &arm->pattern;

                    /* Skip redundant constructor arms (duplicate case label) */
                    if (!pat->is_wildcard && !pat->is_var && pat->ctor) {
                        uint32_t tag = pat->ctor->tag;
                        if (tag < 256 && emitted_tags[tag]) continue;
                        if (tag < 256) emitted_tags[tag] = true;
                    }

                    indent_buf(body, ctx->indent);
                    if (pat->is_wildcard || pat->is_var) {
                        buf_puts(body, "default: {\n");
                        has_default = true;
                    } else {
                        buf_printf(body, "case %u: {\n", pat->ctor->tag);
                    }
                    ctx->indent += 4;

                    /* Bind field variables for constructor patterns */
                    if (!pat->is_wildcard && !pat->is_var && pat->ctor) {
                        for (uint32_t bi = 0; bi < pat->n_bindings; bi++) {
                            Binding *fb = pat->bindings[bi];
                            const char *ctype = type_c_name(fb->type);
                            /* Use name_for_binding to get the canonical C name */
                            char *bname = name_for_binding(ctx, fb);
                            indent_buf(body, ctx->indent);
                            buf_printf(body, "%s %s = (%s)__scrut->as.%s._%u;\n",
                                       ctype, bname, ctype, pat->ctor->name, bi);
                            free(bname);
                        }
                    }

                    /* Emit body */
                    if (!nil_result) {
                        char *bv = emit_value(ctx, body, arm->body);
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s = %s;\n", tmp, bv);
                        free(bv);
                    } else {
                        emit_stmt(ctx, body, arm->body);
                    }

                    indent_buf(body, ctx->indent);
                    buf_puts(body, "break;\n");
                    ctx->indent -= 4;
                    indent_buf(body, ctx->indent);
                    buf_puts(body, "}\n");
                }

                /* If no default/wildcard arm, add a default that does nothing
                 * (exhaustiveness is checked at elab time) */
                if (!has_default) {
                    indent_buf(body, ctx->indent);
                    buf_puts(body, "default: break;\n");
                }

                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");
                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");
            }

            return nil_result ? atom_nil() : tmp;
        }
    }
    return atom_nil();
}

