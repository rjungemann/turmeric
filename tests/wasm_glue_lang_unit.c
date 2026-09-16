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
 *   - set_lang takes a base dialect and nothing else: a trailing token is
 *     rejected outright, with no partial application;
 *   - get_lang reports the canonical slash-namespaced name, including for
 *     the legacy "sweet-exp" alias on input;
 *   - `#s"..."` dispatches with no directive at all and survives a base
 *     switch -- it was the `stringed` layer, toggled through this very entry
 *     point, before the layer axis was decommissioned;
 *   - the registry export walks the C tables: canonical base names + labels,
 *     no "layers" key at all, and never the legacy alias;
 *   - the registry offers EVERY base lang_base_at knows, by count and by
 *     spelling.  Asserting a couple of names is what let the picker drift a
 *     whole language behind the reader: `#lang saffron` worked when typed and
 *     could not be selected, because WASM_LANG_BASES[] was a hand-kept copy
 *     that nothing compared against the real set;
 *   - set_lang carries the LANGUAGE axis, not just the reader -- every
 *     Saffron base shares a Turmeric reader, so dropping the language half
 *     leaves a "switched" session still elaborating as Turmeric.
 */

#include <stdio.h>
#include <string.h>

#include "compiler/lang_dialects.h"
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

    /* Unknown tokens reject outright -- an unrecognised base, and any
     * trailing token at all now that `#lang` takes a base and nothing else. */
    CHECK(turi_wasm_set_lang("no-such-base") == 1,
          "unknown base is rejected");
    CHECK(turi_wasm_set_lang("turmeric no-such-token") == 1,
          "a trailing token is rejected, not silently ignored");
    CHECK(strcmp(turi_wasm_get_lang(), "turmeric/sweet") == 0,
          "a rejected set_lang leaves the environment untouched");

    /* `#s"..."` needs no directive and cannot be switched off.  It used to be
     * the `stringed` layer, toggled through this very entry point; the layer
     * axis is gone and the dispatch is installed for every base. */
    CHECK(turi_wasm_set_lang("turmeric") == 0,
          "set_lang accepts the bare default base");
    CHECK(eval_contains("#s\"on\"", "on") &&
          !eval_contains("#s\"on\"", "#<error"),
          "#s\"...\" dispatches with no layer token and no directive tail");
    CHECK(turi_wasm_set_lang("turmeric/sweet") == 0,
          "set_lang accepts another base");
    CHECK(!eval_contains("#s\"still-on\"", "#<error"),
          "#s\"...\" survives a base switch (it is not base-gated)");

    /* Registry export: built from the C tables, canonical spellings only. */
    const char *reg = turi_wasm_lang_registry();
    CHECK(reg != NULL, "lang registry export returns a string");
    if (reg) {
        CHECK(strstr(reg, "\"bases\":[") != NULL,
              "registry has a bases array");
        CHECK(strstr(reg, "\"layers\"") == NULL,
              "registry has no layers key -- the axis is gone, not emptied");
        CHECK(strstr(reg, "\"name\":\"turmeric/sweet\"") != NULL,
              "registry offers the canonical sweet spelling");
        CHECK(strstr(reg, "sweet-exp") == NULL,
              "registry never offers the legacy alias");
        CHECK(strstr(reg, "\"label\":\"S-expression\"") != NULL,
              "registry carries human-readable base labels");

        /* Completeness, not a spot check: every base the C side accepts has
         * to be offerable, or the picker silently hides a language. */
        size_t n_offered = 0;
        for (const char *q = reg; (q = strstr(q, "\"name\":\"")) != NULL; q++) {
            n_offered++;
        }
        CHECK(n_offered == lang_bases_count(),
              "registry offers every base lang_base_at knows (no drift)");

        for (size_t i = 0; i < lang_bases_count(); i++) {
            LangBaseDescriptor d;
            if (!lang_base_at(i, &d)) continue;
            char needle[96];
            snprintf(needle, sizeof needle, "\"name\":\"%s\"", d.base);
            CHECK(strstr(reg, needle) != NULL,
                  "registry offers this specific base spelling");
        }
        /* saffron GRADUATED at 0.46.0, so it is offered UNBADGED.  Both halves
         * are asserted: that the base is still there, and that no base carries
         * a non-null `experiment` -- a stray badge would tell every playground
         * visitor the language is still a prototype. */
        CHECK(strstr(reg, "\"name\":\"saffron\"") != NULL,
              "the Saffron bases are offered");
        CHECK(strstr(reg, "\"experiment\":\"saffron\"") == NULL,
              "no base is badged with the graduated saffron experiment");
        for (size_t i = 0; i < lang_bases_count(); i++) {
            LangBaseDescriptor d;
            if (!lang_base_at(i, &d)) continue;
            CHECK(d.experiment == NULL,
                  "no base is experiment-gated (all eight are stable)");
        }
    }

    /* The LANGUAGE axis survives a set_lang.  `saffron` reads with the
     * Turmeric reader, so a reader-only switch looks like success and changes
     * nothing -- catch it on semantics, not on the reported name alone. */
    CHECK(turi_wasm_set_lang("saffron") == 0, "set_lang accepts a Saffron base");
    CHECK(strcmp(turi_wasm_get_lang(), "saffron") == 0,
          "get_lang reports both axes, not just the reader");
    CHECK(eval_contains("(defn add [a b] (+ a b))", "add") &&
          eval_contains("(add 7.1 0.5)", "7.6"),
          "an unannotated Saffron parameter takes a float (Turmeric defaults to int)");
    CHECK(turi_wasm_set_lang("saffron/sweet") == 0,
          "set_lang accepts a slash-namespaced Saffron base");
    CHECK(strcmp(turi_wasm_get_lang(), "saffron/sweet") == 0,
          "get_lang reports the Saffron sweet base");

    turi_wasm_shutdown();

    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
