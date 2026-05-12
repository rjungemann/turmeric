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

#include <stdio.h>
#include "arena.h"
#include "expr.h"

/* Run the kind-check pass over the elaborated program.
 *
 * Returns 0 on success, 1 if any diagnostic error was emitted.
 */
int kind_check_pass(Arena *a, Expr *program);

/* Phase HKT-P1: Infer the result kind of a type application (type-app fn arg).
 *
 * fn_type must have hkt_kind == KIND_ARROW or KIND_ARROW2:
 *   KIND_ARROW  (fn : * -> *)   applied to one arg → result kind KIND_STAR
 *   KIND_ARROW2 (fn : * -> * -> *) applied to one arg → result kind KIND_ARROW
 *
 * Emits TUR_E0012_KIND_MISMATCH and returns KIND_STAR if fn_type has KIND_STAR
 * (cannot apply a concrete type).
 *
 * 'span' is used for diagnostics.
 */
Kind kind_of_type_app(Type fn_type, Type arg_type, Span span);

/* Phase HKT-P6: Walk the elaborated AST and print, to `out`, every
 * defclass / definstance / defn that carries a non-KIND_STAR kind
 * annotation.  Useful for debugging the kind-inference pipeline with
 * the --dump-kinds flag. */
void kind_dump_program(Expr *program, FILE *out);

#endif /* TUR_KIND_CHECK_H */
