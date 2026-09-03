/*
 * httpd.c -- minimal raw-socket HTTP/1.1 listener for the guestbook example.
 *
 * Handles one request at a time (single-threaded by design). This keeps the
 * networking layer fully transparent and avoids external dependencies.
 *
 * For production use, replace with libmicrohttpd or similar.
 *
 * Exported functions:
 *   int  httpd_start(int port)
 *   void httpd_next_request(char **out_method, char **out_path,
 *                           char **out_query, char **out_body)
 *   void httpd_send_response(int status, const char *content_type,
 *                            const char *body)
 */

/* strdup / strncasecmp are POSIX, not ISO C: without this the compiler sees no
 * prototype under -std=c11, assumes `int strdup()`, and truncates the returned
 * pointer -- which crashed the very first request. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

#ifdef _WIN32
#  include <winsock2.h>
#  pragma comment(lib, "ws2_32.lib")
   typedef int socklen_t;
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <unistd.h>
   typedef int SOCKET;
#  define INVALID_SOCKET (-1)
#  define closesocket    close
#endif

/* --------------------------------------------------------------------------
 * Internal state
 * -------------------------------------------------------------------------- */

static SOCKET listen_fd = INVALID_SOCKET;
static SOCKET conn_fd   = INVALID_SOCKET;

/* --------------------------------------------------------------------------
 * httpd_start -- bind and listen on port.
 * Returns 0 on success, -1 on error (errno set).
 * -------------------------------------------------------------------------- */
int httpd_start(int port) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return -1;
#endif

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == INVALID_SOCKET) return -1;

    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((unsigned short)port);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        closesocket(listen_fd);
        listen_fd = INVALID_SOCKET;
        return -1;
    }

    if (listen(listen_fd, 5) < 0) {
        closesocket(listen_fd);
        listen_fd = INVALID_SOCKET;
        return -1;
    }

    fprintf(stderr, "[httpd] listening on http://127.0.0.1:%d\n", port);
    return 0;
}

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/* Read a line from conn_fd into buf (max len bytes), stripping \r\n. */
static int read_line(char *buf, int len) {
    int i = 0;
    while (i < len - 1) {
        char c;
        int n = (int)recv(conn_fd, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

/* Read exactly n bytes from conn_fd into buf. */
static int read_exact(char *buf, int n) {
    int total = 0;
    while (total < n) {
        int got = (int)recv(conn_fd, buf + total, n - total, 0);
        if (got <= 0) return -1;
        total += got;
    }
    buf[total] = '\0';
    return total;
}

/* Duplicate a string region [start, end). */
static char *duprange(const char *s, int start, int end) {
    int len = end - start;
    char *out = (char*)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s + start, len);
    out[len] = '\0';
    return out;
}

/* --------------------------------------------------------------------------
 * httpd_next_request -- accept one connection and parse the HTTP request.
 *
 * Writes into the four out-params. Caller must free() all four strings.
 * Blocks until a connection arrives.
 * -------------------------------------------------------------------------- */
void httpd_next_request(char **out_method, char **out_path,
                        char **out_query, char **out_body) {
    /* Accept next connection, closing any previous one. */
    if (conn_fd != INVALID_SOCKET) {
        closesocket(conn_fd);
        conn_fd = INVALID_SOCKET;
    }

    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    conn_fd = accept(listen_fd, (struct sockaddr*)&peer, &peer_len);
    if (conn_fd == INVALID_SOCKET) {
        *out_method = strdup("GET");
        *out_path   = strdup("/");
        *out_query  = strdup("");
        *out_body   = strdup("");
        return;
    }

    /* Parse request line: METHOD SP request-target SP HTTP/version CRLF */
    char line[4096];
    if (read_line(line, sizeof(line)) < 0) goto fail;

    /* Extract method */
    char *sp1 = strchr(line, ' ');
    if (!sp1) goto fail;
    *out_method = duprange(line, 0, (int)(sp1 - line));

    /* Extract path and query */
    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (!sp2) sp2 = target + strlen(target);
    char *qmark = memchr(target, '?', sp2 - target);
    if (qmark) {
        *out_path  = duprange(target, 0, (int)(qmark - target));
        *out_query = duprange(qmark + 1, 0, (int)(sp2 - qmark - 1));
    } else {
        *out_path  = duprange(target, 0, (int)(sp2 - target));
        *out_query = strdup("");
    }

    /* Read headers, looking for Content-Length */
    int content_length = 0;
    while (1) {
        if (read_line(line, sizeof(line)) < 0) break;
        if (line[0] == '\0') break;  /* empty line = end of headers */
        /* Case-insensitive match for Content-Length */
        if (strncasecmp(line, "content-length:", 15) == 0) {
            content_length = atoi(line + 15);
        }
    }

    /* Read body */
    if (content_length > 0) {
        char *body = (char*)malloc(content_length + 1);
        if (!body || read_exact(body, content_length) < 0) {
            free(body);
            *out_body = strdup("");
        } else {
            *out_body = body;
        }
    } else {
        *out_body = strdup("");
    }
    return;

fail:
    *out_method = strdup("GET");
    *out_path   = strdup("/");
    *out_query  = strdup("");
    *out_body   = strdup("");
}

/* --------------------------------------------------------------------------
 * httpd_send_response -- write an HTTP/1.1 response to the open connection.
 * -------------------------------------------------------------------------- */
void httpd_send_response(int status, const char *content_type,
                         const char *body) {
    if (conn_fd == INVALID_SOCKET) return;

    const char *reason = "OK";
    if      (status == 302) reason = "Found";
    else if (status == 400) reason = "Bad Request";
    else if (status == 404) reason = "Not Found";
    else if (status == 500) reason = "Internal Server Error";

    int body_len = body ? (int)strlen(body) : 0;
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, reason,
        content_type ? content_type : "text/plain",
        body_len);

    send(conn_fd, header, hlen, 0);
    if (body && body_len > 0) {
        send(conn_fd, body, body_len, 0);
    }

    closesocket(conn_fd);
    conn_fd = INVALID_SOCKET;
}
