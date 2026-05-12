#include "emit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtins.h"
#include "rc.h"
#include "rc_elision.h"
#include "types.h"

/* Phase R5: Global panic strategy flag (set by main.c --panic-abort) */
extern bool g_panic_abort;

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








/* Mangle a Turmeric struct field name to a valid C identifier.
 * Replaces hyphens and other non-id-safe chars with underscores. Caller frees. */
static char *mangle_field_name(const char *name) {
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
        /* Body contains return/throw - emit as statements only.
         * The temp variable was declared above but will remain uninitialized
         * if return/throw is taken (which is fine - unreachable code after return).
         * If the body doesn't take the return/throw path, it should assign to
         * the temp variable itself.
         */
        emit_stmt(ctx, body, e->as.let_.body);
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
            if (last->kind == EX_RETURN || last->kind == EX_THROW || last->kind == EX_PANIC || last->kind == EX_PANIC_WITH) {
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
            } else {
                emit_stmt(ctx, body, it);
                /* Check if this statement diverges (return/throw/panic) */
                if (it->kind == EX_RETURN || it->kind == EX_THROW || 
                    it->kind == EX_PANIC || it->kind == EX_PANIC_WITH) {
                    emitted_diverging = true;
                }
            }
        }
        
        /* Don't emit cleanup code - return/throw/panic already handles it */
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

/* Phase H §1: Compute the C name of a typeclass instance's dictionary singleton.
 * Mirrors the type_suffix logic in EX_INSTANCE_DEF (emit_stmt) so that the name
 * is consistent wherever it needs to be referenced (EX_DICT emit_value, etc.).
 * Writes "dict_<TypeClass>_<typeargs>" into buf (size buflen). */
