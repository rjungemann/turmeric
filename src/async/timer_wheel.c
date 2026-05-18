/**
 * timer_wheel.c — O(1) hierarchical timing wheel (Phase T24)
 */

#include "timer_wheel.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

int64_t tur_timer_wheel_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)ts.tv_nsec / 1000000LL;
}

/* --------------------------------------------------------------------------
 * Background ticker thread
 * -------------------------------------------------------------------------- */

static void *timer_wheel_thread(void *arg) {
    TurTimerWheel *w = (TurTimerWheel *)arg;
    while (w->running) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L }; /* 1 ms */
        nanosleep(&ts, NULL);
        tur_timer_wheel_tick(w);
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

TurTimerWheel *tur_timer_wheel_new(void) {
    TurTimerWheel *w = (TurTimerWheel *)calloc(1, sizeof(TurTimerWheel));
    if (!w) return NULL;
    pthread_mutex_init(&w->lock, NULL);
    w->current_ms = tur_timer_wheel_now_ms();
    w->running = true;
    pthread_create(&w->thread, NULL, timer_wheel_thread, w);
    return w;
}

void tur_timer_wheel_free(TurTimerWheel *w) {
    if (!w) return;

    w->running = false;
    pthread_join(w->thread, NULL);

    /* Drain remaining nodes */
    pthread_mutex_lock(&w->lock);
    for (int i = 0; i < TUR_WHEEL_SLOTS; i++) {
        TurTimerNode *n = w->slots[i];
        while (n) {
            TurTimerNode *next = n->next;
            free(n);
            n = next;
        }
        w->slots[i] = NULL;
    }
    pthread_mutex_unlock(&w->lock);

    pthread_mutex_destroy(&w->lock);
    free(w);
}

TurTimerNode *tur_timer_wheel_insert(TurTimerWheel *w, int64_t delay_ms,
                                      tur_timer_cb_t callback, void *arg) {
    if (!w || !callback) return NULL;

    TurTimerNode *node = (TurTimerNode *)calloc(1, sizeof(TurTimerNode));
    if (!node) return NULL;

    node->deadline_ms = tur_timer_wheel_now_ms() + delay_ms;
    node->callback    = callback;
    node->arg         = arg;
    node->cancelled   = false;

    /* O(1) insert: hash by deadline into a slot */
    int slot = (int)(node->deadline_ms & TUR_WHEEL_SLOT_MASK);

    pthread_mutex_lock(&w->lock);
    node->next = w->slots[slot];
    node->prev = NULL;
    if (w->slots[slot]) w->slots[slot]->prev = node;
    w->slots[slot] = node;
    pthread_mutex_unlock(&w->lock);

    return node;
}

void tur_timer_wheel_cancel(TurTimerNode *node) {
    if (node) node->cancelled = true;  /* O(1) lazy cancel */
}

void tur_timer_wheel_tick(TurTimerWheel *w) {
    if (!w) return;

    int64_t now     = tur_timer_wheel_now_ms();
    int64_t elapsed = now - w->current_ms;
    if (elapsed <= 0) return;

    /*
     * Advance the wheel one millisecond at a time.  For each tick we check
     * the slot at (current_ms & MASK) and fire any timer whose deadline has
     * been reached.  Timers that share the same modular slot but are due in
     * a future wrap are left in place.
     */
    for (int64_t t = 0; t < elapsed; t++) {
        w->current_ms++;
        int slot = (int)(w->current_ms & TUR_WHEEL_SLOT_MASK);

        /* Collect timers to fire into a local list (hold lock briefly) */
        TurTimerNode *to_fire = NULL;

        pthread_mutex_lock(&w->lock);
        TurTimerNode **pp = &w->slots[slot];
        while (*pp) {
            TurTimerNode *n = *pp;
            if (n->cancelled) {
                /* Lazy-delete cancelled timers */
                *pp = n->next;
                if (n->next) n->next->prev = n->prev;
                free(n);
            } else if (n->deadline_ms <= w->current_ms) {
                /* Deadline reached — remove and queue for firing */
                *pp = n->next;
                if (n->next) n->next->prev = n->prev;
                n->next = to_fire;
                to_fire = n;
            } else {
                /* Still in the future (slot collision from earlier wrap) */
                pp = &n->next;
            }
        }
        pthread_mutex_unlock(&w->lock);

        /* Invoke callbacks outside the lock */
        while (to_fire) {
            TurTimerNode *next = to_fire->next;
            to_fire->callback(to_fire->arg);
            free(to_fire);
            to_fire = next;
        }
    }
}
