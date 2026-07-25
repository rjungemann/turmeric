/* tests/turi/gc-runtime-copy-parity.c -- DEDUP-4 (docs/archive/gc-cycle-collection-plan.md).
 *
 * The GC exists twice: src/runtime/{rc,gc,rc_free_queue}.c (linked into the
 * interpreter, libturi and every embedder) and a hand-written copy emitted into
 * every compiled program by src/compiler/emit_module.c.  Divergence between the
 * two has now produced four bugs, each invisible to half the suite by
 * construction -- compiled fixtures exercise only the emitted copy, this path
 * only the runtime copy.
 *
 * These are behavioral parity assertions on the RUNTIME copy, pinning the
 * semantics the emitted copy has always had.  They are deliberately written
 * against the same entry points the interpreter dispatches to for
 * `(gc-enable!)` / `(gc!)` (see the inline-C dispatch in src/turi/eval.c).
 *
 * Exit status 0 = all parity assertions hold.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "rc.h"
#include "gc.h"

extern uint64_t gc_collections;
extern uint64_t gc_objects_freed;

static int failures = 0;

static void check(int cond, const char *what) {
    if (cond) {
        printf("PASS %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        failures++;
    }
}

/* DEDUP-4: gc_collect() gates on gc_enabled AND gc_mode, and gc_mode starts at
 * GC_DISABLED.  A gc_enable() that sets only the flag makes every subsequent
 * collection a silent no-op -- which is exactly what `(gc-enable!)` followed by
 * `(gc!)` did in the interpreter before this was reconciled.  The emitted copy
 * has always defaulted to GC_MANUAL here. */
static void test_enable_implies_manual_mode(void) {
    gc_disable();
    check(gc_mode == GC_DISABLED, "gc_disable-clears-mode");

    gc_enable();
    check(gc_enabled, "gc_enable-sets-flag");
    check(gc_mode == GC_MANUAL, "gc_enable-defaults-to-manual-mode");

    uint64_t before = gc_collections;
    gc_collect();
    check(gc_collections == before + 1, "gc_collect-runs-after-bare-gc_enable");
}

/* gc_disable() must clear the mode too, so a later gc_collect() is inert
 * without a second gc_enable().  (The emitted copy does both.) */
static void test_disable_stops_collection(void) {
    gc_enable();
    gc_disable();
    uint64_t before = gc_collections;
    gc_collect();
    check(gc_collections == before, "gc_collect-inert-after-gc_disable");
}

/* An explicit gc_set_mode must survive gc_enable -- enable only supplies a
 * default for the DISABLED case, it does not stomp a caller's choice. */
static void test_enable_preserves_explicit_mode(void) {
    gc_disable();
    gc_set_mode(GC_THRESHOLD);
    gc_enable();
    check(gc_mode == GC_THRESHOLD, "gc_enable-preserves-explicit-mode");
    gc_disable();
}

/* DEDUP-4a: the collector must actually reclaim a strong cycle on this path.
 *
 * Until the gc_enable() fix above, gc_mode was never anything but GC_DISABLED
 * for the runtime copy -- nothing in the tree called gc_init() or
 * gc_set_mode() either -- so the runtime's mark / trial-deletion code had
 * never executed at all.  This is the first test to run it.
 *
 * Topology mirrors what the codegen emits for two mutually-referencing
 * `rc<T>` values: each block is RCK_EXISTENTIAL / RCEXP_RC, whose payload is a
 * separately-malloc'd cell holding a pointer to the peer's control block.
 * That is the shape gc_each_child knows how to walk.
 *
 *   A.value -> &B      B.value -> &A       (each holding a strong ref)
 *
 * After dropping the two external references both have strong_count == 1,
 * sustained only by each other: unreachable, and uncollectable by pure
 * refcounting.  This is the CG2 case. */
#define TY_RC_KIND 9   /* TypeKind TY_RC; pinned by the _Static_assert in emit_module.c */

static RcControlBlock *make_rc_cell(void) {
    /* value_size 0 + an explicit heap cell, matching the codegen: the drop
     * glue for TY_RC free()s the payload, so it must not be the inline
     * (cb + 1) region. */
    RcControlBlock *cb = rc_cb_alloc_kinded(0, TY_RC_KIND, NULL,
                                           RCK_EXISTENTIAL, RCEXP_RC);
    void *cell = calloc(1, sizeof(int64_t));
    rc_set_value(cb, cell, NULL);
    return cb;
}

