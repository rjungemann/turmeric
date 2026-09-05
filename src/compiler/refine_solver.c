/* refine_solver.c -- shared machinery for the in-house stages: NNF + the
 * small-DNF cube expansion every theory solver runs over.
 *
 * To prove `hyps |= goal` we refute `hyps AND (not goal)`.  Cube expansion is
 * the "naive S4" the plan sanctions: explicit annotations keep the
 * propositional structure of a real obligation tiny, so small-DNF is enough
 * and we never build a DPLL(T) engine.  Every cap here degrades to
 * RT_UNKNOWN -> runtime check, which is always sound. */

#include "refine_solver.h"

#include <string.h>

/* ------------------------------------------------------------------------- *
 * Cap telemetry (see refine_solver.h)
 * ------------------------------------------------------------------------- */

static RefineCapStats g_caps;

RefineCapStats *refine_caps(void) { return &g_caps; }
void refine_caps_reset(void) { memset(&g_caps, 0, sizeof(g_caps)); }

bool refine_caps_any(void) {
    return g_caps.cubes_hits || g_caps.cube_lits_hits || g_caps.expand_depth_hits ||
           g_caps.la_vars_hits || g_caps.la_constr_hits || g_caps.la_fm_hits ||
           g_caps.euf_terms_hits || g_caps.no_shared_hits || g_caps.no_rounds_hits ||
           g_caps.path_hyps_hits || g_caps.model_vars_hits;
}

/* ------------------------------------------------------------------------- *
 * NNF
 * ------------------------------------------------------------------------- */

/* Push negations to the leaves.  vc_mk already folds (not (not p)) and the
 * constants, so this only has to handle and/or. */
static VCTerm *nnf(RefineVC *vc, VCTerm *t, bool neg) {
    if (!t) return NULL;
    switch (t->op) {
        case VC_NOT:
            return nnf(vc, t->kids[0], !neg);
        case VC_AND:
        case VC_OR: {
            VCOp op = t->op;
            if (neg) op = (op == VC_AND) ? VC_OR : VC_AND;   /* De Morgan */
            VCTerm **kids = (VCTerm **)arena_alloc(vc->arena, t->n * sizeof(VCTerm *));
            for (uint32_t i = 0; i < t->n; i++) kids[i] = nnf(vc, t->kids[i], neg);
            return vc_mk(vc, op, kids, t->n);
        }
        default:
            /* A DISEQUALITY IS A DISJUNCTION.  `a != b` over a totally ordered
             * sort is `a < b OR b < a`, and until it is written that way S2
             * cannot use it at all: `la_assert_cube` skips a negated VC_EQ
             * because a single linear constraint cannot express it.  Since the
             * goal is refuted by asserting its negation, that made an EQUALITY
             * GOAL unprovable whenever it needed any arithmetic -- and `(= r
             * <expr>)` is the most natural postcondition there is. `(- (+ x 1)
             * 1)` against `(= r x)` was Unknown for exactly this reason.
             *
             * The original literal is KEPT alongside the split, so EUF still
             * sees the disequality it was already using; the cubes only gain
             * information. That matters for soundness: we are refuting, so the
             * rewrite has to be an EQUIVALENCE, never a strengthening. It is
             * one -- the added disjunction is implied by the literal it joins.
             *
             * Each disequality doubles the cube count, which REFINE_MAX_CUBES
             * bounds; blowing the cap answers Unknown and keeps the runtime
             * check, as every cap here does. */
            if (neg && t->op == VC_EQ && t->n == 2 &&
                vc_is_arith(t->kids[0]) && vc_is_arith(t->kids[1])) {
                VCTerm *lt  = vc_mk2(vc, VC_LT, t->kids[0], t->kids[1]);
                VCTerm *gt  = vc_mk2(vc, VC_LT, t->kids[1], t->kids[0]);
                return vc_mk2(vc, VC_AND, vc_not(vc, t),
                              vc_mk2(vc, VC_OR, lt, gt));
            }
            return neg ? vc_not(vc, t) : t;
    }
}

/* ------------------------------------------------------------------------- *
 * DNF expansion into cubes
 * ------------------------------------------------------------------------- */

