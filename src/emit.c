#include "emit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtins.h"
#include "rc.h"
#include "rc_elision.h"
#include "types.h"

/* Forward declarations */
struct DeferThunk;

typedef struct EmitCtx {
    Buf  *file;     /* file-scope decls (statics, includes) */
    Buf  *main_;    /* main() body */
    int   indent;
    int   tmp_n;
    /* Phase 2: when emitting a function body, these are the parameter bindings
     * that should use raw names (without ID suffix) when referenced. */
    Binding **fn_params;
    uint8_t   n_fn_params;
    /* Phase 3: Track emitted env struct names to avoid duplicates */
    const Symbol **env_struct_names;
    uint8_t   n_env_struct_names;
    uint8_t   cap_env_struct_names;
    /* Phase 3: For closure thunk emission, track the current closure */
    struct Closure *closure;
    const char *env_var_name;  /* Name of the casted env variable (e.g., "__env_4") */
    /* Phase 4 v1: Frame tracking for unified defer model */
    const char *frame_var;    /* Name of current tur_frame variable (e.g., "__frame_3") */
    bool in_scope_with_defers; /* Track if current scope has defers */
    struct DeferThunk *pending_defer_thunks; /* Thunks to emit at file scope */
    /* Phase 4 v1: For defer thunk emission with captures */
    Binding **defer_captures;    /* Current defer thunk's captures (NULL if none) */
    uint8_t n_defer_captures;
    /* Phase 3/4: Track if a return has been emitted in the current scope */
    bool return_emitted;
    /* Phase 19: Buffer for effect handler functions that must be emitted just
     * before the enclosing function definition (never inside a function body). */
    Buf *pending_handler_fns;
} EmitCtx;

/* Phase 4 v1: Defer thunk tracking */
typedef struct DeferThunk {
    char *name;              /* Thunk function name (e.g., "__defer_1") */
    Expr *body;              /* The defer body expression */
    Binding **captures;      /* Captured bindings (NULL if none) */
    uint8_t n_captures;
    char *env_name;          /* Env struct name for captured defers (NULL if no captures) */
    struct DeferThunk *next; /* Linked list */
} DeferThunk;

/* Forward declarations */
static char *emit_value(EmitCtx *ctx, Buf *body, const Expr *e);
static void emit_stmt(EmitCtx *ctx, Buf *body, const Expr *e);
static void emit_fn_def(EmitCtx *ctx, Buf *file, const Expr *e);

/* ------------ helpers ------------ */

/* Helper to create a Type from TypeKind (mirrors the one in types.c). */
static Type type_from_kind(TypeKind k) {
    Type t;
    t.kind = k;
    t.as.fn.arity = 0;
    return t;
}

static void indent_buf(Buf *b, int n) {
    for (int i = 0; i < n; i++) buf_putc(b, ' ');
}

