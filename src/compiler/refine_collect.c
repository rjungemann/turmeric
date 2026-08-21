/* refine_collect.c -- RT1: constraint collector + RT2 Form -> VC encoder.
 *
 * See refine_collect.h for the model.  The encoder is deliberately narrow: it
 * accepts exactly the predicate fragment the plan permits (quantifier-free
 * linear integer/real arithmetic with equality and uninterpreted functions)
 * and returns NULL for everything else.  A NULL VC is not a failure mode --
 * it is RT_UNKNOWN, which falls back to the runtime contract check the
 * predicate would have had anyway. */

#include "refine_collect.h"

#include <stdio.h>
#include <stdlib.h>   /* getenv -- TUR_REFINE_NO_DISCHARGE test seam */
#include <string.h>

/* ------------------------------------------------------------------------- *
 * Environment
 * ------------------------------------------------------------------------- */

RefineEnv *refine_env_new(Arena *a) {
    RefineEnv *env = (RefineEnv *)arena_alloc(a, sizeof(RefineEnv));
    memset(env, 0, sizeof(*env));
    env->arena = a;
    return env;
}

void refine_env_declare(RefineEnv *env, const char *name, VCSort sort) {
    if (!env || !name) return;
    for (uint32_t i = 0; i < env->n_names; i++) {
        if (strcmp(env->names[i], name) == 0) { env->sorts[i] = sort; return; }
    }
    if (env->n_names == env->cap_names) {
        uint32_t ncap = env->cap_names ? env->cap_names * 2 : 8;
        const char **nn = (const char **)arena_alloc(env->arena, ncap * sizeof(char *));
        VCSort *ns = (VCSort *)arena_alloc(env->arena, ncap * sizeof(VCSort));
        if (env->n_names) {
            memcpy(nn, env->names, env->n_names * sizeof(char *));
            memcpy(ns, env->sorts, env->n_names * sizeof(VCSort));
        }
        env->names = nn; env->sorts = ns; env->cap_names = ncap;
    }
    env->names[env->n_names] = name;
    env->sorts[env->n_names] = sort;
    env->n_names++;
}

void refine_env_set_resolver(RefineEnv *env, RefineFnResolver fn, void *ud) {
    if (!env) return;
    env->resolve_fn = fn;
    env->resolve_ud = ud;
}

VCSort refine_env_sort_of(const RefineEnv *env, const char *name) {
    if (!env || !name) return VS_INT;
    for (uint32_t i = 0; i < env->n_names; i++)
        if (strcmp(env->names[i], name) == 0) return env->sorts[i];
    return VS_INT;
}

void refine_env_push(RefineEnv *env, const Form *pred,
                     const char *bound_var, const char *subject_name) {
    if (!env || !pred) return;
    RefineHyp *h = (RefineHyp *)arena_alloc(env->arena, sizeof(RefineHyp));
    h->pred         = pred;
    h->bound_var    = bound_var;
    h->subject_name = subject_name;
    h->next         = env->head;
    env->head       = h;
}

/* ------------------------------------------------------------------------- *
 * Obligations
 * ------------------------------------------------------------------------- */

void refine_obligations_init(RefineObligationVec *v, Arena *a) {
    v->obs = NULL; v->n = 0; v->cap = 0; v->arena = a;
}

/* TUR_REFINE_NO_DISCHARGE -- a TEST SEAM, not a feature gate.  There is no
 * `--enable`, no EXPERIMENTS[] row and no CLI flag; it lives alongside
 * TUR_REFINE_STATS / TUR_REFINE_DUMP as an env-only knob.
 *
 * It exists because the source-level differential fuzzer
 * (tests/refine-fuzz-src.py) needs a reference build in which every refinement
 * keeps its runtime check -- the ground truth for "does this program actually
 * violate its own refinement".  That is what the `refined` experiment gate gave
 * it until refinement types graduated in v0.33.0.  No shipping flag
 * reconstructs it: `--no-contracts` emits NO checks and `--keep-contracts`
 * emits checks MINUS whatever discharge elided, and it is precisely the elided
 * set that the fuzzer's miscompile property is about (both known refinement
 * soundness bugs proved something false and dropped the check that would have
 * caught it).
 *
 * Suppressing obligations HERE, at the one chokepoint every obligation flows
 * through, is what keeps this to a single conditional: all six callers already
 * treat a NULL obligation as "not proven", so nothing is decided, nothing is
 * elided, and no refinement diagnostic fires -- exactly the old gate-off
 * behavior. */
static bool refine_discharge_disabled(void) {
    static int cached = -1;
    if (cached < 0) cached = getenv("TUR_REFINE_NO_DISCHARGE") ? 1 : 0;
    return cached == 1;
}

