/**
 * io_epoll.c - epoll-based I/O backend for Linux (SCH-P1)
 */

#include "io.h"
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Maximum number of events per epoll_wait call */
#define MAX_EVENTS 64

/* Per-file-descriptor registration */
typedef struct {
    int fd;
    int events;
    io_callback_t callback;
    void *user_data;
} FDRegistration;

/* Epoll backend state */
struct EpollBackend {
    IOBackend base;
    int epoll_fd;
    FDRegistration *registrations;
    size_t registrations_cap;
    size_t registrations_len;
    /* For wakeup pipe */
    int wakeup_pipe[2];
};

/* Convert IOEventType to epoll events */
static int io_events_to_epoll(int io_events) {
    int epoll_events = 0;
    if (io_events & IO_EVENT_READ) epoll_events |= EPOLLIN;
    if (io_events & IO_EVENT_WRITE) epoll_events |= EPOLLOUT;
    if (io_events & IO_EVENT_ERROR) epoll_events |= EPOLLERR;
    if (io_events & IO_EVENT_HUP) epoll_events |= EPOLLHUP;
    return epoll_events;
}

/* Convert epoll events to IOEventType */
static int epoll_events_to_io(int epoll_events) {
    int io_events = 0;
    if (epoll_events & EPOLLIN) io_events |= IO_EVENT_READ;
    if (epoll_events & EPOLLOUT) io_events |= IO_EVENT_WRITE;
    if (epoll_events & EPOLLERR) io_events |= IO_EVENT_ERROR;
    if (epoll_events & EPOLLHUP) io_events |= IO_EVENT_HUP;
    return io_events;
}

/* Find registration by fd using linear search (ok for small n) */
static FDRegistration *find_registration(struct EpollBackend *backend, int fd) {
    for (size_t i = 0; i < backend->registrations_len; i++) {
        if (backend->registrations[i].fd == fd) {
            return &backend->registrations[i];
        }
    }
    return NULL;
}

