#ifndef TUR_BORROW_CHECK_H
#define TUR_BORROW_CHECK_H

#include "expr.h"
#include "lifetimes.h"

/* Borrow checker pass - validates borrow relationships within and across functions.
 * 
 * This module combines:
 * - Phase 11: Move tracking (use-after-move detection)
 * - Phase 12: Aliasing rules (N readers XOR 1 writer)
 * - Phase 13: Lifetime annotations (inter-procedural validation)
 * 
 * The borrow checker runs as a pass after elaboration and before closure conversion.
 */

/* Borrow checking context for the current function */
typedef struct BorrowCheckCtx {
    /* Current function's lifetime context */
    const LifetimeContext *lifetime_ctx;
    
    /* Current scope for tracking active borrows */
    const struct Scope *scope;
    
    /* Error reporting */
    int error_count;
} BorrowCheckCtx;

/* Initialize borrow check context */
void borrow_check_ctx_init(BorrowCheckCtx *ctx, const LifetimeContext *lifetime_ctx, const struct Scope *scope);

/* Run borrow checker on an expression tree */
bool borrow_check_expr(BorrowCheckCtx *ctx, const Expr *e);

/* Run borrow checker on a function definition */
bool borrow_check_fn(BorrowCheckCtx *ctx, const FnDef *fn);

/* Validate that a function signature is well-formed for borrow checking */
bool borrow_check_fn_signature(const FnDef *fn);

/* Validate return type against input lifetimes (prevent dangling references) */
bool borrow_check_return_type(const FnDef *fn);

/* Run borrow checker on an entire program (EX_PROGRAM) */
bool borrow_check_program(const Expr *program);

/* Phase 14: Enable/disable borrow checking (for incremental rollout) */
void borrow_check_set_enabled(bool enabled);
bool borrow_check_is_enabled(void);

#endif
