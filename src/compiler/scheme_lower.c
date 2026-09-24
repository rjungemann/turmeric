/* scheme_lower.c -- r7rs-lang-plan R2: lower the Scheme core forms onto
 * Turmeric's binding and control forms.  See scheme_lower.h for the map. */
#include "scheme_lower.h"
#include "runtime/globals.h"   /* R9: g_synthetic_user_from_line */

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "lang_dialects.h"
#include "expr.h"              /* R10: tur_name_is_reserved_special_form */
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
    /* R7: the rest of (scheme base) that is not a port, and the pure
     * libraries -- (scheme char), (scheme cxr), (scheme complex) -- which
     * live in the prelude like base does. */
    { "boolean=?", "r7rs-boolean=?" },
    { "symbol=?", "r7rs-symbol=?" },
    { "char<=?", "r7rs-char<=?" },
    { "char>=?", "r7rs-char>=?" },
    { "list-set!", "r7rs-list-set!" },
    { "make-list", "r7rs-make-list" },
    { "make-string", "r7rs-make-string" },
    { "string", "r7rs-string" },
    { "string->utf8", "r7rs-string->utf8" },
    { "utf8->string", "r7rs-utf8->string" },
    { "string->vector", "r7rs-string->vector" },
    { "vector->string", "r7rs-vector->string" },
    { "string-for-each", "r7rs-string-for-each" },
    { "string-map", "r7rs-string-map" },
    { "string<=?", "r7rs-string<=?" },
    { "string>=?", "r7rs-string>=?" },
    { "string>?", "r7rs-string>?" },
    { "vector-append", "r7rs-vector-append" },
    { "vector-copy", "r7rs-vector-copy" },
    { "vector-copy!", "r7rs-vector-copy!" },
    { "vector-for-each", "r7rs-vector-for-each" },
    { "vector-map", "r7rs-vector-map" },
    { "bytevector-append", "r7rs-bytevector-append" },
    { "bytevector-copy", "r7rs-bytevector-copy" },
    { "bytevector-copy!", "r7rs-bytevector-copy!" },
    { "features", "r7rs-features" },
    { "rationalize", "r7rs-rationalize" },
    { "write-simple", "r7rs-write-simple" },
    { "char-ci<=?", "r7rs-char-ci<=?" },
    { "char-ci<?", "r7rs-char-ci<?" },
    { "char-ci=?", "r7rs-char-ci=?" },
    { "char-ci>=?", "r7rs-char-ci>=?" },
    { "char-ci>?", "r7rs-char-ci>?" },
    { "char-foldcase", "r7rs-char-foldcase" },
    { "char-lower-case?", "r7rs-char-lower-case?" },
    { "char-upper-case?", "r7rs-char-upper-case?" },
    { "digit-value", "r7rs-digit-value" },
    { "string-ci<=?", "r7rs-string-ci<=?" },
    { "string-ci<?", "r7rs-string-ci<?" },
    { "string-ci=?", "r7rs-string-ci=?" },
    { "string-ci>=?", "r7rs-string-ci>=?" },
    { "string-ci>?", "r7rs-string-ci>?" },
    { "string-downcase", "r7rs-string-downcase" },
    { "string-foldcase", "r7rs-string-foldcase" },
    { "string-upcase", "r7rs-string-upcase" },
    { "angle", "r7rs-angle" },
    { "imag-part", "r7rs-imag-part" },
    { "magnitude", "r7rs-magnitude" },
    { "make-polar", "r7rs-make-polar" },
    { "make-rectangular", "r7rs-make-rectangular" },
    { "real-part", "r7rs-real-part" },
    { "caaar", "r7rs-caaar" },
    { "caadr", "r7rs-caadr" },
    { "cadar", "r7rs-cadar" },
    { "cdaar", "r7rs-cdaar" },
    { "cdadr", "r7rs-cdadr" },
    { "cddar", "r7rs-cddar" },
    { "cdddr", "r7rs-cdddr" },
    { "caaaar", "r7rs-caaaar" },
    { "caaadr", "r7rs-caaadr" },
    { "caadar", "r7rs-caadar" },
    { "caaddr", "r7rs-caaddr" },
    { "cadaar", "r7rs-cadaar" },
    { "cadadr", "r7rs-cadadr" },
    { "caddar", "r7rs-caddar" },
    { "cadddr", "r7rs-cadddr" },
    { "cdaaar", "r7rs-cdaaar" },
    { "cdaadr", "r7rs-cdaadr" },
    { "cdadar", "r7rs-cdadar" },
    { "cdaddr", "r7rs-cdaddr" },
    { "cddaar", "r7rs-cddaar" },
    { "cddadr", "r7rs-cddadr" },
    { "cdddar", "r7rs-cdddar" },
    { "cddddr", "r7rs-cddddr" },
    /* R8: ports (display, write, newline and write-simple are above). */
    { "current-input-port", "r7rs-current-input-port" },
    { "current-output-port", "r7rs-current-output-port" },
    { "current-error-port", "r7rs-current-error-port" },
    { "port?", "r7rs-port?" },
    { "input-port?", "r7rs-input-port?" },
    { "output-port?", "r7rs-output-port?" },
    { "textual-port?", "r7rs-textual-port?" },
    { "binary-port?", "r7rs-binary-port?" },
    { "input-port-open?", "r7rs-input-port-open?" },
    { "output-port-open?", "r7rs-output-port-open?" },
    { "close-port", "r7rs-close-port" },
    { "close-input-port", "r7rs-close-input-port" },
    { "close-output-port", "r7rs-close-output-port" },
    { "call-with-port", "r7rs-call-with-port" },
    { "open-input-string", "r7rs-open-input-string" },
    { "open-output-string", "r7rs-open-output-string" },
    { "get-output-string", "r7rs-get-output-string" },
    { "open-input-bytevector", "r7rs-open-input-bytevector" },
    { "open-output-bytevector", "r7rs-open-output-bytevector" },
    { "get-output-bytevector", "r7rs-get-output-bytevector" },
    { "read-char", "r7rs-read-char" },
    { "peek-char", "r7rs-peek-char" },
    { "read-line", "r7rs-read-line" },
    { "read-string", "r7rs-read-string" },
    { "read-u8", "r7rs-read-u8" },
    { "peek-u8", "r7rs-peek-u8" },
    { "char-ready?", "r7rs-char-ready?" },
    { "u8-ready?", "r7rs-u8-ready?" },
    { "read-bytevector", "r7rs-read-bytevector" },
    { "read-bytevector!", "r7rs-read-bytevector!" },
    { "write-char", "r7rs-write-char" },
    { "write-string", "r7rs-write-string" },
    { "write-u8", "r7rs-write-u8" },
    { "write-bytevector", "r7rs-write-bytevector" },
    { "flush-output-port", "r7rs-flush-output-port" },
    { "write-shared", "r7rs-write-shared" },
    /* R6: control.  `guard`, `parameterize`, `delay` and `delay-force` are
     * forms (lower_guard / lower_parameterize / lower_delay); these are the
     * procedures. */
    { "call/cc",                        "r7rs-call/cc" },
    { "call-with-current-continuation", "r7rs-call/cc" },
    { "dynamic-wind",                   "r7rs-dynamic-wind" },
    { "with-exception-handler",         "r7rs-with-exception-handler" },
    { "raise",                          "r7rs-raise" },
    { "raise-continuable",              "r7rs-raise-continuable" },
    { "error-object?",                  "r7rs-error-object?" },
    { "error-object-message",           "r7rs-error-object-message" },
    { "error-object-irritants",         "r7rs-error-object-irritants" },
    { "read-error?",                    "r7rs-read-error?" },
    { "file-error?",                    "r7rs-file-error?" },
    { "make-parameter",                 "r7rs-make-parameter" },
    { "make-promise",                   "r7rs-make-promise" },
    { "promise?",                       "r7rs-promise?" },
    { "force",                          "r7rs-force" },
    /* R5: numbers.  The operators + - * / = < > <= >= are not rows: in call
     * position the lowering folds them onto the binary helpers, and in value
     * position it names the variadic procedures (lower_operator / sl->ops). */
    { "exact?", "r7rs-exact?" },
    { "inexact?", "r7rs-inexact?" },
    { "exact-integer?", "r7rs-exact-integer?" },
    { "exact-rational?", "r7rs-exact-rational?" },
    { "nan?", "r7rs-nan?" },
    { "infinite?", "r7rs-infinite?" },
    { "finite?", "r7rs-finite?" },
    { "rational?", "r7rs-rational?" },
    { "complex?", "r7rs-complex?" },
    { "exact", "r7rs-exact" },
    { "inexact", "r7rs-inexact" },
    { "exact->inexact", "r7rs-exact->inexact" },
    { "inexact->exact", "r7rs-inexact->exact" },
    { "floor", "r7rs-floor" },
    { "ceiling", "r7rs-ceiling" },
    { "round", "r7rs-round" },
    { "truncate", "r7rs-truncate" },
    { "quotient", "r7rs-quotient" },
    { "remainder", "r7rs-remainder" },
    { "modulo", "r7rs-modulo" },
    { "floor/", "r7rs-floor/" },
    { "truncate/", "r7rs-truncate/" },
    { "floor-quotient", "r7rs-floor-quotient" },
    { "floor-remainder", "r7rs-floor-remainder" },
    { "truncate-quotient", "r7rs-truncate-quotient" },
    { "truncate-remainder", "r7rs-truncate-remainder" },
    { "gcd", "r7rs-gcd" },
    { "lcm", "r7rs-lcm" },
    { "expt", "r7rs-expt" },
    { "exp", "r7rs-exp" },
    { "log", "r7rs-log" },
    { "sin", "r7rs-sin" },
    { "cos", "r7rs-cos" },
    { "tan", "r7rs-tan" },
    { "asin", "r7rs-asin" },
    { "acos", "r7rs-acos" },
    { "atan", "r7rs-atan" },
    { "sqrt", "r7rs-sqrt" },
    { "exact-integer-sqrt", "r7rs-exact-integer-sqrt" },
    { "square", "r7rs-square" },
    { "string->number", "r7rs-string->number" },
    { "numerator", "r7rs-numerator" },
    { "denominator", "r7rs-denominator" },
};
#define N_RENAMES (sizeof(RENAMES) / sizeof(RENAMES[0]))

/* R7: the R7RS-small libraries.  A RESIDENT library's procedures live in the
 * prelude, so importing it is a scoping statement only.  An ON-DEMAND
 * library is its own file under stdlib/r7rs/, spliced in by the load
 * expander when a Scheme file imports it (scheme_import_library_files), so a
 * program that does not import (scheme time) carries none of it; its names
 * rename only once it is imported.  A DEFERRED library is refused at the
 * import with the reason. */
enum { LIB_RESIDENT, LIB_ONDEMAND, LIB_DEFERRED };
static const struct { const char *name; int kind; const char *what; } SCHEME_LIBS[] = {
    { "base",            LIB_RESIDENT, NULL },
    { "case-lambda",     LIB_RESIDENT, NULL },
    { "char",            LIB_RESIDENT, NULL },
    { "complex",         LIB_RESIDENT, NULL },
    { "cxr",             LIB_RESIDENT, NULL },
    { "inexact",         LIB_RESIDENT, NULL },
    { "lazy",            LIB_RESIDENT, NULL },
    { "write",           LIB_RESIDENT, NULL },
    { "time",            LIB_ONDEMAND, "stdlib/r7rs/time.tur" },
    { "process-context", LIB_ONDEMAND, "stdlib/r7rs/process-context.tur" },
    { "file",            LIB_ONDEMAND, "stdlib/r7rs/file.tur" },
    { "eval",            LIB_DEFERRED, "needs an evaluator at run time (r7rs-lang-plan Section 8, question 3)" },
    { "repl",            LIB_DEFERRED, "needs an evaluator at run time (r7rs-lang-plan Section 8, question 3)" },
    { "load",            LIB_DEFERRED, "needs an evaluator at run time (r7rs-lang-plan Section 8, question 3)" },
    { "read",            LIB_ONDEMAND, "stdlib/r7rs/read.tur" },
};
#define N_SCHEME_LIBS (sizeof(SCHEME_LIBS) / sizeof(SCHEME_LIBS[0]))
/* The procedures of the on-demand libraries: Scheme name, prelude-style
 * target, library. */
static const char *const ONDEMAND[][3] = {
    { "current-second",            "r7rs-current-second",            "time" },
    { "current-jiffy",             "r7rs-current-jiffy",             "time" },
    { "jiffies-per-second",        "r7rs-jiffies-per-second",        "time" },
    { "command-line",              "r7rs-command-line",              "process-context" },
    { "exit",                      "r7rs-exit",                      "process-context" },
    { "emergency-exit",            "r7rs-emergency-exit",            "process-context" },
    { "get-environment-variable",  "r7rs-get-environment-variable",  "process-context" },
    { "get-environment-variables", "r7rs-get-environment-variables", "process-context" },
    { "file-exists?",              "r7rs-file-exists?",              "file" },
    { "delete-file",               "r7rs-delete-file",               "file" },
    { "open-input-file", "r7rs-open-input-file", "file" },
    { "open-output-file", "r7rs-open-output-file", "file" },
    { "open-binary-input-file", "r7rs-open-binary-input-file", "file" },
    { "open-binary-output-file", "r7rs-open-binary-output-file", "file" },
    { "call-with-input-file", "r7rs-call-with-input-file", "file" },
    { "call-with-output-file", "r7rs-call-with-output-file", "file" },
    { "with-input-from-file", "r7rs-with-input-from-file", "file" },
    { "with-output-to-file", "r7rs-with-output-to-file", "file" },
    { "read", "r7rs-read", "read" },
};
#define N_ONDEMAND (sizeof(ONDEMAND) / sizeof(ONDEMAND[0]))

/* The SCHEME_LIBS row of a `(scheme <x>)` library name, or -1. */
static int scheme_lib_index(const Form *set) {
    if (!set || set->tag != F_LIST || set->as.list.len != 2) return -1;
    const Form *h = set->as.list.items[0], *t = set->as.list.items[1];
    if (h->tag != F_SYM || t->tag != F_SYM || strcmp(h->as.sym->name, "scheme") != 0) return -1;
    for (size_t i = 0; i < N_SCHEME_LIBS; i++)
        if (strcmp(t->as.sym->name, SCHEME_LIBS[i].name) == 0) return (int)i;
    return -1;
}
static bool is_scheme_libname(const Form *set) {
    return set && set->tag == F_LIST && set->as.list.len >= 1 && set->as.list.items[0]->tag == F_SYM &&
           strcmp(set->as.list.items[0]->as.sym->name, "scheme") == 0;
}

/* A growable item buffer for building lists. */
typedef struct FB { Form **items; uint32_t n, cap; } FB;

/* R10 (hygiene): one lexical scope of the user's LOCAL bindings -- the
 * source name and the unique name the lowering gives it.  Frames chain to
 * their parent and live in the arena; a macro keeps the frame it was defined
 * in, so its template's free identifiers can be resolved there (R7RS 4.3.2,
 * referential transparency).  A body's frame is filled after the macros at
 * its start are defined, so they see the body's own defines. */
