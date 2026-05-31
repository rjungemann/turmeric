/**
 * reactor.c -- lightweight evented I/O reactor (Phase R1/R4/R5/R6)
 *
 * Thin wrapper over IOBackend. Sources are heap-allocated and never freed
 * until tur_reactor_free so that the pointer passed as IOBackend user_data
 * remains valid for the entire lifetime of the reactor (prevents
 * use-after-free when a callback fires for a source that was removed
 * during the same poll batch).
 *
 * Timer sources (Phase R4) are stored in the same sources array but have
 * no IOBackend registration. tur_reactor_poll caps the io_poll timeout at
 * the earliest pending timer deadline and fires elapsed timers synchronously
 * after io_poll returns.
 *
 * Signal sources (Phase R5):
 *   Linux (IO_BACKEND_EPOLL): signalfd. Blocks the signal via sigprocmask,
 *   creates a signalfd and registers it as a readable fd.
 *   macOS/BSD (IO_BACKEND_KQUEUE): self-pipe + sigaction. A global dispatch
 *   table maps signal numbers to pipe write fds; the signal handler writes
 *   the signum as a byte; the reactor reads from the pipe read end.
 *   WASM: tur_reactor_add_signal returns -1 (unsupported).
 *
 * Channel bridge (Phase R6):
 *   tick_chans scans all TUR_RSRC_CHAN sources and non-blocking dequeues one
 *   value per source if available. Callers should call tur_reactor_wake(r)
 *   after chan-send for low-latency delivery.
 */

#include "reactor.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>

#ifdef IO_BACKEND_EPOLL
#include <sys/signalfd.h>
#endif

#define REACTOR_INITIAL_CAP 16

/* ------------------------------------------------------------------ */
/* Internal types                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    TUR_RSRC_FD             = 0,
    TUR_RSRC_TIMER_ONESHOT  = 1,
    TUR_RSRC_TIMER_INTERVAL = 2,
    TUR_RSRC_SIGNAL         = 3,
    TUR_RSRC_CHAN           = 4
} TurSourceKind;

typedef struct {
    int64_t       id;
    TurSourceKind kind;
    /* FD and signal (pipe read or signalfd) */
    int           fd;
    /* Signal-specific */
    int           signum;
    int           sig_write_fd;    /* kqueue: write end of self-pipe */
    struct sigaction sig_old_sa;   /* kqueue: saved action for restore */
    /* Timer-specific */
    int64_t       deadline_ms;     /* absolute CLOCK_MONOTONIC ms */
    int64_t       interval_ms;     /* 0 for one-shot timers */
    /* Channel bridge */
    void         *chan_ptr;        /* ChanBlock* */
    /* Common callback fields */
    int64_t       tur_cb;
    int64_t       tur_user_data;
    bool          active;
} TurReactorSource;

struct TurReactor {
    IOBackend           *backend;
    TurReactorSource   **sources;   /* stable pointers; never realloc'd entries */
    size_t               sources_len;
    size_t               sources_cap;
    int64_t              next_id;
    atomic_int           stop_flag;
};

/* ------------------------------------------------------------------ */
/* Self-pipe signal dispatch table (macOS / kqueue platforms)          */
/* ------------------------------------------------------------------ */

#if !defined(IO_BACKEND_EPOLL) && !defined(__EMSCRIPTEN__)
static int  g_sig_write_fd[NSIG];
static bool g_sig_table_init = false;

