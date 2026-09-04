/**
 * io_kqueue.c - kqueue-based I/O backend for BSD/macOS (SCH-P1)
 */

#include "async_io.h"
#include <sys/event.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Maximum number of events per kevent call */
#define MAX_EVENTS 64

/* Per-file-descriptor registration */
typedef struct {
    int fd;
    int events;
    io_callback_t callback;
    void *user_data;
} FDRegistration;

/* Kqueue backend state */
struct KqueueBackend {
    IOBackend base;
    int kq_fd;
    FDRegistration *registrations;
    size_t registrations_cap;
    size_t registrations_len;
    /* For wakeup pipe */
    int wakeup_pipe[2];
};

/* Convert IOEventType to kevent filters */
static int io_events_to_kqueue_filters(int io_events) {
    int filters = 0;
    if (io_events & IO_EVENT_READ) filters |= EVFILT_READ;
    if (io_events & IO_EVENT_WRITE) filters |= EVFILT_WRITE;
    return filters;
}

/* Convert kevent flags to IOEventType */
static int kqueue_flags_to_io(int flags) {
    int io_events = 0;
    if (flags & EV_EOF) io_events |= IO_EVENT_HUP;
    if (flags & EV_ERROR) io_events |= IO_EVENT_ERROR;
    return io_events;
}

/* Get event flags from kevent */
static int kevent_fflags_to_io(int fflags) __attribute__((unused));
static int kevent_fflags_to_io(int fflags) {
    (void)fflags; /* Not used for basic read/write */
    return 0;
}

/* Find registration by fd */
static FDRegistration *find_registration(struct KqueueBackend *backend, int fd) {
    for (size_t i = 0; i < backend->registrations_len; i++) {
        if (backend->registrations[i].fd == fd) {
            return &backend->registrations[i];
        }
    }
    return NULL;
}

/* Add or update a registration */
static FDRegistration *get_or_add_registration(struct KqueueBackend *backend, int fd) {
    FDRegistration *reg = find_registration(backend, fd);
    if (reg) return reg;
    
    if (backend->registrations_len >= backend->registrations_cap) {
        size_t new_cap = backend->registrations_cap > 0 ? backend->registrations_cap * 2 : 8;
        FDRegistration *new_regs = realloc(backend->registrations, new_cap * sizeof(FDRegistration));
        if (!new_regs) return NULL;
        backend->registrations = new_regs;
        backend->registrations_cap = new_cap;
    }
    
    reg = &backend->registrations[backend->registrations_len++];
    reg->fd = fd;
    reg->events = 0;
    reg->callback = NULL;
    reg->user_data = NULL;
    return reg;
}

/* Remove a registration by fd */
static void remove_registration(struct KqueueBackend *backend, int fd) {
    for (size_t i = 0; i < backend->registrations_len; i++) {
        if (backend->registrations[i].fd == fd) {
            backend->registrations[i] = backend->registrations[backend->registrations_len - 1];
            backend->registrations_len--;
            return;
        }
    }
}

/* Wakeup callback for the wakeup pipe */
static void wakeup_callback(int fd, int events, void *user_data) {
    char buf[16];
    (void)read(fd, buf, sizeof(buf));
}

/* kqueue-specific vtable functions */

static void kqueue_free(IOBackend *backend) {
    struct KqueueBackend *kb = (struct KqueueBackend *)backend;
    if (kb->kq_fd >= 0) {
        close(kb->kq_fd);
    }
    if (kb->wakeup_pipe[0] >= 0) {
        close(kb->wakeup_pipe[0]);
    }
    if (kb->wakeup_pipe[1] >= 0) {
        close(kb->wakeup_pipe[1]);
    }
    free(kb->registrations);
    free(kb);
}

static int kqueue_register(IOBackend *backend, int fd, int events, io_callback_t callback, void *user_data) {
    struct KqueueBackend *kb = (struct KqueueBackend *)backend;
    
    FDRegistration *reg = get_or_add_registration(kb, fd);
    if (!reg) return -1;
    
    reg->events = events;
    reg->callback = callback;
    reg->user_data = user_data;
    
    /* Build kevent for registration */
    int filters = io_events_to_kqueue_filters(events);
    if (filters == 0) return 0; /* No events to monitor */
    
    struct kevent kev;
    EV_SET(&kev, fd, filters, EV_ADD | EV_CLEAR, 0, 0, NULL);
    
    if (kevent(kb->kq_fd, &kev, 1, NULL, 0, NULL) == 0) {
        return 0;
    }
    
    return -1;
}

