/* PS4 (playground-session-hygiene-plan): every def* form can be redefined by a
 * later turn of a long-lived session, as `defn` always could.
 *
 * Try Turmeric's Run button and `tur repl` both feed turns into one session,
 * so re-running a program re-runs its definitions.  Half the def* forms used
 * to refuse that -- `defeffect: 'Ask' is already defined` on the second Run of
 * the shipped effects example -- while defn, defclass, defopaque and others
 * accepted it; the split followed where incremental elaboration happened to
 * land, not any principle.  A definition from an EARLIER turn is now replaced.
 *
 * What stays refused, and is checked here so the rule is not simply deleted:
 *   - a duplicate within ONE turn (one turn is one program);
 *   - a type or instance the stdlib defines: rewriting it in place changes it
 *     for the stdlib code built on it (redefine Pair and `(.fst (pair 1 2))`
 *     stops resolving), and the stdlib-instance refusal is a standing
 *     decision (duplicate-instance-silently-drops-a-user-definstance);
 *   - the builtin Unsafe effect.
 *
 * Built via the tur_session_redefinition CMake target.  Runs with
 * ASAN_OPTIONS=detect_leaks=0 (the interpreter is process-lifetime).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turi/eval.h"
#include "turi/interpreter_natives.h"
#include "turi/preload.h"

static int failures = 0;

static void quiet(TuriEnv *env, int level, const char *code, const char *file,
                  uint32_t line, uint32_t col_start, uint32_t col_end,
                  const char *message, void *ud) {
    (void)env; (void)level; (void)code; (void)file; (void)line;
    (void)col_start; (void)col_end; (void)message; (void)ud;
}

/* Mirrors wasm_preload_stdlib (src/web/wasm_glue.c). */
static TuriEnv *new_session(void) {
    TuriEnv *env = turi_env_new();
    turi_env_set_diag_sink(env, quiet, NULL);   /* the refusals are expected */
    turi_env_preload_macros(env, "stdlib");
    turi_env_preload_native_stubs(env);
    turi_env_preload_collections(env, "stdlib");
    turi_env_preload_typeclasses(env, "stdlib");
    turi_env_pin_prelude(env);
    turi_env_register_interpreter_natives(env);
    return env;
}

static void check_ok(const char *what, TuriValue got) {
    if (got.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [%s]: error: %s\n", what, got.as_error ? got.as_error : "?");
        failures++;
    } else {
        printf("PASS [%s]\n", what);
    }
}

static void check_int(const char *what, int64_t expected, TuriValue got) {
    if (got.tag != TURI_INT || got.as_int != expected) {
        if (got.tag == TURI_ERROR)
            fprintf(stderr, "FAIL [%s]: error: %s\n", what, got.as_error ? got.as_error : "?");
        else
            fprintf(stderr, "FAIL [%s]: expected %lld, got tag %d value %lld\n", what,
                    (long long)expected, got.tag, (long long)got.as_int);
        failures++;
    } else {
        printf("PASS [%s] => %lld\n", what, (long long)got.as_int);
    }
}

static void check_refused(const char *what, TuriValue got) {
    if (got.tag != TURI_ERROR) {
        fprintf(stderr, "FAIL [%s]: expected a refusal\n", what);
        failures++;
    } else {
        printf("PASS [%s] refused\n", what);
    }
}

/* Define, use, redefine differently, use again: each turn its own eval. */
typedef struct {
    const char *form;
    const char *def1, *use1; int64_t want1;
    const char *def2, *use2; int64_t want2;
} Redef;

