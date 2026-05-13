/**
 * io.h - Platform I/O abstraction layer (SCH-P1)
 * 
 * Provides a cross-platform abstraction for I/O event polling.
 * Supports epoll (Linux), kqueue (BSD/macOS), and IOCP (Windows).
 */

#ifndef TUR_IO_H
#define TUR_IO_H

#include <stdbool.h>
#include <stdint.h>

/* Forward declarations */
typedef struct IOBackend IOBackend;
typedef struct IOBackendVTable IOBackendVTable;

/**
 * IO event types
 */
typedef enum {
    IO_EVENT_READ = 1 << 0,   /* Readable */
    IO_EVENT_WRITE = 1 << 1,  /* Writable */
    IO_EVENT_ERROR = 1 << 2,  /* Error condition */
    IO_EVENT_HUP = 1 << 3,    /* Hang up / EOF */
} IOEventType;

/**
 * Callback function type for I/O events
 * 
 * fd: the file descriptor that triggered the event
 * events: bitmask of IOEventType flags
 * user_data: user-provided context pointer
 */
typedef void (*io_callback_t)(int fd, int events, void *user_data);

/* I/O backend vtable - defined here so platform-specific implementations can use it */
struct IOBackendVTable {
    void (*free)(IOBackend *backend);
    int (*register_fd)(IOBackend *backend, int fd, int events, io_callback_t callback, void *user_data);
    int (*modify_fd)(IOBackend *backend, int fd, int events);
    int (*unregister_fd)(IOBackend *backend, int fd);
    int (*poll)(IOBackend *backend, int timeout_ms);
    void (*wake)(IOBackend *backend);
};

/* Base backend structure - defined here so platform-specific implementations can use it */
struct IOBackend {
    const struct IOBackendVTable *vtable;
};

/**
 * Create a new I/O polling backend for the current platform
 * 
 * Returns: pointer to IOBackend, or NULL on error
 */
IOBackend *io_backend_new(void);

/**
 * Free an I/O backend
 */
void io_backend_free(IOBackend *backend);

/**
 * Register interest in I/O events on a file descriptor
 * 
 * backend: the I/O backend
 * fd: file descriptor to monitor
 * events: bitmask of IOEventType flags to monitor
 * callback: function to call when events occur
 * user_data: user-provided context passed to callback
 * 
 * Returns: 0 on success, -1 on error
 */
int io_register(IOBackend *backend, int fd, int events, io_callback_t callback, void *user_data);

/**
 * Modify registered interest for a file descriptor
 * 
 * backend: the I/O backend
 * fd: file descriptor to modify
 * events: new bitmask of IOEventType flags to monitor
 * 
 * Returns: 0 on success, -1 on error
 */
int io_modify(IOBackend *backend, int fd, int events);

/**
 * Unregister interest in a file descriptor
 * 
 * backend: the I/O backend
 * fd: file descriptor to unregister
 * 
 * Returns: 0 on success, -1 on error
 */
int io_unregister(IOBackend *backend, int fd);

/**
 * Poll for I/O events with a timeout
 * 
 * backend: the I/O backend
 * timeout_ms: maximum time to wait in milliseconds (-1 for no timeout, 0 for non-blocking)
 * 
 * Returns: number of events fired, 0 on timeout, -1 on error
 */
int io_poll(IOBackend *backend, int timeout_ms);

/**
 * Wake the poller from another thread
 * 
 * This is used to interrupt a blocking io_poll() call from another thread.
 * Useful for graceful shutdown or emergency wakeup.
 * 
 * backend: the I/O backend
 */
void io_wake(IOBackend *backend);

/**
 * Platform-specific backend types
 */
#if defined(__linux__)
#define IO_BACKEND_EPOLL 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define IO_BACKEND_KQUEUE 1
#elif defined(_WIN32)
#define IO_BACKEND_IOCP 1
#endif

#endif /* TUR_IO_H */
