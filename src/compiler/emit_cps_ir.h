#ifndef TUR_EMIT_CPS_IR_H
#define TUR_EMIT_CPS_IR_H

#include <stdbool.h>

#include "emit_internal.h"

/* =========================================================================
 * CPS-IR-to-C backend (cps-ir-to-c-backend-plan, Phase C1).
 *
 * Lowers a colored function's ANF/CPS IR (a CTerm, src/passes/cps_ir.h) to C
 * under the DK-threading ABI ratified in Phase C0:
 *
 *   colored `f a b`  ->  int64_t f__cps(int64_t a, int64_t b, DK *k)
 *
 * plus a direct-style entry wrapper `f(a, b)` that seeds a dk_done()-terminated
 * root continuation so uncolored/direct callers reach the CPS body unchanged.
 *
 * Fallback-guarded and whole-function granular (plan "Scope and guiding
 * constraint"): a colored function is emitted here ONLY when its entire CTerm
 * lies in the C1 emittable subset -- the non-delimited control/data core
 * (CT_APPCONT, CT_LETVAL, CT_LETPRIM, CT_LETCALL, CT_TAILCALL, CT_LETCONT,
 * CT_IF), scalar (int/bool) types, and no join point that must be reified onto
 * the heap chain (a non-tail cps->cps call).  Everything else keeps its
 * existing direct-style emission.  Coverage grows monotonically as later phases
 * close the gaps.
 *
 * The whole backend is gated behind --enable=cps-backend (g_opt_cps_backend);
 * off by default, so the fixture suite is untouched until a fixture opts in.
 * ========================================================================= */

/* True if `program` has at least one colored function the C1 backend will emit.
 * Self-contained (colors the program and computes the emittable set with its
 * own arena); used to gate the DK runtime prelude.  Returns false when the
 * experiment is off. */
bool emit_cps_ir_program_has_emittable(const Expr *program);

/* If `fn_def_expr`'s FnDef is colored and lies in the C1 emittable subset, emit
 * its CPS body + direct-entry wrapper into `file` and return true.  Otherwise
 * emit nothing and return false, so the caller falls back to the direct-style
 * path (emit_fn_def).  A no-op returning false when the experiment is off. */
bool emit_cps_ir_try_fn(EmitCtx *ctx, Buf *file, const Expr *fn_def_expr);

#endif