typedef struct LFrame {
    struct LFrame  *parent;
    const Symbol  **src, **uq;
    uint32_t        n, cap;
} LFrame;

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
    /* R10: a program's top-level define whose name an auto-loaded Turmeric
     * stdlib module also defines (`list-length`, from tur/list) -- the
     * program's name is spelled `<name>--user` throughout, so the two do not
     * collide at C level ("already defined by an auto-loaded stdlib
     * module").  The stdlib name is not in the Scheme namespace to begin
     * with; the program's definition is the only one it sees. */
    const Symbol **clash_from, **clash_to;
    uint32_t n_clash, cap_clash;
    bool in_user;   /* lowering the user's forms, not the prelude's */
    /* R10 (hygiene): the innermost lexical scope, and the identifiers a
     * template inserted that must mean their GLOBAL (or keyword) binding
     * although the use site binds the same name locally: alias -> name. */
    LFrame        *scope;
    const Symbol **ga_from, **ga_to; uint32_t n_ga, cap_ga;
    /* R10: literals and pattern variables of a `syntax-rules` a template
     * inserts are renamed like binders; a literal still MATCHES by its
     * original name (free-identifier=?): renamed -> original. */
    const Symbol **lit_from, **lit_to; uint32_t n_lit, cap_lit;
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
    /* R4: the syntax-rules macros in scope, innermost last.  A body or a
     * let-syntax records n_macros on entry and restores it on exit; lookup
     * walks from the end so an inner definition shadows an outer one. */
    struct SMacro **macros;
    uint32_t        n_macros, cap_macros;
    /* R4: macros whose templates set! a pattern variable (see collect_muts). */
    const Symbol  **setters;
    uint32_t        n_setters, cap_setters;
    uint32_t        expand_depth;
    const Symbol   *s_syntax_rules, *s_ellipsis, *s_underscore, *s_syntax_error,
                   *s_quote, *s_er_macro_transformer;
    /* R6: the control forms. */
    const Symbol   *s_guard, *s_parameterize, *s_delay, *s_delay_force;
    /* R7: which SCHEME_LIBS rows this unit has imported. */
    bool            lib_imported[N_SCHEME_LIBS];
    const Symbol   *od_from[N_ONDEMAND], *od_to[N_ONDEMAND];
    int             od_lib[N_ONDEMAND];
    /* R5: the nine numeric operators -- the Scheme spelling, the binary
     * prelude helper a call folds onto, and the variadic prelude procedure a
     * bare operator in value position names. */
    const Symbol   *ops[9], *ops_bin[9], *ops_val[9];
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
    sl->s_syntax_rules = I(sl, "syntax-rules");
    sl->s_ellipsis = I(sl, "...");
    sl->s_underscore = I(sl, "_");
    sl->s_syntax_error = I(sl, "syntax-error");
    sl->s_quote = I(sl, "quote");
    sl->s_er_macro_transformer = I(sl, "er-macro-transformer");
    sl->s_guard = I(sl, "guard");
    sl->s_parameterize = I(sl, "parameterize");
    sl->s_delay = I(sl, "delay");
    sl->s_delay_force = I(sl, "delay-force");
    {
        static const char *const OPS[9][3] = {
            { "+",  "r7rs-add2__",   "r7rs-+"  }, { "-",  "r7rs-sub2__",   "r7rs--"  },
            { "*",  "r7rs-mul2__",   "r7rs-*"  }, { "/",  "r7rs-div2__",   "r7rs-/"  },
            { "=",  "r7rs-numeq2__", "r7rs-="  }, { "<",  "r7rs-lt2__",    "r7rs-<"  },
            { ">",  "r7rs-gt2__",    "r7rs->"  }, { "<=", "r7rs-le2__",    "r7rs-<=" },
            { ">=", "r7rs-ge2__",    "r7rs->=" },
        };
        for (int i = 0; i < 9; i++) {
            sl->ops[i] = I(sl, OPS[i][0]);
            sl->ops_bin[i] = I(sl, OPS[i][1]);
            sl->ops_val[i] = I(sl, OPS[i][2]);
        }
    }

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

    for (size_t i = 0; i < N_ONDEMAND; i++) {
        sl->od_from[i] = I(sl, ONDEMAND[i][0]);
        sl->od_to[i]   = I(sl, ONDEMAND[i][1]);
        sl->od_lib[i]  = -1;
        for (size_t j = 0; j < N_SCHEME_LIBS; j++)
            if (strcmp(SCHEME_LIBS[j].name, ONDEMAND[i][2]) == 0) sl->od_lib[i] = (int)j;
    }
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

/* The reader spells `()` as F_NIL; a binding list, formals list or do-spec
 * list written empty (or produced empty by a syntax-rules template) is an
 * empty list to the forms below. */
static Form *nil_to_list(SL *sl, Form *f) {
    if (f && f->tag == F_NIL) return List(sl, f->span, NULL, 0);
    return f;
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
/* R4: the mutability scan runs BEFORE expansion (a top-level `define` is
 * lowered as `def` or `def ^mut` before any later form is looked at), so a
 * `set!` a template performs has to be accounted for syntactically.  A
 * `(set! v ...)` in a syntax-rules template whose target is one of the rule's
 * pattern variables marks the macro as a SETTER of its arguments: every
 * symbol among a use's arguments is then a set! target (per-name, so it is
 * the same over-approximation the scan already makes); a template that sets
 * a name of its own (`(set! counter ...)`) marks that name directly.  Scoping
 * is ignored here on purpose -- an over-approximation by name is safe. */
static void note_setter_macro(SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->n_setters; i++) if (sl->setters[i] == s) return;
    if (sl->n_setters == sl->cap_setters) {
        sl->cap_setters = sl->cap_setters ? sl->cap_setters * 2 : 8;
        sl->setters = (const Symbol **)realloc((void *)sl->setters,
                                               sl->cap_setters * sizeof(const Symbol *));
        if (!sl->setters) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    sl->setters[sl->n_setters++] = s;
}
static bool is_setter_macro(const SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->n_setters; i++) if (sl->setters[i] == s) return true;
    return false;
}
static bool form_mentions_sym(const Form *f, const Symbol *s) {
    if (!f) return false;
    if (f->tag == F_SYM) return f->as.sym == s;
    if (f->tag == F_LIST || f->tag == F_VEC)
        for (uint32_t i = 0; i < f->as.list.len; i++)
            if (form_mentions_sym(f->as.list.items[i], s)) return true;
    return false;
}
static void note_all_syms(SL *sl, const Form *f) {
    if (!f) return;
    if (f->tag == F_SYM) { note_mut(sl, f->as.sym); return; }
    if (f->tag == F_QUOTE) return;
    if (f->tag == F_LIST || f->tag == F_VEC)
        for (uint32_t i = 0; i < f->as.list.len; i++) note_all_syms(sl, f->as.list.items[i]);
}
/* Walk a template for set! forms; `pattern` is the rule's pattern. */
static void scan_template_sets(SL *sl, const Symbol *macro, const Form *pattern, const Form *t) {
    if (!t) return;
    if (t->tag == F_QUOTE) return;
    if (t->tag != F_LIST && t->tag != F_VEC) return;
    if (t->tag == F_LIST && t->as.list.len == 3 && is_sym(t->as.list.items[0], sl->s_set) &&
        t->as.list.items[1]->tag == F_SYM) {
        const Symbol *tgt = t->as.list.items[1]->as.sym;
        if (form_mentions_sym(pattern, tgt)) note_setter_macro(sl, macro);
        else note_mut(sl, tgt);
    }
    for (uint32_t i = 0; i < t->as.list.len; i++) scan_template_sets(sl, macro, pattern, t->as.list.items[i]);
}
static void scan_syntax_rules(SL *sl, const Symbol *macro, const Form *spec) {
    if (!head_is(spec, sl->s_syntax_rules)) return;
    for (uint32_t i = 1; i < spec->as.list.len; i++) {
        const Form *rule = spec->as.list.items[i];
        if (rule->tag == F_LIST && rule->as.list.len == 2 && rule->as.list.items[0]->tag == F_LIST)
            scan_template_sets(sl, macro, rule->as.list.items[0], rule->as.list.items[1]);
    }
}
static void collect_setter_macros(SL *sl, const Form *f) {
    if (!f || (f->tag != F_LIST && f->tag != F_VEC)) return;
    if (head_is(f, sl->s_define_syntax) && f->as.list.len == 3 && f->as.list.items[1]->tag == F_SYM)
        scan_syntax_rules(sl, f->as.list.items[1]->as.sym, f->as.list.items[2]);
    else if ((head_is(f, sl->s_let_syntax) || head_is(f, sl->s_letrec_syntax)) &&
             f->as.list.len >= 2 && f->as.list.items[1]->tag == F_LIST) {
        const Form *bl = f->as.list.items[1];
        for (uint32_t i = 0; i < bl->as.list.len; i++) {
            const Form *b = bl->as.list.items[i];
            if (b->tag == F_LIST && b->as.list.len == 2 && b->as.list.items[0]->tag == F_SYM)
                scan_syntax_rules(sl, b->as.list.items[0]->as.sym, b->as.list.items[1]);
        }
    }
    for (uint32_t i = 0; i < f->as.list.len; i++) collect_setter_macros(sl, f->as.list.items[i]);
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
            if (f->tag == F_LIST && f->as.list.len >= 2 && f->as.list.items[0]->tag == F_SYM &&
                is_setter_macro(sl, f->as.list.items[0]->as.sym))
                for (uint32_t i = 1; i < f->as.list.len; i++) note_all_syms(sl, f->as.list.items[i]);
            for (uint32_t i = 0; i < f->as.list.len; i++)
                collect_muts(sl, f->as.list.items[i]);
            return;
        default: return;
    }
}

/* --- renaming -------------------------------------------------------------- */

/* ---- R10: lexical scope (hygiene) ------------------------------------------ */

static const Symbol *scope_lookup(const LFrame *f, const Symbol *s) {
    for (; f; f = f->parent)
        for (int32_t i = (int32_t)f->n - 1; i >= 0; i--)
            if (f->src[i] == s) return f->uq[i];
    return NULL;
}
/* Open a scope; returns the one to restore with scope_close. */
static LFrame *scope_open(SL *sl) {
    LFrame *saved = sl->scope;
    LFrame *f = (LFrame *)arena_alloc(sl->a, sizeof(LFrame));
    memset(f, 0, sizeof *f);
    f->parent = saved;
    sl->scope = f;
    return saved;
}
static void scope_close(SL *sl, LFrame *saved) { sl->scope = saved; }
static const Symbol *global_alias_orig(const SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->n_ga; i++) if (sl->ga_from[i] == s) return sl->ga_to[i];
    return NULL;
}
static const Symbol *lit_orig(const SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->n_lit; i++) if (sl->lit_from[i] == s) return sl->lit_to[i];
    return s;
}
static void sym_pair_push(SL *sl, const Symbol ***from, const Symbol ***to, uint32_t *n, uint32_t *cap,
                          const Symbol *a, const Symbol *b) {
    if (*n == *cap) {
        uint32_t nc = *cap ? *cap * 2 : 16;
        const Symbol **nf = (const Symbol **)arena_alloc(sl->a, nc * sizeof(Symbol *));
        const Symbol **nt = (const Symbol **)arena_alloc(sl->a, nc * sizeof(Symbol *));
        if (*n) { memcpy((void *)nf, (void *)*from, *n * sizeof(Symbol *)); memcpy((void *)nt, (void *)*to, *n * sizeof(Symbol *)); }
        *from = nf; *to = nt; *cap = nc;
    }
    (*from)[*n] = a; (*to)[*n] = b; (*n)++;
}
static bool prelude_span(Span sp);
/* Declare a local binder in the innermost scope: its unique name.  The
 * prelude is written against its own names and is never renamed. */
static const Symbol *bind_name(SL *sl, const Symbol *s, Span sp) {
    if (!sl->scope || prelude_span(sp)) return s;
    char pre[160];
    snprintf(pre, sizeof pre, "%s__v", s->name);
    const Symbol *u = fresh(sl, pre);
    sym_pair_push(sl, &sl->scope->src, &sl->scope->uq, &sl->scope->n, &sl->scope->cap, s, u);
    return u;
}
static const Symbol *rn_global(SL *sl, const Symbol *s);
static const Symbol *rn(SL *sl, const Symbol *s) {
    const Symbol *g = global_alias_orig(sl, s);
    if (g) return rn_global(sl, g);
    if (sl->scope) {
        const Symbol *u = scope_lookup(sl->scope, s);
        if (u) return u;
    }
    return rn_global(sl, s);
}
/* A keyword test that respects scope: `f` is the identifier `kw` and not a
 * local variable that shadows it (`(let ((=> #f)) (cond (#t => 'ok)))`), or
 * a template's alias of it. */
static bool kw_is(SL *sl, const Form *f, const Symbol *kw) {
    if (!f || f->tag != F_SYM) return false;
    const Symbol *g = global_alias_orig(sl, f->as.sym);
    if (g) return g == kw;
    return f->as.sym == kw && !(sl->scope && scope_lookup(sl->scope, kw));
}

static const Symbol *rn_global(SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->n_renames; i++)
        if (sl->renames[i].from == s) return sl->renames[i].to;
    if (sl->in_user)
        for (uint32_t i = 0; i < sl->n_clash; i++)
            if (sl->clash_from[i] == s) return sl->clash_to[i];
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
    /* R7: an on-demand library's name means its procedure only in a unit
     * that imported the library (the library file is not loaded otherwise,
     * and the name stays free for the program's own use). */
    for (size_t i = 0; i < N_ONDEMAND; i++)
        if (sl->od_from[i] == s && sl->od_lib[i] >= 0 && sl->lib_imported[sl->od_lib[i]])
            return sl->od_to[i];
    return s;
}

static bool is_char_form(SL *sl, const Form *f);
static Form *lower_body(SL *sl, Form **items, uint32_t n, Span sp);
static Form *lower(SL *sl, Form *f);

/* --- R4: syntax-rules ------------------------------------------------------ */
/*
 * A `syntax-rules` transformer is a pattern matcher plus a template
 * instantiator over forms, and it lives HERE, in the Form -> Form lowering,
 * rather than on the `defmacro*` / `Syntax` substrate D5 named.  The reason
 * is ordering, not taste: a `defmacro*` runs inside elaboration, AFTER this
 * pass, and its output would be Scheme forms (`let`, `cond`, a named `let`)
 * that nothing would lower.  An expansion has to happen where the lowering
 * can still see it, so the expander is part of the lowering and every
 * expansion is lowered on the spot.  D5's hygiene verdict stands: (a)
 * renaming -- every identifier a template introduces that lands in a
 * BINDING position is renamed to a fresh symbol, consistently across that
 * expansion, so it cannot capture a use-site name; a free identifier the
 * template introduces keeps its name and means what it means at the
 * definition site, which in one flat namespace is right until the use site
 * shadows it (the referential-transparency gap the plan asks for a named
 * failing test of: tests/fixtures/r7rs-syntax-rules-referential-transparency).
 *
 * Patterns: `_`, literals (matched by name), pattern variables, `...` at any
 * depth including after a subpattern with its own ellipsis, elements after
 * an ellipsis (`(_ a ... b c)`), improper tails (`(_ a . rest)`), vectors,
 * and datum literals (numbers, strings, booleans, chars).  A custom ellipsis
 * (`(syntax-rules ::: (lits) ...)`) and the `(... ...)` escape are honoured.
 * Templates: substitution, `x ...` and `x ... ...`, dotted tails that splice
 * a substituted list, vectors, and `syntax-error`.  Macros scope lexically
 * through `define-syntax` (top level or body start), `let-syntax` and
 * `letrec-syntax` (both letrec-scoped here, since expansion is lazy).
 */

typedef struct SRule { Form *pattern, *template; } SRule;
typedef struct SMacro {
    const Symbol  *name;
    const Symbol  *ellipsis;
    const Symbol **literals; uint32_t n_literals;
    SRule         *rules;    uint32_t n_rules;
    LFrame        *def_scope;   /* R10: where its template's names mean what they mean */
} SMacro;

/* A pattern-variable binding: depth 0 holds the matched form; depth d > 0
 * holds an F_LIST whose items are depth d-1 values, one per iteration of
 * the ellipsis that produced it. */
typedef struct MBind { const Symbol *var; uint32_t depth; Form *val; } MBind;
typedef struct MEnv  { MBind *items; uint32_t n, cap; } MEnv;
/* A symbol buffer (pattern variables with their static depths, binders). */
typedef struct SB { const Symbol **syms; uint32_t *depths; uint32_t n, cap; } SB;

#define SR_MAX_DEPTH 1000

static void sb_push(SB *b, const Symbol *s, uint32_t d) {
    for (uint32_t i = 0; i < b->n; i++) if (b->syms[i] == s) return;
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8;
        b->syms = (const Symbol **)realloc((void *)b->syms, b->cap * sizeof(*b->syms));
        b->depths = (uint32_t *)realloc(b->depths, b->cap * sizeof(uint32_t));
        if (!b->syms || !b->depths) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    b->syms[b->n] = s; b->depths[b->n] = d; b->n++;
}
static void sb_free(SB *b) { free((void *)b->syms); free(b->depths); b->syms = NULL; b->depths = NULL; b->n = b->cap = 0; }

