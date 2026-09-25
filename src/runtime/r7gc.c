/* r7gc.c -- the r7rs-gc experiment: a conservative mark-sweep collector for a
 * compiled `#lang r7rs` program (docs/upcoming/r7rs-gc-plan.md).
 *
 * Why it exists: a Scheme value is a `:heap` box, and the memory model never
 * frees one (docs/guides/gc-guide.md), so a Scheme program's memory only grows
 * (docs/reported/r7rs-heap-data-never-reclaimed.md).  Shared, mutable, cyclic
 * data is the case only a tracing collector handles well.
 *
 * How it plugs in: the emitter pastes this file into the program's
 * translation unit, ahead of everything else in the preamble, and then
 * redirects that unit's allocator -- `malloc`, `calloc`, `realloc`, `free`,
 * `strdup`, `strndup`, and the region fallbacks -- to the `tur_gc_*` entry
 * points below (emit_module.c, emit_r7rs_gc_prologue).  Every object the
 * program itself allocates -- Scheme data, closure environments, DK frames,
 * continuation images, vector buffers, the prelude's C -- lands on this heap.
 *
 * Conservative: any aligned word that points into an allocated object (its
 * first byte or any byte inside it) keeps the object alive.  The roots are
 *   - the C stack from the collector's frame to the thread's stack base, with
 *     the callee-saved registers spilled into a jmp_buf on that stack;
 *   - the executable's writable data and bss (`__data_start` .. `_end`),
 *     which includes the emitted runtime's per-thread state, because the
 *     emitter makes TUR_THREAD_LOCAL plain static storage when the collector
 *     is on (the collector is single-threaded);
 *   - the used bytes of every live and retired region generation
 *     (tur_region_each_used), since a node built in a bracket can point here.
 * Objects are scanned whole, word by word.
 *
 * The runtime archive (libturt_runtime.a: the HAMT, rc<T>, strings, symbols)
 * allocates through the hook in src/runtime/rt_alloc.h, which the
 * constructor below points at this heap, so a Scheme value kept in a
 * Turmeric map or cell is scanned through the node that holds it.  What it
 * still cannot see, and so must not be relied on (the plan's limits): memory
 * libc allocates, the trail's `__thread`-rooted arrays (trail.c), and any
 * other thread.  A pure Scheme program allocates nothing there that points
 * back.
 *
 * Linux/glibc and macOS.  Elsewhere every entry point is the libc call it
 * replaces, and nothing is collected.
 *
 * Knobs (environment, read once):
 *   TUR_GC_STATS=1       print collections, bytes freed, the live size at the
 *                        last collection, and the heap now and at its peak.
 *   TUR_GC_TORTURE=N     collect on every Nth allocation (1 = every one): the
 *                        test mode that turns a missing root into a crash.
 *   TUR_GC_THRESHOLD=B   bytes allocated between collections (floor; default
 *                        8 MiB, raised to the live size after each one). */

#pragma GCC diagnostic ignored "-Wunused-function"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>

/* Off under `tur jit` (TUR_JIT_ENGINE comes from the engine's prelude): a
 * JIT'd program's globals live in memory MIR allocates, not in the
 * executable's data segment, so the root scan would miss them and free live
 * objects.  The entry points stay; they are libc there. */
#if ((defined(__linux__) && defined(__GLIBC__)) || defined(__APPLE__)) && !defined(TUR_JIT_ENGINE)
#define TUR_GC_ON 1
#include <sys/mman.h>
#include <pthread.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#if defined(__APPLE__)
/* The data roots are the main image's writable segments, walked from its
 * Mach-O header (there is no __data_start/_end pair to bracket them). */
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/vm_prot.h>
#endif
#else
#define TUR_GC_ON 0
#endif

#if defined(__SANITIZE_ADDRESS__)
#define TUR_GC_NOASAN __attribute__((no_sanitize_address))
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TUR_GC_NOASAN __attribute__((no_sanitize_address))
#endif
#endif
#ifndef TUR_GC_NOASAN
#define TUR_GC_NOASAN
#endif

#if TUR_GC_ON

#define TUR_GC_CHUNK   ((uintptr_t)1 << 16)          /* 64 KiB, aligned */
#define TUR_GC_MAXSMALL 32768
#define TUR_GC_NCLASS  36

/* Up to half a chunk.  The large classes matter for continuations: a stack
 * image is a few KiB, and as a large object each one cost a 64 KiB mapping. */
