/* Collection native overrides for the tree-walking interpreter.  Relocated
 * verbatim from src/main.c so they land in tur_core / libturi and are available
 * to every interpreter env, not just the `tur` CLI.  See
 * docs/archive/history/turi-interp-collections-libturi-plan.md. */

#include "collections_native.h"
#include "runtime/hamt.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* tur-vec-homog__ / (compile-time homogeneity check) is a runtime :nil no-op;
 * the simple inline-C executor cannot run its bodyless void, so a native
 * restores it. */
static TuriValue native_coll_noop(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud; return turi_nil();
}

static TuriValue native_tur_hamt_new(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    Hamt *m = tur_hamt_new();
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)m; return v;
}
static TuriValue native_tur_hamt_free(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n >= 1) tur_hamt_free((Hamt *)(intptr_t)a[0].as_int);
    return turi_nil();
}
static TuriValue native_tur_hamt_retain(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    Hamt *m = (n >= 1) ? (Hamt *)(intptr_t)a[0].as_int : NULL;
    Hamt *r = tur_hamt_retain(m);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)r; return v;
}
static TuriValue native_tur_hamt_count(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    Hamt *m = (n >= 1) ? (Hamt *)(intptr_t)a[0].as_int : NULL;
    uint32_t c = tur_hamt_count(m);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)c; return v;
}
static TuriValue native_tur_hamt_set(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 4) return turi_nil();
    Hamt *m    = (Hamt *)(intptr_t)a[0].as_int;
    uint64_t h = (uint64_t)a[1].as_int;
    void *key  = (void *)(intptr_t)a[2].as_int;
    void *val  = (void *)(intptr_t)a[3].as_int;
    /* For cstr keys/values, use the actual pointer (they're interned). */
    if (a[2].tag == TURI_CSTR) key = (void *)a[2].as_cstr;
    if (a[3].tag == TURI_CSTR) val = (void *)a[3].as_cstr;
    Hamt *r = tur_hamt_set(m, h, key, val);
    /* If set returned the same HAMT (structural no-op), retain for the new binding. */
    if (r == m) tur_hamt_retain(r);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)r; return v;
}
static TuriValue native_tur_hamt_del(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 3) return turi_nil();
    Hamt *m    = (Hamt *)(intptr_t)a[0].as_int;
    uint64_t h = (uint64_t)a[1].as_int;
    void *key  = (n >= 3 && a[2].tag == TURI_CSTR) ? (void *)a[2].as_cstr
                                                     : (void *)(intptr_t)a[2].as_int;
    Hamt *r = tur_hamt_del(m, h, key);
    /* If del returned the same HAMT (key absent), the caller now holds two
     * bindings (original + new let) to the same object; retain so both
     * tur_hamt_free calls are safe. */
    if (r == m) tur_hamt_retain(r);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)r; return v;
}
static TuriValue native_tur_hamt_has(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 3) { TuriValue v = {0}; v.tag = TURI_BOOL; v.as_bool = false; return v; }
    Hamt *m    = (Hamt *)(intptr_t)a[0].as_int;
    uint64_t h = (uint64_t)a[1].as_int;
    void *key  = (a[2].tag == TURI_CSTR) ? (void *)a[2].as_cstr
                                          : (void *)(intptr_t)a[2].as_int;
    bool r = tur_hamt_has(m, h, key);
    TuriValue v = {0}; v.tag = TURI_BOOL; v.as_bool = r; return v;
}
static TuriValue native_tur_hamt_get(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 3) return turi_nil();
    Hamt *m    = (Hamt *)(intptr_t)a[0].as_int;
    uint64_t h = (uint64_t)a[1].as_int;
    void *key  = (a[2].tag == TURI_CSTR) ? (void *)a[2].as_cstr
                                          : (void *)(intptr_t)a[2].as_int;
    void *r = tur_hamt_get(m, h, key);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)r; return v;
}
/* Content-keyed cstr entry points backing hamt/set-cstr et al. -- the P3
 * ^persistent lowering routes :cstr keys here so runtime-built keys (equal
 * text, distinct pointers) behave like literals.  Same retain-on-no-change
 * quirk as native_tur_hamt_set/del: each persistent binding is freed
 * separately by the interpreter. */
static TuriValue native_tur_hamt_set_cstr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 3) return turi_nil();
    Hamt *m         = (Hamt *)(intptr_t)a[0].as_int;
    const char *key = (a[1].tag == TURI_CSTR) ? a[1].as_cstr
                                              : (const char *)(intptr_t)a[1].as_int;
    void *val       = (a[2].tag == TURI_CSTR) ? (void *)a[2].as_cstr
                                              : (void *)(intptr_t)a[2].as_int;
    Hamt *r = tur_hamt_set_cstr(m, key, val);
    if (r == m) tur_hamt_retain(r);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)r; return v;
}
static TuriValue native_tur_hamt_del_cstr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_nil();
    Hamt *m         = (Hamt *)(intptr_t)a[0].as_int;
    const char *key = (a[1].tag == TURI_CSTR) ? a[1].as_cstr
                                              : (const char *)(intptr_t)a[1].as_int;
    Hamt *r = tur_hamt_del_cstr(m, key);
    if (r == m) tur_hamt_retain(r);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)r; return v;
}
static TuriValue native_tur_hamt_has_cstr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) { TuriValue v = {0}; v.tag = TURI_BOOL; v.as_bool = false; return v; }
    Hamt *m         = (Hamt *)(intptr_t)a[0].as_int;
    const char *key = (a[1].tag == TURI_CSTR) ? a[1].as_cstr
                                              : (const char *)(intptr_t)a[1].as_int;
    bool r = tur_hamt_has_cstr(m, key);
    TuriValue v = {0}; v.tag = TURI_BOOL; v.as_bool = r; return v;
}
static TuriValue native_tur_hamt_get_cstr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_nil();
    Hamt *m         = (Hamt *)(intptr_t)a[0].as_int;
    const char *key = (a[1].tag == TURI_CSTR) ? a[1].as_cstr
                                              : (const char *)(intptr_t)a[1].as_int;
    void *r = tur_hamt_get_cstr(m, key);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)r; return v;
}
static TuriValue native_tur_hamt_merge(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_nil();
    Hamt *ma = (Hamt *)(intptr_t)a[0].as_int;
    Hamt *mb = (Hamt *)(intptr_t)a[1].as_int;
    Hamt *r = tur_hamt_merge(ma, mb);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)r; return v;
}
static TuriValue native_tur_hamt_hash_str(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    const char *s = (n >= 1 && a[0].tag == TURI_CSTR) ? a[0].as_cstr : "";
    uint64_t h = tur_hamt_hash_str(s);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)h; return v;
}
static TuriValue native_tur_hamt_hash_ptr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    void *p = (n >= 1) ? ((a[0].tag == TURI_CSTR) ? (void *)a[0].as_cstr
                                                   : (void *)(intptr_t)a[0].as_int)
                       : NULL;
    uint64_t h = tur_hamt_hash_ptr(p);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)h; return v;
}
static TuriValue native_tur_hamt_iter_init(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_nil();
    HamtIter *iter = (HamtIter *)(intptr_t)a[0].as_int;
    Hamt *m = (Hamt *)(intptr_t)a[1].as_int;
    if (iter) tur_hamt_iter_init(iter, m);
    return turi_nil();
}
static TuriValue native_tur_hamt_iter_free(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n >= 1) { HamtIter *iter = (HamtIter *)(intptr_t)a[0].as_int; if (iter) tur_hamt_iter_free(iter); }
    return turi_nil();
}
static TuriValue native_tur_hamt_iter_next(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 4) { TuriValue v = {0}; v.tag = TURI_BOOL; return v; }
    HamtIter *iter  = (HamtIter *)(intptr_t)a[0].as_int;
    uint64_t *hout  = (uint64_t *)(intptr_t)a[1].as_int;
    void    **kout  = (void **)(intptr_t)a[2].as_int;
    void    **vout  = (void **)(intptr_t)a[3].as_int;
    bool r = tur_hamt_iter_next(iter, hout, kout, vout);
    TuriValue v = {0}; v.tag = TURI_BOOL; v.as_bool = r; return v;
}

/* hamt/merge-with: merge two HAMTs with a Turmeric conflict-resolver closure.
 * Signature: (hamt/merge-with a b fn ctx) where fn is a Turmeric closure
 * called as (fn val_a val_b ctx) for duplicate keys. */
