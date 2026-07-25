/* tests/turi/gc-runtime-copy-parity.c -- DEDUP-4 (gc-cycle-collection-plan).
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

int main(void) {
    test_enable_implies_manual_mode();
    test_disable_stops_collection();
    test_enable_preserves_explicit_mode();

    if (failures) {
        printf("gc-runtime-copy-parity: %d failed\n", failures);
        return 1;
    }
    printf("gc-runtime-copy-parity: all passed\n");
    return 0;
}
