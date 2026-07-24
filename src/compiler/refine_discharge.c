/* refine_discharge.c -- RT3: the discharge pass.
 *
 * Owns the ordered backend array and the fall-through loop.  Backends run in
 * ascending cost and the loop stops at the first non-Unknown verdict:
 *
 *   S0 (normalize/trivial) -> S1 (EUF) -> S2 (arithmetic) -> S3 (N-O combine)
 *      -> [dev-only Z3 scaffold] -> RT_UNKNOWN => keep the runtime check
 *
 * In every default and release build the chain is the in-house stages only --
 * Z3 is not linked, so an obligation no stage can decide falls straight to its
 * runtime contract check.  Adding a stage only ever moves obligations LEFT in
 * the chain; it never changes an answer, because every stage preserves the
 * one-directional soundness invariant.
 *
 * See docs/upcoming/v1/refinement-types-plan.md (phase RT3). */

#include "refine_discharge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "fmt.h"
#include "refine_smtlib.h"
#include "refine_solver.h"
#include "runtime/globals.h"

/* ------------------------------------------------------------------------- *
 * Stats
 * ------------------------------------------------------------------------- */

static RefineStats g_stats;

const RefineStats *refine_stats(void) { return &g_stats; }
void refine_discharge_reset(void) { memset(&g_stats, 0, sizeof(g_stats)); }

static bool stats_enabled(void) {
    const char *s = getenv("TUR_REFINE_STATS");
    return s && s[0] == '1';
}

static bool dump_enabled(void) {
    const char *s = getenv("TUR_REFINE_DUMP");
    return s && s[0] == '1';
}

/* ------------------------------------------------------------------------- *
 * The chain
 * ------------------------------------------------------------------------- */

static const RefineBackend CHAIN[] = {
    refine_s0_decide,   /* trivial / syntactic          */
    refine_s1_decide,   /* congruence closure (EUF)     */
    refine_s2_decide,   /* linear arithmetic            */
    refine_s3_decide,   /* Nelson-Oppen combination     */
#ifdef TUR_REFINE_Z3_ORACLE
    /* DEV-ONLY transitional bootstrap.  The CMake option refuses Release and
     * WASM builds outright, so this link can never reach a shipped artifact.
     * Deleted wholesale once S0..S3 meet the retirement criteria. */
    refine_z3_decide,
#endif
};
#define CHAIN_LEN (sizeof(CHAIN) / sizeof(CHAIN[0]))

#ifdef TUR_REFINE_Z3_ORACLE
/* Oracle cross-check: an in-house stage claiming Valid where Z3 says Invalid
 * is a soundness bug.  Report it and downgrade to Unknown so the build stays
 * sound even while the bug is open. */
static RefineDecision oracle_crosscheck(RefineVC *vc, Arena *a,
                                        RefineDecision d, Span loc) {
    if (d.verdict != RT_VALID) return d;
    RefineDecision z = refine_z3_decide(vc, a);
    if (z.verdict == RT_INVALID) {
        diag_emit_with_code(DIAG_ERROR, loc, TUR_I0379_REFINE_ORACLE_MISMATCH,
                            "internal: in-house refinement stage proved an "
                            "obligation the Z3 oracle refutes; downgrading to "
                            "unknown (this is a solver soundness bug)");
        return refine_unknown();
    }
    return d;
}
#endif

/* ------------------------------------------------------------------------- *
 * Diagnostics
 * ------------------------------------------------------------------------- */

static void describe(const RefineObligation *ob, char *buf, size_t cap) {
    snprintf(buf, cap, "%s%s%s",
             ob->what ? ob->what : "value",
             ob->fn_name ? " of " : "",
             ob->fn_name ? ob->fn_name : "");
}

static void emit_model_note(const RefineObligation *ob) {
    if (!ob->counterex || ob->counterex->n == 0) return;
    char buf[256]; size_t off = 0;
    for (uint32_t i = 0; i < ob->counterex->n && off + 1 < sizeof(buf); i++) {
        const RefineModelBinding *b = &ob->counterex->bindings[i];
        int k = b->is_real
              ? snprintf(buf + off, sizeof(buf) - off, "%s%s = %g",
                         i ? ", " : "", b->name, b->rval)
              : snprintf(buf + off, sizeof(buf) - off, "%s%s = %lld",
                         i ? ", " : "", b->name, (long long)b->ival);
        if (k < 0) break;
        off += (size_t)k;
    }
    diag_emit(DIAG_NOTE, ob->loc, "counterexample: %s", buf);
}

/* ------------------------------------------------------------------------- *
 * Discharge
 * ------------------------------------------------------------------------- */