static TuriValue native_hamt_merge_with(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 4) return turi_nil();
    Hamt *ma  = (Hamt *)(intptr_t)a[0].as_int;
    Hamt *mb  = (Hamt *)(intptr_t)a[1].as_int;
    TuriValue fn  = a[2];
    TuriValue ctx = a[3];
    /* Start with a retain of ma as the result. */
    Hamt *result = tur_hamt_retain(ma);
    /* Iterate over b, merging into result. */
    HamtIter *iter = (HamtIter *)calloc(1, sizeof(HamtIter));
    if (!iter) { return turi_int((int64_t)(intptr_t)result); }
    tur_hamt_iter_init(iter, mb);
    uint64_t h; void *k; void *v;
    while (tur_hamt_iter_next(iter, &h, &k, &v)) {
        void *existing = tur_hamt_get(result, h, k);
        void *new_val;
        if (existing) {
            /* Call the Turmeric closure: (fn existing v ctx) */
            TuriValue call_args[3];
            call_args[0].tag = TURI_INT; call_args[0].as_int = (int64_t)(intptr_t)existing;
            call_args[1].tag = TURI_INT; call_args[1].as_int = (int64_t)(intptr_t)v;
            call_args[2] = ctx;
            TuriValue rv = turi_call(env, fn, call_args, 3);
            if (turi_is_error(rv) || env->throwing) return rv;  /* propagate callback error */
            new_val = (void *)(intptr_t)rv.as_int;
        } else {
            new_val = v;
        }
        Hamt *next = tur_hamt_set(result, h, k, new_val);
        if (next != result) tur_hamt_free(result);
        result = next;
    }
    tur_hamt_iter_free(iter);
    free(iter);
    TuriValue rv = {0}; rv.tag = TURI_INT; rv.as_int = (int64_t)(intptr_t)result; return rv;
}

static TuriValue native_tur_hamt_show(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    Hamt *m = (n >= 1) ? (Hamt *)(intptr_t)a[0].as_int : NULL;
    char *s = tur_hamt_show(m);
    TuriValue v = {0}; v.tag = TURI_CSTR; v.as_cstr = s; return v;
}
static TuriValue native_tur_hamt_transient(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    Hamt *m = (n >= 1) ? (Hamt *)(intptr_t)a[0].as_int : NULL;
    HamtTransient *t = tur_hamt_transient(m);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)t; return v;
}
static TuriValue native_tur_hamt_transient_set(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 4) return turi_nil();
    HamtTransient *t = (HamtTransient *)(intptr_t)a[0].as_int;
    uint64_t h = (uint64_t)a[1].as_int;
    void *key = (a[2].tag == TURI_CSTR) ? (void *)a[2].as_cstr : (void *)(intptr_t)a[2].as_int;
    void *val = (a[3].tag == TURI_CSTR) ? (void *)a[3].as_cstr : (void *)(intptr_t)a[3].as_int;
    if (t) tur_hamt_transient_set(t, h, key, val);
    return turi_nil();
}
static TuriValue native_tur_hamt_transient_del(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 3) return turi_nil();
    HamtTransient *t = (HamtTransient *)(intptr_t)a[0].as_int;
    uint64_t h = (uint64_t)a[1].as_int;
    void *key = (a[2].tag == TURI_CSTR) ? (void *)a[2].as_cstr : (void *)(intptr_t)a[2].as_int;
    if (t) tur_hamt_transient_del(t, h, key);
    return turi_nil();
}
static TuriValue native_tur_hamt_persistent(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    HamtTransient *t = (n >= 1) ? (HamtTransient *)(intptr_t)a[0].as_int : NULL;
    Hamt *m = tur_hamt_persistent(t);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)m; return v;
}

/* -------------------------------------------------------------------------
 * Set operations -- W1b: a Set is set.tur's `struct { void *hamt; }` (one
 * pointer = the persistent HAMT), matching the compiled representation.  These
 * native overrides invoke the real runtime HAMT (the same `tur_hamt_*` the
 * compiled set ops call), so `set-new`/`add`/`count`/... work under --interpret.
 * Element keys use the int-keyed HAMT API (value == key == hash for the caller-
 * supplied `h`); content-keyed sets (needing a C eq callback) remain a gap.
 * ---------------------------------------------------------------------- */
static Hamt *set_hamt(TuriValue v) {
    if (v.tag != TURI_INT || v.as_int == 0) return NULL;
    return (Hamt *)((void **)(intptr_t)v.as_int)[0];
}
/* interp-collections-never-freed: a Set/Map box is a 2-word wrapper --
 * [0] = the persistent HAMT (what set_hamt reads), [1] = the TuriCollBuf
 * tracking node so env-teardown and an explicit set-free/map-free share one
 * O(1) tombstone slot (mirrors the Vec box's slot [3]).  Every box the
 * interpreter hands out is registered on the env so its HAMT is released at
 * turi_env_free; the persistent HAMT's own refcount keeps structural sharing
 * across boxes correct, so freeing each tracked box once is safe now that the
 * delete path retains shared siblings correctly (hamt-delete-sibling-refcount). */
static void set_buf_destroy(void *box) {
    void **s = (void **)box;
    tur_hamt_free((Hamt *)s[0]);
    free(s);
}
/* TR3: enumerate a Set/Map's entries for the eval-boundary sweep.  Keys and
 * values are pointer-encoded int64 carriers with no per-entry tag, so each is
 * visited as a bare candidate int -- that catches a directly-stored handle
 * (a map of vecs), but CANNOT rule out a struct-valued entry holding a handle
 * in a field.  A non-empty map therefore reports its enumeration incomplete
 * (return false), which makes the sweep mark-only for that cycle: nothing is
 * freed on the strength of a scan that might have missed a reference. */
static bool set_buf_scan(void *box, TuriCollBufMarkFn mark, void *ctx) {
    void **s = (void **)box;
    Hamt *h = (Hamt *)s[0];
    if (!h || tur_hamt_count(h) == 0) return true;
    HamtIter it;
    tur_hamt_iter_init(&it, h);
    uint64_t hash;
    void *k, *val;
    while (tur_hamt_iter_next(&it, &hash, &k, &val)) {
        mark(turi_int((int64_t)(intptr_t)k), ctx);
        mark(turi_int((int64_t)(intptr_t)val), ctx);
    }
    tur_hamt_iter_free(&it);
    return false;
}
static TuriValue set_wrap_tracked(TuriEnv *env, Hamt *h) {
    void **s = (void **)malloc(2 * sizeof(void *));
    if (!s) return turi_nil();
    s[0] = (void *)h;
    s[1] = (void *)turi_env_track_collection(env, s, set_buf_destroy, set_buf_scan);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)s; return v;
}
/* Wrap a HAMT that a persistent op derived from `src`.  tur_hamt_set/del (and
 * their _eq* wrappers) return `src` itself *unretained* when the op is a no-op
 * (key already present with the same value / key absent).  Retain in that alias
 * case so this box owns an independent reference -- otherwise two boxes share
 * src's single ref and teardown (or explicit free) of both double-frees the
 * HAMT.  A genuinely fresh result already carries the caller's reference. */
static TuriValue set_wrap_owned(TuriEnv *env, Hamt *src, Hamt *r) {
    if (r == src) tur_hamt_retain(r);
    return set_wrap_tracked(env, r);
}
/* interp-collections-never-freed: release a Set/Map box's HAMT and tombstone
 * its tracking node so the turi_env_free teardown pass skips it (no double
 * free).  Shared by set-free and map-free. */
static TuriValue set_free_box(TuriValue v) {
    if (v.tag != TURI_INT || v.as_int == 0) return turi_nil();
    void **s = (void **)(intptr_t)v.as_int;
    turi_env_untrack_collection((TuriCollBuf *)s[1]);
    tur_hamt_free((Hamt *)s[0]);
    free(s);
    return turi_nil();
}
/* Shared with the map _eq_o natives below: a Turmeric-closure key comparator
 * (from a MapKey `mk-cmp` :turi branch for a multi-word struct/ADT key -- see
 * the #?(:tur ... :turi ...) reader-conditional pattern) is threaded through the
 * HAMT via this ctx + trampoline, exactly as the map natives do.  Defined below
 * (map_turi_eq_tramp); forward-declared here so the set natives can use it. */
typedef struct { TuriEnv *env; TuriValue cmp; } MapTuriEqCtx;
static bool map_turi_eq_tramp(int64_t a, int64_t b, void *vctx);

static TuriValue native_set_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)a; (void)n; (void)ud;
    return set_wrap_tracked(env, tur_hamt_new());
}
/* (set-add s h x) -- persistent insert; returns a new set. */
/* set-add / set-remove / set-member? no longer have plain natives -- they are
 * pure-Turmeric MapKey-dispatching defns (stdlib/set.tur) over the -eq-o raw
 * layer below (see the registration note). */
/* Raw content-keyed set ops: (s h x keyeq owned) -- the interpreter overrides
 * for stdlib/set.tur's set-add-eq-o / set-has-eq-o? / set-del-eq-o inline-C
 * bodies (the value slot is the (void*)1 sentinel; a Set is a Map to unit).
 * keyeq is normally a C function pointer (from MapKey[A].mk-cmp) carried as int;
 * for a multi-word struct/ADT element whose MapKey `mk-cmp` :turi branch returns
 * a Turmeric CLOSURE, thread it through the ctx trampoline exactly like the map
 * _eq_o natives (a raw-int cast + call would be a wild jump).  NULL / non-closure
 * keyeq (int/Sym identity sets) makes the _eq_o path behave like the plain path. */