RefineObligation *refine_collect_obligation(RefineObligationVec *v,
                                            const Form *predicate,
                                            const char *var_name,
                                            const Form *subject,
                                            VCSort base_sort,
                                            const char *base_type_name,
                                            Span loc,
                                            RefineEnv *env,
                                            const char *what,
                                            const char *fn_name) {
    if (!v || !v->arena || !predicate) return NULL;
    if (refine_discharge_disabled()) return NULL;
    RefineObligation *ob = (RefineObligation *)arena_alloc(v->arena, sizeof(RefineObligation));
    memset(ob, 0, sizeof(*ob));
    ob->predicate      = predicate;
    ob->var_name       = var_name;
    ob->subject        = subject;
    ob->base_sort      = base_sort;
    ob->base_type_name = base_type_name;
    ob->loc            = loc;
    ob->env            = env;
    ob->what           = what ? what : "value";
    ob->fn_name        = fn_name;

    if (v->n == v->cap) {
        uint32_t ncap = v->cap ? v->cap * 2 : 16;
        RefineObligation **no = (RefineObligation **)arena_alloc(v->arena,
                                                                 ncap * sizeof(*no));
        if (v->n) memcpy(no, v->obs, v->n * sizeof(*no));
        v->obs = no; v->cap = ncap;
    }
    v->obs[v->n++] = ob;
    return ob;
}

void refine_obligation_set_subst(RefineObligation *ob, Arena *a,
                                 const RefineSubst *subst, uint32_t n) {
    if (!ob || !subst || n == 0) return;
    RefineSubst *copy = (RefineSubst *)arena_alloc(a, n * sizeof(RefineSubst));
    memcpy(copy, subst, n * sizeof(RefineSubst));
    ob->subst   = copy;
    ob->n_subst = n;
}

void refine_obligation_set_frozen(RefineObligation *ob,
                                  const char **frozen_names, uint32_t n) {
    if (!ob || !frozen_names || n == 0) return;
    ob->frozen_names = frozen_names;   /* shared by reference -- immutable */
    ob->n_frozen     = n;
}

/* ------------------------------------------------------------------------- *
 * Form -> VCTerm encoder
 * ------------------------------------------------------------------------- */

#define ENC_MAX_SUBST 8
#define ENC_MAX_DEPTH 64

typedef struct EncSubst {
    const char *name;
    VCTerm     *term;
} EncSubst;

#define ENC_MAX_PROPAGATE 4
#define ENC_MAX_MEASURES  32

/* RM-B2: the sort ONE measure name is declared at, for the whole VC.
 *
 * A symbol that is Int in a hypothesis and Bool in the goal is two symbols
 * that print the same, which is how a congruence bug gets written.  So the
 * sort of every occurrence of a name is settled BEFORE any of them is
 * declared, and a name genuinely used at both sorts rejects the VC instead of
 * guessing (Unknown is always sound -- the runtime check survives). */
typedef struct EncMeasure {
    const char *name;
    VCSort      sort;
    bool        resolved;   /* sort came from the callee's declared return type */
    bool        seen_bool;  /* occurred where a PROPOSITION is required */
    bool        seen_val;   /* occurred where a VALUE is required */
} EncMeasure;

typedef struct EncSorts {
    EncMeasure m[ENC_MAX_MEASURES];
    uint32_t   n;
} EncSorts;

typedef struct Enc {
    RefineVC        *vc;
    const RefineEnv *env;
    const EncSorts  *sorts;  /* RM-B2: name -> sort, resolved once per VC */
    EncSubst         subst[ENC_MAX_SUBST];
    uint32_t         n_subst;
    const char      *fail;   /* set on the first unsupported construct */
    uint32_t         depth;
    /* Names whose return refinement is currently being propagated, so a
     * refinement that mentions its own function cannot recurse forever. */
    const char      *propagating[ENC_MAX_PROPAGATE];
    uint32_t         n_propagating;
    /* C2 / #reads: names frozen (borrowed) at this obligation's site.  A
     * `#reads w` measure whose world argument is one of these is treated as
     * congruent (stable symbol) even though its body is impure.  Shared by
     * reference from the obligation; NULL / 0 when nothing is frozen. */
    const char *const *frozen_names;
    uint32_t          n_frozen;
    /* C2 / #reads: the obligation's callee-param -> caller-arg substitution, so
     * a measure argument written in a CALLEE parameter name (the goal
     * predicate) resolves to the caller expression before the frozen check --
     * which is what keeps a shadowed binder from a false "frozen".  Set only on
     * the GOAL enc, where this subst applies; NULL on hypothesis encs, whose
     * names are already the caller's. */
    const RefineSubst *ob_subst;
    uint32_t           n_ob_subst;
} Enc;

/* C2 / #reads: is `name` frozen (borrowed) at this obligation's site? */
static bool enc_name_is_frozen(const Enc *E, const char *name) {
    if (!E || !name || !E->frozen_names) return false;
    for (uint32_t i = 0; i < E->n_frozen; i++)
        if (E->frozen_names[i] && strcmp(E->frozen_names[i], name) == 0)
            return true;
    return false;
}

/* C2 / #reads: does the `reads`-param argument of measure `f` name a value that
 * is frozen at this site?  `reads_plus1` is 1-based (0 = not a #reads measure).
 * The argument is resolved through the obligation's callee-param subst first --
 * so a goal-predicate argument in a callee parameter name is checked against
 * the actual caller expression, and a shadowed binder cannot masquerade as the
 * frozen one -- then matched by name against the frozen set. */
