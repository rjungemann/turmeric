#ifndef TUR_REFINE_SMTLIB_H
#define TUR_REFINE_SMTLIB_H

/* refine_smtlib.h -- RT2: normalized VC -> SMT-LIB2 text.
 *
 * This serializer was written for the dev-only Z3 scaffold (retired in 0.32.5)
 * and for debugging (`TUR_REFINE_DUMP=1`); the latter is now its only caller.
 * The in-house stages S0..S3 consume the RefineVC directly and never touch
 * SMT-LIB text, so nothing in any build depends on this file's output.
 *
 * It is kept because the corpus path reads SMT-LIB in the other direction
 * (tests/unit/refine_corpus.c) and a dump that round-trips against it is how a
 * VC gets debugged against an external solver by hand. */

#include "refine_vc.h"
#include "runtime/buf.h"

/* Serialize `vc` as a validity query: the hypotheses are asserted and the goal
 * is asserted NEGATED, so `unsat` means valid.  Appends to `out`. */
void refine_smtlib_emit(const RefineVC *vc, Buf *out);

#endif /* TUR_REFINE_SMTLIB_H */
