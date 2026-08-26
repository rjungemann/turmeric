/* trail.c -- SX1 unit tests for the backtrackable-state primitive.
 *
 * The acceptance list from the plan, at the C level: value cells, write-once
 * cells, stamp suppression of redundant trailing, the never-trailed opt-out,
 * per-write pause, commit-instead-of-undo, ownership-correct undo of a
 * refcounted payload, and a stale mark that is a detected error rather than
 * memory corruption.
 *
 * The refcount tests matter most.  A trail that leaks one payload per undone
 * write, or frees one it is about to install, is a bug that ASan finds only
 * once something real is on the other end -- so the fake refcount here is
 * checked to zero explicitly rather than left for the sanitizer. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/trail.h"

static int g_checks = 0, g_fails = 0;

static void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) { g_fails++; printf("  FAIL %s\n", what); }
}

static void check_u32(uint32_t got, uint32_t want, const char *what) {
    g_checks++;
    if (got != want) {
        g_fails++;
        printf("  FAIL %s: got %u, want %u\n", what, got, want);
    }
}

/* ------------------------------------------------------------------------- *
 * A fake refcounted payload, so ownership is observable rather than inferred.
 * ------------------------------------------------------------------------- */

typedef struct Obj { int rc; int id; } Obj;

static int g_live;   /* objects whose rc has not reached zero */

static Obj *obj_new(int id) {
    Obj *o = (Obj *)calloc(1, sizeof(Obj));
    o->rc = 1; o->id = id; g_live++;
    return o;
}
static intptr_t obj_clone(intptr_t p) { ((Obj *)p)->rc++; return p; }
static void     obj_drop(intptr_t p) {
    Obj *o = (Obj *)p;
    if (--o->rc == 0) { g_live--; free(o); }
}

/* ------------------------------------------------------------------------- *
 * Value cells
 * ------------------------------------------------------------------------- */

static void test_value_cell(void) {
    tur_trail_reset();
    TurBtCell c;
    tur_bt_cell_init(&c, 10, NULL, NULL);

    TurTrailMark m = tur_trail_mark();
    tur_bt_set(&c, 20);
    check(tur_bt_get(&c) == 20, "value cell reads the written value");
    check(tur_trail_undo_to(m), "undo to a live mark succeeds");
    check(tur_bt_get(&c) == 10, "undo restores the previous value");

    /* Nesting: each level restores only its own writes. */
    TurTrailMark m1 = tur_trail_mark();
    tur_bt_set(&c, 20);
    TurTrailMark m2 = tur_trail_mark();
    tur_bt_set(&c, 30);
    tur_trail_undo_to(m2);
    check(tur_bt_get(&c) == 20, "inner undo restores to the outer level's value");
    tur_trail_undo_to(m1);
    check(tur_bt_get(&c) == 10, "outer undo restores to the original");
    tur_trail_reset();
}

/* Decision 2: a cell written a thousand times inside one level costs ONE entry.
 * The stamp is the whole reason the primitive is affordable, and depth is the
 * only place it is observable. */
static void test_stamp_suppression(void) {
    tur_trail_reset();
    TurBtCell c;
    tur_bt_cell_init(&c, 0, NULL, NULL);

    TurTrailMark m = tur_trail_mark();
    for (int i = 1; i <= 1000; i++) tur_bt_set(&c, i);
    check_u32(tur_trail_depth(), 1, "1000 writes in one level cost one entry");
    check(tur_bt_get(&c) == 1000, "the last write is what the cell holds");
    tur_trail_undo_to(m);
    check(tur_bt_get(&c) == 0, "undo restores the value from before the level");

    /* A cell born INSIDE the level is stamped at birth, so its first write is
     * not trailed either -- the WAM's address comparison, as a stamp. */
    TurTrailMark m2 = tur_trail_mark();
    TurBtCell inner;
    tur_bt_cell_init(&inner, 1, NULL, NULL);
    tur_bt_set(&inner, 2);
    check_u32(tur_trail_depth(), 0, "a cell born inside the level is not trailed");
    tur_trail_undo_to(m2);
    tur_trail_reset();
}

