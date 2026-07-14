/**
 * io_iocp.c - Windows I/O backend (stub).
 *
 * The real IOCP backend -- CreateIoCompletionPort / GetQueuedCompletionStatusEx,
 * with PostQueuedCompletionStatus for cross-thread wake-ups -- is WIN3 in
 * docs/upcoming/v1/windows-support-plan.md and is deliberately deferred.
 *
 * This stub exists so libturi links on Windows and so every program that does
 * NOT reach for turmeric-side async (which is the whole turmeric-godot target:
 * scripts living inside Godot's frame callbacks) runs normally.
 *
 * It fails LOUDLY, not silently.  A program that does register an fd gets -1
 * plus a one-time diagnostic on stderr, rather than a reactor that quietly
 * never fires a callback -- which presents as a hang and costs an afternoon to
 * diagnose.
 */

#include "async_io.h"

#include <stdio.h>
#include <stdlib.h>

#include <windows.h>  /* Sleep */

struct IocpBackend {
    struct IOBackend base;  /* must be first -- cast-compatible with IOBackend */
};

/* Emit the "not implemented" diagnostic at most once per process, so a polling
 * caller cannot turn it into an unbounded flood of identical lines. */
static void iocp_warn_once(void) {
    static int warned = 0;
    if (warned) {
        return;
    }
    warned = 1;
    fprintf(stderr,
            "turmeric: async I/O is not implemented on Windows yet -- "
            "src/async/io_iocp.c is a stub (see WIN3 in "
            "docs/upcoming/v1/windows-support-plan.md).\n");
}

static void iocp_free(IOBackend *backend) {
    free(backend);
}

static int iocp_register(IOBackend *backend, int fd, int events,
                         io_callback_t callback, void *user_data) {
    (void)backend; (void)fd; (void)events; (void)callback; (void)user_data;
    iocp_warn_once();
    return -1;
}

static int iocp_modify(IOBackend *backend, int fd, int events) {
    (void)backend; (void)fd; (void)events;
    iocp_warn_once();
    return -1;
}

static int iocp_unregister(IOBackend *backend, int fd) {
    (void)backend; (void)fd;
    return -1;
}

/*
 * Report "no events ready" rather than an error: an empty backend genuinely has
 * nothing to report, and returning -1 would make a reactor loop treat a
 * permanent condition as a transient failure.
 *
 * The sleeps matter.  Nothing can ever be registered here and iocp_wake() is a
 * no-op, so a caller asking to block until something happens would otherwise
 * either spin at 100% CPU (if we returned immediately) or deadlock forever (if
 * we honoured an infinite wait).  Sleeping the requested timeout -- and
 * capping an infinite wait at a short slice -- degrades an idle reactor to a
 * cheap slow poll instead.
 */
static int iocp_poll(IOBackend *backend, int timeout_ms) {
    (void)backend;
    if (timeout_ms > 0) {
        Sleep((DWORD)timeout_ms);
    } else if (timeout_ms < 0) {
        Sleep(50);
    }
    return 0;
}

static void iocp_wake(IOBackend *backend) {
    (void)backend;
}

static const struct IOBackendVTable iocp_vtable = {
    .free          = iocp_free,
    .register_fd   = iocp_register,
    .modify_fd     = iocp_modify,
    .unregister_fd = iocp_unregister,
    .poll          = iocp_poll,
    .wake          = iocp_wake,
};

IOBackend *io_iocp_new(void) {
    struct IocpBackend *backend = calloc(1, sizeof(*backend));
    if (!backend) {
        return NULL;
    }
    backend->base.vtable = &iocp_vtable;
    return &backend->base;
}
