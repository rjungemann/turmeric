/* refine_smtlib.c -- RT2: normalized VC -> SMT-LIB2 serializer.
 *
 * Term encoding is the obvious structural map.  Because nonlinear and measure
 * terms were already abstracted to uninterpreted functions when the VC was
 * built (refine_collect.c), this file has no special cases for them -- they
 * are just `declare-fun` applications. */

#include "refine_smtlib.h"

#include <ctype.h>     /* reader: isspace / isdigit */
#include <math.h>
#include <stdlib.h>    /* reader: strtoll / strtod */
#include <string.h>

static const char *sort_name(VCSort s) {
    switch (s) {
        case VS_REAL: return "Real";
        case VS_BOOL: return "Bool";
        default:      return "Int";
    }
}

static void emit_term(const RefineVC *vc, const VCTerm *t, Buf *out);

static void emit_nary(const RefineVC *vc, const VCTerm *t, const char *op, Buf *out) {
    buf_printf(out, "(%s", op);
    for (uint32_t i = 0; i < t->n; i++) { buf_putc(out, ' '); emit_term(vc, t->kids[i], out); }
    buf_putc(out, ')');
}

static void emit_term(const RefineVC *vc, const VCTerm *t, Buf *out) {
    if (!t) { buf_puts(out, "true"); return; }
    switch (t->op) {
        case VC_TRUE:  buf_puts(out, "true");  return;
        case VC_FALSE: buf_puts(out, "false"); return;
        case VC_CONST_INT:
            if (t->as.i < 0) buf_printf(out, "(- %lld)", (long long)(-t->as.i));
            else             buf_printf(out, "%lld", (long long)t->as.i);
            return;
        case VC_CONST_REAL: {
            double v = t->as.r;
            if (v < 0) buf_printf(out, "(- %.17g)", -v);
            else       buf_printf(out, "%.17g", v);
            return;
        }
        case VC_VAR:
            buf_puts(out, t->as.idx < vc->n_vars ? vc->vars[t->as.idx].name : "|?var|");
            return;
        case VC_APP: {
            const VCUFunc *u = t->as.idx < vc->n_ufuncs ? &vc->ufuncs[t->as.idx] : NULL;
            buf_printf(out, "(%s", u ? u->name : "|?fn|");
            for (uint32_t i = 0; i < t->n; i++) { buf_putc(out, ' '); emit_term(vc, t->kids[i], out); }
            buf_putc(out, ')');
            return;
        }
        case VC_ADD: emit_nary(vc, t, "+", out); return;
        case VC_SUB: emit_nary(vc, t, "-", out); return;
        case VC_MUL: emit_nary(vc, t, "*", out); return;
        case VC_DIV: emit_nary(vc, t, vc->has_real ? "/" : "div", out); return;
        case VC_MOD: emit_nary(vc, t, "mod", out); return;
        case VC_NEG: emit_nary(vc, t, "-", out); return;
        case VC_EQ:  emit_nary(vc, t, "=", out); return;
        case VC_LT:  emit_nary(vc, t, "<", out); return;
        case VC_LE:  emit_nary(vc, t, "<=", out); return;
        case VC_AND: emit_nary(vc, t, "and", out); return;
        case VC_OR:  emit_nary(vc, t, "or", out); return;
        case VC_NOT: emit_nary(vc, t, "not", out); return;
        case VC_IMPLIES: emit_nary(vc, t, "=>", out); return;
    }
    buf_puts(out, "true");
}

void refine_smtlib_emit(const RefineVC *vc, Buf *out) {
    if (!vc) return;
    buf_printf(out, "(set-logic %s)\n", vc->has_real ? "QF_UFLRA" : "QF_UFLIA");

    for (uint32_t i = 0; i < vc->n_vars; i++)
        buf_printf(out, "(declare-const %s %s)\n",
                   vc->vars[i].name, sort_name(vc->vars[i].sort));

    for (uint32_t i = 0; i < vc->n_ufuncs; i++) {
        const VCUFunc *u = &vc->ufuncs[i];
        buf_printf(out, "(declare-fun %s (", u->name);
        for (uint32_t j = 0; j < u->arity; j++)
            buf_printf(out, "%s%s", j ? " " : "", vc->has_real ? "Real" : "Int");
        buf_printf(out, ") %s)\n", sort_name(u->sort));
    }

    for (uint32_t i = 0; i < vc->n_hyps; i++) {
        buf_puts(out, "(assert ");
        emit_term(vc, vc->hyps[i], out);
        buf_puts(out, ")\n");
    }

    buf_puts(out, "(assert (not ");
    emit_term(vc, vc->goal, out);
    buf_puts(out, "))\n(check-sat)\n(get-model)\n");
}

