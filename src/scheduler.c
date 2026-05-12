#include "scheduler.h"
#include "fiber.h"
#include "atomic_queue.h"
#include "io.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Phase T23: Multi-threaded work-stealing scheduler implementation */
/* SCH-003: I/O integration */

/* Per-thread work deque using a lock-free ring buffer */
struct WorkStealingDeque {
    FiberBlock **buffer;
    size_t capacity;
    size_t mask;
    size_t top;
    size_t bottom;
    pthread_spinlock_t lock;  /* For v1, use spinlock; upgrade to lock-free later */
};

/* Multi-threaded scheduler */
/* SCH-003: Added IOBackend for I/O event integration */
struct TurSchedulerMT {
    size_t n_threads;
    pthread_t *threads;
    WorkStealingDeque **deques;  /* Per-thread deques */
    AtomicQueue *global_queue;    /* For cross-thread submission */
    bool running;
    bool should_stop;
    pthread_mutex_t stop_lock;
    pthread_cond_t stop_cond;
    
    /* I/O integration */
    IOBackend *io_backend;       /* Platform-specific I/O event backend */
    pthread_mutex_t io_lock;     /* Protects io_waiters list */
    struct IOWaiter *io_waiters; /* List of fibers waiting on I/O */
};

/* I/O waiter structure - tracks fibers waiting on I/O events */
struct IOWaiter {
    FiberBlock *fiber;
    int fd;                     /* File descriptor being waited on */
    int events;                /* Events we're waiting for */
    void *user_data;           /* User callback data */
    struct IOWaiter *next;
};

/* Thread-local storage for current scheduler and thread ID */
static __thread TurSchedulerMT *tur_current_scheduler_mt = NULL;
static __thread size_t tur_current_thread_idx = 0;
static __thread int64_t tur_current_thread_id = 0;

/* Round up to next power of 2 */
static size_t next_power_of_2(size_t x) {
    if (x < 2) return 2;
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
#if SIZE_MAX > UINT32_MAX
    x |= x >> 32;
#endif
    return x + 1;
}

/* Work-stealing deque implementation */

WorkStealingDeque *ws_deque_new(size_t capacity) {
    capacity = next_power_of_2(capacity);
    
    WorkStealingDeque *d = (WorkStealingDeque *)calloc(1, sizeof(WorkStealingDeque));
    if (!d) return NULL;
    
    d->buffer = (FiberBlock **)calloc(capacity, sizeof(FiberBlock *));
    if (!d->buffer) { free(d); return NULL; }
    
    d->capacity = capacity;
    d->mask = capacity - 1;
    d->top = 0;
    d->bottom = 0;
    
    pthread_spin_init(&d->lock, PTHREAD_PROCESS_PRIVATE);
    
    return d;
}

void ws_deque_free(WorkStealingDeque *d) {
    if (!d) return;
    pthread_spin_destroy(&d->lock);
    free(d->buffer);
    free(d);
}

/* Push to the bottom (producer side) */
bool ws_deque_push(WorkStealingDeque *d, FiberBlock *f) {
    pthread_spin_lock(&d->lock);
    
    if (d->bottom - d->top >= d->capacity) {
        pthread_spin_unlock(&d->lock);
        return false;  /* Full */
    }
    
    d->buffer[d->bottom & d->mask] = f;
    d->bottom++;
    
    pthread_spin_unlock(&d->lock);
    return true;
}

/* Pop from the bottom (consumer side - same thread) */
FiberBlock *ws_deque_pop(WorkStealingDeque *d) {
    pthread_spin_lock(&d->lock);
    
    if (d->bottom == d->top) {
        pthread_spin_unlock(&d->lock);
        return NULL;  /* Empty */
    }
    
    d->bottom--;
    FiberBlock *f = d->buffer[d->bottom & d->mask];
    
    pthread_spin_unlock(&d->lock);
    return f;
}