static const uint32_t tur_gc_class_size[TUR_GC_NCLASS] = {
    16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384,
    448, 512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048, 2560, 3072,
    3584, 4096, 5120, 6144, 8192, 10912, 13104, 16384, 21840, 32768
};

/* One chunk (small objects of one size) or one large object (one or more
 * chunks).  Descriptors and bitmaps live in mmap'd metadata, never on the
 * collected heap and never in the data segment the collector scans. */
typedef struct tur_gc_page {
    uintptr_t base;
    size_t    size;       /* slot size, or the large object's byte size */
    uint32_t  nslots;     /* 1 for a large object */
    uint32_t  nchunks;    /* 1 for a small page */
    uint32_t  cls;        /* size class, or TUR_GC_NCLASS for large */
    uint32_t  used;       /* slots handed out so far (bump) */
    uint64_t *alloc;      /* bit per slot: allocated */
    uint64_t *mark;       /* bit per slot: reached this collection */
    struct tur_gc_page *next;   /* every page, for the sweep */
    bool      dead;       /* a released large object's descriptor */
} tur_gc_page;

typedef struct tur_gc_state {
    bool       ready, off;
    uintptr_t  lo, hi;                 /* heap bounds, for a fast reject */
    tur_gc_page *pages;
    /* chunk index (base >> 16) -> page, open addressing, power-of-two size */
    uintptr_t *map_key;
    tur_gc_page **map_val;
    size_t     map_cap, map_n;
    void      *freelist[TUR_GC_NCLASS];
    tur_gc_page *bump[TUR_GC_NCLASS];  /* the page being carved for a class */
    /* metadata bump allocator */
    unsigned char *meta, *meta_end;
    /* mark stack */
    uintptr_t *mstack;
    size_t     mstack_n, mstack_cap;
    /* accounting */
    size_t     since, threshold, floor, live, heap_bytes, heap_peak;
    size_t     n_collect, freed_total;
    unsigned long torture, torture_count;
    bool       stats;
    unsigned char *stack_base;
    bool       collecting;
} tur_gc_state;

static tur_gc_state *tur_gc_G;          /* points into mmap'd metadata */

static void *tur_gc_os(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { fputs("tur: r7rs-gc: out of memory\n", stderr); abort(); }
    return p;
}

static void *tur_gc_meta(size_t n) {
    tur_gc_state *G = tur_gc_G;
    n = (n + 15) & ~(size_t)15;
    if (!G->meta || (size_t)(G->meta_end - G->meta) < n) {
        size_t sz = n > (1u << 20) ? n : (1u << 20);
        G->meta = (unsigned char *)tur_gc_os(sz);
        G->meta_end = G->meta + sz;
    }
    void *p = G->meta;
    G->meta += n;
    return p;
}

/* A chunk-aligned run of `n` chunks. */
static uintptr_t tur_gc_chunks(size_t n) {
    size_t len = n * TUR_GC_CHUNK;
    unsigned char *raw = (unsigned char *)tur_gc_os(len + TUR_GC_CHUNK);
    uintptr_t base = ((uintptr_t)raw + TUR_GC_CHUNK - 1) & ~(TUR_GC_CHUNK - 1);
    size_t head = base - (uintptr_t)raw;
    if (head) munmap(raw, head);
    size_t tail = TUR_GC_CHUNK - head;
    if (tail) munmap((void *)(base + len), tail);
    return base;
}

static size_t tur_gc_hash(uintptr_t k, size_t cap) {
    k ^= k >> 17; k *= 0x9E3779B97F4A7C15ull; k ^= k >> 29;
    return (size_t)k & (cap - 1);
}

