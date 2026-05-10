#include "cps.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "diag.h"
#include "expr.h"

/* Phase 18: CPS transformation for delimited continuations
 * 
 * This pass transforms functions containing shift/shift0 into CPS form.
 * The transformation converts direct-style functions into continuation-passing style,
 * where every function call becomes a tail call into a continuation.
 * 
 * Key concepts:
 * - A function "needs CPS" if it transitively contains shift or shift0.
 * - In CPS form, functions take an additional continuation parameter.
 * - shift captures the current continuation and passes it to its function argument.
 * - reset establishes a new continuation boundary.
 * - Continuations are one-shot (move-only).
 */

/* Check if an expression contains shift or shift0 */
bool cps_expr_contains_shift(const Expr *e) {
    if (!e) return false;
    
    switch (e->kind) {
        case EX_RESET:
        case EX_SHIFT:
        case EX_SHIFT0:
            return true;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (cps_expr_contains_shift(e->as.let_.bindings[i].init)) return true;
            }
            return cps_expr_contains_shift(e->as.let_.body);
        case EX_IF:
            if (cps_expr_contains_shift(e->as.if_.cond)) return true;
            if (cps_expr_contains_shift(e->as.if_.then_)) return true;
            if (e->as.if_.else_or_null && cps_expr_contains_shift(e->as.if_.else_or_null)) return true;
            return false;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (cps_expr_contains_shift(e->as.do_.items[i])) return true;
            }
            return false;
        case EX_WHILE:
            return cps_expr_contains_shift(e->as.while_.cond) || 
                   cps_expr_contains_shift(e->as.while_.body);
        case EX_SET:
            return cps_expr_contains_shift(e->as.set_.value);
        case EX_DEF:
            return e->as.def_.init && cps_expr_contains_shift(e->as.def_.init);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++) {
                if (cps_expr_contains_shift(e->as.builtin.args[i])) return true;
            }
            return false;
        case EX_FN_DEF:
            return cps_fn_needs_transform(e->as.fn_def_.fn);
        case EX_FN:
            return cps_fn_needs_transform(e->as.fn_.fn);
        case EX_CALL:
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                if (cps_expr_contains_shift(e->as.call_.args[i])) return true;
            }
            return false;
        case EX_CLOSURE:
            /* Check if the closure contains shift */
            return cps_fn_needs_transform(e->as.closure_.closure->fn);
        case EX_DEFER:
            return cps_expr_contains_shift(e->as.defer_.body);
        case EX_RETURN:
            return e->as.return_.value && cps_expr_contains_shift(e->as.return_.value);
        case EX_THROW:
            return cps_expr_contains_shift(e->as.throw_.payload);
        case EX_TRY:
            if (cps_expr_contains_shift(e->as.try_.body)) return true;
            for (uint8_t i = 0; i < e->as.try_.n_clauses; i++) {
                if (cps_expr_contains_shift(e->as.try_.clauses[i].handler)) return true;
            }
            return e->as.try_.finally_body && cps_expr_contains_shift(e->as.try_.finally_body);
        default:
            return false;
    }
}

/* Check if a function definition needs CPS transformation */
bool cps_fn_needs_transform(const FnDef *fd) {
    if (!fd) return false;
    return cps_expr_contains_shift(fd->body);
}

/* For now, this is a stub implementation.
 * The full CPS transformation is complex and will be implemented incrementally.
 */
Expr *cps_transform(Arena *a, Expr *program) {
    if (!program || program->kind != EX_PROGRAM) {
        fprintf(stderr, "cps: expected EX_PROGRAM\n");
        return NULL;
    }
    
    /* For Phase 18 v1, we emit placeholders for shift/reset/continuations.
     * The full CPS transformation will be implemented in a future pass.
     * For now, just return the program as-is (it will use the placeholder
     * emit code in emit.c). */
    return program;
}