static TuriValue native_set_add_eq_o(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 5) return n >= 1 ? a[0] : turi_nil();
    Hamt *src = set_hamt(a[0]);
    if (a[3].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[3] };
        Hamt *r = tur_hamt_set_eq_ctx(src, (uint64_t)a[1].as_int,
                                      (void *)(intptr_t)a[2].as_int, (void *)1,
                                      map_turi_eq_tramp, &ctx);
        return set_wrap_owned(e, src, r);
    }
    Hamt *r = tur_hamt_set_eq_o(src, (uint64_t)a[1].as_int,
                                (void *)(intptr_t)a[2].as_int, (void *)1,
                                (tur_hamt_keyeq_fn)(intptr_t)a[3].as_int,
                                (int64_t)a[4].as_int);
    return set_wrap_owned(e, src, r);
}
static TuriValue native_set_has_eq_o(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 5) return turi_bool(false);
    if (a[3].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[3] };
        return turi_bool(tur_hamt_has_eq_ctx(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                             (void *)(intptr_t)a[2].as_int,
                                             map_turi_eq_tramp, &ctx));
    }
    return turi_bool(tur_hamt_has_eq_o(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                       (void *)(intptr_t)a[2].as_int,
                                       (tur_hamt_keyeq_fn)(intptr_t)a[3].as_int,
                                       (int64_t)a[4].as_int));
}
static TuriValue native_set_del_eq_o(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 5) return n >= 1 ? a[0] : turi_nil();
    Hamt *src = set_hamt(a[0]);
    if (a[3].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[3] };
        Hamt *r = tur_hamt_del_eq_ctx(src, (uint64_t)a[1].as_int,
                                      (void *)(intptr_t)a[2].as_int,
                                      map_turi_eq_tramp, &ctx);
        return set_wrap_owned(e, src, r);
    }
    Hamt *r = tur_hamt_del_eq_o(src, (uint64_t)a[1].as_int,
                                (void *)(intptr_t)a[2].as_int,
                                (tur_hamt_keyeq_fn)(intptr_t)a[3].as_int,
                                (int64_t)a[4].as_int);
    return set_wrap_owned(e, src, r);
}
static TuriValue native_set_count(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    return turi_int((int64_t)tur_hamt_count(set_hamt(a[0])));
}
static TuriValue native_set_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    return set_free_box(a[0]);
}
/* set-eq-cmp? -- O(n*m) structural set equality with a user element comparator.
 * set.tur's body double-iterates the HAMTs and fat-dispatches cmp-fn through a C
 * function pointer (un-runnable in the simple inline-C executor); this native
 * re-implements it, invoking the comparator (a turi closure under --interpret)
 * via turi_call.  Elements are stored as the HAMT key (set-add s h x). */
static TuriValue native_set_eq_cmp(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 3) return turi_bool(false);
    Hamt *sa = set_hamt(a[0]), *sb = set_hamt(a[1]);
    TuriValue cmp = a[2];
    if (tur_hamt_count(sa) != tur_hamt_count(sb)) return turi_bool(false);
    uint64_t iter_a[32]; for (int i = 0; i < 32; i++) iter_a[i] = 0;
    tur_hamt_iter_init((HamtIter *)iter_a, sa);
    uint64_t ha; void *ka = NULL, *vap = NULL;
    bool ok = true;
    while (ok && tur_hamt_iter_next((HamtIter *)iter_a, &ha, &ka, &vap)) {
        bool found = false;
        uint64_t iter_b[32]; for (int i = 0; i < 32; i++) iter_b[i] = 0;
        tur_hamt_iter_init((HamtIter *)iter_b, sb);
        uint64_t hb; void *kb = NULL, *vbp = NULL;
        while (tur_hamt_iter_next((HamtIter *)iter_b, &hb, &kb, &vbp)) {
            TuriValue cargs[2];
            cargs[0].tag = TURI_INT; cargs[0].as_int = (int64_t)(intptr_t)ka;
            cargs[1].tag = TURI_INT; cargs[1].as_int = (int64_t)(intptr_t)kb;
            TuriValue rv = turi_call(env, cmp, cargs, 2);
            if (turi_is_error(rv) || env->throwing) {  /* propagate callback error */
                tur_hamt_iter_free((HamtIter *)iter_b);
                tur_hamt_iter_free((HamtIter *)iter_a);
                return rv;
            }
            if (rv.tag == TURI_BOOL ? rv.as_bool : rv.as_int != 0) { found = true; break; }
        }
        tur_hamt_iter_free((HamtIter *)iter_b);
        if (!found) ok = false;
    }
    tur_hamt_iter_free((HamtIter *)iter_a);
    return turi_bool(ok);
}
static TuriValue native_set_union(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return n >= 1 ? a[0] : turi_nil();
    /* Content-keyed union: start from a, insert each element of b through the
     * source's stamped comparator (tur_hamt_keyeq) so content-equal elements
     * (cstr, ...) collapse; NULL comparator (int/Sym) behaves like a plain
     * merge. Mirrors the compiled set-union in stdlib/set.tur. */
    Hamt *ha = set_hamt(a[0]), *hb = set_hamt(a[1]);
    void *keyeq = tur_hamt_keyeq(ha);
    if (!keyeq) keyeq = tur_hamt_keyeq(hb);
    Hamt *result = tur_hamt_retain(ha);
    uint64_t iter_buf[32]; for (int i = 0; i < 32; i++) iter_buf[i] = 0;
    tur_hamt_iter_init((HamtIter *)iter_buf, hb);
    uint64_t h; void *key = NULL, *val = NULL;
    while (tur_hamt_iter_next((HamtIter *)iter_buf, &h, &key, &val)) {
        Hamt *old = result;
        result = tur_hamt_set_eq_o(result, h, key, (void *)1,
                                   (tur_hamt_keyeq_fn)(intptr_t)keyeq, (int64_t)0);
        if (old != result) tur_hamt_free(old);
    }
    tur_hamt_iter_free((HamtIter *)iter_buf);
    return set_wrap_tracked(env, result);
}
/* set-intersect / set-diff iterate `a` and keep keys (present | absent) in `b`. */
static TuriValue set_iter_filter(TuriEnv *env, TuriValue *a, bool keep_if_in_b) {
    Hamt *ha = set_hamt(a[0]), *hb = set_hamt(a[1]);
    /* Content-keyed intersect/diff: membership in b via the stamped comparator
     * (tur_hamt_has_dynamic) and re-insert through it, so content-equal elements
     * match; NULL comparator (int/Sym) behaves like the plain path. Mirrors the
     * compiled set-intersect / set-diff in stdlib/set.tur. */
    void *keyeq = tur_hamt_keyeq(ha);
    if (!keyeq) keyeq = tur_hamt_keyeq(hb);
    Hamt *result = tur_hamt_new();
    uint64_t iter_buf[32]; for (int i = 0; i < 32; i++) iter_buf[i] = 0;
    tur_hamt_iter_init((HamtIter *)iter_buf, ha);
    uint64_t h; void *key = NULL, *val = NULL;
    while (tur_hamt_iter_next((HamtIter *)iter_buf, &h, &key, &val)) {
        if (tur_hamt_has_dynamic(hb, (int64_t)h, key, keyeq) == keep_if_in_b) {
            Hamt *old = result;
            result = tur_hamt_set_eq_o(result, h, key, (void *)1,
                                       (tur_hamt_keyeq_fn)(intptr_t)keyeq, (int64_t)0);
            if (old != result) tur_hamt_free(old);
        }
    }
    tur_hamt_iter_free((HamtIter *)iter_buf);
    /* `result` is a freshly built map the caller owns. */
    return set_wrap_tracked(env, result);
}
static TuriValue native_set_intersect(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud; if (n < 2) return turi_nil();
    return set_iter_filter(env, a, true);
}
static TuriValue native_set_diff(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud; if (n < 2) return turi_nil();
    return set_iter_filter(env, a, false);
}
static TuriValue native_set_eq(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_bool(false);
    Hamt *ha = set_hamt(a[0]), *hb = set_hamt(a[1]);
    if (tur_hamt_count(ha) != tur_hamt_count(hb)) return turi_bool(false);
    uint64_t iter_buf[32]; for (int i = 0; i < 32; i++) iter_buf[i] = 0;
    tur_hamt_iter_init((HamtIter *)iter_buf, ha);
    uint64_t h; void *key = NULL, *val = NULL; bool eq = true;
    while (tur_hamt_iter_next((HamtIter *)iter_buf, &h, &key, &val)) {
        if (!tur_hamt_has(hb, h, key)) { eq = false; break; }
    }
    tur_hamt_iter_free((HamtIter *)iter_buf);
    return turi_bool(eq);
}

