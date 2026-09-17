/* wasm_glue_session_unit.c -- the playground's eval session, driven natively
 * the way Try Turmeric drives it (playground-session-hygiene-plan).
 *
 * Each case is a sequence a user reaches from the page:
 *
 *   - PS2/PS3: hovering a name in the doc panel calls turi_doc_lookup.  It
 *     used to evaluate `(doc-lookup "name")` INTO the session; doc-lookup was
 *     never defined there, so the lookup failed, returned NULL for every stdlib
 *     name, and left the session unable to re-run the program in the editor
 *     ("'main' is already defined by an auto-loaded stdlib module").  A lookup
 *     now reads the generated table from C and must find stdlib docs, find
 *     builtin docs, miss cleanly, and leave Run working however many times it
 *     is called.
 *   - PS3: `(doc ...)` at the prompt, in each spelling, evaluates cleanly.
 *
 * Nothing in wasm_glue.c is wasm-specific, so linking libturi_wasm runs the
 * real entry points.  Runs from the source root, where stdlib/ lives (the
 * browser build embeds the same tree at /stdlib).
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

/* Evaluate `src`; return 1 iff the result string contains `needle`. */
static int eval_contains(const char *src, const char *needle) {
    char *r = turi_wasm_eval(src);
    int hit = r && strstr(r, needle) != NULL;
    if (!hit) fprintf(stderr, "  eval %s\n    => %s\n", src, r ? r : "(null)");
    turi_wasm_free_string(r);
    return hit;
}

/* A Run of a program with a `main`, as runCode does it: the buffer, then
 * `(main)` as its own turn. */
static int run_main(const char *program, const char *expect) {
    return eval_contains(program, "#<fn main>") && eval_contains("(main)", expect);
}

int main(void) {
    CHECK(turi_wasm_init() == 0, "turi_wasm_init succeeds");

    /* ---- PS2/PS3: the doc panel ------------------------------------------ */
    CHECK(run_main("(defn main [] : int 1)", "1"), "first Run");

    const char *d = turi_doc_lookup("vec-map");
    CHECK(d && strncmp(d, "vec-map -- ", 11) == 0,
          "a stdlib name resolves to its generated docstring");
    CHECK(d && strstr(d, "Parameters:") != NULL,
          "the full docstring, not the one-line summary doc-names.json carries");
    d = turi_doc_lookup("tur/list");
    CHECK(d && strstr(d, "tur/list") != NULL, "a module name resolves too");
    d = turi_doc_lookup("let");
    CHECK(d && strstr(d, "(let ") != NULL, "a special form resolves from the builtin table");
    CHECK(turi_doc_lookup("no-such-name-anywhere") == NULL, "an unknown name misses cleanly");
    CHECK(turi_doc_lookup("a\"quote") == NULL, "a name that would break an eval string misses cleanly");

    for (int i = 0; i < 25; i++) (void)turi_doc_lookup("vec-map");
    CHECK(run_main("(defn main [] : int 2)", "2"),
          "Run after doc lookups re-runs the program (the reported failure)");
    CHECK(run_main("(defn main [] : int 3)", "3"), "and keeps working");

    /* ---- PS3: `(doc ...)` at the prompt ----------------------------------- */
    CHECK(eval_contains("(doc vec-map)", "nil"), "(doc name) evaluates");
    CHECK(eval_contains("(doc 'vec-map)", "nil"), "(doc 'name) evaluates");
    CHECK(eval_contains("(doc \"vec-map\")", "nil"), "(doc \"name\") evaluates");
    CHECK(eval_contains("(doc no-such-name-anywhere)", "nil"), "(doc unknown) evaluates");
    CHECK(run_main("(defn main [] : int 4)", "4"), "Run after (doc ...) still works");

    printf("wasm_glue_session_unit: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
