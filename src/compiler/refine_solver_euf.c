/* refine_solver_euf.c -- S1: congruence closure (EUF).
 *
 * Union-find + congruence closure decides quantifier-free equality with
 * uninterpreted functions -- exactly where every measure-as-opaque-function
 * (`len`, `elems`) and every nonlinear-as-uninterpreted term lives.  Textbook
 * treatment: Harrison, *Handbook of Practical Logic and Automated Reasoning*;
 * Bradley & Manna, *The Calculus of Computation*.
 *
 * Every non-leaf term is treated as an application of its operator, so
 * congruence covers `(+ a b)` and `(len v)` uniformly.  Three sources of
 * contradiction:
 *
 *   1. a disequality `a != b` whose sides became congruent;
 *   2. two DISTINCT literals in one class (`3` and `5` can never be equal);
 *   3. a positive atom and a negative atom that are congruent.
 *
 * The closure is a naive O(n^2) fixpoint.  Real obligations carry a handful of
 * terms, so this is the right tradeoff; REFINE_MAX_EUF_TERMS bounds the rest. */

#include "refine_solver.h"
#include "trail_c.h"

#include <stdlib.h>
#include <string.h>

struct EufState {
    RefineVC *vc;
    Arena    *a;
    VCTerm  **terms;   /* every subterm, deduplicated by pointer (hash-consed) */
    uint32_t *parent;  /* union-find over term indices */
    uint32_t *pstamp;  /* SX3: per-slot trail stamps, parallel to parent */
    uint32_t  n, cap;
    bool      unsat;
    TrailC    trail;   /* SX3: value trail over parent[]; see trail_c.h */
};

/* SX3: every parent[] write funnels through here so mark/undo see all of
 * them -- the union in uf_union AND the path compression in uf_find.
 * Trailing compression too is what keeps undo exact; the alternative
 * (compression-free find) costs more than it saves at these sizes.  The
 * merge history a Nieuwenhuis-Oliveras proof forest would need (SX6b's
 * euf_explain) is recoverable from these entries, which is the
 * representation choice the plan asked to keep open. */
static inline void euf_pset(EufState *st, uint32_t i, uint32_t v) {
    trailc_note(&st->trail, i, st->parent[i], &st->pstamp[i]);
    st->parent[i] = v;
}

/* ------------------------------------------------------------------------- *
 * Term registry + union-find
 * ------------------------------------------------------------------------- */

static uint32_t euf_index(EufState *st, VCTerm *t);

static void euf_add(EufState *st, VCTerm *t) {
    if (st->n >= REFINE_MAX_EUF_TERMS) {
        refine_caps()->euf_terms_hits++;
        refine_cap_peak(&refine_caps()->euf_terms_peak, REFINE_MAX_EUF_TERMS);
        st->unsat = false; return;
    }
    if (st->n == st->cap) {
        uint32_t ncap = st->cap ? st->cap * 2 : 32;
        VCTerm **nt = (VCTerm **)arena_alloc(st->a, ncap * sizeof(VCTerm *));
        uint32_t *np = (uint32_t *)arena_alloc(st->a, ncap * sizeof(uint32_t));
        uint32_t *ns = (uint32_t *)arena_alloc(st->a, ncap * sizeof(uint32_t));
        if (st->n) {
            memcpy(nt, st->terms, st->n * sizeof(VCTerm *));
            memcpy(np, st->parent, st->n * sizeof(uint32_t));
            memcpy(ns, st->pstamp, st->n * sizeof(uint32_t));
        }
        st->terms = nt; st->parent = np; st->pstamp = ns; st->cap = ncap;
    }
    st->terms[st->n]  = t;
    st->parent[st->n] = st->n;
    st->pstamp[st->n] = 0;      /* SX3: never stamped at any live level */
    st->n++;
    refine_cap_peak(&refine_caps()->euf_terms_peak, st->n);
}

