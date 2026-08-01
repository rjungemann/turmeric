/* session_test.c -- unit tests for the transport-free LSP session
 * (src/lsp/lsp_session.c + the dispatch lifted out of src/lsp/lsp.c).
 *
 * The point of the transport split is that the server's behaviour no longer
 * depends on there being a pipe. This harness is the proof: it drives scripted
 * JSON-RPC messages straight through lsp_session_handle() and asserts on the
 * JSON array that comes back. No file descriptors, no subprocess, no editor --
 * which also makes it the cheapest place to pin the browser path's behaviour,
 * since the browser path IS this function.
 *
 * tur_collect_symbols is stubbed rather than real. Running the actual compiler
 * would make every assertion here a hostage to whatever the stdlib currently
 * exports; a fixed two-symbol index (one document-local, one "stdlib") makes
 * the ordering, filtering, and path-remapping rules testable by inspection.
 * tests/lsp/mcp_lsp_test.py covers the server against the real compiler.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "diag.h"
#include "lsp_session.h"
#include "lsp_sym.h"
#include "platform_fs.h"  /* setenv on Windows */

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { passed++; } \
        else { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); failed++; } \
    } while (0)

/* -------------------------------------------------------------------------
 * Stub analysis backend
 *
 * Answers as if the buffer defined `local-fn`, and as if the stdlib defined
 * `cons`. The caller hands us the temp path it wrote the document to, and the
 * real collector reports that path on the document's own symbols -- so the
 * stub does too, which is what run_doc_analysis's remap-to-doc-path step keys
 * off.
 * --------------------------------------------------------------------- */

#define STUB_STDLIB_DIR "/stub-stdlib"

/* Set by a test to make the next analysis report a compile error. */
static int stub_emit_error = 0;
/* Set by a test to make the next analysis yield nothing (the "buffer does not
 * parse" state, which is the normal state while typing). */
static int stub_yield_nothing = 0;

static void stub_sym(LspSymbol *s, const char *name, const char *type,
                     const char *file, int line, int c0, int c1) {
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->type_str, sizeof(s->type_str), "%s", type);
    snprintf(s->file_path, sizeof(s->file_path), "%s", file);
    s->line = line;
    s->col_start = c0;
    s->col_end = c1;
}

int tur_collect_symbols(const char *source_path, LspSymbol *out, int cap,
                        int *count_out) {
    *count_out = 0;

    /* The stdlib prime analyses an empty scratch file, which always compiles
     * -- so "this buffer does not parse" must not make the prime fail too, or
     * the fallback would be untestable. Recognise it by name, the way the
     * server names it. */
    int is_prime = strstr(source_path, "tur_lsp_std_") != NULL;
    if (is_prime) {
        if (cap >= 1)
            stub_sym(&out[(*count_out)++], "cons", "(fn [int int] : int)",
                     STUB_STDLIB_DIR "/list.tur", 12, 7, 11);
        return 0;
    }

    if (stub_yield_nothing) return 1;

    if (cap >= 1)
        stub_sym(&out[(*count_out)++], "local-fn", "(fn [int int] : int)",
                 source_path, 3, 7, 15);
    if (cap >= 2)
        stub_sym(&out[(*count_out)++], "cons", "(fn [int int] : int)",
                 STUB_STDLIB_DIR "/list.tur", 12, 7, 11);

    if (stub_emit_error) {
        /* Diagnostics are reported against the temp file the server wrote, and
         * run_doc_analysis remaps that path back to the document's. Register
         * the file so the span has something to resolve to, exactly as a real
         * compile would. */
        static char path_copy[512];
        snprintf(path_copy, sizeof(path_copy), "%s", source_path);
        SourceFile f = {0};
        f.path = path_copy;
        f.src = "";
        f.len = 0;
        f.file_id = 0;
        diag_register_file(&f);
        Span span = {0};
        span.file_id = 0;
        span.line = 2;
        span.col_start = 3;
        span.col_end = 8;
        diag_emit(DIAG_ERROR, span, "stub: undefined name `nope`");
        return 1;
    }
    return 0;
}

/* lsp.c declares this for the `tur check` path; nothing in the session
 * reaches it, but the declaration is enough for a linker to want a body. */
