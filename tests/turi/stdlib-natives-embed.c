/* turi-interp-stdlib-natives-libturi: C embedding parity test for the stdlib
 * inline-C native overrides (option / result / str / math and friends).
 *
 * These natives -- like the Vec/Set/Map collection natives before them -- used
 * to be registered only by the `tur` CLI's command handlers (main.c), so a
 * program driven through the `turi_eval` C API (which links libturi, not
 * main.c) hit "unknown function" / "inline-C not supported in interpreter mode"
 * the moment it touched an op whose stdlib body is inline-C with a native
 * override (option-unwrap-or, result-unwrap-or, sqrt, c-abs, int->str, ...).
 *
 * After turi_env_register_interpreter_natives is auto-invoked from
 * turi_env_new, every libturi consumer -- the `tur` binary, turi_eval
 * embedders, the WASM REPL, and the test harnesses -- resolves the same
 * overrides.  This harness exercises that path end-to-end on a pristine env; it
 * fails to even reach a value before the change (e.g. `(c-abs -5)` reports
 * "unknown function or operator 'c-abs'").
 *
 * Compile with:
 *   cmake --build build --target tur_stdlib_natives_embed
 *   ./build/tur_stdlib_natives_embed
 *
 * Runs with ASAN_OPTIONS=detect_leaks=0: the tree-walking interpreter's
 * carrier allocations are process-lifetime, matching the other turi harnesses'
 * leak policy.
 */

#include <stdio.h>

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

static void check_float(const char *expr, double expected, TuriValue got) {
    if (got.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [%s]: error: %s\n", expr, got.as_error);
        failures++;
        return;
    }
    if (got.tag != TURI_FLOAT) {
        fprintf(stderr, "FAIL [%s]: expected float, got tag %d\n", expr, got.tag);
        failures++;
        return;
    }
    if (got.as_float != expected) {
        fprintf(stderr, "FAIL [%s]: expected %g, got %g\n",
                expr, expected, got.as_float);
        failures++;
        return;
    }
    printf("PASS [%s] => %g\n", expr, got.as_float);
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

static void check_cstr(const char *expr, const char *expected, TuriValue got) {
    if (got.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [%s]: error: %s\n", expr, got.as_error);
        failures++;
        return;
    }
    if (got.tag != TURI_CSTR) {
        fprintf(stderr, "FAIL [%s]: expected cstr, got tag %d\n", expr, got.tag);
        failures++;
        return;
    }
    if (!got.as_cstr) {
        fprintf(stderr, "FAIL [%s]: expected \"%s\", got NULL\n", expr, expected);
        failures++;
        return;
    }
    {
        const char *a = expected, *b = got.as_cstr;
        while (*a && *a == *b) { a++; b++; }
        if (*a != *b) {
            fprintf(stderr, "FAIL [%s]: expected \"%s\", got \"%s\"\n",
                    expr, expected, got.as_cstr);
            failures++;
            return;
        }
    }
    printf("PASS [%s] => \"%s\"\n", expr, got.as_cstr);
}

int main(void) {
    turi_init(false);

    TuriEnv *env = turi_env_new();
    if (!env) {
        fprintf(stderr, "FATAL: turi_env_new failed\n");
        return 1;
    }

    /* --- math / int helpers (native_c_abs / native_popcount) ---------- */
    check_int("c-abs",   5, turi_eval(env, "(c-abs -5)"));
    check_int("popcount", 3, turi_eval(env, "(popcount 7)"));

    /* --- str conversion (native_int_to_str) --------------------------- */
    check_cstr("int->str", "42", turi_eval(env, "(int->str 42)"));

    /* --- Option (native_some / native_some_pred / native_option_unwrap_or) */
    check_bool("some?",             1, turi_eval(env, "(some? (some 1))"));
    check_int ("option-unwrap-or some", 41,
        turi_eval(env, "(option-unwrap-or (some 41) 0)"));
    check_int ("option-unwrap-or none", 7,
        turi_eval(env, "(option-unwrap-or (none) 7)"));

    /* --- Result (native_ok / native_ok_pred / native_result_unwrap_or) - */
    check_bool("ok?",                1, turi_eval(env, "(ok? (ok 5))"));
    check_int ("result-unwrap-or ok", 5,
        turi_eval(env, "(result-unwrap-or (ok 5) 9)"));
    check_int ("result-unwrap-or err", 9,
        turi_eval(env, "(result-unwrap-or (err 1) 9)"));

    /* --- float math (native sqrt / floor); non-zero fractional part so
     *     truncation/rounding would show up (Testing Float Behavior rule) - */
    check_float("sqrt",  1.5, turi_eval(env, "(sqrt 2.25)"));
    check_float("floor", 3.0, turi_eval(env, "(floor 3.25)"));

    turi_env_free(env);

    if (failures == 0) {
        printf("\nAll stdlib-native embedding tests passed.\n");
        return 0;
    }
    fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
    return 1;
}
