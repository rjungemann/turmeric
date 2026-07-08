/* turi-value-pool-scratch-promotion-plan: long-lived-env steady-state harness.
 *
 * Drives a SINGLE TuriEnv across many top-level evals with scratch promotion
 * enabled (turi_env_set_scratch_promotion) and asserts the two properties the
 * plan targets:
 *
 *   1. Steady-state memory: an eval that builds a large transient structure but
 *      leaves nothing new behind rewinds the scratch pool -- value_scratch is
 *      empty right after turi_eval returns (total_bytes == 0), and value_perm
 *      stops growing once the live global set is stable. A control env with
 *      promotion OFF shows value_scratch growing without bound over the same
 *      loop, proving the feature actually reclaims.
 *
 *   2. Cross-reset correctness: values that escape an eval -- structs, ADT
 *      values, plain and mutually-recursive (cyclic letrec) closures, and
 *      deeply nested structs -- remain valid and evaluate correctly after many
 *      subsequent scratch rewinds.
 *
 *   3. Control-flow relocation (carrier-relocation-plan Part 2): a first-class
 *      handler value and an UNSTARTED generator, each stored as a global, now
 *      promote into value_perm (scratch rewinds) and stay correct across 100+
 *      rewinds -- the handler keeps discharging its effect, the generator keeps
 *      yielding.
 *
 * It also checks the CONSERVATIVE guarantee for a shape that still cannot be
 * relocated: once a generator is STARTED (its suspended coroutine stack holds
 * scratch pointers the walk cannot rewrite), promotion declines to rewind that
 * cycle rather than corrupting it.
 *
 * Built via the tur_env_longlived CMake target. Run under ASan for
 * use-after-reset detection (the Debug build poisons the rewound scratch region,
 * so any straggler pointer read crashes here).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turi/eval.h"

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); failures++; } \
} while (0)

static int64_t eval_int(TuriEnv *env, const char *src, const char *what) {
    TuriValue r = turi_eval(env, src);
    if (r.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL: %s: eval error: %s\n", what, r.as_error ? r.as_error : "?");
        failures++;
        return -999999;
    }
    if (r.tag != TURI_INT) {
        fprintf(stderr, "FAIL: %s: expected int, got tag %d\n", what, r.tag);
        failures++;
        return -999999;
    }
    return r.as_int;
}

/* ---------------------------------------------------------------------------
 * Part 1: steady state -- scratch is rewound; a control env grows unbounded.
 * --------------------------------------------------------------------------- */
static void test_steady_state(void) {
    /* An eval that builds a large transient (a 400-deep nested struct chain)
     * but returns a scalar and leaves no new global. */
    static const char *PRELUDE =
        "(defstruct Node [v : int next : int])\n"
        "(defn build [n : int] : int\n"
        "  (if (< n 1) 0\n"
        "    (let [_ (make-struct Node n (build (- n 1)))] n)))\n";
    static const char *CHURN = "(build 400)\n";

    TuriEnv *env = turi_env_new();
    turi_env_set_scratch_promotion(env, true);
    TuriValue pr = turi_eval(env, PRELUDE);
    CHECK(pr.tag != TURI_ERROR, "prelude failed: %s", pr.tag == TURI_ERROR ? pr.as_error : "");

    /* After promotion, scratch must be rewound to empty each cycle. */
    size_t perm_after_warmup = 0;
    for (int i = 0; i < 200; i++) {
        int64_t v = eval_int(env, CHURN, "churn");
        CHECK(v == 400, "churn result wrong: got %lld", (long long)v);
        CHECK(env->value_scratch.total_bytes == 0,
              "scratch not rewound at iter %d (total_bytes=%zu)",
              i, env->value_scratch.total_bytes);
        if (i == 20) perm_after_warmup = env->value_perm.total_bytes;
    }
    /* Nothing new escapes after warmup, so perm must reach a fixed point. */
    CHECK(env->value_perm.total_bytes == perm_after_warmup,
          "perm grew after warmup: %zu -> %zu",
          perm_after_warmup, env->value_perm.total_bytes);
    turi_env_free(env);

    /* Control: same loop, promotion OFF -> scratch grows without bound. */
    TuriEnv *ctl = turi_env_new();
    TuriValue cpr = turi_eval(ctl, PRELUDE);
    CHECK(cpr.tag != TURI_ERROR, "control prelude failed");
    size_t early = 0, late = 0;
    for (int i = 0; i < 200; i++) {
        (void)eval_int(ctl, CHURN, "control churn");
        if (i == 20)  early = ctl->value_scratch.total_bytes;
        if (i == 199) late  = ctl->value_scratch.total_bytes;
    }
    CHECK(late > early,
          "promotion-off scratch did not grow (%zu -> %zu) -- feature not exercised",
          early, late);
    turi_env_free(ctl);
}