typedef struct {
    Arena     *arena;
    VCCube    *cubes;
    uint32_t   n, cap;
    uint32_t   depth;
    bool       overflow;
} CubeAcc;

static void acc_push(CubeAcc *acc, VCTerm **lits, uint32_t n) {
    if (acc->overflow) return;
    if (acc->n >= REFINE_MAX_CUBES) {
        g_caps.cubes_hits++;
        refine_cap_peak(&g_caps.cubes_peak, REFINE_MAX_CUBES);
        acc->overflow = true; return;
    }
    if (acc->n == acc->cap) {
        uint32_t ncap = acc->cap ? acc->cap * 2 : 8;
        VCCube *nc = (VCCube *)arena_alloc(acc->arena, ncap * sizeof(VCCube));
        if (acc->n) memcpy(nc, acc->cubes, acc->n * sizeof(VCCube));
        acc->cubes = nc; acc->cap = ncap;
    }
    VCTerm **copy = (VCTerm **)arena_alloc(acc->arena, (n ? n : 1) * sizeof(VCTerm *));
    if (n) memcpy(copy, lits, n * sizeof(VCTerm *));
    acc->cubes[acc->n].lits = copy;
    acc->cubes[acc->n].n    = n;
    acc->n++;
}

/* Depth-first product expansion.  `conj` holds the literals chosen so far;
 * `pending` is the remaining conjuncts to expand.  A `false` literal prunes
 * the branch (that cube is trivially unsat, so it does not need to be
 * recorded -- an unsat cube is exactly what we want anyway). */
static void expand(CubeAcc *acc, VCTerm **pending, uint32_t n_pending,
                   VCTerm **conj, uint32_t n_conj) {
    if (acc->overflow) return;
    if (acc->depth >= REFINE_MAX_EXPAND_DEPTH) {
        g_caps.expand_depth_hits++;
        refine_cap_peak(&g_caps.expand_depth_peak, REFINE_MAX_EXPAND_DEPTH);
        acc->overflow = true; return;
    }
    acc->depth++;
    refine_cap_peak(&g_caps.expand_depth_peak, acc->depth);

    /* FLATTEN conjunctions first, so the disjunction scan below sees every
     * disjunct that is top-level in the FORMULA rather than top-level in this
     * array.  A `(and A (or B C))` conjunct hides its `or` from a scan that
     * only inspects `pending[i]->op`, and the collect step below used to
     * handle that by re-entering with the same pending list -- which made the
     * identical decision on the next call and recursed forever.  A three-line
     * program with an ordinary disjunctive postcondition crashed the compiler
     * with a stack overflow.
     *
     * Splicing the conjunction's children in place is what makes each frame
     * PROGRESS: one boolean node is removed from the structure every time. */
    for (uint32_t i = 0; i < n_pending; i++) {
        if (pending[i]->op != VC_AND) continue;
        VCTerm *t = pending[i];
        uint32_t cap = n_pending - 1 + t->n;
        if (cap > REFINE_MAX_CUBE_LITS * 4) {
            g_caps.cube_lits_hits++;
            refine_cap_peak(&g_caps.cube_lits_peak, cap);
            acc->overflow = true; acc->depth--; return;
        }
        VCTerm **flat = (VCTerm **)arena_alloc(acc->arena,
                                               (cap ? cap : 1) * sizeof(VCTerm *));
        uint32_t m = 0;
        for (uint32_t j = 0; j < n_pending; j++) {
            if (j == i) for (uint32_t d = 0; d < t->n; d++) flat[m++] = t->kids[d];
            else        flat[m++] = pending[j];
        }
        expand(acc, flat, m, conj, n_conj);
        acc->depth--;
        return;
    }

    /* Find the first disjunction still to split on. */
    for (uint32_t i = 0; i < n_pending; i++) {
        VCTerm *t = pending[i];
        if (t->op != VC_OR) continue;
        VCTerm **rest = (VCTerm **)arena_alloc(acc->arena,
                                               (n_pending ? n_pending : 1) * sizeof(VCTerm *));
        uint32_t m = 0;
        for (uint32_t j = 0; j < n_pending; j++) if (j != i) rest[m++] = pending[j];
        for (uint32_t d = 0; d < t->n && !acc->overflow; d++) {
            VCTerm **nxt = (VCTerm **)arena_alloc(acc->arena, (m + 1) * sizeof(VCTerm *));
            if (m) memcpy(nxt, rest, m * sizeof(VCTerm *));
            nxt[m] = t->kids[d];
            expand(acc, nxt, m + 1, conj, n_conj);
        }
        acc->depth--;
        return;
    }

    /* No disjunctions left: everything pending is a literal (or a conjunction
     * already flattened by vc_mk).  Collect into one cube. */
    uint32_t total = n_conj + n_pending;
    refine_cap_peak(&g_caps.cube_lits_peak, total);
    if (total > REFINE_MAX_CUBE_LITS) {
        g_caps.cube_lits_hits++;
        acc->overflow = true; acc->depth--; return;
    }

    VCTerm **lits = (VCTerm **)arena_alloc(acc->arena, (total ? total : 1) * sizeof(VCTerm *));
    uint32_t k = 0;
    for (uint32_t i = 0; i < n_conj; i++) lits[k++] = conj[i];
    bool has_false = false;
    for (uint32_t i = 0; i < n_pending; i++) {
        VCTerm *x = pending[i];
        /* Both passes above ran to completion, so nothing here is an AND or an
         * OR.  If that ever stops holding, bail to Unknown rather than record
         * a boolean as though it were a literal -- a cube holding an
         * unsatisfied OR would read as more constrained than it is, and a
         * theory stage could then call it unsat and prove the goal. */
        if (x->op == VC_AND || x->op == VC_OR) { acc->overflow = true; acc->depth--; return; }
        if (x->op == VC_FALSE) { has_false = true; continue; }
        if (x->op == VC_TRUE)  continue;
        lits[k++] = x;
    }
    /* A cube containing `false` is already unsatisfiable, and validity needs
     * every cube to be unsatisfiable -- so dropping it is correct (recording
     * it as an empty cube would instead read as "no constraints", which is
     * satisfiable and would wrongly block the proof). */
    if (has_false) { acc->depth--; return; }
    acc_push(acc, lits, k);
    acc->depth--;
}

