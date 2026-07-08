/* test_hamt_del_lineage.c -- delete-path refcount regression
 * (hamt-delete-sibling-refcount).
 *
 * `tur_hamt_del` returns a new persistent map that shares nodes with its
 * parent.  When a child subtree collapses to empty, the bitmap/array collapse
 * arm in node_delete used to release the dropped child on the *original*
 * (shared, immutable) node's behalf -- under-retaining it by one.  Freeing the
 * whole lineage (parent + delete-derived child) then released that node once
 * too often: a heap use-after-free / double free.
 *
 * These cases build a lineage, delete a key so a bitmap slot collapses, then
 * free every version.  Run under AddressSanitizer (+LSan on Linux): the
 * acceptance is no double-free / use-after-free and no leak.  Assoc-only
 * lineages were always clean and are kept here as a control.
 *
 * Built and run by tests/run-hamt-del-lineage.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include "hamt.h"

/* Pointer-identity keys: distinct heap words so the default comparator treats
 * them as distinct.  Values are stable, caller-owned storage. */
static int g_vals[8];

/* Case 1: the reported repro exactly -- two keys in distinct level-0 slots,
 * delete one so its slot collapses, free the whole lineage. */
static void test_flat_collapse(void) {
    printf("Testing flat bitmap-slot collapse on delete...\n");
    void *k1 = (void *)0x1001;
    void *k2 = (void *)0x2002;

    Hamt *e  = tur_hamt_new();
    Hamt *s1 = tur_hamt_set(e,  1, k1, &g_vals[0]);   /* chunk0(1) = 1 */
    Hamt *s2 = tur_hamt_set(s1, 2, k2, &g_vals[1]);   /* chunk0(2) = 2 */
    Hamt *s3 = tur_hamt_del(s2, 2, k2);               /* drop k2's slot */

    assert(tur_hamt_count(s2) == 2);
    assert(tur_hamt_count(s3) == 1);
    /* s2 unchanged by the delete (persistence); s3 no longer has k2. */
    assert(tur_hamt_get(s2, 2, k2) == &g_vals[1]);
    assert(tur_hamt_get(s3, 2, k2) == NULL);
    assert(tur_hamt_get(s3, 1, k1) == &g_vals[0]);

    /* Free the full lineage -- the shared collision node under k2 must be
     * freed exactly once. */
    tur_hamt_free(e);
    tur_hamt_free(s1);
    tur_hamt_free(s2);
    tur_hamt_free(s3);
    printf("  PASS: flat collapse (no double-free)\n");
}

/* Case 2: nested collapse.  k1 and k3 share level-0 chunk but diverge at
 * level 1 (so k1's slot holds a nested bitmap); deleting k3 collapses that
 * nested bitmap, exercising the collapse arm below the root. */
static void test_nested_collapse(void) {
    printf("Testing nested bitmap collapse on delete...\n");
    void *k1 = (void *)0x1001;
    void *k2 = (void *)0x2002;
    void *k3 = (void *)0x3003;
    /* chunk0(1)=1, chunk0(33)=1 (share), chunk1(1)=0, chunk1(33)=1 (diverge). */
    Hamt *e  = tur_hamt_new();
    Hamt *a  = tur_hamt_set(e, 1,  k1, &g_vals[0]);
    Hamt *b  = tur_hamt_set(a, 2,  k2, &g_vals[1]);
    Hamt *c  = tur_hamt_set(b, 33, k3, &g_vals[2]);   /* forces nested bitmap */
    Hamt *d  = tur_hamt_del(c, 33, k3);               /* collapse the nest */

    assert(tur_hamt_count(c) == 3);
    assert(tur_hamt_count(d) == 2);
    assert(tur_hamt_get(c, 33, k3) == &g_vals[2]);    /* c unchanged */
    assert(tur_hamt_get(d, 33, k3) == NULL);
    assert(tur_hamt_get(d, 1,  k1) == &g_vals[0]);
    assert(tur_hamt_get(d, 2,  k2) == &g_vals[1]);

    tur_hamt_free(e);
    tur_hamt_free(a);
    tur_hamt_free(b);
    tur_hamt_free(c);
    tur_hamt_free(d);
    printf("  PASS: nested collapse (no double-free)\n");
}

/* Case 3: delete the last remaining key so the root collapses to NULL, while
 * an earlier version still references the (now shared) subtree. */
static void test_collapse_to_empty(void) {
    printf("Testing collapse-to-empty-root on delete...\n");
    void *k1 = (void *)0x1001;

    Hamt *e  = tur_hamt_new();
    Hamt *s1 = tur_hamt_set(e,  1, k1, &g_vals[0]);
    Hamt *s2 = tur_hamt_del(s1, 1, k1);               /* root -> empty */

    assert(tur_hamt_count(s1) == 1);
    assert(tur_hamt_count(s2) == 0);
    assert(tur_hamt_get(s1, 1, k1) == &g_vals[0]);    /* s1 unchanged */
    assert(tur_hamt_get(s2, 1, k1) == NULL);

    tur_hamt_free(e);
    tur_hamt_free(s1);
    tur_hamt_free(s2);
    printf("  PASS: collapse to empty (no double-free)\n");
}

/* Control: assoc-only lineage (no delete) has always been clean; keep it so a
 * regression that breaks assoc refcounting is caught by the same harness. */
static void test_assoc_control(void) {
    printf("Testing assoc-only control lineage...\n");
    void *k1 = (void *)0x1001;
    void *k2 = (void *)0x2002;
    void *k3 = (void *)0x3003;

    Hamt *e  = tur_hamt_new();
    Hamt *s1 = tur_hamt_set(e,  1, k1, &g_vals[0]);
    Hamt *s2 = tur_hamt_set(s1, 2, k2, &g_vals[1]);
    Hamt *s3 = tur_hamt_set(s2, 3, k3, &g_vals[2]);
    assert(tur_hamt_count(s3) == 3);

    tur_hamt_free(e);
    tur_hamt_free(s1);
    tur_hamt_free(s2);
    tur_hamt_free(s3);
    printf("  PASS: assoc control\n");
}

int main(void) {
    printf("=== HAMT delete-path lineage refcount tests ===\n");
    test_flat_collapse();
    test_nested_collapse();
    test_collapse_to_empty();
    test_assoc_control();
    printf("=== all hamt-del-lineage tests passed ===\n");
    return 0;
}
