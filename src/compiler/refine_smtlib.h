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
 * Always fills `*out`; check `skipped` before using `vc`. */
void refine_smtlib_read(SmtlibQuery *out, const char *text, size_t len, Arena *a);

#endif /* TUR_REFINE_SMTLIB_H */