/* Steal from the top (other threads) */
FiberBlock *ws_deque_steal(WorkStealingDeque *d) {
    pthread_spin_lock(&d->lock);
    
    if (d->bottom == d->top) {
        pthread_spin_unlock(&d->lock);
        return NULL;  /* Empty */
    }
    
    FiberBlock *f = d->buffer[d->top & d->mask];
    d->top++;
    
    pthread_spin_unlock(&d->lock);
    return f;
}

bool ws_deque_empty(WorkStealingDeque *d) {
    pthread_spin_lock(&d->lock);
    bool empty = (d->bottom == d->top);
    pthread_spin_unlock(&d->lock);
    return empty;
}

/* Scheduler worker thread function */
static void *scheduler_worker(void *arg) {
    TurSchedulerMT *s = (TurSchedulerMT *)arg;
    size_t thread_idx = tur_current_thread_idx;
    WorkStealingDeque *my_deque = s->deques[thread_idx];
    
    tur_current_scheduler_mt = s;
    tur_current_thread_idx = thread_idx;
    
    while (1) {
        /* Check if we should stop */
        {
            pthread_mutex_lock(&s->stop_lock);
            bool stop = s->should_stop;
            pthread_mutex_unlock(&s->stop_lock);
            if (stop) break;
        }
        
        /* Try to pop from our own deque first */
        FiberBlock *f = ws_deque_pop(my_deque);
        if (f) {
            /* Run the fiber */
            tur_fiber_block_resume(f, 0);
            continue;
        }
        
        /* Try to steal from other threads */
        bool found = false;
        for (size_t i = 0; i < s->n_threads; i++) {
            if (i == thread_idx) continue;
            f = ws_deque_steal(s->deques[i]);
            if (f) {
                found = true;
                break;
            }
        }
        
        if (found) {
            tur_fiber_block_resume(f, 0);
            continue;
        }
        
        /* Try the global queue */
        bool success;
        f = (FiberBlock *)aq_pop(s->global_queue, &success);
        if (success) {
            tur_fiber_block_resume(f, 0);
            continue;
        }
        
        /* No work - check for I/O events */
        /* SCH-003: Poll for I/O events with a timeout */
        scheduler_mt_poll_io(s, 1);  /* 1ms timeout */
        
        /* If still no work, yield */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000 };  /* 0.1ms */
        nanosleep(&ts, NULL);
    }
    
    return NULL;
}

TurSchedulerMT *tur_scheduler_mt_new(size_t n_threads) {
    if (n_threads == 0) n_threads = 1;
    
    TurSchedulerMT *s = (TurSchedulerMT *)calloc(1, sizeof(TurSchedulerMT));
    if (!s) return NULL;
    
    s->n_threads = n_threads;
    s->threads = (pthread_t *)malloc(n_threads * sizeof(pthread_t));
    if (!s->threads) { free(s); return NULL; }
    
    s->deques = (WorkStealingDeque **)malloc(n_threads * sizeof(WorkStealingDeque *));
    if (!s->deques) { free(s->threads); free(s); return NULL; }
    
    /* Create per-thread deques */
    for (size_t i = 0; i < n_threads; i++) {
        s->deques[i] = ws_deque_new(1024);  /* Default capacity */
        if (!s->deques[i]) {
            for (size_t j = 0; j < i; j++) ws_deque_free(s->deques[j]);
            free(s->deques);
            free(s->threads);
            free(s);
            return NULL;
        }
    }
    
    /* Create global queue for cross-thread submission */
    s->global_queue = aq_new(4096);
    if (!s->global_queue) {
        for (size_t i = 0; i < n_threads; i++) ws_deque_free(s->deques[i]);
        free(s->deques);
        free(s->threads);
        free(s);
        return NULL;
    }
    
    s->running = false;
    s->should_stop = false;
    pthread_mutex_init(&s->stop_lock, NULL);
    pthread_cond_init(&s->stop_cond, NULL);
    
    /* SCH-003: Initialize I/O backend */
    s->io_backend = io_backend_new();
    pthread_mutex_init(&s->io_lock, NULL);
    s->io_waiters = NULL;
    
    /* Create worker threads */
    for (size_t i = 0; i < n_threads; i++) {
        if (pthread_create(&s->threads[i], NULL, scheduler_worker, s) != 0) {
            s->should_stop = true;
            pthread_cond_broadcast(&s->stop_cond);
            for (size_t j = 0; j < i; j++) {
                pthread_join(s->threads[j], NULL);
            }
            aq_free(s->global_queue);
            for (size_t j = 0; j < n_threads; j++) ws_deque_free(s->deques[j]);
            free(s->deques);
            free(s->threads);
            free(s);
            return NULL;
        }
    }
    
    s->running = true;
    return s;
}

