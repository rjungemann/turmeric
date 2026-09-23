/* scheme_lower.c -- r7rs-lang-plan R2: lower the Scheme core forms onto
 * Turmeric's binding and control forms.  See scheme_lower.h for the map. */
#include "scheme_lower.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "lang_dialects.h"
#include "stdlib_autoload.h"   /* R3: which `(turmeric stdlib/x)` imports are no-ops */

/* ---------------------------------------------------------------------------
 * The rename table: Scheme spelling -> prelude spelling.
 *
 * The R7RS prelude (stdlib/r7rs/prelude.tur) cannot define `car` -- the typed
 * stdlib's list.tur already does, on the carrier list, and a top-level
 * redefinition of an auto-loaded name is an error -- so every procedure the
 * prelude provides is spelled `r7rs-<name>` and the Scheme spelling is mapped
 * here.  The table is the prelude's export list; a procedure added to the
 * prelude is added here in the same change, or Scheme code cannot reach it.
 * A user `(define (car x) ...)` maps the same way and therefore collides with
 * the prelude's, which is the Turmeric rule for a stdlib name and the
 * documented R2 deviation from R7RS 5.3.1.
 * ------------------------------------------------------------------------- */
static const char *const RENAMES[][2] = {
    { "car",              "r7rs-car" },
    { "cdr",              "r7rs-cdr" },
    { "caar",             "r7rs-caar" },
    { "cadr",             "r7rs-cadr" },
    { "cdar",             "r7rs-cdar" },
    { "cddr",             "r7rs-cddr" },
    { "caddr",            "r7rs-caddr" },
    { "cons",             "r7rs-cons" },
    { "list",             "r7rs-list" },
    { "null?",            "r7rs-null?" },
    { "pair?",            "r7rs-pair?" },
    { "list?",            "r7rs-list?" },
    { "length",           "r7rs-length" },
    { "list-ref",         "r7rs-list-ref" },
    { "list-tail",        "r7rs-list-tail" },
    { "append",           "r7rs-append" },
    { "reverse",          "r7rs-reverse" },
    { "map",              "r7rs-map" },
    { "for-each",         "r7rs-for-each" },
    { "memv",             "r7rs-memv" },
    { "memq",             "r7rs-memq" },
    { "member",           "r7rs-member" },
    { "assv",             "r7rs-assv" },
    { "assq",             "r7rs-assq" },
    { "assoc",            "r7rs-assoc" },
    { "display",          "r7rs-display" },
    { "write",            "r7rs-write" },
    { "newline",          "r7rs-newline" },
    { "not",              "r7rs-not" },
    { "eqv?",             "r7rs-eqv?" },
    { "eq?",              "r7rs-eq?" },
    { "equal?",           "r7rs-equal?" },
    { "boolean?",         "r7rs-boolean?" },
    { "number?",          "r7rs-number?" },
    { "integer?",         "r7rs-integer?" },
    { "real?",            "r7rs-real?" },
    { "string?",          "r7rs-string?" },
    { "symbol?",          "r7rs-symbol?" },
    { "procedure?",       "r7rs-procedure?" },
    { "zero?",            "r7rs-zero?" },
    { "positive?",        "r7rs-positive?" },
    { "negative?",        "r7rs-negative?" },
    { "even?",            "r7rs-even?" },
    { "odd?",             "r7rs-odd?" },
    { "abs",              "r7rs-abs" },
    { "min",              "r7rs-min" },
    { "max",              "r7rs-max" },
    { "values",           "r7rs-values" },
    { "call-with-values", "r7rs-call-with-values" },
    { "apply",            "r7rs-apply" },
    { "error",            "r7rs-error" },
    { "void",             "r7rs-void" },
    /* R3: data. */
    { "set-car!",         "r7rs-set-car!" },
    { "set-cdr!",         "r7rs-set-cdr!" },
    { "list-copy",        "r7rs-list-copy" },
    { "char?",            "r7rs-char?" },
    { "char->integer",    "r7rs-char->integer" },
    { "integer->char",    "r7rs-integer->char" },
    { "char=?",           "r7rs-char=?" },
    { "char<?",           "r7rs-char<?" },
    { "char>?",           "r7rs-char>?" },
    { "char-upcase",      "r7rs-char-upcase" },
    { "char-downcase",    "r7rs-char-downcase" },
    { "char-alphabetic?", "r7rs-char-alphabetic?" },
    { "char-numeric?",    "r7rs-char-numeric?" },
    { "char-whitespace?", "r7rs-char-whitespace?" },
    { "string-length",    "r7rs-string-length" },
    { "string-ref",       "r7rs-string-ref" },
    { "string-append",    "r7rs-string-append" },
    { "substring",        "r7rs-substring" },
    { "string-copy",      "r7rs-string-copy" },
    { "string=?",         "r7rs-string=?" },
    { "string<?",         "r7rs-string<?" },
    { "string->symbol",   "r7rs-string->symbol" },
    { "symbol->string",   "r7rs-symbol->string" },
    { "string->list",     "r7rs-string->list" },
    { "list->string",     "r7rs-list->string" },
    { "number->string",   "r7rs-number->string" },
    { "vector",           "r7rs-vector" },
    { "vector?",          "r7rs-vector?" },
    { "make-vector",      "r7rs-make-vector" },
    { "vector-ref",       "r7rs-vector-ref" },
    { "vector-set!",      "r7rs-vector-set!" },
    { "vector-length",    "r7rs-vector-length" },
    { "vector->list",     "r7rs-vector->list" },
    { "list->vector",     "r7rs-list->vector" },
    { "vector-fill!",     "r7rs-vector-fill!" },
    { "bytevector",       "r7rs-bytevector" },
    { "bytevector?",      "r7rs-bytevector?" },
    { "make-bytevector",  "r7rs-make-bytevector" },
    { "bytevector-u8-ref",  "r7rs-bytevector-u8-ref" },
    { "bytevector-u8-set!", "r7rs-bytevector-u8-set!" },
    { "bytevector-length",  "r7rs-bytevector-length" },
    { "eof-object",       "r7rs-eof-object" },
    { "eof-object?",      "r7rs-eof-object?" },
};
#define N_RENAMES (sizeof(RENAMES) / sizeof(RENAMES[0]))

/* A growable item buffer for building lists. */
typedef struct FB { Form **items; uint32_t n, cap; } FB;

typedef struct SL {
    Arena       *a;
    SymbolTable *st;
    /* Scheme heads. */
    const Symbol *s_define, *s_lambda, *s_let, *s_letstar, *s_letrec,
                 *s_letrecstar, *s_do, *s_begin, *s_set, *s_if, *s_cond,
                 *s_case, *s_and, *s_or, *s_when, *s_unless, *s_case_lambda,
                 *s_define_values, *s_let_values, *s_letstar_values,
                 *s_else, *s_arrow, *s_dot, *s_main, *s_define_syntax,
                 *s_let_syntax, *s_letrec_syntax, *s_import,
                 *s_define_library, *s_define_record_type, *s_quasiquote,
                 *s_unquote, *s_unquote_splicing;
    /* Turmeric heads and markers. */
    const Symbol *t_defn, *t_def, *t_fn, *t_let, *t_letrec, *t_do, *t_if,
                 *t_set, *t_mut, *t_amp, *t_any, *t_int, *t_bool, *t_true,
                 *t_panic;
    /* Prelude names the lowering itself emits. */
    const Symbol *p_eqv, *p_list, *p_length, *p_list_ref, *p_list_tail,
                 *p_chain_to_list, *p_values_ref, *p_values_rest, *p_cons, *p_append, *p_vector,
                 *p_list_to_vector, *p_char, *s_cond_expand, *s_export,
                 *s_include, *t_import, *t_defmodule, *t_export, *t_refer,
                 *t_as, *t_defstruct, *t_heap, *t_is, *t_nil_sym;
    /* R3: an `(import ...)` was lowered, so the program is wrapped in a
     * defmodule (Turmeric's import is only legal there). */
    bool          needs_module;
    /* R3: `(prefix <set> p)` -- a symbol spelled `p<rest>` reads as
     * `<alias>/<rest>`; `(rename <set> (a b))` -- `b` reads as `a`. */
    struct { const char *prefix; size_t plen; const Symbol *alias; } prefixes[16];
    uint32_t n_prefixes;
    struct { const Symbol *from, *to; } renames[64];
    uint32_t n_renames;
    /* R3: the import forms produced so far (placed first in the module), and
     * a define-library under construction. */
    FB            imports;
    bool          has_library;
    const Symbol *lib_name;
    FB            lib_exports;
    FB            lib_body;
    bool          user_main;
    /* Rename table, interned. */
    const Symbol *rn_from[N_RENAMES];
    const Symbol *rn_to[N_RENAMES];
    /* Every `set!` target in the unit (a per-name over-approximation). */
    const Symbol **muts;
    uint32_t       n_muts, cap_muts;
    uint32_t       next_tmp;
} SL;

static const Symbol *I(SL *sl, const char *s) {
    return symtab_intern(sl->st, strslice(s, (uint32_t)strlen(s)));
}

static void sl_init(SL *sl, Arena *a, SymbolTable *st) {
    memset(sl, 0, sizeof *sl);
    sl->a = a; sl->st = st;
    sl->s_define = I(sl, "define");     sl->s_lambda = I(sl, "lambda");
    sl->s_let = I(sl, "let");           sl->s_letstar = I(sl, "let*");
    sl->s_letrec = I(sl, "letrec");     sl->s_letrecstar = I(sl, "letrec*");
    sl->s_do = I(sl, "do");             sl->s_begin = I(sl, "begin");
    sl->s_set = I(sl, "set!");          sl->s_if = I(sl, "if");
    sl->s_cond = I(sl, "cond");         sl->s_case = I(sl, "case");
    sl->s_and = I(sl, "and");           sl->s_or = I(sl, "or");
    sl->s_when = I(sl, "when");         sl->s_unless = I(sl, "unless");
    sl->s_case_lambda = I(sl, "case-lambda");
    sl->s_define_values = I(sl, "define-values");
    sl->s_let_values = I(sl, "let-values");
    sl->s_letstar_values = I(sl, "let*-values");
    sl->s_else = I(sl, "else");         sl->s_arrow = I(sl, "=>");
    sl->s_dot = I(sl, ".");             sl->s_main = I(sl, "main");
    sl->s_define_syntax = I(sl, "define-syntax");
    sl->s_let_syntax = I(sl, "let-syntax");
    sl->s_letrec_syntax = I(sl, "letrec-syntax");
    sl->s_import = I(sl, "import");
    sl->s_define_library = I(sl, "define-library");
    sl->s_define_record_type = I(sl, "define-record-type");
    sl->s_quasiquote = I(sl, "quasiquote");
    sl->s_unquote = I(sl, "unquote");
    sl->s_unquote_splicing = I(sl, "unquote-splicing");

    sl->t_defn = I(sl, "defn");   sl->t_def = I(sl, "def");
    sl->t_fn = I(sl, "fn");       sl->t_let = I(sl, "let");
    sl->t_letrec = I(sl, "letrec"); sl->t_do = I(sl, "do");
    sl->t_if = I(sl, "if");       sl->t_set = I(sl, "set!");
    sl->t_mut = I(sl, "^mut");    sl->t_amp = I(sl, "&");
    sl->t_any = I(sl, "any");     sl->t_int = I(sl, "int");
    sl->t_bool = I(sl, "bool");   sl->t_true = I(sl, "true");
    sl->t_panic = I(sl, "panic");

    sl->p_eqv = I(sl, "r7rs-eqv?");       sl->p_list = I(sl, "r7rs-list");
    sl->p_length = I(sl, "r7rs-length");  sl->p_list_ref = I(sl, "r7rs-list-ref");
    sl->p_list_tail = I(sl, "r7rs-list-tail");
    sl->p_chain_to_list = I(sl, "r7rs-chain->list__");
    sl->p_values_ref = I(sl, "r7rs-values-ref");
    sl->p_values_rest = I(sl, "r7rs-values-rest");
    sl->p_cons = I(sl, "r7rs-cons");         sl->p_append = I(sl, "r7rs-append");
    sl->p_vector = I(sl, "r7rs-vector");     sl->p_list_to_vector = I(sl, "r7rs-list->vector");
    sl->p_char = I(sl, "r7rs-char__");
    sl->s_cond_expand = I(sl, "cond-expand"); sl->s_export = I(sl, "export");
    sl->s_include = I(sl, "include");
    sl->t_import = I(sl, "import");          sl->t_defmodule = I(sl, "defmodule");
    sl->t_export = I(sl, "export");          sl->t_refer = I(sl, "refer");
    sl->t_as = I(sl, "as");                  sl->t_defstruct = I(sl, "defstruct");
    sl->t_heap = I(sl, "heap");              sl->t_is = I(sl, "is?");
    sl->t_nil_sym = I(sl, "nil");

    for (size_t i = 0; i < N_RENAMES; i++) {
        sl->rn_from[i] = I(sl, RENAMES[i][0]);
        sl->rn_to[i]   = I(sl, RENAMES[i][1]);
    }
}

