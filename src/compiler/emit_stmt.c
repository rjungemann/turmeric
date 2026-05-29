/* emit_stmt.c -- statement-position C emission (emit_stmt and friends). */
#include "emit_internal.h"

void emit_while_stmt(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* while (1) { cond; if (!cond) break; body; } — we re-emit the condition
     * inside the loop because it may itself produce statements. */
    indent_buf(body, ctx->indent);
    buf_puts(body, "while (1) {\n");
    ctx->indent += 4;
    char *cond = emit_value(ctx, body, e->as.while_.cond);
    indent_buf(body, ctx->indent);
    buf_printf(body, "if (!(%s)) break;\n", cond);
    free(cond);
    emit_stmt(ctx, body, e->as.while_.body);
    ctx->indent -= 4;
    indent_buf(body, ctx->indent);
    buf_puts(body, "}\n");
}

void emit_set_stmt(EmitCtx *ctx, Buf *body, const Expr *e) {
    char *bn = name_for_binding(ctx, e->as.set_.target);
    char *v = emit_value(ctx, body, e->as.set_.value);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s = %s;\n", bn, v);
    free(bn); free(v);
}

/* Phase 12: emit (set! (@ r) value) - mutation through mutable borrow */
void emit_set_deref_stmt(EmitCtx *ctx, Buf *body, const Expr *e) {
    char *ref = emit_value(ctx, body, e->as.set_deref_.ref);
    char *val = emit_value(ctx, body, e->as.set_deref_.value);
    const char *inner_type_c = type_c_name(e->as.set_deref_.value->type);
    indent_buf(body, ctx->indent);
    buf_printf(body, "*((%s *)%s) = %s;\n", inner_type_c, ref, val);
    free(ref); free(val);
}

/* Phase DS3: emit (set! (.field s) v) - struct field write.
 * For rc-typed fields, releases the prior pointer before storing the new
 * one (the new value carries its own +1).  For rc<Struct> receivers,
 * accesses the field through the rc-block's value pointer. */
void emit_set_field_stmt(EmitCtx *ctx, Buf *body, const Expr *e) {
    StructDef *def = e->as.set_field_.def;
    uint32_t fi = e->as.set_field_.field_idx;
    char *rv = emit_value(ctx, body, e->as.set_field_.receiver);
    char *vv = emit_value(ctx, body, e->as.set_field_.value);
    char *mfn = mangle_field_name(def->fields[fi].name);

    /* Build the lvalue expression for the field slot. */
    Buf lhs; buf_init(&lhs);
    if (e->as.set_field_.receiver_is_rc) {
        buf_printf(&lhs, "((%s *)((RcControlBlock *)(%s))->value)->%s",
                   def->name, rv, mfn);
    } else {
        buf_printf(&lhs, "(%s).%s", rv, mfn);
    }
    buf_putc(&lhs, '\0');

    TypeKind fk = def->fields[fi].kind;
    if (fk == TY_RC) {
        indent_buf(body, ctx->indent);
        buf_printf(body, "if (%s) { rc_strong_decrement(%s); rc_free_queue_drain(); }\n",
                   lhs.data, lhs.data);
    } else if (fk == TY_WEAK) {
        indent_buf(body, ctx->indent);
        buf_printf(body, "if (%s) rc_weak_decrement(%s);\n", lhs.data, lhs.data);
    } else if (fk == TY_REF || fk == TY_LREF) {
        indent_buf(body, ctx->indent);
        buf_printf(body, "if (%s) free(%s);\n", lhs.data, lhs.data);
    }

    indent_buf(body, ctx->indent);
    /* Phase E: for typed fn-ptr fields, cast through the typedef rather than
     * storing a raw int64_t.  For bare :fn fields (full_type == NULL) keep the
     * existing int64_t assignment so nothing regresses. */
    if (fk == TY_FN && e->as.set_field_.value->type.kind == TY_FN
            && def->fields[fi].full_type
            && def->fields[fi].full_type->kind == TY_FN) {
        const char *fn_td = register_fn_ptr_typedef(def->fields[fi].full_type);
        if (fn_td) {
            buf_printf(body, "%s = (%s)%s;\n", lhs.data, fn_td, vv);
        } else {
            buf_printf(body, "%s = %s;\n", lhs.data, vv);
        }
    } else {
        buf_printf(body, "%s = %s;\n", lhs.data, vv);
    }

    buf_free(&lhs);
    free(rv); free(vv); free(mfn);
}


