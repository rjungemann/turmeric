/* emit_core.c -- shared codegen helpers: naming, atoms, builtins, captures. */
#include "emit_internal.h"

/* ------------ helpers ------------ */

/* Phase M0: Flatten EX_PROGRAM items into a contiguous array, expanding any
 * EX_DEFMODULE nodes into their body items. The returned array is malloc'd
 * and must be freed by the caller. */
const Expr **flatten_program_items(const Expr *program, uint32_t *out_n) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *e = program->as.program.items[i];
        if (e->kind == EX_DEFMODULE)
            total += e->as.defmodule_.mod->n_body;
        else
            total += 1;
    }
    const Expr **flat = (const Expr **)malloc(total * sizeof(Expr *));
    if (!flat && total > 0) { fprintf(stderr, "tur: oom\n"); abort(); }
    uint32_t k = 0;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *e = program->as.program.items[i];
        if (e->kind == EX_DEFMODULE) {
            DefModule *mod = e->as.defmodule_.mod;
            for (uint32_t j = 0; j < mod->n_body; j++)
                flat[k++] = mod->body[j];
        } else {
            flat[k++] = e;
        }
    }
    *out_n = total;
    return flat;
}

/* Helper to create a Type from TypeKind (mirrors the one in types.c). */
Type emit_type_from_kind(TypeKind k) {
    Type t;
    t.kind = k;
    t.as.fn.arity = 0;
    t.hkt_kind = KIND_STAR;  /* Phase HKT-P6: all types are kind * in v1 */
    return t;
}

void indent_buf(Buf *b, int n) {
    for (int i = 0; i < n; i++) buf_putc(b, ' ');
}

/* Phase R1: Helper to check if an expression is fully divergent (never
 * produces a value).  Used by emit_let_value to decide whether to assign
 * the body's emitted value to the let's result tmp: a divergent body
 * emits no value to assign. */
bool expr_is_divergent(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_RETURN:
        case EX_PANIC:
        case EX_PANIC_WITH:
        case EX_DISCONTINUE:
            return true;
        case EX_DO:
            if (e->as.do_.n == 0) return false;
            /* find last non-defer item */
            for (int i = (int)e->as.do_.n - 1; i >= 0; i--) {
                if (e->as.do_.items[i]->kind != EX_DEFER) {
                    return expr_is_divergent(e->as.do_.items[i]);
                }
            }
            return false;
        case EX_LET:
            return expr_is_divergent(e->as.let_.body);
        case EX_IF:
            if (!e->as.if_.else_or_null) return false;
            return expr_is_divergent(e->as.if_.then_) &&
                   expr_is_divergent(e->as.if_.else_or_null);
        default:
            return false;
    }
}

/* Phase 3/4: Helper to check if an expression contains return or panic */
bool expr_contains_return_or_throw(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_RETURN:
        case EX_PANIC:
        case EX_PANIC_WITH:
            return true;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (expr_contains_return_or_throw(e->as.do_.items[i])) {
                    return true;
                }
            }
            return false;
        case EX_LET:
            /* Check body for return/throw */
            return expr_contains_return_or_throw(e->as.let_.body);
        case EX_IF:
            /* Check both branches */
            if (expr_contains_return_or_throw(e->as.if_.then_)) return true;
            if (e->as.if_.else_or_null) {
                return expr_contains_return_or_throw(e->as.if_.else_or_null);
            }
            return false;
        case EX_WHILE:
            /* Check body */
            return expr_contains_return_or_throw(e->as.while_.body);
        /* Phase 19: Algebraic effects - discontinue is like throw */
        case EX_DISCONTINUE:
            return true;
        default:
            return false;
    }
}

/* MS1: Check if a program contains any handle expression with a ^multishot case.
 * Used to decide whether to emit the cloneable cont preamble. */
bool expr_has_multishot_handler(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_HANDLE: {
            const HandleExpr *h = e->as.handle_.handle;
            if (!h) return false;
            for (uint8_t i = 0; i < h->n_cases; i++) {
                if (h->cases[i].cont_kind == CK_MULTISHOT) return true;
            }
            if (expr_has_multishot_handler(h->body)) return true;
            for (uint8_t i = 0; i < h->n_cases; i++) {
                if (expr_has_multishot_handler(h->cases[i].body)) return true;
            }
            return false;
        }
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (expr_has_multishot_handler(e->as.let_.bindings[i].init)) return true;
            }
            return expr_has_multishot_handler(e->as.let_.body);
        case EX_IF:
            if (expr_has_multishot_handler(e->as.if_.cond)) return true;
            if (expr_has_multishot_handler(e->as.if_.then_)) return true;
            return e->as.if_.else_or_null && expr_has_multishot_handler(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (expr_has_multishot_handler(e->as.do_.items[i])) return true;
            }
            return false;
        case EX_FN_DEF:
            return e->as.fn_def_.fn && expr_has_multishot_handler(e->as.fn_def_.fn->body);
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++) {
                if (expr_has_multishot_handler(e->as.program.items[i])) return true;
            }
            return false;
        default:
            return false;
    }
}

char *fresh_tmp(EmitCtx *ctx) {
    char *p = (char *)malloc(24);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(p, 24, "__t%d", ctx->tmp_n++);
    return p;
}

/* Phase 4 v1: Generate a fresh frame variable name */
char *fresh_frame(EmitCtx *ctx) {
    char *p = (char *)malloc(24);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(p, 24, "__frame_%d", ctx->tmp_n++);
    return p;
}

