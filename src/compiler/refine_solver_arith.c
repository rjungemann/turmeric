/* refine_solver_arith.c -- S2: linear arithmetic.
 *
 * The plan's S2a target is difference logic (`x - y <= c`) via Bellman-Ford,
 * with full simplex LRA as S2b.  This implementation takes a different route
 * to the same coverage band: FOURIER-MOTZKIN elimination over exact rationals.
 * It is a strict superset of difference logic (so the S2a target -- index and
 * bounds reasoning, `0 <= i`, `i < len`, `i + 1 <= n` -- is covered), it
 * decides conjunctions of linear constraints over the rationals outright, and
 * it is a few hundred lines rather than the multi-week incremental-simplex
 * investment.  Its weakness is combinatorial blow-up, which is exactly what
 * the RT_UNKNOWN escape hatch absorbs: REFINE_MAX_LA_CONSTR bounds the growth
 * and a cap hit degrades to a runtime check.  If profiling ever shows the cap
 * biting on real obligations, the Dutertre--de Moura simplex slots in behind
 * the same la_* interface.
 *
 * Integers: rational unsatisfiability implies integer unsatisfiability, so
 * every refutation found here is sound for `int` too.  We additionally tighten
 * strict integer constraints (`e < 0` becomes `e <= -1` when every variable is
 * int-sorted and every coefficient is integral), which is what lets
 * `(> x 0) |= (> (* x 2) 0)` go through.  Genuine integer completeness
 * (branch-and-bound / Omega) is the S2c tail we deliberately do not chase.
 *
 * PURIFICATION.  Any term the linear encoder cannot see inside -- a VC_VAR, an
 * uninterpreted application (a measure, or a nonlinear product already
 * abstracted by the RT2 encoder) -- becomes an opaque LA variable.  That is
 * the Nelson-Oppen purification step, and it is what lets S3 exchange
 * equalities with EUF over exactly these shared terms. */

#include "refine_solver.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- *
 * Exact rationals (int64 num/den; every step overflow-checked -> give up)
 * ------------------------------------------------------------------------- */

typedef struct { int64_t num, den; bool bad; } Rat;

static int64_t igcd(int64_t a, int64_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = a % b; a = b; b = t; }
    return a ? a : 1;
}

static Rat rat_bad(void)        { Rat r = {0, 1, true}; return r; }
static Rat rat_of(int64_t v)    { Rat r = {v, 1, false}; return r; }

/* Normalize n/d.  Every arithmetic step below is overflow-checked and falls
 * back to `bad` rather than wrapping: a wrapped coefficient could manufacture
 * a refutation that does not exist, which would break the soundness
 * invariant.  A `bad` rational drops its constraint instead, which can only
 * ever weaken the refutation. */
static Rat rat_norm(int64_t n, int64_t d) {
    if (d == 0) return rat_bad();
    if (d < 0) {
        if (n == INT64_MIN || d == INT64_MIN) return rat_bad();
        n = -n; d = -d;
    }
    int64_t g = igcd(n, d);
    return (Rat){ n / g, d / g, false };
}

static Rat rat_add(Rat a, Rat b) {
    if (a.bad || b.bad) return rat_bad();
    int64_t t1, t2, num, den;
    if (__builtin_mul_overflow(a.num, b.den, &t1)) return rat_bad();
    if (__builtin_mul_overflow(b.num, a.den, &t2)) return rat_bad();
    if (__builtin_add_overflow(t1, t2, &num))      return rat_bad();
    if (__builtin_mul_overflow(a.den, b.den, &den)) return rat_bad();
    return rat_norm(num, den);
}
static Rat rat_mul(Rat a, Rat b) {
    if (a.bad || b.bad) return rat_bad();
    /* Cross-cancel first so ordinary coefficients never approach the limit. */
    int64_t g1 = igcd(a.num, b.den), g2 = igcd(b.num, a.den);
    int64_t an = a.num / g1, bd = b.den / g1;
    int64_t bn = b.num / g2, ad = a.den / g2;
    int64_t num, den;
    if (__builtin_mul_overflow(an, bn, &num)) return rat_bad();
    if (__builtin_mul_overflow(ad, bd, &den)) return rat_bad();
    return rat_norm(num, den);
}
static Rat rat_neg(Rat a)  { if (a.bad || a.num == INT64_MIN) return rat_bad(); a.num = -a.num; return a; }
static bool rat_zero(Rat a){ return !a.bad && a.num == 0; }
static bool rat_pos(Rat a) { return !a.bad && a.num > 0; }
static bool rat_neg_p(Rat a){ return !a.bad && a.num < 0; }
static bool rat_is_int(Rat a){ return !a.bad && a.den == 1; }

