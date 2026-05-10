#include "emit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtins.h"
#include "rc.h"
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
    thunk->env_name = env_name ? strdup(env_name) : NULL;
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

static char *atom_nil(void)         { return strdup("((void)0)"); }
static char *atom_bool(bool b)      { return strdup(b ? "true" : "false"); }
static char *atom_int(int64_t i) {
    char buf[40];
    snprintf(buf, sizeof buf, "INT64_C(%lld)", (long long)i);
    return strdup(buf);
}
static char *atom_var(EmitCtx *ctx, const Binding *b) {
    return name_for_binding(ctx, b);
}
static char *atom_cstr(StrSlice s) {
    /* Build into a Buf, then strdup out. */
    Buf b; buf_init(&b);
    emit_c_string(&b, s);
    buf_putc(&b, '\0');
    char *p = strdup(b.data);
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
        spec->shape == BS_PRINTLN_BOOL ||
        spec->shape == BS_PRINTLN_CSTR) {
        char *arg = emit_value(ctx, body, args[0]);
        indent_buf(body, ctx->indent);
        switch (spec->shape) {
            case BS_PRINTLN_INT:
                buf_printf(body, "printf(\"%%lld\\n\", (long long)(%s));\n", arg);
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
    char *result = strdup(out.data);
    buf_free(&out);
    for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
    free(arg_strs);
    return result;
}

/* ------------ block-shaped emitters ------------ */

static char *emit_let_value(EmitCtx *ctx, Buf *body, const Expr *e) {
    char *tmp = NULL;
    bool nil_result = (e->type.kind == TY_NIL);
    if (!nil_result) {
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
    char *tmp = NULL;
    bool nil_result = (e->type.kind == TY_NIL);
    if (!nil_result) {
        tmp = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s;\n", type_c_name(e->type), tmp);
    }
    char *cond = emit_value(ctx, body, e->as.if_.cond);
    indent_buf(body, ctx->indent);
    buf_printf(body, "if (%s) {\n", cond);
    free(cond);
    ctx->indent += 4;
    if (nil_result) {
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
        if (nil_result) emit_stmt(ctx, body, e->as.if_.else_or_null);
        else {
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

    /* Check if we have any defers in this scope */
    bool has_defers = false;
    for (uint32_t i = 0; i < n; i++) {
        if (e->as.do_.items[i]->kind == EX_DEFER) {
            has_defers = true;
            break;
        }
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
            if (last->type.kind == TY_NIL) {
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

/* ------------ entry points ------------ */

static char *emit_value(EmitCtx *ctx, Buf *body, const Expr *e) {
    switch (e->kind) {
        case EX_NIL_LIT:  return atom_nil();
        case EX_BOOL_LIT: return atom_bool(e->as.b);
        case EX_INT_LIT:  return atom_int(e->as.i);
        case EX_CSTR_LIT: return atom_cstr(e->as.s);
        case EX_VAR:      return atom_var(ctx, e->as.var.binding);
        case EX_LET:      return emit_let_value(ctx, body, e);
        case EX_IF:       return emit_if_value(ctx, body, e);
        case EX_DO:       return emit_do_value(ctx, body, e);
        case EX_BUILTIN:  return emit_builtin(ctx, body, e);
        case EX_WHILE:    emit_while_stmt(ctx, body, e); return atom_nil();
        case EX_SET:      emit_set_stmt(ctx, body, e);   return atom_nil();
        case EX_DEF:      /* handled at top level — shouldn't appear nested. */
        case EX_PROGRAM:
            return atom_nil();
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
                char *result = strdup(out.data);
                buf_free(&out);
                for (uint32_t i = 0; i <= e->as.call_.n_args; i++) free(arg_strs[i]);
                free(arg_strs);
                free(thunk_name);
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
            /* (@ expr) - dereference ref<T> or ptr<T> */
            char *inner = emit_value(ctx, body, e->as.deref_.expr);
            
            /* For ref<T>, we need to cast and dereference */
            if (e->as.deref_.expr->type.kind == TY_REF) {
                char *inner_type_c = strdup(type_c_name(e->type));
                char *tmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "%s %s = *((%s *)%s);\n", 
                           inner_type_c, tmp, inner_type_c, inner);
                free(inner);
                free(inner_type_c);
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
            char *inner_type_c = strdup(type_c_name(e->as.rc_of_.expr->type));
            
            /* Emit: rc_cb_alloc(sizeof(T), TY_T, NULL) */
            char *cb_tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            /* For Phase 9, we use a simplified approach: allocate struct with value */
            buf_printf(body, "RcControlBlock *%s = rc_cb_alloc(sizeof(%s), %d, NULL);\n",
                       cb_tmp, inner_type_c, e->as.rc_of_.expr->type.kind);
            indent_buf(body, ctx->indent);
            /* Copy value into control block */
            buf_printf(body, "*((%s *)((char *)%s + sizeof(RcControlBlock))) = %s;\n",
                       inner_type_c, cb_tmp, inner);
            free(inner);
            free(inner_type_c);
            return cb_tmp;
        }
        case EX_RC_CLONE: {
            /* (rc/clone r) - increment strong count, return same cb */
            char *inner = emit_value(ctx, body, e->as.rc_clone_.expr);
            indent_buf(body, ctx->indent);
            buf_printf(body, "rc_strong_increment(%s);\n", inner);
            /* Return the same pointer (now with incremented count) */
            return strdup(inner);
        }
        case EX_RC_DROP: {
            /* (rc/drop r) - decrement strong count */
            char *inner = emit_value(ctx, body, e->as.rc_drop_.expr);
            indent_buf(body, ctx->indent);
            buf_printf(body, "rc_strong_decrement(%s);\n", inner);
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
        /* Phase 12: Borrow traits */
        case EX_BORROW_IMMUT: {
            /* (& expr) - immutable borrow: emit as &expr */
            char *inner = emit_value(ctx, body, e->as.borrow_immut_.expr);
            /* For borrow, we return a pointer expression */
            char *result = (char *)malloc(strlen(inner) + 10);
            if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
            snprintf(result, strlen(inner) + 10, "&%s", inner);
            free(inner);
            return result;
        }
        case EX_BORROW_MUT: {
            /* (&mut expr) - mutable borrow: emit as &expr */
            char *inner = emit_value(ctx, body, e->as.borrow_mut_.expr);
            /* For mutable borrow, we return a pointer expression */
            char *result = (char *)malloc(strlen(inner) + 10);
            if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
            snprintf(result, strlen(inner) + 10, "&%s", inner);
            free(inner);
            return result;
        }
    }
    return atom_nil();
}

static void emit_stmt(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* Run e for side effects, discard value. */
    switch (e->kind) {
        case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT:
        case EX_CSTR_LIT: case EX_VAR:
            /* No side effects — emit nothing. */
            return;
        case EX_WHILE: emit_while_stmt(ctx, body, e); return;
        case EX_SET:   emit_set_stmt(ctx, body, e);   return;
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
            if (e->type.kind != TY_NIL) {
                indent_buf(body, ctx->indent);
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
        case EX_WEAK:
        case EX_WEAK_UPGRADE:
        case EX_WEAK_PRED: {
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
    }
}

/* ------------ Phase 2: function emission ------------ */

static void emit_fn_def(EmitCtx *ctx, Buf *file, const Expr *e) {
    FnDef *fd = e->as.fn_def_.fn;
    /* Use raw name (without ID suffix) for function name */
    const char *fn_name = raw_name_for_binding(fd->binding);

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
        if (is_main && result == TY_INT) {
            buf_puts(file, "int");  /* C main returns int, not int64_t */
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
            ctx->env_var_name = strdup(env_var_name_buf);
        }
    }

    /* Get the return type */
    TypeKind result_kind = e->type.kind == TY_FN ? e->type.as.fn.result_kind : TY_NIL;

    if (result_kind == TY_NIL) {
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
     * Pass 1: Emit forward declarations for all functions. */
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *e = program->as.program.items[i];
        if (e->kind == EX_FN_DEF) {
            FnDef *fd = e->as.fn_def_.fn;
            /* Skip main - it's not called from other functions in the same file */
            if (strcmp(fd->binding->name->name, "main") == 0) continue;
            /* Emit forward declaration with static */
            buf_puts(&file, "static ");
            if (e->type.kind == TY_FN) {
                TypeKind result = e->type.as.fn.result_kind;
                buf_puts(&file, type_c_name(type_from_kind(result)));
            } else {
                buf_puts(&file, "void");
            }
            const char *fn_name = raw_name_for_binding(fd->binding);
            buf_printf(&file, " %s(", fn_name);
            for (uint8_t j = 0; j < fd->n_params; j++) {
                if (j > 0) buf_puts(&file, ", ");
                buf_puts(&file, type_c_name(fd->param_types[j]));
            }
            buf_puts(&file, ");\n");
            free((void*)fn_name);
        }
    }

    /* Pass 2: collect all top-level defs and fn_defs. */
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *e = program->as.program.items[i];
        if (e->kind == EX_DEF) {
            char *bn = name_for_binding(&ctx, e->as.def_.binding);
            buf_printf(&file, "static %s %s;\n",
                       type_c_name(e->as.def_.binding->type), bn);
            char *iv = emit_value(&ctx, &body, e->as.def_.init);
            indent_buf(&body, ctx.indent);
            buf_printf(&body, "%s = %s;\n", bn, iv);
            free(bn); free(iv);
        } else if (e->kind == EX_FN_DEF) {
            /* Emit function definition at file scope */
            emit_fn_def(&ctx, &file, e);
        } else if (e->kind == EX_EXTERN_C) {
            /* Emit extern-c declaration at file scope */
            ExternC *ec = e->as.extern_c_.ext;
            buf_printf(&file, "extern %s %s(",
                       type_c_name(ec->return_type),
                       ec->c_name->name);
            for (uint8_t j = 0; j < ec->n_params; j++) {
                if (j > 0) buf_puts(&file, ", ");
                buf_printf(&file, "%s", type_c_name(ec->param_types[j]));
            }
            buf_puts(&file, ");\n");
        } else {
            emit_stmt(&ctx, &body, e);
        }
    }

    /* Final assembly. */
    buf_puts(out, "/* generated by tur (phase 2) */\n");
    buf_puts(out, "#include <stdio.h>\n");
    buf_puts(out, "#include <stdint.h>\n");
    buf_puts(out, "#include <stdbool.h>\n");
    /* Phase 5: Declare malloc and free for ref<T> without including stdlib.h
     * to avoid conflicts with user-declared functions like exit. */
    buf_puts(out, "extern void *malloc(size_t);\n");
    buf_puts(out, "extern void free(void *);\n");
    /* Phase 9: Declare abort and memset for rc<T> support */
    buf_puts(out, "extern void abort(void);\n");
    buf_puts(out, "extern void *memset(void *, int, size_t);\n");
    buf_puts(out, "\n");
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
    /* Phase 10: GC globals and helper functions (needed before rc_cb_alloc) */
    buf_puts(out, "#define GC_GLOBAL_REGISTRY_CAPACITY 4096\n");
    buf_puts(out, "static RcControlBlock *gc_all_blocks[GC_GLOBAL_REGISTRY_CAPACITY];\n");
    buf_puts(out, "static uint32_t gc_all_blocks_count = 0;\n\n");
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
    buf_puts(out, "static void gc_on_strong_decrement(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (!cb) return;\n");
    buf_puts(out, "    if (cb->weak_count > 0) {\n");
    buf_puts(out, "        cb->color = GC_PURPLE;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void default_rc_drop_fn(void *value) {\n");
    buf_puts(out, "    free(value);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "RcControlBlock *rc_cb_alloc(size_t value_size, int value_type_kind, RcDropFn drop_fn) {\n");
    buf_puts(out, "    size_t total_size = sizeof(RcControlBlock) + value_size;\n");
    buf_puts(out, "    RcControlBlock *cb = (RcControlBlock *)malloc(total_size);\n");
    buf_puts(out, "    if (!cb) { fprintf(stderr, \"rc: out of memory\\n\"); abort(); }\n");
    buf_puts(out, "    cb->strong_count = 1;\n");
    buf_puts(out, "    cb->weak_count = 0;\n");
    buf_puts(out, "    cb->value = (void *)(cb + 1);\n");
    buf_puts(out, "    cb->drop_fn = drop_fn ? drop_fn : default_rc_drop_fn;\n");
    buf_puts(out, "    cb->value_type_kind = value_type_kind;\n");
    buf_puts(out, "    memset(cb->reserved, 0, sizeof(cb->reserved));\n");
    buf_puts(out, "    /* Register with GC */\n");
    buf_puts(out, "    gc_register_block(cb);\n");
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
    buf_puts(out, "            gc_unregister_block(cb);\n");
    buf_puts(out, "            if (cb->value) cb->drop_fn(cb->value);\n");
    buf_puts(out, "            free(cb);\n");
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
    
    /* Phase 10: Emit remaining gc runtime for cycle collection */
    buf_puts(out, "/* gc (Bacon-Rajan cycle collector) - Phase 10 */\n");
    buf_puts(out, "#define GC_SUSPECT_THRESHOLD 128\n");
    buf_puts(out, "#define GC_MAX_SUSPECTS 4096\n\n");
    buf_puts(out, "typedef enum { GC_DISABLED, GC_MANUAL, GC_THRESHOLD } GcMode;\n\n");
    buf_puts(out, "static RcControlBlock *gc_suspect_roots[GC_MAX_SUSPECTS];\n");
    buf_puts(out, "static uint32_t gc_suspect_count = 0;\n");
    buf_puts(out, "static RcControlBlock *gc_grey_queue[GC_MAX_SUSPECTS];\n");
    buf_puts(out, "static uint32_t gc_grey_count = 0;\n");
    buf_puts(out, "static GcMode gc_mode = GC_DISABLED;\n");
    buf_puts(out, "static bool gc_enabled = false;\n\n");
    buf_puts(out, "static void gc_set_color(RcControlBlock *cb, GcColor color) {\n");
    buf_puts(out, "    if (cb) cb->color = color;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static GcColor gc_get_color(RcControlBlock *cb) {\n");
    buf_puts(out, "    if (cb) return cb->color;\n");
    buf_puts(out, "    return GC_WHITE;\n");
    buf_puts(out, "}\n\n");
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
    buf_puts(out, "static void gc_collect(void) {\n");
    buf_puts(out, "    if (!gc_enabled || gc_mode == GC_DISABLED) return;\n");
    buf_puts(out, "    gc_grey_count = 0;\n");
    buf_puts(out, "    gc_mark_phase();\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_force(void) {\n");
    buf_puts(out, "    gc_collect();\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_enable(void) {\n");
    buf_puts(out, "    gc_enabled = true;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void gc_disable(void) {\n");
    buf_puts(out, "    gc_enabled = false;\n");
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
            char *iv = emit_value(&ctx, &body, e->as.def_.init);
            indent_buf(&body, ctx.indent);
            buf_printf(&body, "%s = %s;\n", bn, iv);
            free(bn); free(iv);
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
