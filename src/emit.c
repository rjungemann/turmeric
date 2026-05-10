#include "emit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtins.h"
#include "types.h"

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
} EmitCtx;

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

/* Forward declarations for the mutually-recursive emit. emit_value emits
 * statements (if any) into `body` and returns a malloc()'d C-expression
 * string that the caller takes ownership of and must free(). */
static char *emit_value(EmitCtx *ctx, Buf *body, const Expr *e);
static void  emit_stmt (EmitCtx *ctx, Buf *body, const Expr *e);
static void  emit_fn_def(EmitCtx *ctx, Buf *file, const Expr *e);

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

/* Phase 4: defer-aware do-block emission.
 *
 * v0 strategy: collect EX_DEFER children of this EX_DO, emit non-defer items
 * in source order, then emit defer bodies in reverse (LIFO) just before the
 * EX_DO finishes. This is the simplest correct lowering for the common case
 * (defers at scope-top in let/defn/while bodies, since those bodies are
 * wrapped in EX_DO during elaboration).
 *
 * Known limitations of this v0 lowering (tracked in turmeric-plan.md §10.5):
 *   - Defers inside `if`/`cond` branches won't fire LIFO with sibling defers
 *     in the surrounding scope; they only fire at the branch's own scope end.
 *   - Defers inside `while` bodies fire on every loop iteration.
 *   - Early `return` is not yet a language feature, so it isn't handled here.
 *
 * Phase 4's architectural target (per effects-plan §6.10) is the unified
 * runtime-list-on-frame model. That replaces this inline reordering with a
 * stack-allocated `tur_frame` per scope and `tur_frame_push_defer` calls.
 * The switch is local to this file plus the new runtime header. */
static char *emit_do_value(EmitCtx *ctx, Buf *body, const Expr *e) {
    uint32_t n = e->as.do_.n;
    if (n == 0) return atom_nil();

    /* Find last non-defer item to source the do-block's value from. */
    int last_value_idx = -1;
    for (int i = (int)n - 1; i >= 0; i--) {
        if (e->as.do_.items[i]->kind != EX_DEFER) { last_value_idx = i; break; }
    }

    for (uint32_t i = 0; i < n; i++) {
        const Expr *it = e->as.do_.items[i];
        if (it->kind == EX_DEFER) continue; /* fired below */
        if ((int)i == last_value_idx) continue; /* emitted as value below */
        emit_stmt(ctx, body, it);
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

    /* Fire defers LIFO. */
    for (int i = (int)n - 1; i >= 0; i--) {
        const Expr *it = e->as.do_.items[i];
        if (it->kind == EX_DEFER) emit_stmt(ctx, body, it->as.defer_.body);
    }

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
            /* Phase 4: same defer-aware ordering as emit_do_value, statement form. */
            uint32_t n = e->as.do_.n;
            for (uint32_t i = 0; i < n; i++) {
                if (e->as.do_.items[i]->kind == EX_DEFER) continue;
                emit_stmt(ctx, body, e->as.do_.items[i]);
            }
            for (int i = (int)n - 1; i >= 0; i--) {
                const Expr *it = e->as.do_.items[i];
                if (it->kind == EX_DEFER) emit_stmt(ctx, body, it->as.defer_.body);
            }
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
    buf_puts(out, "\n");
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
    buf_puts(out, "#include <stdio.h>\n\n");

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
