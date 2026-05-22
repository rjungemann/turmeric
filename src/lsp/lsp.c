#include "lsp.h"
#include "lsp_io.h"
#include "lsp_json.h"
#include "lsp_docs.h"

#include "buf.h"
#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Declared in main.c (non-static wrapper around compile_to_c) */
extern int tur_check_only(const char *path);

/* -------------------------------------------------------------------------
 * JSON helpers
 * --------------------------------------------------------------------- */

static void json_str(Buf *b, const char *s) {
    buf_putc(b, '"');
    for (const char *p = s; p && *p; p++) {
        switch (*p) {
            case '"':  buf_puts(b, "\\\""); break;
            case '\\': buf_puts(b, "\\\\"); break;
            case '\n': buf_puts(b, "\\n");  break;
            case '\r': buf_puts(b, "\\r");  break;
            case '\t': buf_puts(b, "\\t");  break;
            default:
                if ((unsigned char)*p < 0x20)
                    buf_printf(b, "\\u%04x", (unsigned char)*p);
                else
                    buf_putc(b, *p);
        }
    }
    buf_putc(b, '"');
}

static void send_response(int fd_out, const char *id_raw, size_t id_len,
                          const char *result_json) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id_raw && id_len > 0)
        buf_write(&b, id_raw, id_len);
    else
        buf_puts(&b, "null");
    buf_puts(&b, ",\"result\":");
    buf_puts(&b, result_json ? result_json : "null");
    buf_puts(&b, "}");
    lsp_write_message(fd_out, b.data, b.len);
    buf_free(&b);
}

static void send_notification(int fd_out, const char *method,
                              const char *params_json) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":");
    json_str(&b, method);
    buf_puts(&b, ",\"params\":");
    buf_puts(&b, params_json ? params_json : "null");
    buf_puts(&b, "}");
    lsp_write_message(fd_out, b.data, b.len);
    buf_free(&b);
}

/* -------------------------------------------------------------------------
 * Compile + publish diagnostics
 * --------------------------------------------------------------------- */

static void publish_diagnostics(LspDoc *doc, int fd_out) {
    /* Write source to a temp file so compile_to_c can read it */
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/tur_lsp_XXXXXX.tur");
    int tmp_fd = mkstemps(tmp_path, 4);
    if (tmp_fd < 0) return;

    const char *src = doc->text;
    size_t remaining = doc->text_len;
    while (remaining > 0) {
        ssize_t n = write(tmp_fd, src, remaining);
        if (n <= 0) break;
        src += n;
        remaining -= (size_t)n;
    }
    close(tmp_fd);

    /* Run the compiler in LSP collection mode */
    diag_reset();
    diag_init(false);
    diag_lsp_begin();
    tur_check_only(tmp_path);
    diag_lsp_remap_path(tmp_path, doc->path);
    unlink(tmp_path);

    /* Build publishDiagnostics params */
    Buf params;
    buf_init(&params);
    buf_puts(&params, "{\"uri\":");
    json_str(&params, doc->uri);
    buf_puts(&params, ",\"diagnostics\":");
    diag_lsp_flush_array(&params);
    buf_puts(&params, "}");

    diag_lsp_end();

    send_notification(fd_out, "textDocument/publishDiagnostics", params.data);
    buf_free(&params);
}

/* -------------------------------------------------------------------------
 * Request / notification handlers
 * --------------------------------------------------------------------- */

static bool initialized_ = false;
static bool shutdown_    = false;

static void on_initialize(const char *id_raw, size_t id_len, int fd_out) {
    send_response(fd_out, id_raw, id_len,
        "{\"capabilities\":{"
          "\"textDocumentSync\":1,"
          "\"hoverProvider\":false,"
          "\"definitionProvider\":false"
        "}}");
    initialized_ = true;
}

static void on_shutdown(const char *id_raw, size_t id_len, int fd_out) {
    shutdown_ = true;
    send_response(fd_out, id_raw, id_len, "null");
}

static void on_did_open(const char *params, size_t params_len, int fd_out) {
    (void)params_len;
    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) return;

    char uri[1024];
    if (lsp_json_str_copy(td, "uri", uri, sizeof(uri)) < 0) return;

    size_t text_raw_len;
    const char *text_raw = lsp_json_str(td, "text", &text_raw_len);

    /* Unescape the text by round-tripping through a temp buffer */
    char *text = NULL;
    size_t text_len = 0;
    if (text_raw) {
        /* lsp_json_str_copy handles unescaping */
        text = malloc(text_raw_len + 1);
        size_t di = 0;
        for (size_t si = 0; si < text_raw_len && di < text_raw_len; si++) {
            if (text_raw[si] == '\\' && si + 1 < text_raw_len) {
                si++;
                switch (text_raw[si]) {
                    case '"':  text[di++] = '"';  break;
                    case '\\': text[di++] = '\\'; break;
                    case 'n':  text[di++] = '\n'; break;
                    case 'r':  text[di++] = '\r'; break;
                    case 't':  text[di++] = '\t'; break;
                    default:   text[di++] = text_raw[si]; break;
                }
            } else {
                text[di++] = text_raw[si];
            }
        }
        text[di] = '\0';
        text_len = di;
    }

    LspDoc *doc = lsp_doc_open(uri, strlen(uri),
                               text ? text : "", text_len);
    free(text);
    publish_diagnostics(doc, fd_out);
}

