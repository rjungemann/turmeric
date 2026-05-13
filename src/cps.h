#ifndef TUR_CPS_H
#define TUR_CPS_H

#include "expr.h"
#include "arena.h"
#include "typeclass.h"

/* Phase 18: CPS transformation for delimited continuations */

/* Transform a program to CPS form.
 * Returns the transformed program, or NULL on error.
 * `tc_env` (CPS-CL10): if non-NULL, captured bindings at each cloneable-shift
 * site are checked for a Clone typeclass instance; missing instances are
 * diagnosed as TUR-E0014. */
Expr *cps_transform(Arena *a, Expr *program, TypeClassEnv *tc_env);

/* Check if an expression contains shift or shift0 */
bool cps_expr_contains_shift(const Expr *e);

/* Check if a function definition needs (one-shot) CPS transformation */
bool cps_fn_needs_transform(const FnDef *fd);

/* Phase B2: Cloneable CPS pass */

/* Check if an expression contains cloneable-shift or cloneable-reset.
 * Functions containing these forms need a cloneable CPS transformation
 * (where the captured environment must be deep-cloneable via Clone). */
bool cps_expr_contains_cloneable_shift(const Expr *e);

/* Check if a function definition needs cloneable CPS transformation.
 * Returns true if the function body contains EX_CLONEABLE_SHIFT or
 * EX_CLONEABLE_RESET anywhere in its call tree. */
bool cps_fn_needs_cloneable_transform(const FnDef *fd);

#endif
