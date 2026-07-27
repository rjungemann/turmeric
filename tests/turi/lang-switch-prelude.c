/* web-repl-lang-switch-drops-stdlib: a reader-type change must not take the
 * preloaded stdlib with it.
 *
 * A long-lived interpreter env (the browser REPL, `tur repl`) has the stdlib
 * evaluated into it up front, so those defns live in env->src_acc alongside
 * whatever the user has typed.  A `#lang` switch resets src_acc -- prior input
 * cannot be re-parsed under an incompatible reader -- and that used to empty the
 * buffer outright, dropping `hamt-of` and the rest of the stdlib with it:
 *
 *     turi> #map{}
 *     <eval>:1:5: warning [TUR-W0040]: unknown name 'hamt-of' ...
 *     #<error: unknown function or operator 'hamt-of'>
 *
 * The fix pins the preload prefix (turi_env_pin_prelude) so a switch rewinds to
 * it instead of to zero.  The pinned region comes back marked already-run, so
 * the stdlib is visible again without re-executing its `(load ...)` forms.
 *
 * NOTE ON SHAPE -- this matters for the test to bite.  The switch below is its
 * own turi_eval with NO trailing body, which is how both REPLs drive it: the
 * native prompt handles a bare `#lang` line in repl.c and never forwards it, and
 * the browser's language selector calls turi_wasm_set_lang.  A switch whose eval
 * DOES carry a body ("#lang turmeric/sweet\n\n(+ 1 2)") happens to leave enough
 * behind that the next turn still resolves, so writing the test that way passes
 * with and without the fix and gates nothing.
 *
 * Built via the tur_lang_switch_prelude CMake target.  Runs with
 * ASAN_OPTIONS=detect_leaks=0 (the interpreter is process-lifetime).
 */
#include <stdio.h>
#include <stdlib.h>

#include "turi/eval.h"
#include "turi/interpreter_natives.h"
#include "turi/preload.h"

static int failures = 0;

static void check_int(const char *what, int64_t expected, TuriValue got) {
    if (got.tag == TURI_ERROR) {
        fprintf(stderr, "FAIL [%s]: error: %s\n", what, got.as_error ? got.as_error : "?");
        failures++;
        return;
    }
    if (got.tag != TURI_INT) {
        fprintf(stderr, "FAIL [%s]: expected int, got tag %d\n", what, got.tag);
        failures++;
        return;
    }
    if (got.as_int != expected) {
        fprintf(stderr, "FAIL [%s]: expected %lld, got %lld\n",
                what, (long long)expected, (long long)got.as_int);
        failures++;
        return;
    }
    printf("PASS [%s] => %lld\n", what, (long long)got.as_int);
}

/* The preload sequence every interpreter entry point runs, ending in the pin.
 * Mirrors repl_preload_stdlib_and_natives (src/turi/repl.c) and
 * wasm_preload_stdlib (src/web/wasm_glue.c). */
static void preload(TuriEnv *env) {
    turi_env_preload_macros(env, "stdlib");
    turi_env_preload_native_stubs(env);
    turi_env_preload_collections(env, "stdlib");
    turi_env_preload_typeclasses(env, "stdlib");
    turi_env_pin_prelude(env);
    turi_env_register_interpreter_natives(env);
}

int main(void) {
    TuriEnv *env = turi_env_new();
    preload(env);

    /* Baseline: the stdlib is reachable before any switch. */
    check_int("map-count before switch", 2,
              turi_eval(env, "(map-count #map{:a 1 :b 2})"));
    check_int("macro before switch", 5, turi_eval(env, "(when true 5)"));

    /* The switch, on its own turn.  See the shape note in the header. */
    turi_eval(env, "#lang turmeric/sweet");

    /* The names that used to go missing.  `when` is a macro from macros.tur and
     * `hamt-of` (which the #map literal lowers to) a defn from the collection
     * modules -- both arrive via the preload, so both are the same regression. */
    check_int("macro after switch", 5, turi_eval(env, "(when true 5)"));
    check_int("map-count after switch", 2,
              turi_eval(env, "(map-count #map{:a 1 :b 2})"));
    check_int("set literal after switch", 3,
              turi_eval(env, "(set-count #set{1 2 3})"));
    check_int("vec literal after switch", 3,
              turi_eval(env, "(vec-len [10 20 30])"));

    /* Switching back is symmetric: the pin is not consumed by its first use.
     * This is the case that fails if the prelude is REPLAYED rather than rewound
     * to -- the second replay of `(load "stdlib/...")` hits the elaborator's
     * loaded_modules dedup and re-registers nothing. */
    turi_eval(env, "#lang turmeric");
    check_int("macro after switching back", 5, turi_eval(env, "(when true 5)"));
    check_int("map-count after switching back", 1,
              turi_eval(env, "(map-count #map{:z 9})"));

    /* A third switch, to a reader reached for the first time. */
    turi_eval(env, "#lang turmeric/neoteric");
    check_int("map-count under third reader", 2,
              turi_eval(env, "(map-count #map{:p 1 :q 2})"));

    if (failures) {
        fprintf(stderr, "\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall lang-switch prelude checks passed\n");
    return 0;
}
