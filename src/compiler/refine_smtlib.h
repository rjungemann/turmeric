#ifndef TUR_REFINE_SMTLIB_H
#define TUR_REFINE_SMTLIB_H

/* refine_smtlib.h -- the SMT-LIB2 seam, in both directions.
 *
 * WRITER (RT2): normalized VC -> SMT-LIB2 text.  Written for the dev-only Z3
 * scaffold (retired in 0.32.5) and for debugging (`TUR_REFINE_DUMP=1`).  The
 * in-house stages S0..S3 consume the RefineVC directly and never touch
 * SMT-LIB text, so nothing in any build depends on this file's output.
 *
 * READER (SX8a): SMT-LIB2 text -> RefineVC.  Lifted out of
 * tests/unit/refine_corpus.c, where it was reachable only by the ctest
 * harness.  Together the two make `tur` answerable from outside the compile
 * pipeline -- `tur smt` runs a script through the standard chain -- and let an
 * external harness differentially test any solver against `tur` in both
 * directions, without `tur` ever linking one.
 *
 * Scope, deliberately narrow and stated so the surface never overpromises: the
 * corpus subset of SMT-LIB2 over QF_UFLIA / QF_UFLRA, `unknown` is a
 * first-class answer, and parity with a production SMT solver is a non-goal.
 * See docs/upcoming/solver-extension-plan.md (SX8a). */

#include "refine_vc.h"
#include "runtime/buf.h"

/* Serialize `vc` as a validity query: the hypotheses are asserted and the goal
 * is asserted NEGATED, so `unsat` means valid.  Appends to `out`. */
void refine_smtlib_emit(const RefineVC *vc, Buf *out);

/* ------------------------------------------------------------------------- *
 * Reader
 * ------------------------------------------------------------------------- */

/* The script's `(set-info :status ...)` claim, if it made one.  It is a claim
 * about SATISFIABILITY of the assertion set, not about the chain's answer --
 * the reader records it and decides nothing. */
typedef enum {
    SMT_STATUS_SAT,
    SMT_STATUS_UNSAT,
    SMT_STATUS_UNKNOWN,
    SMT_STATUS_NONE,     /* no :status line */
} SmtlibStatus;

typedef struct SmtlibQuery {
    SmtlibStatus status;
    /* True when the script used something outside the accepted fragment.  A
     * skipped query is skipped WHOLE: `vc` must not be decided, because a
     * partially parsed assertion set has weaker hypotheses than the script
     * wrote, and answering `unsat` from it would be a claim about work not
     * done. */
    bool         skipped;
    const char  *skip_reason;
    RefineVC    *vc;
} SmtlibQuery;

/* Parse an SMT-LIB2 script into a validity query: every `(assert phi)` becomes
 * a hypothesis and the goal is `false`, so `hyps |- false` is VALID exactly
 * when the assertion set is UNSAT.  Everything is arena-allocated from `a`.
 *
 * Always fills `*out`; check `skipped` before using `vc`.
 *
 * This is the BATCH door: the whole script is one assertion set and the caller
 * decides once.  `push`/`pop` are outside its fragment by construction (an
 * assertion stack has no meaning when there is only one check).  For a script
 * that uses them, or that checks more than once, use the session below. */
void refine_smtlib_read(SmtlibQuery *out, const char *text, size_t len, Arena *a);

/* ------------------------------------------------------------------------- *
 * Sessions (SX8b) -- the incremental door
 * ------------------------------------------------------------------------- */

/* An SMT-LIB2 session: the same reader and the same fragment, driven one
 * command at a time with an assertion STACK, so `(push)` / `(assert)` /
 * `(check-sat)` / `(pop)` mean what the protocol says they mean.  The caller
 * runs the chain when a step reports `SMT_EV_CHECK_SAT`, which is what keeps
 * this file free of any dependency on the solver.
 *
 * What is and is not incremental, stated plainly because the phase that asked
 * for this expected more: the ASSERTION SET is incremental -- a `pop` restores
 * exactly the hypotheses in scope at the matching `push`, and nothing is
 * re-read or re-translated.  The SOLVER STATE is not: each `check-sat` runs
 * the standard chain from the current assertion set, which rebuilds the DNF
 * cubes.  It cannot do otherwise -- adding one hypothesis changes the cube set
 * wholesale, so there is no mark to undo between two checks.  See
 * docs/upcoming/solver-extension-plan.md (SX8b). */
typedef struct SmtlibSession SmtlibSession;

typedef enum {
    SMT_EV_END,         /* the fed text is exhausted */
    SMT_EV_OK,          /* command consumed; nothing for the caller to do */
    SMT_EV_CHECK_SAT,   /* decide the current assertion set, then step again */
    SMT_EV_GET_MODEL,   /* print the model from the last check, if any */
    SMT_EV_EXIT,        /* `(exit)` */
    SMT_EV_ERROR,       /* outside the fragment -- see refine_smtlib_session_err */
} SmtlibEvent;

/* A session's VC has its goal fixed to `false` from the start, so
 * `hyps |- false` is decidable at any point in the script. */
SmtlibSession *refine_smtlib_session_new(Arena *a);

/* Point the session at more script text.  Atoms and symbol names are copied
 * into the arena as they are read, so `text` need not outlive this call's
 * consumption of it -- an interactive driver may reuse one line buffer. */
void refine_smtlib_session_feed(SmtlibSession *s, const char *text, size_t len);

/* Read and execute the next top-level command from the fed text. */
SmtlibEvent refine_smtlib_session_step(SmtlibSession *s);

RefineVC    *refine_smtlib_session_vc(SmtlibSession *s);
const char  *refine_smtlib_session_err(const SmtlibSession *s);
SmtlibStatus refine_smtlib_session_status(const SmtlibSession *s);
/* Current `(push)` depth -- 0 at the outermost level. */
uint32_t     refine_smtlib_session_depth(const SmtlibSession *s);

#endif /* TUR_REFINE_SMTLIB_H */
