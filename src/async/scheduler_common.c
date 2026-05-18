#include "scheduler_common.h"
#include "scheduler.h"
#include <stdlib.h>
#include <string.h>

/* Thread-local storage for current scheduler */
static __thread TurSchedulerCommon *tur_current_scheduler_common = NULL;

/* I/O wait structures */
typedef struct TurIOWaiter TurIOWaiter;
struct TurIOWaiter {
    FiberBlock *fiber;
    tur_io_callback_t callback;
    void *arg;
    TurIOWaiter *next;
};

typedef struct {
    TurIOWaiter *waiters;
} TurSchedulerIOWaits;

/* Common scheduler wrapper for single-threaded scheduler */
typedef struct {
    TurSchedulerCommon base;
    /* Single-threaded scheduler state would go here */
    /* For now, we use the generated TurScheduler from emit.c */
    /* This is a placeholder - actual ST scheduler is generated per-program */
    void *st_scheduler;
    TurSchedulerIOWaits io_waits;
} TurSchedulerCommonST;

/* Common scheduler wrapper for multi-threaded scheduler */
typedef struct {
    TurSchedulerCommon base;
    TurSchedulerMT *mt_scheduler;
    TurSchedulerIOWaits io_waits;
} TurSchedulerCommonMT;

/* ST scheduler functions — real implementations are in generated output.
 * These stubs satisfy the linker when building the compiler binary itself. */
__attribute__((weak)) void tur_scheduler_spawn_st(void *s, FiberBlock *f) { (void)s; (void)f; }
__attribute__((weak)) void tur_scheduler_run_st(void *s) { (void)s; }
__attribute__((weak)) void tur_scheduler_run_to_completion_st(void *s) { (void)s; }
__attribute__((weak)) void tur_scheduler_yield_st(void) {}
__attribute__((weak)) void tur_scheduler_park_st(FiberBlock *f) { (void)f; }
__attribute__((weak)) void tur_scheduler_unpark_st(FiberBlock *f) { (void)f; }

/* ST scheduler implementations */
static void scheduler_st_spawn(TurSchedulerCommon *s, FiberBlock *f) {
    TurSchedulerCommonST *st = (TurSchedulerCommonST *)s;
    if (st->st_scheduler) {
        tur_scheduler_spawn_st(st->st_scheduler, f);
    }
}

static void scheduler_st_run(TurSchedulerCommon *s) {
    TurSchedulerCommonST *st = (TurSchedulerCommonST *)s;
    if (st->st_scheduler) {
        tur_scheduler_run_st(st->st_scheduler);
    }
}

static void scheduler_st_run_to_completion(TurSchedulerCommon *s) {
    TurSchedulerCommonST *st = (TurSchedulerCommonST *)s;
    if (st->st_scheduler) {
        tur_scheduler_run_to_completion_st(st->st_scheduler);
    }
}

static void scheduler_st_yield(TurSchedulerCommon *s) {
    (void)s; /* ST yield doesn't need scheduler context */
    tur_scheduler_yield_st();
}

static void scheduler_st_park(TurSchedulerCommon *s, FiberBlock *f) {
    (void)s;
    tur_scheduler_park_st(f);
}

static void scheduler_st_unpark(TurSchedulerCommon *s, FiberBlock *f) {
    (void)s;
    tur_scheduler_unpark_st(f);
}

static void scheduler_st_free(TurSchedulerCommon *s) {
    TurSchedulerCommonST *st = (TurSchedulerCommonST *)s;
    /* ST scheduler is managed by generated code, don't free here */
    free(st);
}

/* MT scheduler implementations */
static void scheduler_mt_spawn(TurSchedulerCommon *s, FiberBlock *f) {
    TurSchedulerCommonMT *mt = (TurSchedulerCommonMT *)s;
    if (mt->mt_scheduler) {
        tur_scheduler_mt_spawn(mt->mt_scheduler, f);
    }
}

static void scheduler_mt_run(TurSchedulerCommon *s) {
    TurSchedulerCommonMT *mt = (TurSchedulerCommonMT *)s;
    if (mt->mt_scheduler) {
        tur_scheduler_mt_run(mt->mt_scheduler);
    }
}

static void scheduler_mt_run_to_completion(TurSchedulerCommon *s) {
    TurSchedulerCommonMT *mt = (TurSchedulerCommonMT *)s;
    if (mt->mt_scheduler) {
        tur_scheduler_mt_run_to_completion(mt->mt_scheduler);
    }
}

static void scheduler_mt_yield(TurSchedulerCommon *s) {
    (void)s;
    tur_scheduler_mt_yield();
}

static void scheduler_mt_park(TurSchedulerCommon *s, FiberBlock *f) {
    TurSchedulerCommonMT *mt = (TurSchedulerCommonMT *)s;
    (void)mt;
    tur_scheduler_mt_park();
    /* Note: park doesn't take fiber arg in current MT scheduler */
}

