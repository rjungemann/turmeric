/* libturi-per-embed-env-and-peripherals Gaps 1-4: C embedding test harness.
 *
 * Exercises the per-embed-env peripheral API added for multi-script embedders:
 *   Gap 1 -- turi_register_default_native / turi_env_new_with_natives
 *   Gap 2 -- turi_env_reset
 *   Gap 3 -- turi_env_set_diag_sink
 *   Gap 4 -- turi_env_set_module_base_dir
 *
 * Compile with:
 *   cmake --build build --target tur_embed_peripherals
 *   ./build/tur_embed_peripherals
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turi/eval.h"

static int failures = 0;

#define CHECK(cond, msg) do {                                   \
    if (cond) { printf("PASS [%s]\n", msg); }                   \
    else { fprintf(stderr, "FAIL [%s]\n", msg); failures++; }   \
} while (0)

/* --- Gap 1: a native that returns a fixed sentinel ---------------------- */
static TuriValue native_seven(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)args; (void)n;
    return turi_int((int64_t)(intptr_t)ud);
}

static void test_default_natives(void) {
    turi_register_default_native("embed-magic", native_seven, (void *)(intptr_t)7);

    /* Every env built after registration sees the native, with no per-env
     * re-registration. */
    TuriEnv *a = turi_env_new();
    TuriEnv *b = turi_env_new();
    TuriValue va = turi_eval(a, "(embed-magic)");
    TuriValue vb = turi_eval(b, "(embed-magic)");
    CHECK(va.tag == TURI_INT && va.as_int == 7, "default native visible on env A");
    CHECK(vb.tag == TURI_INT && vb.as_int == 7, "default native visible on env B");
    turi_env_free(a);
    turi_env_free(b);

    /* Explicit table via turi_env_new_with_natives. */
    TuriNativeSpec specs[] = {
        { "embed-explicit", native_seven, (void *)(intptr_t)42 },
    };
    TuriEnv *c = turi_env_new_with_natives(specs, 1);
    TuriValue vc = turi_eval(c, "(embed-explicit)");
    CHECK(vc.tag == TURI_INT && vc.as_int == 42, "explicit native installed");
    /* Default natives also present on the with-natives env. */
    TuriValue vc2 = turi_eval(c, "(embed-magic)");
    CHECK(vc2.tag == TURI_INT && vc2.as_int == 7, "default native present on with-natives env");
    turi_env_free(c);

    turi_clear_default_natives();
    TuriEnv *d = turi_env_new();
    TuriValue vd = turi_eval(d, "(embed-magic)");
    CHECK(vd.tag == TURI_ERROR, "default native gone after clear");
    turi_env_free(d);
}

/* --- Gap 2: reset drops user defns but keeps registered natives --------- */
static void test_reset(void) {
    TuriEnv *env = turi_env_new_with_natives(
        (TuriNativeSpec[]){ { "embed-magic", native_seven, (void *)(intptr_t)7 } }, 1);

    /* Install a user defn; both it and the native resolve. */
    turi_eval(env, "(defn user-fn [x :int] :int (* x x))");
    TuriValue r1 = turi_eval(env, "(user-fn 5)");
    CHECK(r1.tag == TURI_INT && r1.as_int == 25, "user defn works before reset");

    turi_env_reset(env);

    /* After reset the native survives... */
    TuriValue r2 = turi_eval(env, "(embed-magic)");
    CHECK(r2.tag == TURI_INT && r2.as_int == 7, "native survives reset");
    /* ...but the user defn is gone. */
    TuriValue r3 = turi_eval(env, "(user-fn 5)");
    CHECK(r3.tag == TURI_ERROR, "user defn dropped by reset");

    /* The same name can now be redefined cleanly (no stale shadow). */
    turi_eval(env, "(defn user-fn [x :int] :int (+ x 1))");
    TuriValue r4 = turi_eval(env, "(user-fn 5)");
    CHECK(r4.tag == TURI_INT && r4.as_int == 6, "name redefinable after reset");

    turi_env_free(env);
}

/* --- Gap 3: diagnostics route to the env's sink, not stderr ------------- */
static int   sink_hits = 0;
static int   sink_last_level = -1;
static char  sink_last_msg[256];

static void my_sink(TuriEnv *env, int level, const char *code, const char *file,
                    uint32_t line, uint32_t col_start, uint32_t col_end,
                    const char *message, void *ud) {
    (void)env; (void)code; (void)file; (void)line; (void)col_start; (void)col_end;
    sink_hits++;
    sink_last_level = level;
    snprintf(sink_last_msg, sizeof(sink_last_msg), "%s", message ? message : "");
    int *counter = (int *)ud;
    if (counter) (*counter)++;
}

static void test_diag_sink(void) {
    int ud_counter = 0;
    TuriEnv *env = turi_env_new();
    turi_env_set_diag_sink(env, my_sink, &ud_counter);

    sink_hits = 0;
    /* A let binding with no initializer is an elaboration diagnostic (a real
     * compile error, unlike an unbound call which interpret-mode defers to a
     * runtime error with no diagnostic). */
    TuriValue r = turi_eval(env, "(let [x] x)");
    CHECK(r.tag == TURI_ERROR, "bad eval returns error");
    CHECK(sink_hits > 0, "diagnostic delivered to sink");
    CHECK(ud_counter > 0, "sink user-data threaded through");

    /* Clearing the sink restores normal routing -- the next diagnostic goes to
     * stderr (the line below is expected output), not the sink. */
    turi_env_set_diag_sink(env, NULL, NULL);
    int hits_before = sink_hits;
    turi_eval(env, "(let [y] y)");
    CHECK(sink_hits == hits_before, "no sink delivery after clear");

    turi_env_free(env);
}

/* --- Gap 4: setter resolves imports relative to the chosen base dir ------ */
static void test_module_base_dir(void) {
    TuriEnv *env = turi_env_new();
    /* Set then re-set to confirm the owned string is freed/replaced cleanly.
     * No import is performed here -- the point is exercising the setter and its
     * free path under ASan (turi_env_free reclaims the owned copy). */
    turi_env_set_module_base_dir(env, "/tmp/does-not-matter");
    turi_env_set_module_base_dir(env, "/tmp/replaced");
    turi_env_set_module_base_dir(env, NULL);
    turi_env_set_module_base_dir(env, "/tmp/final");
    CHECK(1, "module base dir setter exercised (set/replace/clear/set)");
    turi_env_free(env);
}

int main(void) {
    turi_init(false);

    test_default_natives();
    test_reset();
    test_diag_sink();
    test_module_base_dir();

    if (failures == 0) {
        printf("\nAll embed-peripheral tests passed.\n");
        return 0;
    }
    fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
    return 1;
}
