#include "expr.h"
#include "typeclass.h"  /* Phase 15 */

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
        case EX_FLOAT_LIT: buf_printf(b, "%g", e->as.f); break;
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
        case EX_SET_DEREF:
            buf_puts(b, "(set! (@ ");
            expr_print(b, e->as.set_deref_.ref);
            buf_puts(b, ") ");
            expr_print(b, e->as.set_deref_.value);
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
        /* Phase 3 */
        case EX_CLOSURE:
            buf_puts(b, "<closure>"); break;
        /* Phase 4 */
        case EX_DEFER:
            buf_puts(b, "(defer ");
            expr_print(b, e->as.defer_.body);
            buf_putc(b, ')');
            break;
        case EX_RETURN:
            buf_puts(b, "(return");
            if (e->as.return_.value) {
                buf_putc(b, ' ');
                expr_print(b, e->as.return_.value);
            }
            buf_putc(b, ')');
            break;
        /* Phase 5 */
        case EX_REF:
            buf_puts(b, "(ref ");
            expr_print(b, e->as.ref_.expr);
            buf_putc(b, ')');
            break;
        case EX_DEREF:
            buf_puts(b, "(@ ");
            expr_print(b, e->as.deref_.expr);
            buf_putc(b, ')');
            break;
        /* Phase 9: rc<T> + weak<T> */
        case EX_RC_OF:
            buf_puts(b, "(rc/of ");
            expr_print(b, e->as.rc_of_.expr);
            buf_putc(b, ')');
            break;
        case EX_RC_CLONE:
            buf_puts(b, "(rc/clone ");
            expr_print(b, e->as.rc_clone_.expr);
            buf_putc(b, ')');
            break;
        case EX_RC_DROP:
            buf_puts(b, "(rc/drop ");
            expr_print(b, e->as.rc_drop_.expr);
            buf_putc(b, ')');
            break;
        case EX_RC_PTR:
            buf_puts(b, "(rc->ptr ");
            expr_print(b, e->as.rc_ptr_.expr);
            buf_putc(b, ')');
            break;
        case EX_RC_COUNT:
            buf_puts(b, "(rc/strong-count ");
            expr_print(b, e->as.rc_count_.expr);
            buf_putc(b, ')');
            break;
        case EX_RC_FROM_REF:
            buf_puts(b, "(rc/from-ref ");
            expr_print(b, e->as.rc_from_ref_.expr);
            buf_putc(b, ')');
            break;
        case EX_REF_FROM_RC:
            buf_puts(b, "(ref/from-rc ");
            expr_print(b, e->as.ref_from_rc_.expr);
            buf_putc(b, ')');
            break;
        case EX_WEAK:
            buf_puts(b, "(weak ");
            expr_print(b, e->as.weak_.expr);
            buf_putc(b, ')');
            break;
        case EX_WEAK_UPGRADE:
            buf_puts(b, "(upgrade ");
            expr_print(b, e->as.weak_upgrade_.expr);
            buf_putc(b, ')');
            break;
        case EX_WEAK_PRED:
            buf_puts(b, "(weak? ");
            expr_print(b, e->as.weak_pred_.expr);
            buf_putc(b, ')');
            break;
        case EX_REF_PRED:
            buf_puts(b, "(ref? ");
            expr_print(b, e->as.ref_pred_.expr);
            buf_putc(b, ')');
            break;
        case EX_CONT_PRED:
            buf_puts(b, "(cont? ");
            expr_print(b, e->as.cont_pred_.expr);
            buf_putc(b, ')');
            break;
        /* Phase T21-F: async/await */
        case EX_ASYNC:
            buf_puts(b, "(async ");
            expr_print(b, e->as.async_.fn_expr);
            buf_putc(b, ')');
            break;
        case EX_AWAIT:
            buf_puts(b, "(await ");
            expr_print(b, e->as.await_.fut_expr);
            buf_putc(b, ')');
            break;
        /* Phase 12: Borrow traits */
        case EX_BORROW_IMMUT:
            buf_puts(b, "(& ");
            expr_print(b, e->as.borrow_immut_.expr);
            buf_putc(b, ')');
            break;
        case EX_BORROW_MUT:
            buf_puts(b, "(&mut ");
            expr_print(b, e->as.borrow_mut_.expr);
            buf_putc(b, ')');
            break;
        /* Phase 15: Typeclasses */
        case EX_TYPECLASS_DEF:
            buf_puts(b, "(defclass ");
            buf_puts(b, e->as.typeclass_def_.typeclass->name->name);
            buf_putc(b, ')');
            break;
        case EX_INSTANCE_DEF:
            buf_puts(b, "(definstance ");
            buf_puts(b, e->as.instance_def_.instance->typeclass->name->name);
            buf_putc(b, ')');
            break;
        /* Phase 17: Exceptions */
        case EX_THROW:
            buf_puts(b, "(throw ");
            expr_print(b, e->as.throw_.payload);
            buf_putc(b, ')');
            break;
        /* Phase R2: Panic */
        case EX_PANIC:
            buf_puts(b, "(panic ");
            expr_print(b, e->as.panic_.payload);
            buf_putc(b, ')');
            break;
        case EX_TRY:
            buf_puts(b, "(try ");
            expr_print(b, e->as.try_.body);
            for (uint8_t i = 0; i < e->as.try_.n_clauses; i++) {
                buf_puts(b, " (catch [");
                buf_puts(b, e->as.try_.clauses[i].var_name->name);
                buf_puts(b, "] ");
                expr_print(b, e->as.try_.clauses[i].handler);
                buf_putc(b, ')');
            }
            if (e->as.try_.finally_body) {
                buf_puts(b, " (finally ");
                expr_print(b, e->as.try_.finally_body);
                buf_putc(b, ')');
            }
            buf_putc(b, ')');
            break;
        /* Phase 18: Delimited continuations */
        case EX_RESET:
            buf_puts(b, "(reset ");
            expr_print(b, e->as.reset_.body);
            buf_putc(b, ')');
            break;
        case EX_SHIFT:
            buf_puts(b, "(shift ");
            expr_print(b, e->as.shift_.k_fn);
            buf_puts(b, " ");
            expr_print(b, e->as.shift_.body);
            buf_putc(b, ')');
            break;
        case EX_SHIFT0:
            buf_puts(b, "(shift0 ");
            expr_print(b, e->as.shift0_.k_fn);
            buf_puts(b, " ");
            expr_print(b, e->as.shift0_.body);
            buf_putc(b, ')');
            break;
        /* Phase 19: Algebraic effects */
        case EX_DEFECT:
            buf_puts(b, "(defeffect ");
            buf_puts(b, e->as.effect_def_.def->name->name);
            buf_putc(b, ')');
            break;
        case EX_PERFORM:
            buf_puts(b, "(perform (");
            buf_puts(b, e->as.perform_.perform->effect_name->name);
            for (uint8_t i = 0; i < e->as.perform_.perform->n_args; i++) {
                buf_putc(b, ' ');
                expr_print(b, e->as.perform_.perform->args[i]);
            }
            buf_puts(b, "))");
            break;
        case EX_HANDLE:
            buf_puts(b, "(handle ");
            expr_print(b, e->as.handle_.handle->body);
            for (uint8_t i = 0; i < e->as.handle_.handle->n_cases; i++) {
                buf_puts(b, " (");
                buf_puts(b, e->as.handle_.handle->cases[i].effect_name->name);
                buf_puts(b, " [");
                for (uint8_t j = 0; j < e->as.handle_.handle->cases[i].n_params; j++) {
                    if (j > 0) buf_putc(b, ' ');
                    buf_puts(b, e->as.handle_.handle->cases[i].param_names[j]->name);
                }
                buf_puts(b, "] ");
                buf_puts(b, e->as.handle_.handle->cases[i].k_name->name);
                buf_puts(b, ") ");
                expr_print(b, e->as.handle_.handle->cases[i].body);
            }
            buf_putc(b, ')');
            break;
        case EX_RESUME:
            buf_puts(b, "(resume ");
            expr_print(b, e->as.resume_.resume->k);
            buf_puts(b, " ");
            expr_print(b, e->as.resume_.resume->value);
            buf_putc(b, ')');
            break;
        case EX_DISCONTINUE:
            buf_puts(b, "(discontinue ");
            expr_print(b, e->as.discontinue_.discontinue->k);
            buf_puts(b, " ");
            expr_print(b, e->as.discontinue_.discontinue->exception);
            buf_putc(b, ')');
            break;
        case EX_MAKE_STRUCT:
            buf_puts(b, "<make-struct>");
            break;
        case EX_GET_FIELD:
            buf_puts(b, "<get-field>");
            break;
    }
}