/* ------------------------------------------------------------------------- *
 * Write-once cells (the WAM/logic-variable shape)
 * ------------------------------------------------------------------------- */

static void test_write_once(void) {
    tur_trail_reset();
    TurBtCell v;
    tur_bt_lvar_init(&v, 0, NULL, NULL);
    check(!tur_bt_bound(&v), "a fresh lvar is unbound");

    TurTrailMark m = tur_trail_mark();
    check(tur_bt_set(&v, 42), "binding an unbound lvar succeeds");
    check(tur_bt_bound(&v), "it is bound afterwards");
    check(!tur_bt_set(&v, 43), "binding an already-bound lvar is refused");
    check(tur_bt_get(&v) == 42, "the refused write did not overwrite");

    tur_trail_undo_to(m);
    check(!tur_bt_bound(&v), "undo returns the lvar to unbound");
    check(tur_bt_set(&v, 7), "it can be bound again after undo");
    tur_trail_reset();
}

/* ------------------------------------------------------------------------- *
 * The three opt-outs (plan 3.3)
 * ------------------------------------------------------------------------- */

static void test_pause(void) {
    tur_trail_reset();
    TurBtCell counter;
    tur_bt_cell_init(&counter, 0, NULL, NULL);

    TurTrailMark m = tur_trail_mark();
    tur_trail_pause();
    tur_bt_set(&counter, 99);      /* a statistic that must survive the search */
    tur_trail_resume();
    check_u32(tur_trail_depth(), 0, "a paused write is not trailed");
    tur_trail_undo_to(m);
    check(tur_bt_get(&counter) == 99, "a paused write survives the undo");

    /* Nesting: the counter is what resumes, not the first resume call. */
    tur_trail_pause(); tur_trail_pause();
    tur_trail_resume();
    check(tur_trail_paused(), "pause nests");
    tur_trail_resume();
    check(!tur_trail_paused(), "and unwinds fully");
    tur_trail_reset();
}

static void test_commit(void) {
    tur_trail_reset();
    TurBtCell c;
    tur_bt_cell_init(&c, 1, NULL, NULL);

    TurTrailMark outer = tur_trail_mark();
    tur_bt_set(&c, 2);
    TurTrailMark inner = tur_trail_mark();
    tur_bt_set(&c, 3);
    check(tur_trail_commit_to(inner), "commit to a live mark succeeds");
    check(tur_bt_get(&c) == 3, "committed writes are kept");
    check_u32(tur_trail_level(), outer.level, "commit pops the level");

    /* The outer level must still be able to undo its OWN write.  This is the
     * case the `<` in the stamp comparison exists for: the commit left the cell
     * stamped above the current level, and a `!=` test would re-trail it and
     * then revert a write the caller asked to keep. */
    tur_bt_set(&c, 4);
    tur_trail_undo_to(outer);
    check(tur_bt_get(&c) == 1, "the outer undo still restores the original");
    tur_trail_reset();
}

/* The bug the first draft shipped: commit left the cell stamped at a level that
 * had been popped.  A later mark REUSES that index, the stamp already equals
 * the new level, so the write is not trailed -- and its undo silently does
 * nothing, which is the worst shape a backtracking bug can have (no crash, no
 * error, just a value that quietly failed to come back).
 *
 * The original test_commit did NOT catch this: it undid to the OUTER mark,
 * whose entry restored the right value regardless, so the missing inner entry
 * never showed. Verified by mutation -- reverting the fix must fail this. */
