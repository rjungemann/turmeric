#ifndef TUR_CPS_H
#define TUR_CPS_H

#include "expr.h"
#include "arena.h"

/* Phase 18: CPS transformation for delimited continuations */

/* Transform a program to CPS form.
 * Returns the transformed program, or NULL on error. */
Expr *cps_transform(Arena *a, Expr *program);

/* Check if an expression contains shift or shift0 */
bool cps_expr_contains_shift(const Expr *e);

/* Check if a function definition needs CPS transformation */
bool cps_fn_needs_transform(const FnDef *fd);

#endif