static void env_push(MEnv *e, const Symbol *var, uint32_t depth, Form *val) {
    if (e->n == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 8;
        e->items = (MBind *)realloc(e->items, e->cap * sizeof(MBind));
        if (!e->items) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    e->items[e->n].var = var; e->items[e->n].depth = depth; e->items[e->n].val = val; e->n++;
}
static MBind *env_lookup(MEnv *e, const Symbol *var) {
    for (int32_t i = (int32_t)e->n - 1; i >= 0; i--) if (e->items[i].var == var) return &e->items[i];
    return NULL;
}
static void env_free(MEnv *e) { free(e->items); e->items = NULL; e->n = e->cap = 0; }

static SMacro *sr_lookup(SL *sl, const Symbol *s) {
    for (int32_t i = (int32_t)sl->n_macros - 1; i >= 0; i--)
        if (sl->macros[i]->name == s) return sl->macros[i];
    return NULL;
}
static void sr_push(SL *sl, SMacro *m) {
    if (sl->n_macros == sl->cap_macros) {
        sl->cap_macros = sl->cap_macros ? sl->cap_macros * 2 : 8;
        sl->macros = (SMacro **)realloc(sl->macros, sl->cap_macros * sizeof(SMacro *));
        if (!sl->macros) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    sl->macros[sl->n_macros++] = m;
}
static bool sr_is_literal(const SMacro *m, const Symbol *s) {
    for (uint32_t i = 0; i < m->n_literals; i++) if (m->literals[i] == s) return true;
    return false;
}
/* R10: a literal has priority over the ellipsis (R7RS 4.3.2): with
 * `(syntax-rules ... (...) ...)` the `...` matches and emits itself. */
static bool sr_is_ellipsis(const SMacro *m, const Form *f) {
    return f && f->tag == F_SYM && f->as.sym == m->ellipsis && !sr_is_literal(m, f->as.sym);
}
/* The items of a list form and its dotted tail (NULL when proper). */
static void sr_parts(SL *sl, Form *f, Form ***items, uint32_t *n, Form **tail) {
    *tail = NULL;
    if (f->tag == F_NIL) { *items = NULL; *n = 0; return; }
    *items = f->as.list.items; *n = f->as.list.len;
    if (*n >= 3 && is_sym((*items)[*n - 2], sl->s_dot)) { *tail = (*items)[*n - 1]; *n -= 2; }
}
static bool sr_is_listy(const Form *f) { return f->tag == F_LIST || f->tag == F_NIL; }

/* Pattern variables of `pat` with their static ellipsis depths. */
static void sr_pattern_vars(SL *sl, const SMacro *m, Form *pat, uint32_t depth, SB *out) {
    switch (pat->tag) {
        case F_SYM:
            if (pat->as.sym == sl->s_underscore || sr_is_ellipsis(m, pat) || sr_is_literal(m, pat->as.sym)) return;
            sb_push(out, pat->as.sym, depth);
            return;
        case F_LIST: case F_VEC: {
            if (is_char_form(sl, pat)) return;
            for (uint32_t i = 0; i < pat->as.list.len; i++) {
                Form *it = pat->as.list.items[i];
                if (is_sym(it, sl->s_dot)) continue;
                uint32_t d = depth;
                if (i + 1 < pat->as.list.len && sr_is_ellipsis(m, pat->as.list.items[i + 1])) d++;
                if (sr_is_ellipsis(m, it)) continue;
                sr_pattern_vars(sl, m, it, d, out);
            }
            return;
        }
        default: return;
    }
}

static bool sr_match(SL *sl, const SMacro *m, Form *pat, Form *form, MEnv *env);

/* Match pattern items (with an optional dotted tail pattern) against form
 * items (with an optional dotted tail).  At most one ellipsis per level. */
static bool sr_match_items(SL *sl, const SMacro *m, Form **pi, uint32_t np, Form *ptail,
                           Form **fi, uint32_t nf, Form *ftail, MEnv *env, Span sp) {
    int32_t e = -1;
    for (uint32_t i = 0; i < np; i++) {
        if (sr_is_ellipsis(m, pi[i])) {
            if (i == 0) { err(pi[i], "an ellipsis must follow a subpattern"); return false; }
            if (e >= 0) { err(pi[i], "only one ellipsis per list level in a pattern"); return false; }
            e = (int32_t)i;
        }
    }
    if (e < 0) {
        if (ptail) {
            if (nf < np) return false;
            for (uint32_t i = 0; i < np; i++) if (!sr_match(sl, m, pi[i], fi[i], env)) return false;
            Form *rest;
            if (nf == np) rest = ftail ? ftail : Nil(sl, sp);
            else {
                FB b = {0};
                for (uint32_t i = np; i < nf; i++) fb_push(&b, fi[i]);
                if (ftail) { fb_push(&b, Sym(sl, sp, sl->s_dot)); fb_push(&b, ftail); }
                rest = fb_list(sl, &b, sp);
            }
            return sr_match(sl, m, ptail, rest, env);
        }
        if (nf != np || ftail) return false;
        for (uint32_t i = 0; i < np; i++) if (!sr_match(sl, m, pi[i], fi[i], env)) return false;
        return true;
    }
    uint32_t npre = (uint32_t)e - 1, npost = np - (uint32_t)e - 1;
    Form *sub = pi[e - 1];
    if (nf < npre + npost) return false;
    if (!ptail && ftail) return false;
    uint32_t cnt = nf - npre - npost;
    for (uint32_t i = 0; i < npre; i++) if (!sr_match(sl, m, pi[i], fi[i], env)) return false;
    MEnv *subs = cnt ? (MEnv *)calloc(cnt, sizeof(MEnv)) : NULL;
    bool ok = true;
    for (uint32_t j = 0; j < cnt && ok; j++)
        if (!sr_match(sl, m, sub, fi[npre + j], &subs[j])) ok = false;
    if (ok) {
        SB vars = {0};
        sr_pattern_vars(sl, m, sub, 0, &vars);
        for (uint32_t v = 0; v < vars.n; v++) {
            FB seq = {0};
            for (uint32_t j = 0; j < cnt; j++) {
                MBind *b = env_lookup(&subs[j], vars.syms[v]);
                fb_push(&seq, b ? b->val : Nil(sl, sp));
            }
            env_push(env, vars.syms[v], vars.depths[v] + 1, fb_list(sl, &seq, sp));
        }
        sb_free(&vars);
    }
    for (uint32_t j = 0; j < cnt; j++) env_free(&subs[j]);
    free(subs);
    if (!ok) return false;
    for (uint32_t i = 0; i < npost; i++)
        if (!sr_match(sl, m, pi[e + 1 + i], fi[nf - npost + i], env)) return false;
    if (ptail) return sr_match(sl, m, ptail, ftail ? ftail : Nil(sl, sp), env);
    return true;
}

static bool sr_match(SL *sl, const SMacro *m, Form *pat, Form *form, MEnv *env) {
    switch (pat->tag) {
        case F_SYM:
            /* A literal first: `_` in the literals list matches only `_`
             * (R7RS 4.3.2), it is not the wildcard. */
            /* An input matches a literal when they name the same thing
             * (free-identifier=?): a literal a template inserted was renamed
             * like a binder, and is compared by its original name. */
            if (sr_is_literal(m, pat->as.sym))
                return form->tag == F_SYM && lit_orig(sl, form->as.sym) == lit_orig(sl, pat->as.sym);
            if (pat->as.sym == sl->s_underscore) return true;
            if (sr_is_ellipsis(m, pat)) { err(pat, "misplaced ellipsis in pattern"); return false; }
            env_push(env, pat->as.sym, 0, form);
            return true;
        case F_NIL: case F_LIST: {
            if (is_char_form(sl, pat)) return form_equal(pat, form);
            if (!sr_is_listy(form)) return false;
            Form **pi, **fi, *ptail, *ftail; uint32_t np, nf;
            sr_parts(sl, pat, &pi, &np, &ptail);
            sr_parts(sl, form, &fi, &nf, &ftail);
            return sr_match_items(sl, m, pi, np, ptail, fi, nf, ftail, env, form->span);
        }
        case F_VEC:
            if (form->tag != F_VEC) return false;
            return sr_match_items(sl, m, pat->as.list.items, pat->as.list.len, NULL,
                                  form->as.list.items, form->as.list.len, NULL, env, form->span);
        default:
            return form_equal(pat, form);
    }
}

/* Template variables bound at depth >= 1 in `env`, i.e. the ones an
 * ellipsis over `t` iterates. */
static void sr_template_vars(SL *sl, const SMacro *m, Form *t, MEnv *env, SB *out) {
    switch (t->tag) {
        case F_SYM: {
            MBind *b = env_lookup(env, t->as.sym);
            if (b && b->depth >= 1) sb_push(out, t->as.sym, b->depth);
            return;
        }
        case F_LIST: case F_VEC:
            if (t->as.list.len == 2 && sr_is_ellipsis(m, t->as.list.items[0])) return;   /* (... ...) */
            for (uint32_t i = 0; i < t->as.list.len; i++) sr_template_vars(sl, m, t->as.list.items[i], env, out);
            return;
        case F_QUOTE: case F_QUASIQUOTE: case F_UNQUOTE: case F_UNQUOTE_SPLICING:
            sr_template_vars(sl, m, t->as.list.items[0], env, out);
            return;
        default: return;
    }
}

static Form *sr_inst(SL *sl, const SMacro *m, Form *t, MEnv *env, Span sp, FB *intro, bool esc);

/* `t ...` (k ellipses): instantiate `t` once per element of the iterated
 * variables, flattening k levels. */
static bool sr_inst_ellipsis(SL *sl, const SMacro *m, Form *t, MEnv *env, uint32_t k,
                             Span sp, FB *intro, FB *out) {
    SB vars = {0};
    sr_template_vars(sl, m, t, env, &vars);
    if (vars.n == 0) { err(t, "no pattern variable in the subtemplate before this ellipsis"); return false; }
    int64_t len = -1;
    for (uint32_t v = 0; v < vars.n; v++) {
        MBind *b = env_lookup(env, vars.syms[v]);
        int64_t l = (int64_t)b->val->as.list.len;
        if (len < 0) len = l;
        else if (l != len) { err(t, "pattern variables under this ellipsis matched different lengths"); sb_free(&vars); return false; }
    }
    bool ok = true;
    for (int64_t j = 0; j < len && ok; j++) {
        MEnv e2 = {0};
        for (uint32_t i = 0; i < env->n; i++) env_push(&e2, env->items[i].var, env->items[i].depth, env->items[i].val);
        for (uint32_t v = 0; v < vars.n; v++) {
            MBind *b = env_lookup(env, vars.syms[v]);
            env_push(&e2, vars.syms[v], b->depth - 1, b->val->as.list.items[j]);
        }
        if (k == 1) {
            Form *r = sr_inst(sl, m, t, &e2, sp, intro, false);
            if (!r) ok = false; else fb_push(out, r);
        } else {
            ok = sr_inst_ellipsis(sl, m, t, &e2, k - 1, sp, intro, out);
        }
        env_free(&e2);
    }
    sb_free(&vars);
    return ok;
}

static Form *sr_inst(SL *sl, const SMacro *m, Form *t, MEnv *env, Span sp, FB *intro, bool esc) {
    switch (t->tag) {
        case F_SYM: {
            if (!esc && sr_is_ellipsis(m, t)) { err(t, "misplaced ellipsis in template"); return NULL; }
            MBind *b = env_lookup(env, t->as.sym);
            if (b) {
                if (b->depth != 0) { err(t, "pattern variable '%s' needs %u more ellipsis(es) here", t->as.sym->name, b->depth); return NULL; }
                return b->val;
            }
            Form *s = Sym(sl, sp, t->as.sym);
            fb_push(intro, s);
            return s;
        }
        case F_NIL: return t;
        case F_LIST: case F_VEC: {
            Form **items, *tail; uint32_t n;
            if (t->tag == F_VEC) { items = t->as.list.items; n = t->as.list.len; tail = NULL; }
            else sr_parts(sl, t, &items, &n, &tail);
            if (!esc && t->tag == F_LIST && n == 2 && !tail && sr_is_ellipsis(m, items[0]))
                return sr_inst(sl, m, items[1], env, sp, intro, true);
            FB out = {0};
            for (uint32_t i = 0; i < n; i++) {
                uint32_t k = 0;
                if (!esc) while (i + 1 + k < n && sr_is_ellipsis(m, items[i + 1 + k])) k++;
                if (k == 0) {
                    Form *r = sr_inst(sl, m, items[i], env, sp, intro, esc);
                    if (!r) { free(out.items); return NULL; }
                    fb_push(&out, r);
                } else {
                    if (!sr_inst_ellipsis(sl, m, items[i], env, k, sp, intro, &out)) { free(out.items); return NULL; }
                    i += k;
                }
            }
            if (tail) {
                Form *tv = sr_inst(sl, m, tail, env, sp, intro, esc);
                if (!tv) { free(out.items); return NULL; }
                if (sr_is_listy(tv)) {
                    /* (a . (b c)) is (a b c): splice a substituted list. */
                    if (tv->tag == F_LIST)
                        for (uint32_t i = 0; i < tv->as.list.len; i++) fb_push(&out, tv->as.list.items[i]);
                } else {
                    fb_push(&out, Sym(sl, sp, sl->s_dot));
                    fb_push(&out, tv);
                }
            }
            if (t->tag == F_VEC) {
                Form *vf = fb_vec(sl, &out, sp);
                vf->fx_prov = t->fx_prov;   /* R7: a template's `#(...)` stays a datum */
                return vf;
            }
            if (out.n == 0) { free(out.items); return Nil(sl, sp); }
            return fb_list(sl, &out, sp);
        }
        case F_QUOTE: case F_QUASIQUOTE: case F_UNQUOTE: case F_UNQUOTE_SPLICING: {
            Form *inner = sr_inst(sl, m, t->as.list.items[0], env, sp, intro, esc);
            if (!inner) return NULL;
            Form *g = form_new(sl->a, t->tag, sp);
            Form **one = (Form **)arena_alloc(sl->a, sizeof(Form *));
            one[0] = inner;
            g->as.list.items = one; g->as.list.len = 1;
            return g;
        }
        default: return t;
    }
}

/* --- hygiene (D5a): rename the introduced binders ---------------------------- */

static bool hyg_introduced(const FB *intro, const Form *f) {
    for (uint32_t i = 0; i < intro->n; i++) if (intro->items[i] == f) return true;
    return false;
}
static void hyg_add(SL *sl, Form *f, const FB *intro, SB *out) {
    if (f && f->tag == F_SYM && f->as.sym != sl->s_dot && hyg_introduced(intro, f)) sb_push(out, f->as.sym, 0);
}
/* A formals spec: a symbol, or a (possibly dotted) list of symbols. */
static void hyg_formals(SL *sl, Form *formals, const FB *intro, SB *out) {
    if (!formals) return;
    if (formals->tag == F_SYM) { hyg_add(sl, formals, intro, out); return; }
    if (formals->tag == F_LIST)
        for (uint32_t i = 0; i < formals->as.list.len; i++) hyg_add(sl, formals->as.list.items[i], intro, out);
}
static void hyg_walk(SL *sl, Form *f, const FB *intro, SB *out, bool quoted);
static void hyg_binding_list(SL *sl, Form *bl, const FB *intro, SB *out, bool formals) {
    if (!bl || bl->tag != F_LIST) return;
    for (uint32_t i = 0; i < bl->as.list.len; i++) {
        Form *b = bl->as.list.items[i];
        if (b->tag != F_LIST || b->as.list.len < 1) continue;
        if (formals) hyg_formals(sl, b->as.list.items[0], intro, out);
        else hyg_add(sl, b->as.list.items[0], intro, out);
    }
}
static void hyg_walk(SL *sl, Form *f, const FB *intro, SB *out, bool quoted) {
    if (!f) return;
    switch (f->tag) {
        case F_QUOTE: return;
        case F_QUASIQUOTE: hyg_walk(sl, f->as.list.items[0], intro, out, true); return;
        case F_UNQUOTE: case F_UNQUOTE_SPLICING: hyg_walk(sl, f->as.list.items[0], intro, out, false); return;
        case F_VEC:
            for (uint32_t i = 0; i < f->as.list.len; i++) hyg_walk(sl, f->as.list.items[i], intro, out, quoted);
            return;
        case F_LIST: break;
        default: return;
    }
    if (quoted || f->as.list.len == 0) {
        for (uint32_t i = 0; i < f->as.list.len; i++) hyg_walk(sl, f->as.list.items[i], intro, out, quoted);
        return;
    }
    Form *h = f->as.list.items[0];
    if (h->tag == F_SYM) {
        const Symbol *s = h->as.sym;
        uint32_t n = f->as.list.len;
        if (s == sl->s_quote || s == sl->s_syntax_rules || s == sl->s_define_syntax ||
            s == sl->s_let_syntax || s == sl->s_letrec_syntax) return;
        if (s == sl->s_lambda && n >= 2) hyg_formals(sl, f->as.list.items[1], intro, out);
        else if (s == sl->s_define && n >= 2) {
            Form *t = f->as.list.items[1];
            if (t->tag == F_SYM) hyg_add(sl, t, intro, out); else hyg_formals(sl, t, intro, out);
        }
        else if (s == sl->s_define_values && n >= 2) hyg_formals(sl, f->as.list.items[1], intro, out);
        else if ((s == sl->s_let || s == sl->s_letstar || s == sl->s_letrec || s == sl->s_letrecstar) && n >= 2) {
            uint32_t bi = 1;
            if (f->as.list.items[1]->tag == F_SYM) { hyg_add(sl, f->as.list.items[1], intro, out); bi = 2; }
            if (bi < n) hyg_binding_list(sl, f->as.list.items[bi], intro, out, false);
        }
        else if (s == sl->s_do && n >= 2) hyg_binding_list(sl, f->as.list.items[1], intro, out, false);
        else if ((s == sl->s_let_values || s == sl->s_letstar_values) && n >= 2)
            hyg_binding_list(sl, f->as.list.items[1], intro, out, true);
        else if (s == sl->s_case_lambda) {
            for (uint32_t i = 1; i < n; i++) {
                Form *cl = f->as.list.items[i];
                if (cl->tag == F_LIST && cl->as.list.len >= 1) hyg_formals(sl, cl->as.list.items[0], intro, out);
            }
        }
        else if (s == sl->s_guard && n >= 2 && f->as.list.items[1]->tag == F_LIST &&
                 f->as.list.items[1]->as.list.len >= 1)
            hyg_add(sl, f->as.list.items[1]->as.list.items[0], intro, out);   /* R10 */
    }
    for (uint32_t i = 0; i < f->as.list.len; i++) hyg_walk(sl, f->as.list.items[i], intro, out, quoted);
}
/* Rename every introduced occurrence of a collected binder, in place:
 * introduced symbol nodes are fresh to this expansion, so no other form
 * shares them. */
static void hyg_rename(SL *sl, Form *f, const FB *intro, const SB *binders, const Symbol **aliases, bool quoted) {
    if (!f) return;
    switch (f->tag) {
        case F_SYM:
            if (quoted || !hyg_introduced(intro, f)) return;
            for (uint32_t i = 0; i < binders->n; i++)
                if (binders->syms[i] == f->as.sym) { f->as.sym = aliases[i]; return; }
            return;
        case F_QUOTE: return;
        case F_QUASIQUOTE: hyg_rename(sl, f->as.list.items[0], intro, binders, aliases, true); return;
        case F_UNQUOTE: case F_UNQUOTE_SPLICING: hyg_rename(sl, f->as.list.items[0], intro, binders, aliases, false); return;
        case F_LIST: case F_VEC:
            if (f->tag == F_LIST && f->as.list.len >= 1 && is_sym(f->as.list.items[0], sl->s_quote)) return;
            for (uint32_t i = 0; i < f->as.list.len; i++) hyg_rename(sl, f->as.list.items[i], intro, binders, aliases, quoted);
            return;
        default: return;
    }
}

/* R10: a `syntax-rules` a template inserts (a macro that defines a macro):
 * its literals and each rule's pattern variables, where the TEMPLATE put
 * them, are binders of that inner transformer -- renamed apart from the
 * user's identifiers that the outer substitution put beside them, so
 * `((bar x) 'y)` with the user's `x` in for `y` quotes the user's `x`
 * rather than substituting the inner pattern variable.  Renamed everywhere
 * in the rule, quotes included: a pattern variable is substituted inside a
 * quote.  `made` collects the fresh names. */
static void hyg_sr_collect(SL *sl, const SMacro *om, Form *p, const FB *intro, const Symbol *ell,
                           const SB *lits, SB *out, bool head) {
    if (!p) return;
    if (p->tag == F_SYM) {
        if (head || !hyg_introduced(intro, p)) return;
        const Symbol *x = p->as.sym;
        if (x == ell || x == sl->s_ellipsis || x == sl->s_underscore || x == sl->s_dot) return;
        for (uint32_t i = 0; i < lits->n; i++) if (lits->syms[i] == x) return;
        sb_push(out, x, 0);
        return;
    }
    (void)om;
    if (p->tag == F_LIST || p->tag == F_VEC)
        for (uint32_t i = 0; i < p->as.list.len; i++)
            hyg_sr_collect(sl, om, p->as.list.items[i], intro, ell, lits, out, false);
}
static void hyg_sr_rename(Form *f, const FB *intro, const SB *from, const Symbol **to) {
    if (!f) return;
    if (f->tag == F_SYM) {
        if (!hyg_introduced(intro, f)) return;
        for (uint32_t i = 0; i < from->n; i++) if (from->syms[i] == f->as.sym) { f->as.sym = to[i]; return; }
        return;
    }
    if (f->tag == F_LIST || f->tag == F_VEC || f->tag == F_QUOTE || f->tag == F_QUASIQUOTE ||
        f->tag == F_UNQUOTE || f->tag == F_UNQUOTE_SPLICING)
        for (uint32_t i = 0; i < f->as.list.len; i++) hyg_sr_rename(f->as.list.items[i], intro, from, to);
}
static void hyg_inner_sr(SL *sl, const SMacro *om, Form *f, const FB *intro, SB *made) {
    if (!f || (f->tag != F_LIST && f->tag != F_VEC)) return;
    if (f->tag == F_LIST && f->as.list.len >= 2 && f->as.list.items[0]->tag == F_SYM &&
        f->as.list.items[0]->as.sym == sl->s_syntax_rules) {
        uint32_t i = 1;
        const Symbol *ell = sl->s_ellipsis;
        if (f->as.list.items[i]->tag == F_SYM) { ell = f->as.list.items[i]->as.sym; i++; }
        if (i >= f->as.list.len) return;
        Form *lits = f->as.list.items[i++];
        SB lit = {0};
        if (lits->tag == F_LIST)
            for (uint32_t j = 0; j < lits->as.list.len; j++) {
                Form *l = lits->as.list.items[j];
                if (l->tag == F_SYM && hyg_introduced(intro, l)) sb_push(&lit, l->as.sym, 0);
            }
        /* The literals: renamed across the whole transformer, and remembered
         * by their original names for matching. */
        if (lit.n) {
            const Symbol **to = (const Symbol **)arena_alloc(sl->a, lit.n * sizeof(Symbol *));
            for (uint32_t j = 0; j < lit.n; j++) {
                char pre[160];
                snprintf(pre, sizeof pre, "%s__l", lit.syms[j]->name);
                to[j] = fresh(sl, pre);
                sym_pair_push(sl, &sl->lit_from, &sl->lit_to, &sl->n_lit, &sl->cap_lit, to[j], lit.syms[j]);
                sb_push(made, to[j], 0);
            }
            hyg_sr_rename(f, intro, &lit, to);
        }
        SB lit_now = {0};
        if (lits->tag == F_LIST)
            for (uint32_t j = 0; j < lits->as.list.len; j++)
                if (lits->as.list.items[j]->tag == F_SYM) sb_push(&lit_now, lits->as.list.items[j]->as.sym, 0);
        for (; i < f->as.list.len; i++) {
            Form *rule = f->as.list.items[i];
            if (rule->tag != F_LIST || rule->as.list.len != 2 || rule->as.list.items[0]->tag != F_LIST) continue;
            Form *pat = rule->as.list.items[0];
            SB vars = {0};
            for (uint32_t j = 0; j < pat->as.list.len; j++)
                hyg_sr_collect(sl, om, pat->as.list.items[j], intro, ell, &lit_now, &vars, j == 0);
            if (vars.n) {
                const Symbol **to = (const Symbol **)arena_alloc(sl->a, vars.n * sizeof(Symbol *));
                for (uint32_t j = 0; j < vars.n; j++) {
                    char pre[160];
                    snprintf(pre, sizeof pre, "%s__p", vars.syms[j]->name);
                    to[j] = fresh(sl, pre);
                    sb_push(made, to[j], 0);
                }
                hyg_sr_rename(rule, intro, &vars, to);
            }
            sb_free(&vars);
        }
        sb_free(&lit);
        sb_free(&lit_now);
        return;
    }
    for (uint32_t i = 0; i < f->as.list.len; i++) hyg_inner_sr(sl, om, f->as.list.items[i], intro, made);
}

/* R10 (referential transparency): a free identifier the template inserted
 * means what it means where the MACRO was defined.  Bound there, it becomes
 * that binding's unique name; unbound there (a global, a keyword) but bound
 * locally at the use site, it becomes an alias that names the global --
 * `(let ((if even?)) (my-or ...))` still expands to the core `if`.  Anything
 * else is already right.  Quoted data is left alone. */
static bool sb_has(const SB *b, const Symbol *s) {
    for (uint32_t i = 0; i < b->n; i++) if (b->syms[i] == s) return true;
    return false;
}
static void hyg_resolve_free(SL *sl, const SMacro *m, Form *f, const FB *intro, const SB *made, bool quoted) {
    if (!f) return;
    switch (f->tag) {
        case F_SYM: {
            if (quoted || !hyg_introduced(intro, f)) return;
            const Symbol *x = f->as.sym;
            if (sb_has(made, x) || x == m->ellipsis || x == sl->s_ellipsis ||
                x == sl->s_underscore || x == sl->s_dot)
                return;
            const Symbol *d = scope_lookup(m->def_scope, x);
            if (d) { f->as.sym = d; return; }
            if (sl->scope && scope_lookup(sl->scope, x)) {
                char pre[160];
                snprintf(pre, sizeof pre, "%s__g", x->name);
                const Symbol *a = fresh(sl, pre);
                sym_pair_push(sl, &sl->ga_from, &sl->ga_to, &sl->n_ga, &sl->cap_ga, a, x);
                f->as.sym = a;
            }
            return;
        }
        case F_QUOTE: return;
        case F_QUASIQUOTE: hyg_resolve_free(sl, m, f->as.list.items[0], intro, made, true); return;
        case F_UNQUOTE: case F_UNQUOTE_SPLICING:
            hyg_resolve_free(sl, m, f->as.list.items[0], intro, made, false);
            return;
        case F_LIST: case F_VEC:
            if (f->tag == F_LIST && f->as.list.len >= 1 && is_sym(f->as.list.items[0], sl->s_quote)) return;
            for (uint32_t i = 0; i < f->as.list.len; i++)
                hyg_resolve_free(sl, m, f->as.list.items[i], intro, made, quoted);
            return;
        default: return;
    }
}

/* One expansion of `use` by `m`, hygienically renamed; NULL after an error. */
static Form *sr_expand(SL *sl, SMacro *m, Form *use) {
    Form **fi, *ftail; uint32_t nf;
    sr_parts(sl, use, &fi, &nf, &ftail);
    for (uint32_t r = 0; r < m->n_rules; r++) {
        Form *pat = m->rules[r].pattern;
        Form **pi, *ptail; uint32_t np;
        sr_parts(sl, pat, &pi, &np, &ptail);
        MEnv env = {0};
        /* The keyword position of the pattern is not matched (R7RS 4.3.2). */
        bool ok = np >= 1 && nf >= 1 &&
                  sr_match_items(sl, m, pi + 1, np - 1, ptail, fi + 1, nf - 1, ftail, &env, use->span);
        if (!ok) { env_free(&env); continue; }
        FB intro = {0};
        Form *x = sr_inst(sl, m, m->rules[r].template, &env, use->span, &intro, false);
        env_free(&env);
        if (!x) { free(intro.items); return NULL; }
        SB made = {0};
        hyg_inner_sr(sl, m, x, &intro, &made);
        SB binders = {0};
        hyg_walk(sl, x, &intro, &binders, false);
        if (binders.n > 0) {
            const Symbol **aliases = (const Symbol **)arena_alloc(sl->a, binders.n * sizeof(const Symbol *));
            for (uint32_t i = 0; i < binders.n; i++) {
                char pre[160];
                snprintf(pre, sizeof pre, "%s__h", binders.syms[i]->name);
                aliases[i] = fresh(sl, pre);
                sb_push(&made, aliases[i], 0);
            }
            hyg_rename(sl, x, &intro, &binders, aliases, false);
        }
        sb_free(&binders);
        hyg_resolve_free(sl, m, x, &intro, &made, false);
        sb_free(&made);
        free(intro.items);
        return x;
    }
    err(use, "no syntax-rules pattern of '%s' matches this form", m->name->name);
    return NULL;
}

/* (define-syntax name (syntax-rules [ellipsis] (literal ...) (pattern template) ...)) */
static void sr_define(SL *sl, Form *f) {
    if (f->as.list.len != 3 || f->as.list.items[1]->tag != F_SYM) {
        err(f, "define-syntax expects (define-syntax name (syntax-rules ...))");
        return;
    }
    const Symbol *name = f->as.list.items[1]->as.sym;
    Form *spec = f->as.list.items[2];
    if (head_is(spec, sl->s_er_macro_transformer)) {
        err(spec, "er-macro-transformer is not supported yet (r7rs-lang-plan R4 defers the low-level "
                  "escape); write the transformer with syntax-rules");
        return;
    }
    if (!head_is(spec, sl->s_syntax_rules)) {
        err(spec, "the transformer of '%s' must be a (syntax-rules ...) form", name->name);
        return;
    }
    SMacro *m = (SMacro *)arena_alloc(sl->a, sizeof(SMacro));
    memset(m, 0, sizeof *m);
    m->name = name;
    m->ellipsis = sl->s_ellipsis;
    m->def_scope = sl->scope;
    uint32_t i = 1;
    if (i < spec->as.list.len && spec->as.list.items[i]->tag == F_SYM) { m->ellipsis = spec->as.list.items[i]->as.sym; i++; }
    if (i >= spec->as.list.len || !sr_is_listy(spec->as.list.items[i])) {
        err(spec, "syntax-rules expects a literals list: (syntax-rules (literal ...) (pattern template) ...)");
        return;
    }
    Form *lits = spec->as.list.items[i++];
    if (lits->tag == F_LIST) {
        m->literals = (const Symbol **)arena_alloc(sl->a, lits->as.list.len * sizeof(const Symbol *));
        for (uint32_t j = 0; j < lits->as.list.len; j++) {
            if (lits->as.list.items[j]->tag != F_SYM) { err(lits->as.list.items[j], "syntax-rules literals must be identifiers"); return; }
            m->literals[m->n_literals++] = lits->as.list.items[j]->as.sym;
        }
    }
    uint32_t nrules = spec->as.list.len - i;
    m->rules = (SRule *)arena_alloc(sl->a, (nrules ? nrules : 1) * sizeof(SRule));
    for (; i < spec->as.list.len; i++) {
        Form *rule = spec->as.list.items[i];
        if (rule->tag != F_LIST || rule->as.list.len != 2 || !sr_is_listy(rule->as.list.items[0]) ||
            rule->as.list.items[0]->tag == F_NIL) {
            err(rule, "a syntax-rules rule is ((_ pattern ...) template)");
            return;
        }
        m->rules[m->n_rules].pattern = rule->as.list.items[0];
        m->rules[m->n_rules].template = rule->as.list.items[1];
        m->n_rules++;
    }
    sr_push(sl, m);
}

/* Expand the head of `f` while it is a macro use.  Returns NULL after an
 * error; otherwise the first form whose head is not a macro. */
static Form *sr_expand_head(SL *sl, Form *f) {
    uint32_t steps = 0;
    for (;;) {
        if (!f || f->tag != F_LIST || f->as.list.len == 0 || f->as.list.items[0]->tag != F_SYM) return f;
        /* R10: a local variable of that name shadows the macro; a template's
         * alias of the macro's name is the macro. */
        const Symbol *hs = f->as.list.items[0]->as.sym;
        const Symbol *g = global_alias_orig(sl, hs);
        if (!g && sl->scope && scope_lookup(sl->scope, hs)) return f;
        SMacro *m = sr_lookup(sl, g ? g : hs);
        if (!m) return f;
        if (++steps > SR_MAX_DEPTH) {
            err(f, "macro expansion of '%s' did not terminate after %d steps", m->name->name, SR_MAX_DEPTH);
            return NULL;
        }
        f = sr_expand(sl, m, f);
        if (!f) return NULL;
    }
}

/* (let-syntax ((name (syntax-rules ...)) ...) body...) -- the body is a
 * body (internal defines allowed) with the macros in scope. */
static Form *lower_let_syntax(SL *sl, Form *f) {
    Span sp = f->span;
    if (f->as.list.len < 3 || !sr_is_listy(f->as.list.items[1])) {
        err(f, "%s expects (%s ((name (syntax-rules ...)) ...) body...)",
            f->as.list.items[0]->as.sym->name, f->as.list.items[0]->as.sym->name);
        return Nil(sl, sp);
    }
    uint32_t mark = sl->n_macros;
    Form *bl = f->as.list.items[1];
    if (bl->tag == F_LIST) {
        for (uint32_t i = 0; i < bl->as.list.len; i++) {
            Form *b = bl->as.list.items[i];
            if (b->tag != F_LIST || b->as.list.len != 2) { err(b, "a let-syntax binding is (name (syntax-rules ...))"); continue; }
            Form *def = Ln(sl, b->span, 3, Sym(sl, b->span, sl->s_define_syntax), b->as.list.items[0], b->as.list.items[1]);
            sr_define(sl, def);
        }
    }
    Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
    sl->n_macros = mark;
    return body;
}

/* --- R5: the numeric operators ------------------------------------------- */
/*
 * R7RS arithmetic differs from Turmeric's operators in four ways the lowering
 * has to bridge: `(+)`, `(*)`, `(- x)` and `(/ x)` are legal; `=`/`<`/... take
 * any number of arguments and chain; a mixed exact/inexact pair promotes even
 * when both are literals (Turmeric's static `(+ 1 7.1)` is TUR-E0042); and
 * exact overflow must signal (D8).  So a call `(op a b c)` folds onto the
 * prelude's binary helper (`(r7rs-add2__ (r7rs-add2__ a b) c)`), which takes
 * the checked path for two exact integers and the promoting dynamic operator
 * otherwise, and a comparison chain binds its arguments once and tests each
 * adjacent pair.  A bare operator in value position names the variadic
 * prelude procedure, so `(apply + xs)` works.
 *
 * The prelude is exempt: it is where the helpers are written in terms of the
 * raw operators, so its `(+ a b)` must stay Turmeric's.
 */
static int op_index(SL *sl, const Symbol *s) {
    for (int i = 0; i < 9; i++) if (sl->ops[i] == s) return i;
    return -1;
}
static bool prelude_span(Span sp) {
    const SourceFile *f = diag_source_file(sp.file_id);
    if (!f || !f->path) return false;
    /* R7: a synthetic source (`<eval>`, the interpreter's stub preamble) is
     * Turmeric whatever the session's language, never Scheme to rewrite --
     * R9: up to the end of the pinned preload.  The REPL compiles that
     * preload and the prompt's input as one `<eval>` text, and the input
     * after it is the user's Scheme (g_synthetic_user_from_line). */
    if (f->path[0] == '<')
        return !(g_synthetic_user_from_line && sp.line >= g_synthetic_user_from_line);
    size_t n = strlen(f->path);
    static const char SUFFIX[] = "r7rs/prelude.tur";
    size_t m = sizeof SUFFIX - 1;
    if (n >= m && memcmp(f->path + n - m, SUFFIX, m) == 0) return true;
    /* R7: the on-demand library files are written in the prelude's own
     * Turmeric shapes and against the typed stdlib's real names. */
    return strstr(f->path, "stdlib/r7rs/") != NULL;
}
static Form *lower_operator(SL *sl, Form *f, int op) {
    Span sp = f->span;
    const Symbol *h = sl->ops[op], *bin = sl->ops_bin[op];
    uint32_t n = f->as.list.len - 1;
    Form **args = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(Form *));
    for (uint32_t i = 0; i < n; i++) args[i] = lower(sl, f->as.list.items[1 + i]);
    if (op < 4) {
        if (n == 0) {
            if (op == 0) return Int(sl, sp, 0);
            if (op == 2) return Int(sl, sp, 1);
            err(f, "%s needs at least one argument", h->name);
            return Nil(sl, sp);
        }
        if (n == 1) {
            if (op == 1) return Ln(sl, sp, 3, Sym(sl, sp, bin), Int(sl, sp, 0), args[0]);
            if (op == 3) return Ln(sl, sp, 3, Sym(sl, sp, bin), Int(sl, sp, 1), args[0]);
            return args[0];
        }
        Form *acc = args[0];
        for (uint32_t i = 1; i < n; i++) acc = Ln(sl, sp, 3, Sym(sl, sp, bin), acc, args[i]);
        return acc;
    }
    if (n == 0) { err(f, "%s needs at least one argument", h->name); return Nil(sl, sp); }
    if (n == 1) return Bool(sl, sp, true);
    if (n == 2) return Ln(sl, sp, 3, Sym(sl, sp, bin), args[0], args[1]);
    /* (let [t0 a t1 b t2 c] (if (op t0 t1) (op t1 t2) #f)) */
    const Symbol **t = (const Symbol **)arena_alloc(sl->a, n * sizeof(*t));
    FB b = {0};
    for (uint32_t i = 0; i < n; i++) {
        t[i] = fresh(sl, "__r7rs_cmp");
        fb_push(&b, Sym(sl, sp, t[i]));
        fb_push(&b, args[i]);
    }
    Form *acc = Ln(sl, sp, 3, Sym(sl, sp, bin), Sym(sl, sp, t[n - 2]), Sym(sl, sp, t[n - 1]));
    for (int32_t i = (int32_t)n - 3; i >= 0; i--)
        acc = Ln(sl, sp, 4, Sym(sl, sp, sl->t_if),
                 Ln(sl, sp, 3, Sym(sl, sp, bin), Sym(sl, sp, t[i]), Sym(sl, sp, t[i + 1])),
                 acc, Bool(sl, sp, false));
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &b, sp), acc);
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
    /* R10: a parameter is a binder -- declared in the innermost scope. */
    fb_push(b, Sym(sl, sp, bind_name(sl, name, sp)));
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
        const Symbol *r = bind_name(sl, formals->as.sym, formals->span);
        fb_push(&b, Sym(sl, formals->span, sl->t_amp));
        fb_push(&b, Sym(sl, formals->span, r));
        fb_push(&b, AnyAnn(sl, formals->span));
        if (out_rest) *out_rest = r;
        return fb_vec(sl, &b, formals->span);
    }
    formals = nil_to_list(sl, formals);
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
            const Symbol *r = bind_name(sl, formals->as.list.items[n - 1]->as.sym, p->span);
            fb_push(&b, Sym(sl, p->span, sl->t_amp));
            fb_push(&b, Sym(sl, p->span, r));
            fb_push(&b, AnyAnn(sl, p->span));
            if (out_rest) *out_rest = r;
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
    LFrame *saved = scope_open(sl);
    Form *params = lower_formals(sl, formals, &ok, &rest);
    if (!ok) { scope_close(sl, saved); return Nil(sl, sp); }
    if (nbody == 0) {
        err(formals, "lambda needs a body");
        scope_close(sl, saved);
        return Nil(sl, sp);
    }
    Form *lowered = lower_body(sl, body, nbody, sp);
    lowered = rebind_rest(sl, sp, rest, lowered);
    lowered = rebind_muts(sl, sp, params, lowered);
    scope_close(sl, saved);
    /* R6 (docs/archive/r7rs-compiled-dynamic-shapes.md section 2): every
     * Scheme procedure returns `any`, spelled out.  An unannotated lambda
     * whose body ends in a nil-returning call (`display`) was typed
     * `(fn [any] nil)`, and the compiled dynamic call -- which checks the
     * box against the all-`any` signature -- refused it. */
    return Ln(sl, sp, 4, Sym(sl, sp, sl->t_fn), params, AnyAnn(sl, sp), lowered);
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
    blist = nil_to_list(sl, blist);
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
        /* R10: the inits are in the OUTER scope; the loop name and the
         * variables are the body's. */
        Form **linits = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(Form *));
        for (uint32_t i = 0; i < n; i++) linits[i] = lower(sl, inits[i]);
        LFrame *saved = scope_open(sl);
        const Symbol *loop = bind_name(sl, second->as.sym, second->span);
        LFrame *vars_saved = scope_open(sl);
        FB params = {0};
        for (uint32_t i = 0; i < n; i++) push_param(sl, &params, sp, names[i]);
        Form *pvec = fb_vec(sl, &params, sp);
        Form *lbody = rebind_muts(sl, sp, pvec,
                                  lower_body(sl, f->as.list.items + 3, len - 3, sp));
        scope_close(sl, vars_saved);
        Form *fn = Ln(sl, sp, 3, Sym(sl, sp, sl->t_fn), pvec, lbody);
        FB call = {0};
        fb_push(&call, Sym(sl, sp, loop));
        for (uint32_t i = 0; i < n; i++) fb_push(&call, linits[i]);
        scope_close(sl, saved);
        Form *bvec[2] = { Sym(sl, sp, loop), fn };
        return Ln(sl, sp, 3, Sym(sl, sp, sl->t_letrec), Vec(sl, sp, bvec, 2),
                  fb_list(sl, &call, sp));
    }

    const Symbol **names; Form **inits; uint32_t n;
    if (!parse_bindings(sl, second, &names, &inits, &n)) return Nil(sl, sp);
    /* R10: every init in the OUTER scope, then the binders -- each a unique
     * name, so Turmeric's sequential `let` keeps Scheme's parallel meaning
     * with no temporaries (an init that names a binder means the outer one,
     * which has another name). */
    Form **linits = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(Form *));
    for (uint32_t i = 0; i < n; i++) linits[i] = lower(sl, inits[i]);
    LFrame *saved = scope_open(sl);
    for (uint32_t i = 0; i < n; i++) bind_name(sl, names[i], sp);
    Form *body = lower_body(sl, f->as.list.items + 2, len - 2, sp);
    Form *r = n == 0 ? body : make_let(sl, sp, names, linits, n, body);
    scope_close(sl, saved);
    return r;
}

