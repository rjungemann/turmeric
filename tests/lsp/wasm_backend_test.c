/* wasm_backend_test.c -- exercise the browser's analysis backend natively.
 *
 * src/web/wasm_lsp.c is the front end the WASM playground uses instead of
 * main.c's compile_to_c: read, `#lang` sweep, stdlib autoload, elaborate,
 * collect. It normally only ever runs inside a wasm module, where nothing in
 * CI can look at it -- so a mistake in it would surface as "the playground's
 * completion is empty", found by a person, months later.
 *
 * Nothing in that file is actually wasm-specific: the Emscripten build differs
 * only in where the stdlib is mounted, and that is a TUR_STDLIB_DIR lookup.
 * Pointing it at the in-tree stdlib/ and driving the same lsp_session_handle
 * the bridge drives runs the real thing, against the real compiler, here.
 *
 * What this does NOT cover, and cannot: MEMFS temp-file creation (mkstemps
 * under Emscripten), the embedded /stdlib mount, and wasm bundle size. Those
 * need an emcc build.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "lsp_session.h"
#include "platform_fs.h"  /* setenv on Windows */

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { passed++; } \
        else { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); failed++; } \
    } while (0)

static Buf send_msg(const char *json) {
    Buf out;
    lsp_session_handle(json, strlen(json), &out);
    return out;
}

static int contains(const Buf *b, const char *needle) {
    return b->data && strstr(b->data, needle) != NULL;
}

/* JSON-escape a source string into a didOpen and send it. */
static void open_doc(const char *uri, const char *text) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                 "\"params\":{\"textDocument\":{\"uri\":\"");
    buf_puts(&b, uri);
    buf_puts(&b, "\",\"text\":\"");
    for (const char *p = text; *p; p++) {
        switch (*p) {
            case '"':  buf_puts(&b, "\\\""); break;
            case '\\': buf_puts(&b, "\\\\"); break;
            case '\n': buf_puts(&b, "\\n");  break;
            default:   buf_putc(&b, *p);     break;
        }
    }
    buf_puts(&b, "\"}}}");
    buf_putc(&b, '\0');
    Buf out = send_msg(b.data);
    buf_free(&out);
    buf_free(&b);
}

static void fresh(void) {
    lsp_session_reset();
    Buf out = send_msg("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{}}");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * The stdlib is genuinely in scope
 *
 * The playground's analysis has to prepend the same stdlib the compiler does.
 * Without it every buffer would report `cons` and `Option` as unknown -- code
 * the compiler accepts, flagged as broken by the editor sitting next to it.
 * --------------------------------------------------------------------- */

static void test_stdlib_is_autoloaded(void) {
    fresh();
    open_doc("file:///project/main.tur",
             "(defn twice [x : int] : int (* x 2))\nvec-\n");

    /* Query with a `vec-` prefix rather than an empty one. The response is
     * capped at 200 items and the stdlib's typeclass instances alone overrun
     * that, so an unfiltered query proves nothing about whether list.tur or
     * vec.tur were loaded -- it only proves the cap works. */
    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/completion\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":1,\"character\":4}}}");

    CHECK(contains(&out, "\"label\":\"vec-new\"") ||
          contains(&out, "\"label\":\"vec-push\""),
          "stdlib symbols are in scope without an explicit import");
    CHECK(!contains(&out, "\"label\":\"twice\""),
          "the prefix filter is real, not a raw dump");
    buf_free(&out);
}

static void test_document_local_symbols_come_first(void) {
    fresh();
    open_doc("file:///project/main.tur",
             "(defn twice [x : int] : int (* x 2))\n");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"textDocument/completion\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":1,\"character\":0}}}");

    /* With the cap in play this is not a nicety: the buffer's own definitions
     * are exactly the ones that get dropped if collection order wins. */
    const char *items = out.data ? strstr(out.data, "\"items\":[") : NULL;
    CHECK(items && strncmp(items, "\"items\":[{\"label\":\"twice\"", 25) == 0,
          "the document's own definition is the first item offered");
    buf_free(&out);
}

static void test_hover_reports_a_real_type(void) {
    fresh();
    open_doc("file:///project/main.tur",
             "(defn twice [x : int] : int (* x 2))\n(twice 21)\n");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/hover\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":1,\"character\":2}}}");

    CHECK(contains(&out, "twice"), "hover finds the symbol under the cursor");
    /* The rendered type comes from the elaborator, not from re-reading the
     * source, so this is the assertion that says elaboration really ran. */
    CHECK(contains(&out, "int"), "hover carries the inferred type");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * Diagnostics
 * --------------------------------------------------------------------- */

static void test_type_error_becomes_a_diagnostic(void) {
    fresh();
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                 "\"params\":{\"textDocument\":{\"uri\":\"file:///project/bad.tur\","
                 "\"text\":\"(defn f [] : int (undefined-name 1))\"}}}");
    buf_putc(&b, '\0');
    Buf out = send_msg(b.data);

    CHECK(contains(&out, "textDocument/publishDiagnostics"),
          "an erroring buffer publishes diagnostics");
    CHECK(contains(&out, "\"severity\":1"),
          "the error is reported at severity 1");
    CHECK(contains(&out, "file:///project/bad.tur"),
          "the diagnostic is attributed to the document");
    CHECK(!contains(&out, "tur_lsp_"),
          "the analysis temp path never reaches the client");
    buf_free(&out);
    buf_free(&b);
}

