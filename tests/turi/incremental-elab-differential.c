/* TR2 (turi-incremental-elaboration-design): A/B differential harness.
 *
 * The incremental path must be RESULT-IDENTICAL to the default whole-blob
 * re-parse path. This harness runs the same scripted eval sequences through two
 * envs -- one default, one with turi_env_set_incremental_elab(true) -- and
 * compares every turn's result value (and error status) turn by turn. Any
 * divergence fails, which is the property that makes the gate safe to flip.
 *
 * It also asserts the incremental path is actually DOING something (the
 * accumulated-form vector grows and is kept in sync with prior_toplevel), so a
 * silently-always-falling-back implementation cannot pass vacuously.
 *
 * Built via the tur_incremental_elab_diff CMake target. Runs with
 * ASAN_OPTIONS=detect_leaks=0 (the interpreter is process-lifetime).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turi/eval.h"

static int failures = 0;

/* Render a TuriValue into a stable comparable string. */
static void render(TuriValue v, char *out, size_t cap) {
    switch (v.tag) {
    case TURI_ERROR: snprintf(out, cap, "ERROR");                                  break;
    case TURI_INT:   snprintf(out, cap, "INT:%lld",   (long long)v.as_int);        break;
    case TURI_BOOL:  snprintf(out, cap, "BOOL:%d",    v.as_int ? 1 : 0);           break;
    case TURI_FLOAT: snprintf(out, cap, "FLOAT:%.9g", v.as_float);                 break;
    case TURI_CSTR:  snprintf(out, cap, "CSTR:%s",    v.as_cstr ? v.as_cstr : ""); break;
    case TURI_NIL:   snprintf(out, cap, "NIL");                                    break;
    default:         snprintf(out, cap, "TAG:%d",     v.tag);                      break;
    }
}

/* The env configurations checked against the default path. "promotion" is what
 * `tur repl` itself now runs (TR2.4); "both" is the configuration the REPL gets
 * if the incremental gate is also flipped on. */
typedef struct { const char *label; bool incr; bool promo; } DiffConfig;
static const DiffConfig CONFIGS[] = {
    { "incremental", true,  false },
    { "promotion",   false, true  },
    { "both",        true,  true  },
};
enum { N_CONFIGS = (int)(sizeof(CONFIGS) / sizeof(CONFIGS[0])) };

/* Run one scripted session through the default path and every configuration
 * above, comparing each turn's result. */
static void diff_session(const char *name, const char *const *lines, int n) {
    for (int c = 0; c < N_CONFIGS; c++) {
        /* The incremental path is ON by default since 2026-07-25, so the
         * baseline must explicitly opt OUT -- otherwise this harness would be
         * comparing the incremental path against itself and prove nothing. */
        TuriEnv *base = turi_env_new();
        turi_env_set_incremental_elab(base, false);   /* whole-blob reference */
        TuriEnv *test = turi_env_new();
        turi_env_set_incremental_elab(test, CONFIGS[c].incr);
        if (CONFIGS[c].promo) turi_env_set_scratch_promotion(test, true);

        int diverged = 0;
        for (int i = 0; i < n; i++) {
            TuriValue a = turi_eval(base, lines[i]);
            TuriValue b = turi_eval(test, lines[i]);
            char sa[512], sb[512];
            render(a, sa, sizeof sa);
            render(b, sb, sizeof sb);
            if (strcmp(sa, sb) != 0) {
                fprintf(stderr, "FAIL [%s/%s] turn %d diverged\n  src: %s\n"
                                "  default: %s\n  %s: %s\n",
                        name, CONFIGS[c].label, i, lines[i], sa,
                        CONFIGS[c].label, sb);
                failures++;
                diverged = 1;
            }
        }

        /* An incremental env must have actually accumulated forms and stayed in
         * sync with the session's parsed-form count -- otherwise this session
         * was only ever taking the fallback and proves nothing. */
        if (CONFIGS[c].incr) {
            if (test->n_acc_forms == 0) {
                fprintf(stderr, "FAIL [%s/%s]: accumulated no forms\n",
                        name, CONFIGS[c].label);
                failures++;
            } else if (test->n_acc_forms != test->prior_toplevel) {
                fprintf(stderr, "FAIL [%s/%s]: acc_forms (%u) out of sync with "
                                "prior_toplevel (%u)\n", name, CONFIGS[c].label,
                        test->n_acc_forms, test->prior_toplevel);
                failures++;
            }
        }
        /* A promotion-enabled env must actually be rewinding, not silently
         * declining every cycle (which would make this config vacuous). */
        if (CONFIGS[c].promo && test->promo_rewinds == 0) {
            fprintf(stderr, "FAIL [%s/%s]: promotion never rewound "
                            "(attempts=%llu busy=%llu unreloc=%llu)\n",
                    name, CONFIGS[c].label,
                    (unsigned long long)test->promo_attempts,
                    (unsigned long long)test->promo_decline_busy,
                    (unsigned long long)test->promo_decline_unrelocatable);
            failures++;
        }

        if (!diverged)
            printf("PASS [%s/%s] (%d turns identical)\n",
                   name, CONFIGS[c].label, n);

        turi_env_free(base);
        turi_env_free(test);
    }
}

