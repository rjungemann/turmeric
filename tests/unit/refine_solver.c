/* refine_solver.c -- unit tests for the RT2 normalized VC and the in-house
 * staged decision procedure (S0..S3).
 *
 * The property under test is the SOUNDNESS INVARIANT, which is
 * one-directional: a stage may never answer RT_VALID for an obligation that is
 * not entailed.  RT_UNKNOWN is always acceptable, so the "should not prove"
 * cases assert `verdict != RT_VALID` rather than a specific verdict -- a
 * future stage that legitimately decides one of them must not break this file.
 *
 * See docs/archive/refinement-types-plan.md. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lsp/lsp_sym.h"            /* LspSymbol, for the link stub below */
#include "compiler/refine_discharge.h"   /* RT6 hint search */
#include "compiler/refine_solver.h"
#include "compiler/refine_smtlib.h"
#include "runtime/arena.h"

/* Stub: tur_core's lsp.c references this; this test never touches LSP, but the
 * symbol must resolve when tur_core is linked into a standalone executable.
 * Matches the stub in tests/unit/experiments_user_config.c. */
int tur_collect_symbols(const char *source_path, const char *logical_path,
                        LspSymbol *out, int cap, int *count_out) {
    (void)logical_path;
    (void)source_path;
    (void)out;
    (void)cap;
    if (count_out) *count_out = 0;
    return 0;
}

static int failures = 0;
static int checks   = 0;

static void ok(bool cond, const char *what) {
    checks++;
    if (!cond) { failures++; fprintf(stderr, "FAIL: %s\n", what); }
}

/* Run the whole chain the discharge pass runs, in the same order. */
static RefineVerdict decide(RefineVC *vc, Arena *a) {
    RefineBackend chain[] = { refine_s0_decide, refine_s1_decide,
                              refine_s2_decide, refine_s3_decide };
    for (size_t i = 0; i < sizeof(chain) / sizeof(chain[0]); i++) {
        RefineDecision d = chain[i](vc, a);
        if (d.verdict != RT_UNKNOWN) return d.verdict;
    }
    return RT_UNKNOWN;
}

/* --------------------------------------------------------------------- */

/* Helpers that mirror how refine_collect.c builds terms. */
static VCTerm *V(RefineVC *vc, const char *n) {
    return vc_var_ref(vc, vc_declare_var(vc, n, VS_INT));
}
static VCTerm *R(RefineVC *vc, const char *n) {
    return vc_var_ref(vc, vc_declare_var(vc, n, VS_REAL));
}
static VCTerm *lt(RefineVC *vc, VCTerm *a, VCTerm *b) { return vc_mk2(vc, VC_LT, a, b); }
static VCTerm *le(RefineVC *vc, VCTerm *a, VCTerm *b) { return vc_mk2(vc, VC_LE, a, b); }
static VCTerm *eq(RefineVC *vc, VCTerm *a, VCTerm *b) { return vc_mk2(vc, VC_EQ, a, b); }
static VCTerm *add(RefineVC *vc, VCTerm *a, VCTerm *b) { return vc_mk2(vc, VC_ADD, a, b); }
static VCTerm *mul(RefineVC *vc, VCTerm *a, VCTerm *b) { return vc_mk2(vc, VC_MUL, a, b); }

/* --------------------------------------------------------------------- */

static void test_hash_consing(Arena *a) {
    RefineVC *vc = vc_new(a);
    VCTerm *x1 = V(vc, "x");
    VCTerm *x2 = V(vc, "x");
    ok(x1 == x2, "hash-consing: the same variable interns to one term");
    ok(add(vc, x1, vc_int(vc, 1)) == add(vc, x2, vc_int(vc, 1)),
       "hash-consing: structurally equal compounds are pointer-equal");
    ok(vc_declare_var(vc, "x", VS_INT) == 0, "declaring a variable twice reuses the slot");
}