static void sig_self_pipe_handler(int signum) {
    if (signum >= 0 && signum < NSIG && g_sig_write_fd[signum] >= 0) {
        uint8_t b = (uint8_t)signum;
        (void)write(g_sig_write_fd[signum], &b, 1);
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Monotonic clock helper                                               */
/* ------------------------------------------------------------------ */

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* ------------------------------------------------------------------ */
/* Turmeric fat-closure dispatch                                         */
/* ------------------------------------------------------------------ */

/*
 * Call a Turmeric closure with signature (id:int, events:int, user:ptr<void>)
 * using the fat-pointer calling convention.  Reused for signal and chan
 * callbacks where the second argument carries signum/value respectively.
 */
static void call_tur_fd_cb(int64_t tur_cb, int64_t id, int events,
                            int64_t user_data) {
    if (!tur_cb) return;
    int64_t *fat = (int64_t *)(intptr_t)tur_cb;
    typedef int64_t (*fn3_t)(void *, int64_t, int64_t, int64_t);
    ((fn3_t)(intptr_t)fat[0])((void *)fat, id, (int64_t)events, user_data);
}

/*
 * Call a Turmeric closure with signature (id:int, user:ptr<void>)
 * using the fat-pointer calling convention. Used for timer callbacks.
 */
static void call_tur_timer_cb(int64_t tur_cb, int64_t id, int64_t user_data) {
    if (!tur_cb) return;
    int64_t *fat = (int64_t *)(intptr_t)tur_cb;
    typedef int64_t (*fn2_t)(void *, int64_t, int64_t);
    ((fn2_t)(intptr_t)fat[0])((void *)fat, id, user_data);
}

/*
 * Call a Turmeric closure with signature (id:int, value:int, user:ptr<void>)
 * for channel bridge callbacks where value is a full int64_t.
 */
static void call_tur_chan_cb(int64_t tur_cb, int64_t id, int64_t value,
                              int64_t user_data) {
    if (!tur_cb) return;
    int64_t *fat = (int64_t *)(intptr_t)tur_cb;
    typedef int64_t (*fn3_t)(void *, int64_t, int64_t, int64_t);
    ((fn3_t)(intptr_t)fat[0])((void *)fat, id, value, user_data);
}

/* ------------------------------------------------------------------ */
/* IOBackend shims                                                       */
/* ------------------------------------------------------------------ */

/* Registered as the IOBackend callback for fd sources. */
static void fd_shim(int fd, int events, void *user_data) {
    (void)fd;
    TurReactorSource *src = (TurReactorSource *)user_data;
    if (!src->active) return;
    call_tur_fd_cb(src->tur_cb, src->id, events, src->tur_user_data);
}

/* Registered as the IOBackend callback for signal sources. */
static void signal_shim(int fd, int events, void *user_data) {
    TurReactorSource *src = (TurReactorSource *)user_data;
    if (!src->active) return;
    (void)events;
#ifdef IO_BACKEND_EPOLL
    struct signalfd_siginfo si;
    ssize_t n;
    while ((n = read(fd, &si, sizeof(si))) == (ssize_t)sizeof(si))
        call_tur_fd_cb(src->tur_cb, src->id, (int)si.ssi_signo, src->tur_user_data);
#elif !defined(__EMSCRIPTEN__)
    uint8_t b;
    while (read(fd, &b, 1) == 1)
        call_tur_fd_cb(src->tur_cb, src->id, (int)b, src->tur_user_data);
#else
    (void)fd;
#endif
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

TurReactor *tur_reactor_new(void) {
#ifdef __EMSCRIPTEN__
    return NULL;
#endif
    IOBackend *backend = io_backend_new();
    if (!backend) return NULL;

    TurReactor *r = (TurReactor *)calloc(1, sizeof(TurReactor));
    if (!r) {
        io_backend_free(backend);
        return NULL;
    }

    r->sources = (TurReactorSource **)calloc(REACTOR_INITIAL_CAP,
                                              sizeof(TurReactorSource *));
    if (!r->sources) {
        io_backend_free(backend);
        free(r);
        return NULL;
    }

    r->backend      = backend;
    r->sources_cap  = REACTOR_INITIAL_CAP;
    r->sources_len  = 0;
    r->next_id      = 0;
    atomic_init(&r->stop_flag, 0);
    return r;
}

static void cleanup_source(TurReactor *r, TurReactorSource *src) {
    switch (src->kind) {
    case TUR_RSRC_FD:
        io_unregister(r->backend, src->fd);
        break;
    case TUR_RSRC_SIGNAL:
#ifdef IO_BACKEND_EPOLL
        io_unregister(r->backend, src->fd);
        close(src->fd);
        {
            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, src->signum);
            sigprocmask(SIG_UNBLOCK, &mask, NULL);
        }
#elif !defined(__EMSCRIPTEN__)
        io_unregister(r->backend, src->fd);
        sigaction(src->signum, &src->sig_old_sa, NULL);
        if (src->signum >= 0 && src->signum < NSIG)
            g_sig_write_fd[src->signum] = -1;
        close(src->fd);
        if (src->sig_write_fd >= 0) close(src->sig_write_fd);
#endif
        break;
    case TUR_RSRC_TIMER_ONESHOT:
    case TUR_RSRC_TIMER_INTERVAL:
    case TUR_RSRC_CHAN:
        break; /* no IOBackend registration */
    }
}

void tur_reactor_free(void *rp) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return;
    for (size_t i = 0; i < r->sources_len; i++) {
        TurReactorSource *src = r->sources[i];
        if (src) {
            if (src->active)
                cleanup_source(r, src);
            free(src);
        }
    }
    free(r->sources);
    io_backend_free(r->backend);
    free(r);
}

/* ------------------------------------------------------------------ */
/* Source registration                                                  */
/* ------------------------------------------------------------------ */

/* Append a fresh source slot, growing the array if needed. */
static TurReactorSource *alloc_source(TurReactor *r) {
    if (r->sources_len == r->sources_cap) {
        size_t new_cap = r->sources_cap * 2;
        TurReactorSource **arr = (TurReactorSource **)realloc(
            r->sources, new_cap * sizeof(TurReactorSource *));
        if (!arr) return NULL;
        memset(arr + r->sources_cap, 0,
               (new_cap - r->sources_cap) * sizeof(TurReactorSource *));
        r->sources     = arr;
        r->sources_cap = new_cap;
    }
    TurReactorSource *src = (TurReactorSource *)calloc(1, sizeof(TurReactorSource));
    if (!src) return NULL;
    src->sig_write_fd = -1;
    r->sources[r->sources_len++] = src;
    return src;
}

static TurReactorSource *find_source(TurReactor *r, int64_t id) {
    for (size_t i = 0; i < r->sources_len; i++) {
        TurReactorSource *src = r->sources[i];
        if (src && src->active && src->id == id)
            return src;
    }
    return NULL;
}

int64_t tur_reactor_add_fd(void *rp, int64_t fd, int64_t events,
                            int64_t tur_cb, void *tur_user_data) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return -1;

    TurReactorSource *src = alloc_source(r);
    if (!src) return -1;

    src->id            = r->next_id++;
    src->kind          = TUR_RSRC_FD;
    src->fd            = (int)fd;
    src->tur_cb        = tur_cb;
    src->tur_user_data = (int64_t)(intptr_t)tur_user_data;
    src->active        = true;

    if (io_register(r->backend, src->fd, (int)events, fd_shim, src) < 0) {
        src->active = false;
        return -1;
    }
    return src->id;
}