/* Phase 4 v1: Generate a fresh defer thunk function name */
char *fresh_defer_thunk(EmitCtx *ctx) {
    char *p = (char *)malloc(24);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(p, 24, "__defer_%d", ctx->tmp_n++);
    return p;
}

/* Phase 4 v1: Generate a fresh defer env struct name */
char *fresh_defer_env(EmitCtx *ctx) {
    char *p = (char *)malloc(24);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(p, 24, "__defer_env_%d", ctx->tmp_n++);
    return p;
}

/* Phase 4 v1: Register a defer thunk to be emitted at file scope */
void register_defer_thunk(EmitCtx *ctx, const char *name, const Expr *body, 
                                  Binding **captures, uint8_t n_captures, 
                                  const char *env_name) {
    DeferThunk *thunk = (DeferThunk *)malloc(sizeof(DeferThunk));
    if (!thunk) { fprintf(stderr, "tur: oom\n"); abort(); }
    thunk->name = (char *)name;
    thunk->body = (Expr *)body;  /* Cast away const for storage */
    thunk->captures = captures;
    thunk->n_captures = n_captures;
    thunk->env_name = env_name ? strdup(env_name) : NULL;
    thunk->next = ctx->pending_defer_thunks;
    ctx->pending_defer_thunks = thunk;
}

/* Phase 4 v1: Emit all registered defer thunks to the output buffer */
void emit_pending_defer_thunks(EmitCtx *ctx, Buf *out) {
    /* First pass: emit env struct definitions for thunks with captures */
    DeferThunk *thunk = ctx->pending_defer_thunks;
    while (thunk) {
        if (thunk->env_name) {
            /* Emit env struct type definition */
            buf_printf(out, "struct %s {", thunk->env_name);
            for (uint8_t i = 0; i < thunk->n_captures; i++) {
                if (i > 0) buf_puts(out, "; ");
                Binding *captured = thunk->captures[i];
                buf_printf(out, "%s %s",
                           type_c_name(captured->type), captured->name->name);
            }
            buf_puts(out, "; };\n\n");
        }
        thunk = thunk->next;
    }
    
    /* Second pass: emit thunk functions */
    thunk = ctx->pending_defer_thunks;
    while (thunk) {
        if (thunk->env_name) {
            /* Thunk with captures - cast void* to env struct type */
            buf_printf(out, "static void %s(void *__env) {\n", thunk->name);
            buf_printf(out, "    struct %s *__e = (struct %s *)__env;\n",
                       thunk->env_name, thunk->env_name);
            
            /* Set up env access context for name_for_binding */
            const char *saved_env_var_name = ctx->env_var_name;
            Binding **saved_defer_captures = ctx->defer_captures;
            uint8_t saved_n_defer_captures = ctx->n_defer_captures;
            
            ctx->env_var_name = "__e";
            ctx->defer_captures = thunk->captures;
            ctx->n_defer_captures = thunk->n_captures;
            
            /* Emit the body with env access */
            int saved_indent = ctx->indent;
            ctx->indent = 4;
            emit_stmt(ctx, out, thunk->body);
            ctx->indent = saved_indent;
            
            ctx->env_var_name = saved_env_var_name;
            ctx->defer_captures = saved_defer_captures;
            ctx->n_defer_captures = saved_n_defer_captures;
        } else {
            /* Thunk without captures */
            buf_printf(out, "static void %s(void *__env) {\n", thunk->name);
            int saved_indent = ctx->indent;
            ctx->indent = 4;
            emit_stmt(ctx, out, thunk->body);
            ctx->indent = saved_indent;
        }
        buf_puts(out, "}\n\n");
        thunk = thunk->next;
    }
    
    /* Free the list */
    while (ctx->pending_defer_thunks) {
        DeferThunk *tmp = ctx->pending_defer_thunks;
        ctx->pending_defer_thunks = tmp->next;
        free(tmp->name);
        free(tmp->env_name);  /* May be NULL */
        free(tmp);
    }
}








/* Mangle a Turmeric struct field name to a valid C identifier.
 * Replaces hyphens and other non-id-safe chars with underscores. Caller frees. */
char *mangle_field_name(const char *name) {
    size_t len = strlen(name);
    char *p = (char *)malloc(len + 1);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            p[i] = c;
        } else {
            p[i] = '_';
        }
    }
    p[len] = '\0';
    return p;
}

/* Return a sanitized C identifier for a Binding, without the ID suffix.
 * Used for function names and parameters. Caller frees.
 *
 * Phase M3: For module-level bindings (defining_module_name != NULL), the
 * C name is prefixed with the mangled module name: geom/vector → geom__vector__,
 * so binding `add2` in module `geom/vector` → `geom__vector__add2`.
 * Phase M6: If b->c_export_name is set, use it directly (bypasses mangling). */
