/* kind_check.c — Kind inference pass (Phase HKT H0).
 *
 * v1 stub: All types are KIND_STAR in v1, so no kind mismatches are possible.
 * This file provides the PASS_KIND_CHECK slot in the pipeline for Phase H1.
 *
 * Phase H1 will add actual kind inference here:
 *   - Walk all type constructor definitions and infer their kinds.
 *   - Assign KIND_ARROW(* -> *) to one-parameter type constructors
 *     (option, result, vec, etc.).
 *   - Validate that type arguments are applied at the correct kind.
 */

#include "kind_check.h"

int kind_check_pass(Arena *a, Expr *program) {
    (void)a;
    (void)program;
    /* Phase HKT H0: No-op in v1.  Full kind inference added in Phase H1. */
    return 0;
}