int tur_check_only(const char *path) {
    (void)path;
    return 0;
}

/* -------------------------------------------------------------------------
 * Harness helpers
 * --------------------------------------------------------------------- */

/* Send one message and return the reply array. Caller frees with buf_free. */
static Buf send_msg(const char *json) {
    Buf out;
    lsp_session_handle(json, strlen(json), &out);
    return out;
}

static int contains(const Buf *b, const char *needle) {
    return b->data && strstr(b->data, needle) != NULL;
}

/* Byte offset of `needle` in the reply, or -1. Used to assert ordering
 * between two items in one array. */
static long index_of(const Buf *b, const char *needle) {
    if (!b->data) return -1;
    const char *p = strstr(b->data, needle);
    return p ? (long)(p - b->data) : -1;
}

static void session_open(const char *uri, const char *text) {
    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                   "\"params\":{\"textDocument\":{\"uri\":\"%s\",\"text\":\"%s\"}}}",
               uri, text);
    buf_putc(&b, '\0');
    Buf out = send_msg(b.data);
    buf_free(&out);
    buf_free(&b);
}

static void fresh_session(void) {
    stub_emit_error = 0;
    stub_yield_nothing = 0;
    lsp_session_reset();
    Buf out = send_msg("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{}}");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * initialize
 * --------------------------------------------------------------------- */

static void test_initialize(void) {
    lsp_session_reset();
    Buf out = send_msg("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{}}");

    CHECK(out.data && out.data[0] == '[', "initialize reply is a JSON array");
    CHECK(contains(&out, "\"id\":1"), "initialize echoes the request id");
    CHECK(contains(&out, "\"positionEncoding\":\"utf-8\""),
          "initialize declares utf-8 position encoding");
    CHECK(contains(&out, "\"completionProvider\""),
          "initialize advertises completion");
    CHECK(contains(&out, "\"signatureHelpProvider\""),
          "initialize advertises signature help");
    /* Exactly one message: nothing is open, so no diagnostics ride along. */
    CHECK(!contains(&out, "publishDiagnostics"),
          "initialize alone publishes no diagnostics");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * didOpen publishes diagnostics without anyone asking
 *
 * The stdio server defers analysis until poll() says the client went quiet.
 * A caller with no descriptor to poll gets the flush at the end of the
 * message instead -- without it, an edited buffer's diagnostics would not
 * appear until some unrelated request happened to need symbols.
 * --------------------------------------------------------------------- */

static void test_did_open_publishes_diagnostics(void) {
    fresh_session();
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                 "\"params\":{\"textDocument\":"
                 "{\"uri\":\"file:///project/main.tur\",\"text\":\"(defn f [] 1)\"}}}");
    buf_putc(&b, '\0');
    Buf out = send_msg(b.data);

    CHECK(contains(&out, "textDocument/publishDiagnostics"),
          "didOpen flushes analysis and publishes diagnostics");
    CHECK(contains(&out, "file:///project/main.tur"),
          "publishDiagnostics names the document uri");
    CHECK(contains(&out, "\"diagnostics\":[]"),
          "a clean stub compile publishes an empty diagnostic list");
    buf_free(&out);
    buf_free(&b);
}

static void test_diagnostics_reach_the_client(void) {
    fresh_session();
    stub_emit_error = 1;
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
                 "\"params\":{\"textDocument\":"
                 "{\"uri\":\"file:///project/bad.tur\",\"text\":\"(nope)\"}}}");
    buf_putc(&b, '\0');
    Buf out = send_msg(b.data);

    CHECK(contains(&out, "stub: undefined name"),
          "a compile error reaches publishDiagnostics");
    CHECK(contains(&out, "\"severity\":1"),
          "an error maps to LSP severity 1");
    CHECK(contains(&out, "file:///project/bad.tur"),
          "the diagnostic is attributed to the document, not the temp file");
    CHECK(!contains(&out, "tur_lsp_"),
          "the server's temp path never leaks to the client");
    buf_free(&out);
    buf_free(&b);
    stub_emit_error = 0;
}