/* refine-chain-expands-the-same-dnf-four-times: build once per chain run.
 * See the RefineCubeCache comment in refine_solver.h for why this is a local
 * rather than a cache on the RefineVC, and why it is lazy. */
bool refine_cubes_get(RefineVC *vc, Arena *a, RefineCubeCache *cc,
                      const VCCubeSet **out) {
    if (!cc->tried) {
        cc->tried = true;
        cc->ok = refine_cubes_build(vc, a, &cc->cs);
    }
    *out = &cc->cs;
    return cc->ok;
}

bool refine_cubes_build(RefineVC *vc, Arena *a, VCCubeSet *out) {
    memset(out, 0, sizeof(*out));
    if (!vc || !vc->goal) { out->overflow = true; return false; }

    /* refutation formula: hyps AND (not goal) */
    uint32_t n = vc->n_hyps + 1;
    VCTerm **parts = (VCTerm **)arena_alloc(a, n * sizeof(VCTerm *));
    for (uint32_t i = 0; i < vc->n_hyps; i++) parts[i] = nnf(vc, vc->hyps[i], false);
    parts[vc->n_hyps] = nnf(vc, vc->goal, true);

    /* An immediate `false` anywhere means the refutation is trivially
     * unsatisfiable -- the goal is valid with no cube work at all. */
    for (uint32_t i = 0; i < n; i++) {
        if (!parts[i]) { out->overflow = true; return false; }
        if (parts[i]->op == VC_FALSE) { out->trivial = true; return true; }
    }

    CubeAcc acc; memset(&acc, 0, sizeof(acc));
    acc.arena = a;
    expand(&acc, parts, n, NULL, 0);
    if (acc.overflow) { out->overflow = true; return false; }
    refine_cap_peak(&g_caps.cubes_peak, acc.n);
    out->cubes = acc.cubes;
    out->n     = acc.n;
    return true;
}