/* --- Form helpers ---------------------------------------------------------- */

static Form *Sym(SL *sl, Span sp, const Symbol *s) { return form_sym(sl->a, sp, s); }
/* `:refer`, `:as`, `:heap` -- the reader spells these as keywords, so the
 * elaborator expects F_KEYWORD, not a symbol whose name starts with ':'. */
static Form *Kw(SL *sl, Span sp, const Symbol *s)  { return form_keyword(sl->a, sp, s); }
static Form *Nil(SL *sl, Span sp)                  { return form_nil(sl->a, sp); }
static Form *Int(SL *sl, Span sp, int64_t v)       { return form_int(sl->a, sp, v); }
static Form *Bool(SL *sl, Span sp, bool v)         { return form_bool(sl->a, sp, v); }

static Form *List(SL *sl, Span sp, Form **items, uint32_t n) {
    return form_list(sl->a, sp, items, n);
}
static Form *Vec(SL *sl, Span sp, Form **items, uint32_t n) {
    return form_vec(sl->a, sp, items, n);
}
/* A list from a fixed set of items. */
static Form *Ln(SL *sl, Span sp, int n, ...) {
    Form *items[16];
    va_list ap; va_start(ap, n);
    for (int i = 0; i < n && i < 16; i++) items[i] = va_arg(ap, Form *);
    va_end(ap);
    return List(sl, sp, items, (uint32_t)n);
}
/* `: any`, the way the reader spells a spaced annotation. */
static Form *AnyAnn(SL *sl, Span sp) {
    return form_type_ann(sl->a, sp, Sym(sl, sp, sl->t_any));
}

static bool is_sym(const Form *f, const Symbol *s) {
    return f && f->tag == F_SYM && f->as.sym == s;
}
static bool head_is(const Form *f, const Symbol *s) {
    return f && f->tag == F_LIST && f->as.list.len > 0 && is_sym(f->as.list.items[0], s);
}
static bool is_scheme_file(const Form *f) {
    const SourceFile *sf = f ? diag_source_file(f->span.file_id) : NULL;
    return sf != NULL && sf->lang == LANG_R7RS;
}

static void fb_push(FB *b, Form *f) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8;
        b->items = (Form **)realloc(b->items, b->cap * sizeof(Form *));
        if (!b->items) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    b->items[b->n++] = f;
}
static Form *fb_list(SL *sl, FB *b, Span sp) {
    Form *f = List(sl, sp, b->items, b->n);
    free(b->items);
    return f;
}
static Form *fb_vec(SL *sl, FB *b, Span sp) {
    Form *f = Vec(sl, sp, b->items, b->n);
    free(b->items);
    return f;
}

/* A fresh symbol no source file has used: the same symbol-table check gensym
 * makes, so a lowering temporary can never capture a user name. */
static const Symbol *fresh(SL *sl, const char *prefix) {
    char buf[64];
    for (;;) {
        snprintf(buf, sizeof buf, "%s%u", prefix, sl->next_tmp++);
        StrSlice s = strslice(buf, (uint32_t)strlen(buf));
        if (!symtab_contains(sl->st, s)) return symtab_intern(sl->st, s);
    }
}

static void err(const Form *at, const char *fmt, ...) {
    char msg[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    diag_emit(DIAG_ERROR, at ? at->span : SPAN_UNKNOWN, "%s", msg);
}

/* --- set! targets ---------------------------------------------------------- */

static void note_mut(SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->n_muts; i++) if (sl->muts[i] == s) return;
    if (sl->n_muts == sl->cap_muts) {
        sl->cap_muts = sl->cap_muts ? sl->cap_muts * 2 : 16;
        sl->muts = (const Symbol **)realloc((void *)sl->muts,
                                            sl->cap_muts * sizeof(const Symbol *));
        if (!sl->muts) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    sl->muts[sl->n_muts++] = s;
}
static bool is_mut(const SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->n_muts; i++) if (sl->muts[i] == s) return true;
    return false;
}
static void collect_muts(SL *sl, const Form *f) {
    if (!f) return;
    switch (f->tag) {
        case F_QUOTE: return;
        case F_LIST: case F_VEC: case F_QUASIQUOTE: case F_UNQUOTE:
        case F_UNQUOTE_SPLICING: case F_MAP: case F_SET: case F_MAP_LITERAL:
        case F_SET_LITERAL:
            if (f->tag == F_LIST && f->as.list.len == 3 &&
                is_sym(f->as.list.items[0], sl->s_set) &&
                f->as.list.items[1]->tag == F_SYM)
                note_mut(sl, f->as.list.items[1]->as.sym);
            for (uint32_t i = 0; i < f->as.list.len; i++)
                collect_muts(sl, f->as.list.items[i]);
            return;
        default: return;
    }
}

/* --- renaming -------------------------------------------------------------- */

static const Symbol *rn(SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->n_renames; i++)
        if (sl->renames[i].from == s) return sl->renames[i].to;
    for (uint32_t i = 0; i < sl->n_prefixes; i++) {
        if (s->len > sl->prefixes[i].plen &&
            memcmp(s->name, sl->prefixes[i].prefix, sl->prefixes[i].plen) == 0) {
            char buf[256];
            snprintf(buf, sizeof buf, "%s/%s", sl->prefixes[i].alias->name,
                     s->name + sl->prefixes[i].plen);
            return I(sl, buf);
        }
    }
    for (size_t i = 0; i < N_RENAMES; i++)
        if (sl->rn_from[i] == s) return sl->rn_to[i];
    return s;
}

/* --- the lowering ---------------------------------------------------------- */

static Form *lower(SL *sl, Form *f);
static Form *lower_body(SL *sl, Form **items, uint32_t n, Span sp);
static void lower_toplevel(SL *sl, Form *f, FB *out);
static void lower_import_set(SL *sl, Form *set);
static const Symbol *library_module(SL *sl, Form *set, bool *ok);
static Form *cond_expand_clause(SL *sl, Form *f, uint32_t *out_n, Form ***out_items);
static void lower_record_type(SL *sl, Form *f, FB *out);
static Form *rebind_rest(SL *sl, Span sp, const Symbol *rest, Form *body);

/* Does `f` mention any of `names` (unquoted)?  Decides whether a `let` needs
 * temporaries to keep its bindings parallel. */
static bool mentions(const Form *f, const Symbol **names, uint32_t n) {
    if (!f) return false;
    if (f->tag == F_SYM) {
        for (uint32_t i = 0; i < n; i++) if (f->as.sym == names[i]) return true;
        return false;
    }
    if (f->tag == F_QUOTE) return false;
    switch (f->tag) {
        case F_LIST: case F_VEC: case F_QUASIQUOTE: case F_UNQUOTE:
        case F_UNQUOTE_SPLICING: case F_MAP: case F_SET: case F_MAP_LITERAL:
        case F_SET_LITERAL: case F_TYPE_ANN:
            for (uint32_t i = 0; i < f->as.list.len; i++)
                if (mentions(f->as.list.items[i], names, n)) return true;
            return false;
        default: return false;
    }
}

/* Is `name` (already renamed) the target of a `set!` anywhere in `f`?
 * Lexical, not file-wide: a `(set! n ...)` in one lambda must not turn every
 * other `n` in the program into an `any` cell.  Works on lowered and
 * unlowered forms alike -- `set!` keeps its head and its target is renamed
 * the same way -- and never looks under `quote`. */
static bool form_sets(SL *sl, const Symbol *name, const Form *f) {
    if (!f) return false;
    switch (f->tag) {
        case F_QUOTE: return false;
        case F_LIST:
            if (f->as.list.len == 3 && is_sym(f->as.list.items[0], sl->s_set) &&
                f->as.list.items[1]->tag == F_SYM &&
                rn(sl, f->as.list.items[1]->as.sym) == name)
                return true;
            /* fall through */
        case F_VEC: case F_QUASIQUOTE: case F_UNQUOTE: case F_UNQUOTE_SPLICING:
        case F_MAP: case F_SET: case F_MAP_LITERAL: case F_SET_LITERAL:
        case F_TYPE_ANN:
            for (uint32_t i = 0; i < f->as.list.len; i++)
                if (form_sets(sl, name, f->as.list.items[i])) return true;
            return false;
        default: return false;
    }
}

/* Push one binding into a `let` vector: `[^mut] name [: any] init`.  A cell
 * that is ever `set!` is typed `any` so the store can hold any later value. */
static void push_binding(SL *sl, FB *b, Span sp, const Symbol *name, Form *init,
                         bool mut) {
    name = rn(sl, name);
    if (mut) {
        fb_push(b, Sym(sl, sp, sl->t_mut));
        fb_push(b, Sym(sl, sp, name));
        fb_push(b, AnyAnn(sl, sp));
    } else {
        fb_push(b, Sym(sl, sp, name));
    }
    fb_push(b, init);
}
/* Push one parameter.  Never `^mut`: a `fn` parameter vector does not take
 * the annotation (it reads as an extra parameter and the call then
 * partially applies), so a parameter that is `set!` is rebound as a mutable
 * local inside the body by rebind_muts instead. */
static void push_param(SL *sl, FB *b, Span sp, const Symbol *name) {
    fb_push(b, Sym(sl, sp, rn(sl, name)));
}

/* (let [^mut p : any p] body) for every parameter in `params` that `body`
 * sets -- the mutable-local rebinding push_param promises. */
static Form *rebind_muts(SL *sl, Span sp, const Form *params, Form *body) {
    FB b = {0};
    for (uint32_t i = 0; i < params->as.list.len; i++) {
        const Form *p = params->as.list.items[i];
        if (p->tag != F_SYM || p->as.sym == sl->t_amp) continue;
        if (form_sets(sl, p->as.sym, body))
            push_binding(sl, &b, sp, p->as.sym, Sym(sl, sp, p->as.sym), true);
    }
    if (b.n == 0) { free(b.items); return body; }
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &b, sp), body);
}

/* A sequence of expressions (a cond clause body, a `begin`): one form, or a
 * `do`.  Not a body: no internal defines. */
static Form *lower_seq(SL *sl, Form **items, uint32_t n, Span sp) {
    if (n == 0) return Nil(sl, sp);
    if (n == 1) return lower(sl, items[0]);
    FB b = {0};
    fb_push(&b, Sym(sl, sp, sl->t_do));
    for (uint32_t i = 0; i < n; i++) fb_push(&b, lower(sl, items[i]));
    return fb_list(sl, &b, sp);
}

/* Formals -> a Turmeric parameter vector.  `(a b)`, `(a b . rest)` and a
 * bare `rest` symbol; the rest parameter is `& rest : any`, and its (renamed)
 * symbol comes back through `out_rest` so the body can rebind it -- inside
 * the body a rest parameter is the carrier `int` list, and the elements
 * were packed as `(Cons any)` cells at the call site, so `(:: rest (Cons
 * any))` is the ascription that gives the list its type back. */
