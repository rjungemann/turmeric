#ifndef TUR_REFINE_COLLECT_H
#define TUR_REFINE_COLLECT_H

/* refine_collect.h -- RT1: the constraint collector.
 *
 * Each point where a value crosses INTO a `#refine{ x : T | p }` type is a
 * proof obligation: prove `p[subject/x]` under the hypotheses in scope.  This
 * module owns the obligation record, the hypothesis environment, and the
 * Form -> normalized-VC encoder that RT2 consumes.
 *
 * Crossing points collected today:
 *   - a `defn` whose RETURN type is a contract type (subject = the body form)
 *   - a `defn` `:post` predicate            (subject = the body form, var `result`)
 *
 * Hypotheses come from:
 *   - each parameter declared with a contract type (`v` renamed to the param)
 *   - the function's `:pre` predicate
 *
 * Call-site argument crossings (passing `expr` where the parameter type is a
 * contract type) need the callee's per-parameter predicates on the FnDef; that
 * plumbing is the next slice and is tracked in the plan.
 *
 * See docs/upcoming/v1/refinement-types-plan.md (phase RT1). */

#include <stdbool.h>
#include <stdint.h>

#include "forms.h"
#include "refine_vc.h"
#include "runtime/arena.h"

/* ------------------------------------------------------------------------- *
 * Hypothesis environment
 * ------------------------------------------------------------------------- */

/* One known-true fact.  `pred` is the predicate as written; `bound_var` is the
 * `v` of `#refine{ v : T | p }` and `subject_name` is the in-scope name it
 * stands for, so the encoder renames without rewriting the Form tree.  A
 * `:pre` predicate has `bound_var == NULL` (nothing to rename). */
typedef struct RefineHyp {
    const Form       *pred;
    const char       *bound_var;
    const char       *subject_name;
    struct RefineHyp *next;
} RefineHyp;

typedef struct RefineEnv {
    Arena       *arena;
    RefineHyp   *head;
    /* Declared sorts for in-scope names (from parameter types).  A name with
     * no entry defaults to VS_INT. */
    const char **names;
    VCSort      *sorts;
    uint32_t     n_names, cap_names;
} RefineEnv;

RefineEnv *refine_env_new(Arena *a);
void refine_env_declare(RefineEnv *env, const char *name, VCSort sort);
void refine_env_push(RefineEnv *env, const Form *pred,
                     const char *bound_var, const char *subject_name);
VCSort refine_env_sort_of(const RefineEnv *env, const char *name);

/* ------------------------------------------------------------------------- *
 * Obligation record
 * ------------------------------------------------------------------------- */

typedef struct RefineObligation {
    const Form  *predicate;      /* the p in #refine{ x : T | p } */
    const char  *var_name;       /* the x */
    const Form  *subject;        /* the form substituted for x (may be NULL) */
    VCSort       base_sort;      /* sort of the refined base type T */
    const char  *base_type_name; /* T, for diagnostics */
    Span         loc;
    RefineEnv   *env;            /* in-scope hypotheses at the crossing */
    const char  *what;           /* "return value", "postcondition", ... */
    const char  *fn_name;

    RefineVC    *vc;             /* RT2: filled by the discharge pass */
    bool         discharged;     /* RT3: a verdict was reached */
    bool         proven;         /* RT3: a backend returned RT_VALID */
    RefineModel *counterex;      /* RT3: model when a backend said RT_INVALID */
} RefineObligation;

typedef struct RefineObligationVec {
    RefineObligation **obs;
    uint32_t           n, cap;
    Arena             *arena;
} RefineObligationVec;

void refine_obligations_init(RefineObligationVec *v, Arena *a);

/* Record one crossing.  Returns the obligation so a caller that wants an
 * immediate verdict (to elide the runtime check it is about to inject) can
 * hand it straight to refine_discharge_one. */
RefineObligation *refine_collect_obligation(RefineObligationVec *v,
                                            const Form *predicate,
                                            const char *var_name,
                                            const Form *subject,
                                            VCSort base_sort,
                                            const char *base_type_name,
                                            Span loc,
                                            RefineEnv *env,
                                            const char *what,
                                            const char *fn_name);

/* ------------------------------------------------------------------------- *
 * RT2 lowering: obligation -> normalized VC
 * ------------------------------------------------------------------------- */

/* Build the normalized VC for `ob` (hypotheses from its env, goal from its
 * predicate with `var_name` substituted by `subject`).  Returns NULL when the
 * obligation escapes the supported predicate fragment -- the caller then
 * treats it as RT_UNKNOWN, which is always sound.  `*out_reason` (optional)
 * is set to a short human-readable cause. */
RefineVC *refine_vc_build(RefineObligation *ob, Arena *a, const char **out_reason);

#endif /* TUR_REFINE_COLLECT_H */
