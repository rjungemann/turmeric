#ifndef TUR_REFINE_DISCHARGE_H
#define TUR_REFINE_DISCHARGE_H

/* refine_discharge.h -- RT3: the discharge pass.
 *
 * Owns the ordered backend array and the fall-through loop:
 *
 *   S0 (normalize/trivial) -> S1 (EUF) -> S2 (arithmetic) -> S3 (N-O combine)
 *      -> [dev-only Z3 scaffold] -> RT_UNKNOWN => keep the runtime check
 *
 * In every default and release build the chain is the in-house stages only.
 * An obligation no stage can decide falls straight to the runtime contract
 * check it would have had anyway (or, under --strict-refine, to a hard error).
 *
 * See docs/upcoming/v1/refinement-types-plan.md (phase RT3). */

#include "refine_collect.h"

/* Decide one obligation now, memoized on the record.  Returns true when a
 * backend proved it -- the caller may then elide the runtime check it was
 * about to inject.  Emits TUR-E0371 / TUR-W0372 / TUR-W0373 as appropriate. */
bool refine_discharge_one(RefineObligation *ob, Arena *a);

/* Decide every obligation not yet decided, and print the stats summary when
 * TUR_REFINE_STATS=1. */
void refine_discharge_all(RefineObligationVec *v, Arena *a);

/* Per-compile counters (reset by refine_discharge_reset). */
typedef struct RefineStats {
    uint32_t collected;
    uint32_t proven;
    uint32_t invalid;
    uint32_t unknown;
    uint32_t backend_calls;
    uint32_t templates_tried;   /* RT4 speculative probes (not obligations) */
    uint32_t inferred;          /* RT4 refinements successfully inferred */
} RefineStats;

const RefineStats *refine_stats(void);
void refine_discharge_reset(void);

/* RT6: search for an additional hypothesis that would discharge `vc`.  This is
 * a SECOND query through the solver seam, not a heuristic -- a candidate is
 * only offered once the chain confirms it makes the goal valid AND leaves the
 * hypotheses satisfiable.
 *
 * `out` receives the fact in the variable's own name (`(> x 0)`), `out_decl`
 * the same fact in a refinement's bound variable (`(> v 0)`), which is what a
 * declaration would be written with.  Returns false when nothing helps.
 *
 * Exposed (rather than kept static) so the property that matters -- that a
 * CONTRADICTORY candidate is never suggested -- can be asserted directly by
 * the unit test.  Substring-matching a fixture's stderr can confirm a hint
 * appears; it cannot confirm a wrong one does not. */
bool refine_hint_search(RefineVC *vc, Arena *a, char *out, size_t cap,
                        char *out_decl, size_t decl_cap,
                        const char **out_var, bool *out_real);

#endif /* TUR_REFINE_DISCHARGE_H */
