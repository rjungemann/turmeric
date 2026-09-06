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
 * Returns heap-allocated header string (NUL-terminated), or NULL on EOF. */
static char *read_headers(int fd) {
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    while (1) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) { free(buf); return NULL; }

        if (len + 2 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) return NULL;
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

char *lsp_read_message(int fd_in) {
    lsp_io_binary(fd_in);
    char *headers = read_headers(fd_in);
    if (!headers) return NULL;

    /* Parse Content-Length */
    const char *cl = strstr(headers, "Content-Length:");
    if (!cl) { free(headers); return NULL; }
    cl += 15; /* skip "Content-Length:" */
    while (*cl == ' ') cl++;
    size_t body_len = (size_t)atol(cl);
    free(headers);

    if (body_len == 0) return NULL;

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