void tur_scheduler_mt_free(TurSchedulerMT *s) {
    if (!s) return;
    
    s->should_stop = true;
    pthread_cond_broadcast(&s->stop_cond);
    
    /* SCH-003: Wake I/O backend to unblock any waiting threads */
    if (s->io_backend) {
        io_wake(s->io_backend);
    }
    
    /* Join all worker threads */
    for (size_t i = 0; i < s->n_threads; i++) {
        pthread_join(s->threads[i], NULL);
    }
    
    /* SCH-003: Free I/O waiters list */
    pthread_mutex_lock(&s->io_lock);
    struct IOWaiter *waiter = s->io_waiters;
    while (waiter) {
        struct IOWaiter *next = waiter->next;
        free(waiter);
        waiter = next;
    }
    pthread_mutex_unlock(&s->io_lock);
    pthread_mutex_destroy(&s->io_lock);
    
    /* SCH-003: Free I/O backend */
    if (s->io_backend) {
        io_backend_free(s->io_backend);
    }
    
    aq_free(s->global_queue);
    for (size_t i = 0; i < s->n_threads; i++) {
        ws_deque_free(s->deques[i]);
    }
    free(s->deques);
    free(s->threads);
    pthread_mutex_destroy(&s->stop_lock);
    pthread_cond_destroy(&s->stop_cond);
    free(s);
}

TurSchedulerMT *tur_scheduler_mt_current(void) {
    return tur_current_scheduler_mt;
}

void tur_scheduler_mt_set_current(TurSchedulerMT *s) {
    tur_current_scheduler_mt = s;
}

void tur_scheduler_mt_spawn(TurSchedulerMT *s, FiberBlock *f) {
    if (!s || !f) return;
    
    size_t my_idx = tur_current_thread_idx;
    if (my_idx < s->n_threads) {
        /* Try to push to our thread's deque first */
        if (ws_deque_push(s->deques[my_idx], f)) {
            return;
        }
    }
    
    /* Fall back to global queue */
    aq_push(s->global_queue, (AtomicQueueItem)f);
}

void tur_scheduler_mt_run(TurSchedulerMT *s) {
    if (!s) return;
    tur_current_scheduler_mt = s;
    tur_current_thread_idx = 0;  /* Main thread is index 0 */
    
    /* Run the worker loop on the current thread */
    scheduler_worker(s);
}

void tur_scheduler_mt_run_to_completion(TurSchedulerMT *s) {
    if (!s) return;
    
    /* For v1, this just runs until the global queue is empty */
    /* In a proper implementation, we'd need to track in-flight fibers */
    tur_scheduler_mt_run(s);
}

void tur_scheduler_mt_yield(void) {
    /* For v1, yield just returns; the worker loop will pick up the next fiber */
    /* True yield requires saving the current fiber's state */
}

void tur_scheduler_mt_park(void) {
    /* For v1, park is a no-op */
}

void tur_scheduler_mt_unpark(FiberBlock *f) {
    TurSchedulerMT *s = tur_current_scheduler_mt;
    if (s) {
        tur_scheduler_mt_spawn(s, f);
    }
}

int64_t tur_scheduler_mt_thread_id(void) {
    if (tur_current_thread_id == 0) {
        tur_current_thread_id = (int64_t)(uintptr_t)pthread_self();
    }
    return tur_current_thread_id;
}

/* SCH-003: I/O integration functions */

