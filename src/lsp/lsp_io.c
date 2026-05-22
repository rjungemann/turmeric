#include "lsp_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