/* -------------------------------------------------------------------------
 * completion
 * --------------------------------------------------------------------- */

static void test_completion_orders_local_first(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(defn local-fn [a b] : int a)");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"textDocument/completion\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":0}}}");

    CHECK(contains(&out, "\"isIncomplete\":false"),
          "completion answers with a CompletionList");
    CHECK(contains(&out, "\"label\":\"local-fn\""),
          "completion offers the document's own definition");
    CHECK(contains(&out, "\"label\":\"cons\""),
          "completion offers stdlib symbols too");
    long local = index_of(&out, "\"label\":\"local-fn\"");
    long std_  = index_of(&out, "\"label\":\"cons\"");
    CHECK(local >= 0 && std_ >= 0 && local < std_,
          "document-local symbols come before stdlib ones");
    buf_free(&out);
}

static void test_completion_filters_by_prefix(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(co");

    /* Cursor at the end of "(co" -- the prefix is what precedes it. */
    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"textDocument/completion\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":3}}}");

    CHECK(contains(&out, "\"label\":\"cons\""),
          "prefix `co` keeps cons");
    CHECK(!contains(&out, "\"label\":\"local-fn\""),
          "prefix `co` drops local-fn");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * hover / definition / documentSymbol / signatureHelp
 * --------------------------------------------------------------------- */

static void test_hover(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(cons 1 2)");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"textDocument/hover\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":2}}}");

    CHECK(contains(&out, "\"kind\":\"markdown\""),
          "hover honours the 3.17 markdown default");
    CHECK(contains(&out, "(cons : (fn [int int] : int))"),
          "hover renders the symbol's type");
    buf_free(&out);
}

static void test_definition_reports_document_path(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(local-fn 1 2)");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"textDocument/definition\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":2}}}");

    /* The stub reports the temp path; run_doc_analysis rewrites it to the
     * document's own path before the client ever sees it. A client that
     * followed the temp path would open a file that has already been
     * unlinked. */
    CHECK(contains(&out, "file:///project/main.tur"),
          "definition points at the document, not the analysis temp file");
    CHECK(contains(&out, "\"line\":2"),
          "definition converts the 1-based span to a 0-based LSP position");
    buf_free(&out);
}

static void test_document_symbol_is_file_scoped(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(defn local-fn [a b] : int a)");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"textDocument/documentSymbol\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"}}}");

    CHECK(contains(&out, "\"name\":\"local-fn\""),
          "documentSymbol lists the document's definitions");
    CHECK(!contains(&out, "\"name\":\"cons\""),
          "documentSymbol excludes symbols defined elsewhere");
    buf_free(&out);
}

static void test_signature_help(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(cons 1 ");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"textDocument/signatureHelp\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":8}}}");

    CHECK(contains(&out, "\"signatures\""), "signatureHelp answers a signature");
    CHECK(contains(&out, "(cons : (fn [int int] : int))"),
          "the signature label carries the rendered type");
    CHECK(contains(&out, "\"activeParameter\":1"),
          "the cursor after the first argument selects parameter 1");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * didChange re-analyses; the last-good index survives a broken buffer
 * --------------------------------------------------------------------- */

static void test_did_change_republishes(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(defn local-fn [a b] : int a)");

    stub_emit_error = 1;
    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"contentChanges\":[{\"text\":\"(nope\"}]}}");

    CHECK(contains(&out, "textDocument/publishDiagnostics"),
          "didChange republishes diagnostics without a separate request");
    CHECK(contains(&out, "stub: undefined name"),
          "the new diagnostics describe the new text");
    buf_free(&out);
    stub_emit_error = 0;
}

static void test_stale_index_survives_unparseable_text(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(defn local-fn [a b] : int a)");

    /* Mid-edit the buffer does not parse and yields no symbols at all. The
     * index from the previous revision has to be retained -- dropping it takes
     * completion to zero answers at precisely the moment it is wanted. */
    stub_yield_nothing = 1;
    Buf changed = send_msg(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"contentChanges\":[{\"text\":\"(local-\"}]}}");
    buf_free(&changed);

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"textDocument/completion\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":7}}}");

    CHECK(contains(&out, "\"label\":\"local-fn\""),
          "an unparseable buffer keeps serving the last good index");
    buf_free(&out);
    stub_yield_nothing = 0;
}