static void test_constant_folding(Arena *a) {
    RefineVC *vc = vc_new(a);
    ok(add(vc, vc_int(vc, 1), vc_int(vc, 2))->op == VC_CONST_INT,
       "folding: 1 + 2 folds to a literal");
    ok(add(vc, vc_int(vc, 1), vc_int(vc, 2))->as.i == 3, "folding: 1 + 2 == 3");
    ok(lt(vc, vc_int(vc, 0), vc_int(vc, 3))->op == VC_TRUE, "folding: 0 < 3 is true");
    ok(lt(vc, vc_int(vc, 3), vc_int(vc, 0))->op == VC_FALSE, "folding: 3 < 0 is false");
    VCTerm *x = V(vc, "x");
    ok(lt(vc, x, x)->op == VC_FALSE, "folding: x < x is false");
    ok(le(vc, x, x)->op == VC_TRUE, "folding: x <= x is true");
    ok(vc_not(vc, vc_not(vc, lt(vc, x, vc_int(vc, 5)))) == lt(vc, x, vc_int(vc, 5)),
       "folding: double negation collapses");
    /* Overflow must NOT fold -- a wrapped constant would be unsound. */
    ok(mul(vc, vc_int(vc, INT64_MAX), vc_int(vc, 2))->op == VC_MUL,
       "folding: an overflowing product is left unfolded");
}

static void test_s0_trivial(Arena *a) {
    /* |- 0 < 3 */
    RefineVC *vc = vc_new(a);
    vc_set_goal(vc, lt(vc, vc_int(vc, 0), vc_int(vc, 3)));
    ok(refine_s0_decide(vc, a).verdict == RT_VALID, "S0: constant goal");

    /* x > 0 |- x > 0  (syntactic entailment) */
    vc = vc_new(a);
    VCTerm *x = V(vc, "x");
    vc_add_hyp(vc, lt(vc, vc_int(vc, 0), x));
    vc_set_goal(vc, lt(vc, vc_int(vc, 0), x));
    ok(refine_s0_decide(vc, a).verdict == RT_VALID, "S0: goal is a hypothesis");
}

static void test_s2_linear(Arena *a) {
    /* x > 0 |- 2x > 0 -- needs the strict-to-non-strict integer tightening. */
    RefineVC *vc = vc_new(a);
    VCTerm *x = V(vc, "x");
    vc_add_hyp(vc, lt(vc, vc_int(vc, 0), x));
    vc_set_goal(vc, lt(vc, vc_int(vc, 0), mul(vc, x, vc_int(vc, 2))));
    ok(decide(vc, a) == RT_VALID, "S2: x > 0 |- 2x > 0");

    /* 0 <= i, i < n |- i + 1 <= n  (the array-index shape; difference logic) */
    vc = vc_new(a);
    VCTerm *i = V(vc, "i"), *n = V(vc, "n");
    vc_add_hyp(vc, le(vc, vc_int(vc, 0), i));
    vc_add_hyp(vc, lt(vc, i, n));
    vc_set_goal(vc, le(vc, add(vc, i, vc_int(vc, 1)), n));
    ok(decide(vc, a) == RT_VALID, "S2: 0 <= i < n |- i+1 <= n");

    /* x >= 0.0 |- 0.5x >= 0.0  over the reals */
    vc = vc_new(a);
    VCTerm *r = R(vc, "r");
    vc_add_hyp(vc, le(vc, vc_real(vc, 0.0), r));
    vc_set_goal(vc, le(vc, vc_real(vc, 0.0), mul(vc, r, vc_real(vc, 0.5))));
    ok(decide(vc, a) == RT_VALID, "S2: x >= 0.0 |- 0.5x >= 0.0");

    /* SOUNDNESS: x >= 0 does NOT entail x > 0. */
    vc = vc_new(a);
    VCTerm *y = V(vc, "y");
    vc_add_hyp(vc, le(vc, vc_int(vc, 0), y));
    vc_set_goal(vc, lt(vc, vc_int(vc, 0), y));
    ok(decide(vc, a) != RT_VALID, "S2 soundness: x >= 0 does not entail x > 0");

    /* SOUNDNESS: no hypotheses at all proves nothing about a variable. */
    vc = vc_new(a);
    vc_set_goal(vc, lt(vc, vc_int(vc, 0), V(vc, "z")));
    ok(decide(vc, a) != RT_VALID, "S2 soundness: unconstrained z is not positive");
}

