/* turi-env-owned-value-arena-pool-plan: env-teardown leak harness.
 *
 * Loops N times { turi_env_new; evaluate a script that builds closures,
 * structs, ADTs, cons lists, and produces error values; read a result;
 * turi_env_free; }.  Run under ASAN_OPTIONS=detect_leaks=1 it asserts that a
 * complete create/eval/free cycle leaves ZERO turi-attributable live
 * allocations -- the property Phase 1 of the value-arena-pool plan delivers
 * (the inverse of the interpreter's historical detect_leaks=0 default).
 *
 * Compile via the tur_env_teardown CMake target; the wrapper
 * tests/run-turi-env-teardown.sh runs it with leak detection forced on.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turi/eval.h"

static int failures = 0;

/* Source that exercises every payload kind routed through the value pool:
 *   - closures (defn + fn capture)
 *   - structs (make-struct) and ADTs (defdata constructors)
 *   - error values (unbound reference -> TURI_ERROR via the global pool)
 * The final top-level expression yields an int we read back out. */
static const char *SCRIPT =
    "(defstruct Point [x : int y : int])\n"
    "(defdata Color (Red) (Green) (Blue))\n"
    "(defn fib [n : int] : int\n"
    "  (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))\n"
    "(defn add1 [x : int] : int (+ x 1))\n"
    "(defn use-color [c : Color] : int\n"
    "  (match c (Red) 0 (Green) 1 (Blue) 2))\n"
    "(let [p (make-struct Point 3 4)\n"
    "      c (Green)\n"
    "      f (fn [x : int] : int (+ x (.x p)))]\n"
    "  (+ (fib 12)\n"
    "     (.y p)\n"
    "     (use-color c)\n"
    "     (f 10)\n"
    "     (add1 99)))\n";

/* turi-value-pool-residual-sites: drive a generator so its coroutine stack is
 * allocated (mmap/malloc, outside the value pool) and tracked on the env, then
 * reclaimed by turi_env_free.  Under ASan this catches a use-after-free of the
 * pool-allocated tracking node or a double-free in the teardown walk; the mmap
 * region itself is not LSan-tracked, so this is a soundness check, not a leak
 * assertion.  Sums 1+2+3 = 6. */
static const char *GEN_SCRIPT =
    "(load \"stdlib/gen.tur\")\n"
    "(let [g        (gen []\n"
    "                 (yield 1)\n"
    "                 (yield 2)\n"
    "                 (yield 3))\n"
    "      ^mut sum 0]\n"
    "  (while (not (gen-done? g))\n"
    "    (let [v (gen-next g)]\n"
    "      (if (gen-some? v)\n"
    "        (set! sum (+ sum (gen-unwrap v)))\n"
    "        (set! sum sum))))\n"
    "  sum)\n";

int main(void) {
    turi_init(false);

    const int ITERS = 50;
    for (int i = 0; i < ITERS; i++) {
        TuriEnv *env = turi_env_new();
        if (!env) {
            fprintf(stderr, "FATAL: turi_env_new failed (iter %d)\n", i);
            return 1;
        }

        TuriValue r = turi_eval(env, SCRIPT);
        if (r.tag == TURI_ERROR) {
            fprintf(stderr, "FAIL [iter %d]: script error: %s\n",
                    i, r.as_error ? r.as_error : "?");
            failures++;
        } else if (r.tag != TURI_INT) {
            fprintf(stderr, "FAIL [iter %d]: expected int result, got tag %d\n",
                    i, r.tag);
            failures++;
        }

        /* Read a closure back out of the env (exercises retained closures). */
        TuriValue fnv = turi_env_get(env, "fib");
        if (fnv.tag != TURI_CLOSURE) {
            fprintf(stderr, "FAIL [iter %d]: fib not a closure (tag %d)\n",
                    i, fnv.tag);
            failures++;
        } else {
            TuriValue arg = turi_int(10);
            TuriValue fr  = turi_call(env, fnv, &arg, 1);
            if (fr.tag != TURI_INT || fr.as_int != 55) {
                fprintf(stderr, "FAIL [iter %d]: (fib 10) => tag %d val %lld\n",
                        i, fr.tag, (long long)fr.as_int);
                failures++;
            }
        }

        /* Drive a generator: allocates + tracks + (on free) reclaims a
         * coroutine stack (turi-value-pool-residual-sites). */
        TuriValue gv = turi_eval(env, GEN_SCRIPT);
        if (gv.tag != TURI_INT || gv.as_int != 6) {
            fprintf(stderr, "FAIL [iter %d]: generator drive => tag %d val %lld "
                            "(expected int 6)\n",
                    i, gv.tag, (long long)gv.as_int);
            failures++;
        }

        /* Produce and observe an error value (global-pool string). */
        TuriValue ev = turi_eval(env, "(this-name-is-unbound-xyz 1 2)");
        if (ev.tag != TURI_ERROR) {
            fprintf(stderr, "FAIL [iter %d]: expected error for unbound ref, "
                            "got tag %d\n", i, ev.tag);
            failures++;
        }

        turi_env_free(env);
    }

    if (failures == 0) {
        printf("env-teardown: %d iterations, all clean.\n", ITERS);
        return 0;
    }
    fprintf(stderr, "\n%d check(s) FAILED.\n", failures);
    return 1;
}
