#include "emit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtins.h"

typedef struct EmitCtx {
    Buf  *file;     /* file-scope decls (statics, includes) */
    Buf  *main_;    /* main() body */
    int   indent;
    int   tmp_n;
} EmitCtx;

/* ------------ helpers ------------ */

static void indent_buf(Buf *b, int n) {
    for (int i = 0; i < n; i++) buf_putc(b, ' ');
}

static char *fresh_tmp(EmitCtx *ctx) {
    char *p = (char *)malloc(24);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(p, 24, "__t%d", ctx->tmp_n++);
    return p;
}

/* Return a sanitized C identifier for a Binding. Caller frees. */
static char *name_for_binding(const Binding *b) {
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

/* ------------ atomic emitters (no statements emitted) ------------ */

static char *atom_nil(void)         { return strdup("((void)0)"); }
static char *atom_bool(bool b)      { return strdup(b ? "true" : "false"); }
static char *atom_int(int64_t i) {
    char buf[40];
    snprintf(buf, sizeof buf, "INT64_C(%lld)", (long long)i);
    return strdup(buf);
}
static char *atom_var(const Binding *b) {
    return name_for_binding(b);
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
        char *bn = name_for_binding(b);
        char *iv = emit_value(ctx, body, e->as.let_.bindings[i].init);
        indent_buf(body, ctx->indent);
        buf_printf(body, "%s %s = %s;\n", type_c_name(b->type), bn, iv);
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

static char *emit_do_value(EmitCtx *ctx, Buf *body, const Expr *e) {
    uint32_t n = e->as.do_.n;
    if (n == 0) return atom_nil();
    /* Run all but last as statements; last provides value (or runs as stmt
     * if its type is nil). */
    for (uint32_t i = 0; i < n - 1; i++) {
        emit_stmt(ctx, body, e->as.do_.items[i]);
    }
    const Expr *last = e->as.do_.items[n - 1];
    if (last->type.kind == TY_NIL) {
        emit_stmt(ctx, body, last);
        return atom_nil();
    }
    return emit_value(ctx, body, last);
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
    char *bn = name_for_binding(e->as.set_.target);
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
        case EX_VAR:      return atom_var(e->as.var.binding);
        case EX_LET:      return emit_let_value(ctx, body, e);
        case EX_IF:       return emit_if_value(ctx, body, e);
        case EX_DO:       return emit_do_value(ctx, body, e);
        case EX_BUILTIN:  return emit_builtin(ctx, body, e);
        case EX_WHILE:    emit_while_stmt(ctx, body, e); return atom_nil();
        case EX_SET:      emit_set_stmt(ctx, body, e);   return atom_nil();
        case EX_DEF:      /* handled at top level — shouldn't appear nested. */
        case EX_PROGRAM:
            return atom_nil();
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
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                emit_stmt(ctx, body, e->as.do_.items[i]);
            }
            return;
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
    }
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

    /* Pass 1: collect all top-level defs as static decls, emit init code in main body. */
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *e = program->as.program.items[i];
        if (e->kind == EX_DEF) {
            char *bn = name_for_binding(e->as.def_.binding);
            buf_printf(&file, "static %s %s;\n",
                       type_c_name(e->as.def_.binding->type), bn);
            char *iv = emit_value(&ctx, &body, e->as.def_.init);
            indent_buf(&body, ctx.indent);
            buf_printf(&body, "%s = %s;\n", bn, iv);
            free(bn); free(iv);
        } else {
            emit_stmt(&ctx, &body, e);
        }
    }

    /* Final assembly. */
    buf_puts(out, "/* generated by tur (phase 1) */\n");
    buf_puts(out, "#include <stdio.h>\n");
    buf_puts(out, "#include <stdint.h>\n");
    buf_puts(out, "#include <stdbool.h>\n");
    buf_puts(out, "\n");
    if (file.len) { buf_write(out, file.data, file.len); buf_putc(out, '\n'); }
    buf_puts(out, "int main(void) {\n");
    if (body.len) buf_write(out, body.data, body.len);
    buf_puts(out, "    return 0;\n");
    buf_puts(out, "}\n");

    buf_free(&file);
    buf_free(&body);
    return 0;
}
