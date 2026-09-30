#include "lsp_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>

/* Put the protocol fd in BINARY mode.
 *
 * Windows opens fd 0 and 1 in TEXT mode, which strips the CR from every CRLF
 * on the way in.  This transport frames on a literal CRLF CRLF (read_headers
 * below compares those four bytes), so a client that sends a correct
 * `Content-Length: N\r\n\r\n` header has it silently rewritten to `\n\n`
 * before the compare ever sees it.  The terminator then never matches,
 * read_headers loops to EOF, and the server exits 0 having printed nothing --
 * which is what `tur lsp` and `tur dap` did on Windows for every client.
 * Silent, and indistinguishable from a server that simply has nothing to say.
 *
 * The body read has the same problem one layer down: it asks for exactly
 * body_len bytes, and text mode delivers fewer whenever the JSON contains a
 * CRLF, so the read loop blocks waiting for bytes that were already consumed.
 *
 * main() already does this for stdout and stderr, with a comment about this
 * exact desynchronisation -- stdin was missed.  Doing it here rather than
 * there keeps it to the transport that needs it: `tur format` and the REPL
 * also read stdin, and they want the platform text conventions.
 *
 * Idempotent, and cheap enough to call per message rather than asking every
 * server entry point to remember.  A missed call is invisible until an editor
 * reports nothing at all.
 */
static void lsp_io_binary(int fd) {
    if (fd >= 0) _setmode(fd, _O_BINARY);
}
#else
static void lsp_io_binary(int fd) { (void)fd; }
#endif

/* Read until the 4-byte CRLF CRLF header terminator.
 * Returns heap-allocated header string (NUL-terminated), or NULL on EOF or
 * once the block passes LSP_MAX_HEADER_BYTES -- a peer that never sends the
 * terminator must not be able to grow this buffer without bound. */
static char *read_headers(int fd) {
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    while (1) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) { free(buf); return NULL; }

        if (len + 2 >= cap) {
            if (cap >= LSP_MAX_HEADER_BYTES) { free(buf); return NULL; }
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[len++] = c;
        buf[len] = '\0';

        /* Check for \r\n\r\n */
        if (len >= 4 &&
            buf[len-4] == '\r' && buf[len-3] == '\n' &&
            buf[len-2] == '\r' && buf[len-1] == '\n')
            return buf;
    }
}

/* Case-insensitive compare of the first n bytes (header names are
 * case-insensitive, as in HTTP). */
static int ascii_ncaseeq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

bool lsp_parse_content_length(const char *headers, size_t *out_len) {
    static const char name[] = "content-length:";
    const size_t name_len = sizeof name - 1;
    bool found = false;
    size_t value = 0;

    /* Walk the block line by line, matching the header name only at the start
     * of a line -- a strstr over the whole block also matched the name inside
     * another header's value. */
    for (const char *line = headers; *line; ) {
        const char *eol = strstr(line, "\r\n");
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);
        if (line_len >= name_len && ascii_ncaseeq(line, name, name_len)) {
            const char *p = line + name_len, *end = line + line_len;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            if (p == end || *p < '0' || *p > '9') return false;
            size_t v = 0;
            for (; p < end && *p >= '0' && *p <= '9'; p++) {
                /* Checked on every digit, so v is at most the cap going into
                 * each multiply and no digit string can wrap it.  The "-1"
                 * that used to wrap body_len + 1 to zero (malloc(0), then a
                 * read of SIZE_MAX bytes into it) is rejected at the '-'. */
                v = v * 10 + (size_t)(*p - '0');
                if (v > LSP_MAX_BODY_BYTES) return false;
            }
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            if (p != end) return false;           /* trailing garbage */
            if (found && v != value) return false; /* conflicting duplicates */
            found = true;
            value = v;
        }
        if (!eol) break;
        line = eol + 2;
    }
    if (!found || value == 0) return false;
    *out_len = value;
    return true;
}

char *lsp_read_message(int fd_in) {
    lsp_io_binary(fd_in);
    char *headers = read_headers(fd_in);
    if (!headers) return NULL;

    /* A missing, malformed, zero or oversized Content-Length is a framing
     * error.  Nothing after it can be re-synchronised, so it ends the session
     * exactly as EOF does. */
    size_t body_len = 0;
    bool ok = lsp_parse_content_length(headers, &body_len);
    free(headers);
    if (!ok) return NULL;

    char *body = malloc(body_len + 1);
    if (!body) return NULL;

    size_t total = 0;
    while (total < body_len) {
        ssize_t n = read(fd_in, body + total, body_len - total);
        if (n <= 0) { free(body); return NULL; }
        total += (size_t)n;
    }
    body[body_len] = '\0';
    return body;
}

void lsp_write_message(int fd_out, const char *json, size_t len) {
    lsp_io_binary(fd_out);
    char header[64];
    int hlen = snprintf(header, sizeof(header),
                        "Content-Length: %zu\r\n\r\n", len);
    size_t written = 0;
    while (written < (size_t)hlen) {
        ssize_t n = write(fd_out, header + written, (size_t)hlen - written);
        if (n <= 0) return;
        written += (size_t)n;
    }
    written = 0;
    while (written < len) {
        ssize_t n = write(fd_out, json + written, len - written);
        if (n <= 0) return;
        written += (size_t)n;
    }
}