char *raw_name_for_binding(const Binding *b) {
    if (b->c_export_name) {
        return strdup(b->c_export_name);
    }
    /* Build module prefix if this binding belongs to a named module.
     * Exception: `main` is always the C entry point, never prefixed. */
    char mod_prefix[512];
    size_t mod_prefix_len = 0;
    bool is_main_binding = (b->name->len == 4 &&
                             memcmp(b->name->name, "main", 4) == 0);
    /* Phase M7: Only globals get module-prefixed C names. Function parameters
     * and locals are scoped to their function, so they don't collide across
     * modules — and inline-C bodies reference them by their source names. */
    if (b->defining_module_name != NULL && !is_main_binding && b->is_global) {
        const char *mn = b->defining_module_name->name;
        size_t mn_len = b->defining_module_name->len;
        size_t j = 0;
        for (size_t i = 0; i < mn_len && j < sizeof(mod_prefix) - 3; i++) {
            char c = mn[i];
            if (c == '/') {
                mod_prefix[j++] = '_';
                mod_prefix[j++] = '_';
            } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_') {
                mod_prefix[j++] = c;
            } else {
                mod_prefix[j++] = '_';
            }
        }
        mod_prefix[j++] = '_';
        mod_prefix[j++] = '_';
        mod_prefix[j]   = '\0';
        mod_prefix_len  = j;
    }

    size_t total = mod_prefix_len + b->name->len + 1;
    char *p = (char *)malloc(total);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    size_t k = 0;
    if (mod_prefix_len > 0) {
        memcpy(p, mod_prefix, mod_prefix_len);
        k = mod_prefix_len;
    }
    for (uint32_t i = 0; i < b->name->len; i++) {
        char c = b->name->name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            p[k++] = c;
        } else {
            p[k++] = '_';
        }
    }
    p[k] = '\0';
    return p;
}

/* Return a sanitized C identifier for a Binding. If the binding is a
 * function parameter in the current context, use the raw name (without ID).
 * Otherwise, append the ID suffix. Caller frees. */
char *name_for_binding(EmitCtx *ctx, const Binding *b) {
    /* Phase 3: If this is a captured binding in a closure thunk, emit as env->field */
    if (ctx->closure && ctx->env_var_name) {
        for (uint8_t i = 0; i < ctx->closure->n_captures; i++) {
            if (ctx->closure->captures[i] == b) {
                /* This is a captured binding - emit as env_var_name->field_name */
                char *field_name = raw_name_for_binding(b);
                char *result = (char *)malloc(strlen(ctx->env_var_name) + strlen(field_name) + 4);
                if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
                snprintf(result, strlen(ctx->env_var_name) + strlen(field_name) + 4, "%s->%s", ctx->env_var_name, field_name);
                free(field_name);
                return result;
            }
        }
    }
    /* Phase 4 v1: If this is a captured binding in a defer thunk, emit as env->field */
    if (ctx->env_var_name && ctx->defer_captures) {
        for (uint8_t i = 0; i < ctx->n_defer_captures; i++) {
            if (ctx->defer_captures[i] == b) {
                /* This is a captured binding - emit as env_var_name->field_name */
                char *field_name = raw_name_for_binding(b);
                char *result = (char *)malloc(strlen(ctx->env_var_name) + strlen(field_name) + 4);
                if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
                snprintf(result, strlen(ctx->env_var_name) + strlen(field_name) + 4, "%s->%s", ctx->env_var_name, field_name);
                free(field_name);
                return result;
            }
        }
    }
    /* Phase 19D: If this is a captured binding in a handle body fiber function, emit as __env->field */
    if (ctx->handle_captures && ctx->handle_env_name) {
        for (uint32_t i = 0; i < ctx->n_handle_captures; i++) {
            if (ctx->handle_captures[i] == b) {
                char *field_name = raw_name_for_binding(b);
                char *result = (char *)malloc(strlen(ctx->handle_env_name) + strlen(field_name) + 4);
                if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
                snprintf(result, strlen(ctx->handle_env_name) + strlen(field_name) + 4,
                         "%s->%s", ctx->handle_env_name, field_name);
                free(field_name);
                return result;
            }
        }
    }
    /* Check if this binding is a function parameter in the current context */
    if (ctx->fn_params) {
        for (uint8_t i = 0; i < ctx->n_fn_params; i++) {
            if (ctx->fn_params[i] == b) {
                return raw_name_for_binding(b);
            }
        }
    }
    /* If this is a function binding, use raw name without ID */
    if (b->type.kind == TY_FN) {
        return raw_name_for_binding(b);
    }
    /* `<name>_<id>` with non-id-safe chars mangled to underscores. We append
     * the unique id so different bindings with the same source name don't
     * collide in C. */
    size_t cap = b->name->len + 16;
    char *p = (char *)malloc(cap);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    size_t k = 0;
    for (uint32_t i = 0; i < b->name->len; i++) {
        char c = b->name->name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            p[k++] = c;
        } else {
            p[k++] = '_';
        }
    }
    snprintf(p + k, cap - k, "_%u", b->id);
    return p;
}

/* Emit a C string literal from a StrSlice. */
void emit_c_string(Buf *out, StrSlice s) {
    buf_putc(out, '"');
    for (uint32_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.p[i];
        switch (c) {
            case '"':  buf_puts(out, "\\\""); break;
            case '\\': buf_puts(out, "\\\\"); break;
            case '\n': buf_puts(out, "\\n");  break;
            case '\t': buf_puts(out, "\\t");  break;
            case '\r': buf_puts(out, "\\r");  break;
            default:
                if (c < 0x20 || c == 0x7f) buf_printf(out, "\\x%02x", c);
                else                       buf_putc(out, (char)c);
        }
    }
    buf_putc(out, '"');
}

/* ------------ atomic emitters (no statements emitted) ------------ */