/* let*: nested single-binding lets, innermost last. */
static Form *lower_letstar(SL *sl, Form *f) {
    Span sp = f->span;
    if (f->as.list.len < 3) { err(f, "let* expects (let* bindings body...)"); return Nil(sl, sp); }
    if (f->as.list.items[1]->tag == F_VEC) return NULL;   /* Turmeric let* */
    const Symbol **names; Form **inits; uint32_t n;
    if (!parse_bindings(sl, f->as.list.items[1], &names, &inits, &n)) return Nil(sl, sp);
    /* Each init sees the bindings before it: one scope per binding (R10). */
    Form **linits = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(Form *));
    LFrame **saved = (LFrame **)arena_alloc(sl->a, (n + 1) * sizeof(LFrame *));
    for (uint32_t i = 0; i < n; i++) {
        linits[i] = lower(sl, inits[i]);
        saved[i] = scope_open(sl);
        bind_name(sl, names[i], sp);
    }
    Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
    for (int32_t i = (int32_t)n - 1; i >= 0; i--) {
        body = make_let(sl, sp, names + i, linits + i, 1, body);
        scope_close(sl, saved[i]);
    }
    return body;
}

/* R10: does the lowered `f` mention one of `names` other than as a call's
 * head?  A letrec lambda that CALLS a sibling compiles to a direct call; one
 * that takes a sibling as a VALUE (`(eqv? f g)`, `(map g xs)`) captures it,
 * and the compiled letrec filled that capture before the sibling existed
 * (cc: 'f_N' undeclared -- chibi's `(letrec ((f (lambda () (eqv? f g)))
 * ...))`).  Over-approximates (a shadowing parameter counts): the fallback
 * below is correct for every group, only slower. */
