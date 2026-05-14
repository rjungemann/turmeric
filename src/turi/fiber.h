/* Phase S7: Async fiber scheduler, futures, and timers for the Turmeric eval
 * runtime (libturi).
 *
 * Design:
 *  - Each (async fn-expr) creates a TuriFiber that evaluates fn-expr in a
 *    new ucontext stack.  The fiber returns a TuriFuture handle.
 *  - The event loop (turi_run_event_loop) dequeues ready fibers and resumes
 *    them via swapcontext until all fibers complete or the queue is empty.
 *  - (await fut) suspends the current fiber until the future resolves; if
 *    called from the main (non-fiber) context it runs the event loop inline.
 *  - Timers use CLOCK_MONOTONIC and a sorted singly-linked list; polled each
 *    event-loop tick before dequeuing the next fiber.
 *  - Non-blocking I/O uses poll(2); pending fd entries are registered via
 *    turi_io_pending_add and checked each scheduler tick.
 */

#ifndef TURI_FIBER_H
#define TURI_FIBER_H

/* Platform macros must come before system headers */
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
#endif
#if defined(__APPLE__)
#  ifndef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE
#  endif
#  ifndef _XOPEN_SOURCE
#    define _XOPEN_SOURCE 700
#  endif
#else
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include <stdbool.h>
#include <stdint.h>

#ifndef __EMSCRIPTEN__
#  if defined(__APPLE__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
#  endif
#  include <ucontext.h>
#  if defined(__APPLE__)
#    pragma clang diagnostic pop
#  endif
#else
/* WASM: ucontext.h is not in Emscripten's sysroot.
 * Use Emscripten Fibers for cooperative coroutines.  Requires -sASYNCIFY=1. */
#  include <emscripten/fiber.h>
#  ifndef TURI_ASYNCIFY_STACK_SIZE
#    define TURI_ASYNCIFY_STACK_SIZE 65536
#  endif
#  ifndef TURI_UCONTEXT_STUB_DEFINED
#    define TURI_UCONTEXT_STUB_DEFINED
typedef struct {
    emscripten_fiber_t fiber;
    char asyncify_stack[TURI_ASYNCIFY_STACK_SIZE];
} ucontext_t;
#  endif
/* Capture the current execution context so it can be resumed by swapcontext. */
static inline int getcontext(ucontext_t *u) {
    emscripten_fiber_init_from_current_context(&u->fiber, u->asyncify_stack,
                                               TURI_ASYNCIFY_STACK_SIZE);
    return 0;
}
/* Save current state into 'from' and switch execution to 'to'. */
static inline int swapcontext(ucontext_t *from, ucontext_t *to) {
    emscripten_fiber_swap(&from->fiber, &to->fiber);
    return 0;
}
/* makecontext is replaced by a direct emscripten_fiber_init call in eval.c
 * for the body fiber; keep the no-op for the async-fiber path (unused in WASM). */
#define makecontext(u, fn, argc, ...) ((void)(u), (void)(fn), (void)(argc))
#endif

#include "value.h"

/* Forward declarations */
typedef struct TuriEnv    TuriEnv;    /* from env.h — do not include here */
typedef struct TuriFiber  TuriFiber;
typedef struct TuriFuture TuriFuture;
typedef struct TuriTimer  TuriTimer;
typedef struct TuriIoPending TuriIoPending;

/* -------------------------------------------------------------------------
 * TuriFuture
 * ---------------------------------------------------------------------- */

typedef enum {
    TURI_FUTURE_PENDING = 0,
    TURI_FUTURE_RESOLVED,
    TURI_FUTURE_REJECTED,
} TuriFutureState;

typedef struct TuriWaker {
    TuriFiber       *fiber;
    struct TuriWaker *next;
} TuriWaker;

struct TuriFuture {
    TuriFutureState  state;
    TuriValue        result;    /* resolved value or rejection error */
    TuriWaker       *wakers;   /* fibers waiting on this future */
    TuriFiber       *owner;    /* fiber that produces this future (or NULL) */
    bool             reported; /* unhandled-error callback already fired? */
    TuriFuture      *next_alloc; /* intrusive list in TuriEnv->all_futures */
};

/* Constructors */
static inline TuriValue turi_future_val(TuriFuture *f) {
    return (TuriValue){ TURI_FUTURE, .as_future = f };
}
static inline bool turi_is_future(TuriValue v) { return v.tag == TURI_FUTURE; }
static inline TuriFuture *turi_as_future(TuriValue v) {
    return (v.tag == TURI_FUTURE) ? v.as_future : NULL;
}

/* -------------------------------------------------------------------------
 * TuriFiber
 * ---------------------------------------------------------------------- */

typedef enum {
    TURI_FIBER_READY = 0,
    TURI_FIBER_RUNNING,
    TURI_FIBER_SUSPENDED,
    TURI_FIBER_DONE,
    TURI_FIBER_CANCELLED,
} TuriFiberState;

