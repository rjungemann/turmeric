/* region_unit.c -- RM3 region allocator unit test.
 *
 * The properties worth pinning are the SAFETY ones, not that allocation
 * returns non-NULL.  A region is only shippable because of them:
 *
 *   1. `tur_region_owns` is true for everything the region handed out, LIVE OR
 *      RETIRED.  This is the guard the reclamation plan requires on every free
 *      path -- a pointer into region memory must never reach `free()`, and a
 *      value that outlived its generation is exactly the case where somebody
 *      would otherwise try.
 *   2. A pop WITHOUT reclaim does not reclaim.  The conservative default has
 *      to actually be conservative: memory stays mapped and owned.
 *   3. A mismatched depth is refused rather than rewinding someone else's
 *      generation.  A region form unwound by a panic can leave the stack
 *      deeper than the caller thinks; refusing keeps the failure "no saving"
 *      instead of "reclaimed memory still in use".
 *   4. Nesting: an inner generation's pop does not disturb the outer one, and
 *      allocation always lands in the innermost.
 *   5. With no region open, allocation returns NULL so the caller falls back
 *      to malloc.  A region is an optimisation, never a requirement.
 *
 * See docs/upcoming/regions-plan.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "region.h"

static int failures = 0;

static void check(int cond, const char *what) {
    if (!cond) { printf("FAIL: %s\n", what); failures++; }
}

int main(void) {
    /* (5) no region open -> NULL, so the caller uses malloc. */
    check(!tur_region_active(), "no region active at start");
    check(tur_region_alloc(16) == NULL, "alloc with no region returns NULL");
    check(!tur_region_owns((const void *)0x1234), "owns() false with no region");

    /* (1) live ownership. */
    int d1 = tur_region_push();
    check(d1 == 1, "first push has depth 1");
    check(tur_region_active(), "region active after push");
    void *a = tur_region_alloc(64);
    check(a != NULL, "alloc inside a region succeeds");
    check(tur_region_owns(a), "region owns its own allocation");
    memset(a, 0xAB, 64);   /* writable, and ASan-visible if the slab is short */

    /* (4) nesting: the inner generation takes new allocations, and popping it
     * leaves the outer one intact and still owning what it handed out. */
    int d2 = tur_region_push();
    check(d2 == 2, "nested push has depth 2");
    void *b = tur_region_alloc(32);
    check(b != NULL && b != a, "inner alloc is distinct");
    check(tur_region_owns(b), "owns inner allocation");
    tur_region_pop_reclaim(d2);
    check(tur_region_active(), "outer region still active after inner pop");
    check(tur_region_owns(a), "outer allocation still owned after inner pop");

    /* (3) a mismatched depth must be refused.  d2 is spent; popping it again
     * must not take d1's generation with it. */
    tur_region_pop_reclaim(d2);
    check(tur_region_active(), "stale depth does not pop the live generation");
    check(tur_region_owns(a), "stale depth does not reclaim the live generation");
    tur_region_pop(0);
    check(tur_region_active(), "depth 0 is refused");

    /* (2) a pop without reclaim retires rather than frees: the pointer is no
     * longer in a LIVE generation but is still owned, so no free path may
     * touch it.  This is the conservative default and the status quo. */
    tur_region_pop(d1);
    check(!tur_region_active(), "no region active after popping the last one");
    check(tur_region_owns(a), "retired generation still owns its allocation");
    check(tur_region_alloc(8) == NULL, "alloc after last pop returns NULL");

    /* The R2 routing point: the same call allocates by generation inside a
     * region and from the heap outside one, so a spine-node constructor needs
     * one call site rather than a branch.  The consequence a caller must not
     * forget is that the result is NOT necessarily region memory -- which is
     * exactly why every free path has to ask tur_region_owns first. */
    void *heap = tur_region_alloc_or_malloc(48);
    check(heap != NULL, "alloc_or_malloc succeeds with no region");
    check(!tur_region_owns(heap), "with no region open it is heap memory");
    free(heap);

    int d3 = tur_region_push();
    void *inr = tur_region_alloc_or_malloc(48);
    check(inr != NULL, "alloc_or_malloc succeeds inside a region");
    check(tur_region_owns(inr), "inside a region it is region memory");
    tur_region_pop_reclaim(d3);

    /* ---- R3: the escape check ------------------------------------------
     *
     * The decision procedure is "reclaim only if nothing region-owned was
     * noted as escaping".  What matters is that it errs the RIGHT way: a
     * region-owned escape must block the rewind, and a heap escape must not,
     * or the check is either unsafe or useless. */

    /* (a) a region-owned escape BLOCKS the rewind, and the memory survives. */
    int e1 = tur_region_push();
    void *kept = tur_region_alloc(32);
    check(kept != NULL, "escape case: alloc");
    memset(kept, 0x5A, 32);
    tur_region_note_escape(kept);
    check(tur_region_pop_checked(e1) == false, "region-owned escape blocks reclaim");
    check(tur_region_owns(kept), "blocked generation is retired, still owned");
    check(((unsigned char *)kept)[0] == 0x5A, "escaped value survives the refusal");

    /* (b) a HEAP escape does not block: this is what makes the check worth
     * having rather than a blanket refusal. */
    int e2 = tur_region_push();
    void *inregion = tur_region_alloc(32);
    void *offheap  = malloc(32);
    check(inregion && offheap, "mixed case: allocs");
    tur_region_note_escape(offheap);      /* not region memory -- harmless */
    check(tur_region_pop_checked(e2) == true, "heap escape does not block reclaim");
    check(!tur_region_owns(inregion), "reclaimed memory is no longer owned");
    free(offheap);

    /* (c) no escape at all -> reclaim.  And the rewind REUSES the arena, so a
     * region in a loop pays for its slabs once; the second push takes the
     * pooled one rather than asking the allocator again. */
    int e3 = tur_region_push();
    void *first = tur_region_alloc(64);
    check(first != NULL && tur_region_owns(first), "loop case: alloc owned");
    check(tur_region_pop_checked(e3) == true, "no escape -> reclaim");
    int e4 = tur_region_push();
    void *second = tur_region_alloc(64);
    check(second == first, "a reclaimed generation's memory is reused");
    check(tur_region_pop_checked(e4) == true, "second round reclaims too");

    /* (d) a mismatched depth is refused rather than reclaiming the wrong
     * generation -- the same rule the unchecked pops follow. */
    int e5 = tur_region_push();
    check(tur_region_pop_checked(e5 + 1) == false, "checked pop refuses a bad depth");
    check(tur_region_active(), "refused checked pop left the generation open");
    check(tur_region_pop_checked(e5) == true, "correct depth then reclaims");

    tur_region_shutdown();

    if (failures == 0) printf("region_unit: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