/* Is the argument at 0-based param index `p` a name frozen in scope here? */
static bool enc_reads_one_arg_frozen(const Enc *E, const Form *f, uint32_t p) {
    if (!f || f->tag != F_LIST || (uint32_t)(p + 1) >= f->as.list.len) return false;
    const Form *arg = f->as.list.items[p + 1];    /* items[0] is the head */
    if (!arg || arg->tag != F_SYM || !arg->as.sym) return false;
    const char *nm = arg->as.sym->name;
    for (uint32_t i = 0; i < E->n_ob_subst; i++) {
        if (E->ob_subst[i].name && strcmp(E->ob_subst[i].name, nm) == 0) {
            const Form *tgt = E->ob_subst[i].form;
            nm = (tgt && tgt->tag == F_SYM && tgt->as.sym) ? tgt->as.sym->name : NULL;
            break;
        }
    }
    return nm && enc_name_is_frozen(E, nm);
}

/* multiple-reads-params: the congruence grant is CONJUNCTIVE over the frame.
 *
 * A `#reads` frame naming several parameters says the measure is a function of
 * all of their mutable state.  Two occurrences denote one value only if every
 * one of those states is pinned at this site -- a single unfrozen named
 * parameter is enough for the measure to differ between them, which is exactly
 * the crossing check this grant elides.  So: all frozen, or no grant.
 *
 * Reading this as "any frozen" would be unsound, and silently so: it would
 * elide crossings for a measure whose other read state is free to change.  The
 * mask is never empty when we get here (an empty `#reads` is TUR-E0024), so
 * the all-quantifier cannot degenerate into a vacuous true. */
static bool enc_reads_args_frozen(const Enc *E, const Form *f, uint64_t mask) {
    if (!E || mask == 0 || E->n_frozen == 0) return false;
    for (uint32_t p = 0; p < 64; p++) {
        if (!(mask & (UINT64_C(1) << p))) continue;
        if (!enc_reads_one_arg_frozen(E, f, p)) return false;
    }
    return true;
}

static VCTerm *enc(Enc *E, const Form *f);

static bool sym_is(const Form *f, const char *s) {
    return f && (f->tag == F_SYM || f->tag == F_KEYWORD) && f->as.sym &&
           strcmp(f->as.sym->name, s) == 0;
}

/* What a list head denotes, and therefore what its operands are.  Shared by the
 * encoder and the RM-B2 prescan so the two can never disagree about which
 * positions are propositions. */
typedef enum EncHead {
    EH_MEASURE = 0,  /* unrecognised head: a named measure          */
    EH_ARITH,        /* + - * / mod       -- value operands         */
    EH_ORD,          /* < <= > >=         -- value operands         */
    EH_EQ,           /* = == not= != <>   -- sort-polymorphic       */
    EH_LOGIC,        /* and or not => ... -- proposition operands   */
} EncHead;

/* Which sort a head DEMANDS of its operands.  `=` demands neither: two
 * propositions may be compared for equality just as two numbers may, and
 * enc_cmp is what enforces that the two SIDES agree.  Treating an equality
 * operand as a value would make the perfectly ordinary
 * `(= (alive? w x) (alive? w y))` a sort conflict. */
typedef enum EncPos {
    POS_NEUTRAL = 0,  /* either sort is admissible here */
    POS_VALUE,        /* a number is required           */
    POS_PROP,         /* a proposition is required      */
} EncPos;

static EncHead enc_head_kind(const Form *h) {
    if (sym_is(h, "+") || sym_is(h, "-") || sym_is(h, "*") ||
        sym_is(h, "/") || sym_is(h, "mod")) return EH_ARITH;
    if (sym_is(h, "<") || sym_is(h, "<=") || sym_is(h, ">") ||
        sym_is(h, ">=")) return EH_ORD;
    if (sym_is(h, "=") || sym_is(h, "==") || sym_is(h, "not=") ||
        sym_is(h, "!=") || sym_is(h, "<>")) return EH_EQ;
    if (sym_is(h, "and") || sym_is(h, "or") || sym_is(h, "not") ||
        sym_is(h, "=>")  || sym_is(h, "implies")) return EH_LOGIC;
    return EH_MEASURE;
}

/* The sort every occurrence of `name` is declared at.  The prescan table is
 * authoritative; a name it never saw (a propagated return refinement is
 * encoded after the prescan ran) falls back to the callee's own sort, and
 * vc_declare_ufunc keys on the name, so a symbol still gets exactly one sort
 * either way. */
static VCSort enc_name_sort(const Enc *E, const char *name, VCSort fallback) {
    if (E->sorts) {
        for (uint32_t i = 0; i < E->sorts->n; i++)
            if (strcmp(E->sorts->m[i].name, name) == 0) return E->sorts->m[i].sort;
    }
    return fallback;
}

static VCTerm *enc_lookup(Enc *E, const char *name) {
    for (uint32_t i = 0; i < E->n_subst; i++)
        if (strcmp(E->subst[i].name, name) == 0) return E->subst[i].term;
    return NULL;
}