static void tur_gc_map_put(uintptr_t key, tur_gc_page *pg);
static void tur_gc_map_grow(void) {
    tur_gc_state *G = tur_gc_G;
    size_t oc = G->map_cap, nc = oc ? oc * 2 : 1024;
    uintptr_t *ok = G->map_key; tur_gc_page **ov = G->map_val;
    G->map_key = (uintptr_t *)tur_gc_os(nc * sizeof(uintptr_t));
    G->map_val = (tur_gc_page **)tur_gc_os(nc * sizeof(tur_gc_page *));
    G->map_cap = nc; G->map_n = 0;
    for (size_t i = 0; i < oc; i++) if (ok[i] && ov[i]) tur_gc_map_put(ok[i], ov[i]);
    if (ok) { munmap(ok, oc * sizeof(uintptr_t)); munmap(ov, oc * sizeof(tur_gc_page *)); }
}
static void tur_gc_map_put(uintptr_t key, tur_gc_page *pg) {
    tur_gc_state *G = tur_gc_G;
    if ((G->map_n + 1) * 2 > G->map_cap) tur_gc_map_grow();
    size_t i = tur_gc_hash(key, G->map_cap);
    while (G->map_key[i] && G->map_key[i] != key) i = (i + 1) & (G->map_cap - 1);
    if (!G->map_key[i]) G->map_n++;
    G->map_key[i] = key;
    G->map_val[i] = pg;
}
static tur_gc_page *tur_gc_map_get(uintptr_t key) {
    tur_gc_state *G = tur_gc_G;
    if (!G->map_cap) return NULL;
    size_t i = tur_gc_hash(key, G->map_cap);
    while (G->map_key[i]) {
        if (G->map_key[i] == key) return G->map_val[i];
        i = (i + 1) & (G->map_cap - 1);
    }
    return NULL;
}

static void tur_gc_init(void) {
    tur_gc_state *G = (tur_gc_state *)tur_gc_os(sizeof(tur_gc_state));
    memset(G, 0, sizeof *G);
    tur_gc_G = G;
    const char *e;
    G->floor = (size_t)8 << 20;
    if ((e = getenv("TUR_GC_THRESHOLD")) && atol(e) > 0) G->floor = (size_t)atol(e);
    G->threshold = G->floor;
    if ((e = getenv("TUR_GC_TORTURE")) && atol(e) > 0) G->torture = (unsigned long)atol(e);
    G->stats = (e = getenv("TUR_GC_STATS")) && e[0] == '1';
    G->mstack_cap = 1u << 16;
    G->mstack = (uintptr_t *)tur_gc_os(G->mstack_cap * sizeof(uintptr_t));
#if defined(__APPLE__)
    G->stack_base = (unsigned char *)pthread_get_stackaddr_np(pthread_self());
#else
    pthread_attr_t a; void *addr = NULL; size_t sz = 0;
    extern int pthread_getattr_np(pthread_t, pthread_attr_t *);
    if (pthread_getattr_np(pthread_self(), &a) == 0) {
        if (pthread_attr_getstack(&a, &addr, &sz) == 0 && addr)
            G->stack_base = (unsigned char *)addr + sz;
        pthread_attr_destroy(&a);
    }
#endif
    /* No stack base, no collection: every object is simply kept. */
    if (!G->stack_base) G->off = true;
    G->ready = true;
}

static void tur_gc_note_bounds(uintptr_t base, size_t len) {
    tur_gc_state *G = tur_gc_G;
    if (!G->lo || base < G->lo) G->lo = base;
    if (base + len > G->hi) G->hi = base + len;
}

static tur_gc_page *tur_gc_new_small(uint32_t cls) {
    tur_gc_state *G = tur_gc_G;
    tur_gc_page *pg = (tur_gc_page *)tur_gc_meta(sizeof *pg);
    memset(pg, 0, sizeof *pg);
    pg->base = tur_gc_chunks(1);
    pg->size = tur_gc_class_size[cls];
    pg->nslots = (uint32_t)(TUR_GC_CHUNK / pg->size);
    pg->nchunks = 1;
    pg->cls = cls;
    size_t words = (pg->nslots + 63) / 64;
    pg->alloc = (uint64_t *)tur_gc_meta(words * 8);
    pg->mark = (uint64_t *)tur_gc_meta(words * 8);
    memset(pg->alloc, 0, words * 8);
    memset(pg->mark, 0, words * 8);
    pg->next = G->pages; G->pages = pg;
    tur_gc_map_put(pg->base >> 16, pg);
    tur_gc_note_bounds(pg->base, TUR_GC_CHUNK);
    G->heap_bytes += TUR_GC_CHUNK;
    if (G->heap_bytes > G->heap_peak) G->heap_peak = G->heap_bytes;
    return pg;
}

TUR_GC_NOASAN static void tur_gc_collect_now(void);

static void tur_gc_maybe_collect(size_t n) {
    tur_gc_state *G = tur_gc_G;
    if (G->off || G->collecting) return;
    if (G->torture) {
        if (++G->torture_count >= G->torture) { G->torture_count = 0; tur_gc_collect_now(); }
        return;
    }
    if (G->since + n > G->threshold) tur_gc_collect_now();
}