/* Convert a double literal to an exact rational when it is safely
 * representable; otherwise mark it bad so the constraint is dropped (dropping
 * a constraint only weakens the refutation -- it can never manufacture one). */
static Rat rat_of_double(double d) {
    if (d != d || d > 1e15 || d < -1e15) return rat_bad();
    /* Try denominators up to 1e6; float refinements in practice are things
     * like 0.0, 0.5, 3.25. */
    for (int64_t den = 1; den <= 1000000; den *= 10) {
        double scaled = d * (double)den;
        double r = scaled < 0 ? -scaled : scaled;
        if (r < 9e18 && scaled == (double)(int64_t)scaled)
            return rat_norm((int64_t)scaled, den);
    }
    return rat_bad();
}

/* ------------------------------------------------------------------------- *
 * Linear expressions and constraints
 * ------------------------------------------------------------------------- */

/* `coef[0..n_vars-1] . x + konst  {<,<=}  0` */
typedef struct {
    Rat  coef[REFINE_MAX_LA_VARS];
    Rat  konst;
    bool strict;
    bool bad;
} LinC;

struct LaState {
    RefineVC *vc;
    Arena    *a;
    VCTerm   *vars[REFINE_MAX_LA_VARS];   /* opaque terms, in registration order */
    uint32_t  n_vars;
    LinC     *cs;
    uint32_t  n_cs, cap_cs;
    bool      gave_up;   /* a cap was hit: never claim unsat afterwards */
};

/* A term the linear encoder must treat as opaque. */
bool la_is_shared_term(const VCTerm *t) {
    return t && (t->op == VC_VAR || t->op == VC_APP);
}

static uint32_t la_var(LaState *st, VCTerm *t) {
    for (uint32_t i = 0; i < st->n_vars; i++) if (st->vars[i] == t) return i;
    if (st->n_vars >= REFINE_MAX_LA_VARS) {
        refine_caps()->la_vars_hits++;
        refine_cap_peak(&refine_caps()->la_vars_peak, REFINE_MAX_LA_VARS);
        st->gave_up = true; return UINT32_MAX;
    }
    st->vars[st->n_vars] = t;
    refine_cap_peak(&refine_caps()->la_vars_peak, st->n_vars + 1);
    return st->n_vars++;
}

typedef struct {
    Rat  coef[REFINE_MAX_LA_VARS];
    Rat  konst;
    bool bad;
} LinExp;

static LinExp lin_zero(void) {
    LinExp e; memset(&e, 0, sizeof(e));
    for (uint32_t i = 0; i < REFINE_MAX_LA_VARS; i++) e.coef[i] = rat_of(0);
    e.konst = rat_of(0);
    e.bad = false;
    return e;
}

static LinExp lin_scale(LinExp e, Rat k) {
    if (e.bad || k.bad) { e.bad = true; return e; }
    for (uint32_t i = 0; i < REFINE_MAX_LA_VARS; i++) {
        e.coef[i] = rat_mul(e.coef[i], k);
        if (e.coef[i].bad) { e.bad = true; return e; }
    }
    e.konst = rat_mul(e.konst, k);
    if (e.konst.bad) e.bad = true;
    return e;
}

static LinExp lin_add(LinExp a, LinExp b) {
    if (a.bad || b.bad) { a.bad = true; return a; }
    for (uint32_t i = 0; i < REFINE_MAX_LA_VARS; i++) {
        a.coef[i] = rat_add(a.coef[i], b.coef[i]);
        if (a.coef[i].bad) { a.bad = true; return a; }
    }
    a.konst = rat_add(a.konst, b.konst);
    if (a.konst.bad) a.bad = true;
    return a;
}