/* ------------------------------------------------------------------------- *
 * Bounded counterexample search
 *
 * The proving stages only ever answer Valid or Unknown -- they refute the
 * negated goal, and failing to refute it proves nothing either way.  To say
 * INVALID we need an actual witness, so we build one the honest way: pick a
 * small candidate value set (the literals that appear in the VC, their
 * neighbours, and a handful of small integers), enumerate assignments, and
 * EVALUATE `hyps AND (not goal)` exactly.  An assignment that satisfies it is
 * a genuine counterexample -- no approximation is involved -- which is why
 * this is allowed to produce RT_INVALID.
 *
 * Scope is deliberately tiny: integer variables only, no uninterpreted
 * symbols (a measure has no fixed interpretation to evaluate), at most
 * MODEL_MAX_VARS of them.  Everything else simply finds nothing, and the
 * obligation stays Unknown -> runtime check.
 * ------------------------------------------------------------------------- */

/* MODEL_MAX_VARS lives in refine_solver.h beside the other capped quantities,
 * so the TUR_REFINE_STATS reporter can name the limit its peak is a peak of.
 * MODEL_MAX_CANDS stays here and is uninstrumented: it bounds the candidate
 * VALUE set rather than a structural quantity, and nothing has asked. */
#define MODEL_MAX_CANDS 16

typedef struct {
    const int64_t *vals;   /* one per VC variable */
    bool           fail;   /* evaluation hit overflow / an opaque term */
} EvalCtx;

static int64_t eval_arith(EvalCtx *E, const VCTerm *t);

static bool eval_bool(EvalCtx *E, const VCTerm *t) {
    if (E->fail) return false;
    switch (t->op) {
        case VC_TRUE:  return true;
        case VC_FALSE: return false;
        case VC_NOT:   return !eval_bool(E, t->kids[0]);
        case VC_AND:
            for (uint32_t i = 0; i < t->n; i++) if (!eval_bool(E, t->kids[i])) return false;
            return true;
        case VC_OR:
            for (uint32_t i = 0; i < t->n; i++) if (eval_bool(E, t->kids[i])) return true;
            return false;
        case VC_EQ: case VC_LT: case VC_LE: {
            int64_t a = eval_arith(E, t->kids[0]);
            int64_t b = eval_arith(E, t->kids[1]);
            if (E->fail) return false;
            return t->op == VC_EQ ? (a == b) : t->op == VC_LT ? (a < b) : (a <= b);
        }
        default:
            E->fail = true;   /* an opaque proposition: cannot evaluate */
            return false;
    }
}

static int64_t eval_arith(EvalCtx *E, const VCTerm *t) {
    if (E->fail) return 0;
    int64_t a, b, r = 0;
    switch (t->op) {
        case VC_CONST_INT: return t->as.i;
        case VC_VAR:       return E->vals[t->as.idx];
        case VC_NEG:
            a = eval_arith(E, t->kids[0]);
            if (E->fail || __builtin_sub_overflow((int64_t)0, a, &r)) { E->fail = true; return 0; }
            return r;
        case VC_ADD: case VC_SUB: case VC_MUL: case VC_DIV: case VC_MOD:
            a = eval_arith(E, t->kids[0]);
            b = eval_arith(E, t->kids[1]);
            if (E->fail) return 0;
            switch (t->op) {
                case VC_ADD: if (__builtin_add_overflow(a, b, &r)) E->fail = true; return r;
                case VC_SUB: if (__builtin_sub_overflow(a, b, &r)) E->fail = true; return r;
                case VC_MUL: if (__builtin_mul_overflow(a, b, &r)) E->fail = true; return r;
                case VC_DIV: if (b == 0) { E->fail = true; return 0; } return a / b;
                default:     if (b == 0) { E->fail = true; return 0; } return a % b;
            }
        default:
            E->fail = true;   /* VC_APP, VC_CONST_REAL, ... */
            return 0;
    }
}

static void collect_lits(const VCTerm *t, int64_t *out, uint32_t *n, uint32_t cap) {
    if (!t || *n >= cap) return;
    if (t->op == VC_CONST_INT) {
        for (uint32_t i = 0; i < *n; i++) if (out[i] == t->as.i) return;
        out[(*n)++] = t->as.i;
        return;
    }
    for (uint32_t i = 0; i < t->n; i++) collect_lits(t->kids[i], out, n, cap);
}

static void add_cand(int64_t *c, uint32_t *n, int64_t v) {
    if (*n >= MODEL_MAX_CANDS) return;
    for (uint32_t i = 0; i < *n; i++) if (c[i] == v) return;
    c[(*n)++] = v;
}

