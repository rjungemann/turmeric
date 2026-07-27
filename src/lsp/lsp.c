#include "lsp.h"
#include "lsp_io.h"
#include "lsp_json.h"
#include "lsp_docs.h"
#include "lsp_sym.h"
#include "lsp_util.h"

#include "buf.h"
#include "diag.h"
#include "platform_fs.h"  /* mkstemps() on Windows */

#include <errno.h>
#include <poll.h>
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

/* A request must always be answered. Dropping one leaves a conforming client
 * waiting forever on an id that will never come back. */
static void send_error(int fd_out, const char *id_raw, size_t id_len,
                       int code, const char *message) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id_raw && id_len > 0)
        buf_write(&b, id_raw, id_len);
    else
        buf_puts(&b, "null");
    buf_printf(&b, ",\"error\":{\"code\":%d,\"message\":", code);
    json_str(&b, message);
    buf_puts(&b, "}}");
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
 * Symbol search helper
 * --------------------------------------------------------------------- */

static const LspSymbol *find_symbol(const LspDoc *doc, const char *name) {
    for (int i = 0; i < doc->symbol_count; i++) {
        if (strcmp(doc->symbols[i].name, name) == 0)
            return &doc->symbols[i];
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Compile + publish diagnostics + collect symbols
 * --------------------------------------------------------------------- */

/* Parse exactly 4 hex digits; returns the value, or -1 if they aren't hex. */
static int hex4(const char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int d;
        char c = s[i];
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = (v << 4) | d;
    }
    return v;
}

/* Encode one Unicode scalar as UTF-8. Returns bytes written (1..4). */
static size_t utf8_encode(unsigned int cp, char *dst) {
    if (cp < 0x80u) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        dst[0] = (char)(0xC0u | (cp >> 6));
        dst[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        dst[0] = (char)(0xE0u | (cp >> 12));
        dst[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    dst[0] = (char)(0xF0u | (cp >> 18));
    dst[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    dst[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    dst[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* Decode a JSON string body into `dst`, which must have room for src_len + 1
 * bytes. That bound always holds: every escape shrinks (\uXXXX is 6 input
 * bytes for at most 3 output bytes; a surrogate pair is 12 for 4).
 *
 * \uXXXX must be decoded, not passed through. Dropping the backslash and
 * emitting the literal "u00e9" would corrupt the text *and* shift every byte
 * offset after it, so every diagnostic and hover position later in the file
 * would land on the wrong character. */
static size_t unescape_json(const char *src, size_t src_len, char *dst) {
    size_t di = 0;
    for (size_t si = 0; si < src_len; si++) {
        if (src[si] != '\\' || si + 1 >= src_len) {
            dst[di++] = src[si];
            continue;
        }
        si++;
        switch (src[si]) {
            case '"':  dst[di++] = '"';  break;
            case '\\': dst[di++] = '\\'; break;
            case '/':  dst[di++] = '/';  break;
            case 'b':  dst[di++] = '\b'; break;
            case 'f':  dst[di++] = '\f'; break;
            case 'n':  dst[di++] = '\n'; break;
            case 'r':  dst[di++] = '\r'; break;
            case 't':  dst[di++] = '\t'; break;
            case 'u': {
                if (si + 4 >= src_len) { dst[di++] = src[si]; break; }
                int hi = hex4(src + si + 1);
                if (hi < 0) { dst[di++] = src[si]; break; }
                si += 4;
                unsigned int cp = (unsigned int)hi;
                /* A high surrogate is only meaningful paired with the low one
                 * that follows; together they encode a non-BMP scalar. */
                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    if (si + 6 < src_len && src[si + 1] == '\\' && src[si + 2] == 'u') {
                        int lo = hex4(src + si + 3);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000u + ((cp - 0xD800u) << 10)
                               + ((unsigned int)lo - 0xDC00u);
                            si += 6;
                        }
                    }
                }
                /* An unpaired surrogate is not a legal scalar; substituting
                 * U+FFFD keeps the output valid UTF-8 rather than emitting a
                 * byte sequence the compiler would choke on. */
                if (cp >= 0xD800u && cp <= 0xDFFFu) cp = 0xFFFDu;
                di += utf8_encode(cp, dst + di);
                break;
            }
            default: dst[di++] = src[si]; break;
        }
    }
    dst[di] = '\0';
    return di;
}

#define LSP_SYM_CAP 2048

static void run_doc_analysis(LspDoc *doc, int fd_out) {
    /* 1. Scan docstrings from source text */
    LspDocTable dtable;
    lsp_doc_table_init(&dtable);
    lsp_scan_docs(doc->text, doc->text_len, &dtable);

    /* 2. Write source to a temp file */
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/tur_lsp_XXXXXX.tur");
    int tmp_fd = mkstemps(tmp_path, 4);
    if (tmp_fd < 0) {
        lsp_doc_table_free(&dtable);
        return;
    }
    const char *src = doc->text;
    size_t remaining = doc->text_len;
    while (remaining > 0) {
        ssize_t n = write(tmp_fd, src, remaining);
        if (n <= 0) break;
        src += n;
        remaining -= (size_t)n;
    }
    close(tmp_fd);

    /* 3. Allocate symbol buffer */
    lsp_doc_free_symbols(doc);
    doc->symbols      = calloc((size_t)LSP_SYM_CAP, sizeof(LspSymbol));
    doc->symbol_cap   = LSP_SYM_CAP;
    doc->symbol_count = 0;

    /* 4. Run the compiler: collect symbols + gather diagnostics */
    diag_reset();
    diag_init(false);
    diag_lsp_begin();
    tur_collect_symbols(tmp_path, doc->symbols, doc->symbol_cap,
                        &doc->symbol_count);
    diag_lsp_remap_path(tmp_path, doc->path);

    /* 5. Populate docstrings and fix up file paths */
    for (int i = 0; i < doc->symbol_count; i++) {
        LspSymbol *sym = &doc->symbols[i];
        /* Fill docstring from the re-scanner */
        lsp_scan_docs_lookup(&dtable, sym->name,
                             sym->doc, sizeof(sym->doc));
        /* Remap temp file path to the real document path */
        if (strcmp(sym->file_path, tmp_path) == 0)
            strncpy(sym->file_path, doc->path, sizeof(sym->file_path) - 1);
    }

    unlink(tmp_path);

    /* 6. Build and send publishDiagnostics notification */
    Buf params;
    buf_init(&params);
    buf_puts(&params, "{\"uri\":");
    json_str(&params, doc->uri);
    buf_puts(&params, ",\"diagnostics\":");
    diag_lsp_flush_array(&params);
    buf_puts(&params, "}");
    buf_putc(&params, '\0'); /* NUL-terminate for C-string use; strlen stops here */
    diag_lsp_end();

    send_notification(fd_out, "textDocument/publishDiagnostics", params.data);
    buf_free(&params);
    lsp_doc_table_free(&dtable);
}

/* Analyze every document whose text changed since it was last looked at, and
 * publish its diagnostics.
 *
 * Analysis is deferred rather than run inline from didOpen/didChange because
 * run_doc_analysis writes the buffer to a temp file and runs a full compile on
 * this thread. Doing that per keystroke made a fast typist queue one compile
 * per character, and since there is no $/cancelRequest, every hover and
 * completion behind them waited too. */
static void flush_one_dirty(LspDoc *doc, void *ctx) {
    if (!doc->dirty) return;
    doc->dirty = 0;
    run_doc_analysis(doc, *(int *)ctx);
}

static void lsp_flush_dirty(int fd_out) {
    lsp_docs_iterate_mut(flush_one_dirty, &fd_out);
}

/* Wait up to timeout_ms for another message to show up on fd_in.
 * Returns 1 if input is pending, 0 on timeout.
 *
 * Safe only because lsp_read_message reads straight from the fd with no
 * userspace buffer of its own — otherwise a buffered message could be sitting
 * in memory while poll() reported the fd quiet. */
static int input_pending(int fd_in, int timeout_ms) {
    struct pollfd pfd = { .fd = fd_in, .events = POLLIN, .revents = 0 };
    for (;;) {
        int n = poll(&pfd, 1, timeout_ms);
        if (n < 0 && errno == EINTR) continue;  /* not a timeout; keep waiting */
        return n > 0;
    }
}

/* How long the client must go quiet before a changed document is analyzed. */
#define LSP_ANALYSIS_DEBOUNCE_MS 200

/* -------------------------------------------------------------------------
 * Request / notification handlers
 * --------------------------------------------------------------------- */

static bool initialized_ = false;
static bool shutdown_    = false;

static void on_initialize(const char *id_raw, size_t id_len, int fd_out) {
    send_response(fd_out, id_raw, id_len,
        "{\"capabilities\":{"
          /* LSP 3.17 positionEncoding. `character` offsets here have always
           * been byte offsets (see lsp_word_at_pos in lsp_util.c), not the
           * UTF-16 code units the spec defaults to. Declaring utf-8 makes that
           * the negotiated truth instead of a silent mismatch that only shows
           * up on lines containing non-ASCII. */
          "\"positionEncoding\":\"utf-8\","
          "\"textDocumentSync\":1,"
          "\"hoverProvider\":true,"
          "\"definitionProvider\":true,"
          "\"documentSymbolProvider\":true,"
          "\"workspaceSymbolProvider\":true,"
          "\"completionProvider\":{"
            "\"triggerCharacters\":[\"(\",\" \"]"
          "}"
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

    char *text = NULL;
    size_t text_len = 0;
    if (text_raw) {
        text = malloc(text_raw_len + 1);
        text_len = unescape_json(text_raw, text_raw_len, text);
    }

    lsp_doc_open(uri, strlen(uri), text ? text : "", text_len);
    free(text);
    (void)fd_out;  /* analysis is deferred; see lsp_flush_dirty */
}

static void on_did_change(const char *params, size_t params_len, int fd_out) {
    (void)params_len;
    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) return;

    char uri[1024];
    if (lsp_json_str_copy(td, "uri", uri, sizeof(uri)) < 0) return;

    size_t cc_len;
    const char *cc_arr = lsp_json_raw(params, "contentChanges", &cc_len);
    if (!cc_arr) return;

    const char *p = cc_arr;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p != '{') return;

    size_t text_raw_len;
    const char *text_raw = lsp_json_str(p, "text", &text_raw_len);
    if (!text_raw) return;

    char *text = malloc(text_raw_len + 1);
    size_t text_len = unescape_json(text_raw, text_raw_len, text);

    lsp_doc_change(uri, strlen(uri), text, text_len);
    free(text);
    (void)fd_out;  /* analysis is deferred; see lsp_flush_dirty */
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
 * LD2: textDocument/hover
 * --------------------------------------------------------------------- */

static void on_hover(const char *id_raw, size_t id_len,
                     const char *params, int fd_out) {
    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) { send_response(fd_out, id_raw, id_len, "null"); return; }

    char uri[1024];
    if (lsp_json_str_copy(td, "uri", uri, sizeof(uri)) < 0) {
        send_response(fd_out, id_raw, id_len, "null");
        return;
    }

    size_t pos_len;
    const char *pos = lsp_json_raw(params, "position", &pos_len);
    if (!pos) { send_response(fd_out, id_raw, id_len, "null"); return; }

    int line_0 = (int)lsp_json_int(pos, "line");
    int char_0 = (int)lsp_json_int(pos, "character");

    LspDoc *doc = lsp_doc_get(uri, strlen(uri));
    if (!doc || !doc->symbols) {
        send_response(fd_out, id_raw, id_len, "null");
        return;
    }

    char name[128];
    if (!lsp_word_at_pos(doc->text, doc->text_len,
                         line_0 + 1, char_0 + 1, name, sizeof(name))) {
        send_response(fd_out, id_raw, id_len, "null");
        return;
    }

    const LspSymbol *sym = find_symbol(doc, name);
    if (!sym) {
        send_response(fd_out, id_raw, id_len, "{\"contents\":\"\"}");
        return;
    }

    Buf result;
    buf_init(&result);
    buf_puts(&result, "{\"contents\":{\"kind\":\"markdown\",\"value\":");

    /* Build markdown: ```\n(name : type)\n```\n\ndocstring */
    Buf md;
    buf_init(&md);
    buf_puts(&md, "```\n(");
    buf_puts(&md, sym->name);
    if (sym->type_str[0]) {
        buf_puts(&md, " : ");
        buf_puts(&md, sym->type_str);
    }
    buf_puts(&md, ")\n```");
    if (sym->doc[0]) {
        buf_puts(&md, "\n\n");
        buf_puts(&md, sym->doc);
    }
    buf_putc(&md, '\0');

    json_str(&result, md.data);
    buf_free(&md);
    buf_puts(&result, "}}");
    buf_putc(&result, '\0');

    send_response(fd_out, id_raw, id_len, result.data);
    buf_free(&result);
}

/* -------------------------------------------------------------------------
 * LD3: textDocument/definition
 * --------------------------------------------------------------------- */

static void on_definition(const char *id_raw, size_t id_len,
                          const char *params, int fd_out) {
    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) { send_response(fd_out, id_raw, id_len, "null"); return; }

    char uri[1024];
    if (lsp_json_str_copy(td, "uri", uri, sizeof(uri)) < 0) {
        send_response(fd_out, id_raw, id_len, "null");
        return;
    }

    size_t pos_len;
    const char *pos = lsp_json_raw(params, "position", &pos_len);
    if (!pos) { send_response(fd_out, id_raw, id_len, "null"); return; }

    int line_0 = (int)lsp_json_int(pos, "line");
    int char_0 = (int)lsp_json_int(pos, "character");

    LspDoc *doc = lsp_doc_get(uri, strlen(uri));
    if (!doc || !doc->symbols) {
        send_response(fd_out, id_raw, id_len, "null");
        return;
    }

    char name[128];
    if (!lsp_word_at_pos(doc->text, doc->text_len,
                         line_0 + 1, char_0 + 1, name, sizeof(name))) {
        send_response(fd_out, id_raw, id_len, "null");
        return;
    }

    const LspSymbol *sym = find_symbol(doc, name);
    if (!sym || sym->file_path[0] == '\0') {
        send_response(fd_out, id_raw, id_len, "null");
        return;
    }

    /* Build "file://" URI from sym->file_path */
    char def_uri[1024];
    lsp_path_to_uri(sym->file_path, def_uri, sizeof(def_uri));

    /* Convert 1-based spans to 0-based LSP positions */
    int def_line = sym->line > 0 ? sym->line - 1 : 0;
    int def_col  = sym->col_start > 0 ? sym->col_start - 1 : 0;
    int def_end  = sym->col_end > 0 ? sym->col_end - 1 : def_col;

    Buf result;
    buf_init(&result);
    buf_printf(&result,
        "{\"uri\":\"%s\","
        "\"range\":{"
          "\"start\":{\"line\":%d,\"character\":%d},"
          "\"end\":{\"line\":%d,\"character\":%d}"
        "}}",
        def_uri, def_line, def_col, def_line, def_end);

    send_response(fd_out, id_raw, id_len, result.data);
    buf_free(&result);
}

/* Infer LSP SymbolKind from type_str.  Functions get kind 12, everything
 * else gets kind 13 (Variable).  A ;;; `defstruct` / `defmacro` distinction
 * is not preserved in LspSymbol today, so this is a best-effort mapping. */
static int lsp_symbol_kind(const LspSymbol *sym) {
    return (strncmp(sym->type_str, "(fn", 3) == 0) ? 12 : 13;
}

/* -------------------------------------------------------------------------
 * LD4a: textDocument/documentSymbol
 * --------------------------------------------------------------------- */

static void on_document_symbol(const char *id_raw, size_t id_len,
                                const char *params, int fd_out) {
    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) { send_response(fd_out, id_raw, id_len, "[]"); return; }

    char uri[1024];
    if (lsp_json_str_copy(td, "uri", uri, sizeof(uri)) < 0) {
        send_response(fd_out, id_raw, id_len, "[]");
        return;
    }

    LspDoc *doc = lsp_doc_get(uri, strlen(uri));
    if (!doc || !doc->symbols) {
        send_response(fd_out, id_raw, id_len, "[]");
        return;
    }

    Buf result;
    buf_init(&result);
    buf_putc(&result, '[');
    int emitted = 0;

    for (int i = 0; i < doc->symbol_count; i++) {
        const LspSymbol *sym = &doc->symbols[i];
        if (!sym->name[0]) continue;
        /* Only emit symbols defined in this file */
        if (sym->file_path[0] && strcmp(sym->file_path, doc->path) != 0)
            continue;

        int kind  = lsp_symbol_kind(sym);
        int line  = sym->line > 0 ? sym->line - 1 : 0;
        int col_s = sym->col_start > 0 ? sym->col_start - 1 : 0;
        int col_e = sym->col_end   > 0 ? sym->col_end   - 1 : col_s;

        if (emitted > 0) buf_putc(&result, ',');
        buf_puts(&result, "{\"name\":");
        json_str(&result, sym->name);
        buf_printf(&result, ",\"kind\":%d", kind);
        buf_printf(&result,
            ",\"range\":{"
              "\"start\":{\"line\":%d,\"character\":%d},"
              "\"end\":{\"line\":%d,\"character\":%d}"
            "},\"selectionRange\":{"
              "\"start\":{\"line\":%d,\"character\":%d},"
              "\"end\":{\"line\":%d,\"character\":%d}"
            "}}",
            line, col_s, line, col_e,
            line, col_s, line, col_e);
        emitted++;
    }

    buf_putc(&result, ']');
    buf_putc(&result, '\0');
    send_response(fd_out, id_raw, id_len, result.data);
    buf_free(&result);
}

/* -------------------------------------------------------------------------
 * LD4b: workspace/symbol
 * --------------------------------------------------------------------- */

typedef struct {
    const char *query;
    size_t       query_len;
    Buf         *result;
    int          count;
} WsCtx;

/* Case-insensitive substring search.  Returns 1 if haystack contains needle. */
static int ci_contains(const char *haystack, const char *needle, size_t nlen) {
    if (nlen == 0) return 1;
    for (const char *p = haystack; *p; p++) {
        int match = 1;
        for (size_t j = 0; j < nlen; j++) {
            char a = p[j], b = needle[j];
            if (!a) { match = 0; break; }
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

static void ws_collect_cb(const LspDoc *doc, void *ctx_) {
    WsCtx *ctx = ctx_;
    for (int i = 0; i < doc->symbol_count; i++) {
        const LspSymbol *sym = &doc->symbols[i];
        if (!sym->name[0]) continue;
        /* Only symbols defined in this document */
        if (sym->file_path[0] && strcmp(sym->file_path, doc->path) != 0)
            continue;
        /* Query filter */
        if (ctx->query_len > 0 &&
            !ci_contains(sym->name, ctx->query, ctx->query_len))
            continue;

        char uri[1024];
        lsp_path_to_uri(sym->file_path[0] ? sym->file_path : doc->path,
                        uri, sizeof(uri));
        int kind  = lsp_symbol_kind(sym);
        int line  = sym->line > 0 ? sym->line - 1 : 0;
        int col_s = sym->col_start > 0 ? sym->col_start - 1 : 0;
        int col_e = sym->col_end   > 0 ? sym->col_end   - 1 : col_s;

        if (ctx->count > 0) buf_putc(ctx->result, ',');
        buf_puts(ctx->result, "{\"name\":");
        json_str(ctx->result, sym->name);
        buf_printf(ctx->result, ",\"kind\":%d,\"location\":{\"uri\":", kind);
        json_str(ctx->result, uri);
        buf_printf(ctx->result,
            ",\"range\":{"
              "\"start\":{\"line\":%d,\"character\":%d},"
              "\"end\":{\"line\":%d,\"character\":%d}"
            "}}}",
            line, col_s, line, col_e);
        ctx->count++;
    }
}

static void on_workspace_symbol(const char *id_raw, size_t id_len,
                                 const char *params, int fd_out) {
    size_t  query_len = 0;
    const char *query_raw = lsp_json_str(params, "query", &query_len);

    Buf result;
    buf_init(&result);
    buf_putc(&result, '[');

    WsCtx ctx;
    ctx.query     = query_raw;
    ctx.query_len = query_raw ? query_len : 0;
    ctx.result    = &result;
    ctx.count     = 0;
    lsp_docs_iterate(ws_collect_cb, &ctx);

    buf_putc(&result, ']');
    buf_putc(&result, '\0');
    send_response(fd_out, id_raw, id_len, result.data);
    buf_free(&result);
}

/* -------------------------------------------------------------------------
 * LD4: textDocument/completion
 * --------------------------------------------------------------------- */

/* Known stdlib module names for import-path completion */
static const char *stdlib_modules[] = {
    "stdlib/args",
    "stdlib/contract",
    "stdlib/fix",
    "stdlib/free",
    "stdlib/hamt",
    "stdlib/list",
    "stdlib/macros",
    "stdlib/map",
    "stdlib/option",
    "stdlib/pair",
    "stdlib/result",
    "stdlib/safe",
    "stdlib/str",
    "stdlib/vec",
    NULL
};

static void on_completion(const char *id_raw, size_t id_len,
                          const char *params, int fd_out) {
    size_t td_len;
    const char *td = lsp_json_raw(params, "textDocument", &td_len);
    if (!td) { send_response(fd_out, id_raw, id_len, "[]"); return; }

    char uri[1024];
    if (lsp_json_str_copy(td, "uri", uri, sizeof(uri)) < 0) {
        send_response(fd_out, id_raw, id_len, "[]");
        return;
    }

    size_t pos_len;
    const char *pos = lsp_json_raw(params, "position", &pos_len);
    if (!pos) { send_response(fd_out, id_raw, id_len, "[]"); return; }

    int line_0 = (int)lsp_json_int(pos, "line");
    int char_0 = (int)lsp_json_int(pos, "character");

    LspDoc *doc = lsp_doc_get(uri, strlen(uri));
    if (!doc) { send_response(fd_out, id_raw, id_len, "[]"); return; }

    /* Extract partial word at cursor */
    char prefix[128] = {0};
    lsp_word_at_pos(doc->text, doc->text_len,
                    line_0 + 1, char_0 + 1, prefix, sizeof(prefix));
    size_t prefix_len = strlen(prefix);

    /* Check if cursor is inside an (import ...) form for module completion */
    int in_import = 0;
    if (doc->text && doc->text_len > 0) {
        /* Walk backward from cursor to detect (import */
        const char *p = doc->text;
        const char *end = doc->text;
        /* Find cursor byte offset */
        int cur_line = 1;
        while (p < doc->text + doc->text_len && cur_line < line_0 + 1) {
            if (*p == '\n') cur_line++;
            p++;
        }
        int cur_col = 1;
        while (p < doc->text + doc->text_len && *p != '\n' && cur_col < char_0 + 1) {
            p++;
            cur_col++;
        }
        end = p;
        /* Scan back up to 64 chars for "(import" */
        const char *scan = end > doc->text + 64 ? end - 64 : doc->text;
        for (; scan < end - 7; scan++) {
            if (strncmp(scan, "(import", 7) == 0) {
                in_import = 1;
                break;
            }
        }
    }

    Buf result;
    buf_init(&result);
    buf_puts(&result, "[");

    int emitted = 0;

    /* Import path completions */
    if (in_import) {
        for (int i = 0; stdlib_modules[i]; i++) {
            const char *mod = stdlib_modules[i];
            if (prefix_len > 0) {
                /* Case-insensitive prefix match */
                int match = 1;
                for (size_t j = 0; j < prefix_len; j++) {
                    char a = prefix[j], b = mod[j];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) { match = 0; break; }
                }
                if (!match) continue;
            }
            if (emitted > 0) buf_putc(&result, ',');
            buf_puts(&result, "{\"label\":");
            json_str(&result, mod);
            buf_puts(&result, ",\"kind\":9}");
            emitted++;
        }
    }

    /* Symbol completions (cap at 200) */
    if (!in_import && doc->symbols) {
        for (int i = 0; i < doc->symbol_count && emitted < 200; i++) {
            const LspSymbol *sym = &doc->symbols[i];
            if (!sym->name[0]) continue;

            /* Prefix filter */
            if (prefix_len > 0) {
                int match = 1;
                for (size_t j = 0; j < prefix_len; j++) {
                    char a = prefix[j], b = sym->name[j];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) { match = 0; break; }
                }
                if (!match) continue;
            }

            /* kind: 3 = Function (type starts with "(fn"), 6 = Variable */
            int kind = (strncmp(sym->type_str, "(fn", 3) == 0) ? 3 : 6;

            if (emitted > 0) buf_putc(&result, ',');
            buf_puts(&result, "{\"label\":");
            json_str(&result, sym->name);
            buf_printf(&result, ",\"kind\":%d", kind);
            if (sym->type_str[0]) {
                buf_puts(&result, ",\"detail\":");
                json_str(&result, sym->type_str);
            }
            if (sym->doc[0]) {
                buf_puts(&result,
                    ",\"documentation\":{\"kind\":\"markdown\",\"value\":");
                json_str(&result, sym->doc);
                buf_puts(&result, "}");
            }
            buf_putc(&result, '}');
            emitted++;
        }
    }

    buf_puts(&result, "]");
    buf_putc(&result, '\0'); /* NUL-terminate for strlen in send_response */
    send_response(fd_out, id_raw, id_len, result.data);
    buf_free(&result);
}

/* -------------------------------------------------------------------------
 * Main loop
 * --------------------------------------------------------------------- */

void lsp_server_run(int fd_in, int fd_out) {
    lsp_docs_init();

    for (;;) {
        /* Nothing new arrived while a document was waiting to be analyzed —
         * the client has stopped typing, so do the work now. */
        if (lsp_docs_any_dirty() &&
            !input_pending(fd_in, LSP_ANALYSIS_DEBOUNCE_MS)) {
            lsp_flush_dirty(fd_out);
            continue;
        }

        char *msg = lsp_read_message(fd_in);
        if (!msg) break;

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
        } else if (strcmp(method, "textDocument/hover") == 0 ||
                   strcmp(method, "textDocument/definition") == 0 ||
                   strcmp(method, "textDocument/documentSymbol") == 0 ||
                   strcmp(method, "workspace/symbol") == 0 ||
                   strcmp(method, "textDocument/completion") == 0) {
            /* These all read doc->symbols, so any pending edit has to be
             * analyzed first — otherwise the answer describes the buffer as it
             * was before the user's last keystroke. This is what keeps the
             * debounce invisible to correctness: it delays work, never skips
             * it. */
            lsp_flush_dirty(fd_out);
            if (!params_raw) {
                send_error(fd_out, id_raw, id_len, -32602, "Invalid params");
            } else if (strcmp(method, "textDocument/hover") == 0) {
                on_hover(id_raw, id_len, params_raw, fd_out);
            } else if (strcmp(method, "textDocument/definition") == 0) {
                on_definition(id_raw, id_len, params_raw, fd_out);
            } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
                on_document_symbol(id_raw, id_len, params_raw, fd_out);
            } else if (strcmp(method, "workspace/symbol") == 0) {
                on_workspace_symbol(id_raw, id_len, params_raw, fd_out);
            } else {
                on_completion(id_raw, id_len, params_raw, fd_out);
            }
        } else if (id_raw) {
            send_error(fd_out, id_raw, id_len, -32601, "Method not found");
        }

        free(msg);
    }

    lsp_docs_free();
}