/* A literal-valued term: the multiplication/division linearity test. */
static bool is_const_term(const VCTerm *t) {
    return t && (t->op == VC_CONST_INT || t->op == VC_CONST_REAL);
}

/* Abstract a nonlinear application as an uninterpreted function.  Sound:
 * congruence closure still relates two occurrences of the same product, we
 * only lose the arithmetic facts.  Flags the VC so RT3 can emit TUR-W0373. */
static VCTerm *enc_nonlinear(Enc *E, const char *base, const Form *src,
                             VCTerm *a, VCTerm *b) {
    VCSort s = (a->sort == VS_REAL || b->sort == VS_REAL) ? VS_REAL : VS_INT;
    char nm[48];
    snprintf(nm, sizeof(nm), "%s_%s", base, s == VS_REAL ? "r" : "i");
    uint32_t fn = vc_declare_ufunc(E->vc, nm, 2, s, src, /*nonlinear=*/true);
    VCTerm *args[2] = { a, b };
    return vc_app(E->vc, fn, args, 2);
}

/* RM-B2: a proposition is not a number.  vc_mk would happily build
 * `(+ <bool> 1)` with an arith sort and hand a backend a term whose kid it
 * cannot interpret, so reject instead -- Unknown keeps the runtime check. */
static bool enc_want_value(Enc *E, const VCTerm *t) {
    if (t && t->sort == VS_BOOL) {
        E->fail = "arithmetic operand is a proposition, not a number";
        return false;
    }
    return t != NULL;
}

static bool enc_want_prop(Enc *E, const VCTerm *t) {
    if (t && t->sort != VS_BOOL) {
        E->fail = "logical operand does not denote a proposition";
        return false;
    }
    return t != NULL;
}

static VCTerm *enc_binary_arith(Enc *E, VCOp op, const Form *f,
                                VCTerm *a, VCTerm *b) {
    if (op == VC_MUL && !is_const_term(a) && !is_const_term(b))
        return enc_nonlinear(E, "__nl_mul", f, a, b);
    if ((op == VC_DIV || op == VC_MOD) && !is_const_term(b))
        return enc_nonlinear(E, op == VC_DIV ? "__nl_div" : "__nl_mod", f, a, b);
    return vc_mk2(E->vc, op, a, b);
}

/* n-ary fold for + - * with the language's variadic arithmetic. */
static VCTerm *enc_nary_arith(Enc *E, VCOp op, const Form *f) {
    uint32_t n = f->as.list.len;
    if (n < 2) {
        if (op == VC_SUB && n == 2 - 1) { /* unreachable: n<2 means only head */ }
        E->fail = "arithmetic operator needs at least one operand";
        return NULL;
    }
    /* (- x) is negation. */
    if (op == VC_SUB && n == 2) {
        VCTerm *a = enc(E, f->as.list.items[1]);
        if (!enc_want_value(E, a)) return NULL;
        return vc_mk1(E->vc, VC_NEG, a);
    }
    VCTerm *acc = enc(E, f->as.list.items[1]);
    if (!enc_want_value(E, acc)) return NULL;
    for (uint32_t i = 2; i < n; i++) {
        VCTerm *b = enc(E, f->as.list.items[i]);
        if (!enc_want_value(E, b)) return NULL;
        acc = enc_binary_arith(E, op, f, acc, b);
        if (!acc) return NULL;
    }
    return acc;
}

/* Chained comparison, as the language reads it: (< a b c) == (and (< a b) (< b c)). */
static VCTerm *enc_cmp(Enc *E, VCOp op, bool swap, const Form *f) {
    uint32_t n = f->as.list.len;
    if (n < 3) { E->fail = "comparison needs two operands"; return NULL; }
    VCTerm *acc = NULL;
    VCTerm *prev = enc(E, f->as.list.items[1]);
    if (!prev) return NULL;
    for (uint32_t i = 2; i < n; i++) {
        VCTerm *cur = enc(E, f->as.list.items[i]);
        if (!cur) return NULL;
        /* An ORDERING over propositions is meaningless; an EQUALITY between
         * them is not, but only when both sides are propositions.  A mixed
         * pair is a sort error the encoder must not paper over. */
        if (op != VC_EQ) {
            if (!enc_want_value(E, prev) || !enc_want_value(E, cur)) return NULL;
        } else if ((prev->sort == VS_BOOL) != (cur->sort == VS_BOOL)) {
            E->fail = "equality compares a proposition with a number";
            return NULL;
        }
        VCTerm *atom = swap ? vc_mk2(E->vc, op, cur, prev)
                            : vc_mk2(E->vc, op, prev, cur);
        acc = acc ? vc_mk2(E->vc, VC_AND, acc, atom) : atom;
        prev = cur;
    }
    return acc;
}

static VCTerm *enc_nary_bool(Enc *E, VCOp op, const Form *f) {
    uint32_t n = f->as.list.len;
    if (n < 2) return vc_bool(E->vc, op == VC_AND);
    VCTerm *acc = enc(E, f->as.list.items[1]);
    if (!enc_want_prop(E, acc)) return NULL;
    for (uint32_t i = 2; i < n; i++) {
        VCTerm *b = enc(E, f->as.list.items[i]);
        if (!enc_want_prop(E, b)) return NULL;
        acc = vc_mk2(E->vc, op, acc, b);
    }
    return acc;
}

