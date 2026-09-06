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

/* SMT-LIB symbol spelling.  A VC name is whatever the compiler minted --
 * `tickm#0` (a distinct-occurrence measure), `match`, `when`, `_`, `if` (a
 * form name abstracted as an uninterpreted function) -- and `tur smt`'s own
 * reader takes all of them, so the mis-spelling was invisible until a dumped
 * VC was handed to Z3, which read `tickm#0` as a malformed bit-vector literal
 * and `match` / `_` as the reserved words they are.  Quote anything that is
 * not a simple symbol, plus the reserved and theory-builtin names; the reader
 * unquotes `|...|` back to the same name, so a round trip is exact. */
static bool smt_symbol_is_simple(const char *s) {
    if (!s || !*s || isdigit((unsigned char)*s)) return false;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || strchr("~!@$%^&*_-+=<>.?/", c)) continue;
        return false;
    }
    return true;
}
static bool smt_symbol_is_reserved(const char *s) {
    static const char *const reserved[] = {
        "_", "!", "as", "let", "forall", "exists", "match", "par",
        "ite", "and", "or", "not", "xor", "distinct", "true", "false",
        "div", "mod", "abs", "to_real", "to_int", "is_int", "if",
        "select", "store", NULL
    };
    for (size_t i = 0; reserved[i]; i++)
        if (strcmp(s, reserved[i]) == 0) return true;
    return false;
}
/* A reserved word or theory builtin is RENAMED, not merely quoted: SMT-LIB
 * makes `|match|` the same symbol as `match`, and Z3 rejects a declaration of
 * either ("invalid constant declaration" for `|_|`, "invalid pattern binding"
 * for `|match|`).  The name is uninterpreted on both sides, so `match~rw`
 * means exactly what `match` did, in the dump and on the replay. */
static void emit_sym(const char *s, Buf *out) {
    if (smt_symbol_is_reserved(s)) {
        buf_putc(out, '|'); buf_puts(out, s); buf_puts(out, "~rw|");
        return;
    }
    if (smt_symbol_is_simple(s)) { buf_puts(out, s); return; }
    buf_putc(out, '|'); buf_puts(out, s); buf_putc(out, '|');
}

static void emit_nary(const RefineVC *vc, const VCTerm *t, const char *op, Buf *out) {
    buf_printf(out, "(%s", op);
    for (uint32_t i = 0; i < t->n; i++) { buf_putc(out, ' '); emit_term(vc, t->kids[i], out); }
    buf_putc(out, ')');
}

/* The VC's VC_DIV / VC_MOD on ints are C's TRUNCATING `/` and `%` (quotient
 * toward zero, remainder with the dividend's sign) -- that is what the
 * compiler's `/` and `mod` lower to, what vc_mk folds, what the model search
 * evaluates, and what vc_add_divmod_axioms describes.  SMT-LIB's `div` / `mod`
 * are EUCLIDEAN (`0 <= mod < |n|`), so emitting the node as a bare `(div a k)`
 * mis-stated every negative-dividend case to an external solver.  Spell the
 * truncating value in Euclidean terms instead, which for non-negative
 * operands coincide:
 *
 *     tdiv(a, b) = sgn(a) * sgn(b) * (|a| div |b|)
 *     tmod(a, b) = sgn(a) * (|a| mod |b|)     -- Euclidean mod ignores sgn(b)
 *
 * A literal divisor (the only kind the compiler emits; the reader refuses
 * any other) folds the sgn(b) branch away. */
static void emit_term(const RefineVC *vc, const VCTerm *t, Buf *out);

static void emit_neg_of(const RefineVC *vc, const VCTerm *t, Buf *out) {
    buf_puts(out, "(- "); emit_term(vc, t, out); buf_putc(out, ')');
}

