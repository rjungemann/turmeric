/* TY4: unit tests for lifetime elision (rules 1/2/3, bound to params) and
 * outlives-constraint cycle detection.
 *
 * These exercise the lifetime machinery directly: the surface language does not
 * yet attach `'a` annotations to types, so elision is driven here on Type
 * values constructed in-test.  This pins the rule behavior and the cycle-safe
 * solver so the infrastructure is correct and regression-protected. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "compiler/types.h"
#include "passes/lifetimes.h"
#include "passes/lifetime_elision.h"

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

static void test_self_cycle(void) {
    /* a: a is a trivial self-cycle. */
    LifetimeContext ctx;
    lifetime_context_init(&ctx);
    LifetimeId a = lifetime_context_add(&ctx);
    lifetime_context_add_constraint(&ctx, a, a); /* dropped? add_constraint allows it */
    /* Even if the self-edge is stored, detection must terminate. */
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
    test_self_cycle();
    printf("all lifetime unit tests passed\n");
    return 0;
}