static Form *lower_formals(SL *sl, Form *formals, bool *ok, const Symbol **out_rest) {
    FB b = {0};
    *ok = true;
    if (out_rest) *out_rest = NULL;
    if (formals->tag == F_SYM) {
        fb_push(&b, Sym(sl, formals->span, sl->t_amp));
        fb_push(&b, Sym(sl, formals->span, rn(sl, formals->as.sym)));
        fb_push(&b, AnyAnn(sl, formals->span));
        if (out_rest) *out_rest = rn(sl, formals->as.sym);
        return fb_vec(sl, &b, formals->span);
    }
    if (formals->tag != F_LIST) {
        err(formals, "lambda formals must be a list of identifiers, a single "
                     "identifier, or `(a b . rest)`");
        *ok = false;
        free(b.items);
        return Vec(sl, formals->span, NULL, 0);
    }
    uint32_t n = formals->as.list.len;
    for (uint32_t i = 0; i < n; i++) {
        Form *p = formals->as.list.items[i];
        if (is_sym(p, sl->s_dot)) {
            if (i != n - 2 || formals->as.list.items[n - 1]->tag != F_SYM) {
                err(p, "malformed rest formal: expected `(a b . rest)`");
                *ok = false;
                break;
            }
            fb_push(&b, Sym(sl, p->span, sl->t_amp));
            fb_push(&b, Sym(sl, p->span, rn(sl, formals->as.list.items[n - 1]->as.sym)));
            fb_push(&b, AnyAnn(sl, p->span));
            if (out_rest) *out_rest = rn(sl, formals->as.list.items[n - 1]->as.sym);
            break;
        }
        if (p->tag != F_SYM) {
            err(p, "lambda formal must be an identifier");
            *ok = false;
            break;
        }
        push_param(sl, &b, p->span, p->as.sym);
    }
    return fb_vec(sl, &b, formals->span);
}

/* (let [rest (r7rs-chain->list__ (:: rest (Cons any)))] body) -- see
 * lower_formals.  A variadic `& r : any` arrives as the `(Cons any)` chain
 * Saffron's widen builds; R3 gives Scheme lists their own representation
 * (R7rsPair / the null singleton, so `set-cdr!` and dotted tails work), and
 * the chain is converted at the one place it enters Scheme code, so a rest
 * parameter is a Scheme list like every other list the program sees. */
static Form *rebind_rest(SL *sl, Span sp, const Symbol *rest, Form *body) {
    if (!rest) return body;
    Form *cons_any = Ln(sl, sp, 2, Sym(sl, sp, I(sl, "Cons")), Sym(sl, sp, sl->t_any));
    Form *asc = Ln(sl, sp, 3, Sym(sl, sp, I(sl, "::")), Sym(sl, sp, rest), cons_any);
    Form *conv = Ln(sl, sp, 2, Sym(sl, sp, sl->p_chain_to_list), asc);
    FB b = {0};
    push_binding(sl, &b, sp, rest, conv, form_sets(sl, rest, body));
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &b, sp), body);
}

/* (fn [params] body') */
static Form *lower_lambda_parts(SL *sl, Span sp, Form *formals,
                                Form **body, uint32_t nbody) {
    bool ok;
    const Symbol *rest = NULL;
    Form *params = lower_formals(sl, formals, &ok, &rest);
    if (!ok) return Nil(sl, sp);
    if (nbody == 0) {
        err(formals, "lambda needs a body");
        return Nil(sl, sp);
    }
    Form *lowered = lower_body(sl, body, nbody, sp);
    lowered = rebind_rest(sl, sp, rest, lowered);
    lowered = rebind_muts(sl, sp, params, lowered);
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_fn), params, lowered);
}

static Form *lower_lambda(SL *sl, Form *f) {
    if (f->as.list.len < 3) {
        err(f, "lambda expects (lambda formals body...)");
        return Nil(sl, f->span);
    }
    return lower_lambda_parts(sl, f->span, f->as.list.items[1],
                              f->as.list.items + 2, f->as.list.len - 2);
}

/* Parse `((name init) ...)` into parallel arrays.  Returns false on a shape
 * error (already reported). */
static bool parse_bindings(SL *sl, Form *blist, const Symbol ***out_names,
                           Form ***out_inits, uint32_t *out_n) {
    if (blist->tag != F_LIST) {
        err(blist, "expected a list of (name init) bindings");
        return false;
    }
    uint32_t n = blist->as.list.len;
    const Symbol **names = (const Symbol **)arena_alloc(sl->a, (n + 1) * sizeof(*names));
    Form **inits = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(*inits));
    for (uint32_t i = 0; i < n; i++) {
        Form *b = blist->as.list.items[i];
        if (b->tag == F_SYM) {              /* `(let (x) ...)`: unspecified init */
            names[i] = b->as.sym;
            inits[i] = Nil(sl, b->span);
            continue;
        }
        if (b->tag != F_LIST || b->as.list.len < 1 || b->as.list.len > 2 ||
            b->as.list.items[0]->tag != F_SYM) {
            err(b, "binding must be (name init)");
            return false;
        }
        names[i] = b->as.list.items[0]->as.sym;
        inits[i] = (b->as.list.len == 2) ? b->as.list.items[1] : Nil(sl, b->span);
    }
    *out_names = names; *out_inits = inits; *out_n = n;
    return true;
}

/* (let [b1 i1 ...] body) -- one binding vector. */
static Form *make_let(SL *sl, Span sp, const Symbol **names, Form **inits,
                      uint32_t n, Form *body) {
    FB b = {0};
    for (uint32_t i = 0; i < n; i++)
        push_binding(sl, &b, sp, names[i], inits[i],
                     form_sets(sl, rn(sl, names[i]), body));
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &b, sp), body);
}

static Form *lower_let(SL *sl, Form *f) {
    Span sp = f->span;
    uint32_t len = f->as.list.len;
    if (len < 3) { err(f, "let expects (let bindings body...)"); return Nil(sl, sp); }
    Form *second = f->as.list.items[1];

    /* Turmeric `(let [x 1] ...)` inside a Scheme file: pass through. */
    if (second->tag == F_VEC) return NULL;

    if (second->tag == F_SYM) {
        /* Named let: (letrec [name (fn [vars] body')] (name inits'...)) */
        if (len < 4) { err(f, "named let expects (let name bindings body...)"); return Nil(sl, sp); }
        const Symbol **names; Form **inits; uint32_t n;
        if (!parse_bindings(sl, f->as.list.items[2], &names, &inits, &n)) return Nil(sl, sp);
        const Symbol *loop = rn(sl, second->as.sym);
        FB params = {0};
        for (uint32_t i = 0; i < n; i++) push_param(sl, &params, sp, names[i]);
        Form *pvec = fb_vec(sl, &params, sp);
        Form *lbody = rebind_muts(sl, sp, pvec,
                                  lower_body(sl, f->as.list.items + 3, len - 3, sp));
        Form *fn = Ln(sl, sp, 3, Sym(sl, sp, sl->t_fn), pvec, lbody);
        FB call = {0};
        fb_push(&call, Sym(sl, sp, loop));
        for (uint32_t i = 0; i < n; i++) fb_push(&call, lower(sl, inits[i]));
        Form *bvec[2] = { Sym(sl, sp, loop), fn };
        return Ln(sl, sp, 3, Sym(sl, sp, sl->t_letrec), Vec(sl, sp, bvec, 2),
                  fb_list(sl, &call, sp));
    }

    const Symbol **names; Form **inits; uint32_t n;
    if (!parse_bindings(sl, second, &names, &inits, &n)) return Nil(sl, sp);
    Form *body = lower_body(sl, f->as.list.items + 2, len - 2, sp);
    if (n == 0) return body;
    Form **linits = (Form **)arena_alloc(sl->a, n * sizeof(Form *));
    bool parallel_matters = false;
    for (uint32_t i = 0; i < n; i++) {
        linits[i] = lower(sl, inits[i]);
        if (n > 1 && mentions(inits[i], names, n)) parallel_matters = true;
    }
    if (!parallel_matters) return make_let(sl, sp, names, linits, n, body);
    /* An init mentions a binder: Scheme evaluates every init in the OUTER
     * scope, Turmeric's `let` is sequential, so bind temporaries first. */
    const Symbol **tmps = (const Symbol **)arena_alloc(sl->a, n * sizeof(*tmps));
    Form **tmp_refs = (Form **)arena_alloc(sl->a, n * sizeof(Form *));
    for (uint32_t i = 0; i < n; i++) {
        tmps[i] = fresh(sl, "__r7rs_let");
        tmp_refs[i] = Sym(sl, sp, tmps[i]);
    }
    Form *inner = make_let(sl, sp, names, tmp_refs, n, body);
    FB b = {0};
    for (uint32_t i = 0; i < n; i++) {
        fb_push(&b, Sym(sl, sp, tmps[i]));
        fb_push(&b, linits[i]);
    }
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &b, sp), inner);
}

/* let*: nested single-binding lets, innermost last. */
static Form *lower_letstar(SL *sl, Form *f) {
    Span sp = f->span;
    if (f->as.list.len < 3) { err(f, "let* expects (let* bindings body...)"); return Nil(sl, sp); }
    if (f->as.list.items[1]->tag == F_VEC) return NULL;   /* Turmeric let* */
    const Symbol **names; Form **inits; uint32_t n;
    if (!parse_bindings(sl, f->as.list.items[1], &names, &inits, &n)) return Nil(sl, sp);
    /* Lower inits before the body so temporaries are numbered in source order. */
    Form **linits = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(Form *));
    for (uint32_t i = 0; i < n; i++) linits[i] = lower(sl, inits[i]);
    Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
    for (int32_t i = (int32_t)n - 1; i >= 0; i--)
        body = make_let(sl, sp, names + i, linits + i, 1, body);
    return body;
}

/* letrec / letrec*: Turmeric's letrec (a lambda may name any sibling). */
static Form *lower_letrec(SL *sl, Form *f) {
    Span sp = f->span;
    if (f->as.list.len < 3) { err(f, "letrec expects (letrec bindings body...)"); return Nil(sl, sp); }
    if (f->as.list.items[1]->tag == F_VEC) return NULL;   /* Turmeric letrec */
    const Symbol **names; Form **inits; uint32_t n;
    if (!parse_bindings(sl, f->as.list.items[1], &names, &inits, &n)) return Nil(sl, sp);
    FB b = {0};
    for (uint32_t i = 0; i < n; i++) {
        fb_push(&b, Sym(sl, sp, rn(sl, names[i])));
        fb_push(&b, lower(sl, inits[i]));
    }
    Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
    if (n == 0) { free(b.items); return body; }
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_letrec), fb_vec(sl, &b, sp), body);
}

/* A Scheme `do` loop has the shape (do ((var init step)...) (test res...)
 * cmd...); anything else with that head is Turmeric's `do` sequence. */
static bool looks_like_scheme_do(const Form *f) {
    if (f->as.list.len < 3) return false;
    const Form *specs = f->as.list.items[1];
    const Form *test  = f->as.list.items[2];
    if (specs->tag != F_LIST || test->tag != F_LIST) return false;
    for (uint32_t i = 0; i < specs->as.list.len; i++)
        if (specs->as.list.items[i]->tag != F_LIST) return false;
    return true;
}

static Form *lower_do(SL *sl, Form *f) {
    Span sp = f->span;
    Form *specs = f->as.list.items[1];
    Form *test  = f->as.list.items[2];
    uint32_t n = specs->as.list.len;
    const Symbol **names = (const Symbol **)arena_alloc(sl->a, (n + 1) * sizeof(*names));
    Form **inits = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(Form *));
    Form **steps = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(Form *));
    for (uint32_t i = 0; i < n; i++) {
        Form *s = specs->as.list.items[i];
        if (s->as.list.len < 2 || s->as.list.len > 3 || s->as.list.items[0]->tag != F_SYM) {
            err(s, "do binding must be (var init [step])");
            return Nil(sl, sp);
        }
        names[i] = s->as.list.items[0]->as.sym;
        inits[i] = lower(sl, s->as.list.items[1]);
        steps[i] = (s->as.list.len == 3) ? lower(sl, s->as.list.items[2])
                                         : Sym(sl, sp, rn(sl, names[i]));
    }
    if (test->as.list.len < 1) { err(test, "do needs (test result...)"); return Nil(sl, sp); }
    const Symbol *loop = fresh(sl, "__r7rs_do");
    FB params = {0};
    for (uint32_t i = 0; i < n; i++) push_param(sl, &params, sp, names[i]);
    Form *result = lower_seq(sl, test->as.list.items + 1, test->as.list.len - 1, sp);
    /* (do cmds'... (loop steps'...)) */
    FB again = {0};
    fb_push(&again, Sym(sl, sp, sl->t_do));
    for (uint32_t i = 3; i < f->as.list.len; i++) fb_push(&again, lower(sl, f->as.list.items[i]));
    FB recur = {0};
    fb_push(&recur, Sym(sl, sp, loop));
    for (uint32_t i = 0; i < n; i++) fb_push(&recur, steps[i]);
    fb_push(&again, fb_list(sl, &recur, sp));
    Form *body = Ln(sl, sp, 4, Sym(sl, sp, sl->t_if), lower(sl, test->as.list.items[0]),
                    result, fb_list(sl, &again, sp));
    Form *pvec = fb_vec(sl, &params, sp);
    body = rebind_muts(sl, sp, pvec, body);
    Form *fn = Ln(sl, sp, 3, Sym(sl, sp, sl->t_fn), pvec, body);
    FB call = {0};
    fb_push(&call, Sym(sl, sp, loop));
    for (uint32_t i = 0; i < n; i++) fb_push(&call, inits[i]);
    Form *bvec[2] = { Sym(sl, sp, loop), fn };
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_letrec), Vec(sl, sp, bvec, 2),
              fb_list(sl, &call, sp));
}

