/* mangle_test.c -- unit tests for the injective name mangler in
 * src/compiler/mangle.c (reversible-name-mangling-plan, task T2).
 *
 * Asserts:
 *   (a) the T1 oracle table mangles to the expected C spelling,
 *   (b) every entry round-trips mangle -> demangle back to the source,
 *   (c) injectivity: no two distinct source names share a mangled output.
 */
#include "mangle.h"
#include "cli/demangle.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            failures++;                                                         \
            fprintf(stderr, "FAIL: ");                                          \
            fprintf(stderr, __VA_ARGS__);                                       \
            fprintf(stderr, "\n");                                              \
        }                                                                       \
    } while (0)

/* The T1 oracle: source name -> expected mangled spelling. */
static const struct {
    const char *src;
    const char *mangled;
} oracle[] = {
    /* pure alnum passes through */
    {"x",            "x"},
    {"foo123",       "foo123"},
    /* the headline collision: kebab vs snake are now distinct */
    {"foo-bar",      "foo_hybar"},
    {"foo_bar",      "foo_unbar"},
    {"a-b",          "a_hyb"},
    {"a_b",          "a_unb"},
    {"a/b",          "a_slb"},
    /* sigil mnemonics (unchanged from A3) */
    {"eq?",          "eq_qu"},
    {"empty!",       "empty_ex"},
    {">>>",          "_gt_gt_gt"},
    {"<<<",          "_lt_lt_lt"},
    {"+",            "_pl"},
    {"*",            "_st"},
    {"=",            "_eq"},
    /* arrow vs module-path no longer collide */
    {"list->vec",    "list_hy_gtvec"},
    /* earmuff-ish and mixed separators */
    {"under_score-dash", "under_unscore_hydash"},
    {"a.b:c",        "a_dob_clc"},
    {"plus+minus",   "plus_plminus"},
    {"map&filter",   "map_amfilter"},
    {"a|b",          "a_bab"},
    {"x~y",          "x_tdy"},
    {"$dollar",      "_dldollar"},
    {"at@sign",      "at_atsign"},
    {"hash#tag",     "hash_hstag"},
    {"semi;colon",   "semi_sccolon"},
    {"comma,sep",    "comma_cmsep"},
    {"q'prime",      "q_qtprime"},
    {"pct%mod",      "pct_pcmod"},
    {"car^et",       "car_cret"},
};

#define ORACLE_N ((int)(sizeof(oracle) / sizeof(oracle[0])))