static void test_collects_strong_cycle(void) {
    gc_enable();

    RcControlBlock *a = make_rc_cell();
    RcControlBlock *b = make_rc_cell();

    /* Wire the cycle, taking a strong ref in each direction. */
    *(int64_t *)a->value = (int64_t)(intptr_t)b;
    rc_strong_increment(b);
    *(int64_t *)b->value = (int64_t)(intptr_t)a;
    rc_strong_increment(a);

    /* Drop the external handles.  Neither reaches zero -- the cycle holds. */
    rc_strong_decrement(a);
    rc_strong_decrement(b);
    check(rc_strong_count(a) == 1 && rc_strong_count(b) == 1,
          "cycle-survives-refcounting");

    uint64_t freed_before = gc_objects_freed;
    gc_collect();
    check(gc_objects_freed >= freed_before + 2, "gc_collect-reclaims-strong-cycle");

    gc_disable();
}

/* The mirror of the above, and the assertion that actually has teeth: a cycle
 * that is still REACHABLE must survive.  A collector that passes
 * "reclaims-strong-cycle" by being over-eager fails here. */
static void test_live_cycle_survives(void) {
    gc_enable();

    RcControlBlock *a = make_rc_cell();
    RcControlBlock *b = make_rc_cell();
    *(int64_t *)a->value = (int64_t)(intptr_t)b;
    rc_strong_increment(b);
    *(int64_t *)b->value = (int64_t)(intptr_t)a;
    rc_strong_increment(a);

    /* Drop only ONE external handle: `a` stays externally rooted at 2. */
    rc_strong_decrement(b);
    check(rc_strong_count(a) == 2, "live-cycle-external-root-held");

    gc_collect();
    /* Still readable after collection -- neither block was freed. */
    check(rc_strong_count(a) == 2, "live-cycle-survives-collection");
    check(*(int64_t *)a->value == (int64_t)(intptr_t)b, "live-cycle-edge-intact");

    gc_disable();
}

/* CG4: a collected cycle member with a live weak observer must become a
 * zombie (strong 0, block retained) rather than being freed under the
 * observer's feet -- rc_is_alive must answer false without dereferencing
 * freed memory. */
static void test_cycle_with_weak_observer(void) {
    gc_enable();

    RcControlBlock *a = make_rc_cell();
    RcControlBlock *b = make_rc_cell();
    *(int64_t *)a->value = (int64_t)(intptr_t)b;
    rc_strong_increment(b);
    *(int64_t *)b->value = (int64_t)(intptr_t)a;
    rc_strong_increment(a);

    rc_weak_increment(a);          /* an outside observer holds a weak ref to `a` */
    rc_strong_decrement(a);
    rc_strong_decrement(b);

    gc_collect();

    /* `a` must still be a readable control block, reporting itself dead. */
    check(!rc_is_alive(a), "weak-observed-cycle-member-reports-dead");
    check(rc_weak_count(a) == 1, "weak-observed-cycle-member-retained");
    check(rc_upgrade(a) == NULL, "weak-observed-cycle-member-upgrade-fails");

    rc_weak_decrement(a);          /* last observer leaves; block may go now */
    gc_disable();
}

/* DEDUP-4a: a refcount decrement must NEVER trigger a collection.
 *
 * gc_add_suspect runs on the rc_strong_decrement path.  It used to call
 * gc_collect() once the suspect buffer reached GC_MAX_SUSPECTS and then
 * recurse -- reentering mark/sweep from inside a caller's decrement.  Under
 * GC_MANUAL the only thing that may collect is an explicit gc_collect().
 *
 * Pushes comfortably past the old 4096 watermark and asserts the collection
 * counter never moved, then that one explicit collect drains the buffer. */
static void test_decrement_never_collects(void) {
    enum { N = 5000 };   /* > GC_MAX_SUSPECTS (4096) */
    gc_enable();
    check(gc_mode == GC_MANUAL, "manual-mode-for-watermark-test");

    RcControlBlock **blocks = (RcControlBlock **)calloc(N, sizeof(*blocks));
    uint64_t collections_before = gc_collections;

    for (int i = 0; i < N; i++) {
        blocks[i] = make_rc_cell();
        rc_strong_increment(blocks[i]);        /* 1 -> 2 */
        rc_strong_decrement(blocks[i]);        /* 2 -> 1: offers a candidate root */
    }
    check(gc_collections == collections_before, "decrement-past-watermark-never-collects");
    check(gc_suspect_count >= 4096, "suspect-buffer-grew-past-old-cap");

    gc_collect();
    check(gc_collections == collections_before + 1, "explicit-collect-still-works");

    for (int i = 0; i < N; i++) rc_strong_decrement(blocks[i]);
    free(blocks);
    gc_disable();
}

int main(void) {
    test_enable_implies_manual_mode();
    test_disable_stops_collection();
    test_enable_preserves_explicit_mode();
    test_collects_strong_cycle();
    test_live_cycle_survives();
    test_cycle_with_weak_observer();
    test_decrement_never_collects();

    if (failures) {
        printf("gc-runtime-copy-parity: %d failed\n", failures);
        return 1;
    }
    printf("gc-runtime-copy-parity: all passed\n");
    return 0;
}
