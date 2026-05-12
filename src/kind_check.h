/* kind_check.h — Kind inference pass (Phase HKT H0).
 *
 * This pass runs after elaboration and before effect-lowering.
 * In v1, all types are KIND_STAR, so the pass is a no-op that simply
 * returns success.  The infrastructure is in place for Phase H1, which
 * will add full kind inference for higher-kinded types.
 *
 * Pipeline position:
 *   PASS_ELABORATE → PASS_KIND_CHECK → PASS_EFFECT_LOWER → ...
 */

#ifndef TUR_KIND_CHECK_H
#define TUR_KIND_CHECK_H

#include "arena.h"
#include "expr.h"

/* Run the kind-check pass over the elaborated program.
 *
 * In v1 this is a no-op that returns 0 immediately; it exists to reserve the
 * PASS_KIND_CHECK slot in the pipeline for Phase H1 kind inference.
 *
 * Returns 0 on success, 1 if any diagnostic error was emitted.
 */
int kind_check_pass(Arena *a, Expr *program);

#endif /* TUR_KIND_CHECK_H */