static Form *lower_if(SL *sl, Form *f) {
    Span sp = f->span;
    uint32_t len = f->as.list.len;
    if (len != 3 && len != 4) { err(f, "if expects (if test then [else])"); return Nil(sl, sp); }
    Form *c = lower(sl, f->as.list.items[1]);
    Form *t = lower(sl, f->as.list.items[2]);
    Form *e = (len == 4) ? lower(sl, f->as.list.items[3]) : Nil(sl, sp);
    return Ln(sl, sp, 4, Sym(sl, sp, sl->t_if), c, t, e);
}

/* Scheme `cond` clauses are lists; Turmeric's are a flat pair-up. */
static bool looks_like_scheme_cond(const Form *f) {
    if (f->as.list.len < 2) return false;
    for (uint32_t i = 1; i < f->as.list.len; i++)
        if (f->as.list.items[i]->tag != F_LIST || f->as.list.items[i]->as.list.len == 0)
            return false;
    return true;
}

static Form *cond_chain(SL *sl, Form **clauses, uint32_t n, Span sp) {
    if (n == 0) return Nil(sl, sp);
    Form *cl = clauses[0];
    Form **it = cl->as.list.items;
    uint32_t len = cl->as.list.len;
    if (is_sym(it[0], sl->s_else))
        return lower_seq(sl, it + 1, len - 1, cl->span);
    Form *rest = cond_chain(sl, clauses + 1, n - 1, sp);
    Form *test = lower(sl, it[0]);
    if (len == 1) {
        /* (test): the test's value is the result. */
        const Symbol *t = fresh(sl, "__r7rs_cond");
        Form *body = Ln(sl, cl->span, 4, Sym(sl, cl->span, sl->t_if),
                        Sym(sl, cl->span, t), Sym(sl, cl->span, t), rest);
        Form *bv[2] = { Sym(sl, cl->span, t), test };
        return Ln(sl, cl->span, 3, Sym(sl, cl->span, sl->t_let), Vec(sl, cl->span, bv, 2), body);
    }
    if (is_sym(it[1], sl->s_arrow)) {
        if (len != 3) { err(cl, "cond clause with => expects (test => receiver)"); return Nil(sl, sp); }
        const Symbol *t = fresh(sl, "__r7rs_cond");
        Form *call = Ln(sl, cl->span, 2, lower(sl, it[2]), Sym(sl, cl->span, t));
        Form *body = Ln(sl, cl->span, 4, Sym(sl, cl->span, sl->t_if),
                        Sym(sl, cl->span, t), call, rest);
        Form *bv[2] = { Sym(sl, cl->span, t), test };
        return Ln(sl, cl->span, 3, Sym(sl, cl->span, sl->t_let), Vec(sl, cl->span, bv, 2), body);
    }
    return Ln(sl, cl->span, 4, Sym(sl, cl->span, sl->t_if), test,
              lower_seq(sl, it + 1, len - 1, cl->span), rest);
}

/* A datum in a `case` clause: symbols are quoted, everything else is itself. */
static Form *case_datum(SL *sl, Form *d) {
    if (d->tag == F_SYM) return form_quote(sl->a, d->span, d);
    return d;
}

static bool looks_like_scheme_case(const SL *sl, const Form *f) {
    if (f->as.list.len < 3) return false;
    for (uint32_t i = 2; i < f->as.list.len; i++) {
        const Form *cl = f->as.list.items[i];
        if (cl->tag != F_LIST || cl->as.list.len < 2) return false;
        const Form *d = cl->as.list.items[0];
        if (d->tag != F_LIST && !is_sym(d, sl->s_else)) return false;
    }
    return true;
}

static Form *lower_case(SL *sl, Form *f) {
    Span sp = f->span;
    const Symbol *k = fresh(sl, "__r7rs_case");
    Form *chain = Nil(sl, sp);
    for (int32_t i = (int32_t)f->as.list.len - 1; i >= 2; i--) {
        Form *cl = f->as.list.items[i];
        Form **it = cl->as.list.items;
        uint32_t len = cl->as.list.len;
        Form *body;
        if (len >= 3 && is_sym(it[1], sl->s_arrow)) {
            body = Ln(sl, cl->span, 2, lower(sl, it[2]), Sym(sl, cl->span, k));
        } else {
            body = lower_seq(sl, it + 1, len - 1, cl->span);
        }
        if (is_sym(it[0], sl->s_else)) { chain = body; continue; }
        if (it[0]->tag != F_LIST) { err(cl, "case clause expects ((datum...) body...) or (else body...)"); return Nil(sl, sp); }
        /* (if (eqv? k d1) true (if (eqv? k d2) true false)) -- static bools. */
        Form *test = Bool(sl, cl->span, false);
        for (int32_t j = (int32_t)it[0]->as.list.len - 1; j >= 0; j--) {
            Form *eq = Ln(sl, cl->span, 3, Sym(sl, cl->span, sl->p_eqv),
                          Sym(sl, cl->span, k), case_datum(sl, it[0]->as.list.items[j]));
            test = Ln(sl, cl->span, 4, Sym(sl, cl->span, sl->t_if), eq,
                      Bool(sl, cl->span, true), test);
        }
        chain = Ln(sl, cl->span, 4, Sym(sl, cl->span, sl->t_if), test, body, chain);
    }
    Form *bv[2] = { Sym(sl, sp, k), lower(sl, f->as.list.items[1]) };
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), Vec(sl, sp, bv, 2), chain);
}

/* (and) -> #t; (and a) -> a; (and a b...) -> (let [t a] (if t (and b...) t)) */
static Form *and_chain(SL *sl, Form **args, uint32_t n, Span sp) {
    if (n == 0) return Bool(sl, sp, true);
    if (n == 1) return lower(sl, args[0]);
    const Symbol *t = fresh(sl, "__r7rs_and");
    Form *body = Ln(sl, sp, 4, Sym(sl, sp, sl->t_if), Sym(sl, sp, t),
                    and_chain(sl, args + 1, n - 1, sp), Sym(sl, sp, t));
    Form *bv[2] = { Sym(sl, sp, t), lower(sl, args[0]) };
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), Vec(sl, sp, bv, 2), body);
}
/* (or) -> #f; (or a) -> a; (or a b...) -> (let [t a] (if t t (or b...))) */
static Form *or_chain(SL *sl, Form **args, uint32_t n, Span sp) {
    if (n == 0) return Bool(sl, sp, false);
    if (n == 1) return lower(sl, args[0]);
    const Symbol *t = fresh(sl, "__r7rs_or");
    Form *body = Ln(sl, sp, 4, Sym(sl, sp, sl->t_if), Sym(sl, sp, t),
                    Sym(sl, sp, t), or_chain(sl, args + 1, n - 1, sp));
    Form *bv[2] = { Sym(sl, sp, t), lower(sl, args[0]) };
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), Vec(sl, sp, bv, 2), body);
}

static Form *lower_when_unless(SL *sl, Form *f, bool negate) {
    Span sp = f->span;
    if (f->as.list.len < 3) { err(f, "%s expects (test body...)", negate ? "unless" : "when"); return Nil(sl, sp); }
    Form *c = lower(sl, f->as.list.items[1]);
    Form *body = lower_seq(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
    return negate ? Ln(sl, sp, 4, Sym(sl, sp, sl->t_if), c, Nil(sl, sp), body)
                  : Ln(sl, sp, 4, Sym(sl, sp, sl->t_if), c, body, Nil(sl, sp));
}

/* (case-lambda (formals body...)...) -> a variadic fn that dispatches on the
 * argument count and applies the matching clause as a lambda. */
static Form *lower_case_lambda(SL *sl, Form *f) {
    Span sp = f->span;
    const Symbol *args = fresh(sl, "__r7rs_args");
    const Symbol *nsym = fresh(sl, "__r7rs_nargs");
    Form *chain = Ln(sl, sp, 2, Sym(sl, sp, sl->t_panic),
                     form_str(sl->a, sp, "case-lambda: no clause matches the argument count", 50));
    for (int32_t i = (int32_t)f->as.list.len - 1; i >= 1; i--) {
        Form *cl = f->as.list.items[i];
        if (cl->tag != F_LIST || cl->as.list.len < 2) { err(cl, "case-lambda clause expects (formals body...)"); return Nil(sl, sp); }
        Form *formals = cl->as.list.items[0];
        uint32_t fixed = 0; bool has_rest = false;
        if (formals->tag == F_SYM) { has_rest = true; }
        else if (formals->tag == F_LIST) {
            for (uint32_t j = 0; j < formals->as.list.len; j++) {
                if (is_sym(formals->as.list.items[j], sl->s_dot)) { has_rest = true; break; }
                fixed++;
            }
        } else { err(formals, "case-lambda formals must be a list or an identifier"); return Nil(sl, sp); }
        /* A rest clause takes its rest as ONE list parameter: the clause lambda
         * is built from formals with the dot removed (`(a b . more)` ->
         * `(a b more)`, a bare `more` -> `(more)`) and called with the fixed
         * arguments and `(list-tail args fixed)`.  Same body semantics, and no
         * dynamic call has to spread a list into a variadic closure, which
         * neither back end's apply helpers do. */
        Form *clause_formals = formals;
        if (has_rest) {
            FB nf = {0};
            if (formals->tag == F_SYM) {
                fb_push(&nf, formals);
            } else {
                for (uint32_t j = 0; j < formals->as.list.len; j++) {
                    Form *p = formals->as.list.items[j];
                    if (is_sym(p, sl->s_dot)) continue;
                    fb_push(&nf, p);
                }
            }
            clause_formals = fb_list(sl, &nf, formals->span);
        }
        Form *lam = lower_lambda_parts(sl, cl->span, clause_formals, cl->as.list.items + 1, cl->as.list.len - 1);
        /* (lam (list-ref args 0) ... [(list-tail args fixed)]) */
        FB call = {0};
        fb_push(&call, lam);
        for (uint32_t j = 0; j < fixed; j++)
            fb_push(&call, Ln(sl, cl->span, 3, Sym(sl, cl->span, sl->p_list_ref),
                              Sym(sl, cl->span, args), Int(sl, cl->span, (int64_t)j)));
        if (has_rest) {
            fb_push(&call, Ln(sl, cl->span, 3, Sym(sl, cl->span, sl->p_list_tail),
                              Sym(sl, cl->span, args), Int(sl, cl->span, (int64_t)fixed)));
            Form *test = Ln(sl, cl->span, 3, Sym(sl, cl->span, I(sl, ">=")),
                            Sym(sl, cl->span, nsym), Int(sl, cl->span, (int64_t)fixed));
            chain = Ln(sl, cl->span, 4, Sym(sl, cl->span, sl->t_if), test,
                       fb_list(sl, &call, cl->span), chain);
            continue;
        }
        Form *test = Ln(sl, cl->span, 3, Sym(sl, cl->span, I(sl, "=")),
                        Sym(sl, cl->span, nsym), Int(sl, cl->span, (int64_t)fixed));
        chain = Ln(sl, cl->span, 4, Sym(sl, cl->span, sl->t_if), test,
                   fb_list(sl, &call, cl->span), chain);
    }
    Form *nbind[2] = { Sym(sl, sp, nsym),
                       Ln(sl, sp, 2, Sym(sl, sp, sl->p_length), Sym(sl, sp, args)) };
    Form *body = rebind_rest(sl, sp, args,
                             Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), Vec(sl, sp, nbind, 2), chain));
    Form *params[3] = { Sym(sl, sp, sl->t_amp), Sym(sl, sp, args), AnyAnn(sl, sp) };
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_fn), Vec(sl, sp, params, 3), body);
}

