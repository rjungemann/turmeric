/* rt_alloc.h -- the runtime archive's allocator, as a hook.
 *
 * The TUs of libturt_runtime.a (and the same files when a stdlib
 * `__tur_autolink__` marker compiles them beside a program) used to call libc
 * directly.  That is the right default, and it stays the default: nothing
 * changes for the compiler, the interpreter, or an ordinary compiled program.
 *
 * The r7rs-gc experiment (docs/upcoming/r7rs-gc-plan.md) is the one consumer
 * that needs a say.  A compiled `#lang r7rs` program allocates from a
 * conservative collector that scans its own heap and the executable's data
 * segment -- and nothing else.  A HAMT node holding a Scheme value, or an
 * rc<T> block whose payload points at one, was allocated by these TUs with
 * libc, so the collector never saw the pointer and freed the value out from
 * under the map (the plan's first Limit).  Through this hook the collector
 * installs its own entry points at program start, and every block these TUs
 * allocate is a collected object: scanned while reachable, reclaimed after.
 *
 * Hooked: hamt.c, rc.c, gc.c, rc_free_queue.c, tur_string.c, symbols.c --
 * every archive TU whose blocks can hold a program word or be handed to the
 * program to free.  NOT hooked: region.c/arena.c (a generation's used bytes
 * are roots in their own right, tur_region_each_used) and trail.c (its arrays
 * hang off `__thread` variables, which are not in the scanned data segment;
 * moving them onto the collected heap would lose them).
 *
 * The hooked TUs redirect the four names with object-like macros right after
 * their includes, so a `free` passed as a drop function is the hook's too.
 * The functions here are real functions, never macros: a caller in the
 * emitted unit sees them as ordinary externs. */
#ifndef TUR_RT_ALLOC_H
#define TUR_RT_ALLOC_H

#include <stddef.h>

#ifndef TUR_RT_API
#define TUR_RT_API
#endif

typedef struct tur_rt_allocator {
    void *(*malloc)(size_t n);
    void *(*calloc)(size_t n, size_t m);
    void *(*realloc)(void *p, size_t n);
    void  (*free)(void *p);
} tur_rt_allocator;

/* Install `a` (copied) as the archive's allocator; NULL restores libc.  Call
 * it before the first allocation -- a block obtained from one allocator and
 * handed back to another is the caller's problem to avoid, except that the
 * collector's free forwards a block it does not own to libc. */
TUR_RT_API void tur_rt_set_allocator(const tur_rt_allocator *a);

TUR_RT_API void *tur_rt_malloc(size_t n);
TUR_RT_API void *tur_rt_calloc(size_t n, size_t m);
TUR_RT_API void *tur_rt_realloc(void *p, size_t n);
TUR_RT_API void  tur_rt_free(void *p);

#endif /* TUR_RT_ALLOC_H */
