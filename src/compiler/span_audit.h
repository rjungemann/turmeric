/* span_audit.h -- breakpoint-span coverage audit (debugger Phase 1).
 *
 * Walks an elaborated program tree and reports breakpoint-eligible AST nodes
 * (top-level forms, defns, let forms, and call sites) that carry no usable
 * source span -- i.e. a SPAN_UNKNOWN sentinel or a span with line == 0.
 *
 * This is the Phase 1 deliverable of docs/archive/history/debugger-plan.md: a real
 * debugger needs every breakpoint target and stack frame to resolve back to a
 * {file, line} location, so the spans that already power diagnostics must also
 * survive elaboration intact on these node kinds.  The audit makes the holes
 * visible (and testable) instead of silently worked around.
 */

#ifndef TUR_SPAN_AUDIT_H
#define TUR_SPAN_AUDIT_H

#include <stdio.h>

#include "expr.h"

/* Summary of one audit run. */
typedef struct SpanAuditResult {
    int n_checked;   /* breakpoint-eligible nodes inspected               */
    int n_holes;     /* of those, how many lacked a usable source span    */
} SpanAuditResult;

/* Walk `prog` (the post-elaboration program Expr) and print one line per hole
 * to `out`, followed by a machine-readable summary line:
 *
 *   span-audit: <n_holes> hole(s) in <n_checked> breakpoint-eligible node(s)
 *
 * Returns the number of holes found (0 == clean).  `prog` may be NULL (treated
 * as zero nodes, zero holes). */
int span_audit_program(const Expr *prog, FILE *out);

#endif /* TUR_SPAN_AUDIT_H */
