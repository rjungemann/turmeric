/* test_hamt_owned_keys.c -- WKC2 boxed-key ownership unit test.
 *
 * Exercises the refcount-aware boxed-key carrier (float32-map-key-carrier-plan,
 * WKC2): build wide-keyed maps with structural sharing, hash collisions,
 * content updates, and deletes, then drop every version. Run under
 * AddressSanitizer + LeakSanitizer; the acceptance is that the whole thing is
 * leak-clean (every box freed exactly once) and double-free / use-after-free
 * clean (a box shared across persistent versions is freed only on the last
 * drop).
 *
 * Built and run by tests/run-hamt-owned-keys.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

#include "hamt.h"

/* Comparator over boxed double keys: the carrier words are payload pointers. */
static bool dbl_key_eq(int64_t a, int64_t b) {
    const double *pa = (const double *)(intptr_t)a;
    const double *pb = (const double *)(intptr_t)b;
    return *pa == *pb;
}

static void *box_dbl(double d) {
    return tur_hamt_box_key(&d, sizeof d);
}

static int g_vals[64];  /* stable value storage (vals stay caller-owned) */

/* Build a wide-keyed map of `n` doubles, all sharing ONE hash so they land in a
 * single collision chain -- this maximises collision_node_copy churn (retain on
 * every structural copy) and is the hardest case for the refcount discipline. */
static Hamt *build_collision_map(uint64_t hash, const double *keys, int n) {
    Hamt *m = tur_hamt_new();
    tur_hamt_key_ops ops = tur_hamt_box_key_ops();
    for (int i = 0; i < n; i++) {
        Hamt *next = tur_hamt_set_eq_owned(m, hash, box_dbl(keys[i]),
                                           &g_vals[i], dbl_key_eq, ops);
        tur_hamt_free(m);
        m = next;
    }
    return m;
}

static void test_collision_sharing(void) {
    printf("Testing boxed-key collision chain + structural sharing...\n");
    const uint64_t H = 0x1234;  /* one shared hash -> forced collision chain */
    double keys[] = { 1.5, 2.5, 3.5, 4.5, 5.5 };
    int n = (int)(sizeof keys / sizeof keys[0]);
    tur_hamt_key_ops ops = tur_hamt_box_key_ops();

    Hamt *base = build_collision_map(H, keys, n);
    assert(tur_hamt_count(base) == (uint32_t)n);

    /* Lookups via transient probe boxes (released inside _get/_has). */
    for (int i = 0; i < n; i++) {
        assert(tur_hamt_has_eq_owned(base, H, box_dbl(keys[i]), dbl_key_eq, ops));
        int *v = (int *)tur_hamt_get_eq_owned(base, H, box_dbl(keys[i]),
                                              dbl_key_eq, ops);
        assert(v == &g_vals[i]);
    }
    /* A fractional key never truncates: 2.5 must NOT be found as 2.0. */
    double two = 2.0;
    assert(!tur_hamt_has_eq_owned(base, H, tur_hamt_box_key(&two, sizeof two),
                                  dbl_key_eq, ops));

    /* Fork several persistent versions off `base` (structural sharing of the
     * collision node -> shared boxed keys across versions). */
    Hamt *v_upd = tur_hamt_set_eq_owned(base, H, box_dbl(3.5), &g_vals[10],
                                        dbl_key_eq, ops);  /* update 3.5 */
    Hamt *v_ins = tur_hamt_set_eq_owned(base, H, box_dbl(9.5), &g_vals[11],
                                        dbl_key_eq, ops);  /* insert 9.5 */
    Hamt *v_del = tur_hamt_del_eq_owned(base, H, box_dbl(1.5), dbl_key_eq, ops);

    assert(tur_hamt_count(v_upd) == (uint32_t)n);       /* update: same count */
    assert(tur_hamt_count(v_ins) == (uint32_t)n + 1);   /* insert */
    assert(tur_hamt_count(v_del) == (uint32_t)n - 1);   /* delete */

    /* base is unchanged by the forks (persistence). */
    assert(tur_hamt_count(base) == (uint32_t)n);
    {
        int *v = (int *)tur_hamt_get_eq_owned(base, H, box_dbl(3.5), dbl_key_eq, ops);
        assert(v == &g_vals[2]);   /* base still maps 3.5 -> original */
        v = (int *)tur_hamt_get_eq_owned(v_upd, H, box_dbl(3.5), dbl_key_eq, ops);
        assert(v == &g_vals[10]);  /* v_upd sees the updated value */
        assert(tur_hamt_has_eq_owned(base, H, box_dbl(1.5), dbl_key_eq, ops));
        assert(!tur_hamt_has_eq_owned(v_del, H, box_dbl(1.5), dbl_key_eq, ops));
    }

    /* Drop in an order that exercises freeing a shared collision node both
     * before and after its derived versions. */
    tur_hamt_free(v_upd);
    tur_hamt_free(base);   /* base freed while v_ins/v_del still share its keys */
    tur_hamt_free(v_del);
    tur_hamt_free(v_ins);
    printf("  PASS: collision chain + sharing (leak/double-free clean)\n");
}

static void test_spread_keys(void) {
    printf("Testing boxed keys spread across the trie...\n");
    tur_hamt_key_ops ops = tur_hamt_box_key_ops();
    Hamt *m = tur_hamt_new();
    /* Distinct hashes so keys land in different bitmap/array slots (no
     * collision chain): exercises the inherit-key_ops-through-set path and the
     * tur_hamt_free hook on a deep tree. */
    for (int i = 0; i < 40; i++) {
        double d = (double)i + 0.25;
        Hamt *next = tur_hamt_set_eq_owned(m, (uint64_t)(i * 0x9E37 + 1),
                                           box_dbl(d), &g_vals[i % 64],
                                           dbl_key_eq, ops);
        tur_hamt_free(m);
        m = next;
    }
    assert(tur_hamt_count(m) == 40);
    tur_hamt_free(m);
    printf("  PASS: spread keys\n");
}

static void test_one_word_unchanged(void) {
    printf("Testing one-word (unowned) keys still never freed...\n");
    /* ops == {NULL,NULL}: keys are fake inline pointers (intptr ints); the
     * HAMT must never retain/free them.  A leak/UAF here would mean the
     * ownership hook fired on a non-boxed key. */
    tur_hamt_key_ops none = { NULL, NULL };
    Hamt *m = tur_hamt_new();
    for (int64_t i = 1; i <= 20; i++) {
        Hamt *next = tur_hamt_set_eq_owned(m, (uint64_t)i, (void *)(intptr_t)i,
                                           &g_vals[i % 64], NULL, none);
        tur_hamt_free(m);
        m = next;
    }
    assert(tur_hamt_count(m) == 20);
    assert(tur_hamt_get_eq_owned(m, 7, (void *)(intptr_t)7, NULL, none) != NULL);
    tur_hamt_free(m);
    printf("  PASS: one-word keys untouched\n");
}

int main(void) {
    printf("=== WKC2 boxed-key ownership tests ===\n");
    test_collision_sharing();
    test_spread_keys();
    test_one_word_unchanged();
    printf("=== all WKC2 tests passed ===\n");
    return 0;
}
