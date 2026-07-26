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

typedef struct Enc {
    RefineVC        *vc;
    const RefineEnv *env;
    EncSubst         subst[ENC_MAX_SUBST];
    uint32_t         n_subst;
    const char      *fail;   /* set on the first unsupported construct */
    uint32_t         depth;
    /* Names whose return refinement is currently being propagated, so a
     * refinement that mentions its own function cannot recurse forever. */
    const char      *propagating[ENC_MAX_PROPAGATE];
    uint32_t         n_propagating;
} Enc;

static VCTerm *enc(Enc *E, const Form *f);

static bool sym_is(const Form *f, const char *s) {
    return f && (f->tag == F_SYM || f->tag == F_KEYWORD) && f->as.sym &&
           strcmp(f->as.sym->name, s) == 0;
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
        if (!a) return NULL;
        return vc_mk1(E->vc, VC_NEG, a);
    }
    VCTerm *acc = enc(E, f->as.list.items[1]);
    if (!acc) return NULL;
    for (uint32_t i = 2; i < n; i++) {
        VCTerm *b = enc(E, f->as.list.items[i]);
        if (!b) return NULL;
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
    if (!acc) return NULL;
    for (uint32_t i = 2; i < n; i++) {
        VCTerm *b = enc(E, f->as.list.items[i]);
        if (!b) return NULL;
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

    /* RM-B1: declare the measure with the sort of its callee's result rather
     * than a hard-coded VS_INT.  `enc_callee_is_pure` fills `info.ret_sort`
     * from the resolved callee's return type; an abstract (unresolved) measure
     * leaves it at VS_INT, its historical default. */
    VCSort rsort = info.ret_sort;

    uint32_t argc = f->as.list.len - 1;
    if (argc == 0) {
        /* A nullary call is an opaque constant.  When it is not known pure,
         * each occurrence is a DIFFERENT constant -- which is exactly what
         * makes `(- (tick) (tick))` unprovable again. */
        const char *nm = pure ? head->as.sym->name
                              : enc_fresh_name(E, head->as.sym->name);
        uint32_t v = vc_declare_var(E->vc, nm, rsort);
        return vc_var_ref(E->vc, v);
    }
    VCTerm **args = (VCTerm **)arena_alloc(E->vc->arena, argc * sizeof(VCTerm *));
    for (uint32_t i = 0; i < argc; i++) {
        args[i] = enc(E, f->as.list.items[i + 1]);
        if (!args[i]) return NULL;
    }
    const char *fname = pure ? head->as.sym->name
                             : enc_fresh_name(E, head->as.sym->name);
    uint32_t fn = vc_declare_ufunc(E->vc, fname, argc, rsort,
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
            E2.vc = E->vc; E2.env = E->env;
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
                r = a ? vc_not(E->vc, a) : NULL;
            }
            else if (sym_is(h, "=>") || sym_is(h, "implies")) {
                if (f->as.list.len != 3) { E->fail = "=> takes two operands"; break; }
                VCTerm *a = enc(E, f->as.list.items[1]);
                VCTerm *b = a ? enc(E, f->as.list.items[2]) : NULL;
                r = b ? vc_mk2(E->vc, VC_IMPLIES, a, b) : NULL;
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

    /* --- hypotheses ------------------------------------------------------ */
    for (RefineHyp *h = ob->env ? ob->env->head : NULL; h; h = h->next) {
        Enc E; memset(&E, 0, sizeof(E));
        E.vc = vc; E.env = ob->env;
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
    E.vc = vc; E.env = ob->env;
    /* Sibling substitutions first (a call-site crossing replaces every callee
     * parameter name with the caller's argument), then the refinement's own
     * bound variable.  Encoding each substituted form BEFORE the map is
     * consulted is deliberate: an argument expression is written in the
     * caller's names, so it must not be rewritten by the callee's. */
    for (uint32_t i = 0; i < ob->n_subst && E.n_subst < ENC_MAX_SUBST; i++) {
        if (!ob->subst[i].name || !ob->subst[i].form) continue;
        Enc E2; memset(&E2, 0, sizeof(E2));
        E2.vc = vc; E2.env = ob->env;
        VCTerm *t = enc(&E2, ob->subst[i].form);
        if (!t) continue;   /* un-encodable argument: leave the name free */
        E.subst[E.n_subst].name = ob->subst[i].name;
        E.subst[E.n_subst].term = t;
        E.n_subst++;
    }
    if (ob->var_name && ob->subject) {
        Enc E2; memset(&E2, 0, sizeof(E2));
        E2.vc = vc; E2.env = ob->env;
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