/* (r7rs-values-ref tmp i) / (r7rs-values-rest tmp i) */
static Form *values_ref(SL *sl, Span sp, const Symbol *tmp, uint32_t i, bool rest) {
    return Ln(sl, sp, 3, Sym(sl, sp, rest ? sl->p_values_rest : sl->p_values_ref),
              Sym(sl, sp, tmp), Int(sl, sp, (int64_t)i));
}

/* Push `formals` bound from the Values carrier in `tmp` onto a binding
 * vector; `body` (lowered) decides which cells are mutable. */
static bool push_values_bindings(SL *sl, FB *b, Form *formals, const Symbol *tmp,
                                 const Form *body) {
    Span sp = formals->span;
    if (formals->tag == F_SYM) {
        push_binding(sl, b, sp, formals->as.sym, values_ref(sl, sp, tmp, 0, true),
                     form_sets(sl, rn(sl, formals->as.sym), body));
        return true;
    }
    if (formals->tag != F_LIST) { err(formals, "formals must be a list or an identifier"); return false; }
    uint32_t n = formals->as.list.len;
    for (uint32_t i = 0; i < n; i++) {
        Form *p = formals->as.list.items[i];
        if (is_sym(p, sl->s_dot)) {
            if (i != n - 2 || formals->as.list.items[n - 1]->tag != F_SYM) { err(p, "malformed rest formal"); return false; }
            push_binding(sl, b, sp, formals->as.list.items[n - 1]->as.sym, values_ref(sl, sp, tmp, i, true),
                         form_sets(sl, rn(sl, formals->as.list.items[n - 1]->as.sym), body));
            return true;
        }
        if (p->tag != F_SYM) { err(p, "formal must be an identifier"); return false; }
        push_binding(sl, b, sp, p->as.sym, values_ref(sl, sp, tmp, i, false),
                     form_sets(sl, rn(sl, p->as.sym), body));
    }
    return true;
}

/* let-values / let*-values: (let [t1 e1 ...] (let [a (ref t1 0) ...] body)) --
 * the star form nests one binding at a time. */
static Form *lower_let_values(SL *sl, Form *f, bool star) {
    Span sp = f->span;
    if (f->as.list.len < 3 || f->as.list.items[1]->tag != F_LIST) {
        err(f, "%s expects (((formals) init)...) body...", star ? "let*-values" : "let-values");
        return Nil(sl, sp);
    }
    Form *specs = f->as.list.items[1];
    uint32_t n = specs->as.list.len;
    if (star) {
        Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
        for (int32_t i = (int32_t)n - 1; i >= 0; i--) {
            Form *s = specs->as.list.items[i];
            if (s->tag != F_LIST || s->as.list.len != 2) { err(s, "binding must be (formals init)"); return Nil(sl, sp); }
            const Symbol *tmp = fresh(sl, "__r7rs_vals");
            FB inner = {0};
            if (!push_values_bindings(sl, &inner, s->as.list.items[0], tmp, body)) { free(inner.items); return Nil(sl, sp); }
            body = Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &inner, sp), body);
            Form *bv[2] = { Sym(sl, sp, tmp), lower(sl, s->as.list.items[1]) };
            body = Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), Vec(sl, sp, bv, 2), body);
        }
        return body;
    }
    FB outer = {0}, inner = {0};
    /* Inits first (source order for temporaries), then the body, then the
     * inner bindings, which need the body to decide mutability. */
    Form **linits = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(Form *));
    for (uint32_t i = 0; i < n; i++) {
        Form *s = specs->as.list.items[i];
        if (s->tag != F_LIST || s->as.list.len != 2) { err(s, "binding must be (formals init)"); return Nil(sl, sp); }
        linits[i] = lower(sl, s->as.list.items[1]);
    }
    Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
    for (uint32_t i = 0; i < n; i++) {
        Form *s = specs->as.list.items[i];
        const Symbol *tmp = fresh(sl, "__r7rs_vals");
        fb_push(&outer, Sym(sl, sp, tmp));
        fb_push(&outer, linits[i]);
        if (!push_values_bindings(sl, &inner, s->as.list.items[0], tmp, body)) { free(outer.items); free(inner.items); return Nil(sl, sp); }
    }
    Form *in = Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &inner, sp), body);
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &outer, sp), in);
}

/* `(define-values formals expr)` -> a run of `define`s over a temporary.
 * Returned as SCHEME forms so the caller lowers them in its own context
 * (top level or body). */
static uint32_t expand_define_values(SL *sl, Form *f, FB *out) {
    Span sp = f->span;
    if (f->as.list.len != 3) { err(f, "define-values expects (define-values formals expr)"); return 0; }
    Form *formals = f->as.list.items[1];
    const Symbol *tmp = fresh(sl, "__r7rs_dvals");
    fb_push(out, Ln(sl, sp, 3, Sym(sl, sp, sl->s_define), Sym(sl, sp, tmp), f->as.list.items[2]));
    if (formals->tag == F_SYM) {
        fb_push(out, Ln(sl, sp, 3, Sym(sl, sp, sl->s_define), formals, values_ref(sl, sp, tmp, 0, true)));
        return 2;
    }
    if (formals->tag != F_LIST) { err(formals, "define-values formals must be a list or an identifier"); return 0; }
    uint32_t n = formals->as.list.len, made = 1;
    for (uint32_t i = 0; i < n; i++) {
        Form *p = formals->as.list.items[i];
        if (is_sym(p, sl->s_dot)) {
            if (i != n - 2 || formals->as.list.items[n - 1]->tag != F_SYM) { err(p, "malformed rest formal"); return 0; }
            fb_push(out, Ln(sl, sp, 3, Sym(sl, sp, sl->s_define), formals->as.list.items[n - 1], values_ref(sl, sp, tmp, i, true)));
            made++;
            break;
        }
        if (p->tag != F_SYM) { err(p, "formal must be an identifier"); return 0; }
        fb_push(out, Ln(sl, sp, 3, Sym(sl, sp, sl->s_define), p, values_ref(sl, sp, tmp, i, false)));
        made++;
    }
    return made;
}

/* A body: internal defines at the start (R7RS 5.3.2 -- letrec* order), then
 * the expressions.  Lambdas group into a `letrec` so they may be mutually
 * recursive; a value define is a `let` (mutable if ever set!). */
static Form *lower_body(SL *sl, Form **items, uint32_t n, Span sp) {
    /* Expand define-values into plain defines first, so one loop sees them. */
    FB seq = {0};
    for (uint32_t i = 0; i < n; i++) {
        if (head_is(items[i], sl->s_define_values)) expand_define_values(sl, items[i], &seq);
        else if (head_is(items[i], sl->s_begin)) {
            /* (begin (define ...) ...) at body start splices its defines. */
            for (uint32_t j = 1; j < items[i]->as.list.len; j++) fb_push(&seq, items[i]->as.list.items[j]);
        }
        else fb_push(&seq, items[i]);
    }
    items = seq.items; n = seq.n;

    uint32_t ndef = 0;
    while (ndef < n && head_is(items[ndef], sl->s_define)) ndef++;
    for (uint32_t i = ndef; i < n; i++) {
        if (head_is(items[i], sl->s_define)) {
            err(items[i], "define is only allowed at the beginning of a body (R7RS 5.3.2)");
            free(seq.items);
            return Nil(sl, sp);
        }
    }
    Form *rest = lower_seq(sl, items + ndef, n - ndef, sp);
    if (ndef == 0) { free(seq.items); return rest; }

    /* name / lowered init / is-lambda, in source order. */
    const Symbol **names = (const Symbol **)arena_alloc(sl->a, ndef * sizeof(*names));
    Form **inits = (Form **)arena_alloc(sl->a, ndef * sizeof(Form *));
    bool *is_lam = (bool *)arena_alloc(sl->a, ndef * sizeof(bool));
    for (uint32_t i = 0; i < ndef; i++) {
        Form *d = items[i];
        if (d->as.list.len < 2) { err(d, "define expects (define name expr) or (define (name . formals) body...)"); free(seq.items); return Nil(sl, sp); }
        Form *target = d->as.list.items[1];
        if (target->tag == F_LIST && target->as.list.len >= 1 && target->as.list.items[0]->tag == F_SYM) {
            names[i] = target->as.list.items[0]->as.sym;
            Form *formals = List(sl, target->span, target->as.list.items + 1, target->as.list.len - 1);
            inits[i] = lower_lambda_parts(sl, d->span, formals, d->as.list.items + 2, d->as.list.len - 2);
            is_lam[i] = true;
        } else if (target->tag == F_SYM) {
            if (d->as.list.len != 3) { err(d, "define expects (define name expr)"); free(seq.items); return Nil(sl, sp); }
            names[i] = target->as.sym;
            inits[i] = lower(sl, d->as.list.items[2]);
            is_lam[i] = head_is(d->as.list.items[2], sl->s_lambda);
        } else {
            err(d, "define target must be an identifier or (name . formals)");
            free(seq.items);
            return Nil(sl, sp);
        }
    }
    /* A lambda define that is later `set!` cannot live in a letrec (no
     * mutable letrec cell); it becomes a `let` and loses self-reference by
     * name, which R7RS programs rarely need of a reassigned procedure. */
    for (uint32_t i = 0; i < ndef; i++) {
        if (!is_lam[i]) continue;
        for (uint32_t k = 0; k < n; k++)
            if (form_sets(sl, rn(sl, names[i]), items[k])) { is_lam[i] = false; break; }
    }
    free(seq.items);
    Form *body = rest;
    int32_t i = (int32_t)ndef - 1;
    while (i >= 0) {
        if (is_lam[i]) {
            int32_t j = i;
            while (j > 0 && is_lam[j - 1]) j--;
            FB b = {0};
            for (int32_t k = j; k <= i; k++) {
                fb_push(&b, Sym(sl, sp, rn(sl, names[k])));
                fb_push(&b, inits[k]);
            }
            body = Ln(sl, sp, 3, Sym(sl, sp, sl->t_letrec), fb_vec(sl, &b, sp), body);
            i = j - 1;
        } else {
            body = make_let(sl, sp, names + i, inits + i, 1, body);
            i--;
        }
    }
    return body;
}

/* R3 / D4: `quote` constructs runtime data.  A symbol stays the `(quote sym)`
 * the substrate already understands (an interned Sym), an atom is itself, a
 * character literal is the `(r7rs-char__ n)` call the reader produced, a
 * list is `(r7rs-list d...)` (a dotted one a `r7rs-cons` chain ending in the
 * tail datum), a vector `(r7rs-vector d...)`.  Built at runtime each time the
 * expression runs -- the static `.rodata` table D4 wants is deferred with the
 * literal-mutation question (Section 8, Q2). */