/* -------------------------------------------------------------------------
 * Typed Map[K V] natives (TI10 Tier A -- scalar key types).
 *
 * A Map[K V] is the same {void* hamt} carrier as a Set, so set_hamt/set_wrap
 * are reused.  The public map-* accessors are macros that expand to:
 *   (map-assoc-eq-o (kcheck m) (hash k) (mk-box k) v (mk-cmp k) (mk-owned? k))
 * so unblocking map under --interpret needs natives for (a) the MapKey/Hash
 * instance methods whose inline-C bodies the tree-walker can't run, and (b) the
 * raw map-*-eq-o bridges + map-count/merge/free over the runtime HAMT.
 *
 * mk-cmp returns the *address of a C carrier comparator* -- exactly what the
 * compiled path does -- and the runtime calls it through bool(*)(int64,int64)
 * only on a 64-bit hash collision.  mk-box / mk-cmp / hash for int (and bool,
 * which boxes to int) are plain (non-inline-C) bodies the interpreter already
 * evaluates, so only the cstr / float / float32 instances need natives here.
 * Owned (boxed multi-word) keys are out of scope (owned == 0 for all scalars).
 * ---------------------------------------------------------------------- */

/* Carrier comparators -- passed by address to tur_hamt_*_eq_o; mirror map.tur. */
static bool turi_int_carrier_eq_c(int64_t a, int64_t b) { return a == b; }
static bool turi_cstr_key_eq_c(int64_t a, int64_t b) {
    const char *p = (const char *)(intptr_t)a;
    const char *q = (const char *)(intptr_t)b;
    if (p == q) return true;
    if (!p || !q) return false;
    return strcmp(p, q) == 0;
}
static bool turi_f32_carrier_eq_c(int64_t a, int64_t b) {
    union { float f; int64_t i; } ua, ub; ua.i = a; ub.i = b; return ua.f == ub.f;
}
static bool turi_f64_carrier_eq_c(int64_t a, int64_t b) {
    union { double f; int64_t i; } ua, ub; ua.i = a; ub.i = b; return ua.f == ub.f;
}

/* mk-cmp [K] -- return the carrier comparator address as the int64 carrier. */
TuriValue native_mk_cmp_int(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    return turi_int((int64_t)(intptr_t)&turi_int_carrier_eq_c);
}
static TuriValue native_mk_cmp_cstr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    return turi_int((int64_t)(intptr_t)&turi_cstr_key_eq_c);
}
static TuriValue native_mk_cmp_f32(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    return turi_int((int64_t)(intptr_t)&turi_f32_carrier_eq_c);
}
static TuriValue native_mk_cmp_f64(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    return turi_int((int64_t)(intptr_t)&turi_f64_carrier_eq_c);
}

/* mk-box [K] -- box the key into the int64 carrier word. */
TuriValue native_mk_box_cstr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t carrier = a[0].tag == TURI_CSTR
                          ? (int64_t)(intptr_t)a[0].as_cstr : a[0].as_int;
    return turi_int(carrier);
}
static TuriValue native_mk_box_f32(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    union { float f; int64_t i; } u; u.i = 0; u.f = (float)a[0].as_float;
    return turi_int(u.i);
}
static TuriValue native_mk_box_f64(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    union { double f; int64_t i; } u; u.f = a[0].as_float; return turi_int(u.i);
}

/* hash [K] -- content hash matching stdlib/typeclass-hash.tur. */
static TuriValue native_hash_cstr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    const char *s = a[0].tag == TURI_CSTR ? a[0].as_cstr
                                          : (const char *)(intptr_t)a[0].as_int;
    return turi_int((int64_t)tur_hamt_hash_str(s ? s : ""));
}
static TuriValue native_hash_f32(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    union { float f; int32_t i; } u; u.f = (float)a[0].as_float;
    return turi_int((int64_t)u.i);
}
static TuriValue native_hash_f64(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    union { double f; int64_t i; } u; u.f = a[0].as_float; return turi_int(u.i);
}

/* Raw map bridges over the content-keyed HAMT.  The carrier shares Set's layout
 * so set_hamt/set_wrap apply.  keyeq is the comparator address mk-cmp returned;
 * owned is 0 for scalar keys. */

/* TI10 Tier B -- turi-closure-aware key comparator.
 *
 * A content-keyed map whose MapKey instance is written in Turmeric (not inline-C)
 * has `mk-cmp` return a turi *closure*, not a C function-pointer address. That
 * closure cannot be cast to bool(*)(int64,int64) and handed to the runtime HAMT.
 * The 2a `ctx`-carrying HAMT entry points (tur_hamt_*_eq_ctx) close this: we
 * pack {env, comparator-closure} into a ctx word and pass a C trampoline that,
 * on each collision-time compare, calls the closure via turi_call with the two
 * carrier words as :int args.  This is the map analogue of native_result_eq.
 *
 * The trampoline is only valid for a comparator whose carrier words are directly
 * comparable as interpreter values (e.g. a one-word scalar carrier with custom
 * equality logic). A boxed/owned key whose comparator dereferences the box is an
 * inline-C comparator and never reaches here as a runnable turi closure. */
/* MapTuriEqCtx typedef + this trampoline's forward declaration are hoisted above
 * the set _eq_o natives so both the set and map _eq_o natives share them. */
static bool map_turi_eq_tramp(int64_t a, int64_t b, void *vctx) {
    MapTuriEqCtx *c = (MapTuriEqCtx *)vctx;
    /* If a prior comparison in this HAMT op already raised, stop calling the
     * closure -- the enclosing native's result is discarded once the driver
     * sees env->throwing. */
    if (c->env->throwing) return false;
    TuriValue args[2] = { turi_int(a), turi_int(b) };
    TuriValue r = turi_call(c->env, c->cmp, args, 2);
    if (turi_is_error(r) || c->env->throwing) {
        /* This comparator returns a C bool to the HAMT, so it cannot return the
         * error value; promote a value-level error to a throw so the enclosing
         * native (and the driver) propagate it instead of silently mis-hashing. */
        if (turi_is_error(r)) { c->env->throwing = true; c->env->throw_value = r; }
        return false;
    }
    if (r.tag == TURI_BOOL) return r.as_bool;
    if (r.tag == TURI_INT)  return r.as_int != 0;
    return false;
}

static TuriValue native_map_assoc_eq_o(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    /* (m h key val keyeq owned) */
    if (n < 6) return n >= 1 ? a[0] : turi_nil();
    Hamt *src = set_hamt(a[0]);
    if (a[4].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[4] };
        Hamt *r = tur_hamt_set_eq_ctx(src, (uint64_t)a[1].as_int,
                                      (void *)(intptr_t)a[2].as_int,
                                      (void *)(intptr_t)a[3].as_int,
                                      map_turi_eq_tramp, &ctx);
        return set_wrap_owned(e, src, r);
    }
    Hamt *r = tur_hamt_set_eq_o(src, (uint64_t)a[1].as_int,
                                (void *)(intptr_t)a[2].as_int,
                                (void *)(intptr_t)a[3].as_int,
                                (tur_hamt_keyeq_fn)(intptr_t)a[4].as_int,
                                (int64_t)a[5].as_int);
    return set_wrap_owned(e, src, r);
}
static TuriValue native_map_get_eq_o(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    /* (m h key keyeq owned) */
    if (n < 5) return turi_int(0);
    if (a[3].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[3] };
        void *v = tur_hamt_get_eq_ctx(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                      (void *)(intptr_t)a[2].as_int,
                                      map_turi_eq_tramp, &ctx);
        return turi_int((int64_t)(intptr_t)v);
    }
    void *v = tur_hamt_get_eq_o(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                (void *)(intptr_t)a[2].as_int,
                                (tur_hamt_keyeq_fn)(intptr_t)a[3].as_int,
                                (int64_t)a[4].as_int);
    return turi_int((int64_t)(intptr_t)v);
}
static TuriValue native_map_has_eq_o(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 5) return turi_bool(false);
    if (a[3].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[3] };
        return turi_bool(tur_hamt_has_eq_ctx(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                             (void *)(intptr_t)a[2].as_int,
                                             map_turi_eq_tramp, &ctx));
    }
    return turi_bool(tur_hamt_has_eq_o(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                       (void *)(intptr_t)a[2].as_int,
                                       (tur_hamt_keyeq_fn)(intptr_t)a[3].as_int,
                                       (int64_t)a[4].as_int));
}
static TuriValue native_map_dissoc_eq_o(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 5) return n >= 1 ? a[0] : turi_nil();
    Hamt *src = set_hamt(a[0]);
    if (a[3].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[3] };
        Hamt *r = tur_hamt_del_eq_ctx(src, (uint64_t)a[1].as_int,
                                      (void *)(intptr_t)a[2].as_int,
                                      map_turi_eq_tramp, &ctx);
        return set_wrap_owned(e, src, r);
    }
    Hamt *r = tur_hamt_del_eq_o(src, (uint64_t)a[1].as_int,
                                (void *)(intptr_t)a[2].as_int,
                                (tur_hamt_keyeq_fn)(intptr_t)a[3].as_int,
                                (int64_t)a[4].as_int);
    return set_wrap_owned(e, src, r);
}
/* Explicit-hash content API (map-*-eq, no ownership flag) over tur_hamt_*_eq.
 * The comparator is mk-cmp's C address; the key is raw (:K, == carrier for
 * scalars).  A forced-collision test drives these with a controlled hash. */