/* Phase 3/4: Helper to check if an expression contains return or throw */
static bool expr_contains_return_or_throw(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_RETURN:
        case EX_THROW:
        case EX_PANIC:
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

static char *fresh_tmp(EmitCtx *ctx) {
    char *p = (char *)malloc(24);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(p, 24, "__t%d", ctx->tmp_n++);
    return p;
}

/* Phase 4 v1: Generate a fresh frame variable name */
static char *fresh_frame(EmitCtx *ctx) {
    char *p = (char *)malloc(24);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(p, 24, "__frame_%d", ctx->tmp_n++);
    return p;
}

/* Phase 4 v1: Generate a fresh defer thunk function name */
static char *fresh_defer_thunk(EmitCtx *ctx) {
    char *p = (char *)malloc(24);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(p, 24, "__defer_%d", ctx->tmp_n++);
    return p;
}

/* Phase 4 v1: Generate a fresh defer env struct name */
static char *fresh_defer_env(EmitCtx *ctx) {
    char *p = (char *)malloc(24);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(p, 24, "__defer_env_%d", ctx->tmp_n++);
    return p;
}

/* Phase 4 v1: Register a defer thunk to be emitted at file scope */
static void register_defer_thunk(EmitCtx *ctx, const char *name, const Expr *body, 
                                  Binding **captures, uint8_t n_captures, 
                                  const char *env_name) {
    DeferThunk *thunk = (DeferThunk *)malloc(sizeof(DeferThunk));
    if (!thunk) { fprintf(stderr, "tur: oom\n"); abort(); }
    thunk->name = (char *)name;
    thunk->body = (Expr *)body;  /* Cast away const for storage */
    thunk->captures = captures;
    thunk->n_captures = n_captures;
    thunk->env_name = env_name ? tur_strdup(env_name) : NULL;
    thunk->next = ctx->pending_defer_thunks;
    ctx->pending_defer_thunks = thunk;
}

/* Phase 4 v1: Emit all registered defer thunks to the output buffer */
static void emit_pending_defer_thunks(EmitCtx *ctx, Buf *out) {
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







/* Return a sanitized C identifier for a Binding, without the ID suffix.
 * Used for function names and parameters. Caller frees. */
static char *raw_name_for_binding(const Binding *b) {
    /* Sanitize the raw name: mangle non-id-safe chars to underscores. */
    char *p = (char *)malloc(b->name->len + 1);
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
    p[k] = '\0';
    return p;
}

/* Return a sanitized C identifier for a Binding. If the binding is a
 * function parameter in the current context, use the raw name (without ID).
 * Otherwise, append the ID suffix. Caller frees. */
static char *name_for_binding(EmitCtx *ctx, const Binding *b) {
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
static void emit_c_string(Buf *out, StrSlice s) {
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

static char *atom_nil(void)         { return tur_strdup("((void)0)"); }
static char *atom_bool(bool b)      { return tur_strdup(b ? "true" : "false"); }
static char *atom_int(int64_t i) {
    char buf[40];
    snprintf(buf, sizeof buf, "INT64_C(%lld)", (long long)i);
    return tur_strdup(buf);
}
static char *atom_float(double f) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.15g", f);
    /* Ensure it's a double literal by appending .0 if needed */
    char *p = strchr(buf, '.');
    char *e = strchr(buf, 'e');
    if (!p && !e) {
        /* No decimal point or exponent - append .0 */
        strcat(buf, ".0");
    }
    return tur_strdup(buf);
}
static char *atom_var(EmitCtx *ctx, const Binding *b) {
    return name_for_binding(ctx, b);
}
static char *atom_cstr(StrSlice s) {
    /* Build into a Buf, then strdup out. */
    Buf b; buf_init(&b);
    emit_c_string(&b, s);
    buf_putc(&b, '\0');
    char *p = tur_strdup(b.data);
    buf_free(&b);
    return p;
}

/* ------------ builtin emitters ------------ */

static char *emit_builtin(EmitCtx *ctx, Buf *body, const Expr *e) {
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
        spec->shape == BS_PRINTLN_CSTR) {
        char *arg = emit_value(ctx, body, args[0]);
        indent_buf(body, ctx->indent);
        switch (spec->shape) {
            case BS_PRINTLN_INT:
                buf_printf(body, "printf(\"%%lld\\n\", (long long)(%s));\n", arg);
                break;
            case BS_PRINTLN_FLOAT:
                buf_printf(body, "printf(\"%%g\\n\", %s);\n", arg);
                break;
            case BS_PRINTLN_BOOL:
                buf_printf(body, "puts((%s) ? \"true\" : \"false\");\n", arg);
                break;
            case BS_PRINTLN_CSTR:
                buf_printf(body, "puts(%s);\n", arg);
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
        default:
            /* unreachable for the shapes handled above */
            buf_puts(&out, "((void)0)");
            break;
    }
    buf_putc(&out, '\0');
    char *result = tur_strdup(out.data);
    buf_free(&out);
    for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
    free(arg_strs);
    return result;
}

/* ------------ block-shaped emitters ------------ */

static char *emit_let_value(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* Phase 3/4: Check if body contains return or throw first */
    bool body_has_return_or_throw = expr_contains_return_or_throw(e->as.let_.body);
    
    char *tmp = NULL;
    bool nil_result = (e->type.kind == TY_NIL);
    if (!nil_result && !body_has_return_or_throw) {
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
                       type_c_name(type_from_kind(b->type.as.fn.result_kind)), bn);
            for (uint8_t j = 0; j < b->type.as.fn.arity; j++) {
                if (j > 0) buf_puts(body, ", ");
                buf_printf(body, "%s", 
                           type_c_name(type_from_kind(b->type.as.fn.arg_kinds[j])));
            }
            buf_printf(body, ") = %s;\n", iv);
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
        /* Body contains return/throw - emit as statement, don't create temp variable */
        emit_stmt(ctx, body, e->as.let_.body);
        /* Close scope */
        ctx->indent -= 4;
        indent_buf(body, ctx->indent);
        buf_puts(body, "}\n");
        /* Return nil - the function will use this to know not to expect a value */
        return atom_nil();
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
    if (!nil_result && !any_has_return_or_throw) {
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
        if (nil_result || any_has_return_or_throw) {
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
            if (last->kind == EX_RETURN || last->kind == EX_THROW || last->kind == EX_PANIC) {
                emit_stmt(ctx, body, last);
                return atom_nil();
            } else if (last->type.kind == TY_NIL) {
                emit_stmt(ctx, body, last);
            } else {
                result = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s;\n", type_c_name(last->type), result);
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
        /* Emit all items, registering defers */
        for (uint32_t i = 0; i < n; i++) {
            const Expr *it = e->as.do_.items[i];
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
            } else {
                emit_stmt(ctx, body, it);
            }
        }
        
        /* Don't emit cleanup code - return/throw already handles it */
        ctx->frame_var = saved_frame;
        free(frame_var);
        return atom_nil();
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

static void emit_while_stmt(EmitCtx *ctx, Buf *body, const Expr *e) {
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

static void emit_set_stmt(EmitCtx *ctx, Buf *body, const Expr *e) {
    char *bn = name_for_binding(ctx, e->as.set_.target);
    char *v = emit_value(ctx, body, e->as.set_.value);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s = %s;\n", bn, v);
    free(bn); free(v);
}

/* Phase 12: emit (set! (@ r) value) - mutation through mutable borrow */
static void emit_set_deref_stmt(EmitCtx *ctx, Buf *body, const Expr *e) {
    char *ref = emit_value(ctx, body, e->as.set_deref_.ref);
    char *val = emit_value(ctx, body, e->as.set_deref_.value);
    const char *inner_type_c = type_c_name(e->as.set_deref_.value->type);
    indent_buf(body, ctx->indent);
    buf_printf(body, "*((%s *)%s) = %s;\n", inner_type_c, ref, val);
    free(ref); free(val);
}

/* ------------ entry points ------------ */

static char *emit_value(EmitCtx *ctx, Buf *body, const Expr *e) {
    switch (e->kind) {
        case EX_NIL_LIT:  return atom_nil();
        case EX_BOOL_LIT: return atom_bool(e->as.b);
        case EX_INT_LIT:  return atom_int(e->as.i);
        case EX_FLOAT_LIT: return atom_float(e->as.f);
        case EX_CSTR_LIT: return atom_cstr(e->as.s);
        case EX_VAR:      return atom_var(ctx, e->as.var.binding);
        case EX_LET:      return emit_let_value(ctx, body, e);
        case EX_IF:       return emit_if_value(ctx, body, e);
        case EX_DO:       return emit_do_value(ctx, body, e);
        case EX_BUILTIN:  return emit_builtin(ctx, body, e);
        case EX_WHILE:    emit_while_stmt(ctx, body, e); return atom_nil();
        case EX_SET:      emit_set_stmt(ctx, body, e);   return atom_nil();
        case EX_SET_DEREF: emit_set_deref_stmt(ctx, body, e); return atom_nil();
        case EX_DEF:      /* handled at top level — shouldn't appear nested. */
        case EX_PROGRAM:
        case EX_TYPECLASS_DEF:
            /* Typeclass definitions are compile-time only - no runtime code */
            return atom_nil();
        case EX_INSTANCE_DEF:
            /* Handled in emit_stmt - file scope only */
            return atom_nil();
        /* Phase 17: Exceptions */
        case EX_THROW: {
            /* (throw expr) - emit as tur_throw call */
            const Expr *payload = e->as.throw_.payload;
            char *payload_val = emit_value(ctx, body, payload);
            
            /* Box primitive types on the heap so they survive the throw */
            char *boxed_val = payload_val;
            if (payload->type.kind == TY_INT) {
                /* Box int: allocate int64_t on heap and copy */
                char *tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "int64_t *%s = (int64_t *)malloc(sizeof(int64_t));\n", tmp);
                indent_buf(body, ctx->indent);
                buf_printf(body, "*%s = %s;\n", tmp, payload_val);
                boxed_val = tmp;
                free(payload_val);
            } else if (payload->type.kind == TY_BOOL) {
                /* Box bool: allocate bool on heap and copy */
                char *tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "bool *%s = (bool *)malloc(sizeof(bool));\n", tmp);
                indent_buf(body, ctx->indent);
                buf_printf(body, "*%s = %s;\n", tmp, payload_val);
                boxed_val = tmp;
                free(payload_val);
            }
            /* For pointer types (cstr, ptr_void, ref, rc, weak), use directly */
            
            /* Emit tur_throw call */
            indent_buf(body, ctx->indent);
            /* Get file and line from span - use hardcoded file for now */
            buf_printf(body, "tur_throw(%d, (void*)%s, %d, \"<unknown>\");\n",
                      payload->type.kind, boxed_val, e->span.line);
            
            if (payload->type.kind == TY_INT || payload->type.kind == TY_BOOL) {
                free(boxed_val); /* Free the tmp name, not the allocation */
            }
            return atom_nil();
        }
        /* Phase R2: Panic */
        case EX_PANIC: {
            /* (panic msg) - evaluate msg (cstr), call tur_panic, abort */
            const Expr *payload = e->as.panic_.payload;
            char *msg_val = emit_value(ctx, body, payload);
            indent_buf(body, ctx->indent);
            if (payload->type.kind == TY_CSTR) {
                buf_printf(body, "tur_panic(%s);\n", msg_val);
            } else {
                /* For non-cstr, use a generic message */
                buf_printf(body, "tur_panic(\"(non-string panic)\");\n");
            }
            free(msg_val);
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
        case EX_TRY: {
            /* (try body (catch ...) (finally ...)) - emit as setjmp/longjmp */
            char *handler_var = fresh_tmp(ctx);
            bool returns_value = (e->type.kind != TY_NIL);
            char *result_tmp = returns_value ? fresh_tmp(ctx) : NULL;

            if (returns_value) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s = (%s)0;\n",
                          type_c_name(e->type), result_tmp, type_c_name(e->type));
            }
            
            /* Emit handler setup with setjmp */
            indent_buf(body, ctx->indent);
            buf_printf(body, "ExceptionHandler %s;\n", handler_var);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s.caught = NULL;\n", handler_var);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s.parent = global_handler_chain;\n", handler_var);
            indent_buf(body, ctx->indent);
            buf_printf(body, "global_handler_chain = &%s;\n", handler_var);
            
            /* Check if there's a finally clause */
            bool has_finally = e->as.try_.finally_body != NULL;
            char *finally_label = has_finally ? fresh_tmp(ctx) : NULL;
            
            /* Emit setjmp-based try */
            indent_buf(body, ctx->indent);
            buf_printf(body, "if (setjmp(%s.jmp_buf) == 0) {\n", handler_var);
            ctx->indent += 4;
            
            /* Emit try body */
            if (returns_value) {
                if (e->as.try_.body->type.kind == TY_NIL) {
                    emit_stmt(ctx, body, e->as.try_.body);
                } else {
                    char *try_val = emit_value(ctx, body, e->as.try_.body);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s = %s;\n", result_tmp, try_val);
                    free(try_val);
                }
            } else {
                emit_stmt(ctx, body, e->as.try_.body);
            }
            
            if (has_finally) {
                /* With finally: pop handler and jump to finally */
                indent_buf(body, ctx->indent);
                buf_puts(body, "global_handler_chain = global_handler_chain->parent;\n");
                indent_buf(body, ctx->indent);
                buf_printf(body, "goto %s;\n", finally_label);
            } else {
                /* Without finally: just pop handler */
                indent_buf(body, ctx->indent);
                buf_puts(body, "global_handler_chain = global_handler_chain->parent;\n");
            }
            
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "} else {\n");
            ctx->indent += 4;
            
            /* Exception was caught - check catch clauses */
            if (e->as.try_.n_clauses > 0) {
                /* Emit if-else chain for catch clauses */
                for (uint8_t i = 0; i < e->as.try_.n_clauses; i++) {
                    TryCatchClause *clause = &e->as.try_.clauses[i];
                    
                    if (i == 0) {
                        /* First clause - no else */
                        indent_buf(body, ctx->indent);
                    } else {
                        /* Subsequent clauses - close previous block and add else */
                        ctx->indent -= 4;
                        indent_buf(body, ctx->indent);
                        buf_puts(body, "} else ");
                        ctx->indent += 4;
                    }
                    
                    /* Check if exception matches this clause */
                    buf_printf(body, "if (tur_exception_matches(%s.caught, %d)) {\n",
                              handler_var, clause->catch_type);
                    ctx->indent += 4;
                    
                    /* Bind catch variable to exception payload */
                    indent_buf(body, ctx->indent);
                    char *catch_name = name_for_binding(ctx, clause->binding);
                    switch (clause->binding->type.kind) {
                        case TY_INT:
                            buf_printf(body, "int64_t %s = *((int64_t *)%s.caught->payload);\n",
                                      catch_name, handler_var);
                            break;
                        case TY_BOOL:
                            buf_printf(body, "bool %s = *((bool *)%s.caught->payload);\n",
                                      catch_name, handler_var);
                            break;
                        default:
                            buf_printf(body, "%s %s = (%s)%s.caught->payload;\n",
                                      type_c_name(clause->binding->type),
                                      catch_name,
                                      type_c_name(clause->binding->type),
                                      handler_var);
                            break;
                    }
                    free(catch_name);
                    
                    /* Emit handler body */
                    if (returns_value) {
                        if (clause->handler->type.kind == TY_NIL) {
                            emit_stmt(ctx, body, clause->handler);
                        } else {
                            char *handler_val = emit_value(ctx, body, clause->handler);
                            indent_buf(body, ctx->indent);
                            buf_printf(body, "%s = %s;\n", result_tmp, handler_val);
                            free(handler_val);
                        }
                    } else {
                        emit_stmt(ctx, body, clause->handler);
                    }
                    
                    /* Clean up */
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "tur_exception_free(%s.caught);\n", handler_var);
                    indent_buf(body, ctx->indent);
                    buf_puts(body, "global_handler_chain = global_handler_chain->parent;\n");
                    
                    if (has_finally) {
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "goto %s;\n", finally_label);
                    }
                    
                    ctx->indent -= 4;
                }
                
                /* No match - rethrow */
                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "} else {\n");
                ctx->indent += 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "global_handler_chain = global_handler_chain->parent;\n");
                if (has_finally) {
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "goto %s;\n", finally_label);
                } else {
                    indent_buf(body, ctx->indent);
                    buf_puts(body, "longjmp(global_handler_chain->jmp_buf, 1);\n");
                }
                ctx->indent -= 4;
                indent_buf(body, ctx->indent);
                buf_puts(body, "}\n");
            } else {
                /* No catch clauses, just rethrow (or go to finally) */
                indent_buf(body, ctx->indent);
                buf_puts(body, "global_handler_chain = global_handler_chain->parent;\n");
                if (has_finally) {
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "goto %s;\n", finally_label);
                } else {
                    indent_buf(body, ctx->indent);
                    buf_puts(body, "longjmp(global_handler_chain->jmp_buf, 1);\n");
                }
            }
            
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
            
            /* Emit finally block if present */
            if (has_finally) {
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s:\n", finally_label);
                emit_stmt(ctx, body, e->as.try_.finally_body);
                free(finally_label);
            }
            
            free(handler_var);
            if (returns_value) {
                return result_tmp;
            }
            return atom_nil();
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
            
            /* Phase 3: Check if this is a closure call */
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
                char *result = tur_strdup(out.data);
                buf_free(&out);
                for (uint32_t i = 0; i <= e->as.call_.n_args; i++) free(arg_strs[i]);
                free(arg_strs);
                free(thunk_name);
                return result;
            }

            /* Callback call through ptr<void> parameter. */
            if (fn_binding->type.kind == TY_PTR_VOID) {
                char *fn_name = raw_name_for_binding(fn_binding);
                char **arg_strs = (char **)malloc(e->as.call_.n_args * sizeof(char *));
                if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    arg_strs[i] = emit_value(ctx, body, e->as.call_.args[i]);
                }

                Buf out; buf_init(&out);
                buf_printf(&out, "((%s (*) (", type_c_name(e->type));
                if (e->as.call_.n_args == 0) {
                    buf_puts(&out, "void");
                } else {
                    for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                        if (i > 0) buf_puts(&out, ", ");
                        buf_puts(&out, type_c_name(e->as.call_.args[i]->type));
                    }
                }
                buf_printf(&out, "))%s) (", fn_name);
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    if (i > 0) buf_puts(&out, ", ");
                    buf_printf(&out, "%s", arg_strs[i]);
                }
                buf_puts(&out, ")");
                buf_putc(&out, '\0');

                char *result = tur_strdup(out.data);
                buf_free(&out);
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) free(arg_strs[i]);
                free(arg_strs);
                free(fn_name);
                return result;
            }
            
            /* Regular function call */
            /* Emit function call: fn(arg1, arg2, ...) */
            /* Use raw name for function calls (functions are defined with raw names) */
            char *fn_name = raw_name_for_binding(fn_binding);
            char **arg_strs = (char **)malloc(e->as.call_.n_args * sizeof(char *));
            if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                arg_strs[i] = emit_value(ctx, body, e->as.call_.args[i]);
            }
            Buf out; buf_init(&out);
            buf_printf(&out, "%s(", fn_name);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                if (i > 0) buf_puts(&out, ", ");
                buf_printf(&out, "%s", arg_strs[i]);
            }
            buf_puts(&out, ")");
            buf_putc(&out, '\0');
            char *result = tur_strdup(out.data);
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
            /* Emit the raw C code inline */
            InlineC *ic = e->as.inline_c_.inline_c;
            /* Just emit the C code as-is */
            buf_write(body, ic->code.p, ic->code.len);
            return atom_nil();
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
            char *inner_type_c = tur_strdup(type_c_name(e->as.ref_.expr->type));
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
            
            /* For ref<T>, we need to cast and dereference */
            if (e->as.deref_.expr->type.kind == TY_REF) {
                char *inner_type_c = tur_strdup(type_c_name(e->type));
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
                
                /* Emit: struct __env_N { int64_t field1; int64_t field2; ... }; */
                buf_printf(ctx->file, "struct %s {", env_name->name);
                for (uint8_t i = 0; i < closure->n_captures; i++) {
                    if (i > 0) buf_puts(ctx->file, "; ");
                    Binding *captured = closure->captures[i];
                    buf_printf(ctx->file, "%s %s",
                               type_c_name(captured->type), captured->name->name);
                }
                buf_puts(ctx->file, "; };\n");
            }
            
            /* Emit the env struct as the closure value */
            /* For Phase 3, closure is the env struct, passed by pointer to thunk */
            
            /* Allocate temp for env struct instance */
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "struct %s %s = {", env_name->name, tmp);
            
            /* Populate with captured values */
            for (uint8_t i = 0; i < closure->n_captures; i++) {
                if (i > 0) buf_puts(body, ", ");
                Binding *captured = closure->captures[i];
                char *cn = name_for_binding(ctx, captured);
                buf_printf(body, ".%s = %s", captured->name->name, cn);
                free(cn);
            }
            buf_puts(body, "};\n");
            
            /* Return a pointer to the env struct as the closure value */
            char *ptr_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "void *%s = &%s;\n", ptr_tmp, tmp);
            
            return ptr_tmp;
        }
        /* Phase 9: rc<T> + weak<T> operations */
        case EX_RC_OF: {
            /* (rc/of x) - allocate control block, copy value into it */
            char *inner = emit_value(ctx, body, e->as.rc_of_.expr);
            char *inner_type_c = tur_strdup(type_c_name(e->as.rc_of_.expr->type));
            
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
            return tur_strdup(inner);
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
            /* (cont? k) - check if a Phase 18 continuation has not been consumed.
             * For Phase 19 algebraic-effect continuations (CK_MOVE int64_t dummy),
             * this always returns true since the static one-shot check prevents
             * a double-resume before we reach this point.
             * For Phase 18 tur_cont* continuations, call tur_cont_consumed(). */
            char *inner = emit_value(ctx, body, e->as.cont_pred_.expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            if (e->as.cont_pred_.expr->type.kind == TY_CONT) {
                buf_printf(body, "bool %s = !tur_cont_consumed((tur_cont *)(intptr_t)%s);\n", tmp, inner);
            } else {
                /* Phase P19-5: Phase-19 k is a TurContK* cast to int64_t.
                 * Check the consumed flag for runtime freshness verification. */
                buf_printf(body, "bool %s = !((TurContK *)(intptr_t)%s)->consumed;\n", tmp, inner);
            }
            free(inner);
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
             * Emits a static handler function per case, builds a
             * EffectHandlerFrame on the stack, runs body, pops frame.
             *
             * v1 limitation: handler bodies cannot capture outer-scope
             * variables (no closure analysis for handler functions yet).
             */
            HandleExpr *h = e->as.handle_.handle;
            bool returns_value = (e->type.kind != TY_NIL);

            /* Allocate handler function names */
            char **hfn_names = (char **)malloc(h->n_cases * sizeof(char *));
            for (uint8_t i = 0; i < h->n_cases; i++) {
                hfn_names[i] = (char *)malloc(32);
                snprintf(hfn_names[i], 32, "__effect_handler_%d", ctx->tmp_n++);
            }

            /* Emit static handler functions to pending buffer (flushed before
             * the enclosing function definition, so they land at file scope). */
            for (uint8_t i = 0; i < h->n_cases; i++) {
                HandleCase *c = &h->cases[i];
                Buf *hbuf = ctx->pending_handler_fns;

                /* Forward declaration */
                buf_printf(hbuf,
                    "static int64_t %s(int64_t *__effect_args, int __n_effect_args,"
                    " int64_t __k, void *__env);\n", hfn_names[i]);

                /* Function definition */
                buf_printf(hbuf,
                    "static int64_t %s(int64_t *__effect_args, int __n_effect_args,"
                    " int64_t __k, void *__env) {\n", hfn_names[i]);

                /* Unpack effect arguments into named locals using the correct C type. */
                for (uint8_t j = 0; j < c->n_params; j++) {
                    if (c->param_bindings && c->param_bindings[j]) {
                        const char *ctype = type_c_name(c->param_bindings[j]->type);
                        char *raw = raw_name_for_binding(c->param_bindings[j]);
                        buf_printf(hbuf,
                            "    %s %s_%u = (%s)__effect_args[%d];\n",
                            ctype, raw, (unsigned)c->param_bindings[j]->id, ctype, j);
                        free(raw);
                    }
                }

                /* Bind k */
                if (c->k_binding) {
                    char *raw = raw_name_for_binding(c->k_binding);
                    buf_printf(hbuf,
                        "    int64_t %s_%u = __k;\n",
                        raw, (unsigned)c->k_binding->id);
                    free(raw);
                }

                /* Emit handler body; use hbuf for body text and also as the
                 * pending_handler_fns target so nested handles work. */
                EmitCtx hctx = *ctx;
                hctx.file = hbuf;
                hctx.pending_handler_fns = hbuf;
                hctx.indent = 4;
                hctx.fn_params = NULL;
                hctx.n_fn_params = 0;
                hctx.closure = NULL;
                hctx.env_var_name = NULL;
                hctx.defer_captures = NULL;
                hctx.n_defer_captures = 0;
                hctx.frame_var = NULL;
                hctx.return_emitted = false;

                if (c->body && c->body->type.kind == TY_NIL) {
                    /* Void-typed body: emit as statement, then return 0 */
                    emit_stmt(&hctx, hbuf, c->body);
                    buf_puts(hbuf, "    return 0;\n");
                } else {
                    char *hret = emit_value(&hctx, hbuf, c->body);
                    buf_printf(hbuf, "    return (int64_t)%s;\n", hret);
                    free(hret);
                }
                buf_puts(hbuf, "}\n\n");

                /* Propagate counter back */
                ctx->tmp_n = hctx.tmp_n;
            }

            /* Emit EffectHandlerFrame on the stack */
            char *frame_var = (char *)malloc(32);
            snprintf(frame_var, 32, "__eff_frame_%d", ctx->tmp_n++);

            indent_buf(body, ctx->indent);
            buf_printf(body, "EffectHandlerFrame %s;\n", frame_var);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s.parent = global_effect_handler_chain;\n", frame_var);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s.n_cases = %d;\n", frame_var, h->n_cases);
            for (uint8_t i = 0; i < h->n_cases; i++) {
                HandleCase *c = &h->cases[i];
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s.cases[%d].effect_name = \"%s\";\n",
                           frame_var, i, c->effect_name->name);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s.cases[%d].handler_fn = %s;\n",
                           frame_var, i, hfn_names[i]);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s.cases[%d].env = NULL;\n", frame_var, i);
            }
            indent_buf(body, ctx->indent);
            buf_printf(body, "global_effect_handler_chain = &%s;\n", frame_var);

            /* Evaluate the body */
            char *result = fresh_tmp(ctx);
            if (returns_value) {
                char *bv = emit_value(ctx, body, h->body);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s = %s;\n", type_c_name(e->type), result, bv);
                free(bv);
            } else {
                emit_stmt(ctx, body, h->body);
            }

            /* Pop frame */
            indent_buf(body, ctx->indent);
            buf_puts(body, "global_effect_handler_chain = global_effect_handler_chain->parent;\n");

            /* Cleanup */
            for (uint8_t i = 0; i < h->n_cases; i++) free(hfn_names[i]);
            free(hfn_names);
            free(frame_var);

            if (returns_value) return result;
            free(result);
            return atom_nil();
        }
        case EX_RESUME: {
            /* (resume k value) - resume continuation with value.
             *
             * v1 direct-style semantics: resume is the last expression in a
             * handler body; it returns its value argument (k is a dummy int).
             * The handler function returns this value to tur_effect_perform,
             * which returns it as the result of perform at the call site.
             *
             * Phase P19-5 k-freshness: for Phase-19 TY_INT k values (which are
             * actually TurContK* cast to int64_t), mark the continuation token
             * as consumed before returning, enabling (cont? k) runtime checks.
             */
            if (e->as.resume_.resume) {
                char *k_var = emit_value(ctx, body, e->as.resume_.resume->k);
                /* Mark TurContK consumed for Phase 19 k (TY_INT with CK_MOVE). */
                if (e->as.resume_.resume->k->type.kind == TY_INT) {
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "((TurContK *)(intptr_t)%s)->consumed = true;\n", k_var);
                }
                free(k_var);
                /* Return the resume value */
                return emit_value(ctx, body, e->as.resume_.resume->value);
            }
            return atom_nil();
        }
        case EX_DISCONTINUE: {
            /* (discontinue k exception) - discontinue with exception.
             * v1: throw the exception; k is discarded.
             * Phase P19-5: mark TurContK consumed for Phase-19 TY_INT k values.
             */
            if (e->as.discontinue_.discontinue) {
                char *k_var = emit_value(ctx, body, e->as.discontinue_.discontinue->k);
                if (e->as.discontinue_.discontinue->k->type.kind == TY_INT) {
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "((TurContK *)(intptr_t)%s)->consumed = true;\n", k_var);
                }
                free(k_var);
                char *e_var = emit_value(ctx, body, e->as.discontinue_.discontinue->exception);
                indent_buf(body, ctx->indent);
                buf_printf(body, "tur_throw(5, (void*)%s, __LINE__, \"<unknown>\");\n", e_var);
                free(e_var);
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
                if (i > 0) buf_puts(&lit, ", ");
                buf_printf(&lit, ".%s = %s", def->fields[i].name, fv);
                free(fv);
            }
            buf_puts(&lit, "}");
            buf_putc(&lit, '\0');
            char *result = tur_strdup(lit.data);
            buf_free(&lit);
            return result;
        }
        case EX_GET_FIELD: {
            /* (.field s) - emit s.field_name */
            char *sv = emit_value(ctx, body, e->as.get_field_.struct_expr);
            StructDef *def = e->as.get_field_.def;
            const char *fname = def->fields[e->as.get_field_.field_idx].name;
            Buf lit; buf_init(&lit);
            buf_printf(&lit, "(%s).%s", sv, fname);
            buf_putc(&lit, '\0');
            free(sv);
            char *result = tur_strdup(lit.data);
            buf_free(&lit);
            return result;
        }
    }
    return atom_nil();
}

