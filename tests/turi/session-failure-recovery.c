/* PS1 (playground-session-hygiene-plan): a failed turn must not poison the
 * session for the turns after it.
 *
 * A failed turn discards the persistent elaboration session, because the
 * failed program may already have entered its scope.  The next turn used to
 * rebuild it by elaborating the whole accumulated program in one call with
 * stdlib_prefix = prior_toplevel -- which marks the user's own earlier turns as
 * auto-loaded stdlib.  So after ANY failure, re-running a program was refused
 *
 *     defn: 'main' is already defined by an auto-loaded stdlib module;
 *     rename the local definition
 *
 * and, since that refusal is itself a failure, every run after it was refused
 * the same way.  In Try Turmeric one doc-panel lookup was enough to trigger
 * it (doc-lookup-poisons-the-playground-eval-session).  The session is now
 * rebuilt by replaying the committed turns one at a time.
 *
 * What this checks, for an elaboration failure, a runtime failure, and a
 * sweet-exp session:
 *   - a program re-run after the failure succeeds and its new definition wins;
 *   - no diagnostic anywhere names an auto-loaded stdlib module;
 *   - a recovered session answers exactly like one that never failed, including
 *     for a name the prelude really does define (the interpreter session has
 *     never refused redefining one, so the healthy session is the oracle here,
 *     not a hard-coded error).
 *
 * Built via the tur_session_failure_recovery CMake target.  Runs with
 * ASAN_OPTIONS=detect_leaks=0 (the interpreter is process-lifetime).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turi/eval.h"
#include "turi/interpreter_natives.h"
#include "turi/preload.h"

static int failures = 0;
static int stdlib_blame = 0;   /* diagnostics naming an auto-loaded stdlib module */

static void sink(TuriEnv *env, int level, const char *code, const char *file,
                 uint32_t line, uint32_t col_start, uint32_t col_end,
                 const char *message, void *ud) {
    (void)env; (void)level; (void)code; (void)file; (void)line;
    (void)col_start; (void)col_end; (void)ud;
    if (message && strstr(message, "auto-loaded stdlib module")) {
        fprintf(stderr, "  diagnostic blames the stdlib: %s\n", message);
        stdlib_blame++;
    }
}

/* Mirrors wasm_preload_stdlib (src/web/wasm_glue.c). */
static TuriEnv *new_session(void) {
    TuriEnv *env = turi_env_new();
    turi_env_set_diag_sink(env, sink, NULL);
    turi_env_preload_macros(env, "stdlib");
    turi_env_preload_native_stubs(env);
    turi_env_preload_collections(env, "stdlib");
    turi_env_preload_typeclasses(env, "stdlib");
    turi_env_pin_prelude(env);
    turi_env_register_interpreter_natives(env);
    return env;
}

static void check_int(const char *what, int64_t expected, TuriValue got) {
    if (got.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [%s]: error: %s\n", what, got.as_error ? got.as_error : "?");
        failures++;
    } else if (got.tag != TURI_INT) {
        fprintf(stderr, "FAIL [%s]: expected int, got tag %d\n", what, got.tag);
        failures++;
    } else if (got.as_int != expected) {
        fprintf(stderr, "FAIL [%s]: expected %lld, got %lld\n",
                what, (long long)expected, (long long)got.as_int);
        failures++;
    } else {
        printf("PASS [%s] => %lld\n", what, (long long)got.as_int);
    }
}

static void check_ok(const char *what, TuriValue got) {
    if (got.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [%s]: error: %s\n", what, got.as_error ? got.as_error : "?");
        failures++;
    } else {
        printf("PASS [%s]\n", what);
    }
}

static void check_error(const char *what, TuriValue got) {
    if (got.tag != TURI_ERROR) {
        fprintf(stderr, "FAIL [%s]: expected the turn to fail\n", what);
        failures++;
    } else {
        printf("PASS [%s] fails as intended\n", what);
    }
}