static TuriValue native_map_assoc_eq(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    /* (m h key val keyeq) */
    if (n < 5) return n >= 1 ? a[0] : turi_nil();
    Hamt *src = set_hamt(a[0]);
    if (a[4].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[4] };
        Hamt *r = tur_hamt_set_eq_ctx(src, (uint64_t)a[1].as_int,
                                      (void *)(intptr_t)a[2].as_int,
                                      (void *)(intptr_t)a[3].as_int,
                                      map_turi_eq_tramp, &ctx);
        return set_wrap_owned(e, src, r);
    }
    Hamt *r = tur_hamt_set_eq(src, (uint64_t)a[1].as_int,
                              (void *)(intptr_t)a[2].as_int,
                              (void *)(intptr_t)a[3].as_int,
                              (tur_hamt_keyeq_fn)(intptr_t)a[4].as_int);
    return set_wrap_owned(e, src, r);
}
static TuriValue native_map_get_eq(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    /* (m h key keyeq) */
    if (n < 4) return turi_int(0);
    if (a[3].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[3] };
        void *v = tur_hamt_get_eq_ctx(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                      (void *)(intptr_t)a[2].as_int,
                                      map_turi_eq_tramp, &ctx);
        return turi_int((int64_t)(intptr_t)v);
    }
    void *v = tur_hamt_get_eq(set_hamt(a[0]), (uint64_t)a[1].as_int,
                              (void *)(intptr_t)a[2].as_int,
                              (tur_hamt_keyeq_fn)(intptr_t)a[3].as_int);
    return turi_int((int64_t)(intptr_t)v);
}
static TuriValue native_map_has_eq(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 4) return turi_bool(false);
    if (a[3].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[3] };
        return turi_bool(tur_hamt_has_eq_ctx(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                             (void *)(intptr_t)a[2].as_int,
                                             map_turi_eq_tramp, &ctx));
    }
    return turi_bool(tur_hamt_has_eq(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                     (void *)(intptr_t)a[2].as_int,
                                     (tur_hamt_keyeq_fn)(intptr_t)a[3].as_int));
}
static TuriValue native_map_dissoc_eq(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 4) return n >= 1 ? a[0] : turi_nil();
    Hamt *src = set_hamt(a[0]);
    if (a[3].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[3] };
        Hamt *r = tur_hamt_del_eq_ctx(src, (uint64_t)a[1].as_int,
                                      (void *)(intptr_t)a[2].as_int,
                                      map_turi_eq_tramp, &ctx);
        return set_wrap_owned(e, src, r);
    }
    Hamt *r = tur_hamt_del_eq(src, (uint64_t)a[1].as_int,
                              (void *)(intptr_t)a[2].as_int,
                              (tur_hamt_keyeq_fn)(intptr_t)a[3].as_int);
    return set_wrap_owned(e, src, r);
}
static TuriValue native_map_count(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    return turi_int((int64_t)tur_hamt_count(set_hamt(a[0])));
}
static TuriValue native_map_merge(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return n >= 1 ? a[0] : turi_nil();
    /* tur_hamt_merge returns a caller-owned reference -- track directly. */
    return set_wrap_tracked(e, tur_hamt_merge(set_hamt(a[0]), set_hamt(a[1])));
}
static TuriValue native_map_free(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_nil();
    return set_free_box(a[0]);
}

/* tur-map-homog__ -- compile-time homogeneity check; a :nil no-op at runtime
 * (the #map{...}/hamt-of lowering emits it).  Its inline-C body is `(void)a;
 * (void)b;` with no return, which the simple inline-C executor doesn't cover. */
static TuriValue native_map_homog(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud; return turi_nil();
}

/* map-eq-raw? / map-eq-raw-k? -- structural Map equality.  map.tur's bodies
 * iterate the HAMT and fat-dispatch the value comparator through a C function
 * pointer (`((bool(*)(...))val_cmp[0])(...)`), which the simple inline-C executor
 * cannot run -- so these natives re-implement the iteration and invoke the
 * comparator (a turi closure under --interpret) via turi_call, mirroring
 * native_result_eq.  map-eq-raw-k? additionally threads a MapKey carrier
 * comparator (a real C fn-ptr address from mk-cmp) into the key lookup. */
static bool map_eq_iter(TuriEnv *env, Hamt *h1, Hamt *h2,
                        tur_hamt_keyeq_fn keyeq, TuriValue val_cmp) {
    if (tur_hamt_count(h1) != tur_hamt_count(h2)) return false;
    HamtIter *iter = (HamtIter *)calloc(1, sizeof(HamtIter));
    if (!iter) return false;
    tur_hamt_iter_init(iter, h1);
    uint64_t h; void *k; void *v;
    bool eq = true;
    while (tur_hamt_iter_next(iter, &h, &k, &v)) {
        void *vb = keyeq ? tur_hamt_get_eq(h2, h, k, keyeq)
                         : tur_hamt_get(h2, h, k);
        if (!vb) { eq = false; break; }
        TuriValue cargs[2];
        cargs[0].tag = TURI_INT; cargs[0].as_int = (int64_t)(intptr_t)v;
        cargs[1].tag = TURI_INT; cargs[1].as_int = (int64_t)(intptr_t)vb;
        TuriValue rv = turi_call(env, val_cmp, cargs, 2);
        if (turi_is_error(rv) || env->throwing) {
            /* returns C bool; promote a value-level error to a throw so the
             * caller (native_map_eq*) and the driver propagate it. */
            if (turi_is_error(rv)) { env->throwing = true; env->throw_value = rv; }
            eq = false; break;
        }
        bool veq = rv.tag == TURI_BOOL ? rv.as_bool : rv.as_int != 0;
        if (!veq) { eq = false; break; }
    }
    tur_hamt_iter_free(iter);
    free(iter);
    return eq;
}
static TuriValue native_map_eq_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 3) return turi_bool(false);
    return turi_bool(map_eq_iter(env, set_hamt(a[0]), set_hamt(a[1]), NULL, a[2]));
}
static TuriValue native_map_eq_raw_k(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 4) return turi_bool(false);
    return turi_bool(map_eq_iter(env, set_hamt(a[0]), set_hamt(a[1]),
                                 (tur_hamt_keyeq_fn)(intptr_t)a[2].as_int, a[3]));
}

/* ----- HAMT iterator runtime bridges (Eq[Map]/Eq[Set] pure-Turmeric loop) -----
 * map.tur's Path A Eq[Map] dispatches through map-eq-driver -> map-eq-loop, a
 * pure-Turmeric walk whose leaf calls (hamt/iter-*, hamt/keyeq, hamt/has-dynamic?,
 * map-hamt, map-iter-cur-val-as, map-get-dynamic-as) are thin extern-c/inline-C
 * shims over the tur_hamt_* runtime.  The tree-walker has no extern-c symbol table
 * and cannot run the inline-C bodies, so without these natives the leaf calls
 * silently yield 0/nil and the loop returns a count-only "true".  Bind the
 * underlying runtime entry points (and the inline-C map-* shims) to real natives;
 * pointers ride the int64 carrier exactly as the compiled ABI uses them. */
static TuriValue native_hamt_iter_alloc(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    return turi_int((int64_t)(intptr_t)tur_hamt_iter_alloc((Hamt *)(intptr_t)a[0].as_int));
}
static TuriValue native_hamt_iter_destroy(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n >= 1 && a[0].as_int) tur_hamt_iter_destroy((void *)(intptr_t)a[0].as_int);
    return turi_nil();
}
static TuriValue native_hamt_iter_advance(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_bool(false);
    return turi_bool(tur_hamt_iter_advance((void *)(intptr_t)a[0].as_int));
}
static TuriValue native_hamt_iter_cur_hash(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(tur_hamt_iter_cur_hash((void *)(intptr_t)a[0].as_int));
}
static TuriValue native_hamt_iter_cur_key(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int((int64_t)(intptr_t)tur_hamt_iter_cur_key((void *)(intptr_t)a[0].as_int));
}
static TuriValue native_hamt_iter_cur_val(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int((int64_t)(intptr_t)tur_hamt_iter_cur_val((void *)(intptr_t)a[0].as_int));
}
static TuriValue native_hamt_keyeq(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    return turi_int((int64_t)(intptr_t)tur_hamt_keyeq((Hamt *)(intptr_t)a[0].as_int));
}
/* collection-multiword-element-boxing: hand back the address of the generic
 * struct-key content comparator (turi_struct_key_eq_c, defined in eval.c) as an
 * int carrier.  A struct/ADT key's MapKey `mk-cmp` :turi branch returns
 * `(struct-key-cmp)` so the comparator is a stampable C fn ptr -- identical shape
 * to the primitive `__inst_MapKey_mk_hycmp_cstr` native -- which makes the
 * interpreter content-key AND recover the comparator for structural
 * Eq[Map]/Eq[Set], matching the compiled tur_hamt_box_key_eq path. */