static void emit_trunc_divmod(const RefineVC *vc, const VCTerm *t, Buf *out) {
    const VCTerm *a = t->kids[0], *b = t->kids[1];
    const char *op = t->op == VC_DIV ? "div" : "mod";
    bool lit = b->op == VC_CONST_INT && b->as.i != INT64_MIN;
    int64_t K = lit ? (b->as.i < 0 ? -b->as.i : b->as.i) : 0;
    /* (ite (>= a 0) <a nonneg> <a negative>) */
    buf_puts(out, "(ite (>= "); emit_term(vc, a, out); buf_puts(out, " 0) ");
    if (t->op == VC_MOD) {
        /* a >= 0: (mod a |b|);  a < 0: (- (mod (- a) |b|)) */
        buf_printf(out, "(%s ", op); emit_term(vc, a, out); buf_putc(out, ' ');
        if (lit) buf_printf(out, "%lld", (long long)K); else emit_term(vc, b, out);
        buf_puts(out, ") (- (");
        buf_printf(out, "%s ", op); emit_neg_of(vc, a, out); buf_putc(out, ' ');
        if (lit) buf_printf(out, "%lld", (long long)K); else emit_term(vc, b, out);
        buf_puts(out, ")))");
        return;
    }
    if (lit) {
        /* b > 0:  a >= 0: (div a K);      a < 0: (- (div (- a) K))
         * b < 0:  a >= 0: (- (div a K));  a < 0: (div (- a) K) */
        bool bneg = b->as.i < 0;
        if (bneg) buf_puts(out, "(- ");
        buf_puts(out, "(div "); emit_term(vc, a, out); buf_printf(out, " %lld)", (long long)K);
        if (bneg) buf_putc(out, ')');
        buf_putc(out, ' ');
        if (!bneg) buf_puts(out, "(- ");
        buf_puts(out, "(div "); emit_neg_of(vc, a, out); buf_printf(out, " %lld)", (long long)K);
        if (!bneg) buf_putc(out, ')');
        buf_putc(out, ')');
        return;
    }
    /* General divisor: branch on its sign too. */
    buf_puts(out, "(ite (>= "); emit_term(vc, b, out); buf_puts(out, " 0) (div ");
    emit_term(vc, a, out); buf_putc(out, ' '); emit_term(vc, b, out);
    buf_puts(out, ") (- (div "); emit_term(vc, a, out); buf_putc(out, ' ');
    emit_neg_of(vc, b, out); buf_puts(out, "))) ");
    buf_puts(out, "(ite (>= "); emit_term(vc, b, out); buf_puts(out, " 0) (- (div ");
    emit_neg_of(vc, a, out); buf_putc(out, ' '); emit_term(vc, b, out);
    buf_puts(out, ")) (div "); emit_neg_of(vc, a, out); buf_putc(out, ' ');
    emit_neg_of(vc, b, out); buf_puts(out, ")))");
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
            if (t->as.idx < vc->n_vars) emit_sym(vc->vars[t->as.idx].name, out);
            else buf_puts(out, "|?var|");
            return;
        case VC_APP: {
            const VCUFunc *u = t->as.idx < vc->n_ufuncs ? &vc->ufuncs[t->as.idx] : NULL;
            buf_putc(out, '(');
            if (u) emit_sym(u->name, out); else buf_puts(out, "|?fn|");
            for (uint32_t i = 0; i < t->n; i++) { buf_putc(out, ' '); emit_term(vc, t->kids[i], out); }
            buf_putc(out, ')');
            return;
        }
        case VC_ADD: emit_nary(vc, t, "+", out); return;
        case VC_SUB: emit_nary(vc, t, "-", out); return;
        case VC_MUL: emit_nary(vc, t, "*", out); return;
        case VC_DIV:
            /* Real division is exact and needs no adjustment; the sort of
             * the TERM decides, not the VC's has_real flag -- an int-sorted
             * division inside a mixed VC used to be emitted as `/`, which is
             * a different function again. */
            if (t->sort == VS_REAL) { emit_nary(vc, t, "/", out); return; }
            emit_trunc_divmod(vc, t, out); return;
        case VC_MOD: emit_trunc_divmod(vc, t, out); return;
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

/* The first application of uninterpreted function `idx` reachable from the
 * hypotheses or the goal, or NULL.  Terms are hash-consed and small; a plain
 * walk is cheaper than bookkeeping a per-ufunc back-pointer nobody else
 * needs. */
static const VCTerm *find_app_in(const VCTerm *t, uint32_t idx) {
    if (!t) return NULL;
    if (t->op == VC_APP && t->as.idx == idx) return t;
    for (uint32_t i = 0; i < t->n; i++) {
        const VCTerm *r = find_app_in(t->kids[i], idx);
        if (r) return r;
    }
    return NULL;
}
static const VCTerm *find_app(const RefineVC *vc, uint32_t idx) {
    for (uint32_t i = 0; i < vc->n_hyps; i++) {
        const VCTerm *r = find_app_in(vc->hyps[i], idx);
        if (r) return r;
    }
    return find_app_in(vc->goal, idx);
}

void refine_smtlib_emit(const RefineVC *vc, Buf *out) {
    if (!vc) return;
    /* A VC with reals AND an int-sorted variable or measure result is mixed:
     * `QF_UFLRA` makes an external solver refuse the Int declarations
     * ("logic does not support integers"), so name the mixed logic.  `tur
     * smt`'s reader keys only on the pure-Real logics for numeral typing, so
     * it replays either. */
    bool any_int = false;
    for (uint32_t i = 0; i < vc->n_vars && !any_int; i++)
        if (vc->vars[i].sort == VS_INT) any_int = true;
    for (uint32_t i = 0; i < vc->n_ufuncs && !any_int; i++)
        if (vc->ufuncs[i].sort == VS_INT) any_int = true;
    buf_printf(out, "(set-logic %s)\n",
               vc->has_real ? (any_int ? "QF_UFLIRA" : "QF_UFLRA") : "QF_UFLIA");

    for (uint32_t i = 0; i < vc->n_vars; i++) {
        buf_puts(out, "(declare-const ");
        emit_sym(vc->vars[i].name, out);
        buf_printf(out, " %s)\n", sort_name(vc->vars[i].sort));
    }

    for (uint32_t i = 0; i < vc->n_ufuncs; i++) {
        const VCUFunc *u = &vc->ufuncs[i];
        buf_puts(out, "(declare-fun ");
        emit_sym(u->name, out);
        buf_puts(out, " (");
        /* Parameter sorts come from a real APPLICATION, not from the VC's
         * has_real flag: a `VCUFunc` records arity and result sort only, and
         * an abstracted form (`if`, `match` over an out-of-fragment scrutinee)
         * takes a Bool among its Int or Real arguments.  Declared uniformly,
         * the dump was rejected by Z3 with "unknown constant" for the very
         * function it declared one line up.  Fall back to the old uniform
         * spelling only when no application of the symbol exists. */
        const VCTerm *app = find_app(vc, i);
        for (uint32_t j = 0; j < u->arity; j++) {
            const char *sn = (app && j < app->n && app->kids[j])
                ? sort_name(app->kids[j]->sort)
                : (vc->has_real ? "Real" : "Int");
            buf_printf(out, "%s%s", j ? " " : "", sn);
        }
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

/* One lifted arithmetic ite: the (c, a, b) it stands for, the fresh variable
 * it became, and its definitional hypothesis (see tr_lift_ite; `d2` is a
 * spare slot, NULL today). */
typedef struct { VCTerm *c, *a, *b, *v, *d1, *d2; } IteMemo;

typedef struct {
    RefineVC  *vc;
    Arena     *arena;
    const char *err;                 /* non-NULL => skip the whole benchmark */
    LetBind   *lets;                 /* arena-allocated: too big for the stack */
    uint32_t   n_lets, cap_lets;
    uint32_t   n_ite;                /* serial for fresh ite-lifting variables */
    IteMemo   *ite_memo;             /* every lift so far, by term identity */
    uint32_t   n_ite_memo, cap_ite_memo;
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

/* An ARITHMETIC `(ite c a b)` lifted to a fresh variable with a definition
 * asserted at the top level:
 *     (ite c a b)  ~>  t,  with  (c => t = a)  and  ((not c) => t = b)
 *
 * That is equisatisfiable, not merely sound-in-one-direction, and the
 * polarity of the occurrence does not matter: the two implications DEFINE `t`
 * uniquely given c, a and b, so every model of the original extends to exactly
 * one model of the rewritten form and every model of the rewritten form
 * restricts back.  `t` is fresh per occurrence, which is what the freshness
 * counter is for -- reusing a name across two distinct ite terms would
 * silently equate them.
 *
 * Adding the definitions as hypotheses is safe here because the goal is
 * `false`: the whole benchmark is one conjunction, so a definitional conjunct
 * lands in the same place whether the ite sat under a negation, a
 * disjunction, or nothing at all.  `a` and `b` must already agree on sort.
 *
 * IDENTICAL ites share one variable.  The freshness rule above is about two
 * DIFFERENT ite terms; two occurrences of the same (c, a, b) -- the same
 * hash-consed kids -- denote the same value, and giving each its own variable
 * plus its own pair of implications is not merely wasteful, it is what made a
 * VC replayed from `--dump-refine=json` undecidable: the serializer spells a
 * truncating `mod` as an ite and every axiom mentions it, so six textual
 * occurrences minted six variables and twelve disjunctive hypotheses, past
 * the cube cap.  The memo is keyed on term identity, which the hash-consing
 * makes exact.
 *
 * A memo hit re-adds the definition.  vc_add_hyp dedups by pointer, so
 * while they are present that is a no-op; after a session `pop` truncated
 * them away it puts them back in the CURRENT scope, which is what keeps the
 * shared variable defined wherever it is used -- a reused variable with its
 * definition popped would be unconstrained, and an unconstrained variable is
 * how a contradictory set grows a model. */
static VCTerm *tr_lift_ite(Tr *t, VCTerm *c, VCTerm *a, VCTerm *b) {
    for (uint32_t i = 0; i < t->n_ite_memo; i++) {
        const IteMemo *m = &t->ite_memo[i];
        if (m->c == c && m->a == a && m->b == b) {
            vc_add_hyp(t->vc, m->d1);
            if (m->d2) vc_add_hyp(t->vc, m->d2);
            return m->v;
        }
    }
    char name[32];
    snprintf(name, sizeof(name), "#ite!%u", t->n_ite++);
    uint32_t idx = vc_declare_var(t->vc, arena_strdup(t->arena, name,
                                                      strlen(name)), a->sort);
    VCTerm *fresh = vc_var_ref(t->vc, idx);
    /* One disjunction, not two implications.  `(c => t = a) and (not c =>
     * t = b)` is the same proposition as `(c and t = a) or (not c and t =
     * b)`, but the naive DNF the stages expand sees FOUR cube combinations
     * in the first (two of them contradictory, c with not c) and TWO in the
     * second.  With a div/mod benchmark lifting several ites, the difference
     * is a proof inside the cube cap versus an overflow to unknown:
     * `qf_lia_div_mod_identity_unsat` sat at exactly 64 of 64 cubes on the
     * implication form. */
    VCTerm *d1 = vc_mk2(t->vc, VC_OR,
                        vc_mk2(t->vc, VC_AND, c, vc_mk2(t->vc, VC_EQ, fresh, a)),
                        vc_mk2(t->vc, VC_AND, vc_not(t->vc, c),
                               vc_mk2(t->vc, VC_EQ, fresh, b)));
    VCTerm *d2 = NULL;
    vc_add_hyp(t->vc, d1);
    if (t->n_ite_memo == t->cap_ite_memo) {
        uint32_t nc = t->cap_ite_memo ? t->cap_ite_memo * 2 : 16;
        IteMemo *nm = (IteMemo *)arena_alloc(t->arena, nc * sizeof(IteMemo));
        if (t->n_ite_memo) memcpy(nm, t->ite_memo, t->n_ite_memo * sizeof(IteMemo));
        t->ite_memo = nm; t->cap_ite_memo = nc;
    }
    t->ite_memo[t->n_ite_memo++] = (IteMemo){ c, a, b, fresh, d1, d2 };
    return fresh;
}

/* The serializer's own spelling of the VC's TRUNCATING pair, read back as the
 * primitive it came from.  refine_smtlib_emit writes `VC_DIV(a, k)` /
 * `VC_MOD(a, k)` -- C's `/` and `%` -- in Euclidean terms:
 *
 *     k > 0:  (ite (>= a 0) (div a k) (- (div (- a) k)))       -> VC_DIV(a, k)
 *     k < 0:  (ite (>= a 0) (- (div a |k|)) (div (- a) |k|))   -> VC_DIV(a, k)
 *     any k:  (ite (>= a 0) (mod a |k|) (- (mod (- a) |k|)))   -> VC_MOD(a, |k|)
 *
 * Each of those IS the truncating value (the Euclidean and truncating
 * operations agree on non-negative operands, and the sign is put back
 * outside), so translating it through tr_euclid_divmod would be exact too --
 * three lifted ites and two axiom sets per occurrence, where the compiler's
 * VC had one node.  A VC replayed from `--dump-refine=json` then proved
 * nothing its author proved.  Recognizing the idiom restores the node and
 * its axioms, so a dumped VC reads back as the VC that was dumped.
 *
 * Purely a peephole: the shape is matched on the S-expression, `a` is
 * translated once and the other two occurrences must translate to the SAME
 * interned term (hash-consing makes structural equality pointer equality),
 * and anything that does not match falls through to the general `ite`.
 * `(mod a k)` with k < 0 equals `(mod a |k|)` under truncation, which is why
 * the mod form carries |k| and maps to the positive divisor. */
static const Sx *sx_unary_minus_arg(const Sx *s) {
    return (s && s->kind == SX_LIST && s->n == 2 && sx_head_is(s, "-")) ? s->kids[1] : NULL;
}
static bool sx_divmod_app(const Sx *s, const char **op, const Sx **arg, const char **k) {
    if (!s || s->kind != SX_LIST || s->n != 3 || s->kids[0]->kind != SX_ATOM) return false;
    if (strcmp(s->kids[0]->atom, "div") != 0 && strcmp(s->kids[0]->atom, "mod") != 0) return false;
    if (s->kids[2]->kind != SX_ATOM) return false;
    *op = s->kids[0]->atom; *arg = s->kids[1]; *k = s->kids[2]->atom;
    return true;
}
static VCTerm *tr_trunc_idiom(Tr *t, const Sx *s, uint32_t depth) {
    const Sx *c = s->kids[1], *p = s->kids[2], *q = s->kids[3];
    if (!c || c->kind != SX_LIST || c->n != 3 || !sx_head_is(c, ">=") ||
        !sx_is(c->kids[2], "0"))
        return NULL;
    const Sx *A = c->kids[1];
    const char *op1, *op2, *k1, *k2;
    const Sx *a1, *a2, *inner;
    bool neg_div = false;
    if (sx_divmod_app(p, &op1, &a1, &k1)) {
        /* (ite (>= a 0) (OP a k) (- (OP (- a) k))) */
        inner = sx_unary_minus_arg(q);
        if (!inner || !sx_divmod_app(inner, &op2, &a2, &k2)) return NULL;
    } else {
        /* (ite (>= a 0) (- (div a k)) (div (- a) k)) */
        inner = sx_unary_minus_arg(p);
        if (!inner || !sx_divmod_app(inner, &op1, &a1, &k1) ||
            strcmp(op1, "div") != 0 || !sx_divmod_app(q, &op2, &a2, &k2))
            return NULL;
        neg_div = true;
    }
    if (strcmp(op1, op2) != 0 || strcmp(k1, k2) != 0) return NULL;
    const Sx *a2in = sx_unary_minus_arg(a2);
    if (!a2in) return NULL;
    int64_t K;
    if (!tr_numeral(k1, &K) || K <= 0) return NULL;
    VCTerm *tA = tr_term(t, A, depth + 1);
    if (!tA || t->err) return NULL;
    if (tA->sort != VS_INT) return NULL;
    VCTerm *t1 = tr_term(t, a1, depth + 1);
    if (!t1 || t->err) return NULL;
    VCTerm *t2 = tr_term(t, a2in, depth + 1);
    if (!t2 || t->err) return NULL;
    if (t1 != tA || t2 != tA) return NULL;
    VCTerm *k = vc_int(t->vc, neg_div ? -K : K);
    VCTerm *r = vc_mk2(t->vc, strcmp(op1, "div") == 0 ? VC_DIV : VC_MOD, tA, k);
    vc_add_divmod_axioms(t->vc, tA, k);
    return r;
}

/* SMT-LIB `div` / `mod` are EUCLIDEAN:  a = k*q + r  with  0 <= r < |k|.
 * The VC's VC_DIV / VC_MOD are C's TRUNCATING pair (quotient toward zero,
 * remainder with the dividend's sign) -- what vc_mk folds, what the model
 * search evaluates, and what vc_add_divmod_axioms describes.  Translating
 * `div` to VC_DIV read every negative dividend wrong: `(< (mod x 3) 0)` is
 * unsatisfiable in SMT-LIB and this reader answered `sat` with x = -2.
 *
 * The two agree for a non-negative dividend and differ by exactly one step
 * otherwise, so the Euclidean value is BUILT from the truncating pair:
 *
 *     r_t = VC_MOD(a, k),  q_t = VC_DIV(a, k)
 *     mod_E = ite(r_t < 0,  r_t + |k|,     r_t)
 *     div_E = ite(r_t < 0,  q_t - sgn(k),  q_t)
 *
 * with the truncating axioms asserted for (a, k) so the stages can reason
 * about r_t and q_t, and the ite lifted through tr_lift_ite.  A literal
 * dividend folds outright.
 *
 * A NON-literal divisor is refused whole ("outside the accepted fragment"):
 * the VC would carry a nonlinear VC_DIV the stages treat as opaque while the
 * model search evaluated it with C semantics -- the same mismatch, on a term
 * no axiom describes.  Division by zero is refused too: SMT-LIB leaves it
 * uninterpreted, so no answer this reader could give is one the script
 * asked for. */
static VCTerm *tr_euclid_divmod(Tr *t, VCOp op, VCTerm *a, VCTerm *k) {
    RefineVC *vc = t->vc;
    if (a->sort != VS_INT || k->sort != VS_INT) {
        t->err = "div/mod on a non-integer operand"; return NULL;
    }
    if (k->op != VC_CONST_INT) {
        t->err = "div/mod by a non-literal divisor is outside the accepted fragment";
        return NULL;
    }
    int64_t kv = k->as.i;
    if (kv == 0) { t->err = "div/mod by zero is outside the accepted fragment"; return NULL; }
    if (kv == INT64_MIN) { t->err = "div/mod by INT64_MIN"; return NULL; }
    int64_t K = kv < 0 ? -kv : kv;
    if (a->op == VC_CONST_INT) {
        int64_t av = a->as.i;
        if (av == INT64_MIN && kv == -1) { t->err = "div/mod overflow"; return NULL; }
        int64_t q = av / kv, r = av % kv;
        if (r < 0) { r += K; q -= (kv > 0 ? 1 : -1); }
        return vc_int(vc, op == VC_DIV ? q : r);
    }
    VCTerm *qt = vc_mk2(vc, VC_DIV, a, k);
    VCTerm *rt = vc_mk2(vc, VC_MOD, a, k);
    vc_add_divmod_axioms(vc, a, k);
    VCTerm *neg = vc_mk2(vc, VC_LT, rt, vc_int(vc, 0));
    if (op == VC_MOD)
        return tr_lift_ite(t, neg, vc_mk2(vc, VC_ADD, rt, vc_int(vc, K)), rt);
    return tr_lift_ite(t, neg, vc_mk2(vc, VC_SUB, qt, vc_int(vc, kv > 0 ? 1 : -1)), qt);
}

/* Left-associative fold of `div` / `mod` through tr_euclid_divmod. */
static VCTerm *tr_fold_divmod(Tr *t, VCOp op, const Sx *s, uint32_t depth) {
    if (s->n < 3) { t->err = "div/mod needs two arguments"; return NULL; }
    VCTerm *acc = tr_term(t, s->kids[1], depth + 1);
    if (!acc) return NULL;
    for (uint32_t i = 2; i < s->n; i++) {
        VCTerm *nx = tr_term(t, s->kids[i], depth + 1);
        if (!nx) return NULL;
        acc = tr_euclid_divmod(t, op, acc, nx);
        if (!acc) return NULL;
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
         * asserted at the top level -- tr_lift_ite. */
        if (s->n != 4) { t->err = "ite needs three arguments"; return NULL; }
        /* The serializer's truncating div/mod idiom reads back as the node
         * it was written from (tr_trunc_idiom); anything else lifts. */
        {
            VCTerm *idiom = tr_trunc_idiom(t, s, depth);
            if (idiom || t->err) return idiom;
        }
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
        return tr_lift_ite(t, c, a, b);
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
    if (strcmp(op, "div") == 0) return tr_fold_divmod(t, VC_DIV, s, depth);
    if (strcmp(op, "/")   == 0) return tr_fold_real_div(t, s, depth);
    if (strcmp(op, "mod") == 0) return tr_fold_divmod(t, VC_MOD, s, depth);
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
 * The command dispatcher, and the two doors onto it
 *
 * One command executor serves both the BATCH reader (`refine_smtlib_read`,
 * the corpus/`tur smt` door since SX8a) and the SESSION reader (SX8b, the
 * assertion-stack door).  They differ in exactly one bit -- `stack_ok` --
 * which decides whether `push`/`pop`/`check-sat` are protocol commands or
 * outside the fragment.  Batch mode says outside, and must: it folds a whole
 * script into ONE assertion set, and a stack has no meaning there.  Keeping
 * one executor is what makes the two doors agree about the fragment by
 * construction rather than by parallel maintenance.
 * ------------------------------------------------------------------------- */

#define SMT_MAX_STACK 256

struct SmtlibSession {
    Arena       *arena;
    RefineVC    *vc;
    Tr           t;
    SxReader     r;
    SmtlibQuery  q;            /* status + the skip flag the executor sets */
    /* The assertion stack: `n_hyps` as it stood at each open `push`.  A `pop`
     * truncates back to it, which is the whole undo -- hypotheses are a plain
     * array of hash-consed, arena-allocated terms, so dropping the tail
     * releases exactly the assertions in scope and nothing dangles. */
    uint32_t     stack[SMT_MAX_STACK];
    uint32_t     depth;
};

/* Execute ONE top-level command.  Errors are reported the way the batch reader
 * has always reported them -- `b->skipped` plus a reason -- so every
 * out-of-fragment path below is unchanged from when this was a loop body.
 * `*ev` is only ever raised above SMT_EV_OK in session mode. */
static void smt_exec_cmd(SmtlibSession *s, SmtlibQuery *b, const Sx *cmd,
                         bool stack_ok, SmtlibEvent *ev) {
    Tr t = s->t;                       /* by value: `n_lets` is per-command */
    RefineVC *vc = s->vc;
    *ev = SMT_EV_OK;
    /* do/while(0) so the `continue`s below still read as "this command is
     * finished" -- they are the original loop body's, unedited. */
    do {
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
        /* In BATCH mode these are all inert: the whole script is one assertion
         * set decided once by the caller, so a `check-sat` in the middle of it
         * has nothing distinct to answer.  In SESSION mode each becomes an
         * event the driver acts on at the point it appears. */
        if (sx_head_is(cmd, "check-sat")) {
            if (stack_ok) *ev = SMT_EV_CHECK_SAT;
            continue;
        }
        if (sx_head_is(cmd, "get-model")) {
            if (stack_ok) *ev = SMT_EV_GET_MODEL;
            continue;
        }
        if (sx_head_is(cmd, "exit")) {
            if (stack_ok) *ev = SMT_EV_EXIT;
            continue;
        }
        if (sx_head_is(cmd, "set-option") || sx_head_is(cmd, "get-info"))
            continue;

        /* (push [n]) / (pop [n]) -- session mode only.  `n` defaults to 1.
         *
         * Only the HYPOTHESES are scoped.  Declarations made inside a scope
         * survive the `pop`, which is a deliberate divergence from SMT-LIB and
         * a sound one in this direction: a declared symbol that appears in no
         * assertion is an unconstrained variable, and an unconstrained variable
         * cannot make an assertion set less satisfiable -- so it can never turn
         * a `sat` into an `unsat`, which is the only error that would matter.
         * The reason not to scope them is concrete rather than lazy: hypotheses
         * are hash-consed terms holding var INDICES, and truncating `n_vars`
         * would leave interned terms pointing at slots a later declaration
         * could reuse with a different sort. */
        if (sx_head_is(cmd, "push") || sx_head_is(cmd, "pop")) {
            if (!stack_ok) {
                b->skipped = true; b->skip_reason = "unsupported command"; return;
            }
            uint32_t n = 1;
            if (cmd->n == 2) {
                int64_t v;
                if (cmd->kids[1]->kind != SX_ATOM ||
                    !tr_numeral(cmd->kids[1]->atom, &v) || v < 0) {
                    b->skipped = true; b->skip_reason = "malformed push/pop level"; return;
                }
                n = (uint32_t)v;
            } else if (cmd->n != 1) {
                b->skipped = true; b->skip_reason = "malformed push/pop"; return;
            }
            if (sx_head_is(cmd, "push")) {
                if (s->depth + n > SMT_MAX_STACK) {
                    b->skipped = true; b->skip_reason = "push nested too deeply"; return;
                }
                for (uint32_t i = 0; i < n; i++) s->stack[s->depth++] = vc->n_hyps;
            } else {
                if (n > s->depth) {
                    b->skipped = true; b->skip_reason = "pop with no matching push"; return;
                }
                /* Truncating to the OUTERMOST of the popped scopes; popping n
                 * levels at once must land where n separate pops would. */
                for (uint32_t i = 0; i < n; i++) vc->n_hyps = s->stack[--s->depth];
            }
            continue;
        }

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

        if (sx_head_is(cmd, "define-sort") || sx_head_is(cmd, "assert-soft") ||
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
    } while (0);

    /* Write the translator back: `reals_only` (set by set-logic) and `n_ite`
     * (the serial minting fresh ite-lifting variables) both have to persist
     * across commands -- restarting the serial would let two commands mint the
     * same fresh symbol and silently conflate two different lifted terms. */
    s->t = t;
}

/* ------------------------------------------------------------------------- *
 * Door 1: the batch reader (SX8a)
 * ------------------------------------------------------------------------- */

/* Shared construction.  The goal is NOT set here: batch mode sets it once at
 * the end, exactly as it always has, and a session sets it up front because a
 * session must be decidable at every `check-sat`. */
static void smt_session_init(SmtlibSession *s, Arena *a) {
    memset(s, 0, sizeof(*s));
    s->arena = a;
    s->vc    = vc_new(a);
    s->q.status = SMT_STATUS_NONE;
    s->t.vc    = s->vc;
    s->t.arena = a;
    s->t.cap_lets = TR_MAX_LET_BINDS;
    s->t.lets = (LetBind *)arena_alloc(a, s->t.cap_lets * sizeof(LetBind));
    g_n_sorts = 0;                      /* sorts are per-script */
    g_n_defs  = 0;                      /* and so are macros */
}

void refine_smtlib_read(SmtlibQuery *b, const char *text, size_t len, Arena *a) {
    memset(b, 0, sizeof(*b));
    b->status = SMT_STATUS_NONE;

    SmtlibSession s;
    smt_session_init(&s, a);
    b->vc = s.vc;
    s.r = (SxReader){ text, text + len, a, NULL };

    for (;;) {
        Sx *cmd = sx_read(&s.r);
        if (!cmd) {
            if (s.r.err) { b->skipped = true; b->skip_reason = s.r.err; }
            break;
        }
        if (cmd->kind != SX_LIST || cmd->n == 0) continue;
        SmtlibEvent ev;
        smt_exec_cmd(&s, b, cmd, /*stack_ok=*/false, &ev);
        if (b->skipped) return;         /* skip whole, never partially parsed */
    }

    /* `hyps |- false` is VALID exactly when the assertion set is UNSAT. */
    vc_set_goal(s.vc, vc_bool(s.vc, false));
}

/* ------------------------------------------------------------------------- *
 * Door 2: sessions (SX8b)
 * ------------------------------------------------------------------------- */

SmtlibSession *refine_smtlib_session_new(Arena *a) {
    SmtlibSession *s = (SmtlibSession *)arena_alloc(a, sizeof(*s));
    smt_session_init(s, a);
    /* Fixed for the session's life, so every `check-sat` decides
     * `hyps |- false` over whatever is in scope at that moment. */
    vc_set_goal(s->vc, vc_bool(s->vc, false));
    return s;
}

void refine_smtlib_session_feed(SmtlibSession *s, const char *text, size_t len) {
    s->r = (SxReader){ text, text + len, s->arena, NULL };
}

SmtlibEvent refine_smtlib_session_step(SmtlibSession *s) {
    for (;;) {
        Sx *cmd = sx_read(&s->r);
        if (!cmd) {
            if (s->r.err) { s->q.skipped = true; s->q.skip_reason = s->r.err;
                            return SMT_EV_ERROR; }
            return SMT_EV_END;
        }
        if (cmd->kind != SX_LIST || cmd->n == 0) continue;
        SmtlibEvent ev;
        smt_exec_cmd(s, &s->q, cmd, /*stack_ok=*/true, &ev);
        if (s->q.skipped) return SMT_EV_ERROR;
        if (ev != SMT_EV_OK) return ev;
        /* An ordinary command (declare, assert, push, pop): keep reading until
         * something the driver has to act on, or the text runs out. */
    }
}

RefineVC *refine_smtlib_session_vc(SmtlibSession *s) { return s->vc; }

const char *refine_smtlib_session_err(const SmtlibSession *s) {
    return s->q.skip_reason;
}

SmtlibStatus refine_smtlib_session_status(const SmtlibSession *s) {
    return s->q.status;
}

uint32_t refine_smtlib_session_depth(const SmtlibSession *s) { return s->depth; }
