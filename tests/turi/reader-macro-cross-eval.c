/* TR2.0 (turi-incremental-elaboration-design.md): cross-eval reader-macro
 * regression coverage.
 *
 * A `(reader-macros/define ...)` directive registers into env->reader_macros
 * (persistent), but the macro's `template` is a Form* that lives in the eval
 * arena where the define was PARSED. Today cross-eval reader macros also lean on
 * src_acc replaying the define every turn (reader_macros.c:144-147). The TR2
 * incremental-elaboration change stops re-parsing prior source, so the define
 * will NOT be re-run on later evals -- the macro must keep working purely from
 * the persisted registration plus the retained defining arena.
 *
 * This test pins that contract BEFORE the change: define a reader macro in one
 * turi_eval, use it in separate later evals, across many intervening evals, and
 * after redefinition. It must stay green through TR2.1--TR2.4.
 *
 * Built via the tur_reader_macro_cross_eval CMake target. Runs with
 * ASAN_OPTIONS=detect_leaks=0 (the interpreter is process-lifetime).
 */
#include <stdio.h>
#include <stdlib.h>

#include "turi/eval.h"

static int failures = 0;

static void check_int(const char *what, int64_t expected, TuriValue got) {
    if (got.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [%s]: error: %s\n", what, got.as_error ? got.as_error : "?");
        failures++;
        return;
    }
    if (got.tag != TURI_INT) {
        fprintf(stderr, "FAIL [%s]: expected int, got tag %d\n", what, got.tag);
        failures++;
        return;
    }
    if (got.as_int != expected) {
        fprintf(stderr, "FAIL [%s]: expected %lld, got %lld\n",
                what, (long long)expected, (long long)got.as_int);
        failures++;
        return;
    }
    printf("PASS [%s] => %lld\n", what, (long long)got.as_int);
}

static void must_ok(TuriEnv *env, const char *src) {
    TuriValue v = turi_eval(env, src);
    if (v.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL setup [%s]: %s\n", src, v.as_error ? v.as_error : "?");
        failures++;
    }
}

int main(void) {
    TuriEnv *env = turi_env_new();

    /* Eval 1: define reader macros (a bare constant and an expression template),
     * each in its own turi_eval call. */
    must_ok(env, "(reader-macros/define 'zero :none '0)");
    must_ok(env, "(reader-macros/define 'two-plus-three :none '(+ 2 3))");

    /* Eval 2: use them in SEPARATE evals -- the macros must be visible via the
     * persisted registry, not because this source re-declares them. */
    check_int("#zero (separate eval)",            0, turi_eval(env, "#zero"));
    check_int("(+ #zero 41)",                    41, turi_eval(env, "(+ #zero 41)"));
    check_int("#two-plus-three (separate eval)",  5, turi_eval(env, "#two-plus-three"));

    /* Many intervening evals: this is the incremental case -- the define is not
     * re-run, so the macro must survive on the persisted registration + retained
     * defining arena alone. */
    for (int i = 0; i < 50; i++) must_ok(env, "(let [z 0] z)");
    check_int("#zero after 50 intervening evals",           0, turi_eval(env, "#zero"));
    check_int("#two-plus-three after 50 intervening evals", 5, turi_eval(env, "#two-plus-three"));

    /* Redefinition in a later eval updates the macro in place. */
    must_ok(env, "(reader-macros/define 'zero :none '99)");
    check_int("#zero after redefinition", 99, turi_eval(env, "#zero"));

    turi_env_free(env);

    if (failures == 0) {
        printf("\nAll reader-macro cross-eval tests passed.\n");
        return 0;
    }
    fprintf(stderr, "\n%d reader-macro cross-eval test(s) FAILED.\n", failures);
    return 1;
}