char *atom_nil(void)         { return strdup("((void)0)"); }
char *atom_bool(bool b)      { return strdup(b ? "true" : "false"); }
/* Phase N: emit integer literal with correct C macro for fixed-width type */
char *atom_int_typed(int64_t i, TypeKind k) {
    char buf[64];
    switch (k) {
        case TY_INT8:   snprintf(buf, sizeof buf, "INT8_C(%lld)",   (long long)i); break;
        case TY_INT16:  snprintf(buf, sizeof buf, "INT16_C(%lld)",  (long long)i); break;
        case TY_INT32:  snprintf(buf, sizeof buf, "INT32_C(%lld)",  (long long)i); break;
        case TY_INT64:  snprintf(buf, sizeof buf, "INT64_C(%lld)",  (long long)i); break;
        case TY_UINT8:  snprintf(buf, sizeof buf, "UINT8_C(%llu)",  (unsigned long long)(uint64_t)i); break;
        case TY_UINT16: snprintf(buf, sizeof buf, "UINT16_C(%llu)", (unsigned long long)(uint64_t)i); break;
        case TY_UINT32: snprintf(buf, sizeof buf, "UINT32_C(%llu)", (unsigned long long)(uint64_t)i); break;
        case TY_UINT64: snprintf(buf, sizeof buf, "UINT64_C(%llu)", (unsigned long long)(uint64_t)i); break;
        default:        snprintf(buf, sizeof buf, "INT64_C(%lld)",  (long long)i); break;
    }
    return strdup(buf);
}
/* Phase N: emit float32 literal as a (float) cast */
char *atom_float32(double f) {
    char buf[80];
    snprintf(buf, sizeof buf, "((float)%.9g)", f);
    return strdup(buf);
}

char *atom_float(double f) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.15g", f);
    /* Ensure it's a double literal by appending .0 if needed */
    char *p = strchr(buf, '.');
    char *e = strchr(buf, 'e');
    if (!p && !e) {
        /* No decimal point or exponent - append .0 */
        strcat(buf, ".0");
    }
    return strdup(buf);
}

/* Phase N: type-dispatched float literal emitter */
static char *atom_float_typed(TypeKind k, double f) __attribute__((unused));
static char *atom_float_typed(TypeKind k, double f) {
    if (k == TY_FLOAT32) {
        char buf[64];
        snprintf(buf, sizeof buf, "(float)(%.7g)", f);
        return strdup(buf);
    }
    return atom_float(f);
}
char *atom_var(EmitCtx *ctx, const Binding *b) {
    return name_for_binding(ctx, b);
}
char *atom_cstr(StrSlice s) {
    /* Build into a Buf, then strdup out. */
    Buf b; buf_init(&b);
    emit_c_string(&b, s);
    buf_putc(&b, '\0');
    char *p = strdup(b.data);
    buf_free(&b);
    return p;
}

/* ------------ builtin emitters ------------ */