static void emit_dict_name(char *buf, size_t buflen, const TypeClassInstance *inst) {
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
        /* Phase H §1: dictionary passing — load address of singleton as int64_t */
        case EX_DICT: {
            char dict_name[128];
            emit_dict_name(dict_name, sizeof(dict_name), e->as.dict_.instance);
            Buf out; buf_init(&out);
            buf_printf(&out, "(int64_t)(intptr_t)(&%s_singleton)", dict_name);
            buf_putc(&out, '\0');
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
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
            free(msg_val);
            return atom_nil();
        }
        case EX_PANIC_WITH: {
            /* (panic-with payload) - panic with typed payload */
            const Expr *payload = e->as.panic_with_.payload;
            char *payload_val = emit_value(ctx, body, payload);
            indent_buf(body, ctx->indent);
            /* Set the current frame for panic to fire defers */
            if (ctx->frame_var) {
                buf_printf(body, "tur_panic_set_frame(&%s);\n", ctx->frame_var);
            }
            /* tur_panic_with takes type_tag (int), payload (void*), file, line */
            /* For now, use a placeholder type_tag of 0 (TY_INT) */
            buf_printf(body, "tur_panic_with(0, (void*)%s, __FILE__, __LINE__);\n", payload_val);
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
            /* (cloneable-reset body) - like reset but with cloneable captures
             * For v1 without full cloneable CPS: just evaluate and return body.
             * Full implementation requires cloneable continuation runtime.
             */
            return emit_value(ctx, body, e->as.cloneable_reset_.body);
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
            /* (cloneable-shift k body) - cloneable shift with continuation handler k
             * For v1 without full cloneable CPS: similar to shift but with cloneable continuation.
             * Full implementation requires cloneable continuation runtime.
             */
            char *body_val = emit_value(ctx, body, e->as.cloneable_shift_.body);
            char *result = fresh_tmp(ctx);
            char *k_fn = emit_value(ctx, body, e->as.cloneable_shift_.k_fn);
            
            /* For now, emit as a regular function call (same as shift)
             * Full cloneable support requires runtime implementation */
            if (e->as.cloneable_shift_.k_fn->kind == EX_CLOSURE) {
                struct Closure *closure = e->as.cloneable_shift_.k_fn->as.closure_.closure;
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

            /* Phase 16 v2: indirect capability field call — fn_expr is EX_GET_FIELD.
             * Emit: ((ret_t (*)(arg_t, ...))(intptr_t)(struct_val).field_name)(args...)
             * Effect-row annotation is advisory (erased to a plain function pointer). */
            if (e->as.call_.fn_expr) {
                Expr *gf = e->as.call_.fn_expr;
                char *fn_ptr_val = emit_value(ctx, body, gf);
                const char *ret_c = type_c_name(e->type);

                char **arg_strs = (char **)malloc(e->as.call_.n_args * sizeof(char *));
                if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    arg_strs[i] = emit_value(ctx, body, e->as.call_.args[i]);
                }

                Buf out; buf_init(&out);
                /* Cast function pointer to the right signature, then call. */
                buf_printf(&out, "((%s (*)(", ret_c);
                if (e->as.call_.n_args == 0) {
                    buf_puts(&out, "void");
                } else {
                    for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                        if (i > 0) buf_puts(&out, ", ");
                        buf_puts(&out, type_c_name(e->as.call_.args[i]->type));
                    }
                }
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

                char *result = strdup(out.data);
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
                char *raw = emit_value(ctx, body, e->as.call_.args[i]);
                /* Phase HKT H3/H4: When a function-reference (TY_FN) is passed to an
                 * int64_t parameter, emit an explicit (int64_t)(intptr_t) cast so the
                 * generated C99 code is valid.  Function pointers cannot be implicitly
                 * converted to integers in C99; a reinterpret via intptr_t is needed. */
                /* Phase HKT §5: also cast TY_PTR_VOID (capturing-closure env ptr) to
                 * int64_t when passing to an int64_t parameter. */
                bool needs_fn_cast = (e->as.call_.args[i]->type.kind == TY_FN ||
                                      e->as.call_.args[i]->type.kind == TY_PTR_VOID);
                if (needs_fn_cast && fn_binding->type.kind == TY_FN) {
                    uint8_t n_fnparams = fn_binding->type.as.fn.arity;
                    uint8_t param_idx = (i < n_fnparams) ? i : (n_fnparams > 0 ? n_fnparams - 1 : 0);
                    TypeKind pk = fn_binding->type.as.fn.arg_kinds[param_idx];
                    /* Apply cast when param is int64_t in C: TY_INT, opaque TY_STRUCT
                     * (HKT container), or TY_PTR_VOID (fat-closure env ptr passed as
                     * void * to a polymorphic HKT helper). */
                    needs_fn_cast = (pk == TY_INT || pk == TY_STRUCT || pk == TY_PTR_VOID);
                }
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
        /* Phase T21-F: async/await */
        case EX_ASYNC: {
            /* (async fn-expr) — launch fn-expr (a no-arg fn) in a fiber, return TurFuture* as ptr<void> */
            char *fn_val = emit_value(ctx, body, e->as.async_.fn_expr);
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "void *%s = (void *)tur_async_fiber((int64_t(*)(void))(intptr_t)%s);\n", tmp, fn_val);
            free(fn_val);
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

                if (c->body && (c->body->type.kind == TY_NIL || c->body->type.kind == TY_NEVER)) {
                    /* Void-typed or never-typed body: emit as statement, then return 0 */
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

            /* Derive a chain pointer name parallel to the frame name */
            char *chain_var = (char *)malloc(32);
            snprintf(chain_var, 32, "__eff_chain_%d", ctx->tmp_n - 1);

            indent_buf(body, ctx->indent);
            buf_printf(body, "EffectHandlerFrame %s;\n", frame_var);
            /* T21-B: use fiber-local chain when inside a fiber */
            indent_buf(body, ctx->indent);
            buf_printf(body,
                "EffectHandlerFrame **%s = (tur_current_fiber"
                " ? (EffectHandlerFrame **)&tur_current_fiber->effect_handler_chain"
                " : &global_effect_handler_chain);\n", chain_var);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s.parent = *%s;\n", frame_var, chain_var);
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
            buf_printf(body, "*%s = &%s;\n", chain_var, frame_var);

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
            buf_printf(body, "*%s = (*%s)->parent;\n", chain_var, chain_var);

            /* Cleanup */
            for (uint8_t i = 0; i < h->n_cases; i++) free(hfn_names[i]);
            free(hfn_names);
            free(chain_var);
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
                /* Mark TurContK consumed for Phase 19 k (TY_INT with CK_MOVE).
                 * T21-E: also enforce cross-fiber resume check. */
                if (e->as.resume_.resume->k->type.kind == TY_INT) {
                    indent_buf(body, ctx->indent);
                    buf_printf(body,
                        "if (((TurContK *)(intptr_t)%s)->origin_fiber != "
                        "(void *)tur_current_fiber) { "
                        "fprintf(stderr, \"continuation error: resume on wrong "
                        "fiber\\n\"); abort(); }\n",
                        k_var);
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
    }
    return atom_nil();
}

static void emit_stmt(EmitCtx *ctx, Buf *body, const Expr *e) {
    /* Run e for side effects, discard value. */
    switch (e->kind) {
        case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT:
        case EX_FLOAT_LIT: case EX_CSTR_LIT: case EX_VAR:
        case EX_TYPECLASS_DEF:
        case EX_PANIC_PAYLOAD_TYPE:
        case EX_PANIC_PAYLOAD_VALUE:
        case EX_PANIC_PAYLOAD_FILE:
        case EX_PANIC_PAYLOAD_LINE:
        case EX_PANIC_PAYLOAD_DOWNS:
            /* No side effects — emit nothing. */
            return;
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
        /* These are handled in the main emit_expr switch above */
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
        /* Phase B2: Cloneable continuations */
        case EX_CLONEABLE_RESET:
        case EX_CLONEABLE_SHIFT:
            /* For now, emit a placeholder - full impl deferred */
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
                    ret_type = type_from_kind(
                        method_impl->binding->type.as.fn.result_kind);
                }
                const char *ret_c_name = type_c_name(ret_type);
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
        case EX_CONT_PRED:
        /* Phase T21-F: async/await as statement - emit and discard */
        case EX_ASYNC:
        case EX_AWAIT: {
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
            buf_printf(file, "struct %s { int64_t __fn; ", env_name->name);
            for (uint8_t i = 0; i < fd->closure->n_captures; i++) {
                Binding *captured = fd->closure->captures[i];
                buf_printf(file, "%s %s; ",
                           type_c_name(captured->type), captured->name->name);
            }
            buf_puts(file, "};\n");
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
            ctx->env_var_name = strdup(env_var_name_buf);
        }
    }

    /* Get the return type */
    TypeKind result_kind = e->type.kind == TY_FN ? e->type.as.fn.result_kind : TY_NIL;

    /* Phase 3/4: Check if body contains return/throw */
    bool body_has_return_or_throw = expr_contains_return_or_throw(fd->body);
    
    if (body_has_return_or_throw) {
        /* Body contains a return/throw - emit as statements only */
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
    ctx->closure = saved_closure;
    free((void*)ctx->env_var_name);
    ctx->env_var_name = saved_env_var_name;

    ctx->indent -= 4;
    buf_printf(file, "}\n\n");
    free((void*)fn_name);

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
                char *mfn = mangle_field_name(f->name);
                buf_printf(&early_file, "    %s %s;\n", ctype, mfn);
                free(mfn);
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
                    char *mfn = mangle_field_name(f->name);
                    if (f->kind == TY_RC) {
                        buf_printf(&early_file, "    if (s->%s) { rc_strong_decrement(s->%s); rc_free_queue_drain(); }\n",
                                   mfn, mfn);
                    } else if (f->kind == TY_WEAK) {
                        buf_printf(&early_file, "    if (s->%s) rc_weak_decrement(s->%s);\n",
                                   mfn, mfn);
                    } else if (f->kind == TY_REF) {
                        buf_printf(&early_file, "    if (s->%s) free(s->%s);\n",
                                   mfn, mfn);
                    }
                    free(mfn);
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

    /* Phase R2: tur_panic - integrated with defer chain */
    buf_puts(out, "/* Phase R2: tur_panic */\n");
    buf_puts(out, "static int tur_panic_in_progress = 0;\n");
    buf_puts(out, "static tur_frame *global_panic_frame = NULL;\n");
    buf_puts(out, "static void tur_panic_set_frame(tur_frame *f) {\n");
    buf_puts(out, "    global_panic_frame = f;\n");
    buf_puts(out, "}\n");
    buf_puts(out, "static void tur_panic(const char *msg) {\n");
    buf_puts(out, "    if (tur_panic_in_progress) {\n");
    buf_puts(out, "        fprintf(stderr, \"double panic: aborting\\n\");\n");
    buf_puts(out, "        abort();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    tur_panic_in_progress = 1;\n");
    buf_puts(out, "    fprintf(stderr, \"panic at %s:%d: %s\\n\", __FILE__, __LINE__, msg ? msg : \"(no message)\");\n");
    buf_puts(out, "    if (global_panic_frame) {\n");
    buf_puts(out, "        tur_frame_fire_chain(global_panic_frame);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    abort();\n");
    buf_puts(out, "}\n\n");

    /* Phase R5: tur_panic_abort for #[no-unwind] */
    buf_puts(out, "/* Phase R5: tur_panic_abort - no unwinding, immediate abort */\n");
    buf_puts(out, "static void tur_panic_abort(const char *msg) {\n");
    buf_puts(out, "    fprintf(stderr, \"panic (no unwind): %s\\n\", msg ? msg : \"(no message)\");\n");
    buf_puts(out, "    abort();\n");
    buf_puts(out, "}\n\n");

    /* Phase R2: Panic with typed payload */
    buf_puts(out, "/* Phase R2: tur_panic_with */\n");
    buf_puts(out, "typedef struct tur_panic_payload tur_panic_payload;\n");
    buf_puts(out, "struct tur_panic_payload {\n");
    buf_puts(out, "    int type_tag;\n");
    buf_puts(out, "    void *value;\n");
    buf_puts(out, "    const char *file;\n");
    buf_puts(out, "    int line;\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "static tur_panic_payload *global_panic_payload = NULL;\n");
    buf_puts(out, "static jmp_buf global_panic_jmpbuf;\n");
    buf_puts(out, "static int global_panic_jmpbuf_valid = 0;\n\n");
    buf_puts(out, "static tur_panic_payload *panic_payload_new(int type_tag, void *payload, const char *file, int line) {\n");
    buf_puts(out, "    tur_panic_payload *p = (tur_panic_payload *)malloc(sizeof(tur_panic_payload));\n");
    buf_puts(out, "    if (!p) { fprintf(stderr, \"panic: oom\\n\"); abort(); }\n");
    buf_puts(out, "    p->type_tag = type_tag; p->value = payload; p->file = file; p->line = line;\n");
    buf_puts(out, "    return p;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void panic_payload_free(tur_panic_payload *p) {\n");
    buf_puts(out, "    if (p) { free(p->value); free(p); }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_panic_with(int type_tag, void *payload, const char *file, int line) {\n");
    buf_puts(out, "    if (tur_panic_in_progress) {\n");
    buf_puts(out, "        fprintf(stderr, \"double panic: aborting\\n\");\n");
    buf_puts(out, "        free(payload);\n");
    buf_puts(out, "        abort();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    tur_panic_in_progress = 1;\n");
    /* Phase TG-004-2 PR: Check global handler first (try/catch has priority), then fiber */
    buf_puts(out, "    if (global_panic_jmpbuf_valid) {\n");
    buf_puts(out, "        global_panic_payload = panic_payload_new(type_tag, payload, file, line);\n");
    buf_puts(out, "        longjmp(global_panic_jmpbuf, 1);\n");
    buf_puts(out, "    } else if (tur_current_fiber && tur_current_fiber->panic_jmpbuf_valid) {\n");
    buf_puts(out, "        /* Use per-fiber panic buffer - set up global payload for cleanup */\n");
    buf_puts(out, "        global_panic_payload = panic_payload_new(type_tag, payload, file, line);\n");
    buf_puts(out, "        longjmp(tur_current_fiber->panic_jmpbuf, 1);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    fprintf(stderr, \"panic at %s:%d\\n\", file ? file : \"(unknown)\", line);\n");
    buf_puts(out, "    free(payload);\n");
    buf_puts(out, "    abort();\n");
    buf_puts(out, "}\n\n");
    /* Phase R2: Panic payload accessors */
    buf_puts(out, "static int tur_panic_payload_type(tur_panic_payload *p) {\n");
    buf_puts(out, "    return p ? p->type_tag : 0;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void *tur_panic_payload_value(tur_panic_payload *p) {\n");
    buf_puts(out, "    return p ? p->value : NULL;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static const char *tur_panic_payload_file(tur_panic_payload *p) {\n");
    buf_puts(out, "    return p ? p->file : NULL;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static int tur_panic_payload_line(tur_panic_payload *p) {\n");
    buf_puts(out, "    return p ? p->line : 0;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void *tur_panic_payload_downcast(tur_panic_payload *p, int target_type) {\n");
    buf_puts(out, "    if (!p || p->type_tag != target_type) return NULL;\n");
    buf_puts(out, "    return p->value;\n");
    buf_puts(out, "}\n\n");
    /* Phase R2: catch-unwind and catch-panic-of */
    buf_puts(out, "/* Phase R2: catch-unwind/catch-panic-of */\n");
    buf_puts(out, "typedef enum { TUR_RESULT_OK, TUR_RESULT_ERR } tur_result_tag;\n");
    buf_puts(out, "typedef struct tur_result tur_result;\n");
    buf_puts(out, "struct tur_result {\n");
    buf_puts(out, "    tur_result_tag tag;\n");
    buf_puts(out, "    union { int64_t ok_val; void *ok_ptr; tur_panic_payload *err; } u;\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "typedef void (*tur_thunk_fn)(void *env, tur_result *out);\n\n");
    buf_puts(out, "static bool tur_catch_unwind(tur_thunk_fn thunk, void *env, tur_result *out) {\n");
    buf_puts(out, "    if (global_panic_jmpbuf_valid) {\n");
    buf_puts(out, "        thunk(env, out);\n");
    buf_puts(out, "        return false;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    global_panic_jmpbuf_valid = 1;\n");
    buf_puts(out, "    if (setjmp(global_panic_jmpbuf) == 0) {\n");
    buf_puts(out, "        thunk(env, out);\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "        if (global_panic_payload) {\n");
    buf_puts(out, "            panic_payload_free(global_panic_payload);\n");
    buf_puts(out, "            global_panic_payload = NULL;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        return false;\n");
    buf_puts(out, "    } else {\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "        out->tag = TUR_RESULT_ERR;\n");
    buf_puts(out, "        out->u.err = global_panic_payload;\n");
    buf_puts(out, "        global_panic_payload = NULL;\n");
    buf_puts(out, "        return true;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static bool tur_catch_panic_of(int expected_type, tur_thunk_fn thunk, void *env, tur_result *out) {\n");
    buf_puts(out, "    if (global_panic_jmpbuf_valid) {\n");
    buf_puts(out, "        thunk(env, out);\n");
    buf_puts(out, "        return false;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    global_panic_jmpbuf_valid = 1;\n");
    buf_puts(out, "    if (setjmp(global_panic_jmpbuf) == 0) {\n");
    buf_puts(out, "        thunk(env, out);\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "        if (global_panic_payload) {\n");
    buf_puts(out, "            panic_payload_free(global_panic_payload);\n");
    buf_puts(out, "            global_panic_payload = NULL;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        return false;\n");
    buf_puts(out, "    } else {\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "        if (global_panic_payload && global_panic_payload->type_tag == expected_type) {\n");
    buf_puts(out, "            out->tag = TUR_RESULT_ERR;\n");
    buf_puts(out, "            out->u.err = global_panic_payload;\n");
    buf_puts(out, "            global_panic_payload = NULL;\n");
    buf_puts(out, "            return true;\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "        if (global_panic_payload) {\n");
    buf_puts(out, "            /* Type mismatch - re-panic */\n");
    buf_puts(out, "            tur_panic_with(global_panic_payload->type_tag, global_panic_payload->value,\n");
    buf_puts(out, "                           global_panic_payload->file, global_panic_payload->line);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        return false;\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
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
        /* Phase T21-A/B / P19-8: FiberBlock — cooperative fiber runtime via ucontext_t.
     * tur_current_fiber is thread-local; set/restored by tur_fiber_block_resume.
     * tur_effect_perform checks tur_current_fiber->effect_handler_chain for fiber-local
     * effect handler chains (Phase T21-B / P19-8). */
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
    buf_puts(out, "    int parked; /* Phase T21: scheduler park/unpark */\n");
    buf_puts(out, "    int64_t result;\n");
    buf_puts(out, "    int64_t arg;\n");
    buf_puts(out, "    void *effect_handler_chain; /* Phase P19-8: per-fiber effect handler chain */\n");
    buf_puts(out, "    void (*entry_fn)(void);\n");
    buf_puts(out, "    void *fiber_local; /* Phase T21: fiber-local storage */\n");
    /* Phase T22: Structured concurrency */
    buf_puts(out, "    void *task_group; /* Parent TaskGroup for cancellation */\n");
    buf_puts(out, "    bool cancelled; /* Set when parent TaskGroup is cancelled */\n");
    /* Phase TG-004-1 PR: Per-fiber panic handling for auto-cancel propagation */
    buf_puts(out, "    jmp_buf panic_jmpbuf; /* Per-fiber panic recovery buffer */\n");
    buf_puts(out, "    bool panic_jmpbuf_valid; /* Whether this fiber's panic handler is active */\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "static __thread FiberBlock *tur_current_fiber = NULL;\n");
    /* Phase T22: Cooperative cancellation flag */
    buf_puts(out, "static __thread bool tur_fiber_cancelled_flag = false;\n\n");
    /* Phase T22: TaskGroup notification forward declaration */
    buf_puts(out, "static void tur_task_group_notify_done(void *task_group);\n\n");
    buf_puts(out, "static void tur_fiber_shim(uint32_t hi, uint32_t lo) {\n");
    buf_puts(out, "    FiberBlock *f = (FiberBlock *)(((uintptr_t)hi << 32) | (uintptr_t)(uint32_t)lo);\n");
    /* Phase TG-004-3 PR: Per-fiber panic handling with auto-cancel on panic */
    buf_puts(out, "    void *task_group = f->task_group;\n");
    buf_puts(out, "    if (task_group) {\n");
    buf_puts(out, "        /* Save previous global panic handler state */\n");
    buf_puts(out, "        int prev_global_valid = global_panic_jmpbuf_valid;\n");
    buf_puts(out, "        jmp_buf prev_global_buf;\n");
    buf_puts(out, "        if (prev_global_valid) memcpy(&prev_global_buf, &global_panic_jmpbuf, sizeof(jmp_buf));\n");
    buf_puts(out, "        /* Clear global to prevent interference */\n");
    buf_puts(out, "        global_panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "        /* Set up per-fiber panic handler */\n");
    buf_puts(out, "        if (setjmp(f->panic_jmpbuf) == 0) {\n");
    buf_puts(out, "            f->panic_jmpbuf_valid = 1;\n");
    buf_puts(out, "            tur_current_fiber = f;\n");
    buf_puts(out, "            f->entry_fn();\n");
    buf_puts(out, "            tur_current_fiber = NULL;\n");
    buf_puts(out, "            f->panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "            if (global_panic_payload) {\n");
    buf_puts(out, "                panic_payload_free(global_panic_payload);\n");
    buf_puts(out, "                global_panic_payload = NULL;\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "            /* Restore previous global panic handler */\n");
    buf_puts(out, "            global_panic_jmpbuf_valid = prev_global_valid;\n");
    buf_puts(out, "            if (prev_global_valid) memcpy(&global_panic_jmpbuf, &prev_global_buf, sizeof(jmp_buf));\n");
    buf_puts(out, "        } else {\n");
    buf_puts(out, "            /* Panic caught - auto-cancel task group (TG-004-3) */\n");
    buf_puts(out, "            f->panic_jmpbuf_valid = 0;\n");
    buf_puts(out, "            tur_current_fiber = NULL;\n");
    buf_puts(out, "            if (global_panic_payload) {\n");
    buf_puts(out, "                typedef struct TaskGroupBlock { bool cancelled; bool done; int64_t cancel_reason; pthread_mutex_t lock; pthread_cond_t done_cond; } TaskGroupBlock;\n");
    buf_puts(out, "                TaskGroupBlock *g = (TaskGroupBlock *)task_group;\n");
    buf_puts(out, "                pthread_mutex_lock(&g->lock);\n");
    buf_puts(out, "                g->cancelled = true;\n");
    buf_puts(out, "                g->done = true;\n");
    buf_puts(out, "                g->cancel_reason = 1; /* panic reason (TG-004-2) */\n");
    buf_puts(out, "                pthread_cond_broadcast(&g->done_cond);\n");
    buf_puts(out, "                pthread_mutex_unlock(&g->lock);\n");
    buf_puts(out, "                tur_fiber_set_cancelled(true);\n");
    buf_puts(out, "                panic_payload_free(global_panic_payload);\n");
    buf_puts(out, "                global_panic_payload = NULL;\n");
    buf_puts(out, "            }\n");
    buf_puts(out, "            /* Restore previous global panic handler */\n");
    buf_puts(out, "            global_panic_jmpbuf_valid = prev_global_valid;\n");
    buf_puts(out, "            if (prev_global_valid) memcpy(&global_panic_jmpbuf, &prev_global_buf, sizeof(jmp_buf));\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    } else {\n");
    buf_puts(out, "        /* No task group, just run the function normally */\n");
    buf_puts(out, "        f->entry_fn();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    f->done = 1;\n");
    /* Phase T22: Notify task group on completion (TG-002: only if task_group is set) */
    buf_puts(out, "    if (task_group) tur_task_group_notify_done(task_group);\n");
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
    /* Phase T22: Check if fiber or its task group was cancelled before resuming */
    buf_puts(out, "    if (f->cancelled) { f->done = 1; return 0; }\n");
    buf_puts(out, "    if (f->task_group) {\n");
    buf_puts(out, "        typedef struct TaskGroupBlock { bool cancelled; } TaskGroupBlock;\n");
    buf_puts(out, "        if (((TaskGroupBlock *)f->task_group)->cancelled) { f->cancelled = 1; f->done = 1; return 0; }\n");
    buf_puts(out, "    }\n");
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
    /* Phase T21: Fiber-local storage */
    buf_puts(out, "typedef struct FiberLocalEntry FiberLocalEntry;\n");
    buf_puts(out, "struct FiberLocalEntry {\n");
    buf_puts(out, "    int64_t key;\n");
    buf_puts(out, "    int64_t value;\n");
    buf_puts(out, "    FiberLocalEntry *next;\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "static void tur_fiber_local_free(FiberBlock *f) {\n");
    buf_puts(out, "    FiberLocalEntry *e = (FiberLocalEntry *)f->fiber_local;\n");
    buf_puts(out, "    while (e) { FiberLocalEntry *n = e->next; free(e); e = n; }\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static int64_t tur_fiber_local_get(FiberBlock *f, int64_t key) {\n");
    buf_puts(out, "    FiberLocalEntry *e = (FiberLocalEntry *)f->fiber_local;\n");
    buf_puts(out, "    while (e) { if (e->key == key) return e->value; e = e->next; }\n");
    buf_puts(out, "    return 0;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_fiber_local_set(FiberBlock *f, int64_t key, int64_t value) {\n");
    buf_puts(out, "    FiberLocalEntry *e = (FiberLocalEntry *)f->fiber_local;\n");
    buf_puts(out, "    while (e) { if (e->key == key) { e->value = value; return; } e = e->next; }\n");
    buf_puts(out, "    FiberLocalEntry *n = (FiberLocalEntry *)malloc(sizeof(FiberLocalEntry));\n");
    buf_puts(out, "    if (!n) { fprintf(stderr, \"fiber-local: oom\\n\"); abort(); }\n");
    buf_puts(out, "    n->key = key; n->value = value;\n");
    buf_puts(out, "    n->next = (FiberLocalEntry *)f->fiber_local;\n");
    buf_puts(out, "    f->fiber_local = (void *)n;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_fiber_block_free(FiberBlock *f) {\n");
    buf_puts(out, "    if (!f) return;\n");
    buf_puts(out, "    tur_fiber_local_free(f);\n");
    buf_puts(out, "    free(f->stack); free(f);\n");
    buf_puts(out, "}\n\n");
    /* Phase T22: Fiber cancellation */
    buf_puts(out, "static bool tur_fiber_cancelled(void) {\n");
    buf_puts(out, "    return tur_fiber_cancelled_flag;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_fiber_set_cancelled(bool c) {\n");
    buf_puts(out, "    tur_fiber_cancelled_flag = c;\n");
    buf_puts(out, "}\n\n");
    /* Phase T22: TaskGroup notification on fiber completion */
    buf_puts(out, "static void tur_task_group_notify_done(void *task_group) {\n");
    buf_puts(out, "    if (!task_group) return;\n");
    buf_puts(out, "    typedef struct TaskGroupBlock {\n");
    buf_puts(out, "        pthread_mutex_t lock;\n");
    buf_puts(out, "        pthread_cond_t done_cond;\n");
    buf_puts(out, "        int64_t task_count;\n");
    buf_puts(out, "        int64_t completed_count;\n");
    buf_puts(out, "        bool cancelled;\n");
    buf_puts(out, "        bool done;\n");
    buf_puts(out, "        pthread_t owner_thread;\n");
    buf_puts(out, "    } TaskGroupBlock;\n");
    buf_puts(out, "    TaskGroupBlock *g = (TaskGroupBlock *)task_group;\n");
    buf_puts(out, "    pthread_mutex_lock(&g->lock);\n");
    buf_puts(out, "    g->completed_count++;\n");
    buf_puts(out, "    if (g->completed_count >= g->task_count) {\n");
    buf_puts(out, "        g->done = true;\n");
    buf_puts(out, "        pthread_cond_broadcast(&g->done_cond);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    pthread_mutex_unlock(&g->lock);\n");
    buf_puts(out, "}\n\n");
    /* Phase T21: Cooperative Scheduler for fibers */
    buf_puts(out, "typedef struct TurScheduler TurScheduler;\n");
    buf_puts(out, "struct TurScheduler {\n");
    buf_puts(out, "    FiberBlock **run_queue;\n");
    buf_puts(out, "    int64_t run_queue_cap;\n");
    buf_puts(out, "    int64_t run_queue_len;\n");
    buf_puts(out, "    int64_t run_queue_head;\n");
    buf_puts(out, "    int64_t run_queue_tail;\n");
    buf_puts(out, "    FiberBlock *current_fiber;\n");
    buf_puts(out, "    bool running;\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "static TurScheduler *tur_scheduler = NULL;\n\n");
    buf_puts(out, "static TurScheduler *tur_scheduler_new(void) {\n");
    buf_puts(out, "    TurScheduler *s = (TurScheduler *)calloc(1, sizeof(TurScheduler));\n");
    buf_puts(out, "    if (!s) { fprintf(stderr, \"scheduler: oom\\n\"); abort(); }\n");
    buf_puts(out, "    s->run_queue_cap = 64;\n");
    buf_puts(out, "    s->run_queue = (FiberBlock **)malloc(sizeof(FiberBlock *) * (size_t)s->run_queue_cap);\n");
    buf_puts(out, "    if (!s->run_queue) { free(s); fprintf(stderr, \"scheduler: queue oom\\n\"); abort(); }\n");
    buf_puts(out, "    s->run_queue_len = 0;\n");
    buf_puts(out, "    s->run_queue_head = 0;\n");
    buf_puts(out, "    s->run_queue_tail = 0;\n");
    buf_puts(out, "    s->current_fiber = NULL;\n");
    buf_puts(out, "    s->running = false;\n");
    buf_puts(out, "    return s;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static TurScheduler *tur_scheduler_current(void) {\n");
    buf_puts(out, "    return tur_scheduler;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_enqueue(TurScheduler *s, FiberBlock *f) {\n");
    buf_puts(out, "    if (s->run_queue_len >= s->run_queue_cap) {\n");
    buf_puts(out, "        int64_t new_cap = s->run_queue_cap * 2;\n");
    buf_puts(out, "        FiberBlock **new_q = (FiberBlock **)malloc(sizeof(FiberBlock *) * (size_t)new_cap);\n");
    buf_puts(out, "        if (!new_q) { fprintf(stderr, \"scheduler: grow oom\\n\"); abort(); }\n");
    buf_puts(out, "        for (int64_t i = 0; i < s->run_queue_len; i++)\n");
    buf_puts(out, "            new_q[i] = s->run_queue[(s->run_queue_head + i) % s->run_queue_cap];\n");
    buf_puts(out, "        free(s->run_queue);\n");
    buf_puts(out, "        s->run_queue = new_q;\n");
    buf_puts(out, "        s->run_queue_cap = new_cap;\n");
    buf_puts(out, "        s->run_queue_head = 0;\n");
    buf_puts(out, "        s->run_queue_tail = s->run_queue_len;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    s->run_queue[s->run_queue_tail] = f;\n");
    buf_puts(out, "    s->run_queue_tail = (s->run_queue_tail + 1) % s->run_queue_cap;\n");
    buf_puts(out, "    s->run_queue_len++;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static FiberBlock *tur_scheduler_dequeue(TurScheduler *s) {\n");
    buf_puts(out, "    if (s->run_queue_len == 0) return NULL;\n");
    buf_puts(out, "    FiberBlock *f = s->run_queue[s->run_queue_head];\n");
    buf_puts(out, "    s->run_queue_head = (s->run_queue_head + 1) % s->run_queue_cap;\n");
    buf_puts(out, "    s->run_queue_len--;\n");
    buf_puts(out, "    return f;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_spawn(TurScheduler *s, FiberBlock *f) {\n");
    buf_puts(out, "    tur_scheduler_enqueue(s, f);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_run(TurScheduler *s) {\n");
    buf_puts(out, "    s->running = true;\n");
    buf_puts(out, "    while (s->running) {\n");
    buf_puts(out, "        FiberBlock *f = tur_scheduler_dequeue(s);\n");
    buf_puts(out, "        if (!f) { break; }\n");
    buf_puts(out, "        s->current_fiber = f;\n");
    buf_puts(out, "        tur_fiber_block_resume(f, 0);\n");
    buf_puts(out, "        s->current_fiber = NULL;\n");
    buf_puts(out, "        if (!f->done && !f->parked) tur_scheduler_enqueue(s, f);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    s->running = false;\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_run_to_completion(TurScheduler *s) {\n");
    buf_puts(out, "    s->running = true;\n");
    buf_puts(out, "    while (s->run_queue_len > 0 && s->running) {\n");
    buf_puts(out, "        FiberBlock *f = tur_scheduler_dequeue(s);\n");
    buf_puts(out, "        if (f) {\n");
    buf_puts(out, "            s->current_fiber = f;\n");
    buf_puts(out, "            tur_fiber_block_resume(f, 0);\n");
    buf_puts(out, "            s->current_fiber = NULL;\n");
    buf_puts(out, "            if (!f->done && !f->parked) tur_scheduler_enqueue(s, f);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    s->running = false;\n");
    buf_puts(out, "}\n\n");
    /* Phase T21: Scheduler yield/park/unpark */
    buf_puts(out, "static void tur_scheduler_yield(void) {\n");
    buf_puts(out, "    tur_fiber_block_yield(0);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_park(void) {\n");
    buf_puts(out, "    if (tur_current_fiber) tur_current_fiber->parked = 1;\n");
    buf_puts(out, "    tur_fiber_block_yield(0);\n");
    buf_puts(out, "}\n\n");
    buf_puts(out, "static void tur_scheduler_unpark(FiberBlock *f) {\n");
    buf_puts(out, "    if (!f) return;\n");
    buf_puts(out, "    f->parked = 0;\n");
    buf_puts(out, "    if (tur_scheduler) tur_scheduler_enqueue(tur_scheduler, f);\n");
    buf_puts(out, "}\n\n");

    /* AW-010: Scheduler timeout - schedule callback after delay */
    buf_puts(out, "typedef struct TurTimeout TurTimeout;\n");
    buf_puts(out, "struct TurTimeout {\n");
    buf_puts(out, "    int64_t ms;\n");
    buf_puts(out, "    void (*callback)(void *arg);\n");
    buf_puts(out, "    void *arg;\n");
    buf_puts(out, "    pthread_t thread;\n");
    buf_puts(out, "    TurTimeout *next;\n");
    buf_puts(out, "};\n\n");
    buf_puts(out, "static TurTimeout *tur_timeout_list = NULL;\n");
    buf_puts(out, "static pthread_mutex_t tur_timeout_lock = PTHREAD_MUTEX_INITIALIZER;\n\n");
    
    buf_puts(out, "static void *tur_timeout_thread(void *raw) {\n");
    buf_puts(out, "    TurTimeout *t = (TurTimeout *)raw;\n");
    buf_puts(out, "    if (t->ms > 0) {\n");
    buf_puts(out, "        struct timespec ts;\n");
    buf_puts(out, "        ts.tv_sec = t->ms / 1000;\n");
    buf_puts(out, "        ts.tv_nsec = (t->ms % 1000) * 1000000L;\n");
    buf_puts(out, "        nanosleep(&ts, NULL);\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    t->callback(t->arg);\n");
    buf_puts(out, "    pthread_mutex_lock(&tur_timeout_lock);\n");
    buf_puts(out, "    TurTimeout *prev = NULL;\n");
    buf_puts(out, "    TurTimeout *curr = tur_timeout_list;\n");
    buf_puts(out, "    while (curr && curr != t) { prev = curr; curr = curr->next; }\n");
    buf_puts(out, "    if (prev) prev->next = t->next;\n");
    buf_puts(out, "    else tur_timeout_list = t->next;\n");
    buf_puts(out, "    pthread_mutex_unlock(&tur_timeout_lock);\n");
    buf_puts(out, "    free(t);\n");
    buf_puts(out, "    return NULL;\n");
    buf_puts(out, "}\n\n");
    
    buf_puts(out, "static void tur_scheduler_timeout(int64_t ms, void (*callback)(void *arg), void *arg) {\n");
    buf_puts(out, "    TurTimeout *t = (TurTimeout *)malloc(sizeof(TurTimeout));\n");
    buf_puts(out, "    if (!t) { fprintf(stderr, \"timeout: oom\\n\"); abort(); }\n");
    buf_puts(out, "    t->ms = ms;\n");
    buf_puts(out, "    t->callback = callback;\n");
    buf_puts(out, "    t->arg = arg;\n");
    buf_puts(out, "    t->next = NULL;\n");
    buf_puts(out, "    pthread_mutex_lock(&tur_timeout_lock);\n");
    buf_puts(out, "    t->next = tur_timeout_list;\n");
    buf_puts(out, "    tur_timeout_list = t;\n");
    buf_puts(out, "    pthread_mutex_unlock(&tur_timeout_lock);\n");
    buf_puts(out, "    pthread_attr_t attr;\n");
    buf_puts(out, "    pthread_attr_init(&attr);\n");
    buf_puts(out, "    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);\n");
    buf_puts(out, "    pthread_t tid;\n");
    buf_puts(out, "    pthread_create(&tid, &attr, tur_timeout_thread, t);\n");
    buf_puts(out, "    pthread_attr_destroy(&attr);\n");
    buf_puts(out, "}\n\n");

    buf_puts(out, "#ifdef __clang__\n");
    buf_puts(out, "#pragma clang diagnostic pop\n");
    buf_puts(out, "#endif\n\n");
    /* Phase T21: Scheduler run_one helper */
    buf_puts(out, "static void tur_scheduler_run_one(TurScheduler *s) {\n");
    buf_puts(out, "    FiberBlock *f = tur_scheduler_dequeue(s);\n");
    buf_puts(out, "    if (!f) { return; }\n");
    buf_puts(out, "    s->current_fiber = f;\n");
    buf_puts(out, "    tur_fiber_block_resume(f, 0);\n");
    buf_puts(out, "    s->current_fiber = NULL;\n");
    buf_puts(out, "    if (!f->done && !f->parked) tur_scheduler_enqueue(s, f);\n");
    buf_puts(out, "}\n\n");
    
    /* Phase T21-F: async/await runtime - fiber-based Futures */
    buf_puts(out, "/* Phase T21-F: async/await runtime - fiber-based Futures */\n");
    buf_puts(out, "typedef enum { FUTURE_PENDING, FUTURE_FULFILLED, FUTURE_REJECTED } TurFutureStatus;\n\n");
    
    buf_puts(out, "typedef struct TurFuture TurFuture;\n");
    buf_puts(out, "struct TurFuture {\n");
    buf_puts(out, "    TurFutureStatus status;\n");
    buf_puts(out, "    int64_t value;\n");
    buf_puts(out, "    const char *error;  /* NULL if no error */\n");
    buf_puts(out, "    FiberBlock *fiber;  /* The fiber running the async task */\n");
    buf_puts(out, "    struct { void (*fn)(TurFuture *, int64_t); void *env; } on_complete;\n");
    buf_puts(out, "};\n\n");
    
    buf_puts(out, "/* Create a new pending future */\n");
    buf_puts(out, "static TurFuture *tur_future_new(void) {\n");
    buf_puts(out, "    TurFuture *f = (TurFuture *)calloc(1, sizeof(TurFuture));\n");
    buf_puts(out, "    if (!f) { fprintf(stderr, \"future: oom\\n\"); abort(); }\n");
    buf_puts(out, "    f->status = FUTURE_PENDING;\n");
    buf_puts(out, "    f->fiber = NULL;\n");
    buf_puts(out, "    f->on_complete.fn = NULL;\n");
    buf_puts(out, "    return f;\n");
    buf_puts(out, "}\n\n");
    
    buf_puts(out, "/* Fulfill a future with a value */\n");
    buf_puts(out, "static void tur_future_fulfill(TurFuture *f, int64_t value) {\n");
    buf_puts(out, "    if (!f || f->status != FUTURE_PENDING) return;\n");
    buf_puts(out, "    f->status = FUTURE_FULFILLED;\n");
    buf_puts(out, "    f->value = value;\n");
    buf_puts(out, "    if (f->on_complete.fn) f->on_complete.fn(f, value);\n");
    buf_puts(out, "}\n\n");
    
    buf_puts(out, "/* Reject a future with an error message */\n");
    buf_puts(out, "static void tur_future_reject(TurFuture *f, const char *error) {\n");
    buf_puts(out, "    if (!f || f->status != FUTURE_PENDING) return;\n");
    buf_puts(out, "    f->status = FUTURE_REJECTED;\n");
    buf_puts(out, "    f->error = error;\n");
    buf_puts(out, "}\n\n");
    
    buf_puts(out, "/* Check if future is done (fulfilled or rejected) */\n");
    buf_puts(out, "static int tur_future_done(TurFuture *f) {\n");
    buf_puts(out, "    return f && (f->status == FUTURE_FULFILLED || f->status == FUTURE_REJECTED);\n");
    buf_puts(out, "}\n\n");
    
    buf_puts(out, "/* Get future value (only valid if fulfilled) */\n");
    buf_puts(out, "static int64_t tur_future_get(TurFuture *f) {\n");
    buf_puts(out, "    if (!f || f->status != FUTURE_FULFILLED) return 0;\n");
    buf_puts(out, "    return f->value;\n");
    buf_puts(out, "}\n\n");
    
    /* Create a future that runs fn() and fulfills it with the result */
    buf_puts(out, "static TurFuture *tur_async_fiber(int64_t (*fn)(void)) {\n");
    buf_puts(out, "    TurFuture *future = tur_future_new();\n");
    buf_puts(out, "    if (!tur_scheduler) {\n");
    buf_puts(out, "        /* AW-005: Initialize scheduler on first use */\n");
    buf_puts(out, "        tur_scheduler = tur_scheduler_new();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* AW-005: Simplified v1 - call function directly and fulfill */\n");
    buf_puts(out, "    int64_t result = fn();\n");
    buf_puts(out, "    tur_future_fulfill(future, result);\n");
    buf_puts(out, "    return future;\n");
    buf_puts(out, "}\n\n");
    
    /* Await a future using shift + scheduler. If future is done, return value directly. */
    buf_puts(out, "/* AW-004: await lowering with shift + scheduler callback */\n");
    buf_puts(out, "static int64_t tur_await_future(TurFuture *f) {\n");
    buf_puts(out, "    if (!f) { fprintf(stderr, \"await: null future\\n\"); abort(); }\n");
    buf_puts(out, "    if (tur_future_done(f)) {\n");
    buf_puts(out, "        if (f->status == FUTURE_REJECTED) {\n");
    buf_puts(out, "            fprintf(stderr, \"await: future rejected: %s\\n\", f->error ? f->error : \"unknown\");\n");
    buf_puts(out, "            abort();\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        return f->value;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    /* Future not ready */\n");
    buf_puts(out, "    if (!tur_current_fiber) {\n");
    buf_puts(out, "        /* Not in a fiber - run the scheduler until future is done */\n");
    buf_puts(out, "        if (!tur_scheduler) {\n");
    buf_puts(out, "            fprintf(stderr, \"await: no scheduler and not in fiber\\n\");\n");
    buf_puts(out, "            abort();\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        while (!tur_future_done(f)) {\n");
    buf_puts(out, "            tur_scheduler_run_one(tur_scheduler);\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "    } else {\n");
    buf_puts(out, "        /* In a fiber - yield and let scheduler resume us */\n");
    buf_puts(out, "        /* Register a callback to re-enqueue this fiber when future completes */\n");
    buf_puts(out, "        f->on_complete.fn = (void (*)(TurFuture *, int64_t))tur_fiber_block_resume;\n");
    buf_puts(out, "        f->on_complete.env = (void *)tur_current_fiber;\n");
    buf_puts(out, "        tur_fiber_block_yield(0);\n");
    buf_puts(out, "        /* When we resume, the future should be done */\n");
    buf_puts(out, "        if (tur_future_done(f) && f->status == FUTURE_REJECTED) {\n");
    buf_puts(out, "            fprintf(stderr, \"await: future rejected: %s\\n\", f->error ? f->error : \"unknown\");\n");
    buf_puts(out, "            abort();\n");
    buf_puts(out, "        }\n");
    buf_puts(out, "        return f->value;\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    if (f->status == FUTURE_REJECTED) {\n");
    buf_puts(out, "        fprintf(stderr, \"await: future rejected: %s\\n\", f->error ? f->error : \"unknown\");\n");
    buf_puts(out, "        abort();\n");
    buf_puts(out, "    }\n");
    buf_puts(out, "    return f->value;\n");
    buf_puts(out, "}\n\n");
    
    /* Free a future */
    buf_puts(out, "static void tur_future_free(TurFuture *f) {\n");
    buf_puts(out, "    if (!f) return;\n");
    buf_puts(out, "    free(f);\n");
    buf_puts(out, "}\n\n");
    
    /* Keep old TurAsyncTask for backward compatibility */
    buf_puts(out, "/* Backward compatibility: TurAsyncTask = TurFuture */\n");
    buf_puts(out, "typedef TurFuture TurAsyncTask;\n\n");
    buf_puts(out, "static __thread EffectHandlerFrame *global_effect_handler_chain = NULL;\n\n");
    buf_puts(out, "static int64_t tur_effect_perform(const char *name, int64_t *args, int n_args) {\n");
        /* T21-B: use fiber-local handler chain when inside a fiber */
    buf_puts(out, "    EffectHandlerFrame *frame =\n");
    buf_puts(out, "        (tur_current_fiber && tur_current_fiber->effect_handler_chain)\n");
    buf_puts(out, "        ? (EffectHandlerFrame *)tur_current_fiber->effect_handler_chain\n");
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