/* ========================================================================= *
 * READER: SMT-LIB2 text -> RefineVC
 *
 * Lifted here from tests/unit/refine_corpus.c, where it was reachable only by
 * the ctest harness (SX8a).  Putting it beside the writer makes this file the
 * whole SMT-LIB2 seam in both directions, which is what lets `tur smt` answer
 * queries from outside the compile pipeline and lets an external harness
 * differentially test any solver against `tur` -- without `tur` ever linking
 * one.
 *
 * ## How a satisfiability script becomes an entailment query
 *
 * SMT-LIB `:status` is a claim about the SATISFIABILITY of the assertion set.
 * The in-house chain decides ENTAILMENT -- `hyps |- goal`, internally "is
 * `hyps AND NOT goal` unsatisfiable".  The two line up exactly by taking every
 * `(assert phi)` as a hypothesis and `false` as the goal:
 *
 *     hyps |- false   is VALID   iff   hyps is UNSAT
 *
 * ## Why the reader skips rather than guesses
 *
 * A script using anything outside the fragment is SKIPPED WHOLE, never
 * partially parsed.  Silently dropping an assertion weakens the hypotheses,
 * which cannot make the chain prove something it should not -- but it would
 * quietly turn a real query into a trivial one and report an answer for work
 * not done.  That is the one dishonest failure mode a query surface can have,
 * so every skip carries a reason the caller is expected to surface.
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * S-expression reader
 * ------------------------------------------------------------------------- */

typedef enum { SX_ATOM, SX_LIST } SxKind;

typedef struct Sx {
    SxKind kind;
    char   *atom;          /* SX_ATOM */
    struct Sx **kids;      /* SX_LIST */
    uint32_t n, cap;
} Sx;

typedef struct {
    const char *p;
    const char *end;
    Arena      *a;
    const char *err;
} SxReader;

static void sx_push(Arena *a, Sx *l, Sx *k) {
    if (l->n == l->cap) {
        uint32_t nc = l->cap ? l->cap * 2 : 8;
        Sx **nb = (Sx **)arena_alloc(a, nc * sizeof(Sx *));
        if (l->n) memcpy(nb, l->kids, l->n * sizeof(Sx *));
        l->kids = nb; l->cap = nc;
    }
    l->kids[l->n++] = k;
}

static void sx_skip_ws(SxReader *r) {
    for (;;) {
        while (r->p < r->end && isspace((unsigned char)*r->p)) r->p++;
        if (r->p < r->end && *r->p == ';') {          /* line comment */
            while (r->p < r->end && *r->p != '\n') r->p++;
            continue;
        }
        return;
    }
}

static Sx *sx_read(SxReader *r) {
    sx_skip_ws(r);
    if (r->p >= r->end) return NULL;

    if (*r->p == '(') {
        r->p++;
        Sx *l = (Sx *)arena_alloc(r->a, sizeof(Sx));
        memset(l, 0, sizeof(*l));
        l->kind = SX_LIST;
        for (;;) {
            sx_skip_ws(r);
            if (r->p >= r->end) { r->err = "unterminated list"; return NULL; }
            if (*r->p == ')') { r->p++; return l; }
            Sx *k = sx_read(r);
            if (!k) { if (!r->err) r->err = "bad element"; return NULL; }
            sx_push(r->a, l, k);
        }
    }
    if (*r->p == ')') { r->err = "unexpected ')'"; return NULL; }

    const char *start = r->p;
    if (*r->p == '|') {                                /* |quoted symbol| */
        r->p++;
        while (r->p < r->end && *r->p != '|') r->p++;
        if (r->p >= r->end) { r->err = "unterminated |symbol|"; return NULL; }
        r->p++;
    } else if (*r->p == '"') {                         /* string literal */
        r->p++;
        while (r->p < r->end) {
            if (*r->p == '"') {
                if (r->p + 1 < r->end && r->p[1] == '"') { r->p += 2; continue; }
                r->p++; break;
            }
            r->p++;
        }
    } else {
        while (r->p < r->end && !isspace((unsigned char)*r->p) &&
               *r->p != '(' && *r->p != ')' && *r->p != ';')
            r->p++;
    }
    size_t n = (size_t)(r->p - start);
    if (n == 0) { r->err = "empty atom"; return NULL; }
    Sx *s = (Sx *)arena_alloc(r->a, sizeof(Sx));
    memset(s, 0, sizeof(*s));
    s->kind = SX_ATOM;
    s->atom = (char *)arena_alloc(r->a, n + 1);
    memcpy(s->atom, start, n);
    s->atom[n] = 0;
    return s;
}

static bool sx_is(const Sx *s, const char *name) {
    return s && s->kind == SX_ATOM && strcmp(s->atom, name) == 0;
}
static bool sx_head_is(const Sx *s, const char *name) {
    return s && s->kind == SX_LIST && s->n > 0 && sx_is(s->kids[0], name);
}

/* ------------------------------------------------------------------------- *
 * SMT-LIB2 -> RefineVC, for the fragment the chain decides
 * ------------------------------------------------------------------------- */