/* Cross-eval definition and use: closures, structs, ADTs, recursion. */
static const char *const S_DEFS[] = {
    "(+ 1 2)",
    "(defn square [x : int] : int (* x x))",
    "(square 7)",
    "(defn fib [n : int] : int (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))",
    "(fib 10)",
    "(defstruct Pt [x : int y : int])",
    "(def origin (make-struct Pt 3 4))",
    "(.x origin)",
    "(.y origin)",
    "(defdata Opt (None) (Some :int))",
    "(def s7 (Some 7))",
    "(let [a 1 b 2] (+ a b))",
    "(square (fib 7))",
};

/* Multi-line turns: exercises the incremental line-cursor bookkeeping. */
static const char *const S_MULTILINE[] = {
    "(defn add3 [a : int b : int c : int] : int\n  (+ a\n     (+ b c)))",
    "(add3 1 2 3)",
    "(defn twice [x : int] : int\n  (* 2 x))",
    "(twice\n  21)",
    "(add3 (twice 1)\n      (twice 2)\n      (twice 3))",
};

/* Reader macros defined in one turn, used in later ones (the Form* whose
 * template lives in an earlier eval's arena). */
static const char *const S_READER_MACROS[] = {
    "(reader-macros/define 'zero :none '0)",
    "#zero",
    "(+ #zero 41)",
    "(reader-macros/define 'answer :none '(+ 40 2))",
    "#answer",
    "(let [z #zero] (+ z #answer))",
};

/* Errors mid-session: a failed turn must not corrupt the accumulated state on
 * either path, and recovery must match. */
static const char *const S_ERRORS[] = {
    "(defn ok1 [x : int] : int (+ x 1))",
    "(ok1 1)",
    "(this-function-does-not-exist 1)",   /* error turn */
    "(ok1 2)",                            /* must still work after the error */
    "(defn ok2 [x : int] : int (* x 2))",
    "(ok2 (ok1 3))",
};

/* Redefinition across turns. `defn` redefinition is a KNOWN, intentional
 * divergence covered separately by test_known_divergence below, so it is not
 * part of this identical-results session. */
static const char *const S_REDEF[] = {
    "(def v 1)",
    "v",
    "(def v 2)",
    "v",
};

/* ---------------------------------------------------------------------------
 * KNOWN, INTENTIONAL DIVERGENCE: redefining a top-level `defn` across turns.
 *
 * Default (whole-program) path: every turn re-elaborates the accumulated
 * program with stdlib_prefix = (count of prior forms), which marks all
 * previously-accumulated forms as stdlib. Redefining your own function then
 * fails with the actively misleading "'f' is already defined by an auto-loaded
 * stdlib module".
 *
 * Incremental path: prior turns' definitions live in the session scope as
 * ordinary user bindings, so redefinition behaves the way a REPL should -- the
 * new definition wins.
 *
 * The incremental behavior is the desirable one; this test pins BOTH so the
 * difference is explicit and any further drift on either path is caught.
 * ------------------------------------------------------------------------- */
static void test_known_divergence(void) {
    TuriEnv *base = turi_env_new();
    turi_env_set_incremental_elab(base, false);   /* whole-blob reference */
    TuriEnv *incr = turi_env_new();
    turi_env_set_incremental_elab(incr, true);

    turi_eval(base, "(defn f [x : int] : int (+ x 1))");
    turi_eval(incr, "(defn f [x : int] : int (+ x 1))");

    TuriValue rb = turi_eval(base, "(defn f [x : int] : int (+ x 100))");
    TuriValue ri = turi_eval(incr, "(defn f [x : int] : int (+ x 100))");

    if (rb.tag != TURI_ERROR) {
        fprintf(stderr, "FAIL [known-divergence]: default path no longer rejects "
                        "defn redefinition (tag %d) -- update this test\n", rb.tag);
        failures++;
    }
    if (ri.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [known-divergence]: incremental path rejected defn "
                        "redefinition: %s\n", ri.as_error ? ri.as_error : "?");
        failures++;
    }
    TuriValue vb = turi_eval(base, "(f 10)");
    TuriValue vi = turi_eval(incr, "(f 10)");
    if (vb.tag != TURI_INT || vb.as_int != 11) {
        fprintf(stderr, "FAIL [known-divergence]: default kept old defn? got %lld\n",
                (long long)vb.as_int);
        failures++;
    }
    if (vi.tag != TURI_INT || vi.as_int != 110) {
        fprintf(stderr, "FAIL [known-divergence]: incremental redefinition did not "
                        "take effect; got %lld (want 110)\n", (long long)vi.as_int);
        failures++;
    }
    if (!failures)
        printf("PASS [known-divergence] defn redefinition: default=error, "
               "incremental=new definition wins\n");

    turi_env_free(base);
    turi_env_free(incr);
}