/* -------------------------------------------------------------------------
 * didChange batching: Full sync means the last change wins
 * --------------------------------------------------------------------- */

static void test_did_change_takes_the_last_element(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(defn local-fn [a b] : int a)");

    Buf changed = send_msg(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"contentChanges\":[{\"text\":\"(co\"},{\"text\":\"(local-\"}]}}");
    buf_free(&changed);

    /* If element 0 had won, the prefix at column 7 would be past the end of
     * "(co" and the filter would behave differently. */
    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"textDocument/completion\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":7}}}");

    CHECK(contains(&out, "\"label\":\"local-fn\""),
          "the last contentChanges element is the one adopted");
    CHECK(!contains(&out, "\"label\":\"cons\""),
          "prefix `local-` drops cons");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * Protocol edges
 * --------------------------------------------------------------------- */

static void test_unknown_method_is_answered(void) {
    fresh_session();
    Buf out = send_msg("{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"textDocument/rename\","
                       "\"params\":{}}");
    CHECK(contains(&out, "-32601"), "an unknown request gets Method not found");
    CHECK(contains(&out, "\"id\":42"), "the error echoes the request id");
    buf_free(&out);
}

static void test_unknown_notification_is_silent(void) {
    fresh_session();
    /* No id means a notification, and a notification must never be answered. */
    Buf out = send_msg("{\"jsonrpc\":\"2.0\",\"method\":\"$/setTrace\","
                       "\"params\":{\"value\":\"off\"}}");
    CHECK(out.data && strcmp(out.data, "[]") == 0,
          "an unknown notification produces no reply at all");
    buf_free(&out);
}

static void test_cancel_on_the_session_path_is_a_noop(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(cons 1 2)");

    Buf cancel = send_msg("{\"jsonrpc\":\"2.0\",\"method\":\"$/cancelRequest\","
                          "\"params\":{\"id\":15}}");
    CHECK(cancel.data && strcmp(cancel.data, "[]") == 0,
          "$/cancelRequest itself is never answered");
    buf_free(&cancel);

    /* There is no descriptor to drain, so a cancel that arrives before the
     * request it names cannot strand that id: request 15 must still be
     * answered normally rather than with -32800. */
    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"textDocument/hover\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":2}}}");
    CHECK(!contains(&out, "-32800"),
          "a cancel with nothing to cancel does not poison a later id");
    CHECK(contains(&out, "\"contents\""), "the request is answered normally");
    buf_free(&out);
}

static void test_formatting_skips_the_analysis_flush(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(defn f [] 1)");

    Buf changed = send_msg(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"contentChanges\":[{\"text\":\"(defn  f  []  1)\"}]}}");
    buf_free(&changed);

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"textDocument/formatting\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"options\":{\"tabSize\":2,\"insertSpaces\":true}}}");

    CHECK(contains(&out, "newText"), "formatting returns a full-document edit");
    CHECK(contains(&out, "(defn f [] 1)"),
          "formatting reads doc->text, so it sees the edit without an analysis");
    buf_free(&out);
}

static void test_formatting_with_no_params_errors(void) {
    fresh_session();
    Buf out = send_msg("{\"jsonrpc\":\"2.0\",\"id\":17,"
                       "\"method\":\"textDocument/formatting\"}");
    CHECK(contains(&out, "-32602"), "a request without params gets Invalid params");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * Lifecycle: exit tears the session down, it does not kill the process
 * --------------------------------------------------------------------- */

static void test_exit_ends_the_session_without_exiting(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(cons 1 2)");

    Buf out;
    bool alive = lsp_session_handle("{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}",
                                    strlen("{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}"),
                                    &out);
    CHECK(!alive, "exit reports the session is over");
    CHECK(out.data && strcmp(out.data, "[]") == 0, "exit emits no message");
    buf_free(&out);

    /* Reaching this line at all is most of the assertion: the stdio loop
     * calls _exit() here, and a browser tab is not something to terminate on
     * a client's say-so. The session must also be genuinely fresh afterwards. */
    Buf reopened = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":18,\"method\":\"textDocument/documentSymbol\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"}}}");
    CHECK(contains(&reopened, "\"result\":[]"),
          "documents opened before exit are gone afterwards");
    buf_free(&reopened);
}