static void test_s1_euf(Arena *a) {
    /* len(v) = n, i < n |- i < len(v).
     * `len` is an uninterpreted function -- never unfolded. */
    RefineVC *vc = vc_new(a);
    uint32_t len = vc_declare_ufunc(vc, "len", 1, VS_INT, NULL, false);
    VCTerm *v = V(vc, "v"), *n = V(vc, "n"), *i = V(vc, "i");
    VCTerm *args[1] = { v };
    VCTerm *lenv = vc_app(vc, len, args, 1);
    vc_add_hyp(vc, eq(vc, lenv, n));
    vc_add_hyp(vc, lt(vc, i, n));
    vc_set_goal(vc, lt(vc, i, lenv));
    ok(decide(vc, a) == RT_VALID, "S1/S3: len(v) = n, i < n |- i < len(v)");

    /* Congruence: a = b |- f(a) = f(b). */
    vc = vc_new(a);
    uint32_t f = vc_declare_ufunc(vc, "f", 1, VS_INT, NULL, false);
    VCTerm *aa = V(vc, "a"), *bb = V(vc, "b");
    VCTerm *fa_args[1] = { aa }, *fb_args[1] = { bb };
    vc_add_hyp(vc, eq(vc, aa, bb));
    vc_set_goal(vc, eq(vc, vc_app(vc, f, fa_args, 1), vc_app(vc, f, fb_args, 1)));
    ok(decide(vc, a) == RT_VALID, "S1: congruence a = b |- f(a) = f(b)");

    /* SOUNDNESS: f(a) = f(b) does NOT entail a = b (no injectivity). */
    vc = vc_new(a);
    f = vc_declare_ufunc(vc, "f", 1, VS_INT, NULL, false);
    aa = V(vc, "a"); bb = V(vc, "b");
    VCTerm *g1[1] = { aa }, *g2[1] = { bb };
    vc_add_hyp(vc, eq(vc, vc_app(vc, f, g1, 1), vc_app(vc, f, g2, 1)));
    vc_set_goal(vc, eq(vc, aa, bb));
    ok(decide(vc, a) != RT_VALID, "S1 soundness: uninterpreted functions are not injective");
}

static void test_nonlinear_is_unknown(Arena *a) {
    /* x > 0, y > 0 |- x*y > 0 is TRUE but nonlinear; the product is
     * uninterpreted, so the honest answer is "not proved". */
    RefineVC *vc = vc_new(a);
    uint32_t nl = vc_declare_ufunc(vc, "__nl_mul_i", 2, VS_INT, NULL, true);
    VCTerm *x = V(vc, "x"), *y = V(vc, "y");
    VCTerm *args[2] = { x, y };
    vc_add_hyp(vc, lt(vc, vc_int(vc, 0), x));
    vc_add_hyp(vc, lt(vc, vc_int(vc, 0), y));
    vc_set_goal(vc, lt(vc, vc_int(vc, 0), vc_app(vc, nl, args, 2)));
    ok(decide(vc, a) != RT_VALID, "nonlinear: x*y > 0 is not proved (by design)");
    ok(vc->has_nonlinear, "nonlinear: the VC is flagged for TUR-W0373");
}

