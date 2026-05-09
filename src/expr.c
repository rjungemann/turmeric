#include "expr.h"

#include <string.h>

Expr *expr_new(Arena *a, ExprKind k, Type t, Span span) {
    Expr *e = (Expr *)arena_alloc(a, sizeof(Expr));
    memset(e, 0, sizeof(*e));
    e->kind = k;
    e->type = t;
    e->span = span;
    return e;
}

/* Minimal debug printer; not critical to phase 1, but useful when codegen
 * misbehaves. */
void expr_print(Buf *b, const Expr *e) {
    switch (e->kind) {
        case EX_NIL_LIT:  buf_puts(b, "nil"); break;
        case EX_BOOL_LIT: buf_puts(b, e->as.b ? "true" : "false"); break;
        case EX_INT_LIT:  buf_printf(b, "%lld", (long long)e->as.i); break;
        case EX_CSTR_LIT: buf_putc(b, '"'); buf_write(b, e->as.s.p, e->as.s.len); buf_putc(b, '"'); break;
        case EX_VAR:      buf_write(b, e->as.var.binding->name->name, e->as.var.binding->name->len); break;
        case EX_LET:
            buf_puts(b, "(let [");
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (i) buf_putc(b, ' ');
                Binding *bd = e->as.let_.bindings[i].binding;
                buf_write(b, bd->name->name, bd->name->len);
                buf_putc(b, ' ');
                expr_print(b, e->as.let_.bindings[i].init);
            }
            buf_puts(b, "] ");
            expr_print(b, e->as.let_.body);
            buf_putc(b, ')');
            break;
        case EX_IF:
            buf_puts(b, "(if ");
            expr_print(b, e->as.if_.cond);
            buf_putc(b, ' ');
            expr_print(b, e->as.if_.then_);
            if (e->as.if_.else_or_null) {
                buf_putc(b, ' ');
                expr_print(b, e->as.if_.else_or_null);
            }
            buf_putc(b, ')');
            break;
        case EX_DO:
            buf_puts(b, "(do");
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                buf_putc(b, ' ');
                expr_print(b, e->as.do_.items[i]);
            }
            buf_putc(b, ')');
            break;
        case EX_WHILE:
            buf_puts(b, "(while ");
            expr_print(b, e->as.while_.cond);
            buf_putc(b, ' ');
            expr_print(b, e->as.while_.body);
            buf_putc(b, ')');
            break;
        case EX_SET:
            buf_puts(b, "(set! ");
            buf_write(b, e->as.set_.target->name->name, e->as.set_.target->name->len);
            buf_putc(b, ' ');
            expr_print(b, e->as.set_.value);
            buf_putc(b, ')');
            break;
        case EX_DEF:
            buf_puts(b, "(def ");
            buf_write(b, e->as.def_.binding->name->name, e->as.def_.binding->name->len);
            buf_putc(b, ' ');
            expr_print(b, e->as.def_.init);
            buf_putc(b, ')');
            break;
        case EX_BUILTIN:
            buf_putc(b, '(');
            buf_puts(b, "builtin");
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                buf_putc(b, ' ');
                expr_print(b, e->as.builtin.args[i]);
            }
            buf_putc(b, ')');
            break;
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++) {
                if (i) buf_putc(b, '\n');
                expr_print(b, e->as.program.items[i]);
            }
            break;
        /* Phase 2: not yet implemented */
        case EX_FN:
            buf_puts(b, "<fn>"); break;
        case EX_CALL:
            buf_puts(b, "<call>"); break;
        case EX_FN_DEF:
            buf_puts(b, "<fn-def>"); break;
        case EX_EXTERN_C:
            buf_puts(b, "<extern-c>"); break;
        case EX_INLINE_C:
            buf_puts(b, "<inline-c>"); break;
    }
}