static uint32_t tur_gc_class_of(size_t n) {
    uint32_t lo = 0, hi = TUR_GC_NCLASS - 1;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (tur_gc_class_size[mid] >= n) hi = mid; else lo = mid + 1;
    }
    return lo;
}

static void *tur_gc_alloc_small(size_t n) {
    tur_gc_state *G = tur_gc_G;
    uint32_t cls = tur_gc_class_of(n ? n : 1);
    void *p = G->freelist[cls];
    tur_gc_page *pg;
    uint32_t idx;
    if (p) {
        G->freelist[cls] = *(void **)p;
        pg = tur_gc_map_get((uintptr_t)p >> 16);
        idx = (uint32_t)(((uintptr_t)p - pg->base) / pg->size);
    } else {
        pg = G->bump[cls];
        if (!pg || pg->used >= pg->nslots) pg = G->bump[cls] = tur_gc_new_small(cls);
        idx = pg->used++;
        p = (void *)(pg->base + (uintptr_t)idx * pg->size);
    }
    pg->alloc[idx / 64] |= (uint64_t)1 << (idx % 64);
    memset(p, 0, pg->size);
    G->since += pg->size;
    return p;
}

static void *tur_gc_alloc_large(size_t n) {
    tur_gc_state *G = tur_gc_G;
    size_t nch = (n + TUR_GC_CHUNK - 1) / TUR_GC_CHUNK;
    tur_gc_page *pg = (tur_gc_page *)tur_gc_meta(sizeof *pg);
    memset(pg, 0, sizeof *pg);
    pg->base = tur_gc_chunks(nch);
    pg->size = n;
    pg->nslots = 1;
    pg->nchunks = (uint32_t)nch;
    pg->cls = TUR_GC_NCLASS;
    pg->used = 1;
    pg->alloc = (uint64_t *)tur_gc_meta(8);
    pg->mark = (uint64_t *)tur_gc_meta(8);
    pg->alloc[0] = 1; pg->mark[0] = 0;
    pg->next = G->pages; G->pages = pg;
    for (size_t i = 0; i < nch; i++) tur_gc_map_put((pg->base >> 16) + i, pg);
    tur_gc_note_bounds(pg->base, nch * TUR_GC_CHUNK);
    G->heap_bytes += nch * TUR_GC_CHUNK;
    if (G->heap_bytes > G->heap_peak) G->heap_peak = G->heap_bytes;
    G->since += nch * TUR_GC_CHUNK;
    return (void *)pg->base;   /* fresh mmap memory is already zero */
}

static void *tur_gc_malloc(size_t n) {
    if (!tur_gc_G) tur_gc_init();
    tur_gc_maybe_collect(n);
    return n <= TUR_GC_MAXSMALL ? tur_gc_alloc_small(n) : tur_gc_alloc_large(n);
}

/* The object `p` points into, or NULL: its start, size, page and slot. */
static bool tur_gc_find(uintptr_t w, uintptr_t *start, tur_gc_page **pgo, uint32_t *idxo) {
    tur_gc_state *G = tur_gc_G;
    if (w < G->lo || w >= G->hi) return false;
    tur_gc_page *pg = tur_gc_map_get(w >> 16);
    if (!pg || pg->dead) return false;
    uint32_t idx = pg->nslots == 1 ? 0 : (uint32_t)((w - pg->base) / pg->size);
    if (idx >= pg->nslots) return false;
    if (!(pg->alloc[idx / 64] & ((uint64_t)1 << (idx % 64)))) return false;
    *start = pg->base + (uintptr_t)idx * (pg->nslots == 1 ? 0 : pg->size);
    *pgo = pg; *idxo = idx;
    return true;
}

static void tur_gc_release_large(tur_gc_page *pg) {
    tur_gc_state *G = tur_gc_G;
    for (uint32_t i = 0; i < pg->nchunks; i++) tur_gc_map_put((pg->base >> 16) + i, NULL);
    munmap((void *)pg->base, (size_t)pg->nchunks * TUR_GC_CHUNK);
    G->heap_bytes -= (size_t)pg->nchunks * TUR_GC_CHUNK;
    pg->alloc[0] = 0;
    pg->dead = true;
}

