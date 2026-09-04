#ifndef TUR_TRAIL_H
#define TUR_TRAIL_H

/* trail.h -- SX1: backtrackable state as a runtime primitive.
 *
 * A per-thread trail: writes to a trailed cell record their previous value, and
 * undoing to a mark restores every write made since. This is the WAM / CLP
 * shape, and it is what a solver's theory seam hand-rolls in C -- EUF's
 * union-find parents, simplex's bound stack, a domain bitmap.
 *
 * The whole design turns on one question the review put to this runtime:
 * CAN YOU OPT A CELL OUT? A trail that is all-or-nothing forces the caller to
 * thread the kept state around it, and backtracking in a solver is
 * asymmetric -- a CDCL backjump discards the trail above the backjump level but
 * KEEPS the learned clause, and simplex keeps its basis. Five of the eleven
 * rows in the plan's own state table are opt-outs. So the answer here is yes,
 * at three granularities:
 *
 *   per cell, forever   tur_gcell_*      never trailed at all
 *   per write           tur_trail_pause  a write that must survive the undo
 *   per level           tur_trail_commit_to  keep this level's writes
 *
 * Four decisions, none of them obvious, all from plan section 3.2:
 *
 * 1. TWO CELL FLAVORS. A value cell trails its previous payload on every write
 *    (union-find parents, simplex bounds). A write-once cell goes from unbound
 *    to bound once per level, so its entry only has to say "reset to unbound" --
 *    the WAM shape, and exactly what a logic variable is.
 *
 * 2. THE STAMP IS WHAT MAKES IT CHEAP. Every cell carries the level at which it
 *    was last trailed. A write records only when `stamp < current_level`, then
 *    bumps the stamp, so a cell written a thousand times inside one level costs
 *    ONE entry. This subsumes the WAM's address comparison -- a cell allocated
 *    inside the current level starts already stamped, so its first write is not
 *    trailed either -- and fixes the redundant-trailing problem the CLP
 *    literature reports.
 *
 * 3. UNDO MUST BE OWNERSHIP-CORRECT. A payload can be an rc handle, so restoring
 *    an old value means re-acquiring it and dropping the value being discarded.
 *    The entry carries the clone/drop pair for that. These are deliberately the
 *    SAME function types the continuation machine already uses for owning frame
 *    environments (DKEnvClone / DKEnvDrop, cps_prompt.h) rather than parallel
 *    ones -- one convention for "this word owns something", not two.
 *
 * 4. MARKS ARE GENERATIONAL. A mark is a level id plus a generation, so undoing
 *    to a mark that was already undone or committed is a DETECTED ERROR rather
 *    than memory corruption. This matters because multi-shot continuations make
 *    stale marks reachable: a continuation captures control, not state, so
 *    re-entering one resumes a computation whose trail levels are already gone.
 *    The plan's answer is (a) a checked error, shipped here; (b) snapshotting
 *    the live trail segment is a measured decision, not a default.
 *
 * See docs/upcoming/solver-extension-plan.md, sections 3.2-3.5 and SX1. */

#include <stdbool.h>
#include <stdint.h>

/* Ownership hooks for a trailed payload.  NULL/NULL is the `:copy` fast path:
 * the word is just a word, and undo is a store.  Same signatures as
 * DKEnvClone / DKEnvDrop (src/runtime/cps_prompt.h) on purpose. */
typedef intptr_t (*TurTrailClone)(intptr_t payload);
typedef void     (*TurTrailDrop)(intptr_t payload);

/* A mark: the level, plus the generation that level was issued in.  Passed and
 * returned by value -- it is two words, and making it a handle would put an
 * allocation on the hottest operation in a search loop. */
typedef struct TurTrailMark {
    uint32_t level;
    uint32_t generation;
} TurTrailMark;

/* A trailed cell.  `payload` is the word; `stamp` is decision 2. */
typedef struct TurBtCell {
    intptr_t      payload;
    uint32_t      stamp;
    bool          bound;      /* write-once cells only; always true for value cells */
    bool          write_once;
    TurTrailClone clone;
    TurTrailDrop  drop;
} TurBtCell;

/* ------------------------------------------------------------------------- *
 * Levels
 * ------------------------------------------------------------------------- */

/* Push a level and return its mark. */
TurTrailMark tur_trail_mark(void);

/* Pop to `m`, running every undo recorded since it was taken.
 *
 * Returns false and does nothing when `m` is stale -- already undone, already
 * committed, or from a different generation.  A stale undo is the failure mode
 * a multi-shot re-entry produces, and silently "succeeding" at it would restore
 * words that belong to some other level's cells. */
bool tur_trail_undo_to(TurTrailMark m);

/* Drop the level, KEEPING its writes.  This is Prolog's `assert`, ASP's
 * "global", promoting a lemma to level 0.  Entries below the mark are
 * discarded without running their undo, and any owning payload they were
 * holding for restoration is released. */
bool tur_trail_commit_to(TurTrailMark m);

/* The current level.  0 means "no mark taken", where writes are never
 * trailed -- there is nothing to come back to. */
uint32_t tur_trail_level(void);

/* Entries currently on the trail.  For tests and telemetry: the stamp
 * discipline is only observable as a COUNT, so a fixture that means to assert
 * "a thousand writes cost one entry" has to be able to read this. */
uint32_t tur_trail_depth(void);