int64_t tur_reactor_modify_fd(void *rp, int64_t id, int64_t new_events) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return -1;
    TurReactorSource *src = find_source(r, id);
    if (!src) return -1;
    return (int64_t)io_modify(r->backend, src->fd, (int)new_events);
}

int64_t tur_reactor_remove(void *rp, int64_t id) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return -1;
    TurReactorSource *src = find_source(r, id);
    if (!src) return -1;
    cleanup_source(r, src);
    src->active = false;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Timer registration (Phase R4)                                        */
/* ------------------------------------------------------------------ */

int64_t tur_reactor_add_timer(void *rp, int64_t delay_ms,
                               int64_t tur_cb, void *tur_user_data) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return -1;

    TurReactorSource *src = alloc_source(r);
    if (!src) return -1;

    src->id            = r->next_id++;
    src->kind          = TUR_RSRC_TIMER_ONESHOT;
    src->deadline_ms   = now_ms() + delay_ms;
    src->interval_ms   = 0;
    src->tur_cb        = tur_cb;
    src->tur_user_data = (int64_t)(intptr_t)tur_user_data;
    src->active        = true;
    return src->id;
}

int64_t tur_reactor_add_interval(void *rp, int64_t first_ms, int64_t interval_ms,
                                  int64_t tur_cb, void *tur_user_data) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return -1;

    TurReactorSource *src = alloc_source(r);
    if (!src) return -1;

    src->id            = r->next_id++;
    src->kind          = TUR_RSRC_TIMER_INTERVAL;
    src->deadline_ms   = now_ms() + first_ms;
    src->interval_ms   = interval_ms;
    src->tur_cb        = tur_cb;
    src->tur_user_data = (int64_t)(intptr_t)tur_user_data;
    src->active        = true;
    return src->id;
}