/* Register `t` and every subterm; returns t's index (UINT32_MAX if capped). */
static uint32_t euf_index(EufState *st, VCTerm *t) {
    if (!t) return UINT32_MAX;
    for (uint32_t i = 0; i < st->n; i++) if (st->terms[i] == t) return i;
    for (uint32_t i = 0; i < t->n; i++) euf_index(st, t->kids[i]);
    if (st->n >= REFINE_MAX_EUF_TERMS) {
        refine_caps()->euf_terms_hits++;
        refine_cap_peak(&refine_caps()->euf_terms_peak, REFINE_MAX_EUF_TERMS);
        return UINT32_MAX;
    }
    euf_add(st, t);
    return st->n - 1;
}

static uint32_t uf_find(EufState *st, uint32_t i) {
    while (st->parent[i] != i) {
        uint32_t gp = st->parent[st->parent[i]];
        euf_pset(st, i, gp);        /* path compression, trailed (SX3) */
        i = gp;
    }
    return i;
}

static bool uf_union(EufState *st, uint32_t i, uint32_t j) {
    uint32_t ri = uf_find(st, i), rj = uf_find(st, j);
    if (ri == rj) return false;
    euf_pset(st, rj, ri);
    return true;
}

/* ------------------------------------------------------------------------- *
 * Closure
 * ------------------------------------------------------------------------- */

/* Two terms are congruent when they have the same operator/symbol/arity and
 * their arguments are pairwise equal in the current partition. */
/* Lookup only -- never registers.  The closure loop iterates over st->n, so a
 * registering lookup there would mutate the array mid-iteration. */
static uint32_t euf_lookup(const EufState *st, const VCTerm *t) {
    for (uint32_t i = 0; i < st->n; i++) if (st->terms[i] == t) return i;
    return UINT32_MAX;
}

static bool congruent(EufState *st, VCTerm *x, VCTerm *y) {
    if (x->op != y->op || x->n != y->n || x->n == 0) return false;
    if (x->op == VC_APP && x->as.idx != y->as.idx) return false;
    for (uint32_t i = 0; i < x->n; i++) {
        uint32_t a = euf_lookup(st, x->kids[i]);
        uint32_t b = euf_lookup(st, y->kids[i]);
        if (a == UINT32_MAX || b == UINT32_MAX) return false;
        if (uf_find(st, a) != uf_find(st, b)) return false;
    }
    return true;
}

static void euf_close(EufState *st) {
    bool changed = true;
    uint32_t rounds = 0;
    while (changed && rounds++ < 64) {
        changed = false;
        for (uint32_t i = 0; i < st->n; i++) {
            for (uint32_t j = i + 1; j < st->n; j++) {
                if (uf_find(st, i) == uf_find(st, j)) continue;
                if (congruent(st, st->terms[i], st->terms[j])) {
                    uf_union(st, i, j);
                    changed = true;
                }
            }
        }
    }
}

/* Distinct numeric literals can never share a class. */
static bool literal_conflict(EufState *st) {
    for (uint32_t i = 0; i < st->n; i++) {
        VCTerm *x = st->terms[i];
        if (x->op != VC_CONST_INT && x->op != VC_CONST_REAL) continue;
        for (uint32_t j = i + 1; j < st->n; j++) {
            VCTerm *y = st->terms[j];
            if (y->op != VC_CONST_INT && y->op != VC_CONST_REAL) continue;
            if (uf_find(st, i) == uf_find(st, j)) return true;  /* x != y by construction */
        }
    }
    return false;
}

/* ------------------------------------------------------------------------- *
 * Public API
 * ------------------------------------------------------------------------- */

EufState *euf_new(RefineVC *vc, Arena *a) {
    EufState *st = (EufState *)arena_alloc(a, sizeof(EufState));
    memset(st, 0, sizeof(*st));
    st->vc = vc; st->a = a;
    trailc_init(&st->trail, a);
    return st;
}

/* SX3: mark / undo-to-mark.  The trail restores parent[]; the mark itself
 * restores the append-only pieces (n, unsat) by truncation.  Terms above the
 * mark are simply forgotten -- euf_add re-initializes parent/pstamp on reuse,
 * and trailc_undo_to skips entries for truncated slots. */
EufMark euf_mark(EufState *st) {
    TrailCMark tm = trailc_mark(&st->trail);
    EufMark m = { tm.len, tm.level, st->n, st->unsat };
    return m;
}

