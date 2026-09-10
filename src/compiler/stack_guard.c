/* stack_guard.c -- real C-stack headroom.  See stack_guard.h for why the
 * compiler's depth counters need this as a second trigger. */
/* For pthread_getattr_np (the glibc branch of tur_stack_headroom below);
 * must precede every include so <pthread.h> sees it wherever it is first
 * pulled in. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "stack_guard.h"

#include "../runtime/platform.h"   /* TUR_THREAD_LOCAL */

#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

/* Approximate the REAL stack pointer.  The obvious probe -- the address of a
 * local -- is wrong under ASan: with use-after-return detection (default in
 * modern toolchains) address-taken locals live on the sanitizer's heap-side
 * FAKE stack, so their addresses fall outside the thread stack entirely and
 * say nothing about real consumption.  The SP register itself still tracks
 * the real stack (it is the real stack that overflows), so read it directly
 * on the architectures we build for and fall back to the local's address
 * (correct whenever no fake stack is in play) elsewhere. */
static uintptr_t tur_approx_sp(void) {
#if defined(__GNUC__) && defined(__x86_64__)
    uintptr_t sp; __asm__ ("movq %%rsp, %0" : "=r"(sp)); return sp;
#elif defined(__GNUC__) && defined(__aarch64__)
    uintptr_t sp; __asm__ ("mov %0, sp" : "=r"(sp)); return sp;
#elif defined(__GNUC__) && defined(__i386__)
    uintptr_t sp; __asm__ ("movl %%esp, %0" : "=r"(sp)); return sp;
#else
    volatile char probe = 0;
    return (uintptr_t)&probe;
#endif
}

/* Query the calling thread's stack bounds.  `*lo_out` is the LOW end and
 * `*total_out` the size; returns false when the platform cannot tell us. */
static bool tur_stack_bounds(uintptr_t *lo_out, size_t *total_out) {
#if defined(_WIN32)
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    if (hi <= lo) return false;
    *lo_out = (uintptr_t)lo;
    *total_out = (size_t)(hi - lo);
    return true;
#elif defined(__APPLE__)
    pthread_t self = pthread_self();
    uintptr_t hi = (uintptr_t)pthread_get_stackaddr_np(self); /* HIGH end */
    size_t size = pthread_get_stacksize_np(self);
    if (!hi || !size) return false;
    *lo_out = hi - size;
    *total_out = size;
    return true;
#elif defined(__GLIBC__)
    /* glibc reports the main thread's stack from RLIMIT_STACK + /proc maps,
     * so a `ulimit -s` shrink is seen too. */
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return false;
    void *lo_addr = NULL; size_t size = 0;
    int rc = pthread_attr_getstack(&attr, &lo_addr, &size);
    pthread_attr_destroy(&attr);
    if (rc != 0 || !lo_addr || size == 0) return false;
    *lo_out = (uintptr_t)lo_addr;
    *total_out = size;
    return true;
#else
    (void)lo_out; (void)total_out;
    return false;
#endif
}

/* The bounds are fixed for the life of a thread, and the queries above are not
 * all cheap -- glibc's `pthread_getattr_np` opens and parses /proc/self/maps
 * for the main thread.  The callers ask per recursion level, so cache per
 * thread and leave the hot path at "read SP, subtract".  0 = not yet queried,
 * SIZE_MAX in `total` = queried and unavailable. */
static TUR_THREAD_LOCAL uintptr_t tls_stack_lo;
static TUR_THREAD_LOCAL size_t    tls_stack_total;

size_t tur_stack_headroom(size_t *total_out) {
    if (total_out) *total_out = 0;
    if (tls_stack_total == 0) {
        uintptr_t lo = 0; size_t total = 0;
        if (tur_stack_bounds(&lo, &total) && total) {
            tls_stack_lo = lo;
            tls_stack_total = total;
        } else {
            tls_stack_total = SIZE_MAX;
        }
    }
    if (tls_stack_total == SIZE_MAX) return SIZE_MAX;
    uintptr_t sp = tur_approx_sp();
    if (sp <= tls_stack_lo || sp > tls_stack_lo + tls_stack_total) return SIZE_MAX;
    if (total_out) *total_out = tls_stack_total;
    return (size_t)(sp - tls_stack_lo);
}

bool tur_stack_nearly_exhausted(void) {
    size_t total = 0;
    size_t headroom = tur_stack_headroom(&total);
    if (headroom == SIZE_MAX) return false;   /* unknown: depth counter only */
    size_t margin = total ? total / 8 : (size_t)1 << 20;
    if (margin > ((size_t)1 << 20)) margin = (size_t)1 << 20;
    if (margin < ((size_t)256 << 10)) margin = (size_t)256 << 10;
    return headroom < margin;
}