/* Deepest arithmetic nesting linearize will walk before giving the whole
 * constraint up as `bad` (dropped -- which only ever weakens a refutation,
 * so the bound is sound by construction).  It exists because LinExp is
 * passed and returned BY VALUE: with 32 exact-rational coefficients a frame
 * carries several ~800-byte copies, and under ASan redzones the recursion
 * measured dead between depth 750 and 1000 on an 8 MB stack -- far below
 * the corpus reader's term-depth cap, which is what let a legal benchmark
 * crash the child.  500 sits under the measured floor with margin; a real
 * obligation deeper than this is FM blow-up bait anyway. */
#define LA_MAX_LINEARIZE_DEPTH 500

/* Linearize `t`.  Anything not built from +,-,unary-,literal, and
 * multiplication/division by a literal becomes an opaque variable. */
static LinExp linearize(LaState *st, VCTerm *t, uint32_t depth) {
    LinExp e = lin_zero();
    if (!t) { e.bad = true; return e; }
    if (depth > LA_MAX_LINEARIZE_DEPTH) { e.bad = true; return e; }

    switch (t->op) {
        case VC_CONST_INT:  e.konst = rat_of(t->as.i); return e;
        case VC_CONST_REAL: e.konst = rat_of_double(t->as.r);
                            if (e.konst.bad) e.bad = true;
                            return e;
        case VC_ADD: return lin_add(linearize(st, t->kids[0], depth + 1),
                                    linearize(st, t->kids[1], depth + 1));
        case VC_SUB: return lin_add(linearize(st, t->kids[0], depth + 1),
                                    lin_scale(linearize(st, t->kids[1], depth + 1),
                                              rat_of(-1)));
        case VC_NEG: return lin_scale(linearize(st, t->kids[0], depth + 1), rat_of(-1));
        case VC_MUL: {
            VCTerm *a = t->kids[0], *b = t->kids[1];
            VCTerm *lit = NULL, *other = NULL;
            if (a->op == VC_CONST_INT || a->op == VC_CONST_REAL) { lit = a; other = b; }
            else if (b->op == VC_CONST_INT || b->op == VC_CONST_REAL) { lit = b; other = a; }
            if (!lit) break;   /* nonlinear: fall through to opaque */
            Rat k = (lit->op == VC_CONST_INT) ? rat_of(lit->as.i) : rat_of_double(lit->as.r);
            if (k.bad) { e.bad = true; return e; }
            return lin_scale(linearize(st, other, depth + 1), k);
        }
        case VC_DIV: {
            VCTerm *b = t->kids[1];
            if (b->op != VC_CONST_INT && b->op != VC_CONST_REAL) break;
            Rat k = (b->op == VC_CONST_INT) ? rat_of(b->as.i) : rat_of_double(b->as.r);
            if (k.bad || rat_zero(k)) { e.bad = true; return e; }
            Rat inv = rat_norm(k.den, k.num);
            if (inv.bad) { e.bad = true; return e; }
            /* Integer division truncates, so `(/ x 2)` is NOT `x/2` over the
             * integers.  Only treat it as exact division when the term is
             * real-sorted; otherwise it stays opaque. */
            if (t->sort != VS_REAL) break;
            return lin_scale(linearize(st, t->kids[0], depth + 1), inv);
        }
        default: break;
    }

    /* Opaque: a variable, an application, a nonlinear product, `mod`, or an
     * integer division.  This is the purification boundary S3 shares. */
    uint32_t v = la_var(st, t);
    if (v == UINT32_MAX) { e.bad = true; return e; }
    e.coef[v] = rat_of(1);
    return e;
}

/* True when every variable with a nonzero coefficient is integer-sorted. */
static bool all_int_vars(LaState *st, const LinExp *e) {
    for (uint32_t i = 0; i < st->n_vars; i++) {
        if (rat_zero(e->coef[i])) continue;
        if (st->vars[i]->sort != VS_INT) return false;
    }
    return true;
}