static void test_commit_then_reused_level(void) {
    tur_trail_reset();
    TurBtCell c;
    tur_bt_cell_init(&c, 1, NULL, NULL);

    TurTrailMark m1 = tur_trail_mark();
    tur_bt_set(&c, 2);
    TurTrailMark m2 = tur_trail_mark();
    tur_bt_set(&c, 3);
    tur_trail_commit_to(m2);                 /* keep 3, pop to level 1 */

    TurTrailMark m3 = tur_trail_mark();      /* reuses the popped index */
    check_u32(m3.level, m2.level, "the new mark really does reuse the index");
    tur_bt_set(&c, 4);
    check(tur_trail_undo_to(m3), "undo to the reused level succeeds");
    check(tur_bt_get(&c) == 3,
          "a write at a level that reuses a committed index is still trailed");

    tur_trail_undo_to(m1);
    check(tur_bt_get(&c) == 1, "and the outer level still restores the original");
    tur_trail_reset();
}

static void test_gcell_never_trailed(void) {
    /* The per-cell opt-out is not a flag on this API -- it is the absence of a
     * trailed cell.  A plain word written directly is the C-level GCell, and
     * the property under test is that nothing about it reaches the trail. */
    tur_trail_reset();
    intptr_t global = 5;
    TurBtCell trailed;
    tur_bt_cell_init(&trailed, 1, NULL, NULL);

    TurTrailMark m = tur_trail_mark();
    global = 6;                     /* the opt-out: never routed through a cell */
    tur_bt_set(&trailed, 2);
    check_u32(tur_trail_depth(), 1, "only the trailed cell is on the trail");
    tur_trail_undo_to(m);
    check(global == 6, "the untrailed word survives the undo");
    check(tur_bt_get(&trailed) == 1, "the trailed cell was restored");
    tur_trail_reset();
}

/* ------------------------------------------------------------------------- *
 * Ownership (decision 3)
 * ------------------------------------------------------------------------- */

static void test_owning_undo(void) {
    tur_trail_reset();
    g_live = 0;

    Obj *a = obj_new(1);
    TurBtCell c;
    tur_bt_cell_init(&c, (intptr_t)a, obj_clone, obj_drop);
    check_u32((uint32_t)g_live, 1, "one object live after init");

    TurTrailMark m = tur_trail_mark();
    Obj *b = obj_new(2);
    tur_bt_set(&c, (intptr_t)b);
    check_u32((uint32_t)g_live, 2, "both live while the old one is on the trail");
    check(((Obj *)tur_bt_get(&c))->id == 2, "the cell holds the new object");

    tur_trail_undo_to(m);
    check(((Obj *)tur_bt_get(&c))->id == 1, "undo restores the old object");
    check_u32((uint32_t)g_live, 1, "the discarded object was released exactly once");
    check_u32((uint32_t)((Obj *)tur_bt_get(&c))->rc, 1,
              "the restored object holds exactly one reference");

    obj_drop(tur_bt_get(&c));
    check_u32((uint32_t)g_live, 0, "no leak");
}

static void test_owning_repeated_writes(void) {
    /* The stamp means only the FIRST write per level is trailed, so every
     * subsequent one has to release its own predecessor -- if it did not, a
     * loop that writes a cell a thousand times would leak 999 objects. */
    tur_trail_reset();
    g_live = 0;

    Obj *base = obj_new(0);
    TurBtCell c;
    tur_bt_cell_init(&c, (intptr_t)base, obj_clone, obj_drop);

    TurTrailMark m = tur_trail_mark();
    for (int i = 1; i <= 100; i++) tur_bt_set(&c, (intptr_t)obj_new(i));
    check_u32(tur_trail_depth(), 1, "still one entry after 100 owning writes");
    check_u32((uint32_t)g_live, 2, "only the trailed original and the current live");

    tur_trail_undo_to(m);
    check(((Obj *)tur_bt_get(&c))->id == 0, "undo restores the original");
    check_u32((uint32_t)g_live, 1, "everything written inside the level is gone");

    obj_drop(tur_bt_get(&c));
    check_u32((uint32_t)g_live, 0, "no leak");
}

