/**
 * io.c - Platform I/O abstraction layer (SCH-P1)
 * 
 * This file provides the common interface and platform selection.
 * Platform-specific implementations are in io_epoll.c, io_kqueue.c, io_iocp.c.
 */

#include "io.h"
#include <stdlib.h>
#include <string.h>

/* IOBackend and IOBackendVTable are defined in io.h */

/* Platform-specific constructors */
#if defined(IO_BACKEND_EPOLL)
IOBackend *io_epoll_new(void);
#elif defined(IO_BACKEND_KQUEUE)
IOBackend *io_kqueue_new(void);
#elif defined(IO_BACKEND_IOCP)
IOBackend *io_iocp_new(void);
#elif !defined(__EMSCRIPTEN__)
#error "No I/O backend available for this platform"
#endif

IOBackend *io_backend_new(void) {
#if defined(IO_BACKEND_EPOLL)
    return io_epoll_new();
#elif defined(IO_BACKEND_KQUEUE)
    return io_kqueue_new();
#elif defined(IO_BACKEND_IOCP)
    return io_iocp_new();
#else
    return NULL; /* WASM: no I/O backend */
#endif
}

void io_backend_free(IOBackend *backend) {
    if (backend && backend->vtable && backend->vtable->free) {
        backend->vtable->free(backend);
    }
}

int io_register(IOBackend *backend, int fd, int events, io_callback_t callback, void *user_data) {
    if (!backend || !backend->vtable || !backend->vtable->register_fd) {
        return -1;
    }
    return backend->vtable->register_fd(backend, fd, events, callback, user_data);
}

int io_modify(IOBackend *backend, int fd, int events) {
    if (!backend || !backend->vtable || !backend->vtable->modify_fd) {
        return -1;
    }
    return backend->vtable->modify_fd(backend, fd, events);
}

int io_unregister(IOBackend *backend, int fd) {
    if (!backend || !backend->vtable || !backend->vtable->unregister_fd) {
        return -1;
    }
    return backend->vtable->unregister_fd(backend, fd);
}

int io_poll(IOBackend *backend, int timeout_ms) {
    if (!backend || !backend->vtable || !backend->vtable->poll) {
        return -1;
    }
    return backend->vtable->poll(backend, timeout_ms);
}

void io_wake(IOBackend *backend) {
    if (backend && backend->vtable && backend->vtable->wake) {
        backend->vtable->wake(backend);
    }
}
