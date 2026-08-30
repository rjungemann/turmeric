/* TY4: unit tests for lifetime elision (rules 1/2/3, bound to params) and
 * outlives-constraint cycle detection.
 *
 * These exercise the lifetime machinery directly: the surface language does not
 * yet attach `'a` annotations to types (see docs/lifetime-syntax-plan.md), so
 * elision is driven here on Type values constructed in-test.  This pins the
 * rule behavior and the cycle-safe solver so the infrastructure is correct and
 * regression-protected. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "types.h"
#include "lifetimes.h"
#include "lifetime_elision.h"
#include "lsp_sym.h"

/* The tur_core object set includes lsp/lsp.c, whose run_doc_analysis()
 * references tur_collect_symbols(), normally defined in main.c.  The unit-test
 * binaries don't link main.c, so we provide a stub here (matching the other
 * tur_core-based unit tests, e.g. tests/compiler/reinterpret-codegen.c). */
int tur_collect_symbols(const char *source_path, const char *logical_path,
                        LspSymbol *out, int cap, int *count_out) {
    (void)logical_path;
    (void)source_path; (void)out; (void)cap;
    if (count_out) *count_out = 0;
    return 0;
}

/* Build a fresh &T immutable borrow Type with no lifetime. */
static Type borrow_immut(TypeKind target) {
    return type_ref_immut(target);
}

static void test_rule1_binds_each_param(void) {
    /* Two borrow params, no explicit lifetimes -> each gets its own fresh
     * lifetime, bound onto the param Type.  Return is a non-borrow (int): no
     * output lifetime assigned. */
    Type params[2] = { borrow_immut(TY_INT), borrow_immut(TY_INT) };
    Type ret = type_simple(TY_INT, CK_COPY);
    LifetimeContext ctx;
    uint8_t n = lifetime_elision_apply(&ctx, params, 2, &ret);
    assert(n == 2 && "rule 1: two distinct input lifetimes");
    assert(params[0].n_lifetimes == 1 && params[0].lifetimes[0] != LIFETIME_NONE);
    assert(params[1].n_lifetimes == 1 && params[1].lifetimes[0] != LIFETIME_NONE);
    assert(params[0].lifetimes[0] != params[1].lifetimes[0] &&
           "rule 1: param lifetimes are distinct");
    assert(ret.n_lifetimes == 0 && "non-borrow return gets no lifetime");
    printf("ok rule1_binds_each_param\n");
}

static void test_rule2_single_input_to_output(void) {
    /* One borrow param, borrow return -> the sole input lifetime flows to the
     * elided output. */
    Type params[1] = { borrow_immut(TY_INT) };
    Type ret = borrow_immut(TY_INT);
    LifetimeContext ctx;
    uint8_t n = lifetime_elision_apply(&ctx, params, 1, &ret);
    assert(n == 1);
    assert(params[0].n_lifetimes == 1);
    assert(ret.n_lifetimes == 1 && "rule 2: output lifetime assigned");
    assert(ret.lifetimes[0] == params[0].lifetimes[0] &&
           "rule 2: output lifetime == sole input lifetime");
    printf("ok rule2_single_input_to_output\n");
}

static void test_rule3_self_receiver(void) {
    /* First param is a borrow (receiver), plus a second borrow param, borrow
     * return.  Rule 3 wins over Rule 2: the output takes the first param's
     * lifetime even though there are two input lifetimes. */
    Type params[2] = { borrow_immut(TY_INT), borrow_immut(TY_INT) };
    Type ret = borrow_immut(TY_INT);
    LifetimeContext ctx;
    uint8_t n = lifetime_elision_apply(&ctx, params, 2, &ret);
    assert(n == 2);
    assert(ret.n_lifetimes == 1);
    assert(ret.lifetimes[0] == params[0].lifetimes[0] &&
           "rule 3: output lifetime == receiver (first param) lifetime");
    printf("ok rule3_self_receiver\n");
}

static void test_no_input_no_lifetimes(void) {
    /* Non-borrow params and return -> no lifetimes created or assigned. */
    Type params[2] = { type_simple(TY_INT, CK_COPY), type_simple(TY_BOOL, CK_COPY) };
    Type ret = type_simple(TY_INT, CK_COPY);
    LifetimeContext ctx;
    uint8_t n = lifetime_elision_apply(&ctx, params, 2, &ret);
    assert(n == 0);
    assert(ret.n_lifetimes == 0);
    printf("ok no_input_no_lifetimes\n");
}