/* Add or update a registration */
static FDRegistration *get_or_add_registration(struct EpollBackend *backend, int fd) {
    FDRegistration *reg = find_registration(backend, fd);
    if (reg) return reg;
    
    /* Grow array if needed */
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
static void remove_registration(struct EpollBackend *backend, int fd) {
    for (size_t i = 0; i < backend->registrations_len; i++) {
        if (backend->registrations[i].fd == fd) {
            /* Move last element into this slot */
            backend->registrations[i] = backend->registrations[backend->registrations_len - 1];
            backend->registrations_len--;
            return;
        }
    }
}

/* Wakeup callback for the wakeup pipe */
static void wakeup_callback(int fd, int events, void *user_data) {
    /* Just read and discard the byte(s) */
    char buf[16];
    if (read(fd, buf, sizeof(buf)) < 0) { /* drain failed -- harmless */ }
}

/* epoll-specific vtable functions */

static void epoll_free(IOBackend *backend) {
    struct EpollBackend *eb = (struct EpollBackend *)backend;
    if (eb->epoll_fd >= 0) {
        close(eb->epoll_fd);
    }
    if (eb->wakeup_pipe[0] >= 0) {
        close(eb->wakeup_pipe[0]);
    }
    if (eb->wakeup_pipe[1] >= 0) {
        close(eb->wakeup_pipe[1]);
    }
    free(eb->registrations);
    free(eb);
}

static int epoll_register(IOBackend *backend, int fd, int events, io_callback_t callback, void *user_data) {
    struct EpollBackend *eb = (struct EpollBackend *)backend;
    
    /* Get or create registration */
    FDRegistration *reg = get_or_add_registration(eb, fd);
    if (!reg) return -1;
    
    reg->events = events;
    reg->callback = callback;
    reg->user_data = user_data;
    
    /* Add to epoll */
    struct epoll_event ev;
    ev.events = io_events_to_epoll(events);
    ev.data.fd = fd;
    
    if (epoll_ctl(eb->epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0) {
        return 0;
    }
    
    /* Try ADD if MOD fails (fd not yet registered) */
    if (errno == ENOENT) {
        if (epoll_ctl(eb->epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0) {
            return 0;
        }
    }
    
    return -1;
}

static int epoll_modify(IOBackend *backend, int fd, int events) {
    struct EpollBackend *eb = (struct EpollBackend *)backend;
    
    FDRegistration *reg = find_registration(eb, fd);
    if (!reg) return -1;
    
    reg->events = events;
    
    struct epoll_event ev;
    ev.events = io_events_to_epoll(events);
    ev.data.fd = fd;
    
    if (epoll_ctl(eb->epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0) {
        return 0;
    }
    
    return -1;
}

static int epoll_unregister(IOBackend *backend, int fd) {
    struct EpollBackend *eb = (struct EpollBackend *)backend;
    
    /* Remove from epoll */
    struct epoll_event ev;
    ev.events = 0;
    ev.data.fd = fd;
    
    if (epoll_ctl(eb->epoll_fd, EPOLL_CTL_DEL, fd, &ev) != 0 && errno != ENOENT) {
        return -1;
    }
    
    /* Remove from registration array */
    remove_registration(eb, fd);
    
    return 0;
}

static int epoll_poll(IOBackend *backend, int timeout_ms) {
    struct EpollBackend *eb = (struct EpollBackend *)backend;
    
    struct epoll_event events[MAX_EVENTS];
    int n = epoll_wait(eb->epoll_fd, events, MAX_EVENTS, timeout_ms);
    
    if (n < 0) {
        if (errno == EINTR) return 0; /* Interrupted, try again */
        return -1;
    }
    
    if (n == 0) return 0; /* Timeout */
    
    int fired = 0;
    for (int i = 0; i < n; i++) {
        int fd = events[i].data.fd;
        
        /* Check if this is the wakeup pipe */
        if (fd == eb->wakeup_pipe[0]) {
            /* Drain the wakeup pipe */
            char buf[16];
            while (read(fd, buf, sizeof(buf)) > 0);
            continue;
        }
        
        FDRegistration *reg = find_registration(eb, fd);
        if (reg && reg->callback) {
            int io_events = epoll_events_to_io(events[i].events);
            reg->callback(fd, io_events, reg->user_data);
            fired++;
        }
    }
    
    return fired;
}

static void epoll_wake(IOBackend *backend) {
    struct EpollBackend *eb = (struct EpollBackend *)backend;
    /* Write a byte to wake up the epoll_wait */
    if (eb->wakeup_pipe[1] >= 0) {
        char byte = 1;
        if (write(eb->wakeup_pipe[1], &byte, 1) < 0) { /* wake failed -- harmless */ }
    }
}

/* epoll vtable */
static const struct IOBackendVTable epoll_vtable = {
    epoll_free,
    epoll_register,
    epoll_modify,
    epoll_unregister,
    epoll_poll,
    epoll_wake
};

/* Create a new epoll backend */
IOBackend *io_epoll_new(void) {
    struct EpollBackend *eb = calloc(1, sizeof(struct EpollBackend));
    if (!eb) return NULL;
    
    eb->base.vtable = &epoll_vtable;
    eb->epoll_fd = -1;
    eb->wakeup_pipe[0] = -1;
    eb->wakeup_pipe[1] = -1;
    
    /* Create epoll instance */
    eb->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (eb->epoll_fd < 0) {
        free(eb);
        return NULL;
    }
    
    /* Create wakeup pipe for interrupting epoll_wait */
    if (pipe(eb->wakeup_pipe) != 0) {
        close(eb->epoll_fd);
        free(eb);
        return NULL;
    }
    
    /* Set both ends of pipe to non-blocking */
    int flags = fcntl(eb->wakeup_pipe[0], F_GETFL, 0);
    fcntl(eb->wakeup_pipe[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(eb->wakeup_pipe[1], F_GETFL, 0);
    fcntl(eb->wakeup_pipe[1], F_SETFL, flags | O_NONBLOCK);
    
    /* Register wakeup pipe read end with epoll */
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = eb->wakeup_pipe[0];
    if (epoll_ctl(eb->epoll_fd, EPOLL_CTL_ADD, eb->wakeup_pipe[0], &ev) != 0) {
        close(eb->wakeup_pipe[0]);
        close(eb->wakeup_pipe[1]);
        close(eb->epoll_fd);
        free(eb);
        return NULL;
    }
    
    /* Add wakeup pipe to registrations so it gets cleaned up */
    FDRegistration *reg = get_or_add_registration(eb, eb->wakeup_pipe[0]);
    if (reg) {
        reg->callback = wakeup_callback;
        reg->user_data = NULL;
    }
    
    return &eb->base;
}