static void test_owning_commit(void) {
    /* On commit the WRITE is kept, so it is the SAVED value nobody will hold
     * again.  Getting this backwards frees the live object and leaks the dead
     * one -- and both look fine until something reads the cell. */
    tur_trail_reset();
    g_live = 0;

    Obj *a = obj_new(1);
    TurBtCell c;
    tur_bt_cell_init(&c, (intptr_t)a, obj_clone, obj_drop);

    TurTrailMark m = tur_trail_mark();
    tur_bt_set(&c, (intptr_t)obj_new(2));
    check(tur_trail_commit_to(m), "commit succeeds");
    check(((Obj *)tur_bt_get(&c))->id == 2, "the committed value is live and readable");
    check_u32((uint32_t)g_live, 1, "the superseded value was released");
    check_u32((uint32_t)((Obj *)tur_bt_get(&c))->rc, 1, "and the kept one is not over-held");

    obj_drop(tur_bt_get(&c));
    check_u32((uint32_t)g_live, 0, "no leak");
}

static void test_get_owned(void) {
    tur_trail_reset();
    g_live = 0;
    Obj *a = obj_new(1);
    TurBtCell c;
    tur_bt_cell_init(&c, (intptr_t)a, obj_clone, obj_drop);

    intptr_t kept = tur_bt_get_owned(&c);      /* outlives the next write */
    TurTrailMark m = tur_trail_mark();
    tur_bt_set(&c, (intptr_t)obj_new(2));
    tur_trail_commit_to(m);                    /* the original is dropped here */
    check(((Obj *)kept)->id == 1, "an owned read survives the value leaving the cell");
    check_u32((uint32_t)g_live, 2, "both are still live: one kept, one in the cell");

    obj_drop(kept);
    obj_drop(tur_bt_get(&c));
    check_u32((uint32_t)g_live, 0, "no leak");
}

/* ------------------------------------------------------------------------- *
 * Decision 4: a stale mark is DETECTED
 * ------------------------------------------------------------------------- */

static void test_stale_marks(void) {
    tur_trail_reset();
    TurBtCell c;
    tur_bt_cell_init(&c, 1, NULL, NULL);

    TurTrailMark m = tur_trail_mark();
    tur_bt_set(&c, 2);
    check(tur_trail_undo_to(m), "the first undo succeeds");
    check(!tur_trail_undo_to(m), "undoing the same mark twice is refused");
    check(tur_bt_get(&c) == 1, "and the refused undo changed nothing");

    /* The case that motivates the generation counter: a re-entered continuation
     * holds a mark whose LEVEL INDEX has since been reused by a later mark.
     * Comparing indices alone would accept this and unwind someone else's
     * level. */
    TurTrailMark reused = tur_trail_mark();
    check_u32(reused.level, m.level, "the level index really is reused");
    check(reused.generation != m.generation, "but the generation differs");
    check(!tur_trail_undo_to(m), "so the stale mark is still refused");
    check(tur_trail_undo_to(reused), "while the live one works");

    TurTrailMark committed = tur_trail_mark();
    tur_bt_set(&c, 3);
    tur_trail_commit_to(committed);
    check(!tur_trail_undo_to(committed), "a committed mark cannot then be undone");
    check(tur_bt_get(&c) == 3, "and the committed write is untouched");
    tur_trail_reset();
}

static void test_no_level(void) {
    /* Writing with no mark taken is not an error -- there is simply nothing to
     * come back to, so nothing is recorded. */
    tur_trail_reset();
    TurBtCell c;
    tur_bt_cell_init(&c, 1, NULL, NULL);
    tur_bt_set(&c, 2);
    check_u32(tur_trail_depth(), 0, "a write with no level is not trailed");
    check(tur_bt_get(&c) == 2, "but it still takes effect");
    tur_trail_reset();
}

int main(void) {
    test_value_cell();
    test_stamp_suppression();
    test_write_once();
    test_pause();
    test_commit();
    test_commit_then_reused_level();
    test_gcell_never_trailed();
    test_owning_undo();
    test_owning_repeated_writes();
    test_owning_commit();
    test_get_owned();
    test_stale_marks();
    test_no_level();

    printf("trail: %d checks, %d failure(s)\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
