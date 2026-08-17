/* wasm_glue_lang_unit.c -- exercise the browser's language-mode entry points
 * natively (try-turmeric-lang-toggle-plan T0/T1).
 *
 * turi_wasm_set_lang / turi_wasm_get_lang / turi_wasm_lang_registry normally
 * only run inside a wasm module, where nothing in CI can look at them -- a
 * mistake would surface as "the playground's dialect picker does nothing",
 * found by a person, later.  Nothing in wasm_glue.c is actually
 * wasm-specific, so linking libturi_wasm runs the real thing here.
 *
 * Covered:
 *   - set_lang accepts a full directive tail ("turmeric/sweet stringed") and
 *     rejects an unknown layer token outright (no partial application);
 *   - get_lang reports the canonical slash-namespaced name, including for
 *     the legacy "sweet-exp" alias on input;
 *   - the layer set is ASSIGNED, so a layer omitted from a later set_lang
 *     genuinely turns off (the #s"..." dispatch stops resolving -- the
 *     session reader-macro registry is wiped, not just the layer bits);
 *   - the registry export walks the C tables: canonical base names + labels,
 *     every LANG_LAYERS[] row, and never the legacy alias.
 */

#include <stdio.h>
#include <string.h>

#include "web/wasm_glue.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { passed++; } \
        else { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); failed++; } \
    } while (0)

/* Evaluate and return 1 iff the result string contains `needle`.  The result
 * of turi_wasm_eval is malloc'd; freed here. */
static int eval_contains(const char *src, const char *needle) {
    char *r = turi_wasm_eval(src);
    int hit = r && strstr(r, needle) != NULL;
    turi_wasm_free_string(r);
    return hit;
}

int main(void) {
    CHECK(turi_wasm_init() == 0, "turi_wasm_init succeeds");

    /* Canonical naming, incl. the legacy alias on input. */
    CHECK(strcmp(turi_wasm_get_lang(), "turmeric") == 0,
          "fresh env starts in the default reader");
    CHECK(turi_wasm_set_lang("sweet-exp") == 0,
          "legacy 'sweet-exp' spelling is accepted on input");
    CHECK(strcmp(turi_wasm_get_lang(), "turmeric/sweet") == 0,
          "get_lang reports the canonical slash-namespaced name");

    /* Unknown tokens reject outright -- base and layer alike. */
    CHECK(turi_wasm_set_lang("no-such-base") == 1,
          "unknown base is rejected");
    CHECK(turi_wasm_set_lang("turmeric no-such-layer") == 1,
          "unknown layer token is rejected, not silently ignored");
    CHECK(strcmp(turi_wasm_get_lang(), "turmeric/sweet") == 0,
          "a rejected set_lang leaves the environment untouched");

    /* Layer toggle round-trip in one session: on, working; off, gone. */
    CHECK(turi_wasm_set_lang("turmeric stringed") == 0,
          "set_lang accepts a base plus layer tail");
    CHECK(eval_contains("#s\"on\"", "on") &&
          !eval_contains("#s\"on\"", "#<error"),
          "stringed layer activates the #s\"...\" dispatch");
    CHECK(turi_wasm_set_lang("turmeric") == 0,
          "set_lang accepts the bare default base");
    /* The reader rejects the now-unknown dispatch; the result carries the
     * error marker (the "unknown reader string macro '#s'" detail goes to
     * the diag sink, i.e. the browser console). */
    CHECK(eval_contains("#s\"off\"", "#<error"),
          "dropping the layer deactivates the dispatch (assign, not OR)");

    /* Registry export: built from the C tables, canonical spellings only. */
    const char *reg = turi_wasm_lang_registry();
    CHECK(reg != NULL, "lang registry export returns a string");
    if (reg) {
        CHECK(strstr(reg, "\"bases\":[") && strstr(reg, "\"layers\":[") != NULL,
              "registry has bases and layers arrays");
        CHECK(strstr(reg, "\"name\":\"turmeric/sweet\"") != NULL,
              "registry offers the canonical sweet spelling");
        CHECK(strstr(reg, "sweet-exp") == NULL,
              "registry never offers the legacy alias");
        CHECK(strstr(reg, "\"label\":\"S-expression\"") != NULL,
              "registry carries human-readable base labels");
        CHECK(strstr(reg, "\"name\":\"stringed\"") != NULL &&
              strstr(reg, "\"available\":true") != NULL,
              "registry lists the stringed layer as available");
    }

    turi_wasm_shutdown();

    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