void euf_undo_to(EufState *st, EufMark m) {
    TrailCMark tm = { m.trail_len, m.trail_level };
    trailc_undo_to(&st->trail, tm, st->parent, m.n);
    st->n     = m.n;
    st->unsat = m.unsat;
}

/* SX3 seam: TUR_REFINE_EUF=rebuild restores the per-cube euf_new path so the
 * corpus can be replayed against both.  Env-only, like
 * TUR_REFINE_NO_DISCHARGE; default is incremental. */
bool euf_incremental_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("TUR_REFINE_EUF");
        mode = !(e && strcmp(e, "rebuild") == 0);
    }
    return mode != 0;
}

bool euf_assert_eq(EufState *st, VCTerm *x, VCTerm *y) {
    uint32_t i = euf_index(st, x), j = euf_index(st, y);
    if (i == UINT32_MAX || j == UINT32_MAX) return true;   /* capped: no info */
    uf_union(st, i, j);
    euf_close(st);
    if (literal_conflict(st)) { st->unsat = true; return false; }
    return true;
}

bool euf_equal(EufState *st, VCTerm *x, VCTerm *y) {
    uint32_t i = euf_index(st, x), j = euf_index(st, y);
    if (i == UINT32_MAX || j == UINT32_MAX) return false;
    return uf_find(st, i) == uf_find(st, j);
}

uint32_t euf_term_count(const EufState *st) { return st->n; }
VCTerm  *euf_term_at(const EufState *st, uint32_t i) {
    return i < st->n ? st->terms[i] : NULL;
}

bool euf_assert_cube(EufState *st, const VCCube *c) {
    /* Pass 1: register every term and assert the positive equalities. */
    for (uint32_t i = 0; i < c->n; i++) {
        VCTerm *lit = c->lits[i];
        VCTerm *at  = refine_lit_atom(lit);
        euf_index(st, at);
        if (!refine_lit_is_neg(lit) && at->op == VC_EQ) {
            uint32_t x = euf_index(st, at->kids[0]);
            uint32_t y = euf_index(st, at->kids[1]);
            if (x != UINT32_MAX && y != UINT32_MAX) uf_union(st, x, y);
        }
    }
    euf_close(st);
    if (literal_conflict(st)) { st->unsat = true; return false; }

    /* Pass 2: contradictions. */
    for (uint32_t i = 0; i < c->n; i++) {
        VCTerm *lit = c->lits[i];
        VCTerm *at  = refine_lit_atom(lit);
        if (refine_lit_is_neg(lit) && at->op == VC_EQ) {
            if (euf_equal(st, at->kids[0], at->kids[1])) { st->unsat = true; return false; }
        }
        /* A positive and a negative occurrence of congruent atoms. */
        for (uint32_t j = i + 1; j < c->n; j++) {
            VCTerm *lit2 = c->lits[j];
            if (refine_lit_is_neg(lit) == refine_lit_is_neg(lit2)) continue;
            VCTerm *at2 = refine_lit_atom(lit2);
            if (at == at2) { st->unsat = true; return false; }
            if (congruent(st, at, at2)) { st->unsat = true; return false; }
        }
    }
    return true;
}

RefineDecision refine_s1_decide(RefineVC *vc, Arena *a) {
    if (!vc || !vc->goal) return refine_unknown();

    VCCubeSet cs;
    if (!refine_cubes_build(vc, a, &cs)) return refine_unknown();
    if (cs.trivial || cs.n == 0) return refine_valid();

    /* SX3: one state, mark/undo per cube, instead of a fresh euf_new per
     * cube -- the arrays and trail grow once and are reused.  Each cube still
     * starts from the empty partition (the mark is taken on the empty state),
     * so verdicts and cap telemetry are identical to the rebuild path by
     * construction. */
    EufState *inc = euf_incremental_mode() ? euf_new(vc, a) : NULL;
    for (uint32_t i = 0; i < cs.n; i++) {
        bool survived;
        if (inc) {
            EufMark m = euf_mark(inc);
            survived = euf_assert_cube(inc, &cs.cubes[i]);
            euf_undo_to(inc, m);
        } else {
            EufState *st = euf_new(vc, a);
            survived = euf_assert_cube(st, &cs.cubes[i]);
        }
        if (survived) return refine_unknown();  /* cube survived */
    }
    return refine_valid();
}
