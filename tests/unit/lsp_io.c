/* lsp_io.c -- unit tests for the LSP/DAP Content-Length framing.
 *
 * security-audit-plan M-3: `Content-Length: -1` used to reach
 * `(size_t)atol(...)`, wrap `body_len + 1` to zero, malloc(0) and then read
 * SIZE_MAX bytes into it -- a heap overflow from one line an editor (or
 * anything else on the other end of `tur lsp` / `tur dap`) can send.  The
 * header block itself also grew without bound.  Every case here is a frame
 * that must end the session (NULL) rather than allocate on the peer's word,
 * plus the well-formed shapes that must keep working.
 *
 * lsp_read_message is driven over a real pipe so the test covers the read
 * loop, not just the parser. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lsp/lsp_io.h"

static int g_checks = 0, g_fails = 0;

static void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) { g_fails++; printf("  FAIL %s\n", what); }
}

static void check_parse(const char *hdr, bool want_ok, size_t want_len) {
    size_t got = 12345;
    bool ok = lsp_parse_content_length(hdr, &got);
    g_checks++;
    if (ok != want_ok || (ok && got != want_len)) {
        g_fails++;
        printf("  FAIL parse %-40.40s: got ok=%d len=%zu, want ok=%d len=%zu\n",
               hdr, ok, ok ? got : 0, want_ok, want_len);
    }
}

static void test_parse(void) {
    check_parse("Content-Length: 5\r\n\r\n", true, 5);
    check_parse("content-length:5\r\n\r\n", true, 5);
    check_parse("CONTENT-LENGTH:\t17 \r\n\r\n", true, 17);
    check_parse("Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n"
                "Content-Length: 42\r\n\r\n", true, 42);
    check_parse("Content-Length: 7\r\nContent-Length: 7\r\n\r\n", true, 7);

    /* The overflow. */
    check_parse("Content-Length: -1\r\n\r\n", false, 0);
    check_parse("Content-Length: 18446744073709551615\r\n\r\n", false, 0);
    check_parse("Content-Length: 99999999999999999999999999\r\n\r\n", false, 0);
    /* Just over the cap, and exactly at it. */
    {
        char h[64];
        snprintf(h, sizeof h, "Content-Length: %zu\r\n\r\n", LSP_MAX_BODY_BYTES + 1);
        check_parse(h, false, 0);
        snprintf(h, sizeof h, "Content-Length: %zu\r\n\r\n", LSP_MAX_BODY_BYTES);
        check_parse(h, true, LSP_MAX_BODY_BYTES);
    }
    check_parse("Content-Length: 0\r\n\r\n", false, 0);
    check_parse("Content-Length: \r\n\r\n", false, 0);
    check_parse("Content-Length: 12abc\r\n\r\n", false, 0);
    check_parse("Content-Length: +12\r\n\r\n", false, 0);
    check_parse("Content-Length: 7\r\nContent-Length: 8\r\n\r\n", false, 0);
    check_parse("\r\n", false, 0);
    /* The name only counts at the start of a line. */
    check_parse("X-Note: Content-Length: 5\r\n\r\n", false, 0);
}

/* Write `frame` into a pipe, close the write end, and read one message. */
static char *read_frame(const char *frame, size_t n) {
    int fds[2];
    if (pipe(fds) != 0) { perror("pipe"); exit(2); }
    /* Every frame here fits the pipe buffer, so a blocking write is fine. */
    if (n && write(fds[1], frame, n) != (ssize_t)n) { perror("write"); exit(2); }
    close(fds[1]);
    char *msg = lsp_read_message(fds[0]);
    close(fds[0]);
    return msg;
}

static void test_read(void) {
    static const char ok[] = "Content-Length: 2\r\n\r\n{}";
    char *m = read_frame(ok, sizeof ok - 1);
    check(m && strcmp(m, "{}") == 0, "well-formed frame round-trips");
    free(m);

    static const char neg[] = "Content-Length: -1\r\n\r\nAAAAAAAAAAAAAAAA";
    m = read_frame(neg, sizeof neg - 1);
    check(m == NULL, "Content-Length -1 ends the session");
    free(m);

    static const char trunc[] = "Content-Length: 10\r\n\r\n{}";
    m = read_frame(trunc, sizeof trunc - 1);
    check(m == NULL, "short body ends the session");
    free(m);

    /* A header block that never terminates stops at the cap rather than
     * reading all of it: 16 KiB of header bytes, no CRLF CRLF. */
    size_t big = 2 * LSP_MAX_HEADER_BYTES;
    char *hdr = malloc(big);
    memset(hdr, 'x', big);
    m = read_frame(hdr, big);
    check(m == NULL, "unterminated oversized header block is rejected");
    free(m);
    free(hdr);
}

int main(void) {
    test_parse();
    test_read();
    printf("lsp_io: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
