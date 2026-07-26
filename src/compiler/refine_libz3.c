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

/* A FRESH context per query.
 *
 * The original code kept one context for the whole process, on the assumption
 * that Z3_eval_smtlib2_string is self-contained because each call carries its
 * own declarations.  It is not: assertions accumulate on the context across
 * calls.  One self-contradictory query -- and a trivially-valid obligation is
 * exactly that, since we assert the hypotheses AND the negated goal -- poisons
 * the context, after which every later query returns `unsat` and the oracle
 * cheerfully answers VALID to everything.
 *
 * As the chain tail that would prove false obligations; as the cross-check it
 * would silently rubber-stamp the in-house stages, which is worse, because the
 * whole point of the oracle is to disagree when we are wrong.  The bug was
 * invisible until the scaffold was actually built and run against a real Z3.
 *
 * A fresh context per obligation is the obviously-correct fix; an oracle build
 * is a development harness, so the allocation cost is irrelevant. */
static void z3_err(Z3_context c, Z3_error_code e) { (void)c; (void)e; }

RefineDecision refine_z3_decide(RefineVC *vc, Arena *a) {
    (void)a;
    if (!vc || !vc->goal) return refine_unknown();

    Buf b; buf_init(&b);
    refine_smtlib_emit(vc, &b);
    buf_putc(&b, '\0');   /* Buf is not NUL-terminated; Z3 wants a C string */
    if (!b.data) { buf_free(&b); return refine_unknown(); }

    Z3_config cfg = Z3_mk_config();
    Z3_set_param_value(cfg, "model", "true");
    Z3_context c = Z3_mk_context(cfg);
    Z3_del_config(cfg);
    Z3_set_error_handler(c, z3_err);

    Z3_string res = Z3_eval_smtlib2_string(c, b.data);
    RefineDecision d = refine_unknown();
    if (res) {
        if (strstr(res, "unsat")) {
            /* Sound in both directions: the VC abstracts nonlinear and measure
             * terms to uninterpreted functions, and any model of the CONCRETE
             * obligation is also a model of the abstraction, so unsat of the
             * abstraction implies unsat of the concrete. */
            d = refine_valid();
        } else if (strncmp(res, "sat", 3) == 0) {
            /* NOT sound in this direction once anything was abstracted.  A
             * model that assigns an uninterpreted symbol some convenient value
             * says nothing about the real function it stands for:
             * `x>0, y>0 |- x*y>0` is TRUE, but its abstraction (with `x*y` an
             * opaque symbol) is satisfiable, and reporting that as a
             * counterexample would flag correct code as broken.  Only a VC
             * with no abstracted symbols can be refuted this way -- the same
             * rule refine_model_search already follows. */
            d = (vc->n_ufuncs == 0) ? refine_invalid(NULL) : refine_unknown();
        }
    }
    Z3_del_context(c);
    buf_free(&b);
    return d;
}

#else

/* ISO C forbids an empty translation unit, and this file compiles to nothing
 * in every build that is not the dev oracle -- which is every shipped build. */
typedef int tur_refine_libz3_not_built;

#endif /* TUR_REFINE_Z3_ORACLE */