int main(void) {
    char m[256], d[256];

    /* (a) mangle matches the oracle, and (b) round-trips back. */
    for (int i = 0; i < ORACLE_N; i++) {
        tur_mangle_ident(oracle[i].src, m, sizeof m);
        CHECK(strcmp(m, oracle[i].mangled) == 0,
              "mangle(%s) = %s, expected %s", oracle[i].src, m,
              oracle[i].mangled);

        size_t n = tur_demangle(m, d, sizeof d);
        CHECK(n == strlen(oracle[i].src) && strcmp(d, oracle[i].src) == 0,
              "demangle(%s) = %s, expected %s", m, d, oracle[i].src);
    }

    /* (c) injectivity: no two distinct sources share a mangled output. */
    for (int i = 0; i < ORACLE_N; i++) {
        char mi[256];
        tur_mangle_ident(oracle[i].src, mi, sizeof mi);
        for (int j = i + 1; j < ORACLE_N; j++) {
            char mj[256];
            tur_mangle_ident(oracle[j].src, mj, sizeof mj);
            CHECK(strcmp(mi, mj) != 0,
                  "collision: %s and %s both mangle to %s",
                  oracle[i].src, oracle[j].src, mi);
        }
    }

    /* The "__" structural separator decodes to '/'. */
    CHECK(tur_demangle("geom__vector__add2", d, sizeof d) > 0 &&
              strcmp(d, "geom/vector/add2") == 0,
          "demangle structural separator: got %s", d);

    /* A malformed escape (lone trailing '_') is rejected. */
    CHECK(tur_demangle("foo_", d, sizeof d) == 0,
          "malformed trailing '_' should return 0");
    /* An unknown mnemonic pair is rejected. */
    CHECK(tur_demangle("foo_zz", d, sizeof d) == 0,
          "unknown mnemonic should return 0");

    /* (b') Exhaustive round-trip over every source byte.
     *
     * The oracle above is hand-written, so it only exercises the mnemonics
     * someone thought to list -- a forward entry in sigil_mnemonic() with no
     * inverse in mnemonic_byte() (or an inverse mapping to the wrong byte)
     * would pass every check above it.  That asymmetry is exactly what breaks
     * if the two tables are ever split across files, which the profiling work
     * wants to do (docs/upcoming/profiling-plan.md P0/P1 moves the decoder
     * into the runtime so an emitted binary can symbolize its own profile).
     *
     * Byte-level coverage catches it without needing access to either static
     * table: mangle each single-byte name and demangle it back.  NUL is
     * excluded because it cannot appear in a C-string name. */
    for (int b = 1; b < 256; b++) {
        char src[2] = { (char)b, '\0' };
        tur_mangle_ident(src, m, sizeof m);
        size_t n = tur_demangle(m, d, sizeof d);
        CHECK(n == 1 && (unsigned char)d[0] == (unsigned char)b,
              "byte 0x%02X mangles to %s and demangles to %s (len %zu), "
              "expected the original byte back", b, m, d, n);
    }

    /* (d) tur_name_is_c_keyword. Both keyword lists are bsearch'd, so a
     * mis-sorted entry silently stops matching -- probe a spread of them
     * across the table rather than trusting the source order by eye. */
    static const char *const kw_hits[] = {
        "_Bool", "_Static_assert", "_Thread_local", "alignas", "asm", "auto",
        "bool", "case", "char", "const", "constexpr", "default", "do",
        "double", "enum", "extern", "false", "float", "for", "goto", "if",
        "inline", "int", "long", "nullptr", "register", "restrict", "return",
        "short", "signed", "sizeof", "static", "static_assert", "struct",
        "switch", "thread_local", "true", "typedef", "typeof", "union",
        "unsigned", "void", "volatile", "while",
    };
    for (size_t i = 0; i < sizeof kw_hits / sizeof kw_hits[0]; i++)
        CHECK(tur_name_is_c_keyword(kw_hits[i], strlen(kw_hits[i])),
              "'%s' should be recognized as a C keyword", kw_hits[i]);

    /* Near-misses: ordinary Turmeric names that merely resemble a keyword. */
    static const char *const kw_misses[] = {
        "doubles", "doubl", "Double", "intern", "in", "returns", "structure",
        "voidp", "char-at", "my-int", "tur_u_double", "",
    };
    for (size_t i = 0; i < sizeof kw_misses / sizeof kw_misses[0]; i++)
        CHECK(!tur_name_is_c_keyword(kw_misses[i], strlen(kw_misses[i])),
              "'%s' should NOT be a C keyword", kw_misses[i]);

    /* A prefix slice is not a whole identifier, so it never matches (the same
     * length guard tur_name_collides_libc uses). */
    CHECK(!tur_name_is_c_keyword("intx", 3), "slice 'int' of \"intx\" must not match");

    /* user-defn-named-div-collides-with-libc: every one of these was a name a
     * user could reasonably pick for a helper, and every one emitted a bare
     * `static ... <name>(...)` that cc rejected against the system headers --
     * as errors in generated C under /tmp, with nothing pointing back at the
     * user's own source. The list they were missing from is derived from the
     * headers now rather than grown one report at a time; these twelve are the
     * ones measured breaking, kept here as the concrete oracle.
     *
     * `gets` is the interesting one: glibc declares it only via <bits/stdio2.h>
     * under _FORTIFY_SOURCE, which -O2 turns on -- so it broke `tur run` while
     * `tur emit-c | cc` (no -O2) compiled clean. */
    static const char *const libc_hits[] = {
        "div", "ldiv", "lldiv", "llabs", "atexit", "putchar",
        "getchar", "gets", "chown", "execl", "drand48", "erand48",
        /* already covered before, kept so a regeneration cannot drop them */
        "read", "write", "time", "free", "malloc", "printf", "strlen",
    };
    for (size_t i = 0; i < sizeof libc_hits / sizeof libc_hits[0]; i++)
        CHECK(tur_name_collides_libc(libc_hits[i], strlen(libc_hits[i])),
              "'%s' should collide with libc", libc_hits[i]);

    /* Ordinary turmeric names must NOT be guarded -- over-matching is harmless
     * for correctness but renames symbols for no reason. */
    static const char *const libc_misses[] = {
        "fdiv", "my-div", "divide", "reader", "writer", "timer",
        "user-main", "list-map", "",
    };
    for (size_t i = 0; i < sizeof libc_misses / sizeof libc_misses[0]; i++)
        CHECK(!tur_name_collides_libc(libc_misses[i], strlen(libc_misses[i])),
              "'%s' should NOT collide with libc", libc_misses[i]);

    /* Same whole-identifier guard as the keyword table: a slice that merely
     * starts with a libc name is not that name. */
    CHECK(!tur_name_collides_libc("divx", 3), "slice 'div' of \"divx\" must not match");

    /* The guard prefix survives mangling as data: a user name literally
     * spelled `tur_u_double` cannot alias the guarded form of `double`,
     * because its literal '_' encodes as "_un". */
    tur_mangle_ident("tur_u_double", m, sizeof m);
    CHECK(strcmp(m, "tur_unu_undouble") == 0,
          "mangle(tur_u_double) = %s, expected tur_unu_undouble", m);

    /* (e) `tur demangle`'s token recognizer (src/cli/demangle.c).
     *
     * The decode itself is covered above; what is interesting here is which
     * tokens in an arbitrary text stream get decoded at all.  Recognition is
     * undecidable in general -- the mangling is injective, so `foo_lt_bar` is
     * a valid encoding of `foo<|r` and also an ordinary C symbol -- so these
     * cases pin the heuristic's two edges: real Turmeric names must be
     * rewritten, and the runtime's own C symbols must not be. */
    {
        char o[256];
        /* Rewritten: module-qualified, sigils, kebab. */
        static const struct { const char *tok, *want; } yes[] = {
            {"geom__vector__add2", "geom/vector/add2"},  /* "__" separator */
            {"list_hy_gtvec",      "list->vec"},         /* _hy, _gt        */
            {"foo_hybar",          "foo-bar"},           /* kebab-case      */
            {"add_hytwo",          "add-two"},           /* a bare global   */
            {"hamt_slget",         "hamt/get"},          /* _sl             */
            {"done_qu",            "done?"},             /* trailing '?'    */
            {"empty_ex",           "empty!"},            /* trailing '!'    */
            {"_gt_gt_gt",          ">>>"},               /* all-escape      */
            {"_pl",                "+"},                 /* all-escape      */
            /* The libc/keyword guard prefix is structural: strip, then decode.
             * The prefix is itself the evidence, so this skips the heuristic. */
            {"tur_u_double",       "double"},
            /* The regression a `tur_*` prefix skip-list would have caused:
             * stdlib really does define names in this shape. */
            {"tur_hycontract_hycheck", "tur-contract-check"},
        };
        for (size_t i = 0; i < sizeof yes / sizeof yes[0]; i++)
            CHECK(tur_demangle_token(yes[i].tok, strlen(yes[i].tok), o,
                                     sizeof o, /*strict=*/0) &&
                      strcmp(o, yes[i].want) == 0,
                  "token '%s' should decode to '%s', got '%s'",
                  yes[i].tok, yes[i].want, o);

        /* Passed through: runtime/libc symbols, and anything the decoder
         * rejects.  These are the false positives that would otherwise turn a
         * profile into nonsense. */
        static const char *const no[] = {
            /* Rejected by the decoder itself (no valid mnemonic). */
            "tur_hamt_get", "gc_alloc", "dk_run",
            /* Rejected by the signal heuristic -- these DO decode, and every
             * one was a real false positive measured over `nm ./build/tur`
             * before token_looks_mangled() existed:
             *   analyze_expr -> "analyze!pr", type_eq -> "type=",
             *   pthread_create -> "pthread^eate", tur_string_cmp ->
             *   "tur*ring,p", _exit -> "!it". */
            "analyze_expr", "type_eq", "pthread_create", "tur_string_cmp",
            "tur_string_len", "tur_stm_commit", "elab_do", "n_cmp", "_exit",
            /* Rejected by the leading-"__" rule (would decode to a leading
             * '/'); these are emitted verbatim by the compiler anyway. */
            "__tur_cps_lookup", "__fn_5", "__stdinp", "__func__",
            /* No underscore at all: decodes to itself. */
            "main", "x", "foo123", "atexit",
            /* A decoded control byte can only come from an "_xHH" escape; no
             * real name has one, and emitting raw control bytes into a
             * terminal is not a good way to discover that. */
            "_x07",
            /* A literal '_' in a source name encodes as "_un", which the
             * heuristic scores as noise (measured 6 recall / 85 cost), so a
             * name like `foo_bar` is not recovered. The property that matters
             * is that it is never MIS-decoded: `tur_u_double` as a user name
             * must not be mistaken for the `tur_u_` guard applied to
             * `double`. */
            "foo_unbar", "tur_unu_undouble",
        };
        for (size_t i = 0; i < sizeof no / sizeof no[0]; i++)
            CHECK(!tur_demangle_token(no[i], strlen(no[i]), o, sizeof o,
                                      /*strict=*/0),
                  "token '%s' should have been passed through, got '%s'",
                  no[i], o);

        /* --strict rewrites only module-qualified names ("__" can never arise
         * from data), trading recall for effectively zero false positives. */
        CHECK(tur_demangle_token("geom__vector__add2", 18, o, sizeof o,
                                 /*strict=*/1) &&
                  strcmp(o, "geom/vector/add2") == 0,
              "strict mode should still decode a module-qualified name");
        CHECK(!tur_demangle_token("foo_hybar", 9, o, sizeof o, /*strict=*/1),
              "strict mode should skip a bare global");
    }

    if (failures == 0) {
        printf("mangle_test: all checks passed (%d oracle entries)\n",
               ORACLE_N);
        return 0;
    }
    fprintf(stderr, "mangle_test: %d failure(s)\n", failures);
    return 1;
}
