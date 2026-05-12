/* kind_check.h — Kind inference and validation pass (Phase HKT H1).
 *
 * This pass runs after elaboration and before effect-lowering.
 *
 * In Phase H1, the pass performs:
 *   1. Kind annotation propagation for typeclass type parameters.
 *   2. Belt-and-suspenders kind validation for definstance type arguments
 *      (the same check performed by elab_definstance, here as a safety net).
 *
 * Future phases will add full kind inference for higher-kinded types.
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
 * Returns 0 on success, 1 if any diagnostic error was emitted.
 */
int kind_check_pass(Arena *a, Expr *program);

#endif /* TUR_KIND_CHECK_H */
