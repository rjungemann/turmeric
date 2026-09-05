#ifndef TUR_REFINE_VC_H
#define TUR_REFINE_VC_H

/* refine_vc.h -- RT2: the normalized verification condition (VC) and the
 * solver seam.
 *
 * An obligation collected by RT1 (refine_collect.c) is lowered here to a
 * backend-independent structure: a set of sorted variables, a set of
 * uninterpreted function symbols (named measures + nonlinear terms), a list
 * of hypotheses, and a single goal.  Every backend -- the in-house S0..S3
 * stages, and the dev-only Z3 scaffold that used to sit behind them before its
 * retirement in 0.32.5 -- consumes exactly this and returns one of three
 * verdicts.  The seam is kept backend-independent regardless: it is what makes
 * a new stage droppable into the chain without touching the encoder.
 *
 * THE SOUNDNESS INVARIANT IS ONE-DIRECTIONAL AND ABSOLUTE: a backend may
 * never answer RT_VALID for an obligation that is not genuinely entailed.
 * RT_UNKNOWN is always a safe answer -- it falls back to the runtime contract
 * check the predicate would have had anyway.  That is what makes an
 * incrementally hand-rolled solver shippable.
 *
 * See docs/archive/refinement-types-plan.md (phase RT2). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime/arena.h"
#include "forms.h"

/* ------------------------------------------------------------------------- *
 * Sorts and term operators
 * ------------------------------------------------------------------------- */

typedef enum VCSort {
    VS_INT,   /* integer-sorted term  */
    VS_REAL,  /* real-sorted term     */
    VS_BOOL,  /* proposition          */
} VCSort;

/* Term/atom operators.  The builder normalizes as it interns:
 *   (> a b)  => (< b a)      (>= a b) => (<= b a)
 *   (!= a b) => (not (= a b))
 * so no VC_GT/VC_GE ever exists and every backend has three relations to
 * handle instead of six. */
typedef enum VCOp {
    /* leaves */
    VC_CONST_INT,   /* as.i                                        */
    VC_CONST_REAL,  /* as.r                                        */
    VC_VAR,         /* as.idx -> RefineVC.vars                     */
    VC_APP,         /* as.idx -> RefineVC.ufuncs; kids = arguments */
    /* arithmetic */
    VC_ADD, VC_SUB, VC_MUL, VC_DIV, VC_MOD, VC_NEG,
    /* relations (VS_BOOL) */
    VC_EQ, VC_LT, VC_LE,
    /* propositional (VS_BOOL) */
    VC_AND, VC_OR, VC_NOT, VC_IMPLIES, VC_TRUE, VC_FALSE,
} VCOp;

typedef struct VCTerm VCTerm;

struct VCTerm {
    VCOp      op;
    VCSort    sort;
    uint32_t  n;        /* number of kids (arguments, for VC_APP) */
    VCTerm  **kids;
    union {
        int64_t  i;     /* VC_CONST_INT                  */
        double   r;     /* VC_CONST_REAL                 */
        uint32_t idx;   /* VC_VAR / VC_APP symbol index  */
    } as;
    uint32_t  id;       /* hash-cons id; structurally equal terms share one
                         * VCTerm*, so `a == b` IS structural equality. */
    uint32_t  hash;
};

typedef struct VCVar {
    const char *name;
    VCSort      sort;
} VCVar;

typedef struct VCUFunc {
    const char *name;
    uint32_t    arity;
    VCSort      sort;      /* result sort */
    const Form *origin;    /* source term this symbol abstracts (TUR-W0373 / RT6) */
    bool        nonlinear; /* true when it abstracts a var*var / var/var term */
} VCUFunc;

/* ------------------------------------------------------------------------- *
 * The normalized VC
 * ------------------------------------------------------------------------- */

typedef struct RefineVC {
    Arena     *arena;

    VCVar     *vars;    uint32_t n_vars,   cap_vars;
    VCUFunc   *ufuncs;  uint32_t n_ufuncs, cap_ufuncs;
    VCTerm   **hyps;    uint32_t n_hyps,   cap_hyps;
    VCTerm    *goal;

    bool       has_real;       /* any VS_REAL var/literal -> QF_UFLRA */
    bool       has_nonlinear;  /* a nonlinear subterm was abstracted   */
    const Form *nonlinear_src; /* first such subterm, for TUR-W0373    */

    /* Hash-cons table: open addressing over term ids.  Slots hold VCTerm*. */
    VCTerm   **htab;    uint32_t htab_cap, htab_len;
    uint32_t   next_id;
    /* Serial for minting DISTINCT symbols when two occurrences of the same
     * call must NOT be treated as the same value (see RefineFnInfo.pure). */
    uint32_t   fresh_ctr;
    /* How many `(/ a k)` / `(mod a k)` terms received the DISJUNCTIVE sign
     * axiom (refine_collect.c, enc_divmod_axioms); past its budget the
     * weaker conjunctive bound is used so cube expansion stays bounded. */
    uint32_t   n_divmod_splits;
} RefineVC;

