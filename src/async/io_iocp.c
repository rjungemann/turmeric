/**
 * io_iocp.c - Windows I/O backend for the reactor.
 *
 * Despite the name (kept to match io.c's IO_BACKEND_IOCP selector), this is a
 * select()-based backend, NOT IOCP. The reactor only ever registers SOCKET
 * descriptors (httpd, async socket I/O), and Winsock's select() handles those
 * directly -- so select() is the small, correct primitive here. A true IOCP
 * backend (overlapped I/O, completion ports) would be a large rewrite for no
 * benefit at the level the reactor uses this: readiness notification.
 *
 * Cross-thread wake() uses a loopback socket pair rather than a pipe, because
 * select() on Windows only accepts sockets. FD_SETSIZE is raised before
 * <winsock2.h> so a busy server can register more than the default 64 sockets.
 */

#define FD_SETSIZE 1024

#include "async_io.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
    int           fd;
    int           events;
    io_callback_t callback;
    void         *user_data;
} FDRegistration;

struct SelectBackend {
    IOBackend       base;
    FDRegistration *registrations;
    size_t          registrations_cap;
    size_t          registrations_len;
    SOCKET          wake_recv;   /* read end of the loopback wake pair */
    SOCKET          wake_send;   /* write end */
};

static void tur_wsa_ensure(void) {
    static LONG done = 0;
    if (InterlockedExchange(&done, 1) == 0) {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
    }
}

/*
 * Windows has no socketpair(). Emulate one over loopback: bind+listen a
 * throwaway listener, connect to it, accept -- yielding two connected TCP
 * sockets. Used only to interrupt select() from another thread.
 */
static int tur_socketpair(SOCKET *recv_out, SOCKET *send_out) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;  /* ephemeral */

    int addrlen = (int)sizeof(addr);
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listener, 1) != 0 ||
        getsockname(listener, (struct sockaddr *)&addr, &addrlen) != 0) {
        closesocket(listener);
        return -1;
    }

    SOCKET client = socket(AF_INET, SOCK_STREAM, 0);
    if (client == INVALID_SOCKET) { closesocket(listener); return -1; }

    if (connect(client, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(listener);
        closesocket(client);
        return -1;
    }
    SOCKET server = accept(listener, NULL, NULL);
    closesocket(listener);
    if (server == INVALID_SOCKET) { closesocket(client); return -1; }

    *recv_out = server;   /* reactor reads drained wake bytes here */
    *send_out = client;   /* wake() writes here */
    return 0;
}

static FDRegistration *find_registration(struct SelectBackend *b, int fd) {
    for (size_t i = 0; i < b->registrations_len; i++) {
        if (b->registrations[i].fd == fd) return &b->registrations[i];
    }
    return NULL;
}

static FDRegistration *get_or_add_registration(struct SelectBackend *b, int fd) {
    FDRegistration *reg = find_registration(b, fd);
    if (reg) return reg;
    if (b->registrations_len >= b->registrations_cap) {
        size_t cap = b->registrations_cap ? b->registrations_cap * 2 : 8;
        FDRegistration *nr = realloc(b->registrations, cap * sizeof(FDRegistration));
        if (!nr) return NULL;
        b->registrations = nr;
        b->registrations_cap = cap;
    }
    reg = &b->registrations[b->registrations_len++];
    reg->fd = fd; reg->events = 0; reg->callback = NULL; reg->user_data = NULL;
    return reg;
}

static void remove_registration(struct SelectBackend *b, int fd) {
    for (size_t i = 0; i < b->registrations_len; i++) {
        if (b->registrations[i].fd == fd) {
            b->registrations[i] = b->registrations[b->registrations_len - 1];
            b->registrations_len--;
            return;
        }
    }
}

static void select_free(IOBackend *backend) {
    struct SelectBackend *b = (struct SelectBackend *)backend;
    if (b->wake_recv != INVALID_SOCKET) closesocket(b->wake_recv);
    if (b->wake_send != INVALID_SOCKET) closesocket(b->wake_send);
    free(b->registrations);
    free(b);
}

static int select_register(IOBackend *backend, int fd, int events,
                           io_callback_t callback, void *user_data) {
    struct SelectBackend *b = (struct SelectBackend *)backend;
    FDRegistration *reg = get_or_add_registration(b, fd);
    if (!reg) return -1;
    reg->events = events;
    reg->callback = callback;
    reg->user_data = user_data;
    return 0;
}