char *emit_builtin(EmitCtx *ctx, Buf *body, const Expr *e) {
    const BuiltinSpec *spec = e->as.builtin.spec;
    uint32_t n = e->as.builtin.n;
    Expr **args = e->as.builtin.args;

    /* Eagerly emit all args (left-to-right) for non-short-circuit ops. */
    if (spec->shape == BS_AND_SC || spec->shape == BS_OR_SC) {
        /* Short-circuit: evaluate left, then conditionally evaluate the rest.
         * Lower (and a b c) to: bool t = a; if (t) t = b; if (t) t = c;
         * Lower (or  a b c) to: bool t = a; if (!t) t = b; if (!t) t = c; */
        char *tmp = fresh_tmp(ctx);
        char *first = emit_value(ctx, body, args[0]);
        indent_buf(body, ctx->indent);
        buf_printf(body, "bool %s = %s;\n", tmp, first);
        free(first);
        for (uint32_t i = 1; i < n; i++) {
            indent_buf(body, ctx->indent);
            buf_printf(body, "if (%s%s) {\n",
                       spec->shape == BS_OR_SC ? "!" : "", tmp);
            ctx->indent += 4;
            char *next = emit_value(ctx, body, args[i]);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s = %s;\n", tmp, next);
            free(next);
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
        }
        return tmp;
    }

    /* println — emits a stmt and returns a nil placeholder. */
    if (spec->shape == BS_PRINTLN_INT ||
        spec->shape == BS_PRINTLN_FLOAT ||
        spec->shape == BS_PRINTLN_BOOL ||
        spec->shape == BS_PRINTLN_CSTR ||
        spec->shape == BS_PRINTLN_UINT ||
        spec->shape == BS_PRINTLN_FLOAT32) {
        char *arg = emit_value(ctx, body, args[0]);
        indent_buf(body, ctx->indent);
        switch (spec->shape) {
            case BS_PRINTLN_INT:
                buf_printf(body, "printf(\"%%lld\\n\", (long long)(%s));\n", arg);
                break;
            case BS_PRINTLN_FLOAT:
                buf_printf(body, "printf(\"%%g\\n\", (double)(%s));\n", arg);
                break;
            case BS_PRINTLN_BOOL:
                buf_printf(body, "puts((%s) ? \"true\" : \"false\");\n", arg);
                break;
            case BS_PRINTLN_CSTR:
                buf_printf(body, "puts(%s);\n", arg);
                break;
            case BS_PRINTLN_UINT:
                buf_printf(body, "printf(\"%%llu\\n\", (unsigned long long)(%s));\n", arg);
                break;
            case BS_PRINTLN_FLOAT32:
                buf_printf(body, "printf(\"%%.7g\\n\", (double)(%s));\n", arg);
                break;
            default: break;
        }
        free(arg);
        return atom_nil();
    }

    /* For everything else, evaluate args and build a C expression. */
    char **arg_strs = (char **)malloc(n * sizeof(char *));
    if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
    for (uint32_t i = 0; i < n; i++) arg_strs[i] = emit_value(ctx, body, args[i]);

    Buf out; buf_init(&out);
    switch (spec->shape) {
        case BS_BIN_INFIX:
            buf_printf(&out, "((%s) %s (%s))",
                       arg_strs[0], spec->c_op, arg_strs[1]);
            break;
        case BS_VARIADIC_FOLD:
            /* ((a OP b) OP c) OP d ... */
            for (uint32_t i = 0; i < n - 1; i++) buf_putc(&out, '(');
            buf_printf(&out, "(%s)", arg_strs[0]);
            for (uint32_t i = 1; i < n; i++) {
                buf_printf(&out, " %s (%s))", spec->c_op, arg_strs[i]);
            }
            break;
        case BS_DIV_CHECK:
            /* Division with zero check: (a / b) checks b != 0 */
            buf_printf(&out, "((%s) ? ((%s) / (%s)) : (fprintf(stderr, \"division by zero\\n\"), abort(), 0))",
                       arg_strs[1], arg_strs[0], arg_strs[1]);
            break;
        case BS_PREFIX_UNARY:
            buf_printf(&out, "(%s(%s))", spec->c_op, arg_strs[0]);
            break;
        case BS_PREFIX_UNARY_FREE:
            /* Special case for drop!: free the ref pointer directly
             * arg is a ref<T> which is stored as void* in C for v1
             * Emit: free(arg) as a statement
             * Unlike other builtins, this has side effects and is always emitted
             * as a statement, so we emit it directly here and return nil */
            indent_buf(body, ctx->indent);
            buf_printf(body, "free(%s);\n", arg_strs[0]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        case BS_PTR_WRITE:
            /* Special case for ptr-write: *ptr = value as a statement
             * Emit: *((int64_t *)arg0) = arg1; as a statement, return nil
             * We cast to int64_t * because all Turmeric values are int64_t in v1 */
            indent_buf(body, ctx->indent);
            buf_printf(body, "*((int64_t *)%s) = %s;\n", arg_strs[0], arg_strs[1]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        case BS_PTR_ARITH:
            /* Pointer arithmetic: (char *)ptr op offset
             * In C, pointer arithmetic requires a typed pointer, so we cast to char *
             * Emit: (char *)arg0 op arg1 */
            buf_printf(&out, "((char *)%s %s %s)", arg_strs[0], spec->c_op, arg_strs[1]);
            break;
        case BS_PTR_DEREF:
            /* Pointer dereference: *((T *)ptr)
             * c_op is the prefix like "*((int64_t *)" and we append arg + ")"
             * Emit: c_op + arg + ")" */
            buf_printf(&out, "%s%s)", spec->c_op, arg_strs[0]);
            break;
        /* Phase U3: Unsafe primitives - type casting */
        case BS_UNSAFE_CAST:
            /* unsafe-cast: C-style cast from one type to another
             * arg0 = value to cast, arg1 = type keyword (we ignore it and cast to int64_t for v1)
             * Emit: (int64_t)arg0 */
            buf_printf(&out, "((int64_t)%s)", arg_strs[0]);
            break;
        case BS_REINTERPRET:
            /* reinterpret: bitwise reinterpretation - same as unsafe-cast for v1
             * Emit: (int64_t)arg0 */
            buf_printf(&out, "((int64_t)%s)", arg_strs[0]);
            break;
        case BS_TRANSMUTE:
            /* transmute: type-punning with compile-time size check
             * For v1, we assume sizes match and emit a simple cast
             * Emit: (int64_t)arg0 */
            buf_printf(&out, "((int64_t)%s)", arg_strs[0]);
            break;
        /* Phase U3: Unsafe primitives - unchecked array ops */
        case BS_ARRAY_GET_UNCHECKED:
            /* array-get-unchecked: *(ptr + index)
             * arg0 = pointer, arg1 = index
             * Emit: *((int64_t *)arg0 + arg1) */
            buf_printf(&out, "(*((int64_t *)%s + %s))", arg_strs[0], arg_strs[1]);
            break;
        case BS_ARRAY_SET_UNCHECKED:
            /* array-set-unchecked: *(ptr + index) = value
             * arg0 = pointer, arg1 = index, arg2 = value
             * Emit as statement: *((int64_t *)arg0 + arg1) = arg2; */
            indent_buf(body, ctx->indent);
            buf_printf(body, "(*((int64_t *)%s + %s) = %s);\n", arg_strs[0], arg_strs[1], arg_strs[2]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        /* Phase U3: Unsafe primitives - raw memory */
        case BS_RAW_MALLOC:
            /* raw-malloc: malloc(size)
             * arg0 = size
             * Emit: malloc(arg0) */
            buf_printf(&out, "malloc(%s)", arg_strs[0]);
            break;
        case BS_RAW_FREE:
            /* raw-free: free(ptr)
             * arg0 = pointer
             * Emit as statement: free(arg0); */
            indent_buf(body, ctx->indent);
            buf_printf(body, "free(%s);\n", arg_strs[0]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        case BS_RAW_REALLOC:
            /* raw-realloc: realloc(ptr, new_size)
             * arg0 = pointer, arg1 = new size
             * Emit: realloc(arg0, arg1) */
            buf_printf(&out, "realloc(%s, %s)", arg_strs[0], arg_strs[1]);
            break;
        case BS_RAW_MEMCPY:
            /* raw-memcpy: memcpy(dest, src, n)
             * arg0 = dest, arg1 = src, arg2 = n
             * Emit as statement: memcpy(arg0, arg1, arg2); */
            indent_buf(body, ctx->indent);
            buf_printf(body, "memcpy(%s, %s, %s);\n", arg_strs[0], arg_strs[1], arg_strs[2]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        case BS_RAW_MEMSET:
            /* raw-memset: memset(dest, byte, n)
             * arg0 = dest, arg1 = byte, arg2 = n
             * Emit as statement: memset(arg0, arg1, arg2); */
            indent_buf(body, ctx->indent);
            buf_printf(body, "memset(%s, %s, %s);\n", arg_strs[0], arg_strs[1], arg_strs[2]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        /* Phase U3: FFI */
        case BS_DLOPEN:
            /* dlopen: dlopen(path, RTLD_LAZY)
             * arg0 = path (cstr)
             * Emit: dlopen(arg0, RTLD_LAZY) */
            buf_printf(&out, "dlopen(%s, RTLD_LAZY)", arg_strs[0]);
            break;
        case BS_DLSYM:
            /* dlsym: dlsym(handle, symbol)
             * arg0 = handle (ptr<void>), arg1 = symbol (cstr)
             * Emit: dlsym(arg0, arg1) */
            buf_printf(&out, "dlsym(%s, %s)", arg_strs[0], arg_strs[1]);
            break;
        case BS_DLCLOSE:
            /* dlclose: dlclose(handle)
             * arg0 = handle (ptr<void>)
             * Emit: dlclose(arg0) */
            buf_printf(&out, "dlclose(%s)", arg_strs[0]);
            break;
        case BS_FUNC_CALL:
            /* Generic function call: c_op(arg0, arg1, ...) */
            buf_printf(&out, "%s(", spec->c_op);
            for (uint32_t i = 0; i < n; i++) {
                if (i > 0) buf_puts(&out, ", ");
                buf_puts(&out, arg_strs[i]);
            }
            buf_putc(&out, ')');
            break;
        default:
            /* unreachable for the shapes handled above */
            buf_puts(&out, "((void)0)");
            break;
    }
    buf_putc(&out, '\0');
    char *result = strdup(out.data);
    buf_free(&out);
    for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
    free(arg_strs);
    return result;
}


/* ------------ entry points ------------ */

/* Phase H §1: Compute the C name of a typeclass instance's dictionary singleton.
 * Mirrors the type_suffix logic in EX_INSTANCE_DEF (emit_stmt) so that the name
 * is consistent wherever it needs to be referenced (EX_DICT emit_value, etc.).
 * Writes "dict_<TypeClass>_<typeargs>" into buf (size buflen). */
void emit_dict_name(char *buf, size_t buflen, const TypeClassInstance *inst) {
    const TypeClass *tc = inst->typeclass;
    char type_suffix[64] = "";
    for (uint8_t i = 0; i < inst->n_type_args; i++) {
        if (i == 0) strncat(type_suffix, "_", sizeof(type_suffix) - strlen(type_suffix) - 1);
        const char *component = "T";
        switch (inst->type_args[i].kind) {
            case TY_INT:      component = "int";      break;
            case TY_BOOL:     component = "bool";     break;
            case TY_CSTR:     component = "cstr";     break;
            case TY_NIL:      component = "nil";      break;
            case TY_PTR_VOID: component = "ptr_void"; break;
            case TY_STRUCT:
                if (inst->type_arg_syms && inst->type_arg_syms[i])
                    component = inst->type_arg_syms[i]->name;
                else if (inst->type_args[i].as.struct_.def &&
                         inst->type_args[i].as.struct_.def->name)
                    component = inst->type_args[i].as.struct_.def->name;
                break;
            case TY_APP: {
                const char *fn_part  = "T";
                const char *arg_part = "T";
                if (inst->type_args[i].as.app.fn) {
                    Type *fn = inst->type_args[i].as.app.fn;
                    if (fn->kind == TY_REC && fn->as.rec.name)
                        fn_part = fn->as.rec.name;
                    else if (fn->kind == TY_STRUCT && fn->as.struct_.def &&
                             fn->as.struct_.def->name)
                        fn_part = fn->as.struct_.def->name;
                }
                if (inst->type_args[i].as.app.arg) {
                    const char *n = type_name(*inst->type_args[i].as.app.arg);
                    if (n) arg_part = n;
                }
                char app_comp[48];
                snprintf(app_comp, sizeof(app_comp), "%s_%s", fn_part, arg_part);
                for (char *p = app_comp; *p; p++) {
                    if (!isalnum((unsigned char)*p)) *p = '_';
                }
                strncat(type_suffix, app_comp,
                        sizeof(type_suffix) - strlen(type_suffix) - 1);
                continue;
            }
            default: break;
        }
        char comp_buf[32];
        strncpy(comp_buf, component, sizeof(comp_buf) - 1);
        comp_buf[sizeof(comp_buf) - 1] = '\0';
        for (char *p = comp_buf; *p; p++) {
            if (!isalnum((unsigned char)*p)) *p = '_';
        }
        strncat(type_suffix, comp_buf, sizeof(type_suffix) - strlen(type_suffix) - 1);
    }
    snprintf(buf, buflen, "dict_%s%s", tc->name->name, type_suffix);
}

/* Phase 19D: Collect bindings introduced within an expression (to identify outer captures).
 * defs/ndefs/cdefs is a growable array of Binding*. */
void collect_defined(const Expr *e, Binding ***defs, uint32_t *ndefs, uint32_t *cdefs) {
    if (!e) return;
    switch (e->kind) {
        case EX_LET: {
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                Binding *b = e->as.let_.bindings[i].binding;
                if (b) {
                    if (*ndefs >= *cdefs) {
                        *cdefs = (*cdefs == 0) ? 8 : *cdefs * 2;
                        *defs = (Binding **)realloc(*defs, *cdefs * sizeof(Binding *));
                    }
                    (*defs)[(*ndefs)++] = b;
                }
                collect_defined(e->as.let_.bindings[i].init, defs, ndefs, cdefs);
            }
            collect_defined(e->as.let_.body, defs, ndefs, cdefs);
            break;
        }
        case EX_FN_DEF: {
            if (e->as.fn_def_.fn && e->as.fn_def_.fn->binding) {
                if (*ndefs >= *cdefs) {
                    *cdefs = (*cdefs == 0) ? 8 : *cdefs * 2;
                    *defs = (Binding **)realloc(*defs, *cdefs * sizeof(Binding *));
                }
                (*defs)[(*ndefs)++] = e->as.fn_def_.fn->binding;
            }
            break;
        }
        case EX_FN: {
            if (e->as.fn_.fn && e->as.fn_.fn->binding) {
                if (*ndefs >= *cdefs) {
                    *cdefs = (*cdefs == 0) ? 8 : *cdefs * 2;
                    *defs = (Binding **)realloc(*defs, *cdefs * sizeof(Binding *));
                }
                (*defs)[(*ndefs)++] = e->as.fn_.fn->binding;
            }
            break;
        }
        case EX_DO: {
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                collect_defined(e->as.do_.items[i], defs, ndefs, cdefs);
            break;
        }
        case EX_IF: {
            collect_defined(e->as.if_.cond, defs, ndefs, cdefs);
            collect_defined(e->as.if_.then_, defs, ndefs, cdefs);
            collect_defined(e->as.if_.else_or_null, defs, ndefs, cdefs);
            break;
        }
        case EX_WHILE: {
            collect_defined(e->as.while_.cond, defs, ndefs, cdefs);
            collect_defined(e->as.while_.body, defs, ndefs, cdefs);
            break;
        }
        case EX_HANDLE: {
            /* Walk the handle body to find bindings defined within it */
            if (e->as.handle_.handle)
                collect_defined(e->as.handle_.handle->body, defs, ndefs, cdefs);
            break;
        }
        default:
            break;
    }
}

/* Phase 19D: Collect free variable bindings in an expression (bindings not defined within it).
 * Returns a malloc'd array of outer bindings; caller frees. *n_out is set to count. */
Binding **collect_handle_captures(const Expr *body, uint32_t *n_out) {
    /* Step 1: collect all bindings defined within body */
    Binding **defs = NULL;
    uint32_t ndefs = 0, cdefs = 0;
    collect_defined(body, &defs, &ndefs, &cdefs);

    /* Step 2: walk body for EX_VAR refs not in defined set and not already captured */
    Binding **caps = NULL;
    uint32_t ncaps = 0, ccaps = 0;

    /* Use a simple recursive walk via a stack */
    /* We'll use a small helper approach: collect all EX_VAR bindings from body */
    /* then filter out those in defs */
    /* Simple DFS via recursion - collect all variable references */
    /* For now, implement a conservative approach: collect all EX_VAR that are not
     * defined within body and not globals */
    typedef struct ExprStack { const Expr *e; struct ExprStack *next; } ExprStack;
    ExprStack *stack = NULL;
    ExprStack initial = { body, NULL };
    stack = &initial;

    /* We need heap-allocated stack nodes for recursive traversal */
    /* Use a simple iterative approach with malloc'd stack */
    ExprStack **heap_nodes = NULL;
    uint32_t heap_count = 0, heap_cap = 0;

#define PUSH_EXPR(ex) do { \
    if (ex) { \
        if (heap_count >= heap_cap) { \
            heap_cap = (heap_cap == 0) ? 16 : heap_cap * 2; \
            heap_nodes = (ExprStack **)realloc(heap_nodes, heap_cap * sizeof(ExprStack *)); \
        } \
        ExprStack *_node = (ExprStack *)malloc(sizeof(ExprStack)); \
        _node->e = (ex); _node->next = stack; stack = _node; \
        heap_nodes[heap_count++] = _node; \
    } \
} while(0)

    /* Reset stack - just use heap nodes from the start */
    stack = NULL;
    PUSH_EXPR(body);

    while (stack) {
        ExprStack *top = stack;
        stack = top->next;
        const Expr *cur = top->e;
        if (!cur) continue;

        switch (cur->kind) {
            case EX_VAR: {
                Binding *b = cur->as.var.binding;
                if (!b || b->is_global || b->type.kind == TY_FN) break;
                /* Check if defined within body */
                bool in_defs = false;
                for (uint32_t i = 0; i < ndefs; i++) {
                    if (defs[i] == b) { in_defs = true; break; }
                }
                if (in_defs) break;
                /* Check if already captured */
                bool already = false;
                for (uint32_t i = 0; i < ncaps; i++) {
                    if (caps[i] == b) { already = true; break; }
                }
                if (!already) {
                    if (ncaps >= ccaps) {
                        ccaps = (ccaps == 0) ? 8 : ccaps * 2;
                        caps = (Binding **)realloc(caps, ccaps * sizeof(Binding *));
                    }
                    caps[ncaps++] = b;
                }
                break;
            }
            case EX_LET: {
                for (uint32_t i = 0; i < cur->as.let_.n; i++)
                    PUSH_EXPR(cur->as.let_.bindings[i].init);
                PUSH_EXPR(cur->as.let_.body);
                break;
            }
            case EX_DO: {
                for (uint32_t i = 0; i < cur->as.do_.n; i++)
                    PUSH_EXPR(cur->as.do_.items[i]);
                break;
            }
            case EX_IF: {
                PUSH_EXPR(cur->as.if_.cond);
                PUSH_EXPR(cur->as.if_.then_);
                PUSH_EXPR(cur->as.if_.else_or_null);
                break;
            }
            case EX_WHILE: {
                PUSH_EXPR(cur->as.while_.cond);
                PUSH_EXPR(cur->as.while_.body);
                break;
            }
            case EX_SET: {
                /* The target binding is also a reference (it's being mutated) */
                if (cur->as.set_.target) {
                    Binding *b = cur->as.set_.target;
                    if (!b->is_global && b->type.kind != TY_FN) {
                        /* Check if defined within body */
                        bool in_defs = false;
                        for (uint32_t i = 0; i < ndefs; i++) {
                            if (defs[i] == b) { in_defs = true; break; }
                        }
                        if (!in_defs) {
                            bool already = false;
                            for (uint32_t i = 0; i < ncaps; i++) {
                                if (caps[i] == b) { already = true; break; }
                            }
                            if (!already) {
                                if (ncaps >= ccaps) {
                                    ccaps = (ccaps == 0) ? 8 : ccaps * 2;
                                    caps = (Binding **)realloc(caps, ccaps * sizeof(Binding *));
                                }
                                caps[ncaps++] = b;
                            }
                        }
                    }
                }
                PUSH_EXPR(cur->as.set_.value);
                break;
            }
            case EX_CALL: {
                for (uint32_t i = 0; i < cur->as.call_.n_args; i++)
                    PUSH_EXPR(cur->as.call_.args[i]);
                PUSH_EXPR(cur->as.call_.fn_expr);
                break;
            }
            case EX_BUILTIN: {
                for (uint32_t i = 0; i < cur->as.builtin.n; i++)
                    PUSH_EXPR(cur->as.builtin.args[i]);
                break;
            }
            case EX_RETURN: {
                PUSH_EXPR(cur->as.return_.value);
                break;
            }
            case EX_PERFORM: {
                if (cur->as.perform_.perform) {
                    for (uint32_t i = 0; i < cur->as.perform_.perform->n_args; i++)
                        PUSH_EXPR(cur->as.perform_.perform->args[i]);
                }
                break;
            }
            case EX_HANDLE: {
                if (cur->as.handle_.handle) {
                    /* Only walk the handle body (not case bodies), since case bodies
                     * have their own local scope (effect params + k are local there). */
                    PUSH_EXPR(cur->as.handle_.handle->body);
                }
                break;
            }
            case EX_RESUME: {
                if (cur->as.resume_.resume) {
                    PUSH_EXPR(cur->as.resume_.resume->k);
                    PUSH_EXPR(cur->as.resume_.resume->value);
                }
                break;
            }
            case EX_DISCONTINUE: {
                if (cur->as.discontinue_.discontinue) {
                    PUSH_EXPR(cur->as.discontinue_.discontinue->k);
                    PUSH_EXPR(cur->as.discontinue_.discontinue->exception);
                }
                break;
            }
            case EX_CONT_PRED: {
                PUSH_EXPR(cur->as.cont_pred_.expr);
                break;
            }
            case EX_REF: { PUSH_EXPR(cur->as.ref_.expr); break; }
            case EX_DEREF: { PUSH_EXPR(cur->as.deref_.expr); break; }
            case EX_GET_FIELD: {
                PUSH_EXPR(cur->as.get_field_.struct_expr);
                break;
            }
            case EX_BORROW_IMMUT: {
                PUSH_EXPR(cur->as.borrow_immut_.expr);
                break;
            }
            case EX_BORROW_MUT: {
                PUSH_EXPR(cur->as.borrow_mut_.expr);
                break;
            }
            case EX_DEFER: {
                /* Defer body's captures are pre-computed; add them directly.
                 * Walking the defer body would require separate collect_defined
                 * tracking for bindings defined inside the defer. */
                for (uint8_t j = 0; j < cur->as.defer_.n_captures; j++) {
                    Binding *db = cur->as.defer_.captures[j];
                    if (!db || db->is_global || db->type.kind == TY_FN) continue;
                    bool in_defs = false;
                    for (uint32_t k = 0; k < ndefs; k++) {
                        if (defs[k] == db) { in_defs = true; break; }
                    }
                    if (in_defs) continue;
                    bool already = false;
                    for (uint32_t k = 0; k < ncaps; k++) {
                        if (caps[k] == db) { already = true; break; }
                    }
                    if (!already) {
                        if (ncaps >= ccaps) {
                            ccaps = (ccaps == 0) ? 8 : ccaps * 2;
                            caps = (Binding **)realloc(caps, ccaps * sizeof(Binding *));
                        }
                        caps[ncaps++] = db;
                    }
                }
                break;
            }
            default:
                break;
        }
    }
#undef PUSH_EXPR

    /* Free heap nodes */
    for (uint32_t i = 0; i < heap_count; i++) free(heap_nodes[i]);
    free(heap_nodes);
    free(defs);

    *n_out = ncaps;
    return caps;
}