static void test_outlives_transitive(void) {
    /* a: b, b: c  =>  a outlives c (transitively), c does not outlive a. */
    LifetimeContext ctx;
    lifetime_context_init(&ctx);
    LifetimeId a = lifetime_context_add(&ctx);
    LifetimeId b = lifetime_context_add(&ctx);
    LifetimeId c = lifetime_context_add(&ctx);
    lifetime_context_add_constraint(&ctx, a, b);
    lifetime_context_add_constraint(&ctx, b, c);
    assert(lifetime_outlives(&ctx, a, c) && "transitive outlives");
    assert(!lifetime_outlives(&ctx, c, a) && "no reverse outlives");
    LifetimeId x, y;
    assert(!lifetime_has_cycle(&ctx, &x, &y) && "acyclic graph has no cycle");
    printf("ok outlives_transitive\n");
}

static void test_cycle_detection(void) {
    /* a: b, b: a  =>  a cycle; lifetime_outlives must terminate (cycle-safe)
     * and lifetime_has_cycle must report it. */
    LifetimeContext ctx;
    lifetime_context_init(&ctx);
    LifetimeId a = lifetime_context_add(&ctx);
    LifetimeId b = lifetime_context_add(&ctx);
    lifetime_context_add_constraint(&ctx, a, b);
    lifetime_context_add_constraint(&ctx, b, a);
    /* These calls must return (not infinite-loop). */
    (void)lifetime_outlives(&ctx, a, b);
    (void)lifetime_outlives(&ctx, b, a);
    LifetimeId x = LIFETIME_NONE, y = LIFETIME_NONE;
    assert(lifetime_has_cycle(&ctx, &x, &y) && "2-cycle detected");
    assert(x != LIFETIME_NONE && y != LIFETIME_NONE);
    printf("ok cycle_detection\n");
}

static void test_intern_name_to_id(void) {
    /* LS0: a repeated lifetime name interns to one ID; a distinct name gets a
     * different ID.  String identity (not pointer identity) drives the match. */
    LifetimeContext ctx;
    lifetime_context_init(&ctx);
    char a1[] = "'a";
    char a2[] = "'a";   /* same spelling, different storage */
    char b[]  = "'b";
    LifetimeId ida  = lifetime_context_intern(&ctx, a1);
    LifetimeId ida2 = lifetime_context_intern(&ctx, a2);
    LifetimeId idb  = lifetime_context_intern(&ctx, b);
    assert(ida != LIFETIME_NONE);
    assert(ida == ida2 && "repeated 'a interns to the same ID");
    assert(idb != ida && "distinct 'b gets a fresh ID");
    assert(ctx.count == 2 && "only two distinct lifetimes were interned");
    /* A second pass over the same names is still idempotent. */
    assert(lifetime_context_intern(&ctx, b) == idb);
    assert(ctx.count == 2);
    printf("ok intern_name_to_id\n");
}

static void test_explicit_lifetime_kept(void) {
    /* LS3: a borrow param that already carries an explicit lifetime keeps it --
     * elision's Rule 1 must not overwrite it with a fresh one, and an explicit
     * output lifetime equal to the input is preserved (not reassigned). */
    Type params[1] = { type_ref_immut_lifetime(TY_INT, 7) };
    Type ret = type_ref_immut_lifetime(TY_INT, 7);
    LifetimeContext ctx;
    lifetime_elision_apply(&ctx, params, 1, &ret);
    assert(params[0].n_lifetimes == 1 && params[0].lifetimes[0] == 7 &&
           "explicit input lifetime is not clobbered by Rule 1");
    assert(ret.n_lifetimes == 1 && ret.lifetimes[0] == 7 &&
           "explicit output lifetime is preserved");
    /* No spurious self-constraint -> no cycle. */
    LifetimeId x, y;
    assert(!lifetime_has_cycle(&ctx, &x, &y) &&
           "explicit input==output lifetime is not a cycle");
    printf("ok explicit_lifetime_kept\n");
}

static void test_self_cycle(void) {
    /* a: a is a trivial self-cycle.  Detection must terminate either way. */
    LifetimeContext ctx;
    lifetime_context_init(&ctx);
    LifetimeId a = lifetime_context_add(&ctx);
    lifetime_context_add_constraint(&ctx, a, a);
    LifetimeId x, y;
    (void)lifetime_has_cycle(&ctx, &x, &y);
    printf("ok self_cycle\n");
}

int main(void) {
    test_rule1_binds_each_param();
    test_rule2_single_input_to_output();
    test_rule3_self_receiver();
    test_no_input_no_lifetimes();
    test_outlives_transitive();
    test_cycle_detection();
    test_intern_name_to_id();
    test_explicit_lifetime_kept();
    test_self_cycle();
    printf("all lifetime unit tests passed\n");
    return 0;
}
