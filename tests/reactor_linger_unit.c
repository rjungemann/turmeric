/* reactor_linger_unit -- tur_reactor_linger_close closes a socket gracefully.
 *
 * The case it exists for: a server that answers BEFORE reading the request
 * (httpd's over-cap 503, sent at accept time) and then close()s.  Closing a
 * socket that still holds unread data sends RST, not FIN, and on Windows a
 * peer that has not yet read the reply DISCARDS it when the reset lands --
 * recv() fails with WSAECONNRESET and the reply never arrives.  That is what
 * hung httpd-async-limit on two-core Windows runners
 * (docs/archive/windows-httpd-async-limit-hangs-on-ci.md).
 *
 * Checked here:
 *
 *   1. The peer reads the whole reply and then EOF, even though the request
 *      was never read and the peer only calls recv() after the close.  The
 *      delay before the read is the point: with a plain close() the reset has
 *      landed by then.
 *   2. The linger ends as soon as the peer closes: tur_reactor_run returns
 *      (no active sources left) long before the timeout.
 *   3. A peer that never closes is cut off by the timeout.
 *   4. A socket still lingering at tur_reactor_free is closed there -- the
 *      peer sees EOF, and nothing leaks (LeakSanitizer on the Debug build).
 */
#ifndef _WIN32
/* Built -std=c11 off Windows, which hides clock_gettime, nanosleep and
 * CLOCK_MONOTONIC unless POSIX is asked for. */
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct TurReactor TurReactor;
TurReactor *tur_reactor_new(void);
void        tur_reactor_free(void *r);
void        tur_reactor_run(void *r);
int64_t     tur_reactor_linger_close(void *r, int64_t fd, int64_t timeout_ms);

static int g_failures;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL: %s\n", (msg));                              \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* A connected loopback pair: *client is the connecting end, *server the
 * accepted one.  Both blocking; the client gets a 5s receive timeout so a
 * regression fails the check instead of hanging the test. */
static int make_pair(sock_t *client, sock_t *server) {
    sock_t lst = socket(AF_INET, SOCK_STREAM, 0);
    if (lst == SOCK_BAD) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;
    socklen_t al = sizeof(a);
    if (bind(lst, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(lst, 1) != 0 ||
        getsockname(lst, (struct sockaddr *)&a, &al) != 0) {
        sock_close(lst);
        return -1;
    }
    *client = socket(AF_INET, SOCK_STREAM, 0);
    if (*client == SOCK_BAD ||
        connect(*client, (struct sockaddr *)&a, sizeof(a)) != 0) {
        sock_close(lst);
        return -1;
    }
    *server = accept(lst, NULL, NULL);
    sock_close(lst);
    if (*server == SOCK_BAD) return -1;
#ifdef _WIN32
    DWORD to = 5000;
    setsockopt(*client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
#else
    struct timeval to = { 5, 0 };
    setsockopt(*client, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
#endif
    return 0;
}

/* Read until EOF.  Returns the byte count, or -1 if the read ended in an
 * error (a reset, or the 5s timeout) rather than EOF. */
static int read_to_eof(sock_t s, char *buf, int cap) {
    int total = 0;
    for (;;) {
        int n = (int)recv(s, buf + total, cap - 1 - total, 0);
        if (n == 0) break;
        if (n < 0) return -1;
        total += n;
        if (total >= cap - 1) break;
    }
    buf[total] = '\0';
    return total;
}

/* The server replies without reading the request, then hands the socket to
 * the reactor.  Returns what tur_reactor_linger_close returned. */
static int64_t reply_unread_then_linger(void *r, sock_t client, sock_t server,
                                        int64_t timeout_ms) {
    const char *req = "GET / HTTP/1.0\r\n\r\n";
    send(client, req, (int)strlen(req), 0);
    sleep_ms(50);                      /* the request is sitting unread */
    const char *reply = "HTTP/1.0 503 Service Unavailable\r\n\r\n";
    send(server, reply, (int)strlen(reply), 0);
    return tur_reactor_linger_close(r, (int64_t)server, timeout_ms);
}

/* 1 + 2: the reply survives, and the peer's close ends the linger. */
static void test_peer_close_ends_linger(void) {
    void *r = tur_reactor_new();
    sock_t c, s;
    if (make_pair(&c, &s) != 0) { CHECK(0, "socket pair (1)"); tur_reactor_free(r); return; }
    CHECK(reply_unread_then_linger(r, c, s, 5000) == 1,
          "the socket lingers while the peer still has it open");
    sleep_ms(100);                     /* a reset, if any, has landed by now */
    char buf[256];
    int n = read_to_eof(c, buf, (int)sizeof(buf));
    CHECK(n > 0 && strncmp(buf, "HTTP/1.0 503", 12) == 0,
          "the peer reads the reply and then EOF, not a reset");
    sock_close(c);
    int64_t t0 = now_ms();
    tur_reactor_run(r);                /* returns once the linger is gone */
    CHECK(now_ms() - t0 < 2500, "the peer's close ends the linger early");
    tur_reactor_free(r);
}

/* 3: a peer that never closes is cut off by the timeout. */
static void test_timeout_ends_linger(void) {
    void *r = tur_reactor_new();
    sock_t c, s;
    if (make_pair(&c, &s) != 0) { CHECK(0, "socket pair (3)"); tur_reactor_free(r); return; }
    CHECK(reply_unread_then_linger(r, c, s, 200) == 1, "lingering (3)");
    int64_t t0 = now_ms();
    tur_reactor_run(r);
    int64_t took = now_ms() - t0;
    CHECK(took >= 150 && took < 2500, "the timeout ends the linger");
    char buf[256];
    int n = read_to_eof(c, buf, (int)sizeof(buf));
    CHECK(n > 0 && strncmp(buf, "HTTP/1.0 503", 12) == 0,
          "after a timed-out linger the peer still reads the reply and EOF");
    sock_close(c);
    tur_reactor_free(r);
}

/* 4: teardown closes a socket that is still lingering. */
static void test_free_closes_lingering(void) {
    void *r = tur_reactor_new();
    sock_t c, s;
    if (make_pair(&c, &s) != 0) { CHECK(0, "socket pair (4)"); tur_reactor_free(r); return; }
    CHECK(reply_unread_then_linger(r, c, s, 60000) == 1, "lingering (4)");
    tur_reactor_free(r);
    char buf[256];
    int n = read_to_eof(c, buf, (int)sizeof(buf));
    CHECK(n > 0 && strncmp(buf, "HTTP/1.0 503", 12) == 0,
          "tur_reactor_free closes a lingering socket");
    sock_close(c);
}

int main(void) {
    test_peer_close_ends_linger();
    test_timeout_ends_linger();
    test_free_closes_lingering();
    if (g_failures) {
        fprintf(stderr, "reactor_linger_unit: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("reactor_linger_unit: OK\n");
    return 0;
}