static TuriValue native_struct_key_cmp(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    return turi_int((int64_t)(intptr_t)&turi_struct_key_eq_c);
}
/* collection-multiword-element-boxing: uniform content hash for a struct/ADT key
 * -- a Hash :turi branch returns `(struct-hash p)` so the interpreter hash body
 * is per-struct boilerplate (no field-by-field expression). */
static TuriValue native_struct_hash(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    return turi_int(turi_struct_hash_c(a[0]));
}
static TuriValue native_hamt_get_dynamic(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 4) return turi_int(0);
    void *r = tur_hamt_get_dynamic((Hamt *)(intptr_t)a[0].as_int, a[1].as_int,
                                   (void *)(intptr_t)a[2].as_int,
                                   (void *)(intptr_t)a[3].as_int);
    return turi_int((int64_t)(intptr_t)r);
}
static TuriValue native_hamt_has_dynamic(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 4) return turi_bool(false);
    return turi_bool(tur_hamt_has_dynamic((Hamt *)(intptr_t)a[0].as_int, a[1].as_int,
                                          (void *)(intptr_t)a[2].as_int,
                                          (void *)(intptr_t)a[3].as_int));
}
/* map-hamt: read the HAMT pointer out of the { void* hamt } Map carrier. */
static TuriValue native_map_hamt(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    return turi_int((int64_t)(intptr_t)set_hamt(a[0]));
}

/* Interpreter vec element-tag side table (end-to-end-monomorphization, Vec
 * typed-pointer slice).
 *
 * A native vec stores raw int64 carrier cells in its `data` buffer, so the
 * value tag (TURI_FLOAT / TURI_CSTR / TURI_INT) is lost on push.  Before the
 * Vec typed-pointer migration, `vec-get`'s tyvar return type made the
 * `(:: (vec-get v i) :float)` read-back an EX_ASCRIBE that re-tagged the
 * carrier; now `vec-get` returns the concrete element type, so the elaborator
 * lowers that read to a *transparent* EX_REINTERPRET (tag-preserving by
 * design -- see eval.c) and the re-tag is lost.  Vecs are homogeneous
 * (tur-vec-homog__ enforces a single element type), so we record the element
 * tag once per vec-header pointer here and re-tag on get/pop.  Vecs built by
 * other natives (schema/json int64 buffers) are absent from the table and
 * default to TURI_INT, which is correct -- those only ever hold int carriers.
 * The table is process-lifetime (the interpreter never frees it); entries for
 * freed vecs are harmless (a later vec may reuse the address and overwrite the
 * stale tag on its first push). */
typedef struct { void *key; uint8_t tag; } TurVecTagEnt;
static TurVecTagEnt *g_vec_tag_tab = NULL;
static size_t g_vec_tag_cap = 0;
static size_t g_vec_tag_n   = 0;

static size_t vec_tag_probe(void *key) {
    size_t mask = g_vec_tag_cap - 1;
    size_t h = ((uintptr_t)key >> 4) & mask;
    while (g_vec_tag_tab[h].key && g_vec_tag_tab[h].key != key)
        h = (h + 1) & mask;
    return h;
}
static void vec_tag_set(void *key, uint8_t tag) {
    if (!key) return;
    if (g_vec_tag_n * 2 >= g_vec_tag_cap) {
        size_t old_cap = g_vec_tag_cap;
        TurVecTagEnt *old = g_vec_tag_tab;
        g_vec_tag_cap = old_cap ? old_cap * 2 : 64;
        g_vec_tag_tab = (TurVecTagEnt *)calloc(g_vec_tag_cap, sizeof(TurVecTagEnt));
        g_vec_tag_n = 0;
        for (size_t i = 0; i < old_cap; i++)
            if (old[i].key) {
                g_vec_tag_tab[vec_tag_probe(old[i].key)] = old[i];
                g_vec_tag_n++;
            }
        free(old);
    }
    size_t h = vec_tag_probe(key);
    if (!g_vec_tag_tab[h].key) g_vec_tag_n++;
    g_vec_tag_tab[h].key = key;
    g_vec_tag_tab[h].tag = tag;
}
static uint8_t vec_tag_get(void *key) {
    if (!key || g_vec_tag_cap == 0) return (uint8_t)TURI_INT;
    size_t h = vec_tag_probe(key);
    return g_vec_tag_tab[h].key ? g_vec_tag_tab[h].tag : (uint8_t)TURI_INT;
}
/* Re-tag a raw int64 carrier read from a vec cell according to the vec's
 * recorded homogeneous element tag, so float/cstr elements round-trip with
 * their value tag under --interpret (matching the compiled bit-reinterpret). */
static TuriValue vec_retag_cell(void *vec_key, int64_t cell) {
    switch ((int)vec_tag_get(vec_key)) {
        case TURI_FLOAT: { union { int64_t i; double d; } u; u.i = cell; return turi_float(u.d); }
        case TURI_CSTR:  return turi_cstr((const char *)(intptr_t)cell);
        case TURI_BOOL:  return turi_bool(cell != 0);
        /* A by-value aggregate element (struct/ADT, e.g. (Option int) pushed as a
         * TuriStruct) rides the cell as its pointer; re-tag it so the read-back
         * value is a real TURI_STRUCT and field/.value access works (a bare
         * turi_int would make .is-some/.value misread the struct as an int64
         * carrier).  Guard on non-null to leave a 0/nil carrier alone. */
        case TURI_STRUCT: return cell ? turi_struct_val((TuriStruct *)(intptr_t)cell)
                                      : turi_int(0);
        default:         return turi_int(cell);
    }
}

/* Vec layout: { int64_t *data; size_t len; size_t cap; }
 * Stored as int64_t[4]: [0]=data ptr (as int64_t), [1]=len, [2]=cap,
 * [3]=TuriCollBuf* tracking node (interp-collections-never-freed) so the
 * env-teardown pass and an explicit vec-free share one O(1) tombstone slot.
 * Only vec-new produces this box and only vec-get/len/cap/push/pop/set/free
 * read it, all of which touch [0..2] -- the extra slot is invisible to them. */
static void vec_buf_destroy(void *box) {
    int64_t *v = (int64_t *)box;
    free((void *)(intptr_t)v[0]);   /* the growable data buffer */
    free(v);
}
/* TR3: enumerate a Vec's cells for the eval-boundary sweep's conservative
 * mark.  vec_retag_cell types each cell from the vec's recorded element tag,
 * so a struct-tagged cell surfaces as a real TURI_STRUCT the marker can walk
 * into (a struct element may hold another collection's handle in a field) and
 * everything else surfaces as the carrier int candidate it is.  Every cell is
 * visited, so the enumeration is complete: return true. */