/* Capacity, not policy.  These were first sized for hand-written benchmarks and
 * were far too small for the real library: a single QF_UFLIA benchmark in the
 * 2025 release carries a 287-deep `let` chain, and every one of those files
 * skipped as "too many let bindings" -- a corpus that parses nothing tests
 * nothing.  The depth caps guard `tr_term`'s C-stack recursion; they were
 * raised 4000 -> 6000 and 6000 -> 8000 against a MEASURED limit, not a
 * guessed one: under the ASan Debug build on an 8 MB stack, translation
 * survives 16,000-deep let chains, and the solver-side recursion that used
 * to be the real weak link (`linearize`, which measured dead below depth
 * 1000) is depth-bounded on its own now.  The two deepest benchmarks in the
 * external 2025 sample nest to 4300 and 6857; both fit with margin. */
#define TR_MAX_LET_DEPTH  6000
#define TR_MAX_LET_BINDS  32768
#define TR_MAX_LET_PARALLEL 4096
#define TR_MAX_TERM_DEPTH 8000
#define TR_MAX_SORTS      256
#define TR_MAX_DEFS       32768
#define TR_MAX_DEF_DEPTH  64

typedef struct { const char *name; VCTerm *val; } LetBind;

typedef struct {
    RefineVC  *vc;
    Arena     *arena;
    const char *err;                 /* non-NULL => skip the whole benchmark */
    LetBind   *lets;                 /* arena-allocated: too big for the stack */
    uint32_t   n_lets, cap_lets;
    uint32_t   n_ite;                /* serial for fresh ite-lifting variables */
    bool       reals_only;           /* pure-Real logic: numerals denote reals */
} Tr;

/* `define-fun` is a MACRO, not a new uninterpreted symbol: SMT-LIB gives it a
 * body, and `define-fun-rec` is the separate (and unsupported) form for
 * recursion, so expansion terminates without a cycle check -- the depth bound
 * below is belt-and-braces.
 *
 * Expansion reuses the `let` binding stack: parameters are pushed as bindings
 * over the ALREADY-TRANSLATED argument terms, the body is translated, and the
 * stack is rewound.  That gets correct scoping for free, including a parameter
 * shadowing an outer name, and it means a macro argument is evaluated once
 * rather than duplicated per occurrence in the body.
 *
 * A NULLARY def is different: it has no parameters to bind, so its body is
 * translated ONCE, at definition time, into `val` -- and every reference is
 * then an O(1) lookup.  This is not an optimization but a feasibility
 * matter: CPAchecker-style benchmarks carry tens of thousands of nullary
 * defs where nearly every body references more than one earlier def
 * (measured: 18,209 defs, longest chain 6,187, naive expansion >= 1e18
 * tree nodes), so expanding per reference is exponential where memoized
 * translation is linear.  SMT-LIB scopes definitions before use, so at
 * definition time every referenced def is already translated; a
 * self-reference (define-fun-rec territory) finds no binding and skips as
 * an undeclared symbol.  The one visible change: a nullary def whose body
 * is outside the fragment now skips the benchmark even if nothing ever
 * references it -- conservative, and consistent with skip-whole-never-guess. */
typedef struct { const char *name; const Sx *params; const Sx *body;
                 VCTerm *val; } FunDef;
static FunDef g_defs[TR_MAX_DEFS];
static uint32_t g_n_defs;

static const FunDef *tr_find_def(const char *name, uint32_t arity) {
    for (uint32_t i = g_n_defs; i-- > 0; )                 /* last wins */
        if (strcmp(g_defs[i].name, name) == 0 &&
            g_defs[i].params->n == arity)
            return &g_defs[i];
    return NULL;
}

static VCTerm *tr_term(Tr *t, const Sx *s, uint32_t depth);

static bool tr_numeral(const char *a, int64_t *out) {
    if (!*a) return false;
    const char *p = a;
    if (*p == '-') p++;              /* not SMT-LIB syntax, but harmless */
    if (!*p) return false;
    for (const char *q = p; *q; q++) if (!isdigit((unsigned char)*q)) return false;
    *out = strtoll(a, NULL, 10);
    return true;
}

static bool tr_decimal(const char *a, double *out) {
    const char *dot = strchr(a, '.');
    if (!dot) return false;
    for (const char *q = a; *q; q++)
        if (!isdigit((unsigned char)*q) && *q != '.' && !(q == a && *q == '-'))
            return false;
    *out = strtod(a, NULL);
    return true;
}

/* `>` and `>=` do not exist in the VC -- the normalizer keeps three relations
 * by swapping operands, so this mirrors that rather than inventing ops. */
static VCTerm *tr_rel(Tr *t, const char *op, VCTerm *a, VCTerm *b) {
    if (strcmp(op, "<")  == 0) return vc_mk2(t->vc, VC_LT, a, b);
    if (strcmp(op, "<=") == 0) return vc_mk2(t->vc, VC_LE, a, b);
    if (strcmp(op, ">")  == 0) return vc_mk2(t->vc, VC_LT, b, a);
    if (strcmp(op, ">=") == 0) return vc_mk2(t->vc, VC_LE, b, a);
    if (strcmp(op, "=")  == 0) return vc_mk2(t->vc, VC_EQ, a, b);
    return NULL;
}