/* ------------------------------------------------------------------------- *
 * Per-write opt-out
 * ------------------------------------------------------------------------- */

/* Suspend trailing.  Writes between pause and resume are permanent -- a node
 * counter or statistic that must survive the search that produced it.  Nests:
 * the counter is what resumes, not the first resume call. */
void tur_trail_pause(void);
void tur_trail_resume(void);
bool tur_trail_paused(void);

/* ------------------------------------------------------------------------- *
 * Cells
 * ------------------------------------------------------------------------- */

/* A value cell: trails its previous payload on the first write per level. */
void tur_bt_cell_init(TurBtCell *c, intptr_t init,
                      TurTrailClone clone, TurTrailDrop drop);

/* A write-once cell: unbound until bound, and undo returns it to unbound.
 * `init` is not a value, it is the unbound sentinel the cell reads as. */
void tur_bt_lvar_init(TurBtCell *c, intptr_t unbound,
                      TurTrailClone clone, TurTrailDrop drop);

/* A BORROWED read: valid until the cell is next written or undone.  This is
 * what a union-find `find` wants, and what most reads want. */
static inline intptr_t tur_bt_get(const TurBtCell *c) { return c->payload; }
static inline bool     tur_bt_bound(const TurBtCell *c) { return c->bound; }

/* An OWNED read, for a caller that needs the value to outlive the cell's next
 * write.  Only the cell knows how to duplicate its payload, which is what the
 * clone hook is for. */
intptr_t tur_bt_get_owned(const TurBtCell *c);

/* Write, TAKING OWNERSHIP of `v`.  Records to the trail only when this cell has
 * not yet been trailed at the current level (decision 2).  Returns false for a
 * write-once cell that is already bound -- binding twice is a caller bug, not a
 * silent overwrite. */
bool tur_bt_set(TurBtCell *c, intptr_t v);

/* ------------------------------------------------------------------------- *
 * Lifecycle
 * ------------------------------------------------------------------------- */

/* Drop every level and entry without running undo.  Process/thread teardown
 * and the tests; not part of the search protocol. */
void tur_trail_reset(void);

/* ------------------------------------------------------------------------- *
 * Scalar ABI shims
 *
 * The API above is the one to program against from C.  These exist because a
 * Turmeric `extern-c` declaration can express pointers, integers and bools --
 * not a two-word struct returned by value, which is what a mark is.  Rather
 * than flatten TurTrailMark for everyone's benefit, the mark is PACKED into an
 * int64 at this boundary only: level in the high half, generation in the low.
 *
 * They also own allocation, so stdlib/trail.tur never has to know sizeof a
 * cell -- which keeps the struct free to change without a stdlib edit.
 * ------------------------------------------------------------------------- */

int64_t  tur_trail_mark_packed(void);
bool     tur_trail_undo_to_packed(int64_t packed);
bool     tur_trail_commit_to_packed(int64_t packed);

/* Width shims for `tur_trail_level` / `tur_trail_depth`.
 *
 * Those return uint32_t, which is the right C spelling for a level and a depth.
 * A Turmeric `:int` extern-c declares them as int64_t, and calling a uint32_t
 * function through an int64_t prototype is a real ABI mismatch, not a pedantic
 * one: the callee writes a 32-bit result register and the upper half is
 * unspecified, so the caller can read a level of 4294967297.  It has not bitten
 * yet only because the compilers in use happen to zero-extend.
 *
 * So the int64 boundary gets int64 functions, exactly as the mark does above.
 * stdlib/trail.tur and the emitted serial guard both go through these; nothing
 * declares the uint32_t pair from Turmeric or from emitted C. */
int64_t  tur_trail_level_i64(void);
int64_t  tur_trail_depth_i64(void);

/* ------------------------------------------------------------------------- *
 * A trailed substitution (SX2)
 *
 * The first real consumer of the trail, and the thing SX2 exists to measure:
 * an INDEXED variable->term map, against stdlib/logic.tur's persistent
 * association list.  The two differ where it counts -- the assoc list is O(1)
 * to extend and free to backtrack but O(n) to look up, while this is O(1) to
 * look up and O(1) to bind but pays O(writes) to undo.
 *
 * Terms are encoded in one word so the structure stays the subject of the
 * measurement rather than an allocator: `2k` is the ground value k, `2k+1` is
 * variable k.  `walk` chases the odd ones, which is exactly what
 * `logic-walk` does with TVar.
 * ------------------------------------------------------------------------- */

#define TUR_UF_UNBOUND (-1)

void    *tur_uf_new(int64_t n_vars);
void     tur_uf_free(void *uf);
/* Bind variable `var` to encoded term `val`.  Trailed, so it undoes with the
 * enclosing level.  False if already bound at this level (write-once). */
bool     tur_uf_bind(void *uf, int64_t var, int64_t val);
/* Follow `var`'s bindings to a ground term or an unbound variable, encoded. */
int64_t  tur_uf_walk(void *uf, int64_t var);

void    *tur_bt_cell_new(int64_t init);      /* value cell   */
void    *tur_bt_lvar_new(int64_t unbound);   /* write-once   */
void     tur_bt_cell_free(void *c);
int64_t  tur_bt_cell_get(void *c);
bool     tur_bt_cell_bound(void *c);
bool     tur_bt_cell_set(void *c, int64_t v);

#endif /* TUR_TRAIL_H */