RefineModel *refine_model_search(RefineVC *vc, Arena *a) {
    if (!vc || !vc->goal) return NULL;
    if (vc->n_ufuncs > 0) return NULL;              /* measures have no fixed meaning */
    /* n_vars == 0 is allowed and is the IMPORTANT case: a call site with
     * literal arguments (`(safe-div 10 0)`) has a closed goal, so one
     * evaluation decides it outright.  The odometer below runs exactly once and
     * the model is empty -- there is nothing to bind, the arguments already
     * say it. */
    /* Past the uninterpreted-symbol gate, so this VC could plausibly use the
     * search at SOME cap -- which is what makes its width worth recording.
     * The peak is real, not saturating: n_vars is known before the check. */
    refine_cap_peak(&g_caps.model_vars_peak, vc->n_vars);
    if (vc->n_vars > MODEL_MAX_VARS) {
        g_caps.model_vars_hits++;
        /* Would a higher cap actually let this one search?  Only if every
         * variable is also an integer -- the sort gate below would otherwise
         * decline it anyway, and counting it as "the cap cost us this" would
         * overstate what a raise buys.  This is the number a raise has to be
         * argued from. */
        bool all_int = true;
        for (uint32_t i = 0; i < vc->n_vars; i++)
            if (vc->vars[i].sort != VS_INT) { all_int = false; break; }
        if (all_int) g_caps.model_vars_would_run++;
        return NULL;
    }
    for (uint32_t i = 0; i < vc->n_vars; i++)
        if (vc->vars[i].sort != VS_INT) return NULL;

    /* Candidates: literals in the VC, their immediate neighbours, and a few
     * small integers around zero. */
    int64_t lits[MODEL_MAX_CANDS]; uint32_t n_lits = 0;
    for (uint32_t i = 0; i < vc->n_hyps; i++)
        collect_lits(vc->hyps[i], lits, &n_lits, MODEL_MAX_CANDS);
    collect_lits(vc->goal, lits, &n_lits, MODEL_MAX_CANDS);

    int64_t cand[MODEL_MAX_CANDS]; uint32_t n_cand = 0;
    for (int64_t v = -2; v <= 2; v++) add_cand(cand, &n_cand, v);
    for (uint32_t i = 0; i < n_lits; i++) {
        add_cand(cand, &n_cand, lits[i]);
        if (lits[i] < INT64_MAX) add_cand(cand, &n_cand, lits[i] + 1);
        if (lits[i] > INT64_MIN) add_cand(cand, &n_cand, lits[i] - 1);
    }
    if (n_cand == 0 && vc->n_vars > 0) return NULL;

    uint32_t nv = vc->n_vars;
    int64_t vals[MODEL_MAX_VARS] = {0};
    uint32_t idx[MODEL_MAX_VARS] = {0};
    for (uint32_t i = 0; i < nv; i++) idx[i] = 0;

    for (;;) {
        for (uint32_t i = 0; i < nv; i++) vals[i] = cand[idx[i]];

        EvalCtx E = { vals, false };
        bool sat = true;
        for (uint32_t i = 0; i < vc->n_hyps && sat; i++)
            if (!eval_bool(&E, vc->hyps[i])) sat = false;
        if (sat && !E.fail && !eval_bool(&E, vc->goal) && !E.fail) {
            RefineModel *m = (RefineModel *)arena_alloc(a, sizeof(RefineModel));
            m->n = nv;
            m->bindings = nv ? (RefineModelBinding *)arena_alloc(
                                   a, nv * sizeof(RefineModelBinding)) : NULL;
            for (uint32_t i = 0; i < nv; i++) {
                m->bindings[i].name    = vc->vars[i].name;
                m->bindings[i].is_real = false;
                m->bindings[i].ival    = vals[i];
                m->bindings[i].rval    = 0.0;
            }
            return m;
        }
        if (E.fail) return NULL;   /* the formula is not evaluable at all */

        /* odometer */
        uint32_t k = 0;
        while (k < nv && ++idx[k] >= n_cand) { idx[k] = 0; k++; }
        if (k == nv) break;
    }
    return NULL;
}