static void la_push(LaState *st, LinExp e, bool strict) {
    if (e.bad) return;              /* dropping a constraint only weakens us */
    if (st->n_cs >= REFINE_MAX_LA_CONSTR) {
        refine_caps()->la_constr_hits++;
        refine_cap_peak(&refine_caps()->la_constr_peak, REFINE_MAX_LA_CONSTR);
        st->gave_up = true; return;
    }
    refine_cap_peak(&refine_caps()->la_constr_peak, st->n_cs + 1);
    if (st->n_cs == st->cap_cs) {
        uint32_t ncap = st->cap_cs ? st->cap_cs * 2 : 16;
        LinC *nc = (LinC *)arena_alloc(st->a, ncap * sizeof(LinC));
        if (st->n_cs) memcpy(nc, st->cs, st->n_cs * sizeof(LinC));
        st->cs = nc; st->cap_cs = ncap;
    }
    LinC *c = &st->cs[st->n_cs++];
    memcpy(c->coef, e.coef, sizeof(c->coef));
    c->konst  = e.konst;
    c->strict = strict;
    c->bad    = false;

    /* Integer tightening: `e < 0` over integral data becomes `e + 1 <= 0`. */
    if (strict && all_int_vars(st, &e) && rat_is_int(e.konst)) {
        bool integral = true;
        for (uint32_t i = 0; i < st->n_vars; i++)
            if (!rat_is_int(e.coef[i])) { integral = false; break; }
        if (integral) {
            c->konst  = rat_add(c->konst, rat_of(1));
            c->strict = false;
            if (c->konst.bad) { st->n_cs--; }
        }
    }
}

/* ------------------------------------------------------------------------- *
 * Assertions
 * ------------------------------------------------------------------------- */

LaState *la_new(RefineVC *vc, Arena *a) {
    LaState *st = (LaState *)arena_alloc(a, sizeof(LaState));
    memset(st, 0, sizeof(*st));
    st->vc = vc; st->a = a;
    return st;
}

/* lhs <(=) rhs  ==>  lhs - rhs <(=) 0 */
bool la_assert_le(LaState *st, VCTerm *lhs, VCTerm *rhs, bool strict) {
    LinExp e = lin_add(linearize(st, lhs, 0),
                       lin_scale(linearize(st, rhs, 0), rat_of(-1)));
    la_push(st, e, strict);
    return !e.bad;
}

bool la_assert_eq(LaState *st, VCTerm *x, VCTerm *y) {
    bool ok = la_assert_le(st, x, y, false);
    ok = la_assert_le(st, y, x, false) && ok;
    return ok;
}

bool la_assert_cube(LaState *st, const VCCube *c) {
    for (uint32_t i = 0; i < c->n; i++) {
        VCTerm *lit = c->lits[i];
        VCTerm *at  = refine_lit_atom(lit);
        bool    neg = refine_lit_is_neg(lit);
        if (at->n != 2) continue;
        VCTerm *x = at->kids[0], *y = at->kids[1];
        if (!vc_is_arith(x) || !vc_is_arith(y)) continue;
        switch (at->op) {
            case VC_LT:
                if (neg) la_assert_le(st, y, x, false);   /* !(x<y) => y<=x */
                else     la_assert_le(st, x, y, true);
                break;
            case VC_LE:
                if (neg) la_assert_le(st, y, x, true);    /* !(x<=y) => y<x */
                else     la_assert_le(st, x, y, false);
                break;
            case VC_EQ:
                /* A disequality carries no linear information on its own (it
                 * is a disjunction); S2 skips it, which is sound.  Integer
                 * case-splitting on it is the S2c tail. */
                if (!neg) la_assert_eq(st, x, y);
                break;
            default: break;
        }
    }
    return !st->gave_up;
}

/* ------------------------------------------------------------------------- *
 * Fourier-Motzkin
 * ------------------------------------------------------------------------- */

/* A variable-free constraint that cannot hold. */
static bool constant_false(const LinC *c, uint32_t n_vars) {
    for (uint32_t i = 0; i < n_vars; i++) if (!rat_zero(c->coef[i])) return false;
    if (c->konst.bad) return false;
    return c->strict ? (c->konst.num >= 0) : (c->konst.num > 0);
}

