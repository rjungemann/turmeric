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
    bool       overflow;
} CubeAcc;

static void acc_push(CubeAcc *acc, VCTerm **lits, uint32_t n) {
    if (acc->overflow) return;
    if (acc->n >= REFINE_MAX_CUBES) { acc->overflow = true; return; }
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
        return;
    }

    /* No disjunctions left: everything pending is a literal (or a conjunction
     * already flattened by vc_mk).  Collect into one cube. */
    uint32_t total = n_conj;
    for (uint32_t i = 0; i < n_pending; i++)
        total += (pending[i]->op == VC_AND) ? pending[i]->n : 1;
    if (total > REFINE_MAX_CUBE_LITS) { acc->overflow = true; return; }

    VCTerm **lits = (VCTerm **)arena_alloc(acc->arena, (total ? total : 1) * sizeof(VCTerm *));
    uint32_t k = 0;
    for (uint32_t i = 0; i < n_conj; i++) lits[k++] = conj[i];
    bool has_false = false;
    for (uint32_t i = 0; i < n_pending; i++) {
        VCTerm *t = pending[i];
        uint32_t kn = (t->op == VC_AND) ? t->n : 1;
        for (uint32_t j = 0; j < kn; j++) {
            VCTerm *x = (t->op == VC_AND) ? t->kids[j] : t;
            if (x->op == VC_FALSE) { has_false = true; continue; }
            if (x->op == VC_TRUE)  continue;
            if (x->op == VC_OR || x->op == VC_AND) {
                /* A nested boolean that survived flattening: re-enter. */
                VCTerm **again = (VCTerm **)arena_alloc(acc->arena,
                                                        (n_pending - i) * sizeof(VCTerm *));
                uint32_t m2 = 0;
                for (uint32_t q = i; q < n_pending; q++) again[m2++] = pending[q];
                expand(acc, again, m2, lits, k);
                return;
            }
            lits[k++] = x;
        }
    }
    /* A cube containing `false` is already unsatisfiable, and validity needs
     * every cube to be unsatisfiable -- so dropping it is correct (recording
     * it as an empty cube would instead read as "no constraints", which is
     * satisfiable and would wrongly block the proof). */
    if (has_false) return;
    acc_push(acc, lits, k);
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

#define MODEL_MAX_VARS  3
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
    if (vc->n_vars == 0 || vc->n_vars > MODEL_MAX_VARS) return NULL;
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
    if (n_cand == 0) return NULL;

    uint32_t nv = vc->n_vars;
    int64_t vals[MODEL_MAX_VARS];
    uint32_t idx[MODEL_MAX_VARS];
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
            m->bindings = (RefineModelBinding *)arena_alloc(a, nv * sizeof(RefineModelBinding));
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