static void tur_gc_free(void *p) {
    if (!p) return;
    uintptr_t start; tur_gc_page *pg; uint32_t idx;
    if (!tur_gc_G || !tur_gc_find((uintptr_t)p, &start, &pg, &idx)) {
        /* Not ours: libc's, from before the redirect or from a library. */
        if (!tur_gc_G || (uintptr_t)p < tur_gc_G->lo || (uintptr_t)p >= tur_gc_G->hi
            || !tur_gc_map_get((uintptr_t)p >> 16))
            free(p);
        return;   /* ours but already free: a double free, ignored */
    }
    if (start != (uintptr_t)p) return;   /* an interior pointer is not a malloc result */
    if (pg->cls == TUR_GC_NCLASS) { tur_gc_release_large(pg); return; }
    pg->alloc[idx / 64] &= ~((uint64_t)1 << (idx % 64));
    *(void **)p = tur_gc_G->freelist[pg->cls];
    tur_gc_G->freelist[pg->cls] = p;
}

static void *tur_gc_calloc(size_t n, size_t m) {
    if (m && n > (size_t)-1 / m) return NULL;
    return tur_gc_malloc(n * m);   /* always zeroed */
}

static void *tur_gc_realloc(void *p, size_t n) {
    if (!p) return tur_gc_malloc(n);
    uintptr_t start; tur_gc_page *pg; uint32_t idx;
    if (!tur_gc_G || !tur_gc_find((uintptr_t)p, &start, &pg, &idx) || start != (uintptr_t)p)
        return realloc(p, n);   /* libc's block stays libc's */
    size_t have = pg->size;
    if (n <= have && (pg->cls == TUR_GC_NCLASS || n > have / 2 || have <= 16)) return p;
    void *q = tur_gc_malloc(n);
    memcpy(q, p, have < n ? have : n);
    tur_gc_free(p);
    return q;
}

static char *tur_gc_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *r = (char *)tur_gc_malloc(n);
    memcpy(r, s, n);
    return r;
}

static char *tur_gc_strndup(const char *s, size_t m) {
    size_t n = 0;
    while (n < m && s[n]) n++;
    char *r = (char *)tur_gc_malloc(n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

/* ---- marking ------------------------------------------------------------ */

static void tur_gc_push(uintptr_t start) {
    tur_gc_state *G = tur_gc_G;
    if (G->mstack_n == G->mstack_cap) {
        size_t nc = G->mstack_cap * 2;
        uintptr_t *ns = (uintptr_t *)tur_gc_os(nc * sizeof(uintptr_t));
        memcpy(ns, G->mstack, G->mstack_n * sizeof(uintptr_t));
        munmap(G->mstack, G->mstack_cap * sizeof(uintptr_t));
        G->mstack = ns; G->mstack_cap = nc;
    }
    G->mstack[G->mstack_n++] = start;
}

static void tur_gc_mark_word(uintptr_t w) {
    uintptr_t start; tur_gc_page *pg; uint32_t idx;
    if (!tur_gc_find(w, &start, &pg, &idx)) return;
    uint64_t bit = (uint64_t)1 << (idx % 64);
    if (pg->mark[idx / 64] & bit) return;
    pg->mark[idx / 64] |= bit;
    tur_gc_push(start);
}

TUR_GC_NOASAN static void tur_gc_scan(const void *p, size_t n) {
    uintptr_t a = ((uintptr_t)p + sizeof(uintptr_t) - 1) & ~(uintptr_t)(sizeof(uintptr_t) - 1);
    uintptr_t e = (uintptr_t)p + n;
    for (; a + sizeof(uintptr_t) <= e; a += sizeof(uintptr_t))
        tur_gc_mark_word(*(const volatile uintptr_t *)a);
}

static void tur_gc_scan_cb(const void *p, size_t n, void *ud) { (void)ud; tur_gc_scan(p, n); }

TUR_GC_NOASAN static void tur_gc_drain(void) {
    tur_gc_state *G = tur_gc_G;
    while (G->mstack_n) {
        uintptr_t start = G->mstack[--G->mstack_n];
        tur_gc_page *pg = tur_gc_map_get(start >> 16);
        tur_gc_scan((const void *)start, pg->size);
    }
}

/* The executable's writable data: every static the program and the linked
 * runtime keep, the emitted runtime's per-thread state among them. */
#if defined(__APPLE__)
TUR_GC_NOASAN static void tur_gc_scan_data(void) {
    const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(0);
    if (!mh) return;
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    const struct load_command *lc = (const struct load_command *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const struct segment_command_64 *)lc;
            /* __DATA, __DATA_CONST (relocated on load; read-only after, which
             * a read does not mind) and any other segment loaded writable.
             * __TEXT, __LINKEDIT and __PAGEZERO are not. */
            if ((sg->initprot & VM_PROT_WRITE) && sg->vmsize)
                tur_gc_scan((const void *)((uintptr_t)sg->vmaddr + (uintptr_t)slide), (size_t)sg->vmsize);
        }
        lc = (const struct load_command *)((const char *)lc + lc->cmdsize);
    }
}
#else
extern char __data_start[] __attribute__((weak));
extern char _end[] __attribute__((weak));
TUR_GC_NOASAN static void tur_gc_scan_data(void) {
    uintptr_t ds = (uintptr_t)&__data_start[0], de = (uintptr_t)&_end[0];
    if (ds && de > ds) tur_gc_scan((const void *)ds, (size_t)(de - ds));
}
#endif
/* The region runtime: live and retired generations are roots.  Declared here
 * (the preamble's region.h comes later) with the header's linkage. */