RefineVC *vc_new(Arena *a);

/* Declare (or find) a variable / uninterpreted function.  Names are compared
 * by string; the returned index is stable for the VC's lifetime. */
uint32_t vc_declare_var(RefineVC *vc, const char *name, VCSort sort);
uint32_t vc_declare_ufunc(RefineVC *vc, const char *name, uint32_t arity,
                          VCSort sort, const Form *origin, bool nonlinear);

/* Term constructors.  Every one interns through the hash-cons table and folds
 * constant subterms, so `vc_mk(VC_ADD, [1, 2])` returns the interned `3`. */
VCTerm *vc_int(RefineVC *vc, int64_t v);
VCTerm *vc_real(RefineVC *vc, double v);
VCTerm *vc_bool(RefineVC *vc, bool v);
VCTerm *vc_var_ref(RefineVC *vc, uint32_t idx);
VCTerm *vc_app(RefineVC *vc, uint32_t fn, VCTerm **args, uint32_t n);
VCTerm *vc_mk(RefineVC *vc, VCOp op, VCTerm **kids, uint32_t n);
VCTerm *vc_mk1(RefineVC *vc, VCOp op, VCTerm *a);
VCTerm *vc_mk2(RefineVC *vc, VCOp op, VCTerm *a, VCTerm *b);
VCTerm *vc_not(RefineVC *vc, VCTerm *a);

void vc_add_hyp(RefineVC *vc, VCTerm *t);
void vc_set_goal(RefineVC *vc, VCTerm *t);

/* Pretty-print a term into `buf` (source-ish syntax; used by diagnostics and
 * the SMT-LIB serializer's origin notes).  Always NUL-terminates. */
void vc_term_print(const RefineVC *vc, const VCTerm *t, char *buf, size_t cap);

/* True when `t` is an arithmetic (non-boolean) term. */
static inline bool vc_is_arith(const VCTerm *t) { return t && t->sort != VS_BOOL; }

/* ------------------------------------------------------------------------- *
 * Identity: fingerprint + structural equality (RT7)
 * ------------------------------------------------------------------------- */

/* A hash of the VC's MEANING, for use as a memo key.
 *
 * Computed over a canonical alpha-renaming: variables and uninterpreted
 * function symbols are numbered by first occurrence in (hyps..., goal) rather
 * than by their source names.  Validity does not depend on what a variable is
 * called, so `x > 0 |- x + 1 > 0` and `n > 0 |- n + 1 > 0` are the same
 * question and must land on the same key -- without renaming a memo would
 * miss on every function that spells its parameter differently, which is
 * most of them.
 *
 * A hash is not proof of equality.  Never reuse a decision on a fingerprint
 * match alone; confirm with refine_vc_equal(). */
uint64_t refine_vc_fingerprint(const RefineVC *vc);

/* True when two VCs are the same question up to alpha-renaming.  Used to
 * confirm a fingerprint hit, so a hash collision costs a wasted comparison
 * rather than a wrong verdict. */
bool refine_vc_equal(const RefineVC *a, const RefineVC *b);

/* ------------------------------------------------------------------------- *
 * The solver seam
 * ------------------------------------------------------------------------- */

typedef enum RefineVerdict {
    RT_UNKNOWN = 0,  /* zero value: the always-safe answer */
    RT_VALID,
    RT_INVALID,
} RefineVerdict;

/* A counterexample, in normalized-VC variable terms.  RT6 translates it back
 * to source syntax.  `n_bindings == 0` is a legal best-effort "definitely not
 * valid, no model". */
typedef struct RefineModelBinding {
    const char *name;
    bool        is_real;
    int64_t     ival;
    double      rval;
} RefineModelBinding;

typedef struct RefineModel {
    RefineModelBinding *bindings;
    uint32_t            n;
} RefineModel;

typedef struct RefineDecision {
    RefineVerdict verdict;
    RefineModel  *model;   /* RT_INVALID only; may be NULL */
} RefineDecision;

/* A backend decides a single normalized VC.  It MUST NOT return RT_VALID
 * unless the goal is genuinely entailed by the hypotheses; returning
 * RT_UNKNOWN is always permitted and always sound. */
typedef RefineDecision (*RefineBackend)(RefineVC *vc, Arena *a);

static inline RefineDecision refine_unknown(void) {
    RefineDecision d = { RT_UNKNOWN, NULL };
    return d;
}
static inline RefineDecision refine_valid(void) {
    RefineDecision d = { RT_VALID, NULL };
    return d;
}
static inline RefineDecision refine_invalid(RefineModel *m) {
    RefineDecision d = { RT_INVALID, m };
    return d;
}

#endif /* TUR_REFINE_VC_H */