static bool mentions_as_value(const Form *f, const Symbol **names, uint32_t n) {
    if (!f) return false;
    if (f->tag == F_SYM) {
        for (uint32_t i = 0; i < n; i++) if (f->as.sym == names[i]) return true;
        return false;
    }
    if (f->tag == F_QUOTE) return false;
    if (f->tag != F_LIST && f->tag != F_VEC) return false;
    for (uint32_t i = 0; i < f->as.list.len; i++) {
        const Form *it = f->as.list.items[i];
        if (i == 0 && f->tag == F_LIST && it->tag == F_SYM) continue;
        if (mentions_as_value(it, names, n)) return true;
    }
    return false;
}
/* A letrec over `pairs` (renamed name, lowered init, alternating; consumed).
 * When some init takes a group member as a value, the group is instead a set
 * of `any` cells assigned in order -- letrec* (R7RS 4.2.2) -- which the
 * assignment conversion then turns into shared boxes. */
static Form *make_letrec(SL *sl, Span sp, FB *pairs, Form *body) {
    uint32_t n = pairs->n / 2;
    const Symbol **names = (const Symbol **)arena_alloc(sl->a, (n + 1) * sizeof(*names));
    for (uint32_t i = 0; i < n; i++) names[i] = pairs->items[2 * i]->as.sym;
    bool as_value = false;
    for (uint32_t i = 0; i < n && !as_value; i++)
        as_value = mentions_as_value(pairs->items[2 * i + 1], names, n);
    if (!as_value) return Ln(sl, sp, 3, Sym(sl, sp, sl->t_letrec), fb_vec(sl, pairs, sp), body);
    FB cells = {0}, seq = {0};
    fb_push(&seq, Sym(sl, sp, sl->t_do));
    for (uint32_t i = 0; i < n; i++) {
        fb_push(&cells, Sym(sl, sp, sl->t_mut));
        fb_push(&cells, Sym(sl, sp, names[i]));
        fb_push(&cells, AnyAnn(sl, sp));
        fb_push(&cells, Bool(sl, sp, false));
        fb_push(&seq, Ln(sl, sp, 3, Sym(sl, sp, sl->s_set), Sym(sl, sp, names[i]), pairs->items[2 * i + 1]));
    }
    free(pairs->items);
    fb_push(&seq, body);
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &cells, sp), fb_list(sl, &seq, sp));
}

/* letrec / letrec*: Turmeric's letrec (a lambda may name any sibling). */
static Form *lower_letrec(SL *sl, Form *f) {
    Span sp = f->span;
    if (f->as.list.len < 3) { err(f, "letrec expects (letrec bindings body...)"); return Nil(sl, sp); }
    if (f->as.list.items[1]->tag == F_VEC) return NULL;   /* Turmeric letrec */
    const Symbol **names; Form **inits; uint32_t n;
    if (!parse_bindings(sl, f->as.list.items[1], &names, &inits, &n)) return Nil(sl, sp);
    LFrame *saved = scope_open(sl);
    for (uint32_t i = 0; i < n; i++) bind_name(sl, names[i], sp);
    FB b = {0};
    for (uint32_t i = 0; i < n; i++) {
        fb_push(&b, Sym(sl, sp, rn(sl, names[i])));
        fb_push(&b, lower(sl, inits[i]));
    }
    Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
    Form *r = n == 0 ? body : make_letrec(sl, sp, &b, body);
    if (n == 0) free(b.items);
    scope_close(sl, saved);
    return r;
}

/* A Scheme `do` loop has the shape (do ((var init step)...) (test res...)
 * cmd...); anything else with that head is Turmeric's `do` sequence. */
static bool looks_like_scheme_do(const Form *f) {
    if (f->as.list.len < 3) return false;
    const Form *specs = f->as.list.items[1];
    const Form *test  = f->as.list.items[2];
    if (specs->tag == F_NIL) return test->tag == F_LIST;
    if (specs->tag != F_LIST || test->tag != F_LIST) return false;
    for (uint32_t i = 0; i < specs->as.list.len; i++)
        if (specs->as.list.items[i]->tag != F_LIST) return false;
    return true;
}