static bool vec_buf_scan(void *box, TuriCollBufMarkFn mark, void *ctx) {
    int64_t *v = (int64_t *)box;
    int64_t *data = (int64_t *)(intptr_t)v[0];
    if (!data) return true;
    for (int64_t i = 0; i < v[1]; i++)
        mark(vec_retag_cell(v, data[i]), ctx);
    return true;
}
static TuriValue native_vec_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)a; (void)n; (void)ud;
    int64_t *v = (int64_t *)calloc(4, sizeof(int64_t));
    if (!v) return turi_nil();
    v[3] = (int64_t)(intptr_t)turi_env_track_collection(env, v, vec_buf_destroy,
                                                        vec_buf_scan);
    TuriValue r = {0}; r.tag = TURI_INT; r.as_int = (int64_t)(intptr_t)v; return r;
}
static TuriValue native_vec_len(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    return turi_int(v ? v[1] : 0);
}
static TuriValue native_vec_capacity(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    return turi_int(v ? v[2] : 0);
}
static TuriValue native_vec_get(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    int64_t  i = a[1].as_int;
    if (!v || i < 0 || i >= v[1]) {
        fprintf(stderr, "vec index out of bounds\n");
        fflush(stderr);
        _exit(1);
    }
    int64_t *data = (int64_t *)(intptr_t)v[0];
    return vec_retag_cell(v, data[i]);
}
static TuriValue native_vec_push(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_nil();
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    if (!v) return turi_nil();
    int64_t *data = (int64_t *)(intptr_t)v[0];
    int64_t  len  = v[1];
    int64_t  cap  = v[2];
    if (len >= cap) {
        int64_t new_cap = cap > 0 ? cap * 2 : 4;
        int64_t *nd = (int64_t *)malloc((size_t)new_cap * sizeof(int64_t));
        if (!nd) return turi_nil();
        for (int64_t j = 0; j < len; j++) nd[j] = data[j];
        free(data);
        v[0] = (int64_t)(intptr_t)nd;
        v[2] = new_cap;
        data = nd;
    }
    data[len] = a[1].as_int;
    v[1] = len + 1;
    /* Record the homogeneous element tag so vec-get/pop re-tag float/cstr/bool
     * carriers (the buffer only holds raw int64 cells). */
    if (a[1].tag != TURI_INT) vec_tag_set(v, (uint8_t)a[1].tag);
    return turi_nil();
}
static TuriValue native_vec_pop(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    if (!v || v[1] == 0) {
        fprintf(stderr, "vec pop from empty vec\n");
        return turi_int(0);
    }
    int64_t *data = (int64_t *)(intptr_t)v[0];
    v[1]--;
    return vec_retag_cell(v, data[v[1]]);
}
static TuriValue native_vec_set(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 3) return turi_nil();
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    int64_t  i = a[1].as_int;
    if (!v || i < 0 || i >= v[1]) {
        fprintf(stderr, "vec index out of bounds\n");
        return turi_nil();
    }
    int64_t *data = (int64_t *)(intptr_t)v[0];
    data[i] = a[2].as_int;
    if (a[2].tag != TURI_INT) vec_tag_set(v, (uint8_t)a[2].tag);
    return turi_nil();
}
static TuriValue native_vec_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    if (!v) return turi_nil();
    /* interp-collections-never-freed: tombstone the tracking node so the
     * turi_env_free teardown pass does not free this buffer a second time. */
    turi_env_untrack_collection((TuriCollBuf *)(intptr_t)v[3]);
    int64_t *data = (int64_t *)(intptr_t)v[0];
    free(data);
    free(v);
    return turi_nil();
}

/* vec-set-o! -- interpreter override for the boxed-aware vec-set! helper.
 * The compiled body frees the old element box before overwriting when its
 * `boxed` flag (a[3]) is set; the tree-walker never boxes multi-word elements
 * (they ride as TuriStruct pointers it owns), so the flag is irrelevant here --
 * overwrite the slot exactly like native_vec_set.  vec-set! macro-expands to
 * `(vec-set-o! v i val (tur-vec-elem-wide? v))` on both paths. */
static TuriValue native_vec_set_o(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    return native_vec_set(env, a, n, ud);
}

/* vec-drop-last-o! -- interpreter override for vec-drop-last!'s helper.  Shrinks
 * len; the `boxed` flag (a[1]) is ignored (the interpreter frees no per-element
 * boxes -- elements ride as TuriStruct pointers freed at teardown).  Mirrors
 * native_vec_pop minus the returned value. */
static TuriValue native_vec_drop_last_o(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    if (!v || v[1] == 0) return turi_nil();
    v[1]--;
    return turi_nil();
}

/* vec-free-o -- interpreter override for the boxed-aware vec-free-o helper.
 * The compiled body frees per-element heap boxes when its `boxed` flag is set,
 * but the tree-walker never boxes multi-word elements (they ride as TuriStruct
 * pointers owned by the interpreter's own value model, freed at teardown), so
 * the `boxed` arg (a[1]) is irrelevant here: free the data buffer + header
 * exactly like native_vec_free.  vec-free macro-expands to `(vec-free-o v
 * (tur-vec-elem-wide? v))` on both paths, so this native is what backs
 * `(vec-free v)` under --interpret. */
static TuriValue native_vec_free_o(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    return native_vec_free(env, a, n, ud);
}

/* vec-eq? -- element-wise Vec equality.  vec.tur's body iterates the {data,len,
 * cap} struct and fat-dispatches the element comparator through a C function
 * pointer, which the simple inline-C executor cannot run; this native re-walks
 * the Vec (int64_t[3] = {data,len,cap}) and invokes the comparator (a turi
 * closure under --interpret) via turi_call, mirroring native_map_eq_raw.  This
 * is what makes recursive container values work -- e.g. a Map[K (Vec V)] whose
 * value comparator bottoms out in vec-eq?. */
static TuriValue native_vec_eq(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 3) return turi_bool(false);
    int64_t *v1 = (int64_t *)(intptr_t)a[0].as_int;
    int64_t *v2 = (int64_t *)(intptr_t)a[1].as_int;
    TuriValue cmp = a[2];
    int64_t len1 = v1 ? v1[1] : 0;
    int64_t len2 = v2 ? v2[1] : 0;
    if (len1 != len2) return turi_bool(false);
    int64_t *d1 = v1 ? (int64_t *)(intptr_t)v1[0] : NULL;
    int64_t *d2 = v2 ? (int64_t *)(intptr_t)v2[0] : NULL;
    for (int64_t i = 0; i < len1; i++) {
        TuriValue cargs[2];
        cargs[0].tag = TURI_INT; cargs[0].as_int = d1[i];
        cargs[1].tag = TURI_INT; cargs[1].as_int = d2[i];
        TuriValue rv = turi_call(env, cmp, cargs, 2);
        if (turi_is_error(rv) || env->throwing) return rv;  /* propagate callback error */
        bool eq = rv.tag == TURI_BOOL ? rv.as_bool : rv.as_int != 0;
        if (!eq) return turi_bool(false);
    }
    return turi_bool(true);
}

/* vec-new-filled: allocate a vec of size sz filled with init */
static TuriValue native_vec_new_filled(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t sz  = (n > 0) ? a[0].as_int : 0;
    int64_t val = (n > 1) ? a[1].as_int : 0;
    if (sz < 0) sz = 0;
    int64_t *v = (int64_t *)malloc(3 * sizeof(int64_t));
    if (!v) return turi_nil();
    int64_t *data = sz > 0 ? (int64_t *)malloc((size_t)sz * sizeof(int64_t)) : NULL;
    for (int64_t i = 0; i < sz; i++) data[i] = val;
    v[0] = (int64_t)(intptr_t)data; v[1] = sz; v[2] = sz;
    TuriValue ret = {0}; ret.tag = TURI_INT; ret.as_int = (int64_t)(intptr_t)v;
    return ret;
}

/* -------------------------------------------------------------------------
 * Registration entry point.  Binds every collection native name to its
 * override.  Called from turi_env_new (env.c) so collections resolve for any
 * interpreter env created through libturi.
 * ---------------------------------------------------------------------- */
/* show-concat: interpreter override for the inline-C helper in
 * stdlib/typeclass-show.tur.  Concatenates two NUL-terminated strings into a
 * fresh buffer so the pure-Turmeric collection Show instances (Show[Vec] /
 * Show[Set] / Show[Map]) run under the tree-walking interpreter and the REPL
 * instead of tripping the "inline-C not supported" guard.  Registered here (at
 * turi_env_new time, via turi_register_collection_natives) so it exists before
 * typeclass-show.tur's defn is elaborated -- both the `tur repl` and
 * `--interpret` entry points share this path.  Neither input is freed. */
static TuriValue native_show_concat(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    const char *sa = (n > 0 && a[0].tag == TURI_CSTR && a[0].as_cstr) ? a[0].as_cstr : "";
    const char *sb = (n > 1 && a[1].tag == TURI_CSTR && a[1].as_cstr) ? a[1].as_cstr : "";
    size_t la = strlen(sa), lb = strlen(sb);
    char *out = (char *)malloc(la + lb + 1);
    if (!out) { TuriValue v = {0}; v.tag = TURI_NIL; return v; }
    memcpy(out, sa, la);
    memcpy(out + la, sb, lb);
    out[la + lb] = '\0';
    TuriValue v = {0}; v.tag = TURI_CSTR; v.as_cstr = out; return v;
}

