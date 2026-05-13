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

/* CPS-CL11: For each EX_CLONEABLE_SHIFT in `program`, resolve the per-capture
 * Clone instance method and record its C name into the node's
 * capture_clone_fns[] array (parallel to live_captures[]).  drop fns are
 * recorded in capture_drop_fns[] when a Drop instance exists (currently
 * always NULL until a Drop typeclass is introduced).
 *
 * `tc_env` may be NULL, in which case nothing is recorded (emit.c falls back
 * to bitwise copy + free).  Bindings whose type has no Clone instance get a
 * NULL slot — CPS-CL10 (cps_check_cloneable_captures) will already have
 * emitted TUR-E0014 for those.  This pass is run from cps_transform after
 * liveness and capture-checking, before emit.c. */
void cps_emit_capture_environment(Arena *a, Expr *program, TypeClassEnv *tc_env);

/* Phase B5: --dump-clone-plan: walk the program and print a summary of every
 * EX_CLONEABLE_SHIFT site — its location and the clone function selected for
 * each captured binding (NULL = bitwise copy fallback). */
void cps_dump_clone_plan(const Expr *program, FILE *out);

#endif
