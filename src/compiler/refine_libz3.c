/* refine_libz3.c -- RT3: the Z3 SCAFFOLD backend.
 *
 * THIS FILE IS SCAFFOLDING, NOT A BACKEND WE SHIP.  It compiles only under
 * -DTUR_REFINE_Z3_ORACLE (off by default; the CMake option refuses Release and
 * WASM builds outright), links a SYSTEM-installed Z3 only -- never fetched,
 * never bundled, never statically linked into a distributable -- and serves
 * exactly two development-time roles:
 *
 *   1. CORRECTNESS ORACLE.  Run both an in-house stage and Z3 on every
 *      obligation and assert agreement; an in-house `Valid` where Z3 says
 *      `Invalid` is a soundness bug (TUR-I0379, downgraded to Unknown so the
 *      build stays sound).
 *   2. TRANSITIONAL BOOTSTRAP.  Chain tail, so the end-to-end pipeline
 *      (collector -> VC -> discharge -> check elision) was testable before the
 *      in-house stages existed.
 *
 * Both roles end once S0..S3 meet the retirement criteria in the plan, at
 * which point this file, the find_package(Z3) block, and the CMake option are
 * DELETED.  Nothing in the shipped compiler ever referenced them. */

#ifdef TUR_REFINE_Z3_ORACLE

#include "refine_smtlib.h"
#include "refine_solver.h"

#include <stdlib.h>
#include <string.h>

#include <z3.h>

/* One context per process; Z3_eval_smtlib2_string is self-contained per query
 * (each call carries its own declarations), so no push/pop bookkeeping is
 * needed on top of it. */
static Z3_context g_z3_ctx;

static void z3_err(Z3_context c, Z3_error_code e) { (void)c; (void)e; }

static Z3_context z3_ctx(void) {
    if (!g_z3_ctx) {
        Z3_config cfg = Z3_mk_config();
        Z3_set_param_value(cfg, "model", "true");
        g_z3_ctx = Z3_mk_context(cfg);
        Z3_del_config(cfg);
        Z3_set_error_handler(g_z3_ctx, z3_err);
    }
    return g_z3_ctx;
}

RefineDecision refine_z3_decide(RefineVC *vc, Arena *a) {
    (void)a;
    if (!vc || !vc->goal) return refine_unknown();

    Buf b; buf_init(&b);
    refine_smtlib_emit(vc, &b);
    buf_putc(&b, '\0');   /* Buf is not NUL-terminated; Z3 wants a C string */
    if (!b.data) { buf_free(&b); return refine_unknown(); }

    Z3_context c = z3_ctx();
    Z3_string res = Z3_eval_smtlib2_string(c, b.data);
    buf_free(&b);
    if (!res) return refine_unknown();

    /* `unsat` on the negated goal means the goal is entailed. */
    if (strstr(res, "unsat")) return refine_valid();
    if (strncmp(res, "sat", 3) == 0) return refine_invalid(NULL);
    return refine_unknown();
}

#else

/* ISO C forbids an empty translation unit, and this file compiles to nothing
 * in every build that is not the dev oracle -- which is every shipped build. */
typedef int tur_refine_libz3_not_built;

#endif /* TUR_REFINE_Z3_ORACLE */