static Form *lower_do(SL *sl, Form *f) {
    Span sp = f->span;
    Form *specs = nil_to_list(sl, f->as.list.items[1]);
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
        inits[i] = lower(sl, s->as.list.items[1]);     /* the OUTER scope (R10) */
    }
    if (test->as.list.len < 1) { err(test, "do needs (test result...)"); return Nil(sl, sp); }
    const Symbol *loop = fresh(sl, "__r7rs_do");
    LFrame *saved = scope_open(sl);
    FB params = {0};
    for (uint32_t i = 0; i < n; i++) push_param(sl, &params, sp, names[i]);
    for (uint32_t i = 0; i < n; i++) {
        Form *s = specs->as.list.items[i];
        steps[i] = (s->as.list.len == 3) ? lower(sl, s->as.list.items[2])
                                         : Sym(sl, sp, rn(sl, names[i]));
    }
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
    scope_close(sl, saved);
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
    if (kw_is(sl, it[0], sl->s_else))
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
    if (kw_is(sl, it[1], sl->s_arrow)) {
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
        if (len >= 3 && kw_is(sl, it[1], sl->s_arrow)) {
            body = Ln(sl, cl->span, 2, lower(sl, it[2]), Sym(sl, cl->span, k));
        } else {
            body = lower_seq(sl, it + 1, len - 1, cl->span);
        }
        if (kw_is(sl, it[0], sl->s_else)) { chain = body; continue; }
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

/* R6: a zero-parameter `(fn [] : any body)`. */
static Form *thunk_of(SL *sl, Span sp, Form *body) {
    return Ln(sl, sp, 4, Sym(sl, sp, sl->t_fn), Vec(sl, sp, NULL, 0), AnyAnn(sl, sp), body);
}

/* R6: (guard (var clause...) body...) -- R7RS 4.2.7, over the escape
 * continuation and the handler stack (prelude r7rs-call/cc,
 * r7rs-with-exception-handler):
 *
 *   ((r7rs-call/cc (fn [k] : any
 *      (r7rs-with-exception-handler
 *        (fn [var] : any (k (fn [] : any (cond clause... (else (raise-continuable var))))))
 *        (fn [] : any (let [v body'] (fn [] : any v)))))))
 *
 * Both arms hand the continuation a THUNK, so the clauses are evaluated after
 * the escape, in the guard's own dynamic environment, and a clause-less
 * exception is re-raised from there with `raise-continuable` -- R7RS asks for
 * the raise's dynamic environment, which needs a re-entrant continuation
 * (D7); with escapes only, this is the reachable reading. */
static Form *lower_guard(SL *sl, Form *f) {
    Span sp = f->span;
    if (f->as.list.len < 3 || f->as.list.items[1]->tag != F_LIST ||
        f->as.list.items[1]->as.list.len < 1 ||
        f->as.list.items[1]->as.list.items[0]->tag != F_SYM) {
        err(f, "guard expects (guard (var clause...) body...)");
        return Nil(sl, sp);
    }
    Form *spec = f->as.list.items[1];
    const Symbol *var = spec->as.list.items[0]->as.sym;
    uint32_t ncl = spec->as.list.len - 1;
    Form **given = spec->as.list.items + 1;
    for (uint32_t i = 0; i < ncl; i++)
        if (given[i]->tag != F_LIST || given[i]->as.list.len == 0) {
            err(given[i], "guard clause expects (test body...) or (else body...)");
            return Nil(sl, sp);
        }
    bool has_else = ncl > 0 && kw_is(sl, given[ncl - 1]->as.list.items[0], sl->s_else);
    Form **cls = (Form **)arena_alloc(sl->a, (ncl + 1) * sizeof(Form *));
    if (ncl) memcpy(cls, given, ncl * sizeof(Form *));
    if (!has_else) {
        Form *reraise = Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-raise-continuable")), Sym(sl, sp, var));
        cls[ncl++] = Ln(sl, sp, 2, Sym(sl, sp, sl->s_else), reraise);
    }
    /* R10: the variable is the clauses' binder, not the body's. */
    LFrame *saved = scope_open(sl);
    FB pb = {0};
    push_param(sl, &pb, spec->as.list.items[0]->span, var);
    Form *hparams = fb_vec(sl, &pb, sp);
    Form *chain = cond_chain(sl, cls, ncl, sp);
    const Symbol *k = fresh(sl, "__r7rs_guard_k");
    const Symbol *v = fresh(sl, "__r7rs_guard_v");
    Form *hbody = rebind_muts(sl, sp, hparams, Ln(sl, sp, 2, Sym(sl, sp, k), thunk_of(sl, sp, chain)));
    scope_close(sl, saved);
    Form *handler = Ln(sl, sp, 4, Sym(sl, sp, sl->t_fn), hparams, AnyAnn(sl, sp), hbody);
    Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
    Form *bv[2] = { Sym(sl, sp, v), body };
    Form *bthunk = thunk_of(sl, sp, Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), Vec(sl, sp, bv, 2),
                                       thunk_of(sl, sp, Sym(sl, sp, v))));
    Form *weh = Ln(sl, sp, 3, Sym(sl, sp, I(sl, "r7rs-with-exception-handler")), handler, bthunk);
    Form *kp = Sym(sl, sp, k);
    Form *recv = Ln(sl, sp, 4, Sym(sl, sp, sl->t_fn), Vec(sl, sp, &kp, 1), AnyAnn(sl, sp), weh);
    Form *cc = Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-call/cc")), recv);
    return Ln(sl, sp, 1, cc);
}

/* R6: (parameterize ((p v) ...) body...) ->
 *   (r7rs-parameterize__ (r7rs-list (r7rs-cons p v) ...) (fn [] : any body')) */
static Form *lower_parameterize(SL *sl, Form *f) {
    Span sp = f->span;
    if (f->as.list.len < 3) {
        err(f, "parameterize expects (parameterize ((param value) ...) body...)");
        return Nil(sl, sp);
    }
    Form *bl = nil_to_list(sl, f->as.list.items[1]);
    if (bl->tag != F_LIST) {
        err(bl, "parameterize expects a list of (param value) bindings");
        return Nil(sl, sp);
    }
    FB lb = {0};
    fb_push(&lb, Sym(sl, sp, sl->p_list));
    for (uint32_t i = 0; i < bl->as.list.len; i++) {
        Form *b = bl->as.list.items[i];
        if (b->tag != F_LIST || b->as.list.len != 2) {
            err(b, "parameterize binding expects (param value)");
            free(lb.items);
            return Nil(sl, sp);
        }
        fb_push(&lb, Ln(sl, b->span, 3, Sym(sl, b->span, sl->p_cons),
                        lower(sl, b->as.list.items[0]), lower(sl, b->as.list.items[1])));
    }
    Form *bindings = fb_list(sl, &lb, sp);
    Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
    return Ln(sl, sp, 3, Sym(sl, sp, I(sl, "r7rs-parameterize__")), bindings, thunk_of(sl, sp, body));
}

/* R6: (delay expr) -> (r7rs-delay-force__ (fn [] : any (r7rs-make-promise expr')))
 *     (delay-force expr) -> (r7rs-delay-force__ (fn [] : any expr'))
 * -- the R7RS 7.3 definitions, `delay` being `delay-force` of a forced
 * promise. */
static Form *lower_delay(SL *sl, Form *f, bool is_force) {
    Span sp = f->span;
    if (f->as.list.len != 2) {
        err(f, "%s expects one expression", is_force ? "delay-force" : "delay");
        return Nil(sl, sp);
    }
    Form *x = lower(sl, f->as.list.items[1]);
    if (!is_force) x = Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-make-promise")), x);
    return Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-delay-force__")), thunk_of(sl, sp, x));
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
    /* R6: the no-match arm is the prelude's `any`-typed failure, not a bare
     * `panic`: a never-typed else arm nested in the clause chain lost the
     * outer arms' result assignments on the compiled path, so a two-clause
     * case-lambda answered its second clause with an untagged word. */
    Form *chain = Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-fail-any__")),
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
    formals = nil_to_list(sl, formals);
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

/* R10: declare the identifiers of a formals spec in the innermost scope. */
static void bind_formals(SL *sl, Form *formals) {
    if (formals->tag == F_SYM) { bind_name(sl, formals->as.sym, formals->span); return; }
    formals = nil_to_list(sl, formals);
    if (formals->tag != F_LIST) return;
    for (uint32_t i = 0; i < formals->as.list.len; i++) {
        Form *p = formals->as.list.items[i];
        if (p->tag == F_SYM && p->as.sym != sl->s_dot) bind_name(sl, p->as.sym, p->span);
    }
}

/* let-values / let*-values: (let [t1 e1 ...] (let [a (ref t1 0) ...] body)) --
 * the star form nests one binding at a time. */
static Form *lower_let_values(SL *sl, Form *f, bool star) {
    Span sp = f->span;
    if (f->as.list.len >= 2) f->as.list.items[1] = nil_to_list(sl, f->as.list.items[1]);
    if (f->as.list.len < 3 || f->as.list.items[1]->tag != F_LIST) {
        err(f, "%s expects (((formals) init)...) body...", star ? "let*-values" : "let-values");
        return Nil(sl, sp);
    }
    Form *specs = f->as.list.items[1];
    uint32_t n = specs->as.list.len;
    if (star) {
        /* One scope per binding, each init in the scope of those before it. */
        Form **linits = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(Form *));
        LFrame **saved = (LFrame **)arena_alloc(sl->a, (n + 1) * sizeof(LFrame *));
        LFrame *outer = sl->scope;
        for (uint32_t i = 0; i < n; i++) {
            Form *s = specs->as.list.items[i];
            if (s->tag != F_LIST || s->as.list.len != 2) {
                err(s, "binding must be (formals init)");
                scope_close(sl, outer);
                return Nil(sl, sp);
            }
            linits[i] = lower(sl, s->as.list.items[1]);
            saved[i] = scope_open(sl);
            bind_formals(sl, s->as.list.items[0]);
        }
        Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
        for (int32_t i = (int32_t)n - 1; i >= 0; i--) {
            Form *s = specs->as.list.items[i];
            const Symbol *tmp = fresh(sl, "__r7rs_vals");
            FB inner = {0};
            if (!push_values_bindings(sl, &inner, s->as.list.items[0], tmp, body)) {
                free(inner.items);
                scope_close(sl, outer);
                return Nil(sl, sp);
            }
            scope_close(sl, saved[i]);
            body = Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &inner, sp), body);
            Form *bv[2] = { Sym(sl, sp, tmp), linits[i] };
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
    LFrame *saved = scope_open(sl);
    for (uint32_t i = 0; i < n; i++) bind_formals(sl, specs->as.list.items[i]->as.list.items[0]);
    Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
    for (uint32_t i = 0; i < n; i++) {
        Form *s = specs->as.list.items[i];
        const Symbol *tmp = fresh(sl, "__r7rs_vals");
        fb_push(&outer, Sym(sl, sp, tmp));
        fb_push(&outer, linits[i]);
        if (!push_values_bindings(sl, &inner, s->as.list.items[0], tmp, body)) {
            free(outer.items); free(inner.items);
            scope_close(sl, saved);
            return Nil(sl, sp);
        }
    }
    scope_close(sl, saved);
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
    formals = nil_to_list(sl, formals);
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
/* R4: a body form whose head is a macro is expanded before the body is
 * classified, so a macro can expand to a define (R7RS 5.3.2); a `begin`
 * splices, and a `define-syntax` registers a macro scoped to this body. */
static void sr_expand_body_item(SL *sl, Form *it, FB *out) {
    it = sr_expand_head(sl, it);
    if (!it) return;
    if (head_is(it, sl->s_define_syntax)) { sr_define(sl, it); return; }
    if (head_is(it, sl->s_begin)) {
        for (uint32_t j = 1; j < it->as.list.len; j++) sr_expand_body_item(sl, it->as.list.items[j], out);
        return;
    }
    fb_push(out, it);
}

static Form *lower_body_inner(SL *sl, Form **items, uint32_t n, Span sp);
static Form *lower_body(SL *sl, Form **items, uint32_t n, Span sp) {
    uint32_t mark = sl->n_macros;
    /* R10: a body is a scope -- its internal defines, and the frame the
     * macros defined at its start resolve their templates in. */
    LFrame *saved = scope_open(sl);
    Form *r = lower_body_inner(sl, items, n, sp);
    scope_close(sl, saved);
    sl->n_macros = mark;
    return r;
}

static Form *lower_body_inner(SL *sl, Form **items, uint32_t n, Span sp) {
    /* Macro uses at the head first (R4), then define-values into plain
     * defines, so one loop sees them. */
    FB pre = {0};
    for (uint32_t i = 0; i < n; i++) sr_expand_body_item(sl, items[i], &pre);
    FB seq = {0};
    for (uint32_t i = 0; i < pre.n; i++) {
        if (head_is(pre.items[i], sl->s_define_values)) expand_define_values(sl, pre.items[i], &seq);
        else fb_push(&seq, pre.items[i]);
    }
    free(pre.items);
    items = seq.items; n = seq.n;
    if (n == 0) { free(seq.items); return Nil(sl, sp); }

    uint32_t ndef = 0;
    while (ndef < n && head_is(items[ndef], sl->s_define)) ndef++;
    for (uint32_t i = ndef; i < n; i++) {
        if (head_is(items[i], sl->s_define)) {
            err(items[i], "define is only allowed at the beginning of a body (R7RS 5.3.2)");
            free(seq.items);
            return Nil(sl, sp);
        }
    }
    /* R10: the defines are letrec* -- every name is bound before any init
     * or expression of the body is lowered. */
    for (uint32_t i = 0; i < ndef; i++) {
        Form *d = items[i];
        if (d->as.list.len < 2) continue;
        Form *target = d->as.list.items[1];
        if (target->tag == F_LIST && target->as.list.len >= 1 && target->as.list.items[0]->tag == F_SYM)
            bind_name(sl, target->as.list.items[0]->as.sym, target->span);
        else if (target->tag == F_SYM)
            bind_name(sl, target->as.sym, target->span);
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
            body = make_letrec(sl, sp, &b, body);
            i = j - 1;
        } else {
            const Symbol *self = rn(sl, names[i]);
            if (form_mentions_sym(inits[i], self)) {
                /* R10: letrec* (R7RS 5.3.2) -- a value define whose own init
                 * refers to it, as `(define p (delay ... (force p)))` does.
                 * A plain `let` left that `p` unbound; bind a cell first and
                 * `set!` it, so the init's closure sees the finished value. */
                FB b = {0};
                push_binding(sl, &b, sp, names[i], Bool(sl, sp, false), true);
                Form *store = Ln(sl, sp, 3, Sym(sl, sp, sl->s_set), Sym(sl, sp, self), inits[i]);
                body = Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &b, sp),
                          Ln(sl, sp, 3, Sym(sl, sp, sl->t_do), store, body));
            } else {
                body = make_let(sl, sp, names + i, inits + i, 1, body);
            }
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
            if (d->as.list.len == 0) return Ln(sl, sp, 1, Sym(sl, sp, sl->p_list));
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
        case F_NIL: case F_BOOL:
            /* R10: the WORD `nil` / `true` / `false` quoted is a symbol (the
             * reader stamps them); `()` and `#t`/`#f` are not stamped.  Built
             * by name: a `(quote nil)` would elaborate as Turmeric's nil. */
            if (d->fx_prov == PROV_SCHEME_WORD && !prelude_span(sp)) {
                const char *w = d->tag == F_NIL ? "nil" : d->as.b ? "true" : "false";
                return Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-string->symbol")),
                          form_str(sl->a, sp, w, (uint32_t)strlen(w)));
            }
            if (d->tag == F_NIL) return Ln(sl, sp, 1, Sym(sl, sp, sl->p_list));
            return d;
        default: return d;   /* int, float, string, keyword */
    }
}

/* Quasiquote: the datum walker with holes.  At depth 1 an `unquote` is an
 * expression and an `unquote-splicing` element splices via `r7rs-append`;
 * a nested quasiquote raises the depth and its unquotes lower it, staying
 * data (R7RS 4.2.8). */
static Form *lower_qq(SL *sl, Form *f, int depth);
/* R10: `,x` and `(unquote x)` are the same datum (R7RS 4.2.8), as are the
 * other three abbreviations and their long forms.  The kind of `f` -- one of
 * F_QUOTE / F_QUASIQUOTE / F_UNQUOTE / F_UNQUOTE_SPLICING -- or F_LIST when
 * it is neither; `*arg` is the one operand. */