/* An unrecognised head becomes a named measure -- an uninterpreted function
 * symbol reasoned about by congruence closure, never unfolded.  This is the
 * language rule that keeps S1 tractable. */
/* Two occurrences of a call may only be modelled as the same value when the
 * callee is KNOWN pure.  An unresolvable name is an abstract measure, which the
 * language defines as an uninterpreted mathematical function -- congruent by
 * construction.  Anything else has to say so.
 *
 * Getting this wrong is not a missed proof, it is a miscompile: with `tick`
 * counting up, `(- (tick) (tick))` encoded congruently becomes `t - t`, the
 * solver proves `t - t >= 0`, and the runtime check that would have caught the
 * real value of -1 is elided. */
static bool enc_callee_is_pure(Enc *E, const char *name, RefineFnInfo *out) {
    memset(out, 0, sizeof(*out));
    if (!E->env || !E->env->resolve_fn) return false;   /* no information: assume not */
    if (!E->env->resolve_fn(E->env->resolve_ud, name, out)) {
        /* Unresolved: an abstract measure. */
        out->pure = true;
        return true;
    }
    return out->pure;
}

/* Mint a name that cannot collide with another occurrence's. */
static const char *enc_fresh_name(Enc *E, const char *base) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s#%u", base, E->vc->fresh_ctr++);
    return arena_strdup(E->vc->arena, buf, strlen(buf));
}

static VCTerm *enc_measure(Enc *E, const Form *f) {
    const Form *head = f->as.list.items[0];
    if (head->tag != F_SYM && head->tag != F_KEYWORD) {
        E->fail = "predicate head is not a name";
        return NULL;
    }
    RefineFnInfo info;
    bool pure = enc_callee_is_pure(E, head->as.sym->name, &info);

    /* C2 / #reads: a measure declared `#reads w` (or `#reads [w g]`) is
     * congruent when EVERY named argument is FROZEN at this site -- a live borrow (the `(& w)` a `frozen`
     * region holds) proves the state it reads cannot change here, so two
     * occurrences denote one value.  This grants congruence to an otherwise
     * impure measure, and it is sound ONLY because the callee's own entry check
     * is never elided (this proof elides the caller-side crossing check, not
     * the safety check) -- see docs/guides/stateful-refinements-guide.md.  It
     * never turns a proof INTO impurity, so it cannot lose an existing one. */
    /* R2 (trusted-refinement-claims-plan, graduated 2026-08-20): refuse the
     * grant on positive evidence that the frame omits mutable state the body
     * reads (TUR-W0383's finding, carried on the resolved info).  The measure
     * then encodes fresh-per-occurrence like any unframed impure callee, and
     * the crossing gets the ordinary TUR-W0372.  Keys on "saw a read", never
     * "could not see" -- an unwalkable (inline-C) body carries no evidence and
     * keeps the trusted grant. */
    if (!pure && info.reads_params_mask != 0 &&
        !info.reads_frame_omits_state &&
        enc_reads_args_frozen(E, f, info.reads_params_mask))
        pure = true;

    /* RM-B1/RM-B2: the sort this measure symbol is declared at.  Settled once
     * per VC by the prescan (which consults the same resolver), so every
     * occurrence of a name agrees. */
    VCSort msort = enc_name_sort(E, head->as.sym->name, info.ret_sort);

    uint32_t argc = f->as.list.len - 1;
    if (argc == 0) {
        /* A nullary call is an opaque constant.  When it is not known pure,
         * each occurrence is a DIFFERENT constant -- which is exactly what
         * makes `(- (tick) (tick))` unprovable again. */
        const char *nm = pure ? head->as.sym->name
                              : enc_fresh_name(E, head->as.sym->name);
        uint32_t v = vc_declare_var(E->vc, nm, msort);
        return vc_var_ref(E->vc, v);
    }
    VCTerm **args = (VCTerm **)arena_alloc(E->vc->arena, argc * sizeof(VCTerm *));
    for (uint32_t i = 0; i < argc; i++) {
        args[i] = enc(E, f->as.list.items[i + 1]);
        if (!args[i]) return NULL;
    }
    const char *fname = pure ? head->as.sym->name
                             : enc_fresh_name(E, head->as.sym->name);
    uint32_t fn = vc_declare_ufunc(E->vc, fname, argc, msort,
                                   f, /*nonlinear=*/false);
    VCTerm *app = vc_app(E->vc, fn, args, argc);

    /* RT4: if the callee declares (or had inferred) a return refinement, that
     * predicate holds of the value this call produced -- either because it was
     * proved statically or because the runtime check would have panicked
     * otherwise.  Assert it, so the result of a refined function can satisfy
     * the next obligation instead of being an opaque term.
     *
     * The hypothesis is about THIS application term, so it is sound wherever
     * the call appears in the formula -- including under a negation. */
    if (E->env && E->env->resolve_fn) {
        const char *nm = head->as.sym->name;
        bool cycling = false;
        for (uint32_t i = 0; i < E->n_propagating; i++)
            if (strcmp(E->propagating[i], nm) == 0) { cycling = true; break; }
        /* `info` was filled by the purity probe above.  Propagating a return
         * refinement is sound whether or not the callee is pure -- the fact is
         * asserted about THIS occurrence's term, and an impure callee simply
         * has a term of its own. */
        if (!cycling && E->n_propagating < ENC_MAX_PROPAGATE &&
            info.ret_pred && info.ret_var) {
            Enc E2; memset(&E2, 0, sizeof(E2));
            E2.vc = E->vc; E2.env = E->env; E2.sorts = E->sorts;
            for (uint32_t i = 0; i < E->n_propagating; i++)
                E2.propagating[E2.n_propagating++] = E->propagating[i];
            E2.propagating[E2.n_propagating++] = nm;
            /* The refinement is written in the CALLEE's names: its own result
             * variable and its own parameters.  Bind both to what this call
             * site actually supplied. */
            E2.subst[E2.n_subst].name = info.ret_var;
            E2.subst[E2.n_subst].term = app;
            E2.n_subst++;
            for (uint32_t i = 0; i < info.n_params && i < argc &&
                                 E2.n_subst < ENC_MAX_SUBST; i++) {
                if (!info.param_names[i]) continue;
                E2.subst[E2.n_subst].name = info.param_names[i];
                E2.subst[E2.n_subst].term = args[i];
                E2.n_subst++;
            }
            VCTerm *fact = enc(&E2, info.ret_pred);
            /* A refinement we cannot encode is simply not asserted -- fewer
             * hypotheses can only make a goal harder to prove. */
            if (fact && fact->sort == VS_BOOL) vc_add_hyp(E->vc, fact);
        }
    }
    return app;
}