static const Redef REDEFS[] = {
    { "defn",
      "(defn f1 [] : int 1)", "(f1)", 1,
      "(defn f1 [] : int 2)", "(f1)", 2 },
    { "def",
      "(def v1 : int 1)", "v1", 1,
      "(def v1 : int 2)", "v1", 2 },
    { "define",
      "(define v2 : int 1)", "v2", 1,
      "(define v2 : int 2)", "v2", 2 },
    { "defstruct",
      "(defstruct P1 [x : int])", "(.x (P1 1))", 1,
      "(defstruct P1 [x : int y : int])", "(.y (P1 1 2))", 2 },
    { "defdata",
      "(defdata D1 (A1) (B1 :int))", "(match (B1 5) (A1) 0 (B1 n) n)", 5,
      "(defdata D1 (A1) (B1 :int) (C1))", "(match (C1) (A1) 0 (B1 n) n (C1) 9)", 9 },
    { "defeffect (new signature)",
      "(defeffect E1 [] :int)",
      "(handle (+ 1 (perform (E1))) (E1 [] k) (resume k 41))", 42,
      "(defeffect E1 [x : int] :int)",
      "(handle (+ 2 (perform (E1 5))) (E1 [x] k) (resume k (* x 10)))", 52 },
    { "definstance",
      "(do (defclass C1 [a] (c1m [x : a] : int)) (definstance C1 [int] (c1m [x] (+ x 1))))",
      "(c1m 1)", 2,
      "(definstance C1 [int] (c1m [x] (+ x 2)))", "(c1m 1)", 3 },
    { "defmacro",
      "(defmacro m1 [x] x)", "(m1 1)", 1,
      "(defmacro m1 [x] (list + x 1))", "(m1 1)", 2 },
    { "defmacro*",
      "(defmacro* ms1 [x] x)", "(ms1 1)", 1,
      "(defmacro* ms1 [x] `(+ ~x 10))", "(ms1 1)", 11 },
};

int main(void) {
    TuriEnv *env = new_session();

    for (size_t i = 0; i < sizeof REDEFS / sizeof REDEFS[0]; i++) {
        const Redef *r = &REDEFS[i];
        char what[128];
        snprintf(what, sizeof what, "%s: define", r->form);
        check_ok(what, turi_eval(env, r->def1));
        snprintf(what, sizeof what, "%s: use", r->form);
        check_int(what, r->want1, turi_eval(env, r->use1));
        snprintf(what, sizeof what, "%s: redefine on a later turn", r->form);
        check_ok(what, turi_eval(env, r->def2));
        snprintf(what, sizeof what, "%s: the redefinition wins", r->form);
        check_int(what, r->want2, turi_eval(env, r->use2));
        /* And a third time, over a definition that was itself a redefinition. */
        snprintf(what, sizeof what, "%s: redefine again", r->form);
        check_ok(what, turi_eval(env, r->def2));
        snprintf(what, sizeof what, "%s: still resolves", r->form);
        check_int(what, r->want2, turi_eval(env, r->use2));
    }

    /* The shipped effects example, run the way the Run button runs it. */
    {
        const char *effects =
            "(defeffect Ask [] :int)\n"
            "(defn use-ask [] :int\n"
            "  (+ 1 (perform (Ask))))\n"
            "(handle (use-ask)\n"
            "  (Ask [] k) (resume k 41))";
        check_int("effects example: first Run", 42, turi_eval(env, effects));
        check_int("effects example: second Run", 42, turi_eval(env, effects));
    }

    /* Within one turn a duplicate is still the error it always was. */
    check_refused("same turn: def",
                  turi_eval(env, "(do (def dup1 1) (def dup1 2))"));
    check_refused("same turn: defstruct",
                  turi_eval(env, "(do (defstruct Q1 [a : int]) (defstruct Q1 [a : int]))"));
    check_refused("same turn: defmacro",
                  turi_eval(env, "(do (defmacro dm1 [x] x) (defmacro dm1 [x] x))"));
    check_refused("same turn: definstance",
                  turi_eval(env, "(do (definstance C1 [bool] (c1m [x] 7)) "
                                 "(definstance C1 [bool] (c1m [x] 8)))"));

    /* What the stdlib owns stays the stdlib's. */
    check_refused("stdlib type: defstruct Pair",
                  turi_eval(env, "(defstruct Pair [a : int b : int c : int])"));
    check_refused("stdlib type: defdata Option",
                  turi_eval(env, "(defdata Option (Nada) (Algo :int))"));
    check_int("stdlib Pair still works", 1, turi_eval(env, "(.fst (pair 1 2))"));
    check_int("stdlib Option still works", 3,
              turi_eval(env, "(match (some 3) (None) 0 (Some n) n)"));
    check_refused("stdlib instance: Eq [int]",
                  turi_eval(env, "(definstance Eq [int] (eq? [a b] false))"));
    check_refused("builtin effect: Unsafe",
                  turi_eval(env, "(defeffect Unsafe [] :int)"));

    turi_env_free(env);

    if (failures) {
        fprintf(stderr, "\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall session redefinition checks passed\n");
    return 0;
}
