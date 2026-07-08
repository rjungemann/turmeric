/* turi-interp-collections-libturi: C embedding parity test for the Vec / Set /
 * HAMT collection natives.
 *
 * These natives used to live only in the `tur` CLI (main.c), so a program
 * driven through the `turi_eval` C API -- which links libturi, not main.c --
 * failed at runtime with "inline-C not supported in interpreter mode" the
 * moment it touched vec-push!, set-add, or any HAMT op with a native override
 * but no interpreter-emulatable inline-C body.  After the relocation into
 * tur_core + auto-registration in turi_env_new, every libturi consumer resolves
 * the same overrides.  This harness exercises that path end-to-end; it fails to
 * even reach a value before the change.
 *
 * Compile with:
 *   cmake --build build --target tur_collections_embed
 *   ./build/tur_collections_embed
 *
 * Runs with ASAN_OPTIONS=detect_leaks=0: the tree-walking interpreter's
 * collection buffers are process-lifetime (see the plan's "Known limitation"),
 * matching the other turi harnesses' leak policy.
 */

#include <stdio.h>

#include "turi/eval.h"

static int failures = 0;

static void check_int(const char *expr, int64_t expected, TuriValue got) {
    if (got.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [%s]: error: %s\n", expr, got.as_error);
        failures++;
        return;
    }
    if (got.tag != TURI_INT) {
        fprintf(stderr, "FAIL [%s]: expected int, got tag %d\n", expr, got.tag);
        failures++;
        return;
    }
    if (got.as_int != expected) {
        fprintf(stderr, "FAIL [%s]: expected %lld, got %lld\n",
                expr, (long long)expected, (long long)got.as_int);
        failures++;
        return;
    }
    printf("PASS [%s] => %lld\n", expr, (long long)got.as_int);
}

static void check_float(const char *expr, double expected, TuriValue got) {
    if (got.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [%s]: error: %s\n", expr, got.as_error);
        failures++;
        return;
    }
    if (got.tag != TURI_FLOAT) {
        fprintf(stderr, "FAIL [%s]: expected float, got tag %d\n", expr, got.tag);
        failures++;
        return;
    }
    if (got.as_float != expected) {
        fprintf(stderr, "FAIL [%s]: expected %g, got %g\n",
                expr, expected, got.as_float);
        failures++;
        return;
    }
    printf("PASS [%s] => %g\n", expr, got.as_float);
}

static void check_bool(const char *expr, int expected, TuriValue got) {
    if (got.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [%s]: error: %s\n", expr, got.as_error);
        failures++;
        return;
    }
    if (got.tag != TURI_BOOL) {
        fprintf(stderr, "FAIL [%s]: expected bool, got tag %d\n", expr, got.tag);
        failures++;
        return;
    }
    if ((got.as_bool ? 1 : 0) != (expected ? 1 : 0)) {
        fprintf(stderr, "FAIL [%s]: expected %s, got %s\n",
                expr, expected ? "true" : "false",
                got.as_bool ? "true" : "false");
        failures++;
        return;
    }
    printf("PASS [%s] => %s\n", expr, got.as_bool ? "true" : "false");
}

