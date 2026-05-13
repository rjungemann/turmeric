#ifndef TUR_SCHEDULER_H
#define TUR_SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "timer_wheel.h"

/* Forward declaration */
typedef struct FiberBlock FiberBlock;

/* Phase T23: Multi-threaded work-stealing scheduler */

/* Per-thread work deque for work-stealing */
typedef struct WorkStealingDeque WorkStealingDeque;

/* Multi-threaded scheduler */
typedef struct TurSchedulerMT TurSchedulerMT;

/* Create a new multi-threaded scheduler with n_threads worker threads */
TurSchedulerMT *tur_scheduler_mt_new(size_t n_threads);

/* Free the scheduler */
void tur_scheduler_mt_free(TurSchedulerMT *s);

/* Get the current thread's scheduler */
TurSchedulerMT *tur_scheduler_mt_current(void);

/* Set the current thread's scheduler (for testing) */
void tur_scheduler_mt_set_current(TurSchedulerMT *s);

/* Spawn a fiber on the scheduler */
void tur_scheduler_mt_spawn(TurSchedulerMT *s, FiberBlock *f);

/* Run the scheduler event loop on the current thread */
void tur_scheduler_mt_run(TurSchedulerMT *s);

/* Run until all work is complete */
void tur_scheduler_mt_run_to_completion(TurSchedulerMT *s);

/* Yield the current fiber, allowing other fibers to run */
void tur_scheduler_mt_yield(void);

/* Park the current fiber until explicitly unparked */
void tur_scheduler_mt_park(void);

/* Unpark a specific fiber */
void tur_scheduler_mt_unpark(FiberBlock *f);

/* Get the current thread ID (for debugging) */
int64_t tur_scheduler_mt_thread_id(void);

/* Work-stealing deque API */
WorkStealingDeque *ws_deque_new(size_t capacity);
void ws_deque_free(WorkStealingDeque *d);
bool ws_deque_push(WorkStealingDeque *d, FiberBlock *f);
FiberBlock *ws_deque_pop(WorkStealingDeque *d);
FiberBlock *ws_deque_steal(WorkStealingDeque *d);
bool ws_deque_empty(WorkStealingDeque *d);

/* SCH-003: I/O integration */
/* Register a fiber to wait on I/O events for a file descriptor */
void tur_scheduler_mt_io_wait(TurSchedulerMT *s, int fd, int events, FiberBlock *f, void *user_data);

/* Modify I/O interest for a file descriptor */
void tur_scheduler_mt_io_modify(TurSchedulerMT *s, int fd, int events);

/* Unregister I/O interest for a file descriptor */
void tur_scheduler_mt_io_unregister(TurSchedulerMT *s, int fd);

/* SCH-003: ThreadPool integration */
/* Get the TurSchedulerMT from a ThreadPool (if ThreadPool uses scheduler threads) */
TurSchedulerMT *tur_scheduler_mt_from_threadpool(void *threadpool);

/* Phase T24: Timer wheel integration */
TurTimerWheel *tur_scheduler_mt_get_timer_wheel(TurSchedulerMT *s);

/* Set the TurSchedulerMT for a ThreadPool */
void tur_scheduler_mt_set_for_threadpool(void *threadpool, TurSchedulerMT *s);

/* SCH-006: Performance metrics */

/* Snapshot of scheduler performance counters. */
typedef struct {
    int64_t steal_count;    /* Successful work steals across all threads */
    int64_t steal_attempts; /* Total steal attempts (includes failures) */
    int64_t global_pops;    /* Items taken from the global queue */
    int64_t busy_iters;     /* Worker loop iterations that found and ran work */
    int64_t idle_iters;     /* Worker loop iterations that found no work */
} TurSchedulerMTMetrics;

/* Fill *out with a consistent snapshot of the current metrics. */
void tur_scheduler_mt_get_metrics(TurSchedulerMT *s, TurSchedulerMTMetrics *out);

/* Reset all metric counters to zero. */
void tur_scheduler_mt_reset_metrics(TurSchedulerMT *s);

/* Return the current number of items in one thread's deque (approximate). */
size_t tur_scheduler_mt_deque_length(TurSchedulerMT *s, size_t thread_idx);

#endif /* TUR_SCHEDULER_H */