/* Left-associative fold, which is what the n-ary arithmetic operators mean. */
static VCTerm *tr_fold(Tr *t, VCOp op, const Sx *s, uint32_t depth) {
    VCTerm *acc = tr_term(t, s->kids[1], depth + 1);
    if (!acc) return NULL;
    for (uint32_t i = 2; i < s->n; i++) {
        VCTerm *nx = tr_term(t, s->kids[i], depth + 1);
        if (!nx) return NULL;
        acc = vc_mk2(t->vc, op, acc, nx);
    }
    return acc;
}

/* SMT-LIB has TWO divisions and they are not interchangeable: `div` is integer
 * division, `/` is REAL division.  The VC has one `VC_DIV`, whose constant
 * folding is integer -- correct for the compiler, which only ever emits it from
 * Turmeric's `/` on ints and rejects `(* x (/ 1 2))` outright rather than
 * coercing (TUR-E0042).  Translating SMT-LIB `/` to that node folded `(/ 1 2)`
 * to 0, which is how a satisfiable QF_LRA benchmark came back proved
 * contradictory: `pi/2 > skoA > 0` became `0 > skoA > 0`.
 *
 * So `/` coerces integer literal operands to reals first.  Numerals in a Real
 * context denote reals in SMT-LIB; it is only this reader that had been
 * carrying them as integers. */
static VCTerm *tr_as_real(Tr *t, VCTerm *x) {
    if (x && x->op == VC_CONST_INT) return vc_real(t->vc, (double)x->as.i);
    return x;
}

static VCTerm *tr_fold_real_div(Tr *t, const Sx *s, uint32_t depth) {
    VCTerm *acc = tr_as_real(t, tr_term(t, s->kids[1], depth + 1));
    if (!acc) return NULL;
    for (uint32_t i = 2; i < s->n; i++) {
        VCTerm *nx = tr_as_real(t, tr_term(t, s->kids[i], depth + 1));
        if (!nx) return NULL;
        acc = vc_mk2(t->vc, VC_DIV, acc, nx);
    }
    return acc;
}

/* A chained relation -- `(< a b c)` -- is the conjunction of adjacent pairs. */
static VCTerm *tr_chain(Tr *t, const char *op, const Sx *s, uint32_t depth) {
    VCTerm *acc = NULL;
    for (uint32_t i = 1; i + 1 < s->n; i++) {
        VCTerm *a = tr_term(t, s->kids[i], depth + 1);
        VCTerm *b = tr_term(t, s->kids[i + 1], depth + 1);
        if (!a || !b) return NULL;
        VCTerm *r = tr_rel(t, op, a, b);
        if (!r) { t->err = "unsupported relation"; return NULL; }
        acc = acc ? vc_mk2(t->vc, VC_AND, acc, r) : r;
    }
    return acc;
}