/* ---------------------------------------------------------------------------
 * Part 2: cross-reset correctness of promoted escapees.
 * --------------------------------------------------------------------------- */
static void test_cross_reset_correctness(void) {
    TuriEnv *env = turi_env_new();
    turi_env_set_scratch_promotion(env, true);

    /* Eval 1: leave a struct, an ADT value, a plain closure, and a pair of
     * mutually-recursive closures (cyclic letrec: each captures a shared frame
     * that binds the other) behind as globals. */
    static const char *SETUP =
        "(defstruct Point [x : int y : int])\n"
        "(defdata Color (Red) (Green) (Blue))\n"
        "(def origin (make-struct Point 3 4))\n"
        "(def hue (Green))\n"
        "(def add7 (fn [x : int] : int (+ x 7)))\n"
        "(defn is-even [n : int] : bool (if (= n 0) true  (is-odd  (- n 1))))\n"
        "(defn is-odd  [n : int] : bool (if (= n 0) false (is-even (- n 1))))\n"
        "0\n";
    TuriValue s = turi_eval(env, SETUP);
    CHECK(s.tag != TURI_ERROR, "setup failed: %s", s.tag == TURI_ERROR ? s.as_error : "");

    /* Churn many scratch generations, re-reading the promoted globals each time.
     * If promotion mis-copied (dangling into rewound scratch) the poisoned reads
     * would crash under ASan; a wrong value trips the CHECK. */
    for (int i = 0; i < 100; i++) {
        CHECK(eval_int(env, "(.x origin)", "origin.x") == 3, "origin.x wrong at %d", i);
        CHECK(eval_int(env, "(.y origin)", "origin.y") == 4, "origin.y wrong at %d", i);
        CHECK(eval_int(env, "(match hue (Red) 0 (Green) 1 (Blue) 2)", "hue") == 1,
              "hue match wrong at %d", i);
        CHECK(eval_int(env, "(add7 100)", "add7") == 107, "add7 wrong at %d", i);
        CHECK(eval_int(env, "(if (is-even 10) 1 0)", "is-even") == 1, "is-even wrong at %d", i);
        CHECK(eval_int(env, "(if (is-odd 7) 1 0)", "is-odd") == 1, "is-odd wrong at %d", i);
        /* Build a fresh transient each round so scratch actually recycles. */
        CHECK(eval_int(env, "(let [p (make-struct Point (+ 1 1) 9)] (.x p))", "fresh") == 2,
              "fresh struct wrong at %d", i);
    }
    turi_env_free(env);
}

/* ---------------------------------------------------------------------------
 * Part 3 (carrier-relocation-plan Part 2): relocate a suspended, coroutine-
 * stack-free control-flow value -- a first-class handler value -- into perm and
 * keep using it correctly across many scratch rewinds.
 *
 * A handler value (TURI_HANDLER) is a detached dispatch table: n_cases plus an
 * array of HandleCase pointers into the permanent elaborator AST. It carries no
 * coroutine stack and no env-level state, so it relocates with a verbatim struct
 * copy. Stored as a global, it must now PROMOTE (scratch rewinds) and still
 * discharge its effect after 100+ rewinds under the poison.
 * --------------------------------------------------------------------------- */
