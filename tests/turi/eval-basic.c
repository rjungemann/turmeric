/* Phase S0: C embedding smoke test for libturi.
 *
 * Compile with:
 *   cmake --build build --target tur_eval_basic
 *   ./build/tur_eval_basic
 *
 * Or manually:
 *   cc -std=c11 -I src/ tests/turi/eval-basic.c -o tur_eval_basic \
 *       -L build/ -lturi -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turi/eval.h"

static int failures = 0;

static void check_int(const char *expr, int64_t expected, TuriValue got) {
    if (got.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [%s]: error: %s\n", expr, got.as_error);
        failures++;
        return;
    }
    if (got.tag != TURI_INT) {
        fprintf(stderr, "FAIL [%s]: expected int, got tag %d\n", expr, got.tag);
        failures++;
        return;
    }
    if (got.as_int != expected) {
        fprintf(stderr, "FAIL [%s]: expected %lld, got %lld\n",
                expr, (long long)expected, (long long)got.as_int);
        failures++;
        return;
    }
    printf("PASS [%s] => %lld\n", expr, (long long)got.as_int);
}

static void check_bool(const char *expr, int expected, TuriValue got) {
    if (got.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [%s]: error: %s\n", expr, got.as_error);
        failures++;
        return;
    }
    if (got.tag != TURI_BOOL) {
        fprintf(stderr, "FAIL [%s]: expected bool, got tag %d\n", expr, got.tag);
        failures++;
        return;
    }
    if ((got.as_bool ? 1 : 0) != (expected ? 1 : 0)) {
        fprintf(stderr, "FAIL [%s]: expected %s, got %s\n",
                expr, expected ? "true" : "false",
                got.as_bool ? "true" : "false");
        failures++;
        return;
    }
    printf("PASS [%s] => %s\n", expr, got.as_bool ? "true" : "false");
}

static void check_closure(const char *expr, TuriValue got) {
    if (got.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [%s]: error: %s\n", expr, got.as_error);
        failures++;
        return;
    }
    if (got.tag != TURI_CLOSURE) {
        fprintf(stderr, "FAIL [%s]: expected closure, got tag %d\n", expr, got.tag);
        failures++;
        return;
    }
    char repr[64];
    turi_value_repr(repr, sizeof(repr), got);
    printf("PASS [%s] => %s\n", expr, repr);
}

int main(void) {
    /* Initialise diagnostics (no colour for test output) */
    turi_init(false);

    TuriEnv *env = turi_env_new();
    if (!env) {
        fprintf(stderr, "FATAL: turi_env_new failed\n");
        return 1;
    }

    /* --- Basic arithmetic ------------------------------------------- */
    check_int("(+ 1 2)",        3,  turi_eval(env, "(+ 1 2)"));
    check_int("(- 10 3)",       7,  turi_eval(env, "(- 10 3)"));
    check_int("(* 6 7)",        42, turi_eval(env, "(* 6 7)"));
    check_int("(/ 20 4)",       5,  turi_eval(env, "(/ 20 4)"));
    check_int("(mod 17 5)",     2,  turi_eval(env, "(mod 17 5)"));
    check_int("(+ 1 2 3 4 5)",  15, turi_eval(env, "(+ 1 2 3 4 5)"));

    /* --- Comparisons ------------------------------------------------- */
    check_bool("(< 1 2)",  1, turi_eval(env, "(< 1 2)"));
    check_bool("(> 5 3)",  1, turi_eval(env, "(> 5 3)"));
    check_bool("(= 4 4)",  1, turi_eval(env, "(= 4 4)"));
    check_bool("(= 4 5)",  0, turi_eval(env, "(= 4 5)"));

    /* --- Let --------------------------------------------------------- */
    check_int("(let [x 10] x)",                    10, turi_eval(env, "(let [x 10] x)"));
    check_int("(let [x 3 y 4] (+ x y))",           7,  turi_eval(env, "(let [x 3 y 4] (+ x y))"));

    /* --- If ---------------------------------------------------------- */
    check_int("(if true 1 2)",  1, turi_eval(env, "(if true 1 2)"));
    check_int("(if false 1 2)", 2, turi_eval(env, "(if false 1 2)"));

    /* --- Do ---------------------------------------------------------- */
    check_int("(do 1 2 3)",     3, turi_eval(env, "(do 1 2 3)"));

    /* --- Defn + call (persistent across turi_eval calls) ------------ */
    check_closure("defn square",
        turi_eval(env, "(defn square [x :int] :int (* x x))"));
    check_int("(square 7)",   49, turi_eval(env, "(square 7)"));
    check_int("(square 12)", 144, turi_eval(env, "(square 12)"));

    /* --- Recursive defn --------------------------------------------- */
    turi_eval(env, "(defn fib [n :int] :int"
                   "  (if (< n 2) n"
                   "    (+ (fib (- n 1)) (fib (- n 2)))))");
    check_int("(fib 10)", 55, turi_eval(env, "(fib 10)"));

    turi_env_free(env);

    if (failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
        return 1;
    }
}