/* ------------------------------------------------------------------ */
/* Signal registration (Phase R5)                                       */
/* ------------------------------------------------------------------ */

int64_t tur_reactor_add_signal(void *rp, int64_t signum_i,
                                int64_t tur_cb, void *tur_user_data) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return -1;

    TurReactorSource *src = alloc_source(r);
    if (!src) return -1;

    src->id            = r->next_id++;
    src->kind          = TUR_RSRC_SIGNAL;
    src->signum        = (int)signum_i;
    src->tur_cb        = tur_cb;
    src->tur_user_data = (int64_t)(intptr_t)tur_user_data;
    src->active        = false;

#ifdef IO_BACKEND_EPOLL
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, (int)signum_i);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) return -1;
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0) return -1;
    src->fd = sfd;
    if (io_register(r->backend, sfd, IO_EVENT_READ, signal_shim, src) < 0) {
        close(sfd);
        return -1;
    }
    src->active = true;
    return src->id;

#elif !defined(__EMSCRIPTEN__)
    if (!g_sig_table_init) {
        for (int i = 0; i < NSIG; i++) g_sig_write_fd[i] = -1;
        g_sig_table_init = true;
    }
    int pipefds[2];
    if (pipe(pipefds) < 0) return -1;
    fcntl(pipefds[0], F_SETFL, fcntl(pipefds[0], F_GETFL) | O_NONBLOCK);
    fcntl(pipefds[1], F_SETFL, fcntl(pipefds[1], F_GETFL) | O_NONBLOCK);
    g_sig_write_fd[(int)signum_i] = pipefds[1];
    src->fd           = pipefds[0];
    src->sig_write_fd = pipefds[1];
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_self_pipe_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction((int)signum_i, &sa, &src->sig_old_sa);
    if (io_register(r->backend, pipefds[0], IO_EVENT_READ, signal_shim, src) < 0) {
        sigaction((int)signum_i, &src->sig_old_sa, NULL);
        g_sig_write_fd[(int)signum_i] = -1;
        close(pipefds[0]);
        close(pipefds[1]);
        return -1;
    }
    src->active = true;
    return src->id;

#else
    /* WASM: signals not supported */
    return -1;
#endif
}

/* ------------------------------------------------------------------ */
/* Channel bridge registration (Phase R6)                               */
/* ------------------------------------------------------------------ */

int64_t tur_reactor_add_chan(void *rp, void *chan,
                              int64_t tur_cb, void *tur_user_data) {
    TurReactor *r = (TurReactor *)rp;
    if (!r || !chan) return -1;

    TurReactorSource *src = alloc_source(r);
    if (!src) return -1;

    src->id            = r->next_id++;
    src->kind          = TUR_RSRC_CHAN;
    src->chan_ptr       = chan;
    src->tur_cb        = tur_cb;
    src->tur_user_data = (int64_t)(intptr_t)tur_user_data;
    src->active        = true;
    return src->id;
}

/* ------------------------------------------------------------------ */
/* Timer helpers                                                        */
/* ------------------------------------------------------------------ */

