#ifndef TUR_REFINE_SOLVER_H
#define TUR_REFINE_SOLVER_H

/* refine_solver.h -- shared declarations for the in-house staged decision
 * procedure (S0..S3).  These stages ARE the solver, and always were the only
 * one a user ran: the Z3 backend was dev-only scaffolding, and was retired
 * outright in 0.32.5.
 *
 * Every stage implements the RefineBackend seam from refine_vc.h and obeys the
 * one-directional soundness invariant: never RT_VALID unless genuinely
 * entailed; RT_UNKNOWN is always safe.  A stage returns RT_UNKNOWN for
 * anything outside its competence and the next stage picks it up.
 *
 *   S0  normalize + trivial discharge      refine_solver_s0.c
 *   S1  congruence closure (EUF)           refine_solver_euf.c
 *   S2  linear arithmetic (Fourier-Motzkin) refine_solver_arith.c
 *   S3  Nelson-Oppen combination of the two refine_solver_no.c
 *
 * Common shape: to prove `hyps |= goal` we refute `hyps AND (not goal)`.  That
 * formula is put in NNF and expanded to a small DNF -- a set of CUBES, each a
 * conjunction of literals.  The goal is valid iff EVERY cube is unsatisfiable.
 * Cube expansion is capped; blowing the cap yields RT_UNKNOWN (this is the
 * "naive S4" the plan permits -- we never build a DPLL(T) engine).
 *
 * See docs/archive/refinement-types-plan.md (S0--S4). */

#include "refine_vc.h"

/* Caps.  Every one of these, when hit, produces RT_UNKNOWN -> runtime check. */
#define REFINE_MAX_CUBES        64
#define REFINE_MAX_CUBE_LITS    64
/* Recursion backstop for the DNF product expansion.  Each frame either splits
 * one disjunction or flattens one conjunction, both of which strictly reduce
 * the formula's remaining boolean structure -- so this should never be reached
 * by a well-formed input.  It exists because the alternative to a cap on a
 * recursive walk is a stack overflow, and a crashed compiler is worse than a
 * kept runtime check. */
#define REFINE_MAX_EXPAND_DEPTH 256
#define REFINE_MAX_LA_VARS      32
#define REFINE_MAX_LA_CONSTR    512
#define REFINE_MAX_EUF_TERMS    512
/* S3's two caps live here rather than in refine_solver_no.c so the telemetry
 * below can report a peak against the limit it is a peak of. */
#define NO_MAX_ROUNDS           4
/* Raised 8 -> 16 on SX0(b)'s evidence (solver-extension-plan SX5).  It was the
 * ONLY cap in the solver with a live signal: it turned away eligible terms on
 * four units across the three swept populations, every time by exactly one
 * (9 eligible against a cap of 8), while every other cap sat on 72-98%
 * headroom.  Not free -- the exchange is quadratic in the shared set and each
 * `la_entails_eq` runs Fourier-Motzkin twice, so the pair work per S3 cube is
 * 4x at the new cap and is paid by every obligation reaching S3, not only the
 * ones near the cap.  Measured before landing: no verdict moved on the 125
 * corpus benchmarks or the 89 in-tree refinement fixtures, and the corpus
 * replay did not slow measurably.  Numbers and method in
 * docs/archive/history/no-max-shared-raise.md. */
#define NO_MAX_SHARED           16

/* ------------------------------------------------------------------------- *
 * Cap telemetry
 * ------------------------------------------------------------------------- */

/* Every cap above degrades to RT_UNKNOWN when it bites, which is sound but
 * silent: from the outside, an obligation the solver capped out on and one
 * that was never in its competence look exactly alike.  These counters make
 * the difference visible under TUR_REFINE_STATS=1.
 *
 * They are also decision data.  The solver-extension plan gates its two
 * largest phases on this measurement -- SX4 (incremental simplex) is worth
 * building only if REFINE_MAX_LA_CONSTR actually bites, and SX6 (boolean
 * structure beyond small DNF) only if the cube caps do.  See
 * docs/upcoming/solver-extension-plan.md, SX0(b).
 *
 * `hits` counts the times a cap fired.  `peak` is the high-water mark of the
 * quantity that cap bounds, recorded on every query rather than only on the
 * ones that overflow -- so a cap that never fires still reports how much
 * headroom was left, which is the difference between "we are nowhere near
 * this" and "we are one wide function away from it". */
typedef struct RefineCapStats {
    uint32_t cubes_hits,        cubes_peak;
    uint32_t cube_lits_hits,    cube_lits_peak;
    uint32_t expand_depth_hits, expand_depth_peak;
    uint32_t la_vars_hits,      la_vars_peak;
    uint32_t la_constr_hits,    la_constr_peak;
    uint32_t la_fm_hits;        /* FM elimination backed off before growing   */
    uint32_t euf_terms_hits,    euf_terms_peak;
    uint32_t no_shared_hits,    no_shared_peak;
    uint32_t no_rounds_hits;    /* exchange still progressing when rounds ran out */
} RefineCapStats;