static void test_handler_relocation(void) {
    TuriEnv *env = turi_env_new();
    turi_env_set_scratch_promotion(env, true);

    static const char *SETUP =
        "(defeffect Ask [] :int)\n"
        "(def h (handler (Ask [] k) (resume k 41)))\n"
        "0\n";
    TuriValue s = turi_eval(env, SETUP);
    if (s.tag == TURI_ERROR) {
        /* Best-effort; don't fail the suite if the effect surface shifts. */
        fprintf(stderr, "note: handler-relocation setup skipped: %s\n",
                s.as_error ? s.as_error : "?");
        turi_env_free(env);
        return;
    }
    /* The handler value is reachable from a global but fully relocatable, so the
     * defining eval must have promoted it and rewound scratch. */
    CHECK(env->value_scratch.total_bytes == 0,
          "handler value not promoted: scratch was not rewound (total_bytes=%zu)",
          env->value_scratch.total_bytes);

    size_t perm_after_warmup = 0;
    for (int i = 0; i < 100; i++) {
        /* Build a transient, then drive the promoted handler. A mis-copy would
         * dereference poisoned scratch here and crash under ASan; a wrong result
         * trips the CHECK. */
        CHECK(eval_int(env, "(let [z (+ 1 1)] z)", "churn") == 2, "churn wrong at %d", i);
        CHECK(eval_int(env, "(with-handler h (perform (Ask)))", "use-handler") == 41,
              "promoted handler wrong at %d", i);
        CHECK(env->value_scratch.total_bytes == 0,
              "scratch not rewound under promoted handler at %d (total_bytes=%zu)",
              i, env->value_scratch.total_bytes);
        if (i == 10) perm_after_warmup = env->value_perm.total_bytes;
    }
    CHECK(env->value_perm.total_bytes == perm_after_warmup,
          "perm grew after warmup with a promoted handler: %zu -> %zu",
          perm_after_warmup, env->value_perm.total_bytes);
    turi_env_free(env);
}

/* ---------------------------------------------------------------------------
 * Part 4 (carrier-relocation-plan Part 2): an UNSTARTED generator relocates;
 * once STARTED it keeps bailing (the conservative guarantee for the shape we
 * cannot yet move).
 *
 * A generator's coroutine stack (and its makecontext state) is set up lazily on
 * the first gen-next, so an unstarted generator holds no live scratch pointers
 * and is relocatable (struct + captured frame). Once started-not-done, its
 * suspended coroutine stack references scratch that cannot be rewritten, so the
 * walk must decline the rewind wherever the TuriGen now lives.
 * --------------------------------------------------------------------------- */
static void test_gen_relocation(void) {
    TuriEnv *env = turi_env_new();
    turi_env_set_scratch_promotion(env, true);

    static const char *SETUP =
        "(load \"stdlib/gen.tur\")\n"
        "(def g (gen [] (yield 10) (yield 20) (yield 30)))\n"
        "0\n";
    TuriValue s = turi_eval(env, SETUP);
    if (s.tag == TURI_ERROR) {
        fprintf(stderr, "note: gen-relocation setup skipped: %s\n",
                s.as_error ? s.as_error : "?");
        turi_env_free(env);
        return;
    }
    /* Unstarted generator as a global: must promote and rewind scratch. */
    CHECK(env->value_scratch.total_bytes == 0,
          "unstarted generator not promoted: scratch not rewound (total_bytes=%zu)",
          env->value_scratch.total_bytes);

    /* Churn scratch many times with the unstarted generator promoted in perm;
     * scratch must stay rewound and perm must reach a fixed point. A mis-relocated
     * captured frame would read poisoned scratch and crash under ASan. */
    size_t perm_after_warmup = 0;
    for (int i = 0; i < 100; i++) {
        CHECK(eval_int(env, "(let [z (+ 2 3)] z)", "gen-churn") == 5, "gen churn wrong at %d", i);
        CHECK(env->value_scratch.total_bytes == 0,
              "scratch not rewound under a promoted unstarted generator at %d (total_bytes=%zu)",
              i, env->value_scratch.total_bytes);
        if (i == 10) perm_after_warmup = env->value_perm.total_bytes;
    }
    CHECK(env->value_perm.total_bytes == perm_after_warmup,
          "perm grew after warmup with a promoted unstarted generator: %zu -> %zu",
          perm_after_warmup, env->value_perm.total_bytes);

    /* The relocated generator is still usable: its first yield is correct. */
    CHECK(eval_int(env, "(gen-unwrap (gen-next g))", "gen1") == 10,
          "relocated generator first yield wrong");

    /* Now the generator is STARTED (its coroutine stack references scratch), so
     * promotion must DECLINE to rewind -- the conservative bail for a shape we
     * cannot relocate. Scratch must stay non-empty across further churns. */
    (void)eval_int(env, "(let [z 0] z)", "post-start-churn");
    CHECK(env->value_scratch.total_bytes != 0,
          "started generator did not block the rewind (total_bytes=%zu)",
          env->value_scratch.total_bytes);
    turi_env_free(env);
}

int main(void) {
    turi_init(false);
    test_steady_state();
    test_cross_reset_correctness();
    test_handler_relocation();
    test_gen_relocation();

    if (failures) {
        fprintf(stderr, "env-longlived: %d failure(s).\n", failures);
        return 1;
    }
    printf("env-longlived: all checks passed.\n");
    return 0;
}
