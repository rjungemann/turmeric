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

/* Run one scripted session through both paths, comparing each turn. */
static void diff_session(const char *name, const char *const *lines, int n) {
    TuriEnv *base = turi_env_new();                 /* default: whole-blob path */
    TuriEnv *incr = turi_env_new();
    turi_env_set_incremental_elab(incr, true);

    int diverged = 0;
    for (int i = 0; i < n; i++) {
        TuriValue a = turi_eval(base, lines[i]);
        TuriValue b = turi_eval(incr, lines[i]);
        char sa[512], sb[512];
        render(a, sa, sizeof sa);
        render(b, sb, sizeof sb);
        if (strcmp(sa, sb) != 0) {
            fprintf(stderr, "FAIL [%s] turn %d diverged\n  src: %s\n"
                            "  default:     %s\n  incremental: %s\n",
                    name, i, lines[i], sa, sb);
            failures++;
            diverged = 1;
        }
    }

    /* The incremental env must have actually accumulated forms and stayed in
     * sync with the session's parsed-form count -- otherwise this session was
     * only ever taking the fallback and proves nothing. */
    if (incr->n_acc_forms == 0) {
        fprintf(stderr, "FAIL [%s]: incremental env accumulated no forms\n", name);
        failures++;
    } else if (incr->n_acc_forms != incr->prior_toplevel) {
        fprintf(stderr, "FAIL [%s]: acc_forms (%u) out of sync with prior_toplevel (%u)\n",
                name, incr->n_acc_forms, incr->prior_toplevel);
        failures++;
    }

    if (!diverged) printf("PASS [%s] (%d turns identical)\n", name, n);

    turi_env_free(base);
    turi_env_free(incr);
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

/* Redefinition across turns. */
static const char *const S_REDEF[] = {
    "(defn f [x : int] : int (+ x 1))",
    "(f 10)",
    "(defn f [x : int] : int (+ x 100))",
    "(f 10)",
    "(def v 1)",
    "v",
    "(def v 2)",
    "v",
};

#define SESSION(name, arr) diff_session(name, arr, (int)(sizeof(arr)/sizeof((arr)[0])))

int main(void) {
    SESSION("defs-and-uses",  S_DEFS);
    SESSION("multiline",      S_MULTILINE);
    SESSION("reader-macros",  S_READER_MACROS);
    SESSION("errors",         S_ERRORS);
    SESSION("redefinition",   S_REDEF);

    /* Longer churn: many turns, to shake out offset/line drift that only shows
     * up once the accumulated prefix is large. */
    {
        TuriEnv *base = turi_env_new();
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