static VCTerm *tr_term(Tr *t, const Sx *s, uint32_t depth) {
    if (t->err) return NULL;
    if (depth > TR_MAX_TERM_DEPTH) {
        t->err = "term nested too deeply"; return NULL;
    }

    if (s->kind == SX_ATOM) {
        const char *a = s->atom;
        if (strcmp(a, "true")  == 0) return vc_bool(t->vc, true);
        if (strcmp(a, "false") == 0) return vc_bool(t->vc, false);
        int64_t iv; double dv;
        if (tr_numeral(a, &iv))
            return t->reals_only ? vc_real(t->vc, (double)iv)
                                 : vc_int(t->vc, iv);
        if (tr_decimal(a, &dv)) return vc_real(t->vc, dv);
        /* innermost let binding wins */
        for (uint32_t i = t->n_lets; i-- > 0; )
            if (strcmp(t->lets[i].name, a) == 0) return t->lets[i].val;
        /* A declared symbol.  vc_declare_var finds an existing one by name, so
         * the sort recorded at declare-fun time is what is used here. */
        for (uint32_t i = 0; i < t->vc->n_vars; i++)
            if (strcmp(t->vc->vars[i].name, a) == 0)
                return vc_var_ref(t->vc, i);
        /* A nullary `define-fun` -- a named constant, translated once at
         * definition time; a reference is a lookup, never an expansion. */
        const FunDef *d0 = tr_find_def(a, 0);
        if (d0 && d0->val) return d0->val;
        t->err = "reference to an undeclared symbol";
        return NULL;
    }

    if (s->n == 0) { t->err = "empty application"; return NULL; }
    const Sx *h = s->kids[0];
    if (h->kind != SX_ATOM) { t->err = "non-symbol in head position"; return NULL; }
    const char *op = h->atom;

    if (strcmp(op, "let") == 0) {
        if (s->n != 3 || s->kids[1]->kind != SX_LIST) {
            t->err = "malformed let"; return NULL;
        }
        if (depth > TR_MAX_LET_DEPTH) { t->err = "let nested too deeply"; return NULL; }
        const Sx *binds = s->kids[1];
        uint32_t saved = t->n_lets;
        /* SMT-LIB `let` is PARALLEL: every value is evaluated in the outer
         * scope, so the bindings are pushed only after all are built. */
        if (binds->n > TR_MAX_LET_PARALLEL) {
            t->err = "too many let bindings"; return NULL;
        }
        VCTerm **vals = (VCTerm **)arena_alloc(t->arena,
                                               (binds->n ? binds->n : 1) *
                                               sizeof(VCTerm *));
        for (uint32_t i = 0; i < binds->n; i++) {
            const Sx *b = binds->kids[i];
            if (b->kind != SX_LIST || b->n != 2 || b->kids[0]->kind != SX_ATOM) {
                t->err = "malformed let binding"; return NULL;
            }
            vals[i] = tr_term(t, b->kids[1], depth + 1);
            if (!vals[i]) return NULL;
        }
        for (uint32_t i = 0; i < binds->n; i++) {
            if (t->n_lets >= t->cap_lets) { t->err = "let overflow"; return NULL; }
            t->lets[t->n_lets].name = binds->kids[i]->kids[0]->atom;
            t->lets[t->n_lets].val  = vals[i];
            t->n_lets++;
        }
        VCTerm *body = tr_term(t, s->kids[2], depth + 1);
        t->n_lets = saved;
        return body;
    }

    if (strcmp(op, "not") == 0) {
        if (s->n != 2) { t->err = "not/1 expected"; return NULL; }
        VCTerm *a = tr_term(t, s->kids[1], depth + 1);
        return a ? vc_not(t->vc, a) : NULL;
    }
    if (strcmp(op, "and") == 0 || strcmp(op, "or") == 0) {
        if (s->n < 2) { t->err = "and/or needs arguments"; return NULL; }
        if (s->n == 2) return tr_term(t, s->kids[1], depth + 1);
        return tr_fold(t, strcmp(op, "and") == 0 ? VC_AND : VC_OR, s, depth);
    }
    if (strcmp(op, "=>") == 0) {
        if (s->n < 3) { t->err = "=> needs two arguments"; return NULL; }
        /* right-associative */
        VCTerm *acc = tr_term(t, s->kids[s->n - 1], depth + 1);
        if (!acc) return NULL;
        for (uint32_t i = s->n - 1; i-- > 1; ) {
            VCTerm *a = tr_term(t, s->kids[i], depth + 1);
            if (!a) return NULL;
            acc = vc_mk2(t->vc, VC_IMPLIES, a, acc);
        }
        return acc;
    }
    if (strcmp(op, "xor") == 0) {
        /* (xor a b) == (a or b) and not (a and b).  No VC_XOR needed. */
        if (s->n < 3) { t->err = "xor needs two arguments"; return NULL; }
        VCTerm *acc = tr_term(t, s->kids[1], depth + 1);
        if (!acc) return NULL;
        for (uint32_t i = 2; i < s->n; i++) {
            VCTerm *b = tr_term(t, s->kids[i], depth + 1);
            if (!b) return NULL;
            acc = vc_mk2(t->vc, VC_AND,
                         vc_mk2(t->vc, VC_OR, acc, b),
                         vc_not(t->vc, vc_mk2(t->vc, VC_AND, acc, b)));
        }
        return acc;
    }

    if (strcmp(op, "ite") == 0) {
        /* The VC has no VC_ITE, but it does not need one.
         *
         * A BOOLEAN ite is pure propositional structure:
         *     (ite c p q)  ==  (c and p) or ((not c) and q)
         *
         * An ARITHMETIC ite is lifted to a fresh variable with a definition
         * asserted at the top level:
         *     (ite c a b)  ~>  t,  with  (c => t = a)  and  ((not c) => t = b)
         *
         * That is equisatisfiable, not merely sound-in-one-direction, and the
         * polarity of the occurrence does not matter: the two implications
         * DEFINE `t` uniquely given c, a and b, so every model of the original
         * extends to exactly one model of the rewritten form and every model of
         * the rewritten form restricts back. `t` is fresh per occurrence, which
         * is what the freshness counter is for -- reusing a name across two
         * distinct ite terms would silently equate them.
         *
         * Adding the definitions as hypotheses is safe here because the goal is
         * `false`: the whole benchmark is one conjunction, so a definitional
         * conjunct lands in the same place whether the ite sat under a
         * negation, a disjunction, or nothing at all. */
        if (s->n != 4) { t->err = "ite needs three arguments"; return NULL; }
        VCTerm *c = tr_term(t, s->kids[1], depth + 1);
        VCTerm *a = tr_term(t, s->kids[2], depth + 1);
        VCTerm *b = tr_term(t, s->kids[3], depth + 1);
        if (!c || !a || !b) return NULL;
        if (c->sort != VS_BOOL) { t->err = "ite condition is not boolean"; return NULL; }

        if (a->sort == VS_BOOL && b->sort == VS_BOOL)
            return vc_mk2(t->vc, VC_OR,
                          vc_mk2(t->vc, VC_AND, c, a),
                          vc_mk2(t->vc, VC_AND, vc_not(t->vc, c), b));

        /* Int/Real mixing is legal: a numeral in a Real context denotes a real,
         * so `(ite c 1 2.5)` is well-typed SMT-LIB.  Coerce the literal side,
         * exactly as `/` does -- rejecting the pair was this reader being
         * stricter than the language. */
        if (a->sort != b->sort) {
            if (a->sort == VS_REAL) b = tr_as_real(t, b);
            else if (b->sort == VS_REAL) a = tr_as_real(t, a);
        }
        if (a->sort != b->sort) { t->err = "ite branches disagree on sort"; return NULL; }

        char name[32];
        snprintf(name, sizeof(name), "#ite!%u", t->n_ite++);
        uint32_t idx = vc_declare_var(t->vc, arena_strdup(t->arena, name,
                                                          strlen(name)), a->sort);
        VCTerm *fresh = vc_var_ref(t->vc, idx);
        vc_add_hyp(t->vc, vc_mk2(t->vc, VC_IMPLIES, c,
                                 vc_mk2(t->vc, VC_EQ, fresh, a)));
        vc_add_hyp(t->vc, vc_mk2(t->vc, VC_IMPLIES, vc_not(t->vc, c),
                                 vc_mk2(t->vc, VC_EQ, fresh, b)));
        return fresh;
    }
    if (strcmp(op, "distinct") == 0) {
        if (s->n < 3) { t->err = "distinct needs two arguments"; return NULL; }
        VCTerm *acc = NULL;
        for (uint32_t i = 1; i < s->n; i++)
            for (uint32_t j = i + 1; j < s->n; j++) {
                VCTerm *a = tr_term(t, s->kids[i], depth + 1);
                VCTerm *b = tr_term(t, s->kids[j], depth + 1);
                if (!a || !b) return NULL;
                VCTerm *ne = vc_not(t->vc, vc_mk2(t->vc, VC_EQ, a, b));
                acc = acc ? vc_mk2(t->vc, VC_AND, acc, ne) : ne;
            }
        return acc;
    }
    if (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
        strcmp(op, ">") == 0 || strcmp(op, ">=") == 0 ||
        strcmp(op, "=") == 0) {
        if (s->n < 3) { t->err = "relation needs two arguments"; return NULL; }
        if (s->n == 3) {
            VCTerm *a = tr_term(t, s->kids[1], depth + 1);
            VCTerm *b = tr_term(t, s->kids[2], depth + 1);
            if (!a || !b) return NULL;
            VCTerm *r = tr_rel(t, op, a, b);
            if (!r) { t->err = "unsupported relation"; return NULL; }
            return r;
        }
        return tr_chain(t, op, s, depth);
    }
    if (strcmp(op, "+") == 0) return tr_fold(t, VC_ADD, s, depth);
    if (strcmp(op, "*") == 0) return tr_fold(t, VC_MUL, s, depth);
    if (strcmp(op, "div") == 0) return tr_fold(t, VC_DIV, s, depth);
    if (strcmp(op, "/")   == 0) return tr_fold_real_div(t, s, depth);
    if (strcmp(op, "mod") == 0) return tr_fold(t, VC_MOD, s, depth);
    if (strcmp(op, "-") == 0) {
        if (s->n == 2) {
            VCTerm *a = tr_term(t, s->kids[1], depth + 1);
            return a ? vc_mk1(t->vc, VC_NEG, a) : NULL;
        }
        return tr_fold(t, VC_SUB, s, depth);
    }

    /* An application of a `define-fun` macro.  Checked before the
     * uninterpreted-function path: a name is one or the other, never both. */
    {
        const FunDef *d = tr_find_def(op, s->n - 1);
        if (d) {
            if (depth > TR_MAX_DEF_DEPTH * 20) {
                t->err = "macro expansion too deep"; return NULL;
            }
            uint32_t saved = t->n_lets;
            /* Arguments translate in the CALLER's scope, before any parameter
             * binding is pushed -- otherwise a parameter named like an
             * argument's free variable would capture it. */
            VCTerm **argv = (VCTerm **)arena_alloc(t->arena,
                                (s->n ? s->n : 1) * sizeof(VCTerm *));
            for (uint32_t i = 1; i < s->n; i++) {
                argv[i] = tr_term(t, s->kids[i], depth + 1);
                if (!argv[i]) return NULL;
            }
            for (uint32_t i = 0; i < d->params->n; i++) {
                const Sx *pdecl = d->params->kids[i];
                if (pdecl->kind != SX_LIST || pdecl->n != 2 ||
                    pdecl->kids[0]->kind != SX_ATOM) {
                    t->err = "malformed define-fun parameter"; return NULL;
                }
                if (t->n_lets >= t->cap_lets) {
                    t->err = "macro binding overflow"; return NULL;
                }
                t->lets[t->n_lets].name = pdecl->kids[0]->atom;
                t->lets[t->n_lets].val  = argv[i + 1];
                t->n_lets++;
            }
            VCTerm *body = tr_term(t, d->body, depth + 1);
            t->n_lets = saved;
            return body;
        }
    }

    /* An application of a declared uninterpreted function. */
    for (uint32_t i = 0; i < t->vc->n_ufuncs; i++) {
        if (strcmp(t->vc->ufuncs[i].name, op) != 0) continue;
        if (t->vc->ufuncs[i].arity != s->n - 1) {
            t->err = "uninterpreted application arity mismatch"; return NULL;
        }
        VCTerm *args[16];
        if (s->n - 1 > 16) { t->err = "uninterpreted arity too large"; return NULL; }
        for (uint32_t k = 1; k < s->n; k++) {
            args[k - 1] = tr_term(t, s->kids[k], depth + 1);
            if (!args[k - 1]) return NULL;
        }
        return vc_app(t->vc, i, args, s->n - 1);
    }

    t->err = "unsupported operator";
    return NULL;
}