#ifndef TUR_RT_API
#define TUR_RT_API
#endif
TUR_RT_API void tur_region_each_used(void (*cb)(const void *p, size_t n, void *ud), void *ud);

TUR_GC_NOASAN __attribute__((noinline)) static void tur_gc_mark_roots(void) {
    tur_gc_state *G = tur_gc_G;
    jmp_buf regs;
    setjmp(regs);                       /* callee-saved registers, onto this frame */
    volatile unsigned char here = 0;
    tur_gc_scan((const void *)&regs, sizeof regs);
    unsigned char *sp = (unsigned char *)&here;
    if (sp < G->stack_base) tur_gc_scan(sp, (size_t)(G->stack_base - sp));
    tur_gc_scan_data();
    tur_region_each_used(tur_gc_scan_cb, NULL);
    tur_gc_drain();
}

TUR_GC_NOASAN static void tur_gc_collect_now(void) {
    tur_gc_state *G = tur_gc_G;
    if (G->off || G->collecting) return;
    G->collecting = true;
    for (tur_gc_page *pg = G->pages; pg; pg = pg->next) {
        if (pg->dead) continue;
        memset(pg->mark, 0, ((pg->nslots + 63) / 64) * 8);
    }
    tur_gc_mark_roots();
    /* Sweep: rebuild every free list from the slots nobody reached. */
    for (int c = 0; c < TUR_GC_NCLASS; c++) G->freelist[c] = NULL;
    size_t live = 0, freed = 0;
    for (tur_gc_page *pg = G->pages; pg; pg = pg->next) {
        if (pg->dead) continue;
        if (pg->cls == TUR_GC_NCLASS) {
            if (pg->mark[0]) live += pg->size;
            else { freed += pg->size; tur_gc_release_large(pg); }
            continue;
        }
        for (uint32_t i = 0; i < pg->used; i++) {
            uint64_t bit = (uint64_t)1 << (i % 64);
            void *slot = (void *)(pg->base + (uintptr_t)i * pg->size);
            if (pg->alloc[i / 64] & bit) {
                if (pg->mark[i / 64] & bit) { live += pg->size; continue; }
                pg->alloc[i / 64] &= ~bit;
                freed += pg->size;
            }
            *(void **)slot = G->freelist[pg->cls];
            G->freelist[pg->cls] = slot;
        }
    }
    G->live = live;
    G->freed_total += freed;
    G->n_collect++;
    G->since = 0;
    G->threshold = 2 * live > G->floor ? 2 * live : G->floor;
    G->collecting = false;
}

static void tur_gc_report(void) {
    tur_gc_state *G = tur_gc_G;
    if (!G || !G->stats) return;
    fprintf(stderr, "r7rs-gc: collections=%zu freed=%zu live-at-last=%zu heap=%zu peak-heap=%zu\n",
            G->n_collect, G->freed_total, G->live, G->heap_bytes, G->heap_peak);
}

/* Region fallbacks: outside a bracket the region allocator falls back to
 * malloc, which must be ours; memory a live generation owns stays the
 * region's. */