static bool is_char_form(SL *sl, const Form *f) {
    return f->tag == F_LIST && f->as.list.len == 2 && is_sym(f->as.list.items[0], sl->p_char) &&
           f->as.list.items[1]->tag == F_INT;
}
static Form *lower_datum(SL *sl, Form *d);
static Form *datum_list(SL *sl, Form *f) {
    Span sp = f->span;
    uint32_t n = f->as.list.len;
    /* dotted: (a b . t) */
    if (n >= 3 && is_sym(f->as.list.items[n - 2], sl->s_dot)) {
        Form *tail = lower_datum(sl, f->as.list.items[n - 1]);
        for (int32_t i = (int32_t)n - 3; i >= 0; i--)
            tail = Ln(sl, sp, 3, Sym(sl, sp, sl->p_cons), lower_datum(sl, f->as.list.items[i]), tail);
        return tail;
    }
    FB b = {0};
    fb_push(&b, Sym(sl, sp, sl->p_list));
    for (uint32_t i = 0; i < n; i++) fb_push(&b, lower_datum(sl, f->as.list.items[i]));
    return fb_list(sl, &b, sp);
}
static Form *lower_datum(SL *sl, Form *d) {
    Span sp = d->span;
    switch (d->tag) {
        case F_SYM:
            if (d->as.sym == sl->t_nil_sym) return Ln(sl, sp, 1, Sym(sl, sp, sl->p_list));
            return form_quote(sl->a, sp, d);
        case F_LIST:
            if (is_char_form(sl, d)) return d;
            return datum_list(sl, d);
        case F_VEC: {
            FB b = {0};
            fb_push(&b, Sym(sl, sp, sl->p_vector));
            for (uint32_t i = 0; i < d->as.list.len; i++) fb_push(&b, lower_datum(sl, d->as.list.items[i]));
            return fb_list(sl, &b, sp);
        }
        case F_QUOTE: case F_QUASIQUOTE: case F_UNQUOTE: case F_UNQUOTE_SPLICING: {
            /* ''x is (quote x) as data: a two-element list. */
            const char *head = d->tag == F_QUOTE ? "quote" : d->tag == F_QUASIQUOTE ? "quasiquote"
                             : d->tag == F_UNQUOTE ? "unquote" : "unquote-splicing";
            Form *h = form_quote(sl->a, sp, Sym(sl, sp, I(sl, head)));
            return Ln(sl, sp, 3, Sym(sl, sp, sl->p_list), h, lower_datum(sl, d->as.list.items[0]));
        }
        case F_NIL: return Ln(sl, sp, 1, Sym(sl, sp, sl->p_list));
        default: return d;   /* int, float, string, bool, keyword */
    }
}

/* Quasiquote: the datum walker with holes.  At depth 1 an `unquote` is an
 * expression and an `unquote-splicing` element splices via `r7rs-append`;
 * a nested quasiquote raises the depth and its unquotes lower it, staying
 * data (R7RS 4.2.8). */
static Form *lower_qq(SL *sl, Form *f, int depth);
static Form *qq_list(SL *sl, Form *f, int depth) {
    Span sp = f->span;
    uint32_t n = f->as.list.len;
    Form *tail;
    int32_t last;
    if (n >= 3 && is_sym(f->as.list.items[n - 2], sl->s_dot)) {
        tail = lower_qq(sl, f->as.list.items[n - 1], depth);
        last = (int32_t)n - 3;
    } else {
        tail = Ln(sl, sp, 1, Sym(sl, sp, sl->p_list));
        last = (int32_t)n - 1;
    }
    for (int32_t i = last; i >= 0; i--) {
        Form *it = f->as.list.items[i];
        if (it->tag == F_UNQUOTE_SPLICING && depth == 1)
            tail = Ln(sl, sp, 3, Sym(sl, sp, sl->p_append), lower(sl, it->as.list.items[0]), tail);
        else
            tail = Ln(sl, sp, 3, Sym(sl, sp, sl->p_cons), lower_qq(sl, it, depth), tail);
    }
    return tail;
}
static Form *lower_qq(SL *sl, Form *f, int depth) {
    Span sp = f->span;
    switch (f->tag) {
        case F_UNQUOTE:
            if (depth == 1) return lower(sl, f->as.list.items[0]);
            return Ln(sl, sp, 3, Sym(sl, sp, sl->p_list),
                      form_quote(sl->a, sp, Sym(sl, sp, sl->s_unquote)),
                      lower_qq(sl, f->as.list.items[0], depth - 1));
        case F_UNQUOTE_SPLICING:
            return Ln(sl, sp, 3, Sym(sl, sp, sl->p_list),
                      form_quote(sl->a, sp, Sym(sl, sp, sl->s_unquote_splicing)),
                      lower_qq(sl, f->as.list.items[0], depth - 1));
        case F_QUASIQUOTE:
            return Ln(sl, sp, 3, Sym(sl, sp, sl->p_list),
                      form_quote(sl->a, sp, Sym(sl, sp, sl->s_quasiquote)),
                      lower_qq(sl, f->as.list.items[0], depth + 1));
        case F_LIST:
            if (is_char_form(sl, f)) return f;
            return qq_list(sl, f, depth);
        case F_VEC: {
            Form *as_list = form_new(sl->a, F_LIST, sp);
            as_list->as.list = f->as.list;
            return Ln(sl, sp, 2, Sym(sl, sp, sl->p_list_to_vector), qq_list(sl, as_list, depth));
        }
        default: return lower_datum(sl, f);
    }
}

/* Lower every subform of a list-shaped form, keeping its tag. */
static Form *lower_children(SL *sl, Form *f) {
    Form **it = (Form **)arena_alloc(sl->a, (f->as.list.len + 1) * sizeof(Form *));
    bool changed = false;
    for (uint32_t i = 0; i < f->as.list.len; i++) {
        it[i] = lower(sl, f->as.list.items[i]);
        if (it[i] != f->as.list.items[i]) changed = true;
    }
    if (!changed) return f;
    Form *g = form_new(sl->a, f->tag, f->span);
    *g = *f;
    g->as.list.items = it;
    return g;
}

static Form *lower(SL *sl, Form *f) {
    if (!f) return f;
    switch (f->tag) {
        case F_SYM: {
            const Symbol *r = rn(sl, f->as.sym);
            return (r == f->as.sym) ? f : Sym(sl, f->span, r);
        }
        case F_QUOTE:      return lower_datum(sl, f->as.list.items[0]);
        case F_QUASIQUOTE: return lower_qq(sl, f->as.list.items[0], 1);
        case F_VEC: case F_MAP: case F_SET: case F_MAP_LITERAL: case F_SET_LITERAL:
        case F_UNQUOTE: case F_UNQUOTE_SPLICING:
            return lower_children(sl, f);
        case F_LIST: break;
        default: return f;
    }
    if (f->as.list.len == 0) return f;
    Form *head = f->as.list.items[0];
    if (head->tag == F_SYM) {
        const Symbol *h = head->as.sym;
        Form *r = NULL;
        if (h == sl->s_lambda)      return lower_lambda(sl, f);
        if (h == sl->s_let)         { r = lower_let(sl, f);      if (r) return r; }
        if (h == sl->s_letstar)     { r = lower_letstar(sl, f);  if (r) return r; }
        if (h == sl->s_letrec || h == sl->s_letrecstar) { r = lower_letrec(sl, f); if (r) return r; }
        if (h == sl->s_do && looks_like_scheme_do(f)) return lower_do(sl, f);
        if (h == sl->s_begin)       return lower_seq(sl, f->as.list.items + 1, f->as.list.len - 1, f->span);
        if (h == sl->s_if)          return lower_if(sl, f);
        if (h == sl->s_cond && looks_like_scheme_cond(f))
            return cond_chain(sl, f->as.list.items + 1, f->as.list.len - 1, f->span);
        if (h == sl->s_case && looks_like_scheme_case(sl, f)) return lower_case(sl, f);
        if (h == sl->s_and)         return and_chain(sl, f->as.list.items + 1, f->as.list.len - 1, f->span);
        if (h == sl->s_or)          return or_chain(sl, f->as.list.items + 1, f->as.list.len - 1, f->span);
        if (h == sl->s_when)        return lower_when_unless(sl, f, false);
        if (h == sl->s_unless)      return lower_when_unless(sl, f, true);
        if (h == sl->s_case_lambda) return lower_case_lambda(sl, f);
        if (h == sl->s_let_values)  return lower_let_values(sl, f, false);
        if (h == sl->s_letstar_values) return lower_let_values(sl, f, true);
        if (h == sl->s_define) {
            err(f, "define is not allowed in expression position; a body's "
                   "defines come first (R7RS 5.3.2)");
            return Nil(sl, f->span);
        }
        if (h == sl->s_define_syntax || h == sl->s_let_syntax || h == sl->s_letrec_syntax) {
            err(f, "%s: syntax-rules is r7rs-lang-plan R4 and has not landed yet", h->name);
            return Nil(sl, f->span);
        }
        if (h == sl->s_define_record_type) {
            err(f, "define-record-type is only allowed at the top level or in a library body");
            return Nil(sl, f->span);
        }
        if (h == sl->s_cond_expand) {
            uint32_t n; Form **items;
            if (!cond_expand_clause(sl, f, &n, &items)) return Nil(sl, f->span);
            return lower_seq(sl, items, n, f->span);
        }
    }
    return lower_children(sl, f);
}

/* One top-level Scheme form -> zero or more Turmeric top-level forms. */
static void lower_toplevel(SL *sl, Form *f, FB *out) {
    Span sp = f->span;
    if (head_is(f, sl->s_begin)) {
        for (uint32_t i = 1; i < f->as.list.len; i++) lower_toplevel(sl, f->as.list.items[i], out);
        return;
    }
    if (head_is(f, sl->s_define_values)) {
        FB defs = {0};
        expand_define_values(sl, f, &defs);
        for (uint32_t i = 0; i < defs.n; i++) lower_toplevel(sl, defs.items[i], out);
        free(defs.items);
        return;
    }
    if (head_is(f, sl->s_define)) {
        if (f->as.list.len < 2) { err(f, "define expects (define name expr) or (define (name . formals) body...)"); return; }
        Form *target = f->as.list.items[1];
        if (target->tag == F_LIST) {
            if (target->as.list.len < 1 || target->as.list.items[0]->tag != F_SYM) {
                err(target, "define target must be an identifier or (name . formals); "
                            "a curried define is not R7RS");
                return;
            }
            const Symbol *name = rn(sl, target->as.list.items[0]->as.sym);
            Form *formals = List(sl, target->span, target->as.list.items + 1, target->as.list.len - 1);
            bool ok;
            const Symbol *rest = NULL;
            Form *params = lower_formals(sl, formals, &ok, &rest);
            if (!ok) return;
            if (f->as.list.len < 3) { err(f, "define needs a body"); return; }
            Form *body = rebind_rest(sl, sp, rest,
                                     lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp));
            if (name == sl->s_main && params->as.list.len == 0) {
                /* `(define (main) ...)` is the program's entry: Turmeric's main
                 * returns int, so run the body for effect and answer 0. */
                sl->user_main = true;
                Form *ann = form_type_ann(sl->a, sp, Sym(sl, sp, sl->t_int));
                fb_push(out, Ln(sl, sp, 6, Sym(sl, sp, sl->t_defn), Sym(sl, sp, name),
                                params, ann, body, Int(sl, sp, 0)));
                return;
            }
            fb_push(out, Ln(sl, sp, 4, Sym(sl, sp, sl->t_defn), Sym(sl, sp, name), params, body));
            return;
        }
        if (target->tag != F_SYM || f->as.list.len != 3) {
            err(f, "define expects (define name expr)");
            return;
        }
        const Symbol *name = rn(sl, target->as.sym);
        Form *init = lower(sl, f->as.list.items[2]);
        if (is_mut(sl, name)) {
            fb_push(out, Ln(sl, sp, 5, Sym(sl, sp, sl->t_def), Sym(sl, sp, sl->t_mut),
                            Sym(sl, sp, name), AnyAnn(sl, sp), init));
        } else {
            fb_push(out, Ln(sl, sp, 3, Sym(sl, sp, sl->t_def), Sym(sl, sp, name), init));
        }
        return;
    }
    if (head_is(f, sl->s_import) && f->as.list.len >= 2 && f->as.list.items[1]->tag == F_LIST) {
        /* R3 / D9: each import set maps onto a Turmeric import (or a no-op
         * when the names are already global); the program is then wrapped
         * in a defmodule, where Turmeric's import is legal. */
        for (uint32_t i = 1; i < f->as.list.len; i++) lower_import_set(sl, f->as.list.items[i]);
        return;
    }
    if (head_is(f, sl->s_define_library)) {
        /* (define-library (my utils) (export ...) (import ...) (begin ...))
         * -> (defmodule my/utils (export ...) imports... body...) */
        if (sl->has_library) { err(f, "only one define-library per file (Turmeric: one defmodule per file)"); return; }
        if (f->as.list.len < 2) { err(f, "define-library needs a library name"); return; }
        bool ok;
        const Symbol *name = library_module(sl, f->as.list.items[1], &ok);
        if (!ok) return;
        if (!name) { err(f->as.list.items[1], "a (scheme ...) or auto-loaded stdlib name cannot be defined here"); return; }
        sl->has_library = true;
        sl->lib_name = name;
        for (uint32_t i = 2; i < f->as.list.len; i++) {
            Form *decl = f->as.list.items[i];
            if (head_is(decl, sl->s_export)) {
                for (uint32_t j = 1; j < decl->as.list.len; j++) {
                    Form *nm = decl->as.list.items[j];
                    if (nm->tag == F_LIST && nm->as.list.len == 3 && is_sym(nm->as.list.items[0], I(sl, "rename"))) {
                        err(nm, "(export (rename a b)) is not supported yet; export the name and rename at the import");
                        continue;
                    }
                    if (nm->tag != F_SYM) { err(nm, "export names must be identifiers"); continue; }
                    fb_push(&sl->lib_exports, Sym(sl, nm->span, rn(sl, nm->as.sym)));
                }
            } else if (head_is(decl, sl->s_import)) {
                for (uint32_t j = 1; j < decl->as.list.len; j++) lower_import_set(sl, decl->as.list.items[j]);
            } else if (head_is(decl, sl->s_begin)) {
                for (uint32_t j = 1; j < decl->as.list.len; j++) lower_toplevel(sl, decl->as.list.items[j], &sl->lib_body);
            } else if (head_is(decl, sl->s_cond_expand)) {
                uint32_t n; Form **items;
                if (cond_expand_clause(sl, decl, &n, &items))
                    for (uint32_t j = 0; j < n; j++) {
                        /* a chosen clause holds declarations, not forms */
                        Form *d = items[j];
                        if (head_is(d, sl->s_begin))
                            for (uint32_t k = 1; k < d->as.list.len; k++) lower_toplevel(sl, d->as.list.items[k], &sl->lib_body);
                        else if (head_is(d, sl->s_import))
                            for (uint32_t k = 1; k < d->as.list.len; k++) lower_import_set(sl, d->as.list.items[k]);
                        else err(d, "cond-expand inside define-library takes (import ...) and (begin ...) declarations");
                    }
            } else if (head_is(decl, sl->s_include)) {
                err(decl, "(include \"file\") in a library is not supported yet; write the definitions in a (begin ...)");
            } else {
                err(decl, "define-library declarations are (export ...), (import ...), (begin ...) and (cond-expand ...)");
            }
        }
        return;
    }
    if (head_is(f, sl->s_cond_expand)) {
        uint32_t n; Form **items;
        if (cond_expand_clause(sl, f, &n, &items))
            for (uint32_t i = 0; i < n; i++) lower_toplevel(sl, items[i], out);
        return;
    }
    if (head_is(f, sl->s_define_record_type)) {
        lower_record_type(sl, f, out);
        return;
    }
    if (head_is(f, sl->s_define) && f->as.list.len >= 2 && f->as.list.items[1]->tag == F_LIST &&
        f->as.list.items[1]->as.list.len >= 1 && is_sym(f->as.list.items[1]->as.list.items[0], sl->s_main) &&
        f->as.list.items[1]->as.list.len == 1)
        sl->user_main = true;
    fb_push(out, lower(sl, f));
}