static void test_disjunction(Arena *a) {
    /* (x < 0) or (x > 10) |- (x < 1) or (x > 9)  -- needs cube expansion. */
    RefineVC *vc = vc_new(a);
    VCTerm *x = V(vc, "x");
    vc_add_hyp(vc, vc_mk2(vc, VC_OR,
                          lt(vc, x, vc_int(vc, 0)),
                          lt(vc, vc_int(vc, 10), x)));
    vc_set_goal(vc, vc_mk2(vc, VC_OR,
                           lt(vc, x, vc_int(vc, 1)),
                           lt(vc, vc_int(vc, 9), x)));
    ok(decide(vc, a) == RT_VALID, "cubes: disjunctive hypothesis and goal");

    /* SOUNDNESS: a disjunctive hypothesis does not entail either disjunct. */
    vc = vc_new(a);
    x = V(vc, "x");
    vc_add_hyp(vc, vc_mk2(vc, VC_OR,
                          lt(vc, x, vc_int(vc, 0)),
                          lt(vc, vc_int(vc, 10), x)));
    vc_set_goal(vc, lt(vc, x, vc_int(vc, 0)));
    ok(decide(vc, a) != RT_VALID, "cubes soundness: (p or q) does not entail p");
}

static void test_model_search(Arena *a) {
    /* |- x > 0 is invalid, and the search must produce a witness. */
    RefineVC *vc = vc_new(a);
    VCTerm *x = V(vc, "x");
    vc_set_goal(vc, lt(vc, vc_int(vc, 0), x));
    RefineModel *m = refine_model_search(vc, a);
    ok(m != NULL, "model search: finds a counterexample for an unconstrained goal");
    ok(m && m->n == 1 && m->bindings[0].ival <= 0,
       "model search: the witness actually falsifies the goal");

    /* A VALID obligation must yield no counterexample -- this is the direction
     * that would be a soundness bug if it ever fired. */
    vc = vc_new(a);
    x = V(vc, "x");
    vc_add_hyp(vc, lt(vc, vc_int(vc, 0), x));
    vc_set_goal(vc, lt(vc, vc_int(vc, 0), x));
    ok(refine_model_search(vc, a) == NULL,
       "model search: no counterexample for a valid obligation");

    /* Uninterpreted symbols have no fixed meaning -- refuse to guess. */
    vc = vc_new(a);
    uint32_t f = vc_declare_ufunc(vc, "f", 1, VS_INT, NULL, false);
    VCTerm *args[1] = { V(vc, "y") };
    vc_set_goal(vc, lt(vc, vc_int(vc, 0), vc_app(vc, f, args, 1)));
    ok(refine_model_search(vc, a) == NULL,
       "model search: declines VCs containing uninterpreted functions");
}

/* The call-site shape: a closed goal (every term a literal) decides outright,
 * which is what makes `(safe-div 10 0)` a compile error rather than a shrug. */
static void test_closed_goal(Arena *a) {
    RefineVC *vc = vc_new(a);
    /* (not= 0 0) -- the predicate of NonZero with the argument substituted in */
    vc_set_goal(vc, vc_not(vc, eq(vc, vc_int(vc, 0), vc_int(vc, 0))));
    ok(vc->goal->op == VC_FALSE, "closed goal: folds to false at intern time");
    ok(decide(vc, a) != RT_VALID, "closed goal: a false goal is never proved");
    RefineModel *m = refine_model_search(vc, a);
    ok(m != NULL, "closed goal: the search decides it (zero variables)");
    ok(m && m->n == 0, "closed goal: the model is empty -- nothing to bind");

    /* The satisfied counterpart must NOT produce a counterexample. */
    vc = vc_new(a);
    vc_set_goal(vc, vc_not(vc, eq(vc, vc_int(vc, 2), vc_int(vc, 0))));
    ok(vc->goal->op == VC_TRUE, "closed goal: (not= 2 0) folds to true");
    ok(refine_s0_decide(vc, a).verdict == RT_VALID, "closed goal: S0 proves it");
    ok(refine_model_search(vc, a) == NULL,
       "closed goal: no counterexample for a satisfied predicate");
}

/* RT6: the hint search must offer a fact that genuinely discharges the goal,
 * and must never offer one that contradicts what is already known -- a
 * contradictory hypothesis "proves" anything by ex falso, so without the
 * satisfiability guard the search would happily suggest constraining a
 * variable to be both negative and positive. */
