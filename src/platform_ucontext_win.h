/* platform_ucontext_win.h - ucontext_t implemented over Win32 Fibers.
 *
 * MinGW has no <ucontext.h>.  Win32 Fibers are the genuine equivalent, not an
 * approximation: both are cooperative, manually-scheduled execution contexts
 * that switch only when explicitly told to.  So this is a real implementation
 * rather than a stub -- which matters, because eval.c uses these contexts for
 * generators and effect handlers, not just async.  Stubbing them out would
 * silently remove language features on Windows.
 *
 * The mapping is close to 1:1 because of one lucky property of the call sites:
 * every makecontext() in this codebase passes argc == 0 and threads real
 * arguments through a side channel (POSIX makecontext can only pass ints, so
 * the code already avoids its varargs).  That removes the only genuinely
 * awkward part of emulating makecontext.
 *
 * Known divergences, all benign here:
 *
 *   - CreateFiber allocates its own stack, so the caller's uc_stack.ss_sp
 *     buffer goes unused (we honour ss_size as the requested stack size).  It
 *     is wasted memory, not a correctness problem.
 *   - There is no DeleteFiber call: POSIX ucontext has no destructor, so there
 *     is no hook to hang one on, and each makecontext'd context therefore leaks
 *     one Win32 fiber.  Acceptable for the interpreter (which already keeps its
 *     closures for process lifetime) but worth revisiting if fibers are ever
 *     created in a hot loop.
 *
 * See docs/archive/windows-support-plan.md.
 */

#ifndef TUR_PLATFORM_UCONTEXT_WIN_H
#define TUR_PLATFORM_UCONTEXT_WIN_H

#ifdef _WIN32

/* Keep windows.h from stomping on the codebase: NOGDI drops wingdi.h, which
 * defines a bare `ERROR` macro that collides with diag.h's severity enum, and
 * NOMINMAX drops the min/max function-like macros. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TURI_UCONTEXT_STUB_DEFINED
#define TURI_UCONTEXT_STUB_DEFINED

/* Default when a caller leaves uc_stack.ss_size unset. */
#ifndef TURI_WIN_FIBER_STACK_SIZE
#define TURI_WIN_FIBER_STACK_SIZE 262144
#endif

typedef struct {
    void  *ss_sp;
    size_t ss_size;
} turi_stack_t;

typedef struct tur_ucontext {
    LPVOID                fiber;    /* Win32 fiber handle */
    void                (*entry)(void);
    struct tur_ucontext  *uc_link;
    turi_stack_t          uc_stack;
} ucontext_t;

/* Make the calling thread fiber-capable.  ConvertThreadToFiber fails if the
 * thread already is one, so the check is not merely an optimisation. */
static inline int turi_win_ensure_fiber(void) {
    if (!IsThreadAFiber()) {
        if (ConvertThreadToFiber(NULL) == NULL) {
            return -1;
        }
    }
    return 0;
}

static VOID WINAPI turi_win_fiber_trampoline(LPVOID param) {
    struct tur_ucontext *u = (struct tur_ucontext *)param;
    if (u->entry) {
        u->entry();
    }
    /*
     * Under POSIX, a makecontext'd entry that returns resumes uc_link, or ends
     * the thread when uc_link is NULL.  Every thunk here swapcontext()s away
     * instead of returning, so reaching this point means a bug -- and on Win32,
     * falling off the end of a fiber silently terminates the whole thread,
     * which would surface as a mystery hang.  Fail loudly instead.
     */
    if (u->uc_link != NULL && u->uc_link->fiber != NULL) {
        SwitchToFiber(u->uc_link->fiber);
        return;
    }
    fprintf(stderr, "turmeric: fiber entry returned with no uc_link -- "
                    "this would terminate the thread.\n");
    abort();
}

/* Capture the current execution context so swapcontext() can return to it. */
static inline int turi_getcontext(ucontext_t *u) {
    memset(u, 0, sizeof(*u));
    if (turi_win_ensure_fiber() != 0) {
        return -1;
    }
    u->fiber = GetCurrentFiber();
    return 0;
}

/*
 * Prepare `u` to run `fn` on its own stack.
 *
 * argc is accepted and ignored: every call site passes 0 (see the file header).
 * A non-zero argc would silently drop arguments, so assert on it rather than
 * quietly misbehave.
 */
static inline void turi_makecontext(ucontext_t *u, void (*fn)(void), int argc) {
    if (argc != 0) {
        fprintf(stderr, "turmeric: makecontext with argc=%d is unsupported on "
                        "Windows (pass arguments via a side channel).\n", argc);
        abort();
    }
    if (turi_win_ensure_fiber() != 0) {
        u->fiber = NULL;
        return;
    }
    u->entry = fn;
    SIZE_T stack_size = (SIZE_T)(u->uc_stack.ss_size != 0
                                     ? u->uc_stack.ss_size
                                     : TURI_WIN_FIBER_STACK_SIZE);
    u->fiber = CreateFiber(stack_size, turi_win_fiber_trampoline, u);
}

/* Save the running context into `from`, then switch to `to`. */
static inline int turi_swapcontext(ucontext_t *from, ucontext_t *to) {
    if (turi_win_ensure_fiber() != 0) {
        return -1;
    }
    if (to == NULL || to->fiber == NULL) {
        return -1;
    }
    from->fiber = GetCurrentFiber();
    SwitchToFiber(to->fiber);
    return 0;
}

#define getcontext(u)             turi_getcontext(u)
#define swapcontext(from, to)     turi_swapcontext((from), (to))
#define makecontext(u, fn, argc)  turi_makecontext((u), (fn), (argc))

#endif /* TURI_UCONTEXT_STUB_DEFINED */

#endif /* _WIN32 */

#endif /* TUR_PLATFORM_UCONTEXT_WIN_H */