void emit_stmt(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* Run e for side effects, discard value. */
    switch (e->kind) {
        case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT:
        case EX_FLOAT_LIT: case EX_CSTR_LIT: case EX_VAR:
        case EX_CAST:         /* pure expression, no stmt-level side effects */
        case EX_REINTERPRET:  /* compiler-only pure reinterpret node */
        case EX_UNION_INJECT: /* IT4: pure struct literal, no stmt-level side effects */
        case EX_ANY_TYPE_OF:  /* IT4: pure read, no stmt-level side effects */
        case EX_ANY_CAST:     /* IT4: pure unbox, no stmt-level side effects */
        case EX_TYPECLASS_DEF:
        case EX_DEFMODULE: /* Phase M0: module metadata — nothing to emit */
        case EX_PANIC_PAYLOAD_TYPE:
        case EX_CONS_LIST:    /* AR8: cons-list expr -- allocation side-effects handled in emit_value */
        case EX_PANIC_PAYLOAD_VALUE:
        case EX_PANIC_PAYLOAD_FILE:
        case EX_PANIC_PAYLOAD_LINE:
        case EX_PANIC_PAYLOAD_DOWNS:
        case EX_POLY_WRAP:   /* Phase HRT1: pure struct literal, no stmt-level side effects */
        case EX_ASCRIBE:     /* Phase HRT1: type erased, delegate to inner */
        case EX_EXISTS_PACK: /* Phase HRT2: pure boxing, no stmt-level side effects */
        case EX_DEFGADT:     /* Phase G1: ADT definition — handled in Pass 0 */
            /* No side effects — emit nothing. */
            return;
        case EX_EXISTS_OPEN: { /* Phase HRT2: run open body for side effects */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        case EX_PANIC_WITH: {
            /* Diverging - emit the panic */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        case EX_CATCH_UNWIND:
        case EX_CATCH_PANIC_OF: {
            /* These produce values but also have side effects (setting up handlers) */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        /* Phase S4: throw / try-catch */
        case EX_THROW:
        case EX_TRY_CATCH: {
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        case EX_WHILE: emit_while_stmt(ctx, body, e); return;
        case EX_SET:      emit_set_stmt(ctx, body, e);       return;
        case EX_SET_DEREF: emit_set_deref_stmt(ctx, body, e); return;
        case EX_SET_FIELD: emit_set_field_stmt(ctx, body, e); return;
        case EX_DO: {
            /* v1 lowering: use same frame-based approach as emit_do_value */
            uint32_t n = e->as.do_.n;
            
            /* Check if we have any defers in this scope */
            bool has_defers = false;
            for (uint32_t i = 0; i < n; i++) {
                if (e->as.do_.items[i]->kind == EX_DEFER) {
                    has_defers = true;
                    break;
                }
            }

            /* Also check for return/throw/panic in any item (for emitted_diverging tracking) */
            for (uint32_t i = 0; i < n; i++) {
                if (expr_contains_return_or_throw(e->as.do_.items[i])) {
                    /* Will use emitted_diverging to track */
                    break;
                }
            }

            if (!has_defers) {
                /* No defers - emit all items */
                for (uint32_t i = 0; i < n; i++) {
                    emit_stmt(ctx, body, e->as.do_.items[i]);
                }
                return;
            }

            /* Has defers - use tur_frame */
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

            /* Emit non-defer items and register defer thunks */
            bool emitted_diverging = false;  /* Track if we've emitted a return/throw/panic */
            for (uint32_t i = 0; i < n; i++) {
                const Expr *it = e->as.do_.items[i];
                
                /* If we've already emitted a diverging expression, skip remaining items */
                if (emitted_diverging) {
                    continue;
                }
                
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
                        free(env_name);
                    }
                } else {
                    emit_stmt(ctx, body, it);
                    /* Check if this statement is a panic (which fires defers via tur_frame_fire_chain) */
                    if (it->kind == EX_PANIC || it->kind == EX_PANIC_WITH) {
                        emitted_diverging = true;
                    }
                }
            }

            /* Fire all defers LIFO at scope exit */
            /* But if a diverging expression was emitted, it already fired defers */
            if (!emitted_diverging) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "tur_frame_fire_lifo(&%s);\n", frame_var);
            }

            /* Restore frame state */
            ctx->frame_var = saved_frame;
            free(frame_var);
            return;
        }
        case EX_LET: {
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        case EX_IF: {
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        case EX_BUILTIN: {
            char *v = emit_value(ctx, body, e);
            /* If the builtin produced a value (non-nil), emit it as a
             * void-cast expression statement. For nil-typed (println) this
             * was already a stmt. */
            if (e->type.kind != TY_NIL) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "(void)(%s);\n", v);
            }
            free(v);
            return;
        }
        case EX_DEF: case EX_PROGRAM:
            /* shouldn't reach here at expr level */
            return;
        case EX_DEFDATA:
            /* Phase G0/G1: ADT definition — emitted in Pass 0, nothing to do here */
            return;
        case EX_MATCH: {
            /* Phase G0: emit match in stmt position — discard result */
            char *v = emit_value(ctx, body, e);
            if (v && e->type.kind != TY_NIL) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "(void)(%s);\n", v);
            }
            free(v);
            return;
        }
        /* Phase 2 */
        case EX_FN_DEF:
            /* shouldn't reach here in stmt position */
            fprintf(stderr, "tur: emit: EX_FN_DEF in stmt position\n");
            abort();
            return;
        case EX_CALL: {
            /* Emit as expression statement */
            char *v = emit_value(ctx, body, e);
            indent_buf(body, ctx->indent);
            if (e->type.kind == TY_NIL) {
                buf_printf(body, "%s;\n", v);
            } else {
                buf_printf(body, "(void)(%s);\n", v);
            }
            free(v);
            return;
        }
        case EX_FN:
            fprintf(stderr, "tur: emit: EX_FN not yet implemented in emit_stmt\n");
            abort();
            return;
        case EX_EXTERN_C:
            /* extern-c declarations emit nothing in statement position (they're file-scope) */
            return;
        /* Phase R2: Panic */
        case EX_PANIC: {
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        /* Phase 18: Delimited continuations */
        case EX_RESET:
        case EX_SHIFT:
        case EX_SHIFT0:
            /* For now, emit a placeholder - full impl deferred */
            buf_puts(body, "__builtin_trap();");
            return;
        /* Phase B2: Cloneable continuations */
        case EX_CLONEABLE_RESET:
        case EX_CLONEABLE_SHIFT:
            /* For now, emit a placeholder - full impl deferred */
            buf_puts(body, "__builtin_trap();");
            return;
        /* Phase 21: Serializable continuations — lower to plain reset/shift for now;
         * frame registration stubs are emitted separately by the CPS pass. */
        case EX_SERIAL_RESET:
        case EX_SERIAL_SHIFT:
            buf_puts(body, "__builtin_trap();");
            return;
        case EX_INSTANCE_DEF: {
            /* Phase 15: Emit dictionary struct and global singleton */
            TypeClassInstance *inst = e->as.instance_def_.instance;
            TypeClass *tc = inst->typeclass;
            
            /* Generate dictionary struct name: dict_<TypeClass>_<typeargs>
             * Mirrors the type_suffix logic in elab_definstance so that
             * the struct name matches the method impl names already emitted. */
            char dict_name[128];
            char type_suffix[64] = "";
            for (uint8_t i = 0; i < inst->n_type_args; i++) {
                if (i == 0) strcat(type_suffix, "_");
                const char *component = "T";
                switch (inst->type_args[i].kind) {
                    case TY_INT:      component = "int";      break;
                    case TY_BOOL:     component = "bool";     break;
                    case TY_CSTR:     component = "cstr";     break;
                    case TY_NIL:      component = "nil";      break;
                    case TY_PTR_VOID: component = "ptr_void"; break;
                    case TY_INT8:     component = "int8";     break;
                    case TY_INT16:    component = "int16";    break;
                    case TY_INT32:    component = "int32";    break;
                    case TY_UINT8:    component = "uint8";    break;
                    case TY_UINT16:   component = "uint16";   break;
                    case TY_UINT32:   component = "uint32";   break;
                    case TY_UINT64:   component = "uint64";   break;
                    case TY_FLOAT32:  component = "float32";  break;
                    case TY_FLOAT64:  component = "float64";  break;
                    case TY_STRUCT:
                        /* Phase HKT §1: Use the original type arg symbol name
                         * (e.g. "option", "vec") so that two instances of the
                         * same HKT typeclass get distinct dict struct names.
                         * Fall back to the struct def name, then "T". */
                        if (inst->type_arg_syms && inst->type_arg_syms[i]) {
                            component = inst->type_arg_syms[i]->name;
                        } else if (inst->type_args[i].as.struct_.def &&
                                   inst->type_args[i].as.struct_.def->name) {
                            component = inst->type_args[i].as.struct_.def->name;
                        }
                        break;
                    case TY_APP: {
                        /* Phase HKT §3: partial type application — encode as "ctor_arg"
                         * e.g. (result int) → "result_int" → dict_Functor_result_int */
                        const char *fn_part  = "T";
                        const char *arg_part = "T";
                        if (inst->type_arg_syms && inst->type_arg_syms[i])
                            fn_part = inst->type_arg_syms[i]->name;
                        if (inst->type_args[i].as.app.arg) {
                            const char *n = type_name(*inst->type_args[i].as.app.arg);
                            if (n) arg_part = n;
                        }
                        char app_comp[48];
                        snprintf(app_comp, sizeof(app_comp), "%s_%s", fn_part, arg_part);
                        for (char *p = app_comp; *p; p++) {
                            if (!isalnum((unsigned char)*p)) *p = '_';
                        }
                        strncat(type_suffix, app_comp, sizeof(type_suffix) - strlen(type_suffix) - 1);
                        continue;
                    }
                    default: break;
                }
                /* Sanitise component: replace non-alnum chars with _ */
                char comp_buf[32];
                strncpy(comp_buf, component, sizeof(comp_buf) - 1);
                comp_buf[sizeof(comp_buf) - 1] = '\0';
                for (char *p = comp_buf; *p; p++) {
                    if (!isalnum((unsigned char)*p)) *p = '_';
                }
                strncat(type_suffix, comp_buf, sizeof(type_suffix) - strlen(type_suffix) - 1);
            }
            snprintf(dict_name, sizeof(dict_name), "dict_%s%s", tc->name->name, type_suffix);
            
            /* Helper to sanitize method name for C identifiers */
            char sanitized_method_name[64];
            
            /* Emit the dictionary struct to file scope */
            buf_printf(ctx->file, "typedef struct %s {\n", dict_name);
            for (uint8_t i = 0; i < tc->n_methods; i++) {
                const TypeClassMethod *method = &tc->methods[i];
                FnDef *method_impl = inst->method_impls[i];
                
                /* Sanitize method name for C field name (replace invalid chars with _) */
                char sanitized_method_name[64];
                strncpy(sanitized_method_name, method->name->name, sizeof(sanitized_method_name) - 1);
                sanitized_method_name[sizeof(sanitized_method_name) - 1] = '\0';
                for (char *p = sanitized_method_name; *p; p++) {
                    if (!isalnum((unsigned char)*p) && *p != '_') {
                        *p = '_';
                    }
                }
                
                /* Build function pointer type */
                buf_puts(ctx->file, "    ");
                
                /* Return type — prefer the declared return type from the binding
                 * (inline-C bodies have TY_NIL body type which would wrongly emit
                 * "void"; use the fn result_kind from the binding instead). */
                Type ret_type = method_impl->body->type;
                if ((ret_type.kind == TY_NIL || ret_type.kind == TY_UNKNOWN)
                    && method_impl->binding
                    && method_impl->binding->type.kind == TY_FN) {
                    ret_type = emit_type_from_kind(
                        method_impl->binding->type.as.fn.result_kind);
                }
                const char *ret_c_name = type_c_name(ret_type);
                buf_printf(ctx->file, "%s", ret_c_name);
                buf_puts(ctx->file, " (*");
                buf_printf(ctx->file, "%s", sanitized_method_name);
                buf_puts(ctx->file, ")(");
                
                /* Parameter types — Phase HRT3: use tur_poly_fn_t for poly fn params.
                 * Mirror emit_fns.c: pass_by_ptr structs use const T* in the slot typedef
                 * so it matches the actual by-pointer function signature. */
                bool body_is_inline_c = (method_impl->body
                                         && method_impl->body->kind == EX_INLINE_C);
                for (uint8_t j = 0; j < method_impl->n_params; j++) {
                    if (j > 0) buf_puts(ctx->file, ", ");
                    if (method_impl->params && method_impl->params[j]->is_poly_fn) {
                        buf_puts(ctx->file, "tur_poly_fn_t");
                    } else {
                        Type pt = method_impl->param_types[j];
                        if (!method_impl->closure && !body_is_inline_c
                            && type_struct_pass_by_ptr(pt)) {
                            buf_printf(ctx->file, "const %s *", type_c_name(pt));
                        } else {
                            buf_printf(ctx->file, "%s", type_c_name(pt));
                        }
                    }
                }
                buf_puts(ctx->file, ");\n");
            }
            buf_printf(ctx->file, "} %s;\n\n", dict_name);
            
            /* Emit the global singleton dictionary to file scope */
            buf_printf(ctx->file, "static %s %s_singleton = {\n", dict_name, dict_name);
            for (uint8_t i = 0; i < tc->n_methods; i++) {
                const TypeClassMethod *method = &tc->methods[i];
                
                /* Sanitize method name for C field name */
                strncpy(sanitized_method_name, method->name->name, sizeof(sanitized_method_name) - 1);
                sanitized_method_name[sizeof(sanitized_method_name) - 1] = '\0';
                for (char *p = sanitized_method_name; *p; p++) {
                    if (!isalnum((unsigned char)*p) && *p != '_') {
                        *p = '_';
                    }
                }
                
                buf_puts(ctx->file, "    ");
                buf_printf(ctx->file, ".%s = ", sanitized_method_name);
                /* Phase HKT H3: Use the binding name directly — it already encodes
                 * the correct type-arg suffix (e.g. _option, _vec) as computed in
                 * elab_definstance, so we avoid a second, potentially wrong suffix. */
                FnDef *method_impl_ref = inst->method_impls[i];
                if (method_impl_ref && method_impl_ref->binding) {
                    buf_printf(ctx->file, "%s", method_impl_ref->binding->name->name);
                } else {
                    buf_printf(ctx->file, "__inst_%s_%s", tc->name->name, sanitized_method_name);
                    buf_puts(ctx->file, type_suffix);
                }
                buf_puts(ctx->file, ",\n");
            }
            buf_printf(ctx->file, "};\n\n");
            return;
        }
        /* Phase H §1: dictionary passing — EX_DICT is a pure value node; no statement to emit */
        case EX_DICT:
            return;
        case EX_INLINE_C: {
            /* Emit the raw C code inline as a statement (with __TUR_CAP_N__ /
             * __TUR_VAL_N__ substitution).  Full inline-C function bodies already
             * end with ';' or '}'; session ops need a semicolon appended.
             * Strip trailing whitespace and block comments, then check last byte. */
            InlineC *ic = e->as.inline_c_.inline_c;
            char *subst = inline_c_substitute(ctx, body, ic);
            indent_buf(body, ctx->indent);
            buf_puts(body, subst);
            /* Find last significant byte in the substituted string. */
            size_t l = strlen(subst);
            bool strip_again;
            do {
                strip_again = false;
                while (l > 0 && (subst[l-1] == ' ' || subst[l-1] == '\t'
                                 || subst[l-1] == '\n' || subst[l-1] == '\r')) {
                    l--; strip_again = true;
                }
                if (l >= 2 && subst[l-1] == '/' && subst[l-2] == '*') {
                    size_t j = l - 2;
                    bool found = false;
                    while (j > 0) {
                        j--;
                        if (subst[j] == '/' && subst[j+1] == '*') {
                            l = j; strip_again = true; found = true; break;
                        }
                    }
                    if (!found) { l = 0; break; }
                }
            } while (strip_again);
            bool needs_semi = l > 0 && subst[l-1] != ';' && subst[l-1] != '}';
            free(subst);
            if (needs_semi) { buf_putc(body, ';'); }
            buf_putc(body, '\n');
            return;
        }
        /* Phase 3 */
        case EX_CLOSURE: {
            /* Emit closure as expression and discard value */
            char *v = emit_value(ctx, body, e);
            if (e->type.kind != TY_NIL) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "(void)(%s);\n", v);
            }
            free(v);
            return;
        }
        /* Phase 4: a bare defer that escapes its containing EX_DO (e.g. the
         * sole body of a let or defn) degenerates to "fires at scope end"
         * trivially — emit the body inline. */
        case EX_DEFER: {
            emit_stmt(ctx, body, e->as.defer_.body);
            return;
        }
        /* Phase 3/4: return */
        case EX_RETURN: {
            /* GF1: Inside a generator _next function, (return) terminates the generator */
            if (ctx->gen_var_name) {
                ctx->return_emitted = true;
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s->__state = -1;\n", ctx->gen_var_name);
                indent_buf(body, ctx->indent);
                buf_puts(body, "return NULL;\n");
                return;
            }

            /* (return) or (return expr) - emit full return statement with defer firing */

            /* Set flag to indicate return has been emitted */
            ctx->return_emitted = true;

            /* Fire all defers in the frame chain if we're in a scope with defers */
            if (ctx->frame_var) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "tur_frame_fire_chain(&%s);\n", ctx->frame_var);
            }

            /* Emit the return statement */
            indent_buf(body, ctx->indent);
            if (e->as.return_.value) {
                char *val = emit_value(ctx, body, e->as.return_.value);
                buf_printf(body, "return %s;\n", val);
                free(val);
            } else {
                buf_puts(body, "return;\n");
            }
            return;
        }
        /* GF1: yield inside a generator _next function */
        case EX_YIELD: {
            uint32_t yid    = e->as.yield_.yield_id;
            const char *gv  = ctx->gen_var_name;
            char *yval      = emit_value(ctx, body, e->as.yield_.value);
            /* Save state for resumption */
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s->__state = %u;\n", gv, yid);
            /* Allocate SOME(value) and return it */
            indent_buf(body, ctx->indent);
            buf_puts(body, "{\n");
            ctx->indent += 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "int64_t *__opt = (int64_t *)malloc(sizeof(int64_t));\n");
            indent_buf(body, ctx->indent);
            buf_printf(body, "*__opt = (int64_t)(%s);\n", yval);
            indent_buf(body, ctx->indent);
            buf_puts(body, "return (void *)__opt;\n");
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            free(yval);
            /* Resume label -- execution continues here on next _next call */
            buf_printf(body, "__yield_%u:;\n", yid - 1);
            return;
        }
        /* Phase 5 */
        case EX_REF: {
            /* (ref expr) as statement - emit and discard */
            char *v = emit_value(ctx, body, e);
            if (e->type.kind != TY_NIL) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "(void)(%s);\n", v);
            }
            free(v);
            return;
        }
        case EX_DEREF: {
            /* (@ expr) as statement - emit and discard */
            char *v = emit_value(ctx, body, e);
            if (e->type.kind != TY_NIL) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "(void)(%s);\n", v);
            }
            free(v);
            return;
        }
        /* Phase 9: rc<T> + weak<T> as statements */
        case EX_RC_OF:
        case EX_RC_CLONE:
        case EX_RC_PTR:
        case EX_RC_COUNT:
        case EX_RC_FROM_REF:
        case EX_REF_FROM_RC:
        case EX_WEAK:
        case EX_WEAK_UPGRADE:
        case EX_WEAK_PRED:
        case EX_REF_PRED:
        case EX_CONT_PRED:
        /* Phase T21-F: async/await as statement - emit and discard */
        /* Phase SEL1: select as statement - emit and discard */
        case EX_ASYNC:
        case EX_AWAIT:
        case EX_SELECT: {
            /* Emit as value expression, discard result */
            char *v = emit_value(ctx, body, e);
            if (e->type.kind != TY_NIL) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "(void)(%s);\n", v);
            }
            free(v);
            return;
        }
        case EX_RC_DROP: {
            /* rc/drop as statement - emit and discard (already returns nil) */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        /* Phase 12: Borrow traits as statements */
        case EX_BORROW_IMMUT:
        case EX_BORROW_MUT: {
            /* Borrow as statement - emit and discard (borrows are pointer operations) */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        /* Phase 20: Software Transactional Memory */
        case EX_STM:
        case EX_ATOMICALLY:
        case EX_RETRY:
        case EX_CHECK:
        case EX_OR_ELSE:
        case EX_TVAR_NEW:
        case EX_TVAR_READ:
        case EX_TVAR_WRITE:
        case EX_TVAR_MODIFY:
        case EX_TVAR_SWAP:
        case EX_TVAR_CAS: {
            /* STM expressions as statements - emit and discard result */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        /* Phase 19: Algebraic effects */
        case EX_DEFECT:
            /* Effect definitions are compile-time only */
            return;
        /* DV2: Dynamic vars */
        case EX_DEFDYNAMIC:
            /* Root init is handled by emit_module.c Pass 2 directly; nothing to do here. */
            return;
        case EX_DYNVAR_READ:
            /* Reading a var as a statement discards the value; skip. */
            return;
        case EX_DYNVAR_BINDING:
        case EX_DYNVAR_SET: {
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        /* GF1: Generator expressions as statements -- emit and discard */
        case EX_GEN:
        case EX_GEN_NEXT:
        case EX_GEN_DONE:
        case EX_PERFORM:
        case EX_HANDLE:
        case EX_RESUME:
        case EX_DISCONTINUE:
        case EX_MAKE_STRUCT:
        case EX_GET_FIELD:
        case EX_SET_LIT: {
            /* These should be lowered by effect_lower/CPS passes.
             * Fallback: emit via emit_value and discard result.
             * Do not recurse into emit_stmt with the same node. */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
    }
}