/* I/O callback that unparks a waiting fiber */
static void scheduler_mt_io_callback(int fd, int events, void *user_data) {
    TurSchedulerMT *s = tur_current_scheduler_mt;
    if (!s) return;
    
    /* Find and wake up any fibers waiting on this fd */
    pthread_mutex_lock(&s->io_lock);
    struct IOWaiter **waiter_p = &s->io_waiters;
    
    while (*waiter_p) {
        struct IOWaiter *waiter = *waiter_p;
        if (waiter->fd == fd && (waiter->events & events)) {
            /* Found a matching waiter - unpark its fiber */
            *waiter_p = waiter->next;
            pthread_mutex_unlock(&s->io_lock);
            
            /* Unpark the fiber so it can resume */
            tur_scheduler_mt_unpark(waiter->fiber);
            free(waiter);
            return;
        }
        waiter_p = &(*waiter_p)->next;
    }
    pthread_mutex_unlock(&s->io_lock);
}

/* Register a fiber to wait on I/O events for a file descriptor */
void tur_scheduler_mt_io_wait(TurSchedulerMT *s, int fd, int events, FiberBlock *f, void *user_data) {
    if (!s || !s->io_backend || fd < 0) return;
    
    /* Create an I/O waiter */
    struct IOWaiter *waiter = (struct IOWaiter *)malloc(sizeof(struct IOWaiter));
    if (!waiter) return;
    
    waiter->fiber = f;
    waiter->fd = fd;
    waiter->events = events;
    waiter->user_data = user_data;
    waiter->next = NULL;
    
    /* Register with I/O backend */
    int rc = io_register(s->io_backend, fd, events, scheduler_mt_io_callback, s);
    if (rc != 0) {
        free(waiter);
        return;
    }
    
    /* Add to our waiters list */
    pthread_mutex_lock(&s->io_lock);
    waiter->next = s->io_waiters;
    s->io_waiters = waiter;
    pthread_mutex_unlock(&s->io_lock);
    
    /* Park the fiber - it will be unparked when I/O event occurs */
    tur_scheduler_mt_park();
}

/* Modify I/O interest for a file descriptor */
void tur_scheduler_mt_io_modify(TurSchedulerMT *s, int fd, int events) {
    if (!s || !s->io_backend || fd < 0) return;
    io_modify(s->io_backend, fd, events);
}

/* Unregister I/O interest for a file descriptor */
void tur_scheduler_mt_io_unregister(TurSchedulerMT *s, int fd) {
    if (!s || !s->io_backend || fd < 0) return;
    
    /* Remove from our waiters list */
    pthread_mutex_lock(&s->io_lock);
    struct IOWaiter **waiter_p = &s->io_waiters;
    while (*waiter_p) {
        struct IOWaiter *waiter = *waiter_p;
        if (waiter->fd == fd) {
            *waiter_p = waiter->next;
            free(waiter);
            break;
        }
        waiter_p = &(*waiter_p)->next;
    }
    pthread_mutex_unlock(&s->io_lock);
    
    io_unregister(s->io_backend, fd);
}

/* Poll for I/O events and process them */
static void scheduler_mt_poll_io(TurSchedulerMT *s, int timeout_ms) {
    if (!s || !s->io_backend) return;
    io_poll(s->io_backend, timeout_ms);
}

/* SCH-003: ThreadPool integration */
/* Mapping from ThreadPool to TurSchedulerMT */
/* For now, we use a simple approach: store the scheduler pointer in the ThreadPool */

/* These functions are placeholders for the integration */
/* The full integration would require modifying threadpool.tur to use the scheduler */

TurSchedulerMT *tur_scheduler_mt_from_threadpool(void *threadpool) {
    /* In a full integration, ThreadPoolBlock would contain a TurSchedulerMT pointer */
    /* For now, return the current scheduler */
    (void)threadpool;
    return tur_current_scheduler_mt;
}

void tur_scheduler_mt_set_for_threadpool(void *threadpool, TurSchedulerMT *s) {
    /* In a full integration, we would store s in threadpool */
    (void)threadpool;
    tur_current_scheduler_mt = s;
}
