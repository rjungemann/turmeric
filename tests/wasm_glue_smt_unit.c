/* wasm_glue_smt_unit.c -- exercise the playground's solver entry point
 * natively (solver-extension-plan SX8c).
 *
 * `turi_smt_check` normally only runs inside a wasm module, where nothing in
 * CI can look at it -- a mistake would surface as "the playground's solver
 * panel prints nothing", found by a person, later, with an Emscripten
 * toolchain in the way of every diagnosis.  Nothing in wasm_glue.c is
 * wasm-specific, so linking libturi_wasm runs the real thing here, against the
 * real S0..S3 chain that the browser build links.
 *
 * The load-bearing check is not that it answers -- it is that it answers the
 * SAME as `tur smt`.  Two doors onto one solver that disagree would be a
 * second solver wearing the first one's name, and the browser is exactly where
 * nobody would notice.  So every case below is one whose CLI answer is pinned
 * by tests/fixtures/sx8a-tur-smt or sx8b-smt-push-pop.
 *
 * Covered:
 *   - the three answers and their deciding stages;
 *   - `sat` carries a real witness, not just the word (the bounded search is
 *     the only thing allowed to answer INVALID precisely because it produces
 *     one);
 *   - a script outside the fragment is refused WHOLE -- an `error` key and an
 *     EMPTY results array, never a partial parse whose `unsat` would be a
 *     claim about work not done;
 *   - `(push)`/`(pop)` scope assertions, one result per `(check-sat)`, in
 *     script order (the SX8b semantics, reached through this door);
 *   - a script with no `(check-sat)` is still decided once at the end.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "web/wasm_glue.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { passed++; } \
        else { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); failed++; } \
    } while (0)

/* Run a script and test the JSON for a substring.  The result is malloc'd. */
static int smt_contains(const char *script, const char *needle) {
    char *r = turi_smt_check(script);
    int hit = r && strstr(r, needle) != NULL;
    if (!hit && r) fprintf(stderr, "  (got: %s)\n", r);
    free(r);
    return hit;
}

/* Count the `"answer":` keys, i.e. how many (check-sat)s were answered. */
static int smt_answer_count(const char *script) {
    char *r = turi_smt_check(script);
    int n = 0;
    for (const char *p = r; p && (p = strstr(p, "\"answer\":")); p += 9) n++;
    free(r);
    return n;
}

int main(void) {
    turi_wasm_init();

    /* --- unsat: a contradiction the arithmetic stage refutes -------------- */
    CHECK(smt_contains("(set-logic QF_LIA)(declare-fun x () Int)"
                       "(assert (> x 0))(assert (< x 0))(check-sat)",
                       "\"answer\":\"unsat\""),
          "a contradictory assertion set answers unsat");
    CHECK(smt_contains("(set-logic QF_LIA)(declare-fun x () Int)"
                       "(assert (> x 0))(assert (< x 0))(check-sat)",
                       "\"decided_by\":\"S2 (arithmetic)\""),
          "and reports which stage decided it");

    /* --- sat: the witness itself, not just the word ----------------------- */
    CHECK(smt_contains("(set-logic QF_LIA)(declare-fun x () Int)"
                       "(assert (> x 0))(check-sat)",
                       "\"answer\":\"sat\""),
          "a satisfiable assertion set answers sat");
    CHECK(smt_contains("(set-logic QF_LIA)(declare-fun x () Int)"
                       "(assert (> x 0))(check-sat)",
                       "\"name\":\"x\""),
          "sat carries a model binding the asserted variable");
    CHECK(smt_contains("(set-logic QF_LIA)(declare-fun x () Int)"
                       "(assert (> x 0))(check-sat)",
                       "\"sort\":\"Int\""),
          "the witness carries its sort");

    /* --- unknown is a first-class answer, not a failure ------------------- */
    CHECK(smt_contains("(set-logic QF_UFLIA)"
                       "(declare-fun a () Int)(declare-fun b () Int)"
                       "(declare-fun c () Int)(declare-fun d () Int)"
                       "(declare-fun f (Int) Int)"
                       "(assert (> (+ a b c d) 0))(assert (> (f a) 0))(check-sat)",
                       "\"answer\":\"unknown\""),
          "past the bounded search's scope, unknown is the honest answer");

    /* --- outside the fragment: refused WHOLE ------------------------------ */
    CHECK(smt_contains("(set-logic UFLIA)(declare-fun p (Int) Bool)"
                       "(assert (forall ((x Int)) (p x)))(check-sat)",
                       "\"error\":"),
          "a script outside the fragment reports an error");
    CHECK(smt_contains("(set-logic UFLIA)(declare-fun p (Int) Bool)"
                       "(assert (forall ((x Int)) (p x)))(check-sat)",
                       "\"results\":[]"),
          "and returns NO results -- refused whole, never partially parsed");

    /* --- the SX8b assertion stack, through this door ---------------------- */
    {
        const char *stacked =
            "(set-logic QF_LIA)(declare-fun x () Int)"
            "(assert (> x 10))"
            "(check-sat)"          /* sat */
            "(push 1)"
            "(assert (< x 5))"
            "(check-sat)"          /* unsat -- contradicts x > 10 */
            "(pop 1)"
            "(check-sat)";         /* sat again; the contradiction was scoped */
        CHECK(smt_answer_count(stacked) == 3,
              "one result per (check-sat), in script order");
        char *r = turi_smt_check(stacked);
        int ok = 0;
        if (r) {
            const char *a = strstr(r, "\"answer\":");
            const char *b = a ? strstr(a + 9, "\"answer\":") : NULL;
            const char *c = b ? strstr(b + 9, "\"answer\":") : NULL;
            ok = a && b && c &&
                 strncmp(a, "\"answer\":\"sat\"",   14) == 0 &&
                 strncmp(b, "\"answer\":\"unsat\"", 16) == 0 &&
                 strncmp(c, "\"answer\":\"sat\"",   14) == 0;
            if (!ok) fprintf(stderr, "  (got: %s)\n", r);
        }
        CHECK(ok, "pop restores exactly the hypotheses in scope at the push");
        free(r);
    }

    /* An unmatched pop is a refusal, not a guess. */
    CHECK(smt_contains("(pop)", "\"error\":\"pop with no matching push\""),
          "an unmatched pop is refused by name");

    /* --- no (check-sat) at all is still decided once ---------------------- */
    CHECK(smt_answer_count("(set-logic QF_LIA)(declare-fun x () Int)"
                           "(assert (> x 0))(assert (< x 0))") == 1,
          "a script that never asks is decided once at the end");
    CHECK(smt_contains("(set-logic QF_LIA)(declare-fun x () Int)"
                       "(assert (> x 0))(assert (< x 0))",
                       "\"answer\":\"unsat\""),
          "and that single answer is the right one");

    /* --- the envelope ----------------------------------------------------- */
    CHECK(smt_contains("(check-sat)", "\"schema\":0"),
          "every response carries the unstable-schema marker");
    {
        char *r = turi_smt_check(NULL);
        CHECK(r != NULL, "a NULL script returns a string rather than crashing");
        free(r);
    }

    turi_wasm_shutdown();

    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