static void test_hint_search(Arena *a) {
    char cand[128], decl[128];
    const char *var = NULL;
    bool is_real = false;

    /* |- x > 0, unconstrained: the obvious strengthening is the answer. */
    RefineVC *vc = vc_new(a);
    VCTerm *x = V(vc, "x");
    vc_set_goal(vc, lt(vc, vc_int(vc, 0), x));
    ok(refine_hint_search(vc, a, cand, sizeof(cand), decl, sizeof(decl),
                          &var, &is_real),
       "hint: finds a strengthening for an unconstrained goal");
    ok(strcmp(cand, "(> x 0)") == 0, "hint: names the fact in the variable's own name");
    ok(strcmp(decl, "(> v 0)") == 0, "hint: renders a declaration in the bound variable");

    /* An INDEX bound needs a relation between two variables -- no literal
     * comparison can express `(< i n)`. */
    vc = vc_new(a);
    VCTerm *i = V(vc, "i"), *n = V(vc, "n");
    vc_set_goal(vc, lt(vc, i, n));
    ok(refine_hint_search(vc, a, cand, sizeof(cand), decl, sizeof(decl),
                          &var, &is_real),
       "hint: finds a variable-vs-variable strengthening");
    ok(strcmp(cand, "(< i n)") == 0, "hint: suggests the index bound itself");

    /* THE ONE THAT MATTERS: x < 0 is already known and the goal needs x > 0.
     * Every candidate strong enough to discharge the goal contradicts the
     * hypothesis, so nothing may be offered. */
    vc = vc_new(a);
    x = V(vc, "x");
    vc_add_hyp(vc, lt(vc, x, vc_int(vc, 0)));
    vc_set_goal(vc, lt(vc, vc_int(vc, 0), x));
    ok(!refine_hint_search(vc, a, cand, sizeof(cand), decl, sizeof(decl),
                           &var, &is_real),
       "hint: never suggests a candidate that contradicts the hypotheses");

    /* An already-valid obligation has nothing to suggest either. */
    vc = vc_new(a);
    x = V(vc, "x");
    vc_add_hyp(vc, lt(vc, vc_int(vc, 0), x));
    vc_set_goal(vc, lt(vc, vc_int(vc, 0), x));
    ok(!refine_hint_search(vc, a, cand, sizeof(cand), decl, sizeof(decl),
                           &var, &is_real),
       "hint: offers nothing when the goal already holds");
}

static void test_smtlib(Arena *a) {
    RefineVC *vc = vc_new(a);
    VCTerm *x = V(vc, "x");
    vc_add_hyp(vc, lt(vc, vc_int(vc, 0), x));
    vc_set_goal(vc, lt(vc, vc_int(vc, 0), mul(vc, x, vc_int(vc, 2))));

    Buf b; buf_init(&b);
    refine_smtlib_emit(vc, &b);
    buf_putc(&b, '\0');
    ok(b.data && strstr(b.data, "(set-logic QF_UFLIA)") != NULL,
       "smtlib: integer VC selects QF_UFLIA");
    ok(b.data && strstr(b.data, "(declare-const x Int)") != NULL,
       "smtlib: declares each variable");
    ok(b.data && strstr(b.data, "(assert (not ") != NULL,
       "smtlib: the goal is asserted NEGATED (unsat => valid)");
    buf_free(&b);

    vc = vc_new(a);
    VCTerm *r = R(vc, "r");
    vc_set_goal(vc, le(vc, vc_real(vc, 0.0), r));
    buf_init(&b);
    refine_smtlib_emit(vc, &b);
    buf_putc(&b, '\0');
    ok(b.data && strstr(b.data, "QF_UFLRA") != NULL,
       "smtlib: a real-sorted VC selects QF_UFLRA");
    buf_free(&b);
}