TUR_RT_API bool tur_region_active(void);
TUR_RT_API void *tur_region_alloc(size_t n);
TUR_RT_API bool tur_region_owns(const void *p);
TUR_RT_API void tur_region_free(void *p);
static __attribute__((unused)) void *tur_gc_region_alloc(size_t n) {
    if (tur_region_active()) { void *p = tur_region_alloc(n); if (p) return p; }
    return tur_gc_malloc(n);
}
static __attribute__((unused)) void tur_gc_region_free(void *p) {
    if (p && tur_region_owns(p)) { tur_region_free(p); return; }
    tur_gc_free(p);
}

/* Public for tests and the stats line. */
static __attribute__((unused)) void tur_gc_collect(void) {
    if (!tur_gc_G) tur_gc_init();
    tur_gc_collect_now();
}

/* The runtime archive's allocator hook (src/runtime/rt_alloc.h): with it
 * installed, a HAMT node or an rc<T> block the archive allocates is a
 * collected object too, so a Scheme value kept in a Turmeric map or cell is
 * scanned through it instead of freed under it.  A weak reference, because
 * the program links the archive only when something in it is used: with no
 * archive (or the bare-source autolink) there is nothing to hook, and the
 * symbol resolves to NULL. */
typedef struct tur_gc_rt_allocator {
    void *(*malloc)(size_t n);
    void *(*calloc)(size_t n, size_t m);
    void *(*realloc)(void *p, size_t n);
    void  (*free)(void *p);
} tur_gc_rt_allocator;
extern void tur_rt_set_allocator(const tur_gc_rt_allocator *a) __attribute__((weak));

/* Threads (the plan's limit, decided): the collector stops no other thread
 * and reads the runtime's per-thread state as plain statics, so a second
 * thread would allocate from an unlocked heap and hold roots nowhere the
 * collector looks.  Rather than a silent use-after-free, a program that
 * starts one under the flag stops here with the reason.  Every start site in
 * the unit -- the stdlib's thread/session/task-group wrappers, the emitted
 * multi-threaded scheduler -- spells `pthread_create`, which the macro below
 * routes here; <pthread.h> is already included above, so the macro never
 * meets the declaration. */
static int tur_gc_pthread_create(pthread_t *t, const pthread_attr_t *a,
                                 void *(*fn)(void *), void *arg) {
    (void)t; (void)a; (void)fn; (void)arg;
    fputs("tur: r7rs-gc: this program starts a thread, which the collector does not "
          "support (docs/upcoming/r7rs-gc-plan.md); build it without --enable=r7rs-gc\n",
          stderr);
    exit(70);   /* EX_SOFTWARE */
}
#define pthread_create tur_gc_pthread_create

/* Before every other constructor in the unit (101 is the first user
 * priority), so the archive's first allocation already goes through us. */
static __attribute__((constructor(101))) void tur_gc_ctor(void) {
    if (!tur_gc_G) tur_gc_init();
    if (tur_rt_set_allocator) {
        static const tur_gc_rt_allocator ours = {
            tur_gc_malloc, tur_gc_calloc, tur_gc_realloc, tur_gc_free
        };
        tur_rt_set_allocator(&ours);
    }
    atexit(tur_gc_report);
}

#else  /* !TUR_GC_ON: libc, and nothing is ever collected */

static void *tur_gc_malloc(size_t n) { return malloc(n); }
static void *tur_gc_calloc(size_t n, size_t m) { return calloc(n, m); }
static void *tur_gc_realloc(void *p, size_t n) { return realloc(p, n); }
static void  tur_gc_free(void *p) { free(p); }
static char *tur_gc_strdup(const char *s) {
    size_t n = strlen(s) + 1; char *r = (char *)malloc(n); if (r) memcpy(r, s, n); return r;
}
static char *tur_gc_strndup(const char *s, size_t m) {
    size_t n = 0; while (n < m && s[n]) n++;
    char *r = (char *)malloc(n + 1); if (r) { memcpy(r, s, n); r[n] = 0; } return r;
}
#ifndef TUR_RT_API
#define TUR_RT_API
#endif
TUR_RT_API void *tur_region_alloc_or_malloc(size_t n);
TUR_RT_API void tur_region_free(void *p);
static void *tur_gc_region_alloc(size_t n) { return tur_region_alloc_or_malloc(n); }
static void  tur_gc_region_free(void *p) { tur_region_free(p); }
static __attribute__((unused)) void tur_gc_collect(void) { }

#endif