/* ---------------------------------------------------------------------------
 * REGRESSION: `has_defmodule` is per-FILE state and must not carry across evals.
 *
 * When the persistent elaboration session kept it, the SECOND defmodule-wrapped
 * file loaded in a later eval failed with "only one defmodule is allowed per
 * file" -- which broke `tur repl` startup (its stdlib preload loads several
 * defmodule-wrapped modules) the moment the incremental gate was flipped on.
 * The whole-program path avoided this by resetting at the file boundary.
 *
 * Two temp modules are written and `(load ...)`-ed from SEPARATE evals, which is
 * the shape that regressed; results are compared against the whole-blob path.
 * ------------------------------------------------------------------------- */
static void test_defmodule_across_evals(void) {
    static const char *P1 = "/tmp/tur_tr2_modA.tur";
    static const char *P2 = "/tmp/tur_tr2_modB.tur";
    FILE *f1 = fopen(P1, "w"), *f2 = fopen(P2, "w");
    if (!f1 || !f2) {
        fprintf(stderr, "FAIL [defmodule-across-evals]: cannot write temp modules\n");
        failures++;
        if (f1) fclose(f1);
        if (f2) fclose(f2);
        return;
    }
    fputs("(defmodule tur-tr2-mod-a)\n(defn tr2-a [x : int] : int (+ x 1))\n", f1);
    fputs("(defmodule tur-tr2-mod-b)\n(defn tr2-b [x : int] : int (+ x 2))\n", f2);
    fclose(f1);
    fclose(f2);

    char src1[256], src2[256];
    snprintf(src1, sizeof src1, "(load \"%s\")\n", P1);
    snprintf(src2, sizeof src2, "(load \"%s\")\n", P2);

    for (int incr = 0; incr <= 1; incr++) {
        TuriEnv *env = turi_env_new();
        turi_env_set_incremental_elab(env, incr != 0);
        TuriValue r1 = turi_eval(env, src1);
        TuriValue r2 = turi_eval(env, src2);   /* the one that used to fail */
        if (r1.tag == TURI_ERROR || r2.tag == TURI_ERROR) {
            fprintf(stderr, "FAIL [defmodule-across-evals] incremental=%d: "
                            "first=%s second=%s\n", incr,
                    r1.tag == TURI_ERROR ? (r1.as_error ? r1.as_error : "ERR") : "ok",
                    r2.tag == TURI_ERROR ? (r2.as_error ? r2.as_error : "ERR") : "ok");
            failures++;
        } else {
            printf("PASS [defmodule-across-evals] incremental=%d: two "
                   "defmodule files loaded from separate evals\n", incr);
        }
        turi_env_free(env);
    }
    remove(P1);
    remove(P2);
}

#define SESSION(name, arr) diff_session(name, arr, (int)(sizeof(arr)/sizeof((arr)[0])))

int main(void) {
    SESSION("defs-and-uses",  S_DEFS);
    SESSION("multiline",      S_MULTILINE);
    SESSION("reader-macros",  S_READER_MACROS);
    SESSION("errors",         S_ERRORS);
    SESSION("redefinition",   S_REDEF);
    test_known_divergence();
    test_defmodule_across_evals();

    /* Longer churn: many turns, to shake out offset/line drift that only shows
     * up once the accumulated prefix is large. */
    {
        TuriEnv *base = turi_env_new();
        turi_env_set_incremental_elab(base, false);   /* whole-blob reference */
        TuriEnv *incr = turi_env_new();
        turi_env_set_incremental_elab(incr, true);
        turi_eval(base, "(defn g [x : int] : int (+ x 1))");
        turi_eval(incr, "(defn g [x : int] : int (+ x 1))");
        int bad = 0;
        for (int i = 0; i < 300; i++) {
            char src[128];
            snprintf(src, sizeof src, "(g %d)", i);
            TuriValue a = turi_eval(base, src);
            TuriValue b = turi_eval(incr, src);
            if (a.tag != b.tag || a.as_int != b.as_int) {
                fprintf(stderr, "FAIL [churn] turn %d: default=%lld incremental=%lld\n",
                        i, (long long)a.as_int, (long long)b.as_int);
                failures++; bad = 1; break;
            }
            if (b.tag != TURI_INT || b.as_int != i + 1) {
                fprintf(stderr, "FAIL [churn] turn %d wrong value: %lld\n",
                        i, (long long)b.as_int);
                failures++; bad = 1; break;
            }
        }
        if (!bad) printf("PASS [churn] (300 turns identical)\n");
        turi_env_free(base);
        turi_env_free(incr);
    }

    if (failures == 0) {
        printf("\nAll incremental-elaboration differential tests passed.\n");
        return 0;
    }
    fprintf(stderr, "\n%d differential test(s) FAILED.\n", failures);
    return 1;
}