/* Uninterpreted sorts declared by the benchmark, e.g. `(declare-sort U 0)`.
 * QF_UF is built on them -- every benchmark in the 2025 QF_UF release declares
 * at least one -- so rejecting them meant rejecting the theory the EUF stage
 * most directly targets.
 *
 * They are modelled as VS_INT, which is sound for the quantifier-free
 * equality/UF fragment: a QF formula over an uninterpreted sort is
 * EQUISATISFIABLE with the same formula over an infinite domain. A model over
 * the uninterpreted sort injects into the integers, and a model over the
 * integers restricts to a domain of the right size -- an uninterpreted sort
 * carries no cardinality constraint that could distinguish them.
 *
 * The precondition is that no ARITHMETIC is applied to a sort-typed term. That
 * holds for well-typed input (`(< a b)` on sort-U terms is a type error the
 * benchmark's own logic forbids), and this reader does not type-check, so it is
 * inherited rather than enforced. Only arity-0 sorts are accepted; a parametric
 * sort is skipped rather than guessed at. */
static const char *g_sorts[TR_MAX_SORTS];
static uint32_t g_n_sorts;


static bool tr_sort(const Sx *s, VCSort *out) {
    if (!s || s->kind != SX_ATOM) return false;
    if (strcmp(s->atom, "Int")  == 0) { *out = VS_INT;  return true; }
    if (strcmp(s->atom, "Real") == 0) { *out = VS_REAL; return true; }
    if (strcmp(s->atom, "Bool") == 0) { *out = VS_BOOL; return true; }
    for (uint32_t i = 0; i < g_n_sorts; i++)
        if (strcmp(g_sorts[i], s->atom) == 0) { *out = VS_INT; return true; }
    return false;
}