static void on_did_change(const char *params, size_t params_len, int fd_out) {
    (void)params_len;
    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) return;

    char uri[1024];
    if (lsp_json_str_copy(td, "uri", uri, sizeof(uri)) < 0) return;

    /* contentChanges is an array; for TextDocumentSyncKind.Full (=1),
     * the first entry has the full new text. */
    size_t cc_len;
    const char *cc_arr = lsp_json_raw(params, "contentChanges", &cc_len);
    if (!cc_arr) return;

    /* Find the first object inside the array */
    const char *p = cc_arr;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p != '{') return;

    size_t text_raw_len;
    const char *text_raw = lsp_json_str(p, "text", &text_raw_len);
    if (!text_raw) return;

    char *text = malloc(text_raw_len + 1);
    size_t di = 0;
    for (size_t si = 0; si < text_raw_len && di < text_raw_len; si++) {
        if (text_raw[si] == '\\' && si + 1 < text_raw_len) {
            si++;
            switch (text_raw[si]) {
                case '"':  text[di++] = '"';  break;
                case '\\': text[di++] = '\\'; break;
                case 'n':  text[di++] = '\n'; break;
                case 'r':  text[di++] = '\r'; break;
                case 't':  text[di++] = '\t'; break;
                default:   text[di++] = text_raw[si]; break;
            }
        } else {
            text[di++] = text_raw[si];
        }
    }
    text[di] = '\0';

    lsp_doc_change(uri, strlen(uri), text, di);
    free(text);

    LspDoc *doc = lsp_doc_get(uri, strlen(uri));
    if (doc) publish_diagnostics(doc, fd_out);
}

static void on_did_close(const char *params, size_t params_len) {
    (void)params_len;
    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) return;
    char uri[1024];
    if (lsp_json_str_copy(td, "uri", uri, sizeof(uri)) < 0) return;
    lsp_doc_close(uri, strlen(uri));
}

/* -------------------------------------------------------------------------
 * Main loop
 * --------------------------------------------------------------------- */

void lsp_server_run(int fd_in, int fd_out) {
    lsp_docs_init();

    char *msg;
    while ((msg = lsp_read_message(fd_in)) != NULL) {
        size_t method_len;
        const char *method_raw = lsp_json_str(msg, "method", &method_len);
        if (!method_raw) { free(msg); continue; }

        char method[128];
        size_t copy = method_len < sizeof(method) - 1 ? method_len : sizeof(method) - 1;
        memcpy(method, method_raw, copy);
        method[copy] = '\0';

        size_t id_len = 0;
        const char *id_raw = lsp_json_raw(msg, "id", &id_len);

        size_t params_len = 0;
        const char *params_raw = lsp_json_raw(msg, "params", &params_len);

        if (strcmp(method, "initialize") == 0) {
            on_initialize(id_raw, id_len, fd_out);
        } else if (strcmp(method, "initialized") == 0) {
            /* no-op */
        } else if (strcmp(method, "shutdown") == 0) {
            on_shutdown(id_raw, id_len, fd_out);
        } else if (strcmp(method, "exit") == 0) {
            free(msg);
            lsp_docs_free();
            _exit(shutdown_ ? 0 : 1);
        } else if (strcmp(method, "textDocument/didOpen") == 0 && params_raw) {
            on_did_open(params_raw, params_len, fd_out);
        } else if (strcmp(method, "textDocument/didChange") == 0 && params_raw) {
            on_did_change(params_raw, params_len, fd_out);
        } else if (strcmp(method, "textDocument/didClose") == 0 && params_raw) {
            on_did_close(params_raw, params_len);
        } else if (id_raw) {
            /* Unknown request: return method-not-found */
            Buf resp;
            buf_init(&resp);
            buf_printf(&resp,
                "{\"jsonrpc\":\"2.0\",\"id\":%.*s"
                ",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}",
                (int)id_len, id_raw);
            lsp_write_message(fd_out, resp.data, resp.len);
            buf_free(&resp);
        }

        free(msg);
    }

    lsp_docs_free();
}