/* The stdlib fallback has to survive a reset.
 *
 * The prime used to latch on a function-local static, which on stdio is
 * indistinguishable from correct -- one session is one process there. In a
 * browser the module outlives the session: the latch stayed set while the
 * cache it guarded was freed, so from the second session onward a buffer
 * opened with a syntax error already in it had completion dead for the life
 * of the page, which is exactly the case the fallback exists for. */
static void test_stdlib_fallback_survives_a_reset(void) {
    for (int round = 0; round < 2; round++) {
        fresh_session();
        stub_yield_nothing = 1;
        session_open("file:///project/never-parsed.tur", "(cons");

        Buf out = send_msg(
            "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"textDocument/completion\","
            "\"params\":{\"textDocument\":{\"uri\":\"file:///project/never-parsed.tur\"},"
            "\"position\":{\"line\":0,\"character\":5}}}");

        CHECK(contains(&out, "\"label\":\"cons\""),
              round == 0 ? "a never-parsed buffer completes from the stdlib"
                         : "and still does after the session was reset");
        buf_free(&out);
        stub_yield_nothing = 0;
    }
}

static void test_flush_with_nothing_dirty_is_empty(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(cons 1 2)");

    /* didOpen already flushed, so an explicit flush has nothing to do. A
     * client polling its debounce timer must not get a fresh compile (and a
     * fresh marker repaint) out of an idle buffer. */
    Buf out;
    lsp_session_flush(&out);
    CHECK(out.data && strcmp(out.data, "[]") == 0,
          "flushing a clean session publishes nothing");
    buf_free(&out);
}

static void test_did_close_drops_the_document(void) {
    fresh_session();
    session_open("file:///project/main.tur", "(cons 1 2)");

    Buf closed = send_msg(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"}}}");
    buf_free(&closed);

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"textDocument/hover\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":0,\"character\":2}}}");
    CHECK(contains(&out, "\"result\":null"),
          "hover on a closed document answers null");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * Text decoding
 * --------------------------------------------------------------------- */

static void test_escapes_are_decoded(void) {
    fresh_session();
    /* é has to become the two UTF-8 bytes, not the five literal
     * characters -- otherwise every byte offset after it in the buffer shifts
     * and the positions in every later answer are wrong. */
    session_open("file:///project/main.tur",
                 "(def caf\\u00e9 1)\\n(local-fn 1 2)");

    Buf out = send_msg(
        "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"textDocument/hover\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///project/main.tur\"},"
        "\"position\":{\"line\":1,\"character\":2}}}");
    CHECK(contains(&out, "local-fn"),
          "a decoded \\uXXXX escape keeps later line/column offsets correct");
    buf_free(&out);
}

/* -------------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */

int main(void) {
    printf("Running LSP session tests...\n");

    /* The stdlib cache is keyed off this; the stub reports `cons` as living
     * under it so the never-analyzed-document fallback has something to serve. */
    setenv("TUR_STDLIB_DIR", STUB_STDLIB_DIR, 1);
    diag_init(false);

    test_initialize();
    test_did_open_publishes_diagnostics();
    test_diagnostics_reach_the_client();
    test_completion_orders_local_first();
    test_completion_filters_by_prefix();
    test_hover();
    test_definition_reports_document_path();
    test_document_symbol_is_file_scoped();
    test_signature_help();
    test_did_change_republishes();
    test_stale_index_survives_unparseable_text();
    test_did_change_takes_the_last_element();
    test_unknown_method_is_answered();
    test_unknown_notification_is_silent();
    test_cancel_on_the_session_path_is_a_noop();
    test_formatting_skips_the_analysis_flush();
    test_formatting_with_no_params_errors();
    test_exit_ends_the_session_without_exiting();
    test_stdlib_fallback_survives_a_reset();
    test_flush_with_nothing_dirty_is_empty();
    test_did_close_drops_the_document();
    test_escapes_are_decoded();

    lsp_session_reset();

    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
