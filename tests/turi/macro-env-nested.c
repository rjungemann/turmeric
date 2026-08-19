/* macro-env-nested.c -- Stage 1 gate test for the macro-time interpreter env
 * (docs/upcoming/macro-system-direction-plan.md).
 *
 * Pins the reentrancy contract that Stage 2's procedural macros depend on:
 * a turi env created and evaluated MID-COMPILE (g_interpret_mode == false)
 * must never leave the process-global interpret-mode flag flipped --
 * turi_env_new sets it unconditionally (env.c,
 * libturi-embed-interpret-mode-flag), so elab_macro_env_get brackets the
 * constructor, and turi_eval_with_sink ("Gap 7") brackets each eval.
 * Also pins the macro env's posture: capabilities denied (an I/O builtin
 * errors instead of running), fuel bounds runaway macro-time recursion,
 * and elab_session_free reclaims the env (ASan would flag a leak of the
 * env's non-interpreter allocations or a double free).
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like every turi ctest: the
 * tree-walking interpreter's closures/natives are process-lifetime by
 * design (CMakeLists.txt, turi test block).
 */
#include <stdio.h>
#include <string.h>

#include "elab.h"
#include "globals.h"
#include "turi/eval.h"

static int failures = 0;

static void check(const char *what, int ok) {
    if (ok) {
        printf("PASS %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        failures++;
    }
}

int main(void) {
    /* Compile posture: the elaborator runs with interpret mode off. */
    g_interpret_mode = false;

    ElabSession *sess = elab_session_new();
    check("session created", sess != NULL);

    struct TuriEnv *me = elab_macro_env_get(sess);
    check("macro env created", me != NULL);
    check("macro env is cached", elab_macro_env_get(sess) == me);
    check("g_interpret_mode still false after env creation",
          g_interpret_mode == false);

    /* Nested eval inside the (simulated) compile: interpreter semantics
     * inside, compile mode restored outside. */
    TuriValue v = turi_eval((TuriEnv *)me, "(+ 1 2)");
    check("nested eval computes", v.tag == TURI_INT && v.as_int == 3);
    check("g_interpret_mode still false after nested eval",
          g_interpret_mode == false);

    /* The syntax natives are registered in the macro env like any other
     * interpreter env -- the Stage 2 macro-body value vocabulary. */
    TuriValue sx = turi_eval((TuriEnv *)me,
                             "(syntax->int (syntax-first (syntax-rest "
                             "(read-string \"(+ 41 1)\"))))");
    check("syntax natives usable in macro env",
          sx.tag == TURI_INT && sx.as_int == 41);

    /* The stdlib is preloaded (REPL sequence + the string files), so macro
     * bodies get real string building and the core macros. */
    TuriValue sc = turi_eval((TuriEnv *)me, "(str-concat \"ab\" \"cd\")");
    check("stdlib preloaded: str-concat works in macro env",
          sc.tag == TURI_CSTR && sc.as_cstr && strcmp(sc.as_cstr, "abcd") == 0);
    TuriValue cw = turi_eval((TuriEnv *)me, "(cond (= 1 2) 10 :else 20)");
    check("stdlib preloaded: cond macro works in macro env",
          cw.tag == TURI_INT && cw.as_int == 20);
    check("g_interpret_mode still false after preloaded evals",
          g_interpret_mode == false);

    /* Capability posture: TURI_CAP_NONE -- an I/O builtin is refused. */
    TuriValue io = turi_eval((TuriEnv *)me, "(println \"nope\")");
    check("I/O denied under TURI_CAP_NONE", turi_is_error(io));

    /* Fuel bounds runaway macro-time recursion: with a tiny budget an
     * infinite self-call errors instead of hanging the compile. */
    turi_env_set_fuel((TuriEnv *)me, 50);
    TuriValue fuel = turi_eval((TuriEnv *)me,
                               "(defn spin [n : int] : int (spin (+ n 1)))"
                               "(spin 0)");
    check("fuel exhausts runaway recursion",
          turi_is_error(fuel) &&
          turi_error_message(fuel) &&
          strstr(turi_error_message(fuel), "fuel") != NULL);
    check("g_interpret_mode still false after error paths",
          g_interpret_mode == false);

    /* Teardown: the session owns the env. */
    elab_session_free(sess);
    check("session freed with macro env", 1);

    if (failures) {
        printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