/* Mutable on purpose: the stages bump their own counters in place, which is
 * cheaper and clearer than threading a stats pointer through every seam. */
RefineCapStats *refine_caps(void);
void refine_caps_reset(void);

static inline void refine_cap_peak(uint32_t *slot, uint32_t v) {
    if (v > *slot) *slot = v;
}
/* True when any cap fired since the last reset -- lets the printer stay quiet
 * on the overwhelmingly common "nothing was capped" compile. */
bool refine_caps_any(void);

/* ------------------------------------------------------------------------- *
 * Cubes
 * ------------------------------------------------------------------------- */

/* A literal is a VCTerm that is either an atom or (not atom); use
 * refine_lit_atom / refine_lit_is_neg to take it apart. */
typedef struct VCCube {
    VCTerm **lits;
    uint32_t n;
} VCCube;

typedef struct VCCubeSet {
    VCCube  *cubes;
    uint32_t n;
    bool     overflow;  /* cap hit -- caller must answer RT_UNKNOWN */
    bool     trivial;   /* the refutation formula folded to `false` outright */
} VCCubeSet;

static inline bool refine_lit_is_neg(const VCTerm *l) { return l && l->op == VC_NOT; }
static inline VCTerm *refine_lit_atom(VCTerm *l) {
    return refine_lit_is_neg(l) ? l->kids[0] : l;
}

/* Build the DNF cube set of `hyps AND (not goal)`.  Returns false (and sets
 * out->overflow) when a cap is hit. */
bool refine_cubes_build(RefineVC *vc, Arena *a, VCCubeSet *out);

/* ------------------------------------------------------------------------- *
 * S1: congruence closure (EUF)
 * ------------------------------------------------------------------------- */

typedef struct EufState EufState;

EufState *euf_new(RefineVC *vc, Arena *a);
/* Assert every literal of `c`.  Returns false when a contradiction is found. */
bool      euf_assert_cube(EufState *st, const VCCube *c);
/* Assert `a == b` and re-close.  Returns false on contradiction. */
bool      euf_assert_eq(EufState *st, VCTerm *a, VCTerm *b);
bool      euf_equal(EufState *st, VCTerm *a, VCTerm *b);
/* Terms EUF knows about, for the S3 equality exchange. */
uint32_t  euf_term_count(const EufState *st);
VCTerm   *euf_term_at(const EufState *st, uint32_t i);

/* ------------------------------------------------------------------------- *
 * S2: linear arithmetic (Fourier-Motzkin over the rationals)
 * ------------------------------------------------------------------------- */

typedef struct LaState LaState;

LaState *la_new(RefineVC *vc, Arena *a);
/* Encode every arithmetic literal of `c`.  Non-arithmetic literals are
 * ignored (that only loses information, never soundness).  Returns false when
 * the encoding gives up (too many variables/constraints). */
bool     la_assert_cube(LaState *st, const VCCube *c);
bool     la_assert_le(LaState *st, VCTerm *lhs, VCTerm *rhs, bool strict);
bool     la_assert_eq(LaState *st, VCTerm *a, VCTerm *b);
/* True when the asserted constraint set is provably unsatisfiable. */
bool     la_unsat(LaState *st);
/* True when the constraints entail `a == b` (used by the S3 exchange). */
bool     la_entails_eq(LaState *st, VCTerm *a, VCTerm *b);
/* True when `t` is a term the LA encoder treats as an opaque variable
 * (a VC_VAR or an uninterpreted application) -- i.e. a shared term. */
bool     la_is_shared_term(const VCTerm *t);

/* ------------------------------------------------------------------------- *
 * The stages
 * ------------------------------------------------------------------------- */

/* Bounded counterexample search.  Runs AFTER the proving chain has come back
 * Unknown: enumerate a small candidate assignment space and EVALUATE
 * `hyps AND (not goal)` exactly.  A satisfying assignment is a genuine
 * counterexample, so this is the only thing in the solver that may answer
 * RT_INVALID -- and it does so with a model, never a guess.  Returns NULL when
 * it finds nothing or the VC is outside its (deliberately tiny) scope. */
RefineModel *refine_model_search(RefineVC *vc, Arena *a);

RefineDecision refine_s0_decide(RefineVC *vc, Arena *a);
RefineDecision refine_s1_decide(RefineVC *vc, Arena *a);
RefineDecision refine_s2_decide(RefineVC *vc, Arena *a);
RefineDecision refine_s3_decide(RefineVC *vc, Arena *a);

#endif /* TUR_REFINE_SOLVER_H */
