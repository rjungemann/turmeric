/* refine_corpus.c -- replay a labelled SMT-LIB2 corpus against the in-house
 * staged decision procedure, WITHOUT Z3.
 *
 * This is the durable half of the Z3 retirement criteria
 * (docs/upcoming/v1/refinement-types-plan.md, "Z3 retirement criteria").  The
 * scaffold oracle is a live cross-check that only exists on a dev build with a
 * system Z3 linked; what survives its deletion is a corpus whose labels are
 * DATA, checked into the repo and replayed by a harness that has no solver
 * dependency of its own.
 *
 * ## How a satisfiability benchmark becomes an entailment query
 *
 * SMT-LIB `:status` is a claim about the SATISFIABILITY of the assertion set.
 * The in-house chain decides ENTAILMENT -- `hyps |- goal`, internally "is
 * `hyps AND NOT goal` unsatisfiable".  The two line up exactly by taking every
 * `(assert phi)` as a hypothesis and `false` as the goal:
 *
 *     hyps |- false   is VALID   iff   hyps is UNSAT
 *
 * which gives the check its whole shape:
 *
 *   | :status | RT_VALID              | anything else |
 *   |---------|-----------------------|---------------|
 *   | unsat   | correct (a proof)     | acceptable    |
 *   | sat     | SOUNDNESS FAILURE     | correct       |
 *
 * A `sat` benchmark answered VALID is the one-directional invariant broken:
 * the chain claimed a contradiction in a set of constraints that has a model.
 * That is the property this harness exists to defend, and it needs no oracle
 * to check -- only the label.
 *
 * `unknown` labels are recorded and never fail: they carry no claim.
 *
 * ## Why the reader skips rather than guesses
 *
 * A benchmark using anything outside the fragment is SKIPPED WHOLE, never
 * partially parsed.  Silently dropping an assertion would weaken the
 * hypotheses, which cannot make the chain prove something it should not -- but
 * it would quietly turn a real benchmark into a trivial one and report a pass
 * for work not done.  Every skip is counted and its reason printed, so a corpus
 * that stops testing anything is visible rather than green.
 *
 * See tests/corpus/smtlib/README.md for the corpus itself. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>

#include "lsp/lsp_sym.h"
#include "compiler/refine_solver.h"
#include "compiler/refine_vc.h"
#include "runtime/arena.h"

/* Stub: tur_core's lsp.c references this; this harness never touches LSP, but
 * the symbol must resolve when tur_core is linked into a standalone
 * executable.  Matches the stub in tests/unit/refine_solver.c. */
int tur_collect_symbols(const char *source_path, LspSymbol *out, int cap,
                        int *count_out) {
    (void)source_path; (void)out; (void)cap;
    if (count_out) *count_out = 0;
    return 0;
}

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

#define TR_MAX_LET_DEPTH 32
#define TR_MAX_LET_BINDS 256

typedef struct { const char *name; VCTerm *val; } LetBind;