static void scheduler_mt_unpark(TurSchedulerCommon *s, FiberBlock *f) {
    (void)s;
    tur_scheduler_mt_unpark(f);
}

static void scheduler_mt_free(TurSchedulerCommon *s) {
    TurSchedulerCommonMT *mt = (TurSchedulerCommonMT *)s;
    if (mt->mt_scheduler) {
        tur_scheduler_mt_free(mt->mt_scheduler);
    }
    free(mt);
}

/* Create a new common scheduler wrapper */
TurSchedulerCommon *tur_scheduler_common_new(TurSchedulerType type, size_t n_threads) {
    if (type == SCHEDULER_SINGLE_THREADED) {
        TurSchedulerCommonST *st = (TurSchedulerCommonST *)calloc(1, sizeof(TurSchedulerCommonST));
        if (!st) return NULL;
        
        st->base.spawn = scheduler_st_spawn;
        st->base.run = scheduler_st_run;
        st->base.run_to_completion = scheduler_st_run_to_completion;
        st->base.yield = scheduler_st_yield;
        st->base.park = scheduler_st_park;
        st->base.unpark = scheduler_st_unpark;
        st->base.free = scheduler_st_free;
        st->base.impl = NULL; /* ST scheduler is global in generated code */
        st->base.is_mt = false;
        st->base.n_threads = 1;
        st->io_waits.waiters = NULL;
        
        return (TurSchedulerCommon *)st;
    } else {
        TurSchedulerCommonMT *mt = (TurSchedulerCommonMT *)calloc(1, sizeof(TurSchedulerCommonMT));
        if (!mt) return NULL;
        
        mt->mt_scheduler = tur_scheduler_mt_new(n_threads);
        if (!mt->mt_scheduler) {
            free(mt);
            return NULL;
        }
        
        mt->base.spawn = scheduler_mt_spawn;
        mt->base.run = scheduler_mt_run;
        mt->base.run_to_completion = scheduler_mt_run_to_completion;
        mt->base.yield = scheduler_mt_yield;
        mt->base.park = scheduler_mt_park;
        mt->base.unpark = scheduler_mt_unpark;
        mt->base.free = scheduler_mt_free;
        mt->base.impl = mt->mt_scheduler;
        mt->base.is_mt = true;
        mt->base.n_threads = (int64_t)n_threads;
        mt->io_waits.waiters = NULL;
        
        return (TurSchedulerCommon *)mt;
    }
}

TurSchedulerCommon *tur_scheduler_common_current(void) {
    return tur_current_scheduler_common;
}

void tur_scheduler_common_set_current(TurSchedulerCommon *s) {
    tur_current_scheduler_common = s;
}

/* I/O wait management */
static void io_waiter_free(TurIOWaiter *waiter) __attribute__((unused));
static void io_waiter_free(TurIOWaiter *waiter) {
    if (waiter) {
        io_waiter_free(waiter->next);
        free(waiter);
    }
}

void tur_scheduler_io_wait(TurSchedulerCommon *s, FiberBlock *f, 
                           tur_io_callback_t callback, void *arg) {
    if (!s || !f) return;
    
    TurIOWaiter *waiter = (TurIOWaiter *)malloc(sizeof(TurIOWaiter));
    if (!waiter) return;
    
    waiter->fiber = f;
    waiter->callback = callback;
    waiter->arg = arg;
    waiter->next = NULL;
    
    /* Add to scheduler's IO wait list */
    if (s->is_mt) {
        TurSchedulerCommonMT *mt = (TurSchedulerCommonMT *)s;
        waiter->next = mt->io_waits.waiters;
        mt->io_waits.waiters = waiter;
    } else {
        TurSchedulerCommonST *st = (TurSchedulerCommonST *)s;
        waiter->next = st->io_waits.waiters;
        st->io_waits.waiters = waiter;
    }
    
    /* Park the fiber - it will be unparked when IO completes */
    s->park(s, f);
}

void tur_scheduler_io_signal(TurSchedulerCommon *s, tur_io_callback_t callback, 
                             void *arg, int64_t result) {
    if (!s) return;
    
    TurIOWaiter **waiter_p = NULL;
    if (s->is_mt) {
        TurSchedulerCommonMT *mt = (TurSchedulerCommonMT *)s;
        waiter_p = &mt->io_waits.waiters;
    } else {
        TurSchedulerCommonST *st = (TurSchedulerCommonST *)s;
        waiter_p = &st->io_waits.waiters;
    }
    
    /* Find and remove matching waiters */
    while (waiter_p && *waiter_p) {
        TurIOWaiter *waiter = *waiter_p;
        if (waiter->callback == callback && waiter->arg == arg) {
            /* Found a match - unpark the fiber and invoke callback */
            *waiter_p = waiter->next;
            
            /* Invoke the callback */
            if (waiter->callback) {
                waiter->callback(waiter->arg, result);
            }
            
            /* Unpark the fiber */
            s->unpark(s, waiter->fiber);
            
            free(waiter);
        } else {
            waiter_p = &(*waiter_p)->next;
        }
    }
}