/* ---------------------------------------------------------------------------
 * R3 / D9: the library system and the `(turmeric ...)` seam.
 * ------------------------------------------------------------------------- */

/* Is `<name>.tur` an auto-loaded stdlib file?  Its names are global already,
 * so `(import (turmeric stdlib/<name>))` is a no-op rather than an import
 * the module loader would fail to resolve. */
static bool stdlib_autoloaded(const char *name) {
    const char *const *files = tur_stdlib_autoload_files();
    size_t n = strlen(name);
    for (size_t i = 0; files && files[i]; i++) {
        const char *f = files[i];
        size_t fl = strlen(f);
        if (fl == n + 4 && memcmp(f, name, n) == 0 && strcmp(f + n, ".tur") == 0) return true;
    }
    return false;
}

/* The Turmeric module a library NAME denotes, or NULL when the import is a
 * no-op (`(scheme ...)` is the prelude; an auto-loaded stdlib file is already
 * global).  `(turmeric a/b/c)` is the module `a/b/c` with a leading `stdlib/`
 * dropped (the module loader's stdlib fallback finds it); any other name
 * `(my utils)` joins with `/`. */
static const Symbol *library_module(SL *sl, Form *set, bool *ok) {
    *ok = true;
    if (set->tag != F_LIST || set->as.list.len == 0 || set->as.list.items[0]->tag != F_SYM) {
        err(set, "a library name is a list of identifiers, e.g. (scheme base) or (turmeric stdlib/vec)");
        *ok = false;
        return NULL;
    }
    const char *head = set->as.list.items[0]->as.sym->name;
    if (strcmp(head, "scheme") == 0) return NULL;
    if (strcmp(head, "turmeric") == 0) {
        if (set->as.list.len != 2 || set->as.list.items[1]->tag != F_SYM) {
            err(set, "(turmeric <module>) takes one module path, e.g. (turmeric stdlib/vec) or (turmeric json/encode)");
            *ok = false;
            return NULL;
        }
        const char *m = set->as.list.items[1]->as.sym->name;
        if (strncmp(m, "stdlib/", 7) == 0) m += 7;
        if (stdlib_autoloaded(m)) return NULL;
        return I(sl, m);
    }
    char buf[256]; size_t at = 0;
    for (uint32_t i = 0; i < set->as.list.len; i++) {
        const Form *p = set->as.list.items[i];
        if (p->tag != F_SYM) { err(p, "a library name part must be an identifier"); *ok = false; return NULL; }
        int w = snprintf(buf + at, sizeof buf - at, "%s%s", i ? "/" : "", p->as.sym->name);
        if (w < 0 || (size_t)w >= sizeof buf - at) { err(set, "library name too long"); *ok = false; return NULL; }
        at += (size_t)w;
    }
    return I(sl, buf);
}

/* One import set -> zero or one Turmeric `(import ...)` form in sl->imports,
 * plus the rename/prefix rules `rename`/`prefix` need. */
static void lower_import_set(SL *sl, Form *set) {
    Span sp = set->span;
    if (set->tag != F_LIST || set->as.list.len == 0) { err(set, "malformed import set"); return; }
    Form *head = set->as.list.items[0];
    const char *h = head->tag == F_SYM ? head->as.sym->name : "";
    bool ok;
    if (strcmp(h, "only") == 0 || strcmp(h, "rename") == 0 || strcmp(h, "prefix") == 0 ||
        strcmp(h, "except") == 0) {
        if (set->as.list.len < 2) { err(set, "(%s <import set> ...) needs an import set", h); return; }
        Form *inner = set->as.list.items[1];
        if (inner->tag == F_LIST && inner->as.list.len > 0 && inner->as.list.items[0]->tag == F_SYM) {
            const char *ih = inner->as.list.items[0]->as.sym->name;
            if (strcmp(ih, "only") == 0 || strcmp(ih, "rename") == 0 || strcmp(ih, "prefix") == 0 ||
                strcmp(ih, "except") == 0) {
                err(set, "nested import sets are not supported yet; use one of only/prefix/rename directly on the library name");
                return;
            }
        }
        if (strcmp(h, "except") == 0) {
            err(set, "(except ...) is not supported: Turmeric's import has no \"all but\"; list the names with (only ...) instead");
            return;
        }
        const Symbol *mod = library_module(sl, inner, &ok);
        if (!ok) return;
        if (strcmp(h, "only") == 0) {
            FB names = {0};
            for (uint32_t i = 2; i < set->as.list.len; i++) {
                Form *nm = set->as.list.items[i];
                if (nm->tag != F_SYM) { err(nm, "(only ...) names must be identifiers"); free(names.items); return; }
                fb_push(&names, Sym(sl, nm->span, rn(sl, nm->as.sym)));
            }
            if (!mod) { free(names.items); return; }   /* already global */
            fb_push(&sl->imports, Ln(sl, sp, 4, Sym(sl, sp, sl->t_import), Sym(sl, sp, mod),
                                     Kw(sl, sp, sl->t_refer), fb_vec(sl, &names, sp)));
            sl->needs_module = true;
            return;
        }
        if (strcmp(h, "rename") == 0) {
            FB names = {0};
            for (uint32_t i = 2; i < set->as.list.len; i++) {
                Form *pr = set->as.list.items[i];
                if (pr->tag != F_LIST || pr->as.list.len != 2 || pr->as.list.items[0]->tag != F_SYM ||
                    pr->as.list.items[1]->tag != F_SYM) {
                    err(pr, "(rename <set> (from to) ...) takes identifier pairs"); free(names.items); return;
                }
                const Symbol *from = rn(sl, pr->as.list.items[0]->as.sym);
                if (sl->n_renames < 64) {
                    sl->renames[sl->n_renames].from = pr->as.list.items[1]->as.sym;
                    sl->renames[sl->n_renames].to   = from;
                    sl->n_renames++;
                }
                fb_push(&names, Sym(sl, pr->span, from));
            }
            if (!mod) { free(names.items); return; }
            fb_push(&sl->imports, Ln(sl, sp, 4, Sym(sl, sp, sl->t_import), Sym(sl, sp, mod),
                                     Kw(sl, sp, sl->t_refer), fb_vec(sl, &names, sp)));
            sl->needs_module = true;
            return;
        }
        /* prefix */
        if (set->as.list.len != 3 || set->as.list.items[2]->tag != F_SYM) {
            err(set, "(prefix <set> <identifier>) takes one prefix identifier"); return;
        }
        const Symbol *pfx = set->as.list.items[2]->as.sym;
        if (sl->n_prefixes >= 16) { err(set, "too many (prefix ...) imports"); return; }
        if (!mod) {
            /* Auto-loaded: the names are global and bare, so the prefix just
             * comes off.  Recorded as an alias-less rule. */
            sl->prefixes[sl->n_prefixes].prefix = pfx->name;
            sl->prefixes[sl->n_prefixes].plen   = pfx->len;
            sl->prefixes[sl->n_prefixes].alias  = NULL;
            sl->n_prefixes++;
            return;
        }
        /* The alias is the prefix without a trailing ':' or '-'; `p:name` then
         * reads as `p/name`, which is Turmeric's `:as p` spelling. */
        char alias[128];
        snprintf(alias, sizeof alias, "%s", pfx->name);
        size_t al = strlen(alias);
        while (al > 0 && (alias[al - 1] == ':' || alias[al - 1] == '-')) alias[--al] = '\0';
        if (al == 0) { err(set, "(prefix ...) needs a non-empty prefix"); return; }
        sl->prefixes[sl->n_prefixes].prefix = pfx->name;
        sl->prefixes[sl->n_prefixes].plen   = pfx->len;
        sl->prefixes[sl->n_prefixes].alias  = I(sl, alias);
        sl->n_prefixes++;
        fb_push(&sl->imports, Ln(sl, sp, 4, Sym(sl, sp, sl->t_import), Sym(sl, sp, mod),
                                 Kw(sl, sp, sl->t_as), Sym(sl, sp, I(sl, alias))));
        sl->needs_module = true;
        return;
    }
    const Symbol *mod = library_module(sl, set, &ok);
    if (!ok || !mod) return;
    fb_push(&sl->imports, Ln(sl, sp, 2, Sym(sl, sp, sl->t_import), Sym(sl, sp, mod)));
    sl->needs_module = true;
}

/* cond-expand feature requirements: `r7rs`, `turmeric`, `else` and any
 * `(library (scheme ...))` hold; `and`/`or`/`not` compose. */