bool refine_discharge_one(RefineObligation *ob, Arena *a) {
    if (!ob) return false;
    if (ob->discharged) return ob->proven;
    ob->discharged = true;
    g_stats.collected++;

    char what[128];
    describe(ob, what, sizeof(what));

    const char *reason = NULL;
    RefineVC *vc = refine_vc_build(ob, a, &reason);
    ob->vc = vc;

    if (!vc) {
        g_stats.unknown++;
        diag_emit_with_code(g_strict_refine ? DIAG_ERROR : DIAG_WARNING, ob->loc,
                            TUR_W0372_REFINE_UNKNOWN,
                            "refinement predicate on the %s could not be decided "
                            "statically (%s); runtime check kept",
                            what, reason ? reason : "outside the supported fragment");
        return false;
    }

    if (vc->has_nonlinear) {
        /* Name the SOURCE subterm, not the internal symbol we abstracted it
         * to -- "(* x y)" is actionable, "__nl_mul_i" is not. */
        char sub[160] = "<nonlinear subterm>";
        for (uint32_t i = 0; i < vc->n_ufuncs; i++) {
            if (!vc->ufuncs[i].nonlinear) continue;
            snprintf(sub, sizeof(sub), "%s", vc->ufuncs[i].name);
            const Form *origin = vc->ufuncs[i].origin;
            if (origin) {
                Buf fb; buf_init(&fb);
                FmtOptions fo; memset(&fo, 0, sizeof(fo));
                fo.indent_width = 2; fo.line_width = 120;
                Form *one = (Form *)origin;
                if (fmt_print(&fb, &one, 1, fo) == 0 && fb.data && fb.len) {
                    size_t n = fb.len < sizeof(sub) - 1 ? fb.len : sizeof(sub) - 1;
                    while (n && (fb.data[n - 1] == '\n' || fb.data[n - 1] == ' ')) n--;
                    memcpy(sub, fb.data, n);
                    sub[n] = '\0';
                }
                buf_free(&fb);
            }
            break;
        }
        diag_emit_with_code(DIAG_WARNING, ob->loc, TUR_W0373_REFINE_NONLINEAR,
                            "non-linear predicate subterm '%s' treated as "
                            "uninterpreted; arithmetic reasoning is incomplete "
                            "for it", sub);
    }

    if (dump_enabled()) {
        Buf b; buf_init(&b);
        refine_smtlib_emit(vc, &b);
        fprintf(stderr, "--- refinement VC (%s) ---\n", what);
        if (b.data && b.len) fwrite(b.data, 1, b.len, stderr);  /* Buf is not NUL-terminated */
        buf_free(&b);
    }

    RefineDecision d = refine_unknown();
    for (size_t i = 0; i < CHAIN_LEN; i++) {
        g_stats.backend_calls++;
        d = CHAIN[i](vc, a);
#ifdef TUR_REFINE_Z3_ORACLE
        if (CHAIN[i] != refine_z3_decide) d = oracle_crosscheck(vc, a, d, ob->loc);
#endif
        if (d.verdict != RT_UNKNOWN) break;
    }

    /* Nothing proved it.  Before settling for "unknown", try to REFUTE it: a
     * concrete satisfying assignment for `hyps AND (not goal)` is a genuine
     * counterexample and turns a vague W0372 into an actionable E0371 with a
     * model.  Finding none leaves the verdict exactly where it was. */
    if (d.verdict == RT_UNKNOWN) {
        RefineModel *m = refine_model_search(vc, a);
        if (m) d = refine_invalid(m);
    }

    switch (d.verdict) {
        case RT_VALID:
            ob->proven = true;
            g_stats.proven++;
            return true;

        case RT_INVALID:
            ob->counterex = d.model;
            g_stats.invalid++;
            diag_emit_with_code(DIAG_ERROR, ob->loc, TUR_E0371_REFINE_NOT_PROVED,
                                "refinement predicate on the %s cannot be proved "
                                "statically", what);
            emit_model_note(ob);
            return false;

        default:
            g_stats.unknown++;
            diag_emit_with_code(g_strict_refine ? DIAG_ERROR : DIAG_WARNING, ob->loc,
                                TUR_W0372_REFINE_UNKNOWN,
                                "solver returned unknown for the refinement "
                                "predicate on the %s; runtime check kept", what);
            return false;
    }
}

void refine_discharge_all(RefineObligationVec *v, Arena *a) {
    if (!v) return;
    for (uint32_t i = 0; i < v->n; i++) refine_discharge_one(v->obs[i], a);
    if (stats_enabled()) {
        fprintf(stderr,
                "refine: %u obligation(s): %u proven, %u refuted, %u unknown "
                "(%u backend call(s))\n",
                g_stats.collected, g_stats.proven, g_stats.invalid,
                g_stats.unknown, g_stats.backend_calls);
    }
}