/* SX3: euf_mark / euf_undo_to.  The property is that undo restores the
 * PARTITION, not merely the term count: an equality asserted after the mark
 * must stop holding after undo, one asserted before must keep holding, and
 * the unsat flag must roll back.  Nested marks exercise the monotonic-level
 * discipline (trail_c.h) whose failure mode is a silently-skipped trail entry
 * -- precisely what the runtime trail shipped and had to fix. */
static void test_euf_mark_undo(Arena *a) {
    RefineVC *vc = vc_new(a);
    VCTerm *x = V(vc, "x"), *y = V(vc, "y"), *z = V(vc, "z");
    EufState *st = euf_new(vc, a);

    /* Base fact, below every mark. */
    ok(euf_assert_eq(st, x, y), "SX3: base x = y asserts");

    EufMark m1 = euf_mark(st);
    ok(euf_assert_eq(st, y, z), "SX3: y = z asserts above mark");
    ok(euf_equal(st, x, z), "SX3: x ~ z holds via transitivity");

    /* Nested mark: assert a conflict, roll back only to m2. */
    EufMark m2 = euf_mark(st);
    VCTerm *c3 = vc_int(vc, 3), *c5 = vc_int(vc, 5);
    (void)euf_assert_eq(st, x, c3);
    bool conflicted = !euf_assert_eq(st, y, c5);   /* 3 = 5: contradiction */
    ok(conflicted, "SX3: distinct literals conflict above m2");
    euf_undo_to(st, m2);
    ok(euf_equal(st, x, z), "SX3: undo-to-m2 keeps x ~ z");
    ok(!euf_equal(st, x, c3), "SX3: undo-to-m2 forgets x = 3");

    /* After the conflict was undone, the state must be usable again: the
     * unsat flag rolled back with the mark. */
    ok(euf_assert_eq(st, x, c3), "SX3: state usable after conflict undo");
    euf_undo_to(st, m2);

    euf_undo_to(st, m1);
    ok(euf_equal(st, x, y), "SX3: undo-to-m1 keeps the base fact");
    ok(!euf_equal(st, y, z), "SX3: undo-to-m1 forgets y = z");
    ok(!euf_equal(st, x, z), "SX3: undo-to-m1 forgets x ~ z");

    /* The stale-stamp trap, hit precisely.  It only bites when a write lands
     * on a slot that (a) was registered BELOW the mark, so undo does not
     * truncate it away and re-registration cannot refresh its stamp, and
     * (b) was stamped between the previous mark and its undo.  A first draft
     * of this test asserted y = z here instead -- undo had truncated z, the
     * re-assertion re-registered it with a fresh stamp, and the
     * no-level-bump mutation sailed through.  So: register two fresh vars
     * below the marks (euf_equal registers without writing), write them
     * under one mark, undo, and write them again under a second mark at the
     * same depth.  If marks reuse levels, the second write is not trailed
     * and the final undo silently keeps the union. */
    VCTerm *p = V(vc, "p"), *q = V(vc, "q");
    (void)euf_equal(st, p, q);              /* register below the marks */
    EufMark mA = euf_mark(st);
    ok(euf_assert_eq(st, p, q), "SX3: p = q under mark A");
    euf_undo_to(st, mA);
    ok(!euf_equal(st, p, q), "SX3: undo A forgets p = q");
    EufMark mB = euf_mark(st);
    ok(euf_assert_eq(st, p, q), "SX3: p = q under mark B (same depth)");
    euf_undo_to(st, mB);
    ok(!euf_equal(st, p, q),
       "SX3: undo B forgets p = q (stale-stamp trap: stamped slots reused)");
}

int main(void) {
    Arena a;
    arena_init(&a, 1 << 20);

    test_hash_consing(&a);
    test_euf_mark_undo(&a);
    test_constant_folding(&a);
    test_s0_trivial(&a);
    test_s2_linear(&a);
    test_s1_euf(&a);
    test_nonlinear_is_unknown(&a);
    test_disjunction(&a);
    test_model_search(&a);
    test_closed_goal(&a);
    test_hint_search(&a);
    test_smtlib(&a);

    arena_free(&a);

    printf("refine_solver: %d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
