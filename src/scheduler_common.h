#ifndef TUR_SCHEDULER_COMMON_H
#define TUR_SCHEDULER_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration */
typedef struct FiberBlock FiberBlock;

/* Common scheduler interface for both single-threaded and multi-threaded schedulers */
typedef struct TurSchedulerCommon TurSchedulerCommon;

struct TurSchedulerCommon {
    /* Virtual function table for scheduler operations */
    void (*spawn)(TurSchedulerCommon *s, FiberBlock *f);
    void (*run)(TurSchedulerCommon *s);
    void (*run_to_completion)(TurSchedulerCommon *s);
    void (*yield)(TurSchedulerCommon *s);
    void (*park)(TurSchedulerCommon *s, FiberBlock *f);
    void (*unpark)(TurSchedulerCommon *s, FiberBlock *f);
    void (*free)(TurSchedulerCommon *s);
    
    /* Scheduler-specific data */
    void *impl;
    bool is_mt;  /* true for multi-threaded, false for single-threaded */
    int64_t n_threads;  /* 1 for single-threaded, >1 for MT */
};

/* Scheduler type enum */
typedef enum {
    SCHEDULER_SINGLE_THREADED,
    SCHEDULER_MULTI_THREADED
} TurSchedulerType;

/* Create a new scheduler of the specified type */
TurSchedulerCommon *tur_scheduler_common_new(TurSchedulerType type, size_t n_threads);

/* Get the current scheduler for this thread */
TurSchedulerCommon *tur_scheduler_common_current(void);

/* Set the current scheduler for this thread */
void tur_scheduler_common_set_current(TurSchedulerCommon *s);

/* I/O integration: callback type for I/O completion */
typedef void (*tur_io_callback_t)(void *arg, int64_t result);

/* Register an I/O wait - scheduler will park the fiber until callback is called */
void tur_scheduler_io_wait(TurSchedulerCommon *s, FiberBlock *f, 
                           tur_io_callback_t callback, void *arg);

/* Signal I/O completion - wake up any fibers waiting on this */
void tur_scheduler_io_signal(TurSchedulerCommon *s, tur_io_callback_t callback, 
                             void *arg, int64_t result);

#endif /* TUR_SCHEDULER_COMMON_H */