#define TURI_ASYNC_STACK_SIZE (512 * 1024) /* 512 KB per async fiber */

struct TuriFiber {
    ucontext_t       ctx;
    char            *stack;
    TuriFiberState   state;
    bool             cancelled;
    TuriFuture      *own_future;       /* future this fiber resolves/rejects */
    TuriFuture      *awaiting_future;  /* future this fiber is blocked on */
    TuriFiber       *sched_next;       /* intrusive linked list for scheduler */
    /* Eval context */
    TuriEnv         *env;
    TuriValue        fn_closure_val; /* pre-evaluated closure to call */
};

/* -------------------------------------------------------------------------
 * TuriTimer — min-heap entry for sleep-async / with-timeout
 * ---------------------------------------------------------------------- */

struct TuriTimer {
    uint64_t      deadline_ms;
    TuriFuture   *future;        /* future to resolve when timer fires */
    TuriTimer    *next;          /* sorted linked list (ascending deadline) */
};

/* -------------------------------------------------------------------------
 * TuriIoPending — non-blocking I/O registration for S7.7
 * ---------------------------------------------------------------------- */

#define TURI_IO_READ  1
#define TURI_IO_WRITE 2

struct TuriIoPending {
    int             fd;
    int             op;          /* TURI_IO_READ or TURI_IO_WRITE */
    TuriFuture     *future;
    TuriFiber      *fiber;
    /* For read: buffer info */
    char           *read_buf;
    int             read_len;
    /* For write: remaining data */
    char           *write_buf;
    int             write_len;
    int             write_cap;
    TuriIoPending  *next;
};

/* -------------------------------------------------------------------------
 * Scheduler functions (called from eval.c and public API)
 * ---------------------------------------------------------------------- */

/* Current wall-clock time in milliseconds (CLOCK_MONOTONIC). */
uint64_t turi_now_ms(void);

/* Allocate a new pending future (heap-allocated, owned by TuriEnv until freed). */
TuriFuture *turi_future_new(TuriEnv *env);

/* Settle a future, waking all registered fiber-wakers. */
void turi_future_resolve(TuriEnv *env, TuriFuture *f, TuriValue result);
void turi_future_reject(TuriEnv *env, TuriFuture *f, TuriValue error);

/* Register a fiber as a waker on a future. */
void turi_future_add_waker(TuriFuture *f, TuriFiber *fiber);

/* Add a fiber to the ready queue. */
void turi_sched_enqueue(TuriEnv *env, TuriFiber *fiber);

/* Run the event loop until all fibers and timers complete. */
void turi_run_event_loop(TuriEnv *env);

/* Run the event loop until the given future resolves or rejects. */
TuriValue turi_await_future(TuriEnv *env, TuriFuture *f);

/* Schedule a timer: resolve `future` after `ms` milliseconds. */
void turi_timer_add(TuriEnv *env, uint64_t ms, TuriFuture *future);

/* Cancel a task future (marks owner fiber as cancelled and rejects future). */
void turi_task_cancel(TuriEnv *env, TuriFuture *f);

/* Check if current fiber is cancelled (safe to call from any context). */
bool turi_fiber_is_cancelled(TuriEnv *env);

/* Register a non-blocking read request. Resolves future with bytes read (int). */
void turi_io_read_async(TuriEnv *env, int fd, int n, TuriFuture *future,
                        TuriFiber *fiber);

/* Register a non-blocking write request. Resolves future with bytes written (int). */
void turi_io_write_async(TuriEnv *env, int fd, const char *data, int len,
                         TuriFuture *future, TuriFiber *fiber);

/* Initialise scheduler state in env (called by turi_env_new). */
void turi_sched_init(TuriEnv *env);

/* Free all scheduler state (called by turi_env_free). */
void turi_sched_free(TuriEnv *env);

/* Register native async builtins into env (called by turi_env_new after sched_init). */
void turi_async_register_builtins(TuriEnv *env);

/* -------------------------------------------------------------------------
 * Public C API (from eval.h / used by embedders)
 * ---------------------------------------------------------------------- */

/* Spawn a fiber from a source-code string; returns TURI_FUTURE value. */
TuriValue turi_task_spawn(TuriEnv *env, const char *src);

/* Poll a future: returns result/error, or TURI_NIL if still pending. */
TuriValue turi_future_poll_val(TuriFuture *f);

/* Start an async sleep; returns TURI_FUTURE that resolves to nil. */
TuriValue turi_sleep_async(TuriEnv *env, uint64_t ms);

/* Thread-local fiber pointer: set by the scheduler immediately before the
 * first swapcontext into a fiber, read once by async_fiber_thunk (eval.c).
 * Defined in eval.c; extern here so fiber.c can set it before each resume. */
extern _Thread_local TuriFiber *g_pending_async_fiber;

#endif /* TURI_FIBER_H */