typedef struct {
    RefineVC  *vc;
    const char *err;                 /* non-NULL => skip the whole benchmark */
    LetBind    lets[TR_MAX_LET_BINDS];
    uint32_t   n_lets;
} Tr;

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
    if (depth > 400) { t->err = "term nested too deeply"; return NULL; }

    if (s->kind == SX_ATOM) {
        const char *a = s->atom;
        if (strcmp(a, "true")  == 0) return vc_bool(t->vc, true);
        if (strcmp(a, "false") == 0) return vc_bool(t->vc, false);
        int64_t iv; double dv;
        if (tr_numeral(a, &iv)) return vc_int(t->vc, iv);
        if (tr_decimal(a, &dv)) return vc_real(t->vc, dv);
        /* innermost let binding wins */
        for (uint32_t i = t->n_lets; i-- > 0; )
            if (strcmp(t->lets[i].name, a) == 0) return t->lets[i].val;
        /* A declared symbol.  vc_declare_var finds an existing one by name, so
         * the sort recorded at declare-fun time is what is used here. */
        for (uint32_t i = 0; i < t->vc->n_vars; i++)
            if (strcmp(t->vc->vars[i].name, a) == 0)
                return vc_var_ref(t->vc, i);
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
        VCTerm *vals[32];
        if (binds->n > 32) { t->err = "too many let bindings"; return NULL; }
        for (uint32_t i = 0; i < binds->n; i++) {
            const Sx *b = binds->kids[i];
            if (b->kind != SX_LIST || b->n != 2 || b->kids[0]->kind != SX_ATOM) {
                t->err = "malformed let binding"; return NULL;
            }
            vals[i] = tr_term(t, b->kids[1], depth + 1);
            if (!vals[i]) return NULL;
        }
        for (uint32_t i = 0; i < binds->n; i++) {
            if (t->n_lets >= TR_MAX_LET_BINDS) { t->err = "let overflow"; return NULL; }
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
    if (strcmp(op, "xor") == 0 || strcmp(op, "ite") == 0) {
        /* No VC_ITE / VC_XOR: encoding them would mean inventing a translation
         * this harness cannot verify, so the benchmark is skipped instead. */
        t->err = "ite/xor not in the fragment";
        return NULL;
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
    if (strcmp(op, "div") == 0 || strcmp(op, "/") == 0)
        return tr_fold(t, VC_DIV, s, depth);
    if (strcmp(op, "mod") == 0) return tr_fold(t, VC_MOD, s, depth);
    if (strcmp(op, "-") == 0) {
        if (s->n == 2) {
            VCTerm *a = tr_term(t, s->kids[1], depth + 1);
            return a ? vc_mk1(t->vc, VC_NEG, a) : NULL;
        }
        return tr_fold(t, VC_SUB, s, depth);
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

static bool tr_sort(const Sx *s, VCSort *out) {
    if (!s || s->kind != SX_ATOM) return false;
    if (strcmp(s->atom, "Int")  == 0) { *out = VS_INT;  return true; }
    if (strcmp(s->atom, "Real") == 0) { *out = VS_REAL; return true; }
    if (strcmp(s->atom, "Bool") == 0) { *out = VS_BOOL; return true; }
    return false;
}

/* ------------------------------------------------------------------------- *
 * One benchmark
 * ------------------------------------------------------------------------- */

typedef enum { LBL_SAT, LBL_UNSAT, LBL_UNKNOWN, LBL_NONE } Label;

typedef struct {
    Label   label;
    bool    skipped;
    const char *skip_reason;
    RefineVC *vc;
} Bench;

static void bench_load(Bench *b, const char *text, size_t len, Arena *a) {
    memset(b, 0, sizeof(*b));
    b->label = LBL_NONE;

    SxReader r = { text, text + len, a, NULL };
    RefineVC *vc = vc_new(a);
    b->vc = vc;
    Tr t; memset(&t, 0, sizeof(t)); t.vc = vc;

    for (;;) {
        Sx *cmd = sx_read(&r);
        if (!cmd) {
            if (r.err) { b->skipped = true; b->skip_reason = r.err; }
            break;
        }
        if (cmd->kind != SX_LIST || cmd->n == 0) continue;

        if (sx_head_is(cmd, "set-info")) {
            if (cmd->n >= 3 && sx_is(cmd->kids[1], ":status")) {
                if (sx_is(cmd->kids[2], "sat"))        b->label = LBL_SAT;
                else if (sx_is(cmd->kids[2], "unsat")) b->label = LBL_UNSAT;
                else                                   b->label = LBL_UNKNOWN;
            }
            continue;
        }
        if (sx_head_is(cmd, "set-logic") || sx_head_is(cmd, "set-option") ||
            sx_head_is(cmd, "check-sat") || sx_head_is(cmd, "exit") ||
            sx_head_is(cmd, "get-model") || sx_head_is(cmd, "get-info"))
            continue;

        if (sx_head_is(cmd, "push") || sx_head_is(cmd, "pop") ||
            sx_head_is(cmd, "define-fun") || sx_head_is(cmd, "declare-sort") ||
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

/* ------------------------------------------------------------------------- *
 * Runner
 * ------------------------------------------------------------------------- */

typedef RefineDecision (*Stage)(RefineVC *, Arena *);

static RefineVerdict run_chain(RefineVC *vc, Arena *a) {
    static const Stage CHAIN[] = {
        refine_s0_decide, refine_s1_decide, refine_s2_decide, refine_s3_decide,
    };
    for (size_t i = 0; i < sizeof(CHAIN) / sizeof(CHAIN[0]); i++) {
        RefineDecision d = CHAIN[i](vc, a);
        if (d.verdict != RT_UNKNOWN) return d.verdict;
    }
    return RT_UNKNOWN;
}

static int g_soundness_failures = 0;
static int g_proved = 0, g_unproved = 0, g_skipped = 0, g_unlabelled = 0;
static int g_sat_ok = 0, g_total = 0;

static char *slurp(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    *len_out = got;
    return buf;
}

static void run_one(const char *path) {
    size_t len = 0;
    char *text = slurp(path, &len);
    if (!text) { printf("  ERROR   %s (unreadable)\n", path); g_soundness_failures++; return; }

    Arena arena;
    arena_init(&arena, 1 << 20);
    Arena *a = &arena;
    Bench b;
    bench_load(&b, text, len, a);
    g_total++;

    if (b.skipped) {
        g_skipped++;
        printf("  skip    %s (%s)\n", path, b.skip_reason ? b.skip_reason : "?");
        arena_free(a); free(text);
        return;
    }
    if (b.label == LBL_NONE || b.label == LBL_UNKNOWN) {
        g_unlabelled++;
        printf("  unlab   %s (no :status claim)\n", path);
        arena_free(a); free(text);
        return;
    }

    RefineVerdict v = run_chain(b.vc, a);

    if (b.label == LBL_SAT) {
        /* The invariant: a satisfiable assertion set must never be proved
         * contradictory.  RT_UNKNOWN and RT_INVALID are both correct here. */
        if (v == RT_VALID) {
            g_soundness_failures++;
            printf("  SOUND!  %s -- labelled sat, chain answered VALID "
                   "(claimed the constraints are contradictory)\n", path);
        } else {
            g_sat_ok++;
            printf("  ok      %s (sat, not proved -- correct)\n", path);
        }
    } else {
        if (v == RT_VALID) {
            g_proved++;
            printf("  ok      %s (unsat, proved)\n", path);
        } else {
            g_unproved++;
            printf("  weak    %s (unsat, not proved -- incomplete, not unsound)\n", path);
        }
    }
    arena_free(a);
    free(text);
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Recurses, so the corpus can separate hand-written benchmarks from generated
 * ones without the runner needing to know the layout.  Sorted at every level so
 * the report is stable across filesystems. */
static void run_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        printf("refine_corpus: no corpus directory at %s\n", dir);
        return;
    }
    char *files[8192]; int nf = 0;
    char *subdirs[256];  int nd = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;          /* . .. and dotfiles */
        size_t l = strlen(e->d_name);
        char *full = (char *)malloc(strlen(dir) + l + 2);
        sprintf(full, "%s/%s", dir, e->d_name);
        if (l > 5 && strcmp(e->d_name + l - 5, ".smt2") == 0) {
            if (nf < 8192) files[nf++] = full; else free(full);
            continue;
        }
        DIR *probe = opendir(full);
        if (probe) {
            closedir(probe);
            if (nd < 256) subdirs[nd++] = full; else free(full);
        } else {
            free(full);                              /* README.md and friends */
        }
    }
    closedir(d);
    qsort(files, (size_t)nf, sizeof(char *), cmp_str);
    qsort(subdirs, (size_t)nd, sizeof(char *), cmp_str);
    for (int i = 0; i < nf; i++) { run_one(files[i]); free(files[i]); }
    for (int i = 0; i < nd; i++) { run_dir(subdirs[i]); free(subdirs[i]); }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "tests/corpus/smtlib";
    printf("refine_corpus: replaying %s against the in-house chain (no Z3)\n", dir);
    run_dir(dir);

    printf("\n  benchmarks              : %d\n", g_total);
    printf("  unsat, proved           : %d\n", g_proved);
    printf("  unsat, not proved       : %d  (incomplete, allowed)\n", g_unproved);
    printf("  sat, correctly not proved: %d\n", g_sat_ok);
    printf("  skipped (outside fragment): %d\n", g_skipped);
    printf("  unlabelled              : %d\n", g_unlabelled);
    printf("  SOUNDNESS FAILURES      : %d\n", g_soundness_failures);

    if (g_soundness_failures) {
        printf("\nrefine_corpus: FAIL -- the chain proved a satisfiable "
               "benchmark contradictory\n");
        return 1;
    }
    /* A corpus that decides nothing is not a regression net.  Guard against
     * silently losing the corpus (an empty directory, a reader that skips
     * everything) rather than reporting green for work not done. */
    if (g_total == 0) {
        printf("\nrefine_corpus: FAIL -- no benchmarks found\n");
        return 1;
    }
    if (g_proved + g_sat_ok == 0) {
        printf("\nrefine_corpus: FAIL -- no benchmark was actually decided\n");
        return 1;
    }
    printf("\nrefine_corpus: PASS\n");
    return 0;
}
