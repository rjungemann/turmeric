/**
 * timer_wheel.h — O(1) hierarchical timing wheel (Phase T24)
 *
 * Provides efficient timer management with O(1) insert, O(1) cancel,
 * and O(1) amortized tick (each timer slot is processed once per ms).
 *
 * Resolution: 1ms per slot.
 * Capacity: 256 slots (wraps every ~256ms; timers in same modular slot
 *           are distinguished by their absolute deadline).
 */

#ifndef TUR_TIMER_WHEEL_H
#define TUR_TIMER_WHEEL_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#define TUR_WHEEL_SLOTS 256
#define TUR_WHEEL_SLOT_MASK 255

typedef void (*tur_timer_cb_t)(void *arg);

typedef struct TurTimerNode TurTimerNode;
struct TurTimerNode {
    int64_t deadline_ms;   /* absolute deadline (CLOCK_MONOTONIC ms)  */
    tur_timer_cb_t callback;
    void *arg;
    bool cancelled;        /* lazy cancel flag                         */
    TurTimerNode *next;    /* intrusive linked list within slot        */
    TurTimerNode *prev;
};

typedef struct TurTimerWheel TurTimerWheel;
struct TurTimerWheel {
    TurTimerNode *slots[TUR_WHEEL_SLOTS];
    int64_t current_ms;    /* last processed millisecond               */
    pthread_mutex_t lock;
    bool running;
    pthread_t thread;      /* background ticker thread                 */
};

/** Create a new timer wheel and start its ticker thread. */
TurTimerWheel *tur_timer_wheel_new(void);

/** Stop the ticker thread and free all resources. */
void tur_timer_wheel_free(TurTimerWheel *w);

/**
 * Insert a timer that fires callback(arg) after delay_ms milliseconds.
 * Returns a node pointer that can be passed to tur_timer_wheel_cancel.
 * The node is freed automatically after the callback fires.
 */
TurTimerNode *tur_timer_wheel_insert(TurTimerWheel *w, int64_t delay_ms,
                                      tur_timer_cb_t callback, void *arg);

/**
 * Cancel a pending timer.  The callback will not be invoked.
 * O(1): just marks the node; memory is reclaimed on the next tick.
 */
void tur_timer_wheel_cancel(TurTimerNode *node);

/**
 * Advance the wheel to the current time and fire all elapsed timers.
 * Called by the background ticker thread; may also be called manually.
 */
void tur_timer_wheel_tick(TurTimerWheel *w);

/** Return monotonic time in milliseconds. */
int64_t tur_timer_wheel_now_ms(void);

#endif /* TUR_TIMER_WHEEL_H */