static void emit_stmt(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* Run e for side effects, discard value. */
    switch (e->kind) {
        case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT:
        case EX_FLOAT_LIT: case EX_CSTR_LIT: case EX_VAR:
        case EX_TYPECLASS_DEF:
            /* No side effects — emit nothing. */
            return;
        case EX_WHILE: emit_while_stmt(ctx, body, e); return;
        case EX_SET:      emit_set_stmt(ctx, body, e);       return;
        case EX_SET_DEREF: emit_set_deref_stmt(ctx, body, e); return;
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
                } else {
                    emit_stmt(ctx, body, it);
                }
            }

            /* Fire all defers LIFO at scope exit */
            indent_buf(body, ctx->indent);
            buf_printf(body, "tur_frame_fire_lifo(&%s);\n", frame_var);

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
        /* Phase 17: Exceptions */
        case EX_THROW: {
            /* (throw expr) - emit as statement with tur_throw call */
            /* Delegate to emit_value which handles the full implementation */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        /* Phase R2: Panic */
        case EX_PANIC: {
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
        case EX_TRY: {
            /* (try body (catch ...) (finally ...)) - emit as setjmp/longjmp */
            /* Delegate to emit_value which handles the full implementation */
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
        case EX_INSTANCE_DEF: {
            /* Phase 15: Emit dictionary struct and global singleton */
            TypeClassInstance *inst = e->as.instance_def_.instance;
            TypeClass *tc = inst->typeclass;
            
            /* Generate dictionary struct name: dict_<TypeClass>_<typeargs> */
            char dict_name[128];
            char type_suffix[64] = "";
            for (uint8_t i = 0; i < inst->n_type_args; i++) {
                if (i == 0) strcat(type_suffix, "_");
                switch (inst->type_args[i].kind) {
                    case TY_INT: strcat(type_suffix, "int"); break;
                    case TY_BOOL: strcat(type_suffix, "bool"); break;
                    case TY_CSTR: strcat(type_suffix, "cstr"); break;
                    case TY_NIL: strcat(type_suffix, "nil"); break;
                    case TY_PTR_VOID: strcat(type_suffix, "ptr_void"); break;
                    default: strcat(type_suffix, "T"); break;
                }
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
                
                /* Return type */
                const char *ret_c_name = type_c_name(method_impl->body->type);
                buf_printf(ctx->file, "%s", ret_c_name);
                buf_puts(ctx->file, " (*");
                buf_printf(ctx->file, "%s", sanitized_method_name);
                buf_puts(ctx->file, ")(");
                
                /* Parameter types */
                for (uint8_t j = 0; j < method_impl->n_params; j++) {
                    if (j > 0) buf_puts(ctx->file, ", ");
                    buf_printf(ctx->file, "%s", type_c_name(method_impl->param_types[j]));
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
                /* Method implementation function name - use sanitized method name */
                buf_printf(ctx->file, "__inst_%s_%s", tc->name->name, sanitized_method_name);
                /* Add type suffix */
                buf_puts(ctx->file, type_suffix);
                buf_puts(ctx->file, ",\n");
            }
            buf_printf(ctx->file, "};\n\n");
            return;
        }
        case EX_INLINE_C: {
            /* Emit the raw C code inline as a statement */
            InlineC *ic = e->as.inline_c_.inline_c;
            indent_buf(body, ctx->indent);
            buf_write(body, ic->code.p, ic->code.len);
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
        case EX_CONT_PRED: {
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
        /* Phase 19: Algebraic effects */
        case EX_DEFECT:
            /* Effect definitions are compile-time only */
            return;
        case EX_PERFORM:
        case EX_HANDLE:
        case EX_RESUME:
        case EX_DISCONTINUE:
        case EX_MAKE_STRUCT:
        case EX_GET_FIELD: {
            /* These should be lowered by effect_lower/CPS passes.
             * Fallback: emit via emit_value and discard result.
             * Do not recurse into emit_stmt with the same node. */
            char *v = emit_value(ctx, body, e);
            free(v);
            return;
        }
    }
}

/* ------------ Phase 2: function emission ------------ */

static void emit_fn_def(EmitCtx *ctx, Buf *file, const Expr *e) {
    FnDef *fd = e->as.fn_def_.fn;
    /* Use raw name (without ID suffix) for function name */
    const char *fn_name = raw_name_for_binding(fd->binding);

    /* Phase 19: Drain any pending effect handler functions accumulated while
     * elaborating the PREVIOUS top-level expression.  They must appear before
     * this function definition so they are at file scope. */
    if (ctx->pending_handler_fns && ctx->pending_handler_fns->len > 0) {
        buf_write(file, ctx->pending_handler_fns->data, ctx->pending_handler_fns->len);
        buf_free(ctx->pending_handler_fns);
        buf_init(ctx->pending_handler_fns);
    }

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
            if (ctx->n_env_struct_names >= ctx->cap_env_struct_names) {
                ctx->cap_env_struct_names = ctx->cap_env_struct_names ? ctx->cap_env_struct_names * 2 : 8;
                ctx->env_struct_names = (const Symbol **)realloc(ctx->env_struct_names,
                    ctx->cap_env_struct_names * sizeof(const Symbol *));
            }
            ctx->env_struct_names[ctx->n_env_struct_names++] = env_name;
            
            buf_printf(file, "struct %s {", env_name->name);
            for (uint8_t i = 0; i < fd->closure->n_captures; i++) {
                if (i > 0) buf_puts(file, "; ");
                Binding *captured = fd->closure->captures[i];
                buf_printf(file, "%s %s",
                           type_c_name(captured->type), captured->name->name);
            }
            buf_puts(file, "; };\n");
        }
    }

    /* Emit function signature */
    /* Special case: C's main() must return int, not int64_t */
    bool is_main = (strcmp(fn_name, "main") == 0);
    if (!is_main) {
        buf_printf(file, "static ");
    }
    /* Return type from fn_type */
    if (e->type.kind == TY_FN) {
        TypeKind result = e->type.as.fn.result_kind;
        if (is_main) {
            buf_puts(file, "int");  /* C main must always return int */
        } else {
            buf_puts(file, type_c_name(type_from_kind(result)));
        }
    } else {
        buf_puts(file, "void");
    }
    buf_printf(file, " %s(", fn_name);

    /* Emit parameters - use raw names (without ID suffix) */
    for (uint8_t i = 0; i < fd->n_params; i++) {
        if (i > 0) buf_puts(file, ", ");
        /* Use the param's type for emission */
        buf_puts(file, type_c_name(fd->param_types[i]));
        const char *pn = raw_name_for_binding(fd->params[i]);
        buf_printf(file, " %s", pn);
        free((void*)pn);
    }
    buf_puts(file, ") {\n");

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
            ctx->env_var_name = tur_strdup(env_var_name_buf);
        }
    }

    /* Get the return type */
    TypeKind result_kind = e->type.kind == TY_FN ? e->type.as.fn.result_kind : TY_NIL;

    /* Phase 3/4: Check if body contains return/throw */
    bool body_has_return_or_throw = expr_contains_return_or_throw(fd->body);
    
    if (body_has_return_or_throw) {
        /* Body contains a return/throw - emit as statements only */
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
                case TY_INT:   ret_val = tur_strdup("0"); break;
                case TY_BOOL:  ret_val = tur_strdup("false"); break;
                default:       ret_val = tur_strdup("0"); break;
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
    ctx->closure = saved_closure;
    free((void*)ctx->env_var_name);
    ctx->env_var_name = saved_env_var_name;

    ctx->indent -= 4;
    buf_printf(file, "}\n\n");
    free((void*)fn_name);
}





/* ------------ program-level emit ------------ */

int emit_program(Buf *out, const Expr *program) {
    if (!program || program->kind != EX_PROGRAM) {
        fprintf(stderr, "tur: emit: expected EX_PROGRAM\n");
        return -1;
    }

    /* Two buffers: file scope (statics) and main body. We assemble at the end. */
    Buf file; buf_init(&file);
    Buf body; buf_init(&body);
    /* Phase 19: separate buffers for ordered final assembly:
     *   early_file   - pass 0: struct typedefs + drop glue
     *   fwd_decls    - pass 1: function forward declarations
     *   extern_decls - user extern-c declarations
     *   file         - pass 2: function definitions + globals
     *   pending_handler_fns emitted between fwd_decls and file
     * Order ensures: struct typedefs visible to fwd_decls; fwd_decls visible
     * to handler functions; handler functions visible to fn definitions. */
    Buf early_file;  buf_init(&early_file);
    Buf fwd_decls;   buf_init(&fwd_decls);
    Buf extern_decls; buf_init(&extern_decls);

    EmitCtx ctx;
    ctx.file = &file;
    ctx.main_ = &body;
    ctx.indent = 4;
    ctx.tmp_n = 0;
    ctx.fn_params = NULL;
    ctx.n_fn_params = 0;
    /* Phase 3: closure tracking */
    ctx.closure = NULL;
    ctx.env_var_name = NULL;
    /* Phase 3: env struct tracking */
    ctx.env_struct_names = NULL;
    ctx.n_env_struct_names = 0;
    ctx.cap_env_struct_names = 0;
    /* Phase 4 v1: frame tracking */
    ctx.frame_var = NULL;
    ctx.in_scope_with_defers = false;
    ctx.pending_defer_thunks = NULL;
    /* Phase 4 v1: defer captures tracking */
    ctx.defer_captures = NULL;
    ctx.n_defer_captures = 0;
    /* Phase 3/4: Track return emission */
    ctx.return_emitted = false;
    /* Phase 19: Pending effect handler function buffer */
    Buf pending_hfns; buf_init(&pending_hfns);
    ctx.pending_handler_fns = &pending_hfns;

    /* Check if user defined a main function */
    bool user_has_main = false;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *e = program->as.program.items[i];
        if (e->kind == EX_FN_DEF) {
            FnDef *fd = e->as.fn_def_.fn;
            if (strcmp(fd->binding->name->name, "main") == 0) {
                user_has_main = true;
                break;
            }
        }
    }

    /* Phase 2: Two-pass emission for mutual recursion support.
     * Pass 0: Emit struct typedefs + drop glue (must precede function forward decls). */
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *e = program->as.program.items[i];
        if (e->kind == EX_DEF && e->as.def_.struct_def) {
            StructDef *def = e->as.def_.struct_def;
            /* Emit: typedef struct Name { fields... } Name; */
            buf_printf(&early_file, "typedef struct %s {\n", def->name);
            for (uint32_t j = 0; j < def->n_fields; j++) {
                StructField *f = &def->fields[j];
                const char *ctype;
                switch (f->kind) {
                    case TY_INT:      ctype = "int64_t"; break;
                    case TY_BOOL:     ctype = "bool"; break;
                    case TY_FLOAT:    ctype = "double"; break;
                    case TY_CSTR:     ctype = "const char *"; break;
                    case TY_PTR_VOID: ctype = "void *"; break;
                    case TY_RC:
                    case TY_WEAK:     ctype = "RcControlBlock *"; break;
                    case TY_REF:      ctype = "void *"; break;
                    default:          ctype = "int64_t"; break;
                }
                buf_printf(&early_file, "    %s %s;\n", ctype, f->name);
            }
            buf_printf(&early_file, "} %s;\n\n", def->name);
            /* If any RC/ref/weak field, emit drop glue function */
            if (def->needs_drop_glue) {
                buf_printf(&early_file, "static void drop_glue_%s(void *ptr) {\n", def->name);
                buf_printf(&early_file, "    if (!ptr) return;\n");
                buf_printf(&early_file, "    %s *s = (%s *)ptr;\n", def->name, def->name);
                /* Drop fields in REVERSE order */
                for (int32_t j = (int32_t)def->n_fields - 1; j >= 0; j--) {
                    StructField *f = &def->fields[j];
                    if (f->kind == TY_RC) {
                        buf_printf(&early_file, "    if (s->%s) { rc_strong_decrement(s->%s); rc_free_queue_drain(); }\n",
                                   f->name, f->name);
                    } else if (f->kind == TY_WEAK) {
                        buf_printf(&early_file, "    if (s->%s) rc_weak_decrement(s->%s);\n",
                                   f->name, f->name);
                    } else if (f->kind == TY_REF) {
                        buf_printf(&early_file, "    if (s->%s) free(s->%s);\n",
                                   f->name, f->name);
                    }
                }
                buf_printf(&early_file, "    free(ptr);\n");
                buf_printf(&early_file, "}\n\n");
            }
        }
    }

    /* Pass 1: Emit forward declarations for all functions.
     * Written to fwd_decls buffer (emitted before pending_handler_fns in final
     * assembly) so that effect handler functions can call user-defined functions. */
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *e = program->as.program.items[i];
        if (e->kind == EX_FN_DEF) {
            FnDef *fd = e->as.fn_def_.fn;
            /* Skip main - it's not called from other functions in the same file */
            if (strcmp(fd->binding->name->name, "main") == 0) continue;
            /* Emit forward declaration with static */
            buf_puts(&fwd_decls, "static ");
            if (e->type.kind == TY_FN) {
                TypeKind result = e->type.as.fn.result_kind;
                buf_puts(&fwd_decls, type_c_name(type_from_kind(result)));
            } else {
                buf_puts(&fwd_decls, "void");
            }
            const char *fn_name = raw_name_for_binding(fd->binding);
            buf_printf(&fwd_decls, " %s(", fn_name);
            for (uint8_t j = 0; j < fd->n_params; j++) {
                if (j > 0) buf_puts(&fwd_decls, ", ");
                buf_puts(&fwd_decls, type_c_name(fd->param_types[j]));
            }
            buf_puts(&fwd_decls, ");\n");
            free((void*)fn_name);
        }
    }

    /* Pass 2: collect all top-level defs and fn_defs. */
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *e = program->as.program.items[i];
        if (e->kind == EX_DEF) {
            /* Phase 11: skip struct typedefs — already emitted in Pass 0 */
            if (e->as.def_.struct_def) continue;
            char *bn = name_for_binding(&ctx, e->as.def_.binding);
            buf_printf(&file, "static %s %s;\n",
                       type_c_name(e->as.def_.binding->type), bn);
            if (e->as.def_.init) {
                char *iv = emit_value(&ctx, &body, e->as.def_.init);
                indent_buf(&body, ctx.indent);
                buf_printf(&body, "%s = %s;\n", bn, iv);
                free(iv);
            }
            free(bn);
        } else if (e->kind == EX_FN_DEF) {
            /* Emit function definition at file scope */
            emit_fn_def(&ctx, &file, e);
        } else if (e->kind == EX_EXTERN_C) {
            /* Emit extern-c declaration early (before handler functions) */
            ExternC *ec = e->as.extern_c_.ext;
            buf_printf(&extern_decls, "extern %s %s(",
                       type_c_name(ec->return_type),
                       ec->c_name->name);
            for (uint8_t j = 0; j < ec->n_params; j++) {
                if (j > 0) buf_puts(&extern_decls, ", ");
                buf_printf(&extern_decls, "%s", type_c_name(ec->param_types[j]));
            }
            buf_puts(&extern_decls, ");\n");
        } else {
            emit_stmt(&ctx, &body, e);
        }
    }

    /* Final assembly. */
    buf_puts(out, "/* generated by tur (phase 2) */\n");
    /* Suppress warnings for unused helpers that are part of the runtime preamble
     * but not exercised by every program.  Both GCC and Clang honour these. */
    buf_puts(out, "#pragma GCC diagnostic ignored \"-Wunused-function\"\n");
    buf_puts(out, "#pragma GCC diagnostic ignored \"-Wunused-variable\"\n");
    buf_puts(out, "#include <stdio.h>\n");
    buf_puts(out, "#include <stdint.h>\n");
    buf_puts(out, "#include <stdbool.h>\n");
    /* Phase 17: Exceptions - setjmp/longjmp */
    buf_puts(out, "#include <setjmp.h>\n");
    /* Phase T19: Thread primitives - pthread on all supported platforms */
    buf_puts(out, "#include <pthread.h>\n");
    /* Phase T21: Fiber context switching via ucontext_t (POSIX; deprecated on
     * macOS but present; suppress warning). */
    buf_puts(out, "#define _XOPEN_SOURCE 700\n");
    buf_puts(out, "#include <ucontext.h>\n");
    buf_puts(out, "#undef _XOPEN_SOURCE\n");
    /* Phase 5: Declare malloc and free for ref<T> without including stdlib.h
     * to avoid conflicts with user-declared functions like exit. */
    buf_puts(out, "extern void *malloc(size_t);\n");
    buf_puts(out, "extern void *calloc(size_t, size_t);\n");
    buf_puts(out, "extern void free(void *);\n");
    /* Phase 9: Declare abort and memset for rc<T> support */
    buf_puts(out, "extern void abort(void);\n");
    buf_puts(out, "extern void *memset(void *, int, size_t);\n");
    buf_puts(out, "extern void *memmove(void *, const void *, size_t);\n");
    /* Phase 19: strcmp for effect handler name matching */
    buf_puts(out, "extern int strcmp(const char *, const char *);\n");
    buf_puts(out, "\n");
    /* Phase 7 follow-up: minimal in-process test registry for stdlib/test.tur. */
    buf_puts(out, "#define TUR_TEST_REGISTRY_MAX 1024\n");
    buf_puts(out, "typedef int64_t (*tur_test_callback_t)(void);\n");
    buf_puts(out, "static const char *tur_test_registry_names[TUR_TEST_REGISTRY_MAX];\n");
    buf_puts(out, "static tur_test_callback_t tur_test_registry_fns[TUR_TEST_REGISTRY_MAX];\n");
    buf_puts(out, "static int64_t tur_test_registry_count = 0;\n\n");
    buf_puts(out, "int64_t tur_test_register(const char *name, void *test_fn) {\n");
    buf_puts(out, "    if (!test_fn) return 0;\n");
    buf_puts(out, "    if (tur_test_registry_count >= TUR_TEST_REGISTRY_MAX) return 0;\n");
    buf_puts(out, "    tur_test_registry_names[tur_test_registry_count] = name ? name : \"<unnamed>\";\n");
    buf_puts(out, "    tur_test_registry_fns[tur_test_registry_count] = (tur_test_callback_t)test_fn;\n");
    buf_puts(out, "    tur_test_registry_count++;\n");
    buf_puts(out, "    return 1;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "int64_t tur_test_run_all(void) {\n");
    buf_puts(out, "    int64_t passed = 0;\n");
    buf_puts(out, "    int64_t failed = 0;\n");
    buf_puts(out, "    for (int64_t i = 0; i < tur_test_registry_count; i++) {\n");
    buf_puts(out, "        tur_test_callback_t fn = tur_test_registry_fns[i];\n");
    buf_puts(out, "        int64_t rc = fn ? fn() : 0;\n");
    buf_puts(out, "        if (rc == 1) {\n");
    buf_puts(out, "            putchar('.');\n");
    buf_puts(out, "            putchar('\\n');\n");
    buf_puts(out, "            passed++;\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "            putchar('F');\n");
    buf_puts(out, "            putchar('\\n');\n");
    buf_puts(out, "            printf(\"%s\\n\", tur_test_registry_names[i]);\n");
    buf_puts(out, "            failed++;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    printf(\"summary: %lld passed, %lld failed\\n\",\n");
    buf_puts(out, "           (long long)passed, (long long)failed);\n");
    buf_puts(out, "    return failed == 0 ? 0 : 1;\n");
    buf_puts(out, "}\n\n");
    /* Phase 4 v1 lowering: emit tur_frame inline from runtime.h */
    buf_puts(out, "/* tur_frame - phase 4 v1 lowering (from runtime.h) */\n");
    buf_puts(out, "typedef void (*defer_fn_t)(void *env);\n");
    buf_puts(out, "#define TUR_FRAME_MAX_DEFERS 32\n\n");
    buf_puts(out, "typedef struct tur_frame {\n");
    buf_puts(out, "    defer_fn_t defers[TUR_FRAME_MAX_DEFERS];\n");
    buf_puts(out, "    void *envs[TUR_FRAME_MAX_DEFERS];\n");
    buf_puts(out, "    int n;\n");
    buf_puts(out, "    struct tur_frame *parent;\n");
    buf_puts(out, "    bool may_capture;\n");
    buf_puts(out, "} tur_frame;\n\n");
    buf_puts(out, "static inline void tur_frame_init(tur_frame *f, tur_frame *parent) {\n");
    buf_puts(out, "    f->n = 0; f->parent = parent; f->may_capture = false;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static inline int tur_frame_push_defer(tur_frame *f, defer_fn_t thunk, void *env) {\n");
    buf_puts(out, "    if (f->n >= TUR_FRAME_MAX_DEFERS) return -1;\n");
    buf_puts(out, "    f->defers[f->n] = thunk;\n");
    buf_puts(out, "    f->envs[f->n] = env;\n");
    buf_puts(out, "    f->n++;\n");
    buf_puts(out, "    return 0;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static void tur_frame_fire_lifo(tur_frame *f) {\n");
    buf_puts(out, "    for (int i = f->n - 1; i >= 0; i--) f->defers[i](f->envs[i]);\n");
    buf_puts(out, "    f->n = 0;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static void tur_frame_fire_chain(tur_frame *f) {\n");
    buf_puts(out, "    tur_frame *frames[64];\n");
    buf_puts(out, "    int n_frames = 0;\n");
    buf_puts(out, "    for (tur_frame *cur = f; cur != NULL && n_frames < 64; cur = cur->parent) {\n");
    buf_puts(out, "        frames[n_frames++] = cur;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    for (int i = n_frames - 1; i >= 0; i--) {\n");
    buf_puts(out, "        tur_frame_fire_lifo(frames[i]);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    
    /* Phase 17: Exception runtime (inline from exn.h/c) */
    buf_puts(out, "/* Exception handling - Phase 17 */\n");
    buf_puts(out, "typedef struct tur_exception tur_exception;\n\n");
    buf_puts(out, "struct tur_exception {\n");
    buf_puts(out, "    int payload_type;      /* TypeKind enum value */\n");
    buf_puts(out, "    void *payload;         /* The exception payload */\n");
    buf_puts(out, "    int line;              /* Line where thrown */\n");
    buf_puts(out, "    const char *file;      /* File where thrown */\n");
    buf_puts(out, "    tur_exception *cause;  /* Chained exception */\n");
    buf_puts(out, "};\n\n");
    
    /* Exception handler chain */
    buf_puts(out, "typedef struct ExceptionHandler ExceptionHandler;\n");
    buf_puts(out, "struct ExceptionHandler {\n");
    buf_puts(out, "    jmp_buf jmp_buf;\n");
    buf_puts(out, "    int active;\n");
    buf_puts(out, "    tur_exception *caught;\n");
    buf_puts(out, "    ExceptionHandler *parent;\n");
    buf_puts(out, "};\n\n");
    
    /* Thread-local exception handler chain (single-threaded v1) */
    buf_puts(out, "static ExceptionHandler *global_handler_chain = NULL;\n\n");
    
    /* Exception helper functions */
    buf_puts(out, "ExceptionHandler *exn_push_handler(void) {\n");
    buf_puts(out, "    ExceptionHandler *h = (ExceptionHandler *)malloc(sizeof(ExceptionHandler));\n");
    buf_puts(out, "    if (!h) { fprintf(stderr, \"exn: out of memory\\n\"); abort(); }\n");
    buf_puts(out, "    h->active = 1;\n");
    buf_puts(out, "    h->caught = NULL;\n");
    buf_puts(out, "    h->parent = global_handler_chain;\n");
    buf_puts(out, "    global_handler_chain = h;\n");
    buf_puts(out, "    return h;\n");
    buf_puts(out, "}\n\n");
    
    buf_puts(out, "ExceptionHandler *exn_pop_handler(void) {\n");
    buf_puts(out, "    ExceptionHandler *old = global_handler_chain;\n");
    buf_puts(out, "    if (old) global_handler_chain = old->parent;\n");
    buf_puts(out, "    return old;\n");
    buf_puts(out, "}\n\n");
    
    buf_puts(out, "void tur_exception_free(tur_exception *exn) {\n");
    buf_puts(out, "    if (!exn) return;\n");
    buf_puts(out, "    if (exn->cause) { tur_exception_free(exn->cause); }\n");
    buf_puts(out, "    /* Only free heap-allocated payloads (int=3, bool=2). */\n");
    buf_puts(out, "    /* cstr/ptr payloads point to literals or external memory, not owned. */\n");
    buf_puts(out, "    if (exn->payload_type == 3 || exn->payload_type == 2) {\n");
    buf_puts(out, "        free(exn->payload);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    free(exn);\n");
    buf_puts(out, "}\n\n");
    
    buf_puts(out, "bool tur_exception_matches(tur_exception *exn, int expected_type) {\n");
    buf_puts(out, "    if (!exn) return false;\n");
    buf_puts(out, "    if (exn->payload_type == expected_type) return true;\n");
    buf_puts(out, "    if (expected_type == 1) return true;  /* TY_NIL = 1 = catch-all */\n");
    buf_puts(out, "    return false;\n");
    buf_puts(out, "}\n\n");
    
    /* tur_throw - called from user code */
    buf_puts(out, "void tur_throw(int payload_type, void *payload, int line, const char *file) {\n");
    buf_puts(out, "    tur_exception *exn = (tur_exception *)malloc(sizeof(tur_exception));\n");
    buf_puts(out, "    if (!exn) { abort(); }\n");
    buf_puts(out, "    exn->payload_type = payload_type;\n");
    buf_puts(out, "    exn->payload = payload;\n");
    buf_puts(out, "    exn->line = line;\n");
    buf_puts(out, "    exn->file = file;\n");
    buf_puts(out, "    exn->cause = NULL;\n");
    buf_puts(out, "    ExceptionHandler *h = global_handler_chain;\n");
    buf_puts(out, "    if (h) {\n");
    buf_puts(out, "        if (h->caught) tur_exception_free(h->caught);\n");
    buf_puts(out, "        h->caught = exn;\n");
    buf_puts(out, "        h->active = 0;\n");
    buf_puts(out, "        longjmp(h->jmp_buf, 1);\n");
    buf_puts(out, "    } else {\n");
    buf_puts(out, "        fprintf(stderr, \"Uncaught exception thrown at %s:%d\\n\", file ? file : \"<unknown>\", line);\n");
    buf_puts(out, "        tur_exception_free(exn);\n");
    buf_puts(out, "        abort();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");

    /* Phase R2: tur_panic */
    buf_puts(out, "/* Phase R2: tur_panic */\n");
    buf_puts(out, "static int tur_panic_in_progress = 0;\n");
    buf_puts(out, "static void tur_panic(const char *msg) {\n");
    buf_puts(out, "    if (tur_panic_in_progress) {\n");
    buf_puts(out, "        fprintf(stderr, \"double panic: aborting\\n\");\n");
    buf_puts(out, "        abort();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    tur_panic_in_progress = 1;\n");
    buf_puts(out, "    fprintf(stderr, \"panic: %s\\n\", msg ? msg : \"(no message)\");\n");
    buf_puts(out, "    abort();\n");
    buf_puts(out, "}\n\n");

    /* Phase 19: Effect handler chain */
    buf_puts(out, "/* Phase 19: Algebraic effect handler chain */\n");
    /* Phase P19-5: TurContK — a lightweight per-invocation continuation token.
     * Each tur_effect_perform call allocates one on the stack and passes its
     * address as `k` to the handler function.  `consumed` is set by (resume k v)
     * so that (cont? k) can verify freshness at runtime. */
    buf_puts(out, "typedef struct { bool consumed; void *origin_fiber; } TurContK;\n\n");
    buf_puts(out, "typedef struct EffectHandlerCase EffectHandlerCase;\n");
    buf_puts(out, "struct EffectHandlerCase {\n");
    buf_puts(out, "    const char *effect_name;\n");
    buf_puts(out, "    int64_t (*handler_fn)(int64_t *args, int n_args, int64_t k, void *env);\n");
    buf_puts(out, "    void *env;\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "typedef struct EffectHandlerFrame EffectHandlerFrame;\n");
    buf_puts(out, "struct EffectHandlerFrame {\n");
    buf_puts(out, "    struct EffectHandlerFrame *parent;\n");
    buf_puts(out, "    int n_cases;\n");
    buf_puts(out, "    EffectHandlerCase cases[8];\n");
    buf_puts(out, "};\n\n");
        /* Phase T21-A/B: FiberBlock — cooperative fiber runtime via ucontext_t.
     * tur_current_fiber is thread-local; set/restored by tur_fiber_block_resume.
     * tur_effect_perform checks tur_current_fiber->handler_chain for fiber-local
     * effect handler chains (Phase T21-B). */
    buf_puts(out, "/* Phase T21: FiberBlock */\n");
    buf_puts(out, "#ifdef __clang__\n");
    buf_puts(out, "#pragma clang diagnostic push\n");
    buf_puts(out, "#pragma clang diagnostic ignored \"-Wdeprecated-declarations\"\n");
    buf_puts(out, "#endif\n");
    buf_puts(out, "typedef struct FiberBlock FiberBlock;\n");
    buf_puts(out, "struct FiberBlock {\n");
    buf_puts(out, "    ucontext_t ctx;\n");
    buf_puts(out, "    ucontext_t caller_ctx;\n");
    buf_puts(out, "    char *stack;\n");
    buf_puts(out, "    size_t stack_size;\n");
    buf_puts(out, "    int done;\n");
    buf_puts(out, "    int64_t result;\n");
    buf_puts(out, "    int64_t arg;\n");
    buf_puts(out, "    void *handler_chain;\n");
    buf_puts(out, "    void (*entry_fn)(void);\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "static __thread FiberBlock *tur_current_fiber = NULL;\n\n");
    buf_puts(out, "static void tur_fiber_shim(uint32_t hi, uint32_t lo) {\n");
    buf_puts(out, "    FiberBlock *f = (FiberBlock *)(((uintptr_t)hi << 32) | (uintptr_t)(uint32_t)lo);\n");
    buf_puts(out, "    f->entry_fn();\n");
    buf_puts(out, "    f->done = 1;\n");
    buf_puts(out, "    swapcontext(&f->ctx, &f->caller_ctx);\n");
    buf_puts(out, "    abort();\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static FiberBlock *tur_fiber_block_new(void (*fn)(void), size_t stack_size) {\n");
    buf_puts(out, "    if (!stack_size) stack_size = 1024 * 1024;\n");
    buf_puts(out, "    FiberBlock *f = (FiberBlock *)calloc(1, sizeof(FiberBlock));\n");
    buf_puts(out, "    if (!f) { fprintf(stderr, \"fiber: oom\\n\"); abort(); }\n");
    buf_puts(out, "    f->stack = (char *)malloc(stack_size);\n");
    buf_puts(out, "    if (!f->stack) { free(f); abort(); }\n");
    buf_puts(out, "    f->stack_size = stack_size; f->entry_fn = fn; f->done = 0;\n");
    buf_puts(out, "    getcontext(&f->ctx);\n");
    buf_puts(out, "    f->ctx.uc_stack.ss_sp = f->stack;\n");
    buf_puts(out, "    f->ctx.uc_stack.ss_size = stack_size;\n");
    buf_puts(out, "    f->ctx.uc_link = NULL;\n");
    buf_puts(out, "    uintptr_t _fp = (uintptr_t)f;\n");
    buf_puts(out, "    uint32_t _hi = (uint32_t)(_fp >> 32);\n");
    buf_puts(out, "    uint32_t _lo = (uint32_t)(_fp & 0xFFFFFFFFU);\n");
    buf_puts(out, "    makecontext(&f->ctx, (void(*)(void))tur_fiber_shim, 2, _hi, _lo);\n");
    buf_puts(out, "    return f;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static int64_t tur_fiber_block_resume(FiberBlock *f, int64_t arg) {\n");
    buf_puts(out, "    if (!f || f->done) return f ? f->result : 0;\n");
    buf_puts(out, "    FiberBlock *_prev = tur_current_fiber;\n");
    buf_puts(out, "    tur_current_fiber = f;\n");
    buf_puts(out, "    f->arg = arg;\n");
    buf_puts(out, "    swapcontext(&f->caller_ctx, &f->ctx);\n");
    buf_puts(out, "    tur_current_fiber = _prev;\n");
    buf_puts(out, "    return f->result;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_fiber_block_yield(int64_t value) {\n");
    buf_puts(out, "    FiberBlock *f = tur_current_fiber;\n");
    buf_puts(out, "    if (!f) { fprintf(stderr, \"fiber-yield: not in fiber\\n\"); abort(); }\n");
    buf_puts(out, "    f->result = value;\n");
    buf_puts(out, "    swapcontext(&f->ctx, &f->caller_ctx);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_fiber_block_free(FiberBlock *f) {\n");
    buf_puts(out, "    if (!f) return; free(f->stack); free(f);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "#ifdef __clang__\n");
    buf_puts(out, "#pragma clang diagnostic pop\n");
    buf_puts(out, "#endif\n\n");
    buf_puts(out, "static __thread EffectHandlerFrame *global_effect_handler_chain = NULL;\n\n");
    buf_puts(out, "static int64_t tur_effect_perform(const char *name, int64_t *args, int n_args) {\n");
        /* T21-B: use fiber-local handler chain when inside a fiber */
    buf_puts(out, "    EffectHandlerFrame *frame =\n");
    buf_puts(out, "        (tur_current_fiber && tur_current_fiber->handler_chain)\n");
    buf_puts(out, "        ? (EffectHandlerFrame *)tur_current_fiber->handler_chain\n");
    buf_puts(out, "        : global_effect_handler_chain;\n");
    buf_puts(out, "    while (frame) {\n");
    buf_puts(out, "        for (int __i = 0; __i < frame->n_cases; __i++) {\n");
    buf_puts(out, "            if (strcmp(frame->cases[__i].effect_name, name) == 0) {\n");
    /* Phase P19-5: allocate a fresh TurContK on the stack per invocation */
    buf_puts(out, "                TurContK __fresh_k = {false, tur_current_fiber};\n");
    buf_puts(out, "                return frame->cases[__i].handler_fn(args, n_args, (int64_t)(intptr_t)&__fresh_k, frame->cases[__i].env);\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        frame = frame->parent;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    fprintf(stderr, \"Unhandled effect: %s\\n\", name);\n");
    buf_puts(out, "    abort();\n");
    buf_puts(out, "    return 0;\n");
    buf_puts(out, "}\n\n");

    /* Phase 9: Emit rc.h inline for rc<T> + weak<T> support */
    /* Phase 10: GC color enum and runtime (needed by rc_cb_alloc) */
    buf_puts(out, "/* rc<T> + weak<T> reference counting - Phase 9 */\n");
    buf_puts(out, "/* Phase 10: GC color enum for Bacon-Rajan */\n");
    buf_puts(out, "typedef enum { GC_WHITE, GC_GREY, GC_BLACK, GC_PURPLE } GcColor;\n\n");
    buf_puts(out, "typedef void (*RcDropFn)(void *value);\n\n");
    buf_puts(out, "typedef struct RcControlBlock RcControlBlock;\n\n");
    buf_puts(out, "struct RcControlBlock {\n");
    buf_puts(out, "    uint64_t strong_count;\n");
    buf_puts(out, "    uint64_t weak_count;\n");
    buf_puts(out, "    void *value;\n");
    buf_puts(out, "    RcDropFn drop_fn;\n");
    buf_puts(out, "    uint8_t value_type_kind;\n");
    /* Phase 10: Bacon-Rajan GC fields */
    buf_puts(out, "    uint8_t color;           /* GC color */\n");
    buf_puts(out, "    bool may_contain_cycles;  /* Hint for GC */\n");
    buf_puts(out, "    uint8_t reserved[6];\n");
    buf_puts(out, "};\n\n");
    /* Phase 9: Deferred free queue to avoid deep recursion in rc_strong_decrement */
    buf_puts(out, "#define RC_FREE_QUEUE_CAPACITY 65536\n");
    buf_puts(out, "static RcControlBlock *rc_free_queue[RC_FREE_QUEUE_CAPACITY];\n");
    buf_puts(out, "static uint32_t rc_free_queue_count = 0;\n");
    buf_puts(out, "static uint32_t rc_free_queue_drain(void);  /* Forward decl */\n");
    buf_puts(out, "static void rc_free_queue_push(RcControlBlock *cb);  /* Forward decl */\n");
    buf_puts(out, "bool rc_strong_decrement(RcControlBlock *cb);  /* Forward decl */\n");
    buf_puts(out, "bool rc_weak_decrement(RcControlBlock *cb);    /* Forward decl */\n");
    buf_puts(out, "static void rc_free_queue_push(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return;\n");
    buf_puts(out, "    if (rc_free_queue_count >= RC_FREE_QUEUE_CAPACITY) {\n");
    buf_puts(out, "        fprintf(stderr, \"rc_free_queue: full, aborting\\n\");\n");
    buf_puts(out, "        abort();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    rc_free_queue[rc_free_queue_count++] = cb;\n");
    buf_puts(out, "}\n\n");
    /* Phase 10: GC globals and helper functions (needed before rc_cb_alloc) */
    buf_puts(out, "#define GC_GLOBAL_REGISTRY_CAPACITY 4096\n");
    buf_puts(out, "static RcControlBlock *gc_all_blocks[GC_GLOBAL_REGISTRY_CAPACITY];\n");
    buf_puts(out, "static uint32_t gc_all_blocks_count = 0;\n\n");
    /* GC state must be declared early because gc_on_strong_decrement (called from
     * rc_strong_decrement) needs it.  The collector functions themselves are
     * defined later, after all RC helpers. */
    buf_puts(out, "#define GC_SUSPECT_THRESHOLD 128\n");
    buf_puts(out, "#define GC_MAX_SUSPECTS 4096\n\n");
    buf_puts(out, "typedef enum { GC_DISABLED, GC_MANUAL, GC_THRESHOLD } GcMode;\n\n");
    buf_puts(out, "static RcControlBlock *gc_suspect_roots[GC_MAX_SUSPECTS];\n");
    buf_puts(out, "static uint32_t gc_suspect_count = 0;\n");
    buf_puts(out, "static RcControlBlock *gc_grey_queue[GC_MAX_SUSPECTS];\n");
    buf_puts(out, "static uint32_t gc_grey_count = 0;\n");
    buf_puts(out, "static GcMode gc_mode = GC_DISABLED;\n");
    buf_puts(out, "static bool gc_enabled = false;\n\n");
    buf_puts(out, "static void gc_collect(void);  /* Forward decl */\n\n");
    buf_puts(out, "static void gc_set_color(RcControlBlock *cb, GcColor color) {\n");
    buf_puts(out, "    if (cb) cb->color = color;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static GcColor gc_get_color(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (cb) return cb->color;\n");
    buf_puts(out, "    return GC_WHITE;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_register_block(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb || gc_all_blocks_count >= GC_GLOBAL_REGISTRY_CAPACITY) return;\n");
    buf_puts(out, "    gc_all_blocks[gc_all_blocks_count++] = cb;\n");
    buf_puts(out, "    cb->color = GC_WHITE;\n");
    buf_puts(out, "    cb->may_contain_cycles = true;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_unregister_block(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < gc_all_blocks_count; i++) {\n");
    buf_puts(out, "        if (gc_all_blocks[i] == cb) {\n");
    buf_puts(out, "            gc_all_blocks[i] = gc_all_blocks[gc_all_blocks_count - 1];\n");
    buf_puts(out, "            gc_all_blocks_count--;\n");
    buf_puts(out, "            break;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    /* Phase 10: Suspect buffer management (before rc_strong_decrement) */
    buf_puts(out, "static void gc_add_suspect(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb || !gc_enabled || gc_mode == GC_DISABLED) return;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < gc_suspect_count; i++) {\n");
    buf_puts(out, "        if (gc_suspect_roots[i] == cb) return;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    if (gc_suspect_count >= GC_MAX_SUSPECTS) return;\n");
    buf_puts(out, "    gc_suspect_roots[gc_suspect_count++] = cb;\n");
    buf_puts(out, "    cb->color = GC_PURPLE;\n");
    buf_puts(out, "    /* Threshold mode: auto-collect when buffer is full */\n");
    buf_puts(out, "    if (gc_mode == GC_THRESHOLD && gc_suspect_count >= GC_SUSPECT_THRESHOLD) {\n");
    buf_puts(out, "        gc_collect();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_remove_suspect(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return;\n");
    buf_puts(out, "    for (uint32_t i = 0; i < gc_suspect_count; i++) {\n");
    buf_puts(out, "        if (gc_suspect_roots[i] == cb) {\n");
    buf_puts(out, "            gc_suspect_roots[i] = gc_suspect_roots[gc_suspect_count - 1];\n");
    buf_puts(out, "            gc_suspect_count--;\n");
    buf_puts(out, "            return;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_on_strong_decrement(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return;\n");
    buf_puts(out, "    /* Zombie: strong reached 0 but weak refs still exist */\n");
    buf_puts(out, "    if (cb->weak_count > 0) {\n");
    buf_puts(out, "        gc_add_suspect(cb);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "\n/* Phase 9: Deferred free queue drain */\n");
    buf_puts(out, "static uint32_t rc_free_queue_drain(void) {\n");
    buf_puts(out, "    if (rc_free_queue_count == 0) return 0;\n");
    buf_puts(out, "    uint32_t freed = 0;\n");
    buf_puts(out, "    while (rc_free_queue_count > 0) {\n");
    buf_puts(out, "        RcControlBlock *cb = rc_free_queue[0];\n");
    buf_puts(out, "        memmove(rc_free_queue, rc_free_queue + 1,\n");
    buf_puts(out, "                (rc_free_queue_count - 1) * sizeof(RcControlBlock *));\n");
    buf_puts(out, "        rc_free_queue_count--;\n");
    buf_puts(out, "        gc_unregister_block(cb);\n");
    buf_puts(out, "        if (cb->value) cb->drop_fn(cb->value);\n");
    buf_puts(out, "        free(cb);\n");
    buf_puts(out, "        freed++;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return freed;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void default_rc_drop_fn(void *value) {\n");
    buf_puts(out, "    free(value);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void drop_ref_payload(void *value) {\n");
    buf_puts(out, "    if (!value) return;\n");
    buf_puts(out, "    void *inner = *((void **)value);\n");
    buf_puts(out, "    if (inner) free(inner);\n");
    buf_puts(out, "    free(value);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void drop_rc_payload(void *value) {\n");
    buf_puts(out, "    if (!value) return;\n");
    buf_puts(out, "    RcControlBlock *inner = *((RcControlBlock **)value);\n");
    buf_puts(out, "    if (inner) { rc_strong_decrement(inner); rc_free_queue_drain(); }\n");
    buf_puts(out, "    free(value);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void drop_weak_payload(void *value) {\n");
    buf_puts(out, "    if (!value) return;\n");
    buf_puts(out, "    RcControlBlock *inner = *((RcControlBlock **)value);\n");
    buf_puts(out, "    if (inner) rc_weak_decrement(inner);\n");
    buf_puts(out, "    free(value);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static RcDropFn default_drop_fn_for_type(int value_type_kind) {\n");
    buf_puts(out, "    switch (value_type_kind) {\n");
    buf_puts(out, "        case 8: return drop_ref_payload;   /* TY_REF */\n");
    buf_puts(out, "        case 9: return drop_rc_payload;    /* TY_RC */\n");
    buf_puts(out, "        case 10: return drop_weak_payload; /* TY_WEAK */\n");
    buf_puts(out, "        default: return default_rc_drop_fn;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "RcControlBlock *rc_cb_alloc(size_t value_size, int value_type_kind, RcDropFn drop_fn) {\n");
    buf_puts(out, "    size_t total_size = sizeof(RcControlBlock) + value_size;\n");
    buf_puts(out, "    RcControlBlock *cb = (RcControlBlock *)malloc(total_size);\n");
    buf_puts(out, "    if (!cb) { fprintf(stderr, \"rc: out of memory\\n\"); abort(); }\n");
    buf_puts(out, "    cb->strong_count = 1;\n");
    buf_puts(out, "    cb->weak_count = 0;\n");
    buf_puts(out, "    cb->value = (void *)(cb + 1);\n");
    buf_puts(out, "    cb->drop_fn = drop_fn ? drop_fn : default_drop_fn_for_type(value_type_kind);\n");
    buf_puts(out, "    cb->value_type_kind = value_type_kind;\n");
    buf_puts(out, "    memset(cb->reserved, 0, sizeof(cb->reserved));\n");
    buf_puts(out, "    /* Register with GC; primitives (type_kind<=7) cannot form cycles */\n");
    buf_puts(out, "    gc_register_block(cb);\n");
    buf_puts(out, "    if (value_type_kind <= 7) cb->may_contain_cycles = false;\n");
    buf_puts(out, "    return cb;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "uint64_t rc_strong_increment(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return 0;\n");
    buf_puts(out, "    return ++cb->strong_count;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "bool rc_strong_decrement(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return false;\n");
    buf_puts(out, "    cb->strong_count--;\n");
    buf_puts(out, "    if (cb->strong_count == 0) {\n");
    buf_puts(out, "        if (cb->weak_count > 0) {\n");
    buf_puts(out, "            gc_on_strong_decrement(cb);\n");
    buf_puts(out, "            return false;\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "            rc_free_queue_push(cb);\n");
    buf_puts(out, "            return true;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return false;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "uint64_t rc_weak_increment(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return 0;\n");
    buf_puts(out, "    return ++cb->weak_count;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "bool rc_weak_decrement(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return false;\n");
    buf_puts(out, "    cb->weak_count--;\n");
    buf_puts(out, "    if (cb->weak_count == 0 && cb->strong_count == 0) {\n");
    buf_puts(out, "        gc_unregister_block(cb);\n");
    buf_puts(out, "        /* Free zombie value if GC did not already collect it */\n");
    buf_puts(out, "        if (cb->value && cb->drop_fn) {\n");
    buf_puts(out, "            cb->drop_fn(cb->value);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        free(cb);\n");
    buf_puts(out, "        return true;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return false;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "uint64_t rc_strong_count(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return 0;\n");
    buf_puts(out, "    return cb->strong_count;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "uint64_t rc_weak_count(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return 0;\n");
    buf_puts(out, "    return cb->weak_count;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "bool rc_is_alive(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return false;\n");
    buf_puts(out, "    return cb->strong_count > 0;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "RcControlBlock *rc_upgrade(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return NULL;\n");
    buf_puts(out, "    if (cb->strong_count > 0) {\n");
    buf_puts(out, "        rc_strong_increment(cb);\n");
    buf_puts(out, "        return cb;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return NULL;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "void *rc_get_value(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return NULL;\n");
    buf_puts(out, "    return cb->value;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "RcControlBlock *tur_rc_from_ref(void *ref_value, int value_type_kind) {\n");
    buf_puts(out, "    if (!ref_value) return NULL;\n");
    buf_puts(out, "    RcControlBlock *cb = (RcControlBlock *)malloc(sizeof(RcControlBlock));\n");
    buf_puts(out, "    if (!cb) { fprintf(stderr, \"rc/from-ref: out of memory\\n\"); abort(); }\n");
    buf_puts(out, "    cb->strong_count = 1;\n");
    buf_puts(out, "    cb->weak_count = 0;\n");
    buf_puts(out, "    cb->value = ref_value;\n");
    buf_puts(out, "    cb->drop_fn = default_drop_fn_for_type(value_type_kind);\n");
    buf_puts(out, "    cb->value_type_kind = (uint8_t)value_type_kind;\n");
    buf_puts(out, "    cb->color = GC_WHITE;\n");
    buf_puts(out, "    cb->may_contain_cycles = true;\n");
    buf_puts(out, "    memset(cb->reserved, 0, sizeof(cb->reserved));\n");
    buf_puts(out, "    gc_register_block(cb);\n");
    buf_puts(out, "    return cb;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "void *tur_ref_from_rc(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return NULL;\n");
    buf_puts(out, "    if (cb->strong_count != 1 || cb->weak_count != 0) {\n");
    buf_puts(out, "        fprintf(stderr, \"ref/from-rc requires unique rc (strong_count==1 and weak_count==0), got strong=%llu weak=%llu\\n\",\n");
    buf_puts(out, "                (unsigned long long)cb->strong_count, (unsigned long long)cb->weak_count);\n");
    buf_puts(out, "        abort();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    void *value = cb->value;\n");
    buf_puts(out, "    cb->value = NULL;\n");
    buf_puts(out, "    gc_unregister_block(cb);\n");
    buf_puts(out, "    free(cb);\n");
    buf_puts(out, "    return value;\n");
    buf_puts(out, "}\n\n");
    
    /* Phase 10: Emit remaining GC runtime — mark + trial deletion phases */
    buf_puts(out, "/* gc (Bacon-Rajan cycle collector - trial deletion) - Phase 10 */\n");
    buf_puts(out, "static void gc_mark_phase(void) {\n");
    buf_puts(out, "    for (uint32_t i = 0; i < gc_all_blocks_count; i++) {\n");
    buf_puts(out, "        gc_all_blocks[i]->color = GC_WHITE;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    for (uint32_t i = 0; i < gc_all_blocks_count; i++) {\n");
    buf_puts(out, "        RcControlBlock *cb = gc_all_blocks[i];\n");
    buf_puts(out, "        if (cb->strong_count > 0) {\n");
    buf_puts(out, "            cb->color = GC_BLACK;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    /* Trial deletion: free zombie suspects not reachable from strong roots */
    buf_puts(out, "static void gc_trial_deletion_phase(void) {\n");
    buf_puts(out, "    uint32_t i = 0;\n");
    buf_puts(out, "    while (i < gc_suspect_count) {\n");
    buf_puts(out, "        RcControlBlock *cb = gc_suspect_roots[i];\n");
    buf_puts(out, "        /* If revived or reachable from strong roots, keep */\n");
    buf_puts(out, "        if (cb->strong_count > 0 || cb->color == GC_BLACK) {\n");
    buf_puts(out, "            cb->color = GC_WHITE;\n");
    buf_puts(out, "            i++;\n");
    buf_puts(out, "            continue;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        /* WHITE and strong_count == 0: zombie — free value, keep cb for weak refs */\n");
    buf_puts(out, "        if (cb->value && cb->drop_fn) {\n");
    buf_puts(out, "            cb->drop_fn(cb->value);\n");
    buf_puts(out, "            cb->value = NULL;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        /* Unregister from global registry (cb stays alive for weak refs) */\n");
    buf_puts(out, "        gc_unregister_block(cb);\n");
    buf_puts(out, "        /* Remove from suspect buffer without advancing i */\n");
    buf_puts(out, "        gc_suspect_roots[i] = gc_suspect_roots[gc_suspect_count - 1];\n");
    buf_puts(out, "        gc_suspect_count--;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_collect(void) {\n");
    buf_puts(out, "    if (!gc_enabled || gc_mode == GC_DISABLED) return;\n");
    buf_puts(out, "    gc_grey_count = 0;\n");
    buf_puts(out, "    gc_mark_phase();\n");
    buf_puts(out, "    gc_trial_deletion_phase();\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_force(void) {\n");
    buf_puts(out, "    gc_collect();\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_enable(void) {\n");
    buf_puts(out, "    gc_enabled = true;\n");
    buf_puts(out, "    /* Default to manual mode when enabled */\n");
    buf_puts(out, "    if (gc_mode == GC_DISABLED) gc_mode = GC_MANUAL;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_disable(void) {\n");
    buf_puts(out, "    gc_enabled = false;\n");
    buf_puts(out, "    gc_mode = GC_DISABLED;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_set_mode(GcMode mode) {\n");
    buf_puts(out, "    gc_mode = mode;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static bool gc_is_alive(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return false;\n");
    buf_puts(out, "    if (cb->strong_count > 0) return true;\n");
    buf_puts(out, "    return (cb->color == GC_BLACK || cb->color == GC_GREY);\n");
    buf_puts(out, "}\n\n");
    
    /* Phase 4 v1: Emit all defer thunks at file scope (after tur_frame defs) */
    emit_pending_defer_thunks(&ctx, out);

    /* Final assembly order (ensures correct C visibility):
     *  1. early_file  - struct typedefs + drop glue (visible to everything)
     *  2. extern_decls - user extern-c declarations
     *  3. fwd_decls   - Turmeric function forward declarations (visible to handlers)
     *  4. pending_handler_fns - effect handler functions (can call Turmeric fns)
     *  5. file        - Turmeric function definitions (can reference handler fns by name)
     *  6. main()      - entry point body
     */
    if (early_file.len)  { buf_write(out, early_file.data, early_file.len); buf_putc(out, '\n'); }
    if (extern_decls.len){ buf_write(out, extern_decls.data, extern_decls.len); buf_putc(out, '\n'); }
    if (fwd_decls.len)   { buf_write(out, fwd_decls.data, fwd_decls.len); buf_putc(out, '\n'); }
    buf_free(&early_file);
    buf_free(&extern_decls);
    buf_free(&fwd_decls);

    /* Phase 19: Effect handler functions (after fwd_decls so they can call
     * user-defined Turmeric functions, before file so fn defs can reference them). */
    if (ctx.pending_handler_fns && ctx.pending_handler_fns->len > 0) {
        buf_write(out, ctx.pending_handler_fns->data, ctx.pending_handler_fns->len);
        buf_free(ctx.pending_handler_fns);
        buf_init(ctx.pending_handler_fns);
    }

    if (file.len) { buf_write(out, file.data, file.len); buf_putc(out, '\n'); }

    if (!user_has_main) {
        /* Only generate main() if user didn't define one */
        buf_puts(out, "int main(void) {\n");
        if (body.len) buf_write(out, body.data, body.len);
        buf_puts(out, "    return 0;\n");
        buf_puts(out, "}\n");
    }

    buf_free(&file);
    buf_free(&body);
    return 0;
}

/* ------------ Phase 2: Multi-file support ------------ */

/* Sanitize a module name for use in C header guards. */
static void sanitize_module_name(char *out, const char *name, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; name[i] && k < cap - 1; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '/') {
            out[k++] = (c == '-' || c == '/') ? '_' : c;
        }
    }
    out[k] = '\0';
}

/* Emit a C header file for a module. Contains declarations (not definitions). */
int emit_header(Buf *out, const char *module_name, const Expr *program) {
    if (!program || program->kind != EX_PROGRAM) {
        fprintf(stderr, "tur: emit_header: expected EX_PROGRAM\n");
        return -1;
    }

    char guard[256];
    sanitize_module_name(guard, module_name, sizeof(guard));

    /* Header guard */
    buf_printf(out, "/* generated by tur (phase 2) */\n");
    buf_printf(out, "#ifndef TUR_%s_H\n", guard);
    buf_printf(out, "#define TUR_%s_H\n\n", guard);

    /* Standard includes */
    buf_puts(out, "#include <stdint.h>\n");
    buf_puts(out, "#include <stdbool.h>\n");
    buf_puts(out, "#include <stdio.h>\n");
    buf_puts(out, "#include <stdlib.h>\n");
    buf_puts(out, "#include <string.h>\n\n");

    /* Forward declarations for functions */
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *e = program->as.program.items[i];
        if (e->kind == EX_FN_DEF) {
            FnDef *fd = e->as.fn_def_.fn;
            const char *fn_name = raw_name_for_binding(fd->binding);
            bool is_main = (strcmp(fn_name, "main") == 0);

            if (is_main) continue; /* main is not exported */

            /* Emit function declaration */
            if (e->type.kind == TY_FN) {
                TypeKind result = e->type.as.fn.result_kind;
                buf_puts(out, type_c_name(type_from_kind(result)));
            } else {
                buf_puts(out, "void");
            }
            buf_printf(out, " %s(", fn_name);
            for (uint8_t j = 0; j < fd->n_params; j++) {
                if (j > 0) buf_puts(out, ", ");
                buf_puts(out, type_c_name(fd->param_types[j]));
            }
            buf_puts(out, ");\n");
            free((void*)fn_name);
        } else if (e->kind == EX_EXTERN_C) {
            ExternC *ec = e->as.extern_c_.ext;
            /* Emit extern-c declaration in header */
            buf_printf(out, "extern %s %s(",
                       type_c_name(ec->return_type),
                       ec->c_name->name);
            for (uint8_t j = 0; j < ec->n_params; j++) {
                if (j > 0) buf_puts(out, ", ");
                buf_puts(out, type_c_name(ec->param_types[j]));
            }
            buf_puts(out, ");\n");
        }
    }

    if (out->len > 0 && out->data[out->len - 1] != '\n') {
        buf_putc(out, '\n');
    }

    /* Header guard end */
    buf_printf(out, "#endif /* TUR_%s_H */\n", guard);
    return 0;
}

/* Emit a C implementation file for a module. Contains definitions. */
int emit_implementation(Buf *out, const char *module_name, const Expr *program) {
    if (!program || program->kind != EX_PROGRAM) {
        fprintf(stderr, "tur: emit_implementation: expected EX_PROGRAM\n");
        return -1;
    }

    Buf file; buf_init(&file);
    Buf body; buf_init(&body);

    EmitCtx ctx;
    ctx.file = &file;
    ctx.main_ = &body;
    ctx.indent = 4;
    ctx.tmp_n = 0;
    ctx.fn_params = NULL;
    ctx.n_fn_params = 0;
    /* Phase 3: closure tracking */
    ctx.closure = NULL;
    ctx.env_var_name = NULL;
    /* Phase 3: env struct tracking */
    ctx.env_struct_names = NULL;
    ctx.n_env_struct_names = 0;
    ctx.cap_env_struct_names = 0;
    /* Phase 3/4: Track return emission */
    ctx.return_emitted = false;
    /* Phase 19: Pending effect handler function buffer */
    Buf pending_hfns2; buf_init(&pending_hfns2);
    ctx.pending_handler_fns = &pending_hfns2;
    char guard[256];
    sanitize_module_name(guard, module_name, sizeof(guard));

    /* Include the corresponding header */
    buf_printf(out, "/* generated by tur (phase 2) */\n");
    buf_printf(out, "#include \"%s.h\"\n\n", guard);

    /* Check if user defined a main function */
    bool user_has_main = false;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *e = program->as.program.items[i];
        if (e->kind == EX_FN_DEF) {
            FnDef *fd = e->as.fn_def_.fn;
            if (strcmp(fd->binding->name->name, "main") == 0) {
                user_has_main = true;
                break;
            }
        }
    }

    /* Pass 1: emit all top-level definitions */
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *e = program->as.program.items[i];
        if (e->kind == EX_DEF) {
            char *bn = name_for_binding(&ctx, e->as.def_.binding);
            buf_printf(&file, "static %s %s;\n",
                       type_c_name(e->as.def_.binding->type), bn);
            if (e->as.def_.init) {
                char *iv = emit_value(&ctx, &body, e->as.def_.init);
                indent_buf(&body, ctx.indent);
                buf_printf(&body, "%s = %s;\n", bn, iv);
                free(iv);
            }
            free(bn);
        } else if (e->kind == EX_FN_DEF) {
            /* Emit function definition */
            emit_fn_def(&ctx, &file, e);
        } else if (e->kind == EX_EXTERN_C) {
            /* In implementation, just emit the extern declaration reference */
            ExternC *ec = e->as.extern_c_.ext;
            buf_printf(&file, "extern %s %s(",
                       type_c_name(ec->return_type),
                       ec->c_name->name);
            for (uint8_t j = 0; j < ec->n_params; j++) {
                if (j > 0) buf_puts(&file, ", ");
                buf_puts(&file, type_c_name(ec->param_types[j]));
            }
            buf_puts(&file, ");\n");
        } else {
            emit_stmt(&ctx, &body, e);
        }
    }

    /* Assemble: includes + file-scope decls + body (initializers) */
    if (file.len) { buf_write(out, file.data, file.len); buf_putc(out, '\n'); }
    if (!user_has_main) {
        /* Only generate main() if user didn't define one */
        buf_puts(out, "int main(void) {\n");
        if (body.len) buf_write(out, body.data, body.len);
        buf_puts(out, "    return 0;\n");
        buf_puts(out, "}\n");
    }

    buf_free(&file);
    buf_free(&body);
    return 0;
}