void turi_register_collection_natives(TuriEnv *env) {
    turi_env_register_native(env, "show-concat", native_show_concat, NULL);
    turi_env_register_native(env, "tur_hamt_new", native_tur_hamt_new, NULL);
    turi_env_register_native(env, "tur_hamt_free", native_tur_hamt_free, NULL);
    turi_env_register_native(env, "tur_hamt_retain", native_tur_hamt_retain, NULL);
    turi_env_register_native(env, "tur_hamt_count", native_tur_hamt_count, NULL);
    turi_env_register_native(env, "tur_hamt_set", native_tur_hamt_set, NULL);
    turi_env_register_native(env, "tur_hamt_del", native_tur_hamt_del, NULL);
    turi_env_register_native(env, "tur_hamt_has", native_tur_hamt_has, NULL);
    turi_env_register_native(env, "tur_hamt_get", native_tur_hamt_get, NULL);
    turi_env_register_native(env, "tur_hamt_set_cstr", native_tur_hamt_set_cstr, NULL);
    turi_env_register_native(env, "tur_hamt_del_cstr", native_tur_hamt_del_cstr, NULL);
    turi_env_register_native(env, "tur_hamt_has_cstr", native_tur_hamt_has_cstr, NULL);
    turi_env_register_native(env, "tur_hamt_get_cstr", native_tur_hamt_get_cstr, NULL);
    turi_env_register_native(env, "tur_hamt_merge", native_tur_hamt_merge, NULL);
    turi_env_register_native(env, "tur_hamt_hash_str", native_tur_hamt_hash_str, NULL);
    turi_env_register_native(env, "tur_hamt_hash_ptr", native_tur_hamt_hash_ptr, NULL);
    turi_env_register_native(env, "tur_hamt_iter_init", native_tur_hamt_iter_init, NULL);
    turi_env_register_native(env, "tur_hamt_iter_free", native_tur_hamt_iter_free, NULL);
    turi_env_register_native(env, "tur_hamt_iter_next", native_tur_hamt_iter_next, NULL);
    turi_env_register_native(env, "tur_hamt_show", native_tur_hamt_show, NULL);
    turi_env_register_native(env, "tur_hamt_merge_with", native_hamt_merge_with, NULL);
    turi_env_register_native(env, "tur_hamt_transient", native_tur_hamt_transient, NULL);
    turi_env_register_native(env, "tur_hamt_transient_set", native_tur_hamt_transient_set, NULL);
    turi_env_register_native(env, "tur_hamt_transient_del", native_tur_hamt_transient_del, NULL);
    turi_env_register_native(env, "tur_hamt_persistent", native_tur_hamt_persistent, NULL);

    turi_env_register_native(env, "set-new", native_set_new, NULL);
    /* set-add / set-remove / set-member? are now pure-Turmeric defns in
     * stdlib/set.tur that dispatch MapKey[A] and delegate to the content-keyed
     * -eq-o raw layer below.  Registering the old plain-tur_hamt_set natives
     * here would shadow those defns and silently drop the comparator (content
     * dedup / membership would fall back to pointer identity), so they are not
     * registered -- the interpreter runs the Turmeric bodies over the -eq-o
     * natives, exactly like Map's macro layer over map-*-eq-o. */
    turi_env_register_native(env, "set-add-eq-o", native_set_add_eq_o, NULL);
    turi_env_register_native(env, "set-has-eq-o?", native_set_has_eq_o, NULL);
    turi_env_register_native(env, "set-del-eq-o", native_set_del_eq_o, NULL);
    turi_env_register_native(env, "set-count", native_set_count, NULL);
    turi_env_register_native(env, "set-free", native_set_free, NULL);
    turi_env_register_native(env, "set-union", native_set_union, NULL);
    turi_env_register_native(env, "set-intersect", native_set_intersect, NULL);
    turi_env_register_native(env, "set-diff", native_set_diff, NULL);
    turi_env_register_native(env, "set-eq?", native_set_eq, NULL);
    turi_env_register_native(env, "set-eq-cmp?", native_set_eq_cmp, NULL);

    turi_env_register_native(env, "__inst_MapKey_mk_hycmp_int", native_mk_cmp_int, NULL);
    turi_env_register_native(env, "__inst_MapKey_mk_hycmp_cstr", native_mk_cmp_cstr, NULL);
    turi_env_register_native(env, "__inst_MapKey_mk_hycmp_float", native_mk_cmp_f64, NULL);
    turi_env_register_native(env, "__inst_MapKey_mk_hycmp_float32", native_mk_cmp_f32, NULL);
    turi_env_register_native(env, "__inst_MapKey_mk_hybox_cstr", native_mk_box_cstr, NULL);
    turi_env_register_native(env, "__inst_MapKey_mk_hybox_float", native_mk_box_f64, NULL);
    turi_env_register_native(env, "__inst_MapKey_mk_hybox_float32", native_mk_box_f32, NULL);
    turi_env_register_native(env, "__inst_Hash_hash_cstr", native_hash_cstr, NULL);
    turi_env_register_native(env, "__inst_Hash_hash_float", native_hash_f64, NULL);
    turi_env_register_native(env, "__inst_Hash_hash_float32", native_hash_f32, NULL);

    turi_env_register_native(env, "map-new", native_set_new, NULL);
    turi_env_register_native(env, "map-assoc-eq-o", native_map_assoc_eq_o, NULL);
    turi_env_register_native(env, "map-get-eq-o", native_map_get_eq_o, NULL);
    turi_env_register_native(env, "map-has-eq-o?", native_map_has_eq_o, NULL);
    turi_env_register_native(env, "map-dissoc-eq-o", native_map_dissoc_eq_o, NULL);
    turi_env_register_native(env, "map-assoc-eq", native_map_assoc_eq, NULL);
    turi_env_register_native(env, "map-get-eq", native_map_get_eq, NULL);
    turi_env_register_native(env, "map-has-eq?", native_map_has_eq, NULL);
    turi_env_register_native(env, "map-dissoc-eq", native_map_dissoc_eq, NULL);
    turi_env_register_native(env, "map-count", native_map_count, NULL);
    turi_env_register_native(env, "map-merge", native_map_merge, NULL);
    turi_env_register_native(env, "map-free", native_map_free, NULL);
    turi_env_register_native(env, "map-eq-raw?", native_map_eq_raw, NULL);
    turi_env_register_native(env, "map-eq-raw-k?", native_map_eq_raw_k, NULL);
    turi_env_register_native(env, "tur_hamt_iter_alloc", native_hamt_iter_alloc, NULL);
    turi_env_register_native(env, "tur_hamt_iter_destroy", native_hamt_iter_destroy, NULL);
    turi_env_register_native(env, "tur_hamt_iter_advance", native_hamt_iter_advance, NULL);
    turi_env_register_native(env, "tur_hamt_iter_cur_hash", native_hamt_iter_cur_hash, NULL);
    turi_env_register_native(env, "tur_hamt_iter_cur_key", native_hamt_iter_cur_key, NULL);
    turi_env_register_native(env, "tur_hamt_iter_cur_val", native_hamt_iter_cur_val, NULL);
    turi_env_register_native(env, "tur_hamt_keyeq", native_hamt_keyeq, NULL);
    turi_env_register_native(env, "struct-key-cmp", native_struct_key_cmp, NULL);
    turi_env_register_native(env, "struct-hash", native_struct_hash, NULL);
    turi_env_register_native(env, "tur_hamt_get_dynamic", native_hamt_get_dynamic, NULL);
    turi_env_register_native(env, "tur_hamt_has_dynamic", native_hamt_has_dynamic, NULL);
    turi_env_register_native(env, "map-hamt", native_map_hamt, NULL);
    turi_env_register_native(env, "map-iter-cur-val-as", native_hamt_iter_cur_val, NULL);
    turi_env_register_native(env, "map-get-dynamic-as", native_hamt_get_dynamic, NULL);
    turi_env_register_native(env, "tur-map-homog__", native_map_homog, NULL);

    turi_env_register_native(env, "vec-new", native_vec_new, NULL);
    turi_env_register_native(env, "tur-vec-homog__", native_coll_noop, NULL);
    turi_env_register_native(env, "vec-len", native_vec_len, NULL);
    turi_env_register_native(env, "vec-capacity", native_vec_capacity, NULL);
    turi_env_register_native(env, "vec-get", native_vec_get, NULL);
    turi_env_register_native(env, "vec-push!", native_vec_push, NULL);
    turi_env_register_native(env, "vec-push-ptr!", native_vec_push, NULL);
    turi_env_register_native(env, "vec-pop!", native_vec_pop, NULL);
    turi_env_register_native(env, "vec-set!", native_vec_set, NULL);
    turi_env_register_native(env, "vec-set-o!", native_vec_set_o, NULL);
    turi_env_register_native(env, "vec-drop-last-o!", native_vec_drop_last_o, NULL);
    turi_env_register_native(env, "vec-free", native_vec_free, NULL);
    turi_env_register_native(env, "vec-free-o", native_vec_free_o, NULL);
    turi_env_register_native(env, "vec-eq?", native_vec_eq, NULL);
    turi_env_register_native(env, "vec-new-filled", native_vec_new_filled, NULL);

    turi_env_register_native(env, "hamt-new", native_tur_hamt_new, NULL);
    turi_env_register_native(env, "hamt-free", native_tur_hamt_free, NULL);
    turi_env_register_native(env, "hamt-set", native_tur_hamt_set, NULL);
    turi_env_register_native(env, "hamt-get", native_tur_hamt_get, NULL);
    turi_env_register_native(env, "hamt-hash-ptr", native_tur_hamt_hash_ptr, NULL);
}
