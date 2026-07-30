/* map-show-keyword-key-raw-int (root cause B): the REPL's auto-show must show a
 * collection's ELEMENTS through their own Show instances, not through the
 * int-carrier representative.
 *
 * The display path hands turi_try_show_by_tag a head-only type tag ("Map"),
 * which is enough to find the `Show [Map]` instance but says nothing about K
 * and V.  That instance is generic -- `map-show-loop [^Show K ^Show V]` shows
 * each element via `(show (:: (hamt/iter-cur-key iter) K))` -- and the
 * ascription only re-resolves away from the baked int-carrier instance if `K`
 * is bound in the frame chain.  A call synthesised from C has no call site to
 * bind it from, so every element used to show as Show[int], printing the raw
 * carrier integer:
 *
 *     turi> #map{:a 1}          => #map{108920370806512 1}
 *     turi> #map{"s" 1}         => #map{32702428 1}
 *
 * Note the second line: the bug was NOT Sym-specific.  A cstr key -- which has
 * a distinct C carrier and cannot be confused with int by the compiled path's
 * carrier collapse (root cause A) -- misprinted just the same.  That is the
 * diagnostic separating the two root causes, so it is asserted here explicitly.
 * `#map{7 70}` was correct only by the coincidence of carrier == value.
 *
 * The fix retains the full elaborated type (env->last_result_type) beside the
 * head tag and seeds the instance's constraint tyvars from it.
 *
 * WHY A C TEST AND NOT A .tur FIXTURE: the defect is in the auto-show display
 * path, which only runs when a result is rendered at a prompt.  A fixture
 * program calling (show-line (:: m (Map Sym int))) writes the element types
 * itself and passes with and without the fix -- it gates nothing.  Reaching the
 * bug requires driving turi_eval_typed + turi_try_show_by_tag the way repl.c
 * and wasm_glue.c do, which is what this does.
 *
 * Built via the tur_show_collection_elems CMake target.  Runs with
 * ASAN_OPTIONS=detect_leaks=0 (the interpreter is process-lifetime).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turi/eval.h"
#include "turi/interpreter_natives.h"
#include "turi/preload.h"

static int failures = 0;

/* Drive the same two steps the REPL does: evaluate capturing the head tag, then
 * render through the by-tag Show route. */
static void check_show(const char *what, const char *src, const char *expected) {
    TuriEnv *env = turi_env_new();
    turi_env_preload_macros(env, "stdlib");
    turi_env_preload_native_stubs(env);
    turi_env_preload_collections(env, "stdlib");
    turi_env_preload_typeclasses(env, "stdlib");
    turi_env_pin_prelude(env);
    turi_env_register_interpreter_natives(env);

    char      type_tag[64] = {0};
    TuriValue result = turi_eval_typed(env, src, type_tag, sizeof(type_tag));

    if (result.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [%s]: eval error: %s\n",
                what, result.as_error ? result.as_error : "?");
        failures++;
        return;
    }

    const char *shown = turi_try_show_by_tag(env, result, type_tag);
    if (!shown) {
        fprintf(stderr, "FAIL [%s]: no Show route for tag '%s'\n", what, type_tag);
        failures++;
        return;
    }
    if (strcmp(shown, expected) != 0) {
        fprintf(stderr, "FAIL [%s]: expected '%s', got '%s'\n",
                what, expected, shown);
        failures++;
        free((char *)shown);
        return;
    }
    printf("PASS [%s] => %s\n", what, shown);
    free((char *)shown);
}

int main(void) {
    /* Sym keys: the reported symptom. */
    check_show("map with Sym key", "#map{:a 1}", "#map{:a 1}");

    /* cstr key: the diagnostic that proves this is type erasure in the display
     * path, not the compiled path's int64_t carrier collision.  cstr has its own
     * carrier and still misprinted before the fix. */
    check_show("map with cstr key", "#map{\"s\" 1}", "#map{s 1}");

    /* A non-int VALUE exercises the V constraint independently of K. */
    check_show("map with Sym key and cstr value", "#map{:a \"v\"}", "#map{:a v}");

    /* int keys were already right -- by coincidence, not by resolution.  They
     * must stay right: the seeding binds K=int here rather than skipping. */
    check_show("map with int key", "#map{7 70}", "#map{7 70}");

    /* Set and Vec share the same generic-element machinery via their own
     * one-constraint instances. */
    check_show("set of Sym", "#set{:x}", "#set{:x}");
    check_show("vec of Sym", "(vec-of :a :b)", "[:a :b]");
    check_show("vec of int", "(vec-of 1 2)", "[1 2]");

    if (failures) {
        fprintf(stderr, "\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall collection-element show checks passed\n");
    return 0;
}