bool la_unsat(LaState *st) {
    if (st->gave_up) return false;

    uint32_t n_vars = st->n_vars;
    uint32_t n = st->n_cs;
    if (n == 0) return false;

    LinC *cur = (LinC *)arena_alloc(st->a, (n ? n : 1) * sizeof(LinC));
    memcpy(cur, st->cs, n * sizeof(LinC));

    for (uint32_t i = 0; i < n; i++)
        if (constant_false(&cur[i], n_vars)) return true;

    for (uint32_t v = 0; v < n_vars; v++) {
        /* Partition on the sign of x_v. */
        uint32_t n_pos = 0, n_neg = 0, n_zero = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (rat_pos(cur[i].coef[v]))        n_pos++;
            else if (rat_neg_p(cur[i].coef[v])) n_neg++;
            else                                n_zero++;
        }
        uint64_t next = (uint64_t)n_zero + (uint64_t)n_pos * (uint64_t)n_neg;
        if (next > REFINE_MAX_LA_CONSTR) {
            /* Counted apart from the static adder above: this is elimination
             * GROWING past the cap, not an input that arrived too wide, and
             * the two point at different fixes (SX4's simplex kills this one;
             * only a wider cap helps the other). */
            refine_caps()->la_fm_hits++;
            refine_cap_peak(&refine_caps()->la_constr_peak, REFINE_MAX_LA_CONSTR);
            return false;   /* blow-up: give up, stay sound */
        }
        refine_cap_peak(&refine_caps()->la_constr_peak, (uint32_t)next);

        LinC *nxt = (LinC *)arena_alloc(st->a, (next ? next : 1) * sizeof(LinC));
        uint32_t m = 0;
        for (uint32_t i = 0; i < n; i++)
            if (rat_zero(cur[i].coef[v])) nxt[m++] = cur[i];

        for (uint32_t i = 0; i < n && m <= next; i++) {
            if (!rat_pos(cur[i].coef[v])) continue;
            for (uint32_t j = 0; j < n; j++) {
                if (!rat_neg_p(cur[j].coef[v])) continue;
                /* Scale so the x_v terms cancel: |a_j| * C_i + a_i * C_j. */
                Rat si = rat_neg(cur[j].coef[v]);   /* > 0 */
                Rat sj = cur[i].coef[v];            /* > 0 */
                LinC out; memset(&out, 0, sizeof(out));
                for (uint32_t k = 0; k < REFINE_MAX_LA_VARS; k++) out.coef[k] = rat_of(0);
                out.konst = rat_of(0);
                bool bad = false;
                for (uint32_t k = 0; k < n_vars; k++) {
                    Rat t = rat_add(rat_mul(cur[i].coef[k], si), rat_mul(cur[j].coef[k], sj));
                    if (t.bad) { bad = true; break; }
                    out.coef[k] = t;
                }
                if (bad) continue;   /* overflow: drop this combination */
                Rat kk = rat_add(rat_mul(cur[i].konst, si), rat_mul(cur[j].konst, sj));
                if (kk.bad) continue;
                out.konst  = kk;
                out.strict = cur[i].strict || cur[j].strict;
                if (m < next) nxt[m++] = out;
                if (constant_false(&out, n_vars)) return true;
            }
        }
        cur = nxt; n = m;
        for (uint32_t i = 0; i < n; i++)
            if (constant_false(&cur[i], n_vars)) return true;
        if (n == 0) return false;
    }
    for (uint32_t i = 0; i < n; i++)
        if (constant_false(&cur[i], n_vars)) return true;
    return false;
}

bool la_entails_eq(LaState *st, VCTerm *x, VCTerm *y) {
    if (st->gave_up) return false;
    uint32_t saved = st->n_cs;

    la_assert_le(st, x, y, true);         /* x < y */
    bool lt_unsat = la_unsat(st);
    st->n_cs = saved;
    if (!lt_unsat) return false;

    la_assert_le(st, y, x, true);         /* y < x */
    bool gt_unsat = la_unsat(st);
    st->n_cs = saved;
    return gt_unsat;
}

RefineDecision refine_s2_decide(RefineVC *vc, Arena *a) {
    if (!vc || !vc->goal) return refine_unknown();

    VCCubeSet cs;
    if (!refine_cubes_build(vc, a, &cs)) return refine_unknown();
    if (cs.trivial || cs.n == 0) return refine_valid();

    for (uint32_t i = 0; i < cs.n; i++) {
        LaState *st = la_new(vc, a);
        la_assert_cube(st, &cs.cubes[i]);
        if (!la_unsat(st)) return refine_unknown();
    }
    return refine_valid();
}