static int64_t cap_timeout(TurReactor *r, int64_t timeout_ms) {
    int64_t t = now_ms();
    for (size_t i = 0; i < r->sources_len; i++) {
        TurReactorSource *src = r->sources[i];
        if (!src || !src->active) continue;
        if (src->kind != TUR_RSRC_TIMER_ONESHOT &&
            src->kind != TUR_RSRC_TIMER_INTERVAL) continue;

        int64_t remaining = src->deadline_ms - t;
        if (remaining < 0) remaining = 0;
        if (timeout_ms < 0 || remaining < timeout_ms)
            timeout_ms = remaining;
    }
    return timeout_ms;
}

static int tick_timers(TurReactor *r) {
    int64_t t = now_ms();
    int fired = 0;
    for (size_t i = 0; i < r->sources_len; i++) {
        TurReactorSource *src = r->sources[i];
        if (!src || !src->active) continue;
        if (src->kind != TUR_RSRC_TIMER_ONESHOT &&
            src->kind != TUR_RSRC_TIMER_INTERVAL) continue;
        if (t < src->deadline_ms) continue;

        call_tur_timer_cb(src->tur_cb, src->id, src->tur_user_data);
        fired++;

        if (src->kind == TUR_RSRC_TIMER_ONESHOT) {
            src->active = false;
        } else {
            src->deadline_ms += src->interval_ms;
            if (src->deadline_ms <= t)
                src->deadline_ms = t + src->interval_ms;
        }
    }
    return fired;
}

/* ------------------------------------------------------------------ */
/* Channel bridge tick (Phase R6)                                       */
/* ------------------------------------------------------------------ */

/*
 * Minimal ChanBlock layout -- must match the inline typedef used by
 * chan.tur and async-chan*.  Both this file and the generated C are compiled
 * into the same binary, so the platform's pthread struct sizes agree.
 */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
    int64_t        *buf;
    int64_t         head;
    int64_t         tail;
    int64_t         count;
    int64_t         cap;
    void           *recv_waiters;
    void           *send_waiters;
} TurChanBlock;

static int tick_chans(TurReactor *r) {
    int fired = 0;
    for (size_t i = 0; i < r->sources_len; i++) {
        TurReactorSource *src = r->sources[i];
        if (!src || !src->active || src->kind != TUR_RSRC_CHAN) continue;
        TurChanBlock *c = (TurChanBlock *)src->chan_ptr;
        pthread_mutex_lock(&c->lock);
        if (c->count == 0) {
            pthread_mutex_unlock(&c->lock);
            continue;
        }
        int64_t v = c->buf[c->head];
        c->head = (c->head + 1) % c->cap;
        c->count--;
        pthread_cond_signal(&c->not_full);
        pthread_mutex_unlock(&c->lock);
        call_tur_chan_cb(src->tur_cb, src->id, v, src->tur_user_data);
        src->active = false; /* one-shot */
        fired++;
    }
    return fired;
}

/* ------------------------------------------------------------------ */
/* Running                                                              */
/* ------------------------------------------------------------------ */

int64_t tur_reactor_poll(void *rp, int64_t timeout_ms) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return -1;
    int64_t capped = cap_timeout(r, timeout_ms);
    int fd_count = io_poll(r->backend, (int)capped);
    if (fd_count < 0) return -1;
    int timer_count = tick_timers(r);
    int chan_count  = tick_chans(r);
    return (int64_t)(fd_count + timer_count + chan_count);
}

void tur_reactor_run(void *rp) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return;
    atomic_store(&r->stop_flag, 0);
    for (;;) {
        if (atomic_load(&r->stop_flag)) break;

        bool has_active = false;
        for (size_t i = 0; i < r->sources_len; i++) {
            if (r->sources[i] && r->sources[i]->active) {
                has_active = true;
                break;
            }
        }
        if (!has_active) break;

        int64_t capped = cap_timeout(r, -1);
        io_poll(r->backend, (int)capped);
        tick_timers(r);
        tick_chans(r);
    }
}

void tur_reactor_stop(void *rp) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return;
    atomic_store(&r->stop_flag, 1);
    io_wake(r->backend);
}

void tur_reactor_wake(void *rp) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return;
    io_wake(r->backend);
}