static VCTerm *enc(Enc *E, const Form *f) {
    if (!f) { E->fail = "empty predicate"; return NULL; }
    if (E->fail) return NULL;
    if (++E->depth > ENC_MAX_DEPTH) { E->fail = "predicate nested too deeply"; return NULL; }

    VCTerm *r = NULL;
    switch (f->tag) {
        case F_INT:   r = vc_int(E->vc, f->as.i);   break;
        case F_FLOAT: r = vc_real(E->vc, f->as.f);  break;
        case F_BOOL:  r = vc_bool(E->vc, f->as.b);  break;

        case F_KEYWORD:
        case F_SYM: {
            const char *nm = f->as.sym ? f->as.sym->name : NULL;
            if (!nm) { E->fail = "unnamed symbol"; break; }
            if (strcmp(nm, "true") == 0)  { r = vc_bool(E->vc, true);  break; }
            if (strcmp(nm, "false") == 0) { r = vc_bool(E->vc, false); break; }
            VCTerm *sub = enc_lookup(E, nm);
            if (sub) { r = sub; break; }
            uint32_t v = vc_declare_var(E->vc, nm, refine_env_sort_of(E->env, nm));
            r = vc_var_ref(E->vc, v);
            break;
        }

        case F_LIST: {
            if (f->as.list.len == 0) { E->fail = "empty list in predicate"; break; }
            const Form *h = f->as.list.items[0];
            if      (sym_is(h, "+"))    r = enc_nary_arith(E, VC_ADD, f);
            else if (sym_is(h, "-"))    r = enc_nary_arith(E, VC_SUB, f);
            else if (sym_is(h, "*"))    r = enc_nary_arith(E, VC_MUL, f);
            else if (sym_is(h, "/"))    r = enc_nary_arith(E, VC_DIV, f);
            else if (sym_is(h, "mod"))  r = enc_nary_arith(E, VC_MOD, f);
            else if (sym_is(h, "=") || sym_is(h, "==")) r = enc_cmp(E, VC_EQ, false, f);
            else if (sym_is(h, "<"))    r = enc_cmp(E, VC_LT, false, f);
            else if (sym_is(h, "<="))   r = enc_cmp(E, VC_LE, false, f);
            else if (sym_is(h, ">"))    r = enc_cmp(E, VC_LT, true,  f);
            else if (sym_is(h, ">="))   r = enc_cmp(E, VC_LE, true,  f);
            else if (sym_is(h, "not=") || sym_is(h, "!=") || sym_is(h, "<>")) {
                VCTerm *eq = enc_cmp(E, VC_EQ, false, f);
                r = eq ? vc_not(E->vc, eq) : NULL;
            }
            else if (sym_is(h, "and"))  r = enc_nary_bool(E, VC_AND, f);
            else if (sym_is(h, "or"))   r = enc_nary_bool(E, VC_OR, f);
            else if (sym_is(h, "not")) {
                if (f->as.list.len != 2) { E->fail = "not takes one operand"; break; }
                VCTerm *a = enc(E, f->as.list.items[1]);
                r = enc_want_prop(E, a) ? vc_not(E->vc, a) : NULL;
            }
            else if (sym_is(h, "=>") || sym_is(h, "implies")) {
                if (f->as.list.len != 3) { E->fail = "=> takes two operands"; break; }
                VCTerm *a = enc(E, f->as.list.items[1]);
                if (!enc_want_prop(E, a)) break;
                VCTerm *b = enc(E, f->as.list.items[2]);
                if (!enc_want_prop(E, b)) break;
                r = vc_mk2(E->vc, VC_IMPLIES, a, b);
            }
            else r = enc_measure(E, f);
            break;
        }

        default:
            E->fail = "unsupported form in predicate";
            break;
    }
    E->depth--;
    return E->fail ? NULL : r;
}