static bool feature_holds(SL *sl, Form *req) {
    if (req->tag == F_SYM) {
        const char *n = req->as.sym->name;
        return strcmp(n, "r7rs") == 0 || strcmp(n, "turmeric") == 0 || strcmp(n, "else") == 0 ||
               strcmp(n, "exact-closed") == 0;
    }
    if (req->tag != F_LIST || req->as.list.len == 0 || req->as.list.items[0]->tag != F_SYM) return false;
    const char *h = req->as.list.items[0]->as.sym->name;
    if (strcmp(h, "and") == 0) {
        for (uint32_t i = 1; i < req->as.list.len; i++) if (!feature_holds(sl, req->as.list.items[i])) return false;
        return true;
    }
    if (strcmp(h, "or") == 0) {
        for (uint32_t i = 1; i < req->as.list.len; i++) if (feature_holds(sl, req->as.list.items[i])) return true;
        return false;
    }
    if (strcmp(h, "not") == 0) return req->as.list.len == 2 && !feature_holds(sl, req->as.list.items[1]);
    if (strcmp(h, "library") == 0 && req->as.list.len == 2) {
        bool ok; const Symbol *m = library_module(sl, req->as.list.items[1], &ok);
        (void)m;
        return ok;   /* (scheme ...) and an auto-loaded stdlib file hold; a module we cannot check is assumed present */
    }
    return false;
}
/* The body of the first cond-expand clause that holds, or NULL. */
static Form *cond_expand_clause(SL *sl, Form *f, uint32_t *out_n, Form ***out_items) {
    for (uint32_t i = 1; i < f->as.list.len; i++) {
        Form *cl = f->as.list.items[i];
        if (cl->tag != F_LIST || cl->as.list.len == 0) { err(cl, "cond-expand clause expects (<feature requirement> body...)"); return NULL; }
        if (feature_holds(sl, cl->as.list.items[0])) {
            *out_items = cl->as.list.items + 1;
            *out_n = cl->as.list.len - 1;
            return cl;
        }
    }
    *out_n = 0; *out_items = NULL;
    return NULL;
}

/* (define-record-type <name> (ctor f...) pred (f accessor [modifier])...)
 * -> a heap defstruct of `any` fields plus the procedures (D3: a record is
 * an ordinary Turmeric type, its predicate an `is?`). */
static void lower_record_type(SL *sl, Form *f, FB *out) {
    Span sp = f->span;
    if (f->as.list.len < 4 || f->as.list.items[1]->tag != F_SYM || f->as.list.items[2]->tag != F_LIST ||
        f->as.list.items[3]->tag != F_SYM) {
        err(f, "define-record-type expects (define-record-type <name> (ctor field...) pred (field accessor [modifier])...)");
        return;
    }
    /* Struct name: the record name with `<`/`>` dropped and a prefix, so it
     * can never collide with a stdlib type. */
    char sbuf[128]; size_t at = 0;
    const char *rn_name = f->as.list.items[1]->as.sym->name;
    at += (size_t)snprintf(sbuf, sizeof sbuf, "R7rsRec_");
    for (const char *p = rn_name; *p && at + 1 < sizeof sbuf; p++) {
        char c = *p;
        if (c == '<' || c == '>') continue;
        sbuf[at++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
    }
    sbuf[at] = '\0';
    const Symbol *sname = I(sl, sbuf);
    /* Fields, in declaration order. */
    uint32_t nf = f->as.list.len - 4;
    const Symbol **fields = (const Symbol **)arena_alloc(sl->a, (nf + 1) * sizeof(*fields));
    FB fvec = {0};
    for (uint32_t i = 0; i < nf; i++) {
        Form *spec = f->as.list.items[4 + i];
        if (spec->tag != F_LIST || spec->as.list.len < 2 || spec->as.list.len > 3 || spec->as.list.items[0]->tag != F_SYM) {
            err(spec, "record field spec expects (field accessor [modifier])"); free(fvec.items); return;
        }
        fields[i] = spec->as.list.items[0]->as.sym;
        fb_push(&fvec, Sym(sl, spec->span, fields[i]));
        fb_push(&fvec, AnyAnn(sl, spec->span));
    }
    fb_push(out, Ln(sl, sp, 4, Sym(sl, sp, sl->t_defstruct), Sym(sl, sp, sname), Kw(sl, sp, sl->t_heap),
                    fb_vec(sl, &fvec, sp)));
    /* Constructor: its parameters name a subset of the fields, in any order;
     * an unmentioned field starts as nil. */
    Form *ctor = f->as.list.items[2];
    if (ctor->as.list.len < 1 || ctor->as.list.items[0]->tag != F_SYM) { err(ctor, "record constructor spec expects (name field...)"); return; }
    FB params = {0}, args = {0};
    for (uint32_t i = 1; i < ctor->as.list.len; i++) {
        Form *p = ctor->as.list.items[i];
        if (p->tag != F_SYM) { err(p, "constructor field must be an identifier"); free(params.items); free(args.items); return; }
        fb_push(&params, Sym(sl, p->span, rn(sl, p->as.sym)));
    }
    fb_push(&args, Sym(sl, sp, sname));
    for (uint32_t i = 0; i < nf; i++) {
        bool named = false;
        for (uint32_t j = 1; j < ctor->as.list.len; j++)
            if (ctor->as.list.items[j]->as.sym == fields[i]) { named = true; break; }
        fb_push(&args, named ? Sym(sl, sp, rn(sl, fields[i])) : Nil(sl, sp));
    }
    fb_push(out, Ln(sl, sp, 4, Sym(sl, sp, sl->t_defn), Sym(sl, sp, rn(sl, ctor->as.list.items[0]->as.sym)),
                    fb_vec(sl, &params, sp), fb_list(sl, &args, sp)));
    /* Predicate. */
    {
        const Symbol *x = I(sl, "x");
        Form *pv[1] = { Sym(sl, sp, x) };
        fb_push(out, Ln(sl, sp, 5, Sym(sl, sp, sl->t_defn), Sym(sl, sp, rn(sl, f->as.list.items[3]->as.sym)),
                        Vec(sl, sp, pv, 1), form_type_ann(sl->a, sp, Sym(sl, sp, sl->t_bool)),
                        Ln(sl, sp, 3, Sym(sl, sp, sl->t_is), Sym(sl, sp, x), Sym(sl, sp, sname))));
    }
    /* Accessors and modifiers. */
    for (uint32_t i = 0; i < nf; i++) {
        Form *spec = f->as.list.items[4 + i];
        const Symbol *r = I(sl, "r");
        char fld[128]; snprintf(fld, sizeof fld, ".%s", fields[i]->name);
        Form *sann = form_type_ann(sl->a, sp, Sym(sl, sp, sname));
        Form *acc_params[2] = { Sym(sl, sp, r), sann };
        Form *read = Ln(sl, sp, 2, Sym(sl, sp, I(sl, fld)), Sym(sl, sp, r));
        if (spec->as.list.items[1]->tag != F_SYM) { err(spec, "accessor must be an identifier"); return; }
        fb_push(out, Ln(sl, sp, 5, Sym(sl, sp, sl->t_defn), Sym(sl, sp, rn(sl, spec->as.list.items[1]->as.sym)),
                        Vec(sl, sp, acc_params, 2), AnyAnn(sl, sp), read));
        if (spec->as.list.len == 3) {
            if (spec->as.list.items[2]->tag != F_SYM) { err(spec, "modifier must be an identifier"); return; }
            const Symbol *v = I(sl, "v");
            Form *mod_params[3] = { Sym(sl, sp, r), form_type_ann(sl->a, sp, Sym(sl, sp, sname)), Sym(sl, sp, v) };
            Form *store = Ln(sl, sp, 3, Sym(sl, sp, sl->t_set), read, Sym(sl, sp, v));
            fb_push(out, Ln(sl, sp, 4, Sym(sl, sp, sl->t_defn), Sym(sl, sp, rn(sl, spec->as.list.items[2]->as.sym)),
                            Vec(sl, sp, mod_params, 3), store));
        }
    }
}

bool scheme_lower_needed(Form *const *forms, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (is_scheme_file(forms[i])) return true;
    return false;
}

Form **scheme_lower_program(Arena *a, SymbolTable *st,
                            Form *const *forms, uint32_t n, uint32_t *out_n) {
    SL sl;
    sl_init(&sl, a, st);
    for (uint32_t i = 0; i < n; i++)
        if (is_scheme_file(forms[i])) collect_muts(&sl, forms[i]);
    FB out = {0}, sforms = {0};
    Span first_sp = SPAN_UNKNOWN;
    bool have_first = false;
    for (uint32_t i = 0; i < n; i++) {
        if (is_scheme_file(forms[i])) {
            if (!have_first) { first_sp = forms[i]->span; have_first = true; }
            lower_toplevel(&sl, forms[i], &sforms);
        } else {
            fb_push(&out, forms[i]);
        }
    }
    if (sl.has_library) {
        /* A library file: exactly the defmodule.  Anything else at top level
         * in the same file has nowhere to go. */
        if (sforms.n > 0) err(sforms.items[0], "a file with a define-library may hold nothing else at top level");
        FB m = {0};
        fb_push(&m, Sym(&sl, first_sp, sl.t_defmodule));
        fb_push(&m, Sym(&sl, first_sp, sl.lib_name));
        FB ex = {0};
        fb_push(&ex, Sym(&sl, first_sp, sl.t_export));
        for (uint32_t i = 0; i < sl.lib_exports.n; i++) fb_push(&ex, sl.lib_exports.items[i]);
        fb_push(&m, fb_list(&sl, &ex, first_sp));
        for (uint32_t i = 0; i < sl.imports.n; i++) fb_push(&m, sl.imports.items[i]);
        for (uint32_t i = 0; i < sl.lib_body.n; i++) fb_push(&m, sl.lib_body.items[i]);
        fb_push(&out, fb_list(&sl, &m, first_sp));
        free(sl.lib_exports.items); free(sl.lib_body.items); free(sl.imports.items);
    } else if (sl.needs_module && have_first) {
        /* A program with imports: wrap it in a defmodule named after its file,
         * imports first, definitions next, and the top-level expressions as
         * the body of a synthesized `main` (the top-level fold does not look
         * inside a module). */
        const SourceFile *sf = diag_source_file(first_sp.file_id);
        char mbuf[128] = "r7rs-program";
        if (sf && sf->path) {
            const char *base = strrchr(sf->path, '/');
            base = base ? base + 1 : sf->path;
            size_t at = 0;
            for (const char *p = base; *p && at + 1 < sizeof mbuf; p++) {
                if (*p == '.') break;
                mbuf[at++] = (*p == '-' || *p == '_' || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                              (*p >= '0' && *p <= '9')) ? *p : '-';
            }
            if (at) mbuf[at] = '\0';
        }
        FB m = {0}, stmts = {0};
        fb_push(&m, Sym(&sl, first_sp, sl.t_defmodule));
        fb_push(&m, Sym(&sl, first_sp, I(&sl, mbuf)));
        for (uint32_t i = 0; i < sl.imports.n; i++) fb_push(&m, sl.imports.items[i]);
        for (uint32_t i = 0; i < sforms.n; i++) {
            Form *f = sforms.items[i];
            bool is_def = f->tag == F_LIST && f->as.list.len > 0 && f->as.list.items[0]->tag == F_SYM &&
                          strncmp(f->as.list.items[0]->as.sym->name, "def", 3) == 0;
            if (is_def) fb_push(&m, f); else fb_push(&stmts, f);
        }
        if (stmts.n > 0) {
            if (sl.user_main) {
                err(stmts.items[0], "a program that imports libraries and defines (main) cannot also have top-level expressions; move them into main");
            }
            FB body = {0};
            fb_push(&body, Sym(&sl, first_sp, sl.t_do));
            for (uint32_t i = 0; i < stmts.n; i++) fb_push(&body, stmts.items[i]);
            fb_push(&body, Int(&sl, first_sp, 0));
            fb_push(&m, Ln(&sl, first_sp, 5, Sym(&sl, first_sp, sl.t_defn), Sym(&sl, first_sp, sl.s_main),
                           Vec(&sl, first_sp, NULL, 0),
                           form_type_ann(sl.a, first_sp, Sym(&sl, first_sp, sl.t_int)),
                           fb_list(&sl, &body, first_sp)));
        }
        free(stmts.items);
        fb_push(&out, fb_list(&sl, &m, first_sp));
        free(sl.imports.items);
    } else {
        for (uint32_t i = 0; i < sforms.n; i++) fb_push(&out, sforms.items[i]);
        free(sl.imports.items);
    }
    free(sforms.items);
    Form **res = (Form **)arena_alloc(a, (out.n + 1) * sizeof(Form *));
    for (uint32_t i = 0; i < out.n; i++) res[i] = out.items[i];
    res[out.n] = NULL;
    *out_n = out.n;
    free(out.items);
    free((void *)sl.muts);
    return res;
}