static int select_modify(IOBackend *backend, int fd, int events) {
    struct SelectBackend *b = (struct SelectBackend *)backend;
    FDRegistration *reg = find_registration(b, fd);
    if (!reg) return -1;
    reg->events = events;
    return 0;
}

static int select_unregister(IOBackend *backend, int fd) {
    remove_registration((struct SelectBackend *)backend, fd);
    return 0;
}

static int select_poll(IOBackend *backend, int timeout_ms) {
    struct SelectBackend *b = (struct SelectBackend *)backend;

    fd_set rfds, wfds, efds;
    FD_ZERO(&rfds); FD_ZERO(&wfds); FD_ZERO(&efds);

    /* Always watch the wake socket for readability. */
    FD_SET(b->wake_recv, &rfds);

    for (size_t i = 0; i < b->registrations_len; i++) {
        FDRegistration *reg = &b->registrations[i];
        SOCKET s = (SOCKET)reg->fd;
        if (reg->events & IO_EVENT_READ)  FD_SET(s, &rfds);
        if (reg->events & IO_EVENT_WRITE) FD_SET(s, &wfds);
        FD_SET(s, &efds);  /* always surface errors */
    }

    struct timeval tv;
    struct timeval *tvp = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }

    /* nfds is ignored on Windows. */
    int n = select(0, &rfds, &wfds, &efds, tvp);
    if (n == SOCKET_ERROR) return -1;
    if (n == 0) return 0;

    /* Drain the wake socket if it fired. */
    if (FD_ISSET(b->wake_recv, &rfds)) {
        char buf[64];
        while (recv(b->wake_recv, buf, sizeof(buf), 0) > 0) { /* drain */ }
    }

    /*
     * Snapshot the ready set before dispatching. A callback may register or
     * unregister fds, and remove_registration swaps the last element into the
     * freed slot -- so iterating the live array while it mutates would skip or
     * double-fire entries. Collect the ready (fd, events, cb, user) tuples
     * first, then invoke. This mirrors io_epoll.c, which dispatches from its
     * own event array rather than the registration list.
     */
    struct { int fd; int events; io_callback_t cb; void *ud; } ready[FD_SETSIZE];
    int nready = 0;
    for (size_t i = 0; i < b->registrations_len && nready < FD_SETSIZE; i++) {
        FDRegistration *reg = &b->registrations[i];
        SOCKET s = (SOCKET)reg->fd;
        int io_events = 0;
        if (FD_ISSET(s, &rfds)) io_events |= IO_EVENT_READ;
        if (FD_ISSET(s, &wfds)) io_events |= IO_EVENT_WRITE;
        if (FD_ISSET(s, &efds)) io_events |= IO_EVENT_ERROR;
        if (io_events && reg->callback) {
            ready[nready].fd = reg->fd;
            ready[nready].events = io_events;
            ready[nready].cb = reg->callback;
            ready[nready].ud = reg->user_data;
            nready++;
        }
    }

    for (int i = 0; i < nready; i++) {
        ready[i].cb(ready[i].fd, ready[i].events, ready[i].ud);
    }

    return nready;
}

static void select_wake(IOBackend *backend) {
    struct SelectBackend *b = (struct SelectBackend *)backend;
    if (b->wake_send != INVALID_SOCKET) {
        char byte = 1;
        send(b->wake_send, &byte, 1, 0);
    }
}

static const struct IOBackendVTable select_vtable = {
    select_free,
    select_register,
    select_modify,
    select_unregister,
    select_poll,
    select_wake,
};

IOBackend *io_iocp_new(void) {
    tur_wsa_ensure();

    struct SelectBackend *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->base.vtable = &select_vtable;
    b->wake_recv = INVALID_SOCKET;
    b->wake_send = INVALID_SOCKET;

    if (tur_socketpair(&b->wake_recv, &b->wake_send) != 0) {
        free(b);
        return NULL;
    }
    /* Non-blocking wake ends so drain/wake never stall. */
    u_long nb = 1;
    ioctlsocket(b->wake_recv, FIONBIO, &nb);
    ioctlsocket(b->wake_send, FIONBIO, &nb);

    return &b->base;
}