/* ------------------------------------------------------------------------- *
 * RM-B2: measure sort prescan
 *
 * Runs over every Form the VC will encode, BEFORE any symbol is declared, and
 * answers one question per measure name: Int, Real, or Bool?
 *
 *   - the name resolves to a function  -> its declared return type decides
 *     (RM-B1; this is what makes `(alive? w x)` a proposition and stops a
 *     float-returning `norm` from being declared Int and then integer-tightened)
 *   - the name resolves to nothing     -> it is an ABSTRACT measure with no
 *     declaration to read, so POSITION decides: Bool where the predicate
 *     grammar requires a proposition (the goal itself, or an operand of
 *     and/or/not/=>), Int everywhere else.
 *
 * A name used at BOTH sorts rejects the VC.  Picking one would declare a
 * symbol that means different things in the hypotheses and in the goal, which
 * is precisely how a congruence bug is written; the runtime check still covers
 * the obligation either way.
 * ------------------------------------------------------------------------- */

static void presort_note(EncSorts *S, const RefineEnv *env,
                         const char *name, EncPos pos) {
    EncMeasure *m = NULL;
    for (uint32_t i = 0; i < S->n; i++)
        if (strcmp(S->m[i].name, name) == 0) { m = &S->m[i]; break; }
    if (!m) {
        /* Table full: leave the name out.  enc_name_sort then falls back to the
         * callee's own sort, which is still ONE sort per name. */
        if (S->n == ENC_MAX_MEASURES) return;
        m = &S->m[S->n++];
        memset(m, 0, sizeof(*m));
        m->name = name;
        m->sort = VS_INT;
        RefineFnInfo info; memset(&info, 0, sizeof(info));
        if (env && env->resolve_fn && env->resolve_fn(env->resolve_ud, name, &info)) {
            m->resolved = true;
            m->sort     = info.ret_sort;
        }
    }
    if (pos == POS_PROP)       m->seen_bool = true;
    else if (pos == POS_VALUE) m->seen_val  = true;
}

static void presort_walk(EncSorts *S, const RefineEnv *env, const Form *f,
                         EncPos pos, uint32_t depth) {
    if (!f || depth > ENC_MAX_DEPTH) return;
    if (f->tag != F_LIST || f->as.list.len == 0) return;
    const Form *h = f->as.list.items[0];
    EncHead k = enc_head_kind(h);
    if (k == EH_MEASURE) {
        if ((h->tag == F_SYM || h->tag == F_KEYWORD) && h->as.sym)
            presort_note(S, env, h->as.sym->name, pos);
        /* A measure's own arity and parameter types are not known here (an
         * abstract measure has none), so its ARGUMENTS demand nothing. */
        for (uint32_t i = 1; i < f->as.list.len; i++)
            presort_walk(S, env, f->as.list.items[i], POS_NEUTRAL, depth + 1);
        return;
    }
    EncPos kid = (k == EH_LOGIC) ? POS_PROP
               : (k == EH_EQ)    ? POS_NEUTRAL
                                 : POS_VALUE;
    for (uint32_t i = 1; i < f->as.list.len; i++)
        presort_walk(S, env, f->as.list.items[i], kid, depth + 1);
}

/* Settle every unresolved name's sort and report the first name that cannot
 * have one.  Returns NULL when the table is consistent.
 *
 * Only an UNRESOLVED name can conflict: a resolved one has exactly one sort,
 * its callee's, whatever positions it turns up in.  A resolved measure used in
 * the wrong kind of position is a local error and is handled locally, by the
 * enc_want_value / enc_want_prop guards -- which drop just that hypothesis
 * (sound: fewer hypotheses only make a goal harder) instead of discarding the
 * whole VC. */