/* Run the same program twice around a failing turn, as the Run button does. */
static void rerun_after(const char *label, const char *failing_turn) {
    TuriEnv *env = new_session();
    char what[160];

    snprintf(what, sizeof what, "%s: first run", label);
    check_ok(what, turi_eval(env, "(defn main [] : int 1)"));
    snprintf(what, sizeof what, "%s: first (main)", label);
    check_int(what, 1, turi_eval(env, "(main)"));

    snprintf(what, sizeof what, "%s: failing turn", label);
    check_error(what, turi_eval(env, failing_turn));
    /* A second failure in a row must not compound the first. */
    snprintf(what, sizeof what, "%s: failing turn again", label);
    check_error(what, turi_eval(env, failing_turn));

    snprintf(what, sizeof what, "%s: re-run after failure", label);
    check_ok(what, turi_eval(env, "(defn main [] : int 2)"));
    snprintf(what, sizeof what, "%s: (main) sees the re-run", label);
    check_int(what, 2, turi_eval(env, "(main)"));
    /* ...and the turn after the recovered one is ordinary again. */
    snprintf(what, sizeof what, "%s: re-run once more", label);
    check_ok(what, turi_eval(env, "(defn main [] : int 3)"));
    snprintf(what, sizeof what, "%s: (main) sees that too", label);
    check_int(what, 3, turi_eval(env, "(main)"));

    turi_env_free(env);
}

/* A recovered session must answer every probe the way a session that never
 * failed does.  Each probe is its own turn.  Redefining the setup turn's `main`
 * goes FIRST: the old whole-program rebuild ran on the first turn after a
 * failure, and a turn that succeeded there rebuilt the session and hid the
 * difference from every probe after it. */
static const char *const PROBES[] = {
    "(defn main [] : int 5)",                    /* defined by an earlier turn */
    "(main)",
    "(defn vec-map [x : int] : int (+ x 1))",   /* defined by the prelude */
    "(vec-map 41)",
    "(defn helper [x : int] : int (* x 10))",
    "(helper 4)",
    "(map-count #map{:a 1 :b 2})",               /* the prelude still resolves */
    "(when true 7)",
    "(defn helper [x : int] : int (* x 100))",
    "(helper 4)",
};

static void render(TuriValue v, char *out, size_t cap) {
    if (v.tag == TURI_ERROR)    snprintf(out, cap, "ERROR");
    else if (v.tag == TURI_INT) snprintf(out, cap, "INT:%lld", (long long)v.as_int);
    else                        snprintf(out, cap, "TAG:%d", v.tag);
}

static void recovered_matches_healthy(void) {
    TuriEnv *healthy   = new_session();
    TuriEnv *recovered = new_session();
    check_ok("oracle: setup", turi_eval(healthy, "(defn main [] : int 1)"));
    check_ok("oracle: setup (recovered)", turi_eval(recovered, "(defn main [] : int 1)"));
    check_error("oracle: failing turn",
                turi_eval(recovered, "(defn broken [] : int (+ 1 \"x\"))"));

    int n = (int)(sizeof(PROBES) / sizeof(PROBES[0]));
    int diverged = 0;
    for (int i = 0; i < n; i++) {
        char a[64], b[64];
        render(turi_eval(healthy, PROBES[i]), a, sizeof a);
        render(turi_eval(recovered, PROBES[i]), b, sizeof b);
        if (strcmp(a, b) != 0) {
            fprintf(stderr, "FAIL [oracle] probe %d diverged: %s\n"
                            "  healthy: %s\n  recovered: %s\n", i, PROBES[i], a, b);
            failures++;
            diverged = 1;
        }
    }
    if (!diverged) printf("PASS [oracle] %d probes identical\n", n);
    turi_env_free(healthy);
    turi_env_free(recovered);
}

int main(void) {
    /* Fails in the elaborator: a type error. */
    rerun_after("elaboration failure", "(defn broken [] : int (+ 1 \"x\"))");
    /* Elaborates (with a TUR-W0040 warning) and fails at runtime -- after the
     * session has already absorbed the turn's `defn`. */
    rerun_after("runtime failure", "(defn half-done [] : int 5)\n(undefined-thing 1)");

    /* A sweet-exp session: incremental PARSING is off for sweet-exp, which the
     * plan flagged as a possible hole in the recovery.  Parsing and
     * elaboration are gated separately, so the replay covers it too. */
    {
        TuriEnv *env = new_session();
        check_ok("sweet: switch", turi_eval(env, "#lang turmeric/sweet"));
        check_ok("sweet: first run", turi_eval(env, "defn main [] : int\n  1\n"));
        check_error("sweet: failing turn", turi_eval(env, "undefined-thing(1)"));
        check_ok("sweet: re-run after failure",
                 turi_eval(env, "defn main [] : int\n  2\n"));
        check_int("sweet: main() sees the re-run", 2, turi_eval(env, "main()"));
        turi_env_free(env);
    }

    recovered_matches_healthy();

    if (stdlib_blame) {
        fprintf(stderr, "FAIL: %d diagnostic(s) blamed an auto-loaded stdlib module\n",
                stdlib_blame);
        failures++;
    }
    if (failures) {
        fprintf(stderr, "\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall session failure-recovery checks passed\n");
    return 0;
}
