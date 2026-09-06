#ifndef TUR_REFINE_REPORT_H
#define TUR_REFINE_REPORT_H

/* refine_report.h -- SX8a: the JSON obligation dump.
 *
 * One record per refinement obligation: where it came from, what it had to
 * prove, the VC as SMT-LIB2 text, what the solver answered, which stage
 * answered it, the counterexample when there was one, and which caps (if any)
 * bit while deciding it.
 *
 * This is the second consumer of SX0(b)'s cap telemetry, not a second copy of
 * it: the counters live in refine_solver.h, the per-compile summary reads them
 * for `TUR_REFINE_STATS=1`, and each obligation carries its own delta so a
 * record can say "this one hit the cube cap" rather than only "something in
 * this unit did".
 *
 * The schema is STABLE since SX9 and says so in every record (`"schema": 1`;
 * 0 was the same shape while it was flagged unstable).
 * A query surface that hardens into a compatibility contract before the solver
 * settles is a listed risk in the plan; the field stabilizes at SX9, not
 * before.  Consumers should branch on it rather than assume.
 *
 * See docs/archive/solver-extension-plan.md (SX8a). */

#include "refine_collect.h"
#include "runtime/buf.h"

/* Append the whole dump -- a JSON object with a "obligations" array -- to
 * `out`.  Safe on an empty or NULL vector (emits a well-formed empty report),
 * because a unit with no refinements should produce parseable output rather
 * than nothing at all. */
void refine_report_json(const RefineObligationVec *v, Buf *out);

#endif /* TUR_REFINE_REPORT_H */
