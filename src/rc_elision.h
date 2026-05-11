#ifndef TUR_RC_ELISION_H
#define TUR_RC_ELISION_H

#include "expr.h"

/*
 * Phase 9 follow-up: SSA-like last-use elision for RC clone/drop pairs.
 *
 * Analysis model (safety model from deferred-prerequisite-decisions.md §9.6):
 *   - Elide retain/release only when single-use proof is available.
 *   - Do NOT elide across barriers: EX_CALL, EX_BUILTIN, EX_WHILE, EX_THROW,
 *     EX_TRY, EX_CLOSURE, or EX_DEFER nodes in prior statements.
 *   - Prefer conservative false negatives over unsound elision.
 *
 * An EX_RC_CLONE binding is eligible when:
 *   (a) It is bound in an EX_LET (not an inline/temporary position).
 *   (b) The binding is referenced exactly once in the let body.
 *   (c) No barrier expression appears in statements before the single use
 *       when the let body is a do-block. (If the body is a single expression,
 *       there are no prior statements and no barrier is possible.)
 *
 * When eligibility is confirmed:
 *   - The EX_RC_CLONE node is marked elide=true  → emitter skips rc_strong_increment.
 *   - Any matching EX_RC_DROP of the same binding in the let body is also marked
 *     elide=true → emitter skips rc_strong_decrement + rc_free_queue_drain.
 *
 * Call rc_elision_analyze_fn() once per function body before emitting.
 */
void rc_elision_analyze_fn(Expr *fn_body);

#endif /* TUR_RC_ELISION_H */