static const char *presort_finish(EncSorts *S) {
    for (uint32_t i = 0; i < S->n; i++) {
        EncMeasure *m = &S->m[i];
        if (m->resolved) continue;
        m->sort = m->seen_bool ? VS_BOOL : VS_INT;
        if (m->seen_bool && m->seen_val) return m->name;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- *
 * RT2: obligation -> normalized VC
 * ------------------------------------------------------------------------- */

RefineVC *refine_vc_build(RefineObligation *ob, Arena *a, const char **out_reason) {
    if (out_reason) *out_reason = NULL;
    if (!ob || !ob->predicate) {
        if (out_reason) *out_reason = "no predicate";
        return NULL;
    }

    RefineVC *vc = vc_new(a);

    /* Declare every in-scope name with its sort up front so the encoder never
     * has to guess (an undeclared name still defaults to VS_INT). */
    if (ob->env) {
        for (uint32_t i = 0; i < ob->env->n_names; i++)
            vc_declare_var(vc, ob->env->names[i], ob->env->sorts[i]);
    }

    /* --- RM-B2: settle every measure's sort before declaring any symbol --- */
    EncSorts sorts; memset(&sorts, 0, sizeof(sorts));
    for (RefineHyp *h = ob->env ? ob->env->head : NULL; h; h = h->next)
        presort_walk(&sorts, ob->env, h->pred, POS_PROP, 0);
    /* A sibling substitution stands in for a CALLEE parameter whose type this
     * side does not have, so it demands nothing.  The subject does: it stands
     * in for the refinement's bound variable, whose sort is the base type's. */
    for (uint32_t i = 0; i < ob->n_subst; i++)
        presort_walk(&sorts, ob->env, ob->subst[i].form, POS_NEUTRAL, 0);
    presort_walk(&sorts, ob->env, ob->subject,
                 ob->base_sort == VS_BOOL ? POS_PROP : POS_VALUE, 0);
    presort_walk(&sorts, ob->env, ob->predicate, POS_PROP, 0);
    const char *clash = presort_finish(&sorts);
    if (clash) {
        if (out_reason) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "measure '%s' is used both as a proposition and as a value",
                     clash);
            *out_reason = arena_strdup(a, buf, strlen(buf));
        }
        return NULL;
    }

    /* --- hypotheses ------------------------------------------------------ */
    for (RefineHyp *h = ob->env ? ob->env->head : NULL; h; h = h->next) {
        Enc E; memset(&E, 0, sizeof(E));
        E.vc = vc; E.env = ob->env; E.sorts = &sorts;
        /* C2 / #reads: a hypothesis (a recovered guard) is written in the
         * CALLER's names, so it needs the frozen set but NOT the callee-param
         * subst. */
        E.frozen_names = ob->frozen_names; E.n_frozen = ob->n_frozen;
        if (h->bound_var && h->subject_name) {
            uint32_t v = vc_declare_var(vc, h->subject_name,
                                        refine_env_sort_of(ob->env, h->subject_name));
            E.subst[E.n_subst].name = h->bound_var;
            E.subst[E.n_subst].term = vc_var_ref(vc, v);
            E.n_subst++;
        }
        VCTerm *t = enc(&E, h->pred);
        /* A hypothesis we cannot encode is simply dropped: fewer hypotheses
         * can only make the goal HARDER to prove, never easier, so this stays
         * on the safe side of the soundness invariant. */
        if (t) vc_add_hyp(vc, t);
    }

    /* --- goal ------------------------------------------------------------ */
    Enc E; memset(&E, 0, sizeof(E));
    E.vc = vc; E.env = ob->env; E.sorts = &sorts;
    /* C2 / #reads: the goal predicate is written in the CALLEE's parameter
     * names, so it needs both the frozen set and the callee-param subst to
     * resolve a `#reads` world argument back to the caller expression. */
    E.frozen_names = ob->frozen_names; E.n_frozen = ob->n_frozen;
    E.ob_subst = ob->subst; E.n_ob_subst = ob->n_subst;
    /* Sibling substitutions first (a call-site crossing replaces every callee
     * parameter name with the caller's argument), then the refinement's own
     * bound variable.  Encoding each substituted form BEFORE the map is
     * consulted is deliberate: an argument expression is written in the
     * caller's names, so it must not be rewritten by the callee's. */
    for (uint32_t i = 0; i < ob->n_subst && E.n_subst < ENC_MAX_SUBST; i++) {
        if (!ob->subst[i].name || !ob->subst[i].form) continue;
        Enc E2; memset(&E2, 0, sizeof(E2));
        E2.vc = vc; E2.env = ob->env; E2.sorts = &sorts;
        VCTerm *t = enc(&E2, ob->subst[i].form);
        if (!t) continue;   /* un-encodable argument: leave the name free */
        E.subst[E.n_subst].name = ob->subst[i].name;
        E.subst[E.n_subst].term = t;
        E.n_subst++;
    }
    if (ob->var_name && ob->subject) {
        Enc E2; memset(&E2, 0, sizeof(E2));
        E2.vc = vc; E2.env = ob->env; E2.sorts = &sorts;
        VCTerm *subj = enc(&E2, ob->subject);
        if (!subj) {
            if (out_reason) *out_reason = E2.fail ? E2.fail : "subject expression is outside the supported fragment";
            return NULL;
        }
        if (E.n_subst < ENC_MAX_SUBST) {
            E.subst[E.n_subst].name = ob->var_name;
            E.subst[E.n_subst].term = subj;
            E.n_subst++;
        }
    }
    VCTerm *goal = enc(&E, ob->predicate);
    if (!goal) {
        if (out_reason) *out_reason = E.fail ? E.fail : "predicate is outside the supported fragment";
        return NULL;
    }
    if (goal->sort != VS_BOOL) {
        if (out_reason) *out_reason = "predicate does not denote a proposition";
        return NULL;
    }
    vc_set_goal(vc, goal);
    return vc;
}
