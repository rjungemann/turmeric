#ifndef TUR_REFINE_SMTLIB_H
#define TUR_REFINE_SMTLIB_H

/* refine_smtlib.h -- RT2: normalized VC -> SMT-LIB2 text.
 *
 * This serializer exists for the DEV-ONLY Z3 scaffold (refine_libz3.c) and for
 * debugging (`TUR_REFINE_DUMP=1`).  The in-house stages S0..S3 consume the
 * RefineVC directly and never touch SMT-LIB text, so nothing in a default or
 * release build depends on this file's output. */

#include "refine_vc.h"
#include "runtime/buf.h"

/* Serialize `vc` as a validity query: the hypotheses are asserted and the goal
 * is asserted NEGATED, so `unsat` means valid.  Appends to `out`. */
void refine_smtlib_emit(const RefineVC *vc, Buf *out);

#endif /* TUR_REFINE_SMTLIB_H */