static void test_clean_buffer_publishes_no_diagnostics(void) {
    fresh();
    open_doc("file:///project/ok.tur", "(defn f [] : int 1)\n");

    /* Re-analyse by editing, so the assertion is about a real compile rather
     * than about nothing having happened yet. */
    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/ok.tur\"},"
        "\"contentChanges\":[{\"text\":\"(defn f [] : int 2)\\n\"}]}}");

    CHECK(contains(&out, "\"diagnostics\":[]"),
          "a buffer that compiles reports no markers");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * #lang handling
 *
 * Try Turmeric's default buffer opens with `#lang turmeric/sweet`. If the
 * analysis handed that to the s-expression reader, the very first thing every
 * visitor saw would be a buffer covered in syntax errors describing nothing
 * they did.
 * --------------------------------------------------------------------- */

static void test_sweet_exp_buffer_is_read_correctly(void) {
    fresh();
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                 "\"params\":{\"textDocument\":{\"uri\":\"file:///project/sweet.tur\","
                 "\"text\":\"#lang turmeric/sweet\\n\\n"
                 "defn twice [x : int] : int\\n  *(x 2)\\n\"}}}");
    buf_putc(&b, '\0');
    Buf out = send_msg(b.data);

    CHECK(contains(&out, "\"diagnostics\":[]"),
          "a #lang turmeric/sweet buffer analyses without syntax errors");
    buf_free(&out);
    buf_free(&b);

    Buf syms = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/documentSymbol\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/sweet.tur\"}}}");
    CHECK(contains(&syms, "\"name\":\"twice\""),
          "sweet-exp definitions land in the symbol index");
    buf_free(&syms);
}

/* -------------------------------------------------------------------------
 * Cross-tab workspace symbols
 *
 * workspace/symbol covers open documents only -- recorded as a gap against
 * native clients, where most of a project is on disk and unopened. In the
 * playground every tab IS an open document, so the gap does not exist here.
 * --------------------------------------------------------------------- */

static void test_workspace_symbol_spans_tabs(void) {
    fresh();
    open_doc("file:///project/a.tur", "(defn alpha-fn [] : int 1)\n");
    open_doc("file:///project/b.tur", "(defn beta-fn [] : int 2)\n");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"workspace/symbol\","
        "\"params\":{\"query\":\"-fn\"}}");

    CHECK(contains(&out, "\"name\":\"alpha-fn\""), "tab a contributes symbols");
    CHECK(contains(&out, "\"name\":\"beta-fn\""),  "tab b contributes symbols");
    CHECK(contains(&out, "file:///project/a.tur"),
          "each result names the tab it came from");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * Definition across tabs
 * --------------------------------------------------------------------- */

static void test_definition_within_a_tab(void) {
    fresh();
    open_doc("file:///project/main.tur",
             "(defn helper [] : int 7)\n(defn use-it [] : int (helper))\n");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"textDocument/definition\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":1,\"character\":23}}}");

    CHECK(contains(&out, "file:///project/main.tur"),
          "definition resolves to the document's own uri");
    CHECK(contains(&out, "\"line\":0"),
          "definition points at the defining line, 0-based");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * A buffer that has never parsed still completes
 * --------------------------------------------------------------------- */

static void test_broken_buffer_falls_back_to_stdlib(void) {
    fresh();
    /* Opened already-unbalanced: there is no earlier revision to fall back on,
     * so the stdlib surface is the only honest answer -- and it is a correct
     * one, because Turmeric auto-loads it. */
    open_doc("file:///project/broken.tur", "(defn f [] : int (vec-\n");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"textDocument/completion\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/broken.tur\"},"
        "\"position\":{\"line\":0,\"character\":22}}}");

    CHECK(contains(&out, "\"label\":\"vec-new\"") ||
          contains(&out, "\"label\":\"vec-push\""),
          "a never-parsed buffer still completes stdlib names");
    CHECK(!contains(&out, "\"label\":\"f\""),
          "the fallback carries no document-local symbols");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */

int main(int argc, char **argv) {
    /* The wasm build finds the stdlib at its MEMFS mount point; here it is
     * wherever the harness says, defaulting to the in-tree copy. */
    const char *stdlib_dir = (argc > 1) ? argv[1] : "stdlib";
    setenv("TUR_STDLIB_DIR", stdlib_dir, 1);

    printf("Running WASM LSP backend tests (stdlib=%s)...\n", stdlib_dir);

    test_stdlib_is_autoloaded();
    test_document_local_symbols_come_first();
    test_hover_reports_a_real_type();
    test_type_error_becomes_a_diagnostic();
    test_clean_buffer_publishes_no_diagnostics();
    test_sweet_exp_buffer_is_read_correctly();
    test_workspace_symbol_spans_tabs();
    test_definition_within_a_tab();
    test_broken_buffer_falls_back_to_stdlib();

    lsp_session_reset();

    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
