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
 * - Phase P19-7: Effect handler capture checking (borrow types may not escape
 *   into handler case bodies, which are emitted as top-level C static functions)
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

    /* Phase P19-7: When non-NULL, we are inside an effect handler case body.
     * Variables with borrow types (&T / &mut T) that are NOT local to this
     * case (i.e., not one of its param_bindings or k_binding) must not be
     * referenced, because handler case bodies are emitted as top-level C
     * static functions with no access to the enclosing stack frame. */
    const HandleCase *handler_case;

    /* When true, borrow_check_var skips the use-after-move check and only
     * performs the handler-capture check.  Used by the always-on
     * borrow_check_effect_handler_captures() entry point. */
    bool only_handler_captures;
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

/* Phase P19-7: Always-on check that borrow-typed variables from the enclosing
 * scope are not referenced inside effect handler case bodies.  This is
 * independent of borrow_check_set_enabled(). */
bool borrow_check_effect_handler_captures(const Expr *program);

/* TY4: always-on lifetime pass -- runs elision per top-level function and
 * rejects cyclic outlives-constraint graphs (TUR-E0106). */
bool lifetime_check_program(const Expr *program);

#endif
