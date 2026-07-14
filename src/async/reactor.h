/**
 * reactor.h -- lightweight evented I/O reactor (Phase R1/R4/R5/R6)
 *
 * A single-threaded event loop that multiplexes file descriptors, timers,
 * OS signals, and channel receives using the platform IOBackend (epoll/kqueue).
 * Sources are identified by an int64_t id returned at registration; callers do
 * not need to track raw fds, timer nodes, or signal fds separately.
 *
 * Thread safety: a TurReactor is NOT thread-safe. Each thread that wants to
 * poll owns its own reactor. Only tur_reactor_stop and tur_reactor_wake are
 * safe to call from other threads.
 *
 * FD callbacks (Phase R1):
 *   Turmeric fat-closure with signature: (id:int, events:int, user:ptr<void>)
 *   int64_t *fat = (int64_t *)(intptr_t)tur_cb;
 *   ((fn3_t)(intptr_t)fat[0])((void *)fat, id, events, user_data);
 *
 * Timer callbacks (Phase R4):
 *   Turmeric fat-closure with signature: (id:int, user:ptr<void>)
 *   int64_t *fat = (int64_t *)(intptr_t)tur_cb;
 *   ((fn2_t)(intptr_t)fat[0])((void *)fat, id, user_data);
 *
 * Signal callbacks (Phase R5):
 *   Same 3-arg convention as FD; second arg is the signal number.
 *
 * Channel bridge callbacks (Phase R6):
 *   Same 3-arg convention as FD; second arg is the received value.
 *   One-shot: fires once per tur_reactor_add_chan call and deactivates.
 *   Re-register from the callback for a persistent watcher.
 *   Caller must call tur_reactor_wake(r) after chan-send for low-latency
 *   delivery when the reactor is sleeping in tur_reactor_run.
 */

#ifndef TUR_REACTOR_H
#define TUR_REACTOR_H

#include <stdint.h>
#include <stdbool.h>
#include "async_io.h"

typedef struct TurReactor TurReactor;

/** Create a reactor for the current thread. Returns NULL on error or WASM. */
TurReactor *tur_reactor_new(void);

/** Free a reactor; unregisters all attached sources and cancels all timers. */
void tur_reactor_free(void *r);

/**
 * Register a file descriptor. Returns a source id (>= 0) or -1 on error.
 *
 * tur_cb        -- Turmeric fat-closure called as (id:int, events:int, user:ptr<void>)
 * tur_user_data -- forwarded to cb as the third argument
 */
int64_t tur_reactor_add_fd(void *r, int64_t fd, int64_t events,
                            int64_t tur_cb, void *tur_user_data);

/** Change the watched event mask for a source. Returns 0 or -1. */
int64_t tur_reactor_modify_fd(void *r, int64_t id, int64_t new_events);

/** Unregister a source by id. Returns 0 or -1. */
int64_t tur_reactor_remove(void *r, int64_t id);

/**
 * Relinquish reactor ownership of a source's callback box.
 *
 * The tur_reactor_add_* functions take ownership of the cb box by default and
 * free() it at tur_reactor_free -- the emitted program hands off a heap
 * fat-closure it never frees itself, so this is what keeps reactor programs
 * leak-clean. A caller that manages the box lifetime itself (e.g. the httpd
 * runtime, which caches and frees its own accept-callback box) must call this
 * after registering, or the box is double-freed. A no-op for id < 0.
 * Idempotent.
 */
void tur_reactor_disown_cb(void *r, int64_t id);

/**
 * Add a one-shot timer that fires once after delay_ms milliseconds.
 * Returns a source id (>= 0) or -1 on error.
 *
 * tur_cb        -- Turmeric fat-closure called as (id:int, user:ptr<void>)
 * tur_user_data -- forwarded to cb as the second argument
 */
int64_t tur_reactor_add_timer(void *r, int64_t delay_ms,
                               int64_t tur_cb, void *tur_user_data);

/**
 * Add a repeating interval timer.
 * Fires first after first_ms milliseconds, then every interval_ms thereafter.
 * Returns a source id (>= 0) or -1 on error.
 */
int64_t tur_reactor_add_interval(void *r, int64_t first_ms, int64_t interval_ms,
                                  int64_t tur_cb, void *tur_user_data);

/**
 * Watch an OS signal. Returns a source id (>= 0) or -1 on error.
 *
 * Uses signalfd on Linux and a self-pipe + sigaction on macOS/BSD.
 * The signal is blocked from default delivery while registered; removing
 * the source restores the previous disposition. Returns -1 on WASM.
 *
 * tur_cb -- Turmeric fat-closure called as (id:int, signum:int, user:ptr<void>)
 */
int64_t tur_reactor_add_signal(void *r, int64_t signum,
                                int64_t tur_cb, void *tur_user_data);

/**
 * Watch a channel for an incoming value (one-shot).
 * Fires at most once: when a value is available the callback receives it and
 * the source is deactivated. Re-register from the callback for a persistent
 * watcher.
 *
 * chan -- void * handle to a ChanBlock (from chan-new / async-chan-new)
 * tur_cb -- Turmeric fat-closure called as (id:int, value:int, user:ptr<void>)
 *
 * Delivery latency: tick_chans runs inside every tur_reactor_poll / run
 * iteration. For prompt delivery, call tur_reactor_wake(r) after chan-send.
 */
int64_t tur_reactor_add_chan(void *r, void *chan,
                              int64_t tur_cb, void *tur_user_data);

/**
 * Poll for events. Blocks up to timeout_ms (-1 = forever, 0 = non-blocking).
 * Timer deadlines cap the effective timeout so timers fire on time.
 * Returns the number of callbacks dispatched (fds + timers + signals + chans),
 * or -1 on error.
 */
int64_t tur_reactor_poll(void *r, int64_t timeout_ms);

/** Run until tur_reactor_stop is called or no active sources remain. */
void tur_reactor_run(void *r);

/** Ask reactor-run to stop after the current dispatch round. Thread-safe. */
void tur_reactor_stop(void *r);

/** Wake a reactor blocked in poll/run from another thread. Thread-safe. */
void tur_reactor_wake(void *r);

#endif /* TUR_REACTOR_H */
