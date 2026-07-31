/* I4 (docs/upcoming/mir-interp-tier-plan.md section 2.4): the JIT engine is
 * reachable from a plain libturi embedder.
 *
 * Before I4 the engine was compiled onto the `tur` executable only, so a C
 * host that linked libturi could evaluate Turmeric with the tree-walking
 * interpreter but had no way to reach the in-process JIT short of shelling
 * out to `tur jit`.  This test is the proof that it can now, and that a host
 * built WITHOUT a JIT still works by falling back to the interpreter.
 *
 * The two halves are deliberately compared against each other:
 *
 *   - turi_eval  -- evaluates Turmeric source with the tree-walker.  Always
 *                   available; this is the fallback an embedded host takes
 *                   when TUR_HAVE_JIT is not defined (an embedder has no cc
 *                   at runtime, so `tur jit`'s step-6 cc fallback is not
 *                   available to it -- the interpreter is).
 *   - tur_jit_*  -- compiles and runs a C translation unit in process and
 *                   calls a function out of the resulting image.
 *
 * Both compute the same value, so a mismatch is a real signal rather than
 * two independent assertions that happen to sit in one file.
 *
 * Compile with:
 *   cmake --build build --target tur_jit_embed
 *   ./build/tur_jit_embed
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turi/eval.h"

#ifdef TUR_HAVE_JIT
#include "jit_engine.h"
#endif

static int failures = 0;

/* The same computation, expressed once in Turmeric and once in C. */
#define EXPECTED 4950
static const char TURMERIC_SRC[] =
    "(defn sum-to [n : int acc : int] : int"
    "  (if (= n 0) acc (sum-to (- n 1) (+ acc n))))"
    "(sum-to 99 0)";

#ifdef TUR_HAVE_JIT
static const char C_SRC[] =
    "long long embed_sum_to(long long n) {\n"
    "    long long acc = 0;\n"
    "    while (n != 0) { acc += n; n -= 1; }\n"
    "    return acc;\n"
    "}\n";
#endif

static long long eval_via_interpreter (void) {
    TuriEnv *env = turi_env_new ();
    if (env == NULL) {
        fprintf (stderr, "FAIL [interp]: turi_env_new returned NULL\n");
        failures++;
        return -1;
    }
    TuriValue v = turi_eval (env, TURMERIC_SRC);
    long long got = -1;
    if (v.tag == TURI_ERROR) {
        fprintf (stderr, "FAIL [interp]: %s\n", v.as_error);
        failures++;
    } else if (v.tag != TURI_INT) {
        fprintf (stderr, "FAIL [interp]: expected int, got tag %d\n", v.tag);
        failures++;
    } else {
        got = (long long) v.as_int;
        printf ("PASS [interp] sum-to 99 => %lld\n", got);
    }
    turi_env_free (env);
    return got;
}

#ifdef TUR_HAVE_JIT
static long long eval_via_jit (void) {
    TurJitImage *img = NULL;
    int rc = tur_jit_compile_image (C_SRC, sizeof C_SRC - 1,
                                    NULL,      /* no autolink flags */
                                    NULL, 0,   /* no include dirs needed */
                                    &img);
    if (rc != TUR_JIT_OK || img == NULL) {
        fprintf (stderr, "FAIL [jit]: compile_image rc=%d\n", rc);
        failures++;
        return -1;
    }
    /* MIR item lookup by name -- unlike dlsym it also sees static functions,
     * which is what lets the single-TU spice emission keep static linkage. */
    long long (*fn) (long long) =
        (long long (*) (long long)) tur_jit_image_sym (img, "embed_sum_to");
    if (fn == NULL) {
        fprintf (stderr, "FAIL [jit]: image has no embed_sum_to\n");
        failures++;
        tur_jit_image_free (img);
        return -1;
    }
    long long got = fn (99);
    printf ("PASS [jit] embed_sum_to(99) => %lld\n", got);
    tur_jit_image_free (img);
    return got;
}
#endif

int main (void) {
    printf ("libturi JIT embedding test\n");
#ifdef TUR_HAVE_JIT
    printf ("  built WITH a JIT (TUR_HAVE_JIT)\n");
#else
    printf ("  built WITHOUT a JIT -- interpreter fallback only\n");
#endif

    long long interp = eval_via_interpreter ();
    if (interp != EXPECTED) {
        fprintf (stderr, "FAIL [interp]: expected %d, got %lld\n",
                 EXPECTED, interp);
        failures++;
    }

#ifdef TUR_HAVE_JIT
    long long jit = eval_via_jit ();
    if (jit != EXPECTED) {
        fprintf (stderr, "FAIL [jit]: expected %d, got %lld\n",
                 EXPECTED, jit);
        failures++;
    }
    /* The point of the test: an embedder reaching the engine gets the same
     * answer the interpreter does. */
    if (interp != jit) {
        fprintf (stderr, "FAIL: engine disagreement -- interp %lld, jit %lld\n",
                 interp, jit);
        failures++;
    } else {
        printf ("PASS [parity] interpreter and JIT agree (%lld)\n", interp);
    }
#else
    /* No JIT in this build: the interpreter result IS the answer, and the
     * host needed no cc subprocess to get it. */
    printf ("SKIP [jit] no engine in this build; fallback path exercised\n");
#endif

    if (failures != 0) {
        fprintf (stderr, "\n%d failure(s)\n", failures);
        return 1;
    }
    printf ("\nall checks passed\n");
    return 0;
}
