/* SB0: sandbox eval test harness.
 *
 * Evaluates each fixture in tests/fixtures/sandbox/ using a sandboxed
 * environment and asserts that every result is TURI_ERROR.  After SB1-SB4
 * land, all fixtures must pass; until then, only the already-blocked cases
 * (inline-C, println, dlopen, async) will pass.
 *
 * Compile (via CMake):
 *   cmake --build build --target tur_eval_sandbox
 *   ./build/tur_eval_sandbox
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turi/eval.h"

static int failures = 0;
static int passes   = 0;

static void check_error(const char *fixture, TuriValue got) {
    if (got.tag == TURI_ERROR) {
        printf("PASS [%s] => error: %s\n", fixture, got.as_error ? got.as_error : "(null)");
        passes++;
    } else {
        char repr[128];
        turi_value_repr(repr, sizeof(repr), got);
        fprintf(stderr, "FAIL [%s]: expected TURI_ERROR, got tag %d (%s)\n",
                fixture, got.tag, repr);
        failures++;
    }
}

/* Run a single fixture file in a fresh sandboxed environment. */
static void run_fixture(const char *path, const char *name) {
    TuriEnv *env = turi_env_new_sandboxed();
    if (!env) {
        fprintf(stderr, "FAIL [%s]: turi_env_new_sandboxed returned NULL\n", name);
        failures++;
        return;
    }
    TuriValue result = turi_eval_file(env, path);
    check_error(name, result);
    turi_env_free(env);
}

/* Run a mixed-caps fixture: sandboxed baseline + async re-enabled. */
static void run_mixed_caps_test(void) {
    TuriEnv *env = turi_env_new_sandboxed();
    if (!env) { fprintf(stderr, "FAIL [mixed-caps]: env alloc\n"); failures++; return; }

    /* Re-enable async; I/O must still be denied. */
    turi_env_allow(env, TURI_CAP_ASYNC);

    /* async should succeed (returns a future, not an error) */
    TuriValue r1 = turi_eval(env, "(async (fn [] :int 42))");
    if (r1.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [mixed-caps/async-allowed]: unexpected error: %s\n",
                r1.as_error ? r1.as_error : "(null)");
        failures++;
    } else {
        printf("PASS [mixed-caps/async-allowed] => tag %d\n", r1.tag);
        passes++;
    }

    /* I/O must still be blocked */
    TuriValue r2 = turi_eval(env, "(println-int 1)");
    if (r2.tag == TURI_ERROR) {
        printf("PASS [mixed-caps/io-denied] => error: %s\n",
               r2.as_error ? r2.as_error : "(null)");
        passes++;
    } else {
        fprintf(stderr, "FAIL [mixed-caps/io-denied]: expected error, got tag %d\n", r2.tag);
        failures++;
    }

    turi_env_free(env);
}

/* Verify turi_env_set_fuel and turi_env_set_max_depth are callable. */
static void run_api_smoke_tests(void) {
    TuriEnv *env = turi_env_new_sandboxed();
    if (!env) { fprintf(stderr, "FAIL [api-smoke]: env alloc\n"); failures++; return; }

    /* Setting fuel to a large value and running a short program must succeed. */
    turi_env_set_fuel(env, 1000000u);
    turi_env_allow(env, TURI_CAP_IO); /* allow I/O so println works */
    TuriValue r = turi_eval(env, "(+ 1 2)");
    if (r.tag == TURI_INT && r.as_int == 3) {
        printf("PASS [api-smoke/set-fuel]\n");
        passes++;
    } else {
        fprintf(stderr, "FAIL [api-smoke/set-fuel]: expected 3, got tag %d\n", r.tag);
        failures++;
    }

    /* Setting depth and running a shallow call must succeed. */
    turi_env_set_max_depth(env, 512);
    TuriValue r2 = turi_eval(env, "(+ 1 1)");
    if (r2.tag == TURI_INT && r2.as_int == 2) {
        printf("PASS [api-smoke/set-depth]\n");
        passes++;
    } else {
        fprintf(stderr, "FAIL [api-smoke/set-depth]: expected 2, got tag %d\n", r2.tag);
        failures++;
    }

    turi_env_free(env);
}

int main(void) {
    turi_init(false);

    /* Locate fixture directory relative to working directory (project root). */
    const char *fixture_dir = "tests/fixtures/sandbox";

    struct { const char *name; const char *file; } fixtures[] = {
        { "sb-inline-c",    "sb-inline-c.tur"    },
        { "sb-println",     "sb-println.tur"      },
        { "sb-dlopen",      "sb-dlopen.tur"       },
        { "sb-async",       "sb-async.tur"        },
        { "sb-raw-malloc",  "sb-raw-malloc.tur"   },
        { "sb-raw-free",    "sb-raw-free.tur"     },
        { "sb-ptr-deref",   "sb-ptr-deref.tur"    },
        { "sb-ptr-write",   "sb-ptr-write.tur"    },
        { "sb-ptr-arith",   "sb-ptr-arith.tur"    },
        { "sb-raw-memset",  "sb-raw-memset.tur"   },
        { "sb-raw-memcpy",  "sb-raw-memcpy.tur"   },
        { "sb-unsafe-cast", "sb-unsafe-cast.tur"  },
        { "sb-reinterpret", "sb-reinterpret.tur"  },
        { "sb-transmute",   "sb-transmute.tur"    },
        { "sb-import",      "sb-import.tur"       },
        { "sb-step-limit",  "sb-step-limit.tur"   },
        { "sb-depth-limit", "sb-depth-limit.tur"  },
    };

    size_t n = sizeof(fixtures) / sizeof(fixtures[0]);
    for (size_t i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", fixture_dir, fixtures[i].file);
        run_fixture(path, fixtures[i].name);
    }

    /* SB4: mixed-caps test */
    run_mixed_caps_test();

    /* API smoke tests */
    run_api_smoke_tests();

    printf("\nsummary: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
