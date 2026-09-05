/* refine_solver_s0.c -- S0: normalize + trivial discharge.
 *
 * No real solver.  Reflexivity, constant folding (already done at intern time
 * by vc_mk), syntactic entailment, and per-cube literal-level refutation.
 * This discharges a startling fraction of demo-grade obligations: `0 <= 3`, a
 * goal syntactically present among the hypotheses, `b != 0` after a literal
 * guard.  Every later stage runs over the same normalized cube set.
 *
 * Effort: days.  Coverage: everything that needs no reasoning at all. */

#include "refine_solver.h"

#include <string.h>

/* True when `t` occurs (structurally -- hash-consing makes that pointer
 * equality) among the hypotheses, either directly or as a conjunct. */
static bool hyp_contains(RefineVC *vc, VCTerm *t) {
    for (uint32_t i = 0; i < vc->n_hyps; i++) {
        VCTerm *h = vc->hyps[i];
        if (h == t) return true;
        if (h->op == VC_AND) {
            for (uint32_t j = 0; j < h->n; j++) if (h->kids[j] == t) return true;
        }
    }
    return false;
}

/* Literal-level refutation of one cube:
 *   - a `false` literal
 *   - complementary literals L and (not L)
 *   - an ordering literal over identical terms that cannot hold (x < x)
 * Constant-only atoms never reach here: vc_mk folded them at intern time. */
static bool cube_trivially_unsat(const VCCube *c) {
    for (uint32_t i = 0; i < c->n; i++) {
        VCTerm *l = c->lits[i];
        if (l->op == VC_FALSE) return true;
        if (l->op == VC_NOT && l->kids[0]->op == VC_TRUE) return true;
        for (uint32_t j = i + 1; j < c->n; j++) {
            VCTerm *m = c->lits[j];
            if (refine_lit_atom(l) == refine_lit_atom(m) &&
                refine_lit_is_neg(l) != refine_lit_is_neg(m))
                return true;
        }
    }
    return false;
}

RefineDecision refine_s0_decide_cc(RefineVC *vc, Arena *a, RefineCubeCache *cc) {
    if (!vc || !vc->goal) return refine_unknown();

    /* Folded goal: `(< 0 3)` interned straight to VC_TRUE. */
    if (vc->goal->op == VC_TRUE) return refine_valid();

    /* Syntactic entailment: the goal is literally one of the hypotheses. */
    if (hyp_contains(vc, vc->goal)) return refine_valid();
    if (vc->goal->op == VC_AND) {
        bool all = true;
        for (uint32_t i = 0; i < vc->goal->n; i++)
            if (!hyp_contains(vc, vc->goal->kids[i])) { all = false; break; }
        if (all) return refine_valid();
    }

    /* Contradictory hypotheses entail anything (ex falso). */
    for (uint32_t i = 0; i < vc->n_hyps; i++)
        if (vc->hyps[i]->op == VC_FALSE) return refine_valid();

    const VCCubeSet *cs;
    if (!refine_cubes_get(vc, a, cc, &cs)) return refine_unknown();
    if (cs->trivial) return refine_valid();
    if (cs->n == 0)  return refine_valid();   /* every branch pruned as unsat */

    for (uint32_t i = 0; i < cs->n; i++)
        if (!cube_trivially_unsat(&cs->cubes[i])) return refine_unknown();
    return refine_valid();
}

/* refine-chain-expands-the-same-dnf-four-times: the original entry point, kept
 * so every caller outside the chain driver (`tur smt`, tests/unit, the SX8
 * doors) is unchanged -- a fresh cache means it builds exactly once, as it
 * always did. */
RefineDecision refine_s0_decide(RefineVC *vc, Arena *a) {
    RefineCubeCache cc = {0};
    return refine_s0_decide_cc(vc, a, &cc);
}