/* ------------------------------------------------------------------------- *
 * The reader entry point
 * ------------------------------------------------------------------------- */

void refine_smtlib_read(SmtlibQuery *b, const char *text, size_t len, Arena *a) {
    memset(b, 0, sizeof(*b));
    b->status = SMT_STATUS_NONE;

    g_n_sorts = 0;                      /* sorts are per-benchmark */
    g_n_defs  = 0;                      /* and so are macros */
    SxReader r = { text, text + len, a, NULL };
    RefineVC *vc = vc_new(a);
    b->vc = vc;
    Tr t; memset(&t, 0, sizeof(t));
    t.vc = vc;
    t.arena = a;
    t.cap_lets = TR_MAX_LET_BINDS;
    t.lets = (LetBind *)arena_alloc(a, t.cap_lets * sizeof(LetBind));

    for (;;) {
        Sx *cmd = sx_read(&r);
        if (!cmd) {
            if (r.err) { b->skipped = true; b->skip_reason = r.err; }
            break;
        }
        if (cmd->kind != SX_LIST || cmd->n == 0) continue;

        if (sx_head_is(cmd, "set-info")) {
            if (cmd->n >= 3 && sx_is(cmd->kids[1], ":status")) {
                if (sx_is(cmd->kids[2], "sat"))        b->status = SMT_STATUS_SAT;
                else if (sx_is(cmd->kids[2], "unsat")) b->status = SMT_STATUS_UNSAT;
                else                                   b->status = SMT_STATUS_UNKNOWN;
            }
            continue;
        }
        if (sx_head_is(cmd, "set-logic")) {
            /* In a pure-Real logic there are no Int-sorted terms: numerals
             * denote reals (SMT-LIB 2.6, Reals theory declaration).  Typing
             * them by the logic -- rather than carrying them as ints and
             * coercing at the sites that know about it -- is what lets a
             * lifted ite variable come out real-sorted, which no post-hoc
             * literal rewrite (tr_as_real) can fix. */
            if (cmd->n == 2 && cmd->kids[1]->kind == SX_ATOM) {
                const char *lg = cmd->kids[1]->atom;
                t.reals_only = strcmp(lg, "QF_LRA")   == 0 ||
                               strcmp(lg, "QF_RDL")   == 0 ||
                               strcmp(lg, "QF_UFLRA") == 0;
            }
            continue;
        }
        if (sx_head_is(cmd, "set-option") ||
            sx_head_is(cmd, "check-sat") || sx_head_is(cmd, "exit") ||
            sx_head_is(cmd, "get-model") || sx_head_is(cmd, "get-info"))
            continue;

        if (sx_head_is(cmd, "declare-sort")) {
            /* (declare-sort NAME ARITY) -- arity 0 only. */
            if (cmd->n != 3 || cmd->kids[1]->kind != SX_ATOM ||
                !sx_is(cmd->kids[2], "0")) {
                b->skipped = true;
                b->skip_reason = "parametric or malformed declare-sort";
                return;
            }
            if (g_n_sorts >= TR_MAX_SORTS) {
                b->skipped = true; b->skip_reason = "too many sorts"; return;
            }
            g_sorts[g_n_sorts++] = cmd->kids[1]->atom;
            continue;
        }

        if (sx_head_is(cmd, "define-fun")) {
            /* (define-fun NAME ((p SORT)...) RETSORT body) */
            VCSort ret;
            if (cmd->n != 5 || cmd->kids[1]->kind != SX_ATOM ||
                cmd->kids[2]->kind != SX_LIST || !tr_sort(cmd->kids[3], &ret)) {
                b->skipped = true; b->skip_reason = "unsupported define-fun"; return;
            }
            const Sx *ps = cmd->kids[2];
            for (uint32_t i = 0; i < ps->n; i++) {
                VCSort psort;
                if (ps->kids[i]->kind != SX_LIST || ps->kids[i]->n != 2 ||
                    !tr_sort(ps->kids[i]->kids[1], &psort)) {
                    b->skipped = true;
                    b->skip_reason = "unsupported define-fun parameter sort";
                    return;
                }
            }
            if (g_n_defs >= TR_MAX_DEFS) {
                b->skipped = true; b->skip_reason = "too many define-funs"; return;
            }
            g_defs[g_n_defs].name   = cmd->kids[1]->atom;
            g_defs[g_n_defs].params = ps;
            g_defs[g_n_defs].body   = cmd->kids[4];
            g_defs[g_n_defs].val    = NULL;
            /* Nullary: translate the body NOW (see the FunDef comment).  The
             * def is not yet registered, so a self-reference fails as an
             * undeclared symbol rather than recursing. */
            if (ps->n == 0) {
                t.n_lets = 0;
                VCTerm *v = tr_term(&t, cmd->kids[4], 0);
                if (!v || t.err) {
                    b->skipped = true;
                    b->skip_reason = t.err ? t.err : "unparsable define-fun body";
                    return;
                }
                g_defs[g_n_defs].val = v;
            }
            g_n_defs++;
            continue;
        }

        if (sx_head_is(cmd, "push") || sx_head_is(cmd, "pop") ||
            sx_head_is(cmd, "define-sort") || sx_head_is(cmd, "assert-soft") ||
            sx_head_is(cmd, "minimize") || sx_head_is(cmd, "maximize")) {
            b->skipped = true; b->skip_reason = "unsupported command"; return;
        }

        if (sx_head_is(cmd, "declare-const")) {
            VCSort srt;
            if (cmd->n != 3 || cmd->kids[1]->kind != SX_ATOM ||
                !tr_sort(cmd->kids[2], &srt)) {
                b->skipped = true; b->skip_reason = "unsupported declare-const"; return;
            }
            vc_declare_var(vc, cmd->kids[1]->atom, srt);
            continue;
        }

        if (sx_head_is(cmd, "declare-fun")) {
            if (cmd->n != 4 || cmd->kids[1]->kind != SX_ATOM ||
                cmd->kids[2]->kind != SX_LIST) {
                b->skipped = true; b->skip_reason = "unsupported declare-fun"; return;
            }
            VCSort ret;
            if (!tr_sort(cmd->kids[3], &ret)) {
                b->skipped = true; b->skip_reason = "unsupported result sort"; return;
            }
            const Sx *params = cmd->kids[2];
            if (params->n == 0) {
                vc_declare_var(vc, cmd->kids[1]->atom, ret);
            } else {
                for (uint32_t i = 0; i < params->n; i++) {
                    VCSort ps;
                    if (!tr_sort(params->kids[i], &ps)) {
                        b->skipped = true; b->skip_reason = "unsupported param sort"; return;
                    }
                }
                vc_declare_ufunc(vc, cmd->kids[1]->atom, params->n, ret, NULL, false);
            }
            continue;
        }

        if (sx_head_is(cmd, "assert")) {
            if (cmd->n != 2) {
                b->skipped = true; b->skip_reason = "malformed assert"; return;
            }
            t.n_lets = 0;
            VCTerm *phi = tr_term(&t, cmd->kids[1], 0);
            if (!phi || t.err) {
                b->skipped = true;
                b->skip_reason = t.err ? t.err : "unparsable assertion";
                return;
            }
            vc_add_hyp(vc, phi);
            continue;
        }

        b->skipped = true; b->skip_reason = "unknown command"; return;
    }

    /* `hyps |- false` is VALID exactly when the assertion set is UNSAT. */
    vc_set_goal(vc, vc_bool(vc, false));
}