static int kqueue_modify(IOBackend *backend, int fd, int events) {
    struct KqueueBackend *kb = (struct KqueueBackend *)backend;
    
    FDRegistration *reg = find_registration(kb, fd);
    if (!reg) return -1;
    
    reg->events = events;
    
    int filters = io_events_to_kqueue_filters(events);
    if (filters == 0) return 0;
    
    /* First remove old registration. kqueue filters are enum VALUES, not a
     * bitmask: EVFILT_READ | EVFILT_WRITE collapses to EVFILT_READ
     * (-1 | -2 == -1), which left WRITE knotes registered forever. Delete
     * each filter individually. */
    struct kevent remove_kev;
    EV_SET(&remove_kev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    if (kevent(kb->kq_fd, &remove_kev, 1, NULL, 0, NULL) != 0 && errno != ENOENT) {
        return -1;
    }
    EV_SET(&remove_kev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    if (kevent(kb->kq_fd, &remove_kev, 1, NULL, 0, NULL) != 0 && errno != ENOENT) {
        return -1;
    }
    
    /* Add new registration */
    struct kevent add_kev;
    EV_SET(&add_kev, fd, filters, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(kb->kq_fd, &add_kev, 1, NULL, 0, NULL) != 0) {
        return -1;
    }
    
    return 0;
}

static int kqueue_unregister(IOBackend *backend, int fd) {
    struct KqueueBackend *kb = (struct KqueueBackend *)backend;
    
    /* Remove from kqueue -- one EV_DELETE per filter (filters are values,
     * not flags; see kqueue_modify). */
    struct kevent kev;
    EV_SET(&kev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    if (kevent(kb->kq_fd, &kev, 1, NULL, 0, NULL) != 0 && errno != ENOENT) {
        return -1;
    }
    EV_SET(&kev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    if (kevent(kb->kq_fd, &kev, 1, NULL, 0, NULL) != 0 && errno != ENOENT) {
        return -1;
    }
    
    /* Remove from registration array */
    remove_registration(kb, fd);
    
    return 0;
}

static int kqueue_poll(IOBackend *backend, int timeout_ms) {
    struct KqueueBackend *kb = (struct KqueueBackend *)backend;
    
    struct kevent events[MAX_EVENTS];
    struct timespec ts;
    
    if (timeout_ms < 0) {
        /* No timeout */
        ts.tv_sec = -1;
        ts.tv_nsec = 0;
    } else if (timeout_ms == 0) {
        /* Non-blocking */
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    } else {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
    }
    
    int n = kevent(kb->kq_fd, NULL, 0, events, MAX_EVENTS, timeout_ms < 0 ? NULL : &ts);
    
    if (n < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
    
    if (n == 0) return 0; /* Timeout */
    
    int fired = 0;
    for (int i = 0; i < n; i++) {
        int fd = (int)events[i].ident;
        
        /* Check if this is the wakeup pipe */
        if (fd == kb->wakeup_pipe[0]) {
            char buf[16];
            while (read(fd, buf, sizeof(buf)) > 0);
            continue;
        }
        
        FDRegistration *reg = find_registration(kb, fd);
        if (reg && reg->callback) {
            int io_events = 0;
            if (events[i].filter == EVFILT_READ) io_events |= IO_EVENT_READ;
            if (events[i].filter == EVFILT_WRITE) io_events |= IO_EVENT_WRITE;
            io_events |= kqueue_flags_to_io(events[i].flags);
            reg->callback(fd, io_events, reg->user_data);
            fired++;
        }
    }
    
    return fired;
}

static void kqueue_wake(IOBackend *backend) {
    struct KqueueBackend *kb = (struct KqueueBackend *)backend;
    if (kb->wakeup_pipe[1] >= 0) {
        char byte = 1;
        (void)write(kb->wakeup_pipe[1], &byte, 1);
    }
}

/* kqueue vtable */
static const struct IOBackendVTable kqueue_vtable = {
    kqueue_free,
    kqueue_register,
    kqueue_modify,
    kqueue_unregister,
    kqueue_poll,
    kqueue_wake
};

/* Create a new kqueue backend */
IOBackend *io_kqueue_new(void) {
    struct KqueueBackend *kb = calloc(1, sizeof(struct KqueueBackend));
    if (!kb) return NULL;
    
    kb->base.vtable = &kqueue_vtable;
    kb->kq_fd = -1;
    kb->wakeup_pipe[0] = -1;
    kb->wakeup_pipe[1] = -1;
    
    /* Create kqueue */
    kb->kq_fd = kqueue();
    if (kb->kq_fd < 0) {
        free(kb);
        return NULL;
    }
    
    /* Create wakeup pipe */
    if (pipe(kb->wakeup_pipe) != 0) {
        close(kb->kq_fd);
        free(kb);
        return NULL;
    }
    
    /* Set non-blocking */
    int flags = fcntl(kb->wakeup_pipe[0], F_GETFL, 0);
    fcntl(kb->wakeup_pipe[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(kb->wakeup_pipe[1], F_GETFL, 0);
    fcntl(kb->wakeup_pipe[1], F_SETFL, flags | O_NONBLOCK);
    
    /* Register wakeup pipe with kqueue */
    struct kevent kev;
    EV_SET(&kev, kb->wakeup_pipe[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(kb->kq_fd, &kev, 1, NULL, 0, NULL) != 0) {
        close(kb->wakeup_pipe[0]);
        close(kb->wakeup_pipe[1]);
        close(kb->kq_fd);
        free(kb);
        return NULL;
    }
    
    /* Add wakeup pipe to registrations */
    FDRegistration *reg = get_or_add_registration(kb, kb->wakeup_pipe[0]);
    if (reg) {
        reg->callback = wakeup_callback;
        reg->user_data = NULL;
    }
    
    return &kb->base;
}