static FormTag qq_kind(SL *sl, Form *f, Form **arg) {
    switch (f->tag) {
        case F_QUOTE: case F_QUASIQUOTE: case F_UNQUOTE: case F_UNQUOTE_SPLICING:
            *arg = f->as.list.items[0];
            return f->tag;
        case F_LIST:
            if (f->as.list.len == 2 && f->as.list.items[0]->tag == F_SYM) {
                const Symbol *h = f->as.list.items[0]->as.sym;
                *arg = f->as.list.items[1];
                if (h == sl->s_quasiquote) return F_QUASIQUOTE;
                if (h == sl->s_unquote) return F_UNQUOTE;
                if (h == sl->s_unquote_splicing) return F_UNQUOTE_SPLICING;
                if (h == I(sl, "quote")) return F_QUOTE;
            }
            return F_LIST;
        default:
            return f->tag;
    }
}
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
        Form *it = f->as.list.items[i], *arg = NULL;
        if (qq_kind(sl, it, &arg) == F_UNQUOTE_SPLICING && depth == 1)
            tail = Ln(sl, sp, 3, Sym(sl, sp, sl->p_append), lower(sl, arg), tail);
        else
            tail = Ln(sl, sp, 3, Sym(sl, sp, sl->p_cons), lower_qq(sl, it, depth), tail);
    }
    return tail;
}
static Form *lower_qq(SL *sl, Form *f, int depth) {
    Span sp = f->span;
    Form *arg = NULL;
    switch (qq_kind(sl, f, &arg)) {
        case F_UNQUOTE:
            if (depth == 1) return lower(sl, arg);
            return Ln(sl, sp, 3, Sym(sl, sp, sl->p_list),
                      form_quote(sl->a, sp, Sym(sl, sp, sl->s_unquote)),
                      lower_qq(sl, arg, depth - 1));
        case F_UNQUOTE_SPLICING:
            return Ln(sl, sp, 3, Sym(sl, sp, sl->p_list),
                      form_quote(sl->a, sp, Sym(sl, sp, sl->s_unquote_splicing)),
                      lower_qq(sl, arg, depth - 1));
        case F_QUASIQUOTE:
            return Ln(sl, sp, 3, Sym(sl, sp, sl->p_list),
                      form_quote(sl->a, sp, Sym(sl, sp, sl->s_quasiquote)),
                      lower_qq(sl, arg, depth + 1));
        case F_QUOTE:
            /* R10: `',x` is `(quote (unquote x))` -- a quote does not change
             * the quasiquote level, so the unquote inside it still fires. */
            return Ln(sl, sp, 3, Sym(sl, sp, sl->p_list),
                      form_quote(sl->a, sp, Sym(sl, sp, I(sl, "quote"))),
                      lower_qq(sl, arg, depth));
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
            /* R5: the prelude is exempt from the rename table as well as the
             * operator rewrite -- it is written against the typed stdlib by
             * its real names (`floor`, `sqrt`, `exp`, ...), which are exactly
             * the Scheme names the table maps onto the prelude's own
             * procedures; renamed, `(floor x)` inside r7rs-floor called
             * itself forever. */
            if (prelude_span(f->span)) return f;
            /* R10: a local variable (or a template's global alias) first --
             * `(let ((+ -)) (+ 3 1))` means the local `+`. */
            if (global_alias_orig(sl, f->as.sym) ||
                (sl->scope && scope_lookup(sl->scope, f->as.sym))) {
                const Symbol *r = rn(sl, f->as.sym);
                return (r == f->as.sym) ? f : Sym(sl, f->span, r);
            }
            int op = op_index(sl, f->as.sym);
            /* R10: an operator as a VALUE is the variadic prelude procedure,
             * as an `any` -- `((if #f + *) 3 4)` merged two fn types into a
             * C function pointer spelled from carrier kinds, which cc rejects
             * as incompatible pointer types (an error from GCC 14). */
            if (op >= 0)
                return Ln(sl, f->span, 3, Sym(sl, f->span, I(sl, "::")),
                          Sym(sl, f->span, sl->ops_val[op]), Sym(sl, f->span, sl->t_any));
            const Symbol *r = rn(sl, f->as.sym);
            return (r == f->as.sym) ? f : Sym(sl, f->span, r);
        }
        case F_QUOTE:      return lower_datum(sl, f->as.list.items[0]);
        case F_QUASIQUOTE: return lower_qq(sl, f->as.list.items[0], 1);
        case F_LIST: {
            /* R10: `(quasiquote x)` written out is the same as `` `x ``. */
            Form *arg = NULL;
            if (!prelude_span(f->span) && qq_kind(sl, f, &arg) == F_QUASIQUOTE &&
                !sr_lookup(sl, sl->s_quasiquote))
                return lower_qq(sl, arg, 1);
            break;
        }
        case F_VEC:
            /* R7: a vector is self-evaluating (R7RS 4.1.2): `#(a b c)` is
             * the constant `'#(a b c)`, its elements DATA, not expressions to
             * evaluate (R10: chibi's suite writes `(test #(a b c) ...)`).  So
             * it is built exactly as a quoted one -- through `vector`, always
             * a `(Vec any)`.  Only a vector the reader read from `#(`
             * (PROV_SCHEME_VECTOR): a Turmeric-shaped `(defn f [x] ...)` in a
             * Scheme file keeps its binding vector. */
            if (!prelude_span(f->span) && f->fx_prov == PROV_SCHEME_VECTOR)
                return lower_datum(sl, f);
            return lower_children(sl, f);
        case F_MAP: case F_SET: case F_MAP_LITERAL: case F_SET_LITERAL:
        case F_UNQUOTE: case F_UNQUOTE_SPLICING:
            return lower_children(sl, f);
        default: return f;
    }
    if (f->as.list.len == 0) return Ln(sl, f->span, 1, Sym(sl, f->span, sl->p_list));
    Form *head = f->as.list.items[0];
    if (head->tag == F_SYM && !prelude_span(f->span)) {
        /* R10 (hygiene): a head that is a LOCAL variable is a call, whatever
         * its name -- `(let ((if even?)) (if 7))` calls even?.  A template's
         * alias of a keyword or global is that keyword or global, however
         * the use site binds the name. */
        if (!global_alias_orig(sl, head->as.sym) && sl->scope && scope_lookup(sl->scope, head->as.sym))
            return lower_children(sl, f);
    }
    /* The name the head DISPATCHES on: an alias's original.  The form keeps
     * the alias, so a global procedure it names is still called globally. */
    const Symbol *hsym = head->tag == F_SYM ? head->as.sym : NULL;
    if (hsym && global_alias_orig(sl, hsym)) hsym = global_alias_orig(sl, hsym);
    if (hsym && sr_lookup(sl, hsym)) {
        /* R4: a macro use.  Expand until the head is not a macro, then lower
         * the expansion; the depth guard catches an expansion that grows a
         * macro use inside itself forever. */
        Form *x = sr_expand_head(sl, f);
        if (!x) return Nil(sl, f->span);
        if (sl->expand_depth >= SR_MAX_DEPTH) {
            err(f, "macro expansion nested more than %d deep -- does '%s' expand to itself?",
                SR_MAX_DEPTH, head->as.sym->name);
            return Nil(sl, f->span);
        }
        sl->expand_depth++;
        Form *r = lower(sl, x);
        sl->expand_depth--;
        return r;
    }
    if (head->tag == F_SYM) {
        const Symbol *h = hsym;
        Form *r = NULL;
        int op = op_index(sl, h);
        if (op >= 0 && !prelude_span(f->span)) return lower_operator(sl, f, op);
        if (h == sl->s_quote && f->as.list.len == 2) return lower_datum(sl, f->as.list.items[1]);
        if (h == sl->s_quasiquote && f->as.list.len == 2) return lower_qq(sl, f->as.list.items[1], 1);
        if (h == sl->s_syntax_error) {
            const char *msg = (f->as.list.len >= 2 && f->as.list.items[1]->tag == F_STR)
                ? f->as.list.items[1]->as.s.p : "syntax-error";
            err(f, "%s%s", msg, f->as.list.len > 2 ? " (see the forms that follow the message)" : "");
            return Nil(sl, f->span);
        }
        if (h == sl->s_let_syntax || h == sl->s_letrec_syntax) return lower_let_syntax(sl, f);
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
        if (!prelude_span(f->span)) {
            /* R7: named refusals rather than an unbound-name error. */
            const char *hn = h->name;
            if (strcmp(hn, "string-set!") == 0 || strcmp(hn, "string-fill!") == 0 ||
                strcmp(hn, "string-copy!") == 0) {
                err(f, "%s is not supported: #lang r7rs strings are immutable (a Turmeric cstr); "
                       "build a new string with string-append, substring or list->string", hn);
                return Nil(sl, f->span);
            }
            if (h == sl->s_include || strcmp(hn, "include-ci") == 0) {
                err(f, "%s is not supported yet: an included file would have to be read as Scheme "
                       "without its own #lang line; put the definitions in a define-library and import it", hn);
                return Nil(sl, f->span);
            }
        }
        if (h == sl->s_guard)        return lower_guard(sl, f);
        if (h == sl->s_parameterize) return lower_parameterize(sl, f);
        if (h == sl->s_delay)        return lower_delay(sl, f, false);
        if (h == sl->s_delay_force)  return lower_delay(sl, f, true);
        if (h == sl->s_define) {
            err(f, "define is not allowed in expression position; a body's "
                   "defines come first (R7RS 5.3.2)");
            return Nil(sl, f->span);
        }
        if (h == sl->s_define_syntax) {
            err(f, "define-syntax is only allowed at the top level or at the beginning of a body");
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
    if (head_is(f, sl->s_define_syntax)) { sr_define(sl, f); return; }
    if (f->tag == F_LIST && f->as.list.len > 0 && f->as.list.items[0]->tag == F_SYM &&
        sr_lookup(sl, f->as.list.items[0]->as.sym)) {
        Form *x = sr_expand_head(sl, f);
        if (!x) return;
        lower_toplevel(sl, x, out);
        return;
    }
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
            LFrame *saved = scope_open(sl);     /* R10: the parameters' scope */
            Form *params = lower_formals(sl, formals, &ok, &rest);
            if (!ok || f->as.list.len < 3) {
                if (ok) err(f, "define needs a body");
                scope_close(sl, saved);
                return;
            }
            Form *body = rebind_rest(sl, sp, rest,
                                     lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp));
            scope_close(sl, saved);
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
        /* R6: `(define f (lambda formals body...))` of a name that is never
         * `set!` is `(define (f . formals) body...)` -- a defn, so it is a
         * known global with its variadic signature (a `def` of a lambda value
         * drops the rest marker from the binding's type, and `(apply f xs)`
         * then cannot pack for it) and a forward reference works. */
        if (!is_mut(sl, name) && f->as.list.items[2]->tag == F_LIST &&
            f->as.list.items[2]->as.list.len >= 3 &&
            is_sym(f->as.list.items[2]->as.list.items[0], sl->s_lambda) &&
            !sr_lookup(sl, sl->s_lambda)) {
            Form *lam = f->as.list.items[2];
            Form *formals = nil_to_list(sl, lam->as.list.items[1]);
            FB tb = {0};
            fb_push(&tb, target);
            if (formals->tag == F_SYM) {
                fb_push(&tb, Sym(sl, formals->span, sl->s_dot));
                fb_push(&tb, formals);
            } else if (formals->tag == F_LIST) {
                for (uint32_t i = 0; i < formals->as.list.len; i++) fb_push(&tb, formals->as.list.items[i]);
            } else {
                err(formals, "lambda formals must be a list of identifiers, a single "
                             "identifier, or `(a b . rest)`");
                free(tb.items);
                return;
            }
            FB db = {0};
            fb_push(&db, f->as.list.items[0]);
            fb_push(&db, fb_list(sl, &tb, target->span));
            for (uint32_t i = 2; i < lam->as.list.len; i++) fb_push(&db, lam->as.list.items[i]);
            lower_toplevel(sl, fb_list(sl, &db, sp), out);
            return;
        }
        Form *init = lower(sl, f->as.list.items[2]);
        /* R10: `(define first car)` -- a global that IS another procedure.
         * Left bare, it became a thin C function pointer whose declared
         * parameters were the carrier `int64_t` while the aliased procedure
         * takes a typed struct (cc: incompatible-pointer / int-conversion, an
         * error from GCC 14).  As an `any` it is called through the dynamic
         * path, like every other Scheme procedure value. */
        if (init->tag == F_SYM && !is_mut(sl, name))
            init = Ln(sl, sp, 3, Sym(sl, sp, I(sl, "::")), init, Sym(sl, sp, sl->t_any));
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
    if (strcmp(head, "scheme") == 0) {
        /* R7: every R7RS-small library is known by name.  A resident one is
         * the prelude; an on-demand one is spliced in by the load expander;
         * a deferred one says why it is not here yet. */
        int li = scheme_lib_index(set);
        if (li < 0) {
            err(set, "no such library in R7RS-small: the (scheme ...) libraries are base, case-lambda, "
                     "char, complex, cxr, eval, file, inexact, lazy, load, process-context, read, "
                     "repl, time and write");
            *ok = false;
            return NULL;
        }
        if (SCHEME_LIBS[li].kind == LIB_DEFERRED) {
            err(set, "(scheme %s) is not supported yet: it %s", SCHEME_LIBS[li].name, SCHEME_LIBS[li].what);
            *ok = false;
            return NULL;
        }
        sl->lib_imported[li] = true;
        return NULL;
    }
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
        if (strcmp(n, "else") == 0) return true;
        /* R7: the same list `(features)` returns (r7rs-features in the
         * prelude) -- keep the two equal. */
        return strcmp(n, "r7rs") == 0 || strcmp(n, "exact-closed") == 0 ||
               strcmp(n, "turmeric") == 0;
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
        /* R7: a (scheme ...) requirement asks the library table, quietly -- a
         * deferred or unknown library is simply absent. */
        if (is_scheme_libname(req->as.list.items[1])) {
            int li = scheme_lib_index(req->as.list.items[1]);
            return li >= 0 && SCHEME_LIBS[li].kind != LIB_DEFERRED;
        }
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

/* ---- R10: assignment conversion -----------------------------------------
 *
 * A compiled closure COPIES the variables it captures into its environment.
 * That is right for a variable nothing assigns, and wrong for one that is
 * `set!`: `(let ((sum 0)) (do ((i 0 (+ i 1))) ((= i n)) (set! sum (+ sum
 * i))) sum)` answered 0, because the `do` loop is a lambda and its `set!`
 * updated the loop's copy.  The interpreter's frames are shared, so it said
 * 10 -- the back ends disagreed on a basic R7RS program.
 *
 * The textbook fix, on the lowered forms: a `^mut` `let` binding that a
 * nested `fn` mentions becomes a heap cell (`R7rsBox`, the prelude), bound
 * once and never reassigned, so every closure's copy is the same pointer.
 * Each read of the name in its scope becomes `(r7rs-unbox__ n)` and each
 * `(set! n v)` becomes `(r7rs-box-set!__ n v)`; a scope that rebinds the name
 * (a `let`, `letrec`, `fn` or `defn` parameter) stops the rewrite.  A `set!`
 * variable no lambda sees keeps its plain mutable cell. */
static bool ac_is_set(SL *sl, const Form *f) {
    return f->tag == F_LIST && f->as.list.len == 3 &&
           (is_sym(f->as.list.items[0], sl->t_set) || is_sym(f->as.list.items[0], sl->s_set));
}
static bool ac_vec_binds(const Form *pv, const Symbol *n) {
    if (!pv || pv->tag != F_VEC) return false;
    for (uint32_t i = 0; i < pv->as.list.len; i++)
        if (is_sym(pv->as.list.items[i], n)) return true;
    return false;
}
/* One binding of a lowered `let` vector: `[^marker] name [: T] init`. */
typedef struct { uint32_t name, init; bool mut; } AcBind;
static uint32_t ac_parse_binds(SL *sl, const Form *v, AcBind *out) {
    uint32_t n = 0, i = 0, len = v->as.list.len;
    while (i < len) {
        bool mut = false;
        const Form *it = v->as.list.items[i];
        if (it->tag == F_SYM && it->as.sym->name[0] == '^') { mut = it->as.sym == sl->t_mut; i++; }
        if (i >= len) break;
        uint32_t name = i++;
        if (i < len && v->as.list.items[i]->tag == F_TYPE_ANN) i++;
        if (i >= len) break;
        out[n].name = name; out[n].init = i++; out[n].mut = mut;
        n++;
    }
    return n;
}
static Form *ac_copy(SL *sl, const Form *f, Form **items) {
    Form *c = (Form *)arena_alloc(sl->a, sizeof(Form));
    *c = *f;
    c->as.list.items = items;
    return c;
}
static bool ac_captured(SL *sl, const Symbol *n, const Form *f, bool inside) {
    if (!f) return false;
    if (f->tag == F_SYM) return inside && f->as.sym == n;
    if (f->tag != F_LIST && f->tag != F_VEC) return false;
    uint32_t from = 0;
    if (head_is(f, sl->t_fn) && f->as.list.len >= 2) {
        if (ac_vec_binds(f->as.list.items[1], n)) return false;
        inside = true; from = 2;
    } else if (head_is(f, sl->t_defn) && f->as.list.len >= 3) {
        if (ac_vec_binds(f->as.list.items[2], n)) return false;
        inside = true; from = 3;
    }
    for (uint32_t i = from; i < f->as.list.len; i++)
        if (ac_captured(sl, n, f->as.list.items[i], inside)) return true;
    return false;
}
static Form *ac_subst(SL *sl, const Symbol *n, Form *f);
/* `f` with items[from..] substituted; `f` itself when nothing changed. */
static Form *ac_subst_from(SL *sl, const Symbol *n, Form *f, uint32_t from) {
    uint32_t len = f->as.list.len;
    Form **ni = NULL;
    for (uint32_t i = from; i < len; i++) {
        Form *x = ac_subst(sl, n, f->as.list.items[i]);
        if (x != f->as.list.items[i] && !ni) {
            ni = (Form **)arena_alloc(sl->a, len * sizeof(Form *));
            memcpy(ni, f->as.list.items, len * sizeof(Form *));
        }
        if (ni) ni[i] = x;
    }
    return ni ? ac_copy(sl, f, ni) : f;
}
static Form *ac_subst(SL *sl, const Symbol *n, Form *f) {
    if (!f) return f;
    Span sp = f->span;
    if (f->tag == F_SYM)
        return f->as.sym == n ? Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-unbox__")), f) : f;
    if (f->tag != F_LIST && f->tag != F_VEC) return f;
    if (f->tag == F_LIST && ac_is_set(sl, f) && is_sym(f->as.list.items[1], n))
        return Ln(sl, sp, 3, Sym(sl, sp, I(sl, "r7rs-box-set!__")), f->as.list.items[1],
                  ac_subst(sl, n, f->as.list.items[2]));
    if (head_is(f, sl->t_fn) && f->as.list.len >= 2)
        return ac_vec_binds(f->as.list.items[1], n) ? f : ac_subst_from(sl, n, f, 2);
    if (head_is(f, sl->t_defn) && f->as.list.len >= 3)
        return ac_vec_binds(f->as.list.items[2], n) ? f : ac_subst_from(sl, n, f, 3);
    if (head_is(f, sl->t_letrec) && f->as.list.len >= 2)
        return ac_vec_binds(f->as.list.items[1], n) ? f : ac_subst_from(sl, n, f, 1);
    if (head_is(f, sl->t_let) && f->as.list.len >= 2 && f->as.list.items[1]->tag == F_VEC) {
        /* Sequential: each init sees the bindings before it, so the rewrite
         * runs through the inits and stops after the one that rebinds `n`. */
        Form *v = f->as.list.items[1];
        AcBind *bs = (AcBind *)arena_alloc(sl->a, (v->as.list.len + 1) * sizeof(AcBind));
        uint32_t nb = ac_parse_binds(sl, v, bs);
        Form **vi = (Form **)arena_alloc(sl->a, (v->as.list.len + 1) * sizeof(Form *));
        memcpy(vi, v->as.list.items, v->as.list.len * sizeof(Form *));
        bool shadowed = false;
        for (uint32_t k = 0; k < nb && !shadowed; k++) {
            vi[bs[k].init] = ac_subst(sl, n, v->as.list.items[bs[k].init]);
            if (is_sym(v->as.list.items[bs[k].name], n)) shadowed = true;
        }
        Form **li = (Form **)arena_alloc(sl->a, f->as.list.len * sizeof(Form *));
        memcpy(li, f->as.list.items, f->as.list.len * sizeof(Form *));
        li[1] = ac_copy(sl, v, vi);
        if (!shadowed)
            for (uint32_t i = 2; i < f->as.list.len; i++) li[i] = ac_subst(sl, n, f->as.list.items[i]);
        return ac_copy(sl, f, li);
    }
    return ac_subst_from(sl, n, f, 0);
}
/* Convert binding number `k` of the `let` form `f` (already walked). */
static Form *ac_convert(SL *sl, Form *f, uint32_t k) {
    Form *v = f->as.list.items[1];
    uint32_t vlen = v->as.list.len;
    AcBind *bs = (AcBind *)arena_alloc(sl->a, (vlen + 1) * sizeof(AcBind));
    uint32_t nb = ac_parse_binds(sl, v, bs);
    const Symbol *n = v->as.list.items[bs[k].name]->as.sym;
    Span sp = v->as.list.items[bs[k].name]->span;
    FB nv = {0};
    bool shadowed = false;
    for (uint32_t j = 0; j < nb; j++) {
        uint32_t start = j == 0 ? 0 : bs[j - 1].init + 1;
        if (j == k) {
            fb_push(&nv, v->as.list.items[bs[j].name]);
            fb_push(&nv, Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-box__")), v->as.list.items[bs[j].init]));
            continue;
        }
        for (uint32_t i = start; i < bs[j].init; i++) fb_push(&nv, v->as.list.items[i]);
        Form *init = v->as.list.items[bs[j].init];
        fb_push(&nv, (j > k && !shadowed) ? ac_subst(sl, n, init) : init);
        if (j > k && is_sym(v->as.list.items[bs[j].name], n)) shadowed = true;
    }
    Form **li = (Form **)arena_alloc(sl->a, f->as.list.len * sizeof(Form *));
    memcpy(li, f->as.list.items, f->as.list.len * sizeof(Form *));
    li[1] = fb_vec(sl, &nv, v->span);
    if (!shadowed)
        for (uint32_t i = 2; i < f->as.list.len; i++) li[i] = ac_subst(sl, n, f->as.list.items[i]);
    return ac_copy(sl, f, li);
}
static Form *ac_walk(SL *sl, Form *f) {
    if (!f || (f->tag != F_LIST && f->tag != F_VEC)) return f;
    uint32_t len = f->as.list.len;
    Form **ni = NULL;
    for (uint32_t i = 0; i < len; i++) {
        Form *x = ac_walk(sl, f->as.list.items[i]);
        if (x != f->as.list.items[i] && !ni) {
            ni = (Form **)arena_alloc(sl->a, len * sizeof(Form *));
            memcpy(ni, f->as.list.items, len * sizeof(Form *));
        }
        if (ni) ni[i] = x;
    }
    Form *g = ni ? ac_copy(sl, f, ni) : f;
    if (!(head_is(g, sl->t_let) && len >= 2 && g->as.list.items[1]->tag == F_VEC)) return g;
    uint32_t vlen = g->as.list.items[1]->as.list.len;
    AcBind *bs = (AcBind *)arena_alloc(sl->a, (vlen + 1) * sizeof(AcBind));
    uint32_t nb = ac_parse_binds(sl, g->as.list.items[1], bs);
    for (uint32_t k = 0; k < nb; k++) {
        /* Re-parse: a conversion drops the `^mut` marker and the annotation,
         * which moves the later bindings' indices (never their ordinals). */
        Form *v = g->as.list.items[1];
        nb = ac_parse_binds(sl, v, bs);
        if (k >= nb || !bs[k].mut) continue;
        const Symbol *n = v->as.list.items[bs[k].name]->as.sym;
        bool cap = false;
        for (uint32_t j = k + 1; j < nb && !cap; j++)
            cap = ac_captured(sl, n, v->as.list.items[bs[j].init], false);
        for (uint32_t i = 2; i < g->as.list.len && !cap; i++)
            cap = ac_captured(sl, n, g->as.list.items[i], false);
        if (cap) g = ac_convert(sl, g, k);
    }
    return g;
}

/* R10: the names the stdlib forms ahead of the program define -- every
 * `defn`/`def`/`defmacro` at the top of a non-Scheme form or directly inside
 * its `defmodule`. */
static void stdlib_names_of(SL *sl, const Form *f, FB *out, int depth) {
    if (!f || f->tag != F_LIST || f->as.list.len < 2 || f->as.list.items[0]->tag != F_SYM) return;
    const char *h = f->as.list.items[0]->as.sym->name;
    if (strcmp(h, "defmodule") == 0 && depth == 0) {
        for (uint32_t i = 2; i < f->as.list.len; i++) stdlib_names_of(sl, f->as.list.items[i], out, 1);
        return;
    }
    if (strcmp(h, "defn") && strcmp(h, "def") && strcmp(h, "defmacro")) return;
    for (uint32_t i = 1; i < f->as.list.len; i++) {
        const Form *x = f->as.list.items[i];
        if (x->tag != F_SYM) continue;
        if (x->as.sym->name[0] == '^') continue;
        fb_push(out, (Form *)x);
        return;
    }
}
static void user_define_names(SL *sl, const Form *f, FB *out) {
    if (head_is(f, sl->s_begin)) {
        for (uint32_t i = 1; i < f->as.list.len; i++) user_define_names(sl, f->as.list.items[i], out);
        return;
    }
    if (!head_is(f, sl->s_define) || f->as.list.len < 2) return;
    Form *t = f->as.list.items[1];
    while (t->tag == F_LIST && t->as.list.len >= 1) t = t->as.list.items[0];  /* (define ((f a) b) ...) */
    if (t->tag == F_SYM) fb_push(out, t);
}
/* R10: every identifier the user's Scheme code BINDS -- formals, `let`-family
 * and `do` variables, named-let names, `guard` variables.  One named like a
 * Turmeric special form (`return`, `handle`, `perform`, `resume`, ...) was
 * elaborated AS that form wherever it headed a call: chibi's
 * `(call/cc (lambda (return) ... (return #f)))` compiled to an early return
 * (invalid C) and returned #f into a `+` on the interpreter. */
static void binders_of_formals(const Form *f, FB *out) {
    if (!f) return;
    if (f->tag == F_SYM) { fb_push(out, (Form *)f); return; }
    if (f->tag == F_LIST || f->tag == F_VEC)
        for (uint32_t i = 0; i < f->as.list.len; i++)
            if (f->as.list.items[i]->tag == F_SYM) fb_push(out, f->as.list.items[i]);
}
static void binders_of_bindings(const Form *b, FB *out) {
    if (!b || b->tag != F_LIST) return;
    for (uint32_t i = 0; i < b->as.list.len; i++) {
        const Form *x = b->as.list.items[i];
        if (x->tag == F_LIST && x->as.list.len >= 1) binders_of_formals(x->as.list.items[0], out);
    }
}
static void user_binders(SL *sl, const Form *f, FB *out) {
    if (!f || f->tag == F_QUOTE) return;
    if (f->tag != F_LIST && f->tag != F_VEC) return;
    uint32_t len = f->as.list.len;
    if (f->tag == F_LIST && len >= 2) {
        const Form *h = f->as.list.items[0];
        if (is_sym(h, sl->s_lambda)) binders_of_formals(f->as.list.items[1], out);
        else if (is_sym(h, sl->s_define) && f->as.list.items[1]->tag == F_LIST)
            binders_of_formals(f->as.list.items[1], out);
        else if (is_sym(h, sl->s_let) || is_sym(h, sl->s_letstar) || is_sym(h, sl->s_letrec) ||
                 is_sym(h, sl->s_letrecstar) || is_sym(h, sl->s_do) ||
                 is_sym(h, sl->s_let_values) || is_sym(h, sl->s_letstar_values)) {
            const Form *b = f->as.list.items[1];
            if (b->tag == F_SYM && len >= 3) { fb_push(out, (Form *)b); b = f->as.list.items[2]; }
            binders_of_bindings(b, out);
        } else if (is_sym(h, sl->s_case_lambda)) {
            for (uint32_t i = 1; i < len; i++)
                if (f->as.list.items[i]->tag == F_LIST && f->as.list.items[i]->as.list.len >= 1)
                    binders_of_formals(f->as.list.items[i]->as.list.items[0], out);
        } else if (is_sym(h, sl->s_guard) && f->as.list.items[1]->tag == F_LIST &&
                   f->as.list.items[1]->as.list.len >= 1 &&
                   f->as.list.items[1]->as.list.items[0]->tag == F_SYM) {
            fb_push(out, f->as.list.items[1]->as.list.items[0]);
        }
    }
    for (uint32_t i = 0; i < len; i++) user_binders(sl, f->as.list.items[i], out);
}
static void add_clash(SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->n_clash; i++) if (sl->clash_from[i] == s) return;
    if (sl->n_clash == sl->cap_clash) {
        sl->cap_clash = sl->cap_clash ? sl->cap_clash * 2 : 8;
        sl->clash_from = (const Symbol **)realloc((void *)sl->clash_from, sl->cap_clash * sizeof(Symbol *));
        sl->clash_to = (const Symbol **)realloc((void *)sl->clash_to, sl->cap_clash * sizeof(Symbol *));
        if (!sl->clash_from || !sl->clash_to) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    char buf[256];
    snprintf(buf, sizeof buf, "%s--user", s->name);
    sl->clash_from[sl->n_clash] = s;
    sl->clash_to[sl->n_clash++] = I(sl, buf);
}
static void note_stdlib_clashes(SL *sl, Form *const *forms, uint32_t n) {
    FB lib = {0}, user = {0};
    bool library = false;
    for (uint32_t i = 0; i < n; i++) {
        if (!is_scheme_file(forms[i])) stdlib_names_of(sl, forms[i], &lib, 0);
        else if (!prelude_span(forms[i]->span)) {
            if (head_is(forms[i], sl->s_define_library)) library = true;
            user_define_names(sl, forms[i], &user);
        }
    }
    /* A library's names live in its module and are exported by name. */
    if (!library) {
        for (uint32_t u = 0; u < user.n; u++) {
            const Symbol *s = user.items[u]->as.sym;
            if (rn(sl, s) != s) continue;   /* a standard name: R7RS 5.2 */
            bool clash = false;
            for (uint32_t k = 0; k < lib.n && !clash; k++) clash = lib.items[k]->as.sym == s;
            if (clash) add_clash(sl, s);
        }
    }
    FB binders = {0};
    for (uint32_t i = 0; i < n; i++)
        if (is_scheme_file(forms[i]) && !prelude_span(forms[i]->span)) user_binders(sl, forms[i], &binders);
    for (uint32_t b = 0; b < binders.n; b++) {
        const Symbol *s = binders.items[b]->as.sym;
        if (rn(sl, s) == s && tur_name_is_reserved_special_form(s->name)) add_clash(sl, s);
    }
    free(binders.items);
    free(lib.items);
    free(user.items);
}

Form **scheme_lower_program(Arena *a, SymbolTable *st,
                            Form *const *forms, uint32_t n, uint32_t *out_n) {
    SL sl;
    sl_init(&sl, a, st);
    for (uint32_t i = 0; i < n; i++)
        if (is_scheme_file(forms[i])) collect_setter_macros(&sl, forms[i]);
    for (uint32_t i = 0; i < n; i++)
        if (is_scheme_file(forms[i])) collect_muts(&sl, forms[i]);
    note_stdlib_clashes(&sl, forms, n);
    FB out = {0}, sforms = {0};
    Span first_sp = SPAN_UNKNOWN;
    bool have_first = false;
    for (uint32_t i = 0; i < n; i++) {
        if (is_scheme_file(forms[i]) && prelude_span(forms[i]->span)) {
            /* R9: the prelude and the on-demand library files are `#lang
             * r7rs` too, and they share this stream with the user's file.
             * Lower them, but in place: they are neither part of a user
             * `define-library` (which may hold nothing else, so a project
             * library build was refused over the prelude's first defstruct)
             * nor of the module a program with imports is wrapped in. */
            FB pf = {0};
            sl.in_user = false;
            lower_toplevel(&sl, forms[i], &pf);
            for (uint32_t k = 0; k < pf.n; k++) fb_push(&out, pf.items[k]);
            free(pf.items);
        } else if (is_scheme_file(forms[i])) {
            if (!have_first) { first_sp = forms[i]->span; have_first = true; }
            uint32_t from = sforms.n, lib_from = sl.lib_body.n;
            sl.in_user = true;
            lower_toplevel(&sl, forms[i], &sforms);
            sl.in_user = false;
            for (uint32_t k = from; k < sforms.n; k++) sforms.items[k] = ac_walk(&sl, sforms.items[k]);
            for (uint32_t k = lib_from; k < sl.lib_body.n; k++)
                sl.lib_body.items[k] = ac_walk(&sl, sl.lib_body.items[k]);
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
    free((void *)sl.clash_from);
    free((void *)sl.clash_to);
    free((void *)sl.setters);
    free(sl.macros);
    return res;
}

/* R7: the on-demand library files a top-level form of a Scheme file needs --
 * an `(import ...)` of (scheme time) / (scheme process-context) /
 * (scheme file), directly or under only/prefix/rename/except, or the same
 * inside a define-library's import declarations.  The load expander splices
 * each one in (deduplicated by its visited set) before the lowering runs, so
 * the library's forms are lowered with everything else. */
static void lib_files_of_set(const Form *set, const char **out, uint32_t cap, uint32_t *n) {
    while (set && set->tag == F_LIST && set->as.list.len >= 2 && set->as.list.items[0]->tag == F_SYM) {
        const char *h = set->as.list.items[0]->as.sym->name;
        if (strcmp(h, "only") && strcmp(h, "prefix") && strcmp(h, "rename") && strcmp(h, "except")) break;
        set = set->as.list.items[1];
    }
    int li = scheme_lib_index(set);
    if (li < 0 || SCHEME_LIBS[li].kind != LIB_ONDEMAND) return;
    for (uint32_t i = 0; i < *n; i++) if (out[i] == SCHEME_LIBS[li].what) return;
    if (*n < cap) out[(*n)++] = SCHEME_LIBS[li].what;
}
uint32_t scheme_import_library_files(const Form *f, const char **out, uint32_t cap) {
    uint32_t n = 0;
    if (!f || f->tag != F_LIST || f->as.list.len == 0 || f->as.list.items[0]->tag != F_SYM) return 0;
    if (!is_scheme_file(f)) return 0;
    const char *h = f->as.list.items[0]->as.sym->name;
    if (strcmp(h, "import") == 0) {
        for (uint32_t i = 1; i < f->as.list.len; i++) lib_files_of_set(f->as.list.items[i], out, cap, &n);
    } else if (strcmp(h, "define-library") == 0) {
        for (uint32_t i = 2; i < f->as.list.len; i++) {
            const Form *d = f->as.list.items[i];
            if (d->tag == F_LIST && d->as.list.len > 0 && d->as.list.items[0]->tag == F_SYM &&
                strcmp(d->as.list.items[0]->as.sym->name, "import") == 0)
                for (uint32_t j = 1; j < d->as.list.len; j++) lib_files_of_set(d->as.list.items[j], out, cap, &n);
        }
    }
    return n;
}