int main(void) {
    turi_init(false);

    TuriEnv *env = turi_env_new();
    if (!env) {
        fprintf(stderr, "FATAL: turi_env_new failed\n");
        return 1;
    }

    /* --- Vec ---------------------------------------------------------- */
    /* Element type is pinned by the (:: ... (Vec int)) ascription (the no-
     * context fallback for a zero-arg vec-new). */
    check_int("vec push/get",
        9, turi_eval(env,
            "(let [v (:: (vec-new) (Vec int))]"
            "  (vec-push! v 7) (vec-push! v 9) (vec-get v 1))"));
    check_int("vec len",
        3, turi_eval(env,
            "(let [v (:: (vec-new) (Vec int))]"
            "  (vec-push! v 1) (vec-push! v 2) (vec-push! v 3) (vec-len v))"));
    check_int("vec set!/pop!",
        50, turi_eval(env,
            "(let [v (:: (vec-new) (Vec int))]"
            "  (vec-push! v 10) (vec-push! v 20) (vec-set! v 1 50) (vec-pop! v))"));
    /* Float element exercises the carrier-retag path (vec cells are raw int64;
     * the recorded element tag re-tags float on read-back). */
    check_float("vec float retag",
        3.25, turi_eval(env,
            "(let [v (:: (vec-new) (Vec float))]"
            "  (vec-push! v 3.25) (vec-get v 0))"));

    /* --- Set (int-keyed low-level natives: key == hash == element) ---- */
    check_int("set count",
        2, turi_eval(env,
            "(let [s (set-add (set-add (set-new) 10 10) 20 20)] (set-count s))"));
    check_bool("set member? hit",
        1, turi_eval(env,
            "(set-member? (set-add (set-new) 10 10) 10 10)"));
    check_bool("set member? miss",
        0, turi_eval(env,
            "(set-member? (set-add (set-new) 10 10) 99 99)"));
    check_int("set union count",
        3, turi_eval(env,
            "(let [a (set-add (set-add (set-new) 1 1) 2 2)"
            "      b (set-add (set-add (set-new) 2 2) 3 3)]"
            "  (set-count (set-union a b)))"));

    /* --- HAMT (the runtime backing Map[K V]; pointer/int carrier ABI) - */
    check_int("hamt set/get",
        42, turi_eval(env,
            "(tur_hamt_get (tur_hamt_set (tur_hamt_new) 5 5 42) 5 5)"));
    check_bool("hamt has",
        1, turi_eval(env,
            "(tur_hamt_has (tur_hamt_set (tur_hamt_new) 7 7 1) 7 7)"));
    check_int("hamt count",
        2, turi_eval(env,
            "(tur_hamt_count"
            "  (tur_hamt_set (tur_hamt_set (tur_hamt_new) 1 1 100) 2 2 200))"));

    turi_env_free(env);

    /* --- High-level Set/Map stdlib API via `load` -------------------- */
    /* interp-load-set-map-elaboration-gap: an isolated `(load "stdlib/set.tur")`
     * / `(load "stdlib/map.tur")` used to fail elaboration -- the modules refer
     * to hamt-module names and Eq/Hash that only exist once their dependencies
     * are loaded, and the CLI-only auto-load prelude was the sole thing that
     * loaded them in order.  Both files now self-declare those deps with
     * `(load ...)` (deduped under the CLI prelude), so a bare libturi embedder
     * can load them and use the public Set/Map surface, not just the relocated
     * natives.  A fresh env models the real embedder flow (load the stdlib, then
     * use it).  Requires cwd == repo root so the "stdlib/..." paths resolve. */
    TuriEnv *senv = turi_env_new();
    if (!senv) {
        fprintf(stderr, "FATAL: second turi_env_new failed\n");
        return 1;
    }
    TuriValue ls = turi_eval(senv, "(load \"stdlib/set.tur\")");
    if (ls.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [load stdlib/set.tur]: %s\n", ls.as_error);
        failures++;
    } else {
        printf("PASS [load stdlib/set.tur]\n");
    }
    TuriValue lm = turi_eval(senv, "(load \"stdlib/map.tur\")");
    if (lm.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [load stdlib/map.tur]: %s\n", lm.as_error);
        failures++;
    } else {
        printf("PASS [load stdlib/map.tur]\n");
    }
    /* set-add takes a precomputed element hash; the element doubles as its own
     * hash for these int keys. */
    check_int("Set count (loaded)",
        2, turi_eval(senv,
            "(let [s (set-add (set-add (set-new) 3 3) 5 5)] (set-count s))"));
    check_bool("Set member? (loaded)",
        1, turi_eval(senv, "(set-member? (set-add (set-new) 5 5) 5 5)"));
    /* Public map-assoc / map-get / map-count macros over an ascribed map-new. */
    check_int("Map count (loaded)",
        2, turi_eval(senv,
            "(let [m (:: (map-new) (Map int int))]"
            "  (map-count (map-assoc (map-assoc m 1 10) 2 20)))"));
    check_int("Map get (loaded)",
        42, turi_eval(senv,
            "(let [m (:: (map-new) (Map int int))]"
            "  (map-get (map-assoc m 7 42) 7))"));

    turi_env_free(senv);

    if (failures == 0) {
        printf("\nAll collection embedding tests passed.\n");
        return 0;
    }
    fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
    return 1;
}
