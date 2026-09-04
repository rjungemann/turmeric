/*
 * demangle.c -- `tur demangle`: rewrite mangled C identifiers in a text stream
 * back to their Turmeric source spellings (docs/upcoming/profiling-plan.md, P0).
 *
 * Motivation: `perf`, `samply`, Instruments, `nm`, and gdb all work on a
 * Turmeric binary today, but every Turmeric function shows up under its
 * mangled C spelling (`geom__vector__add2`, `list_hy_gtvec`), which makes the
 * output close to unreadable.  This filter is the c++filt of that pipeline:
 *
 *     perf record -g ./build/bin/myprog
 *     perf script | tur demangle | perf report -i -
 *
 * The whole decoder already exists -- tur_demangle() in compiler/mangle.c is
 * the exact inverse of the injective scheme the emitter applies.  What is NOT
 * decidable is *which* tokens in an arbitrary text stream are Turmeric names,
 * and that is what this file is really about.
 *
 * ---------------------------------------------------------------------------
 * The recognizer, and why it is a heuristic
 * ---------------------------------------------------------------------------
 * The mangling is injective, so round-tripping cannot be used as a test: a C
 * symbol like `foo_lt_bar` is a perfectly valid mangling of the Turmeric name
 * `foo<|r`, and re-mangling `foo<|r` reproduces `foo_lt_bar` byte for byte.
 * There is no signal there to exploit -- injectivity is what makes the scheme
 * correct and simultaneously what makes recognition undecidable.
 *
 * It is tempting to stop there and lean on tur_demangle()'s own strictness (a
 * lone '_' must be followed by "x"+two hex digits or one of 23 two-letter
 * mnemonics).  That is not enough, and the failure is not subtle.  Measured
 * over the 14418 C symbols in `nm ./build/tur` -- a binary with no Turmeric
 * code in it at all, so every rewrite is by definition wrong -- decoder
 * strictness alone accepts 149 of them:
 *
 *     analyze_expr   -> "analyze!pr"     (_ex = '!')
 *     tur_string_cmp -> "tur*ring,p"     (_st = '*', _cm = ',')
 *     pthread_create -> "pthread^eate"   (_cr = '^')
 *     type_eq        -> "type="          (_eq = '=')
 *
 * Those are exactly the hot symbols a profile shows, so ~1% corruption on the
 * symbol table is much worse than 1% noise in the output.
 *
 * The fix is to require corroborating evidence that the mangler, and not a C
 * author's word separator, produced the underscore.  Each mnemonic was scored
 * on real data -- recall = stdlib `defn` names containing that sigil (1486
 * names), cost = C symbols in `nm ./build/tur` containing that escape:
 *
 *     '-' _hy   recall 1178   cost   1      '!' _ex   recall  74   cost 112
 *     '/' _sl   recall  351   cost  47      '_' _un   recall   6   cost  85
 *     '?' _qu   recall  137   cost  21      '=' _eq   recall   1   cost  83
 *     '>' _gt   recall   26   cost   0      '*' _st   recall   0   cost 253
 *
 * So `_hy` (kebab-case, the dominant Turmeric convention) is nearly free,
 * while `_st`/`_eq`/`_un` are pure noise.  The two interesting rows are '?'
 * and '!': both are Turmeric's *trailing* predicate/bang convention, and
 * anchoring them to the end of the token collapses their cost to nothing --
 * `_qu` at end scores 137 recall / 0 cost (every `?` name ends in `?`), `_ex`
 * at end scores 74 / 4, versus 74 / 112 unanchored.
 *
 * Hence token_looks_mangled() below.  Measured after it:
 *
 *   precision  8 rewrites over the 14418 C symbols of `nm ./build/tur`
 *              (0.055%, down from 149) -- all 8 the same `_sl` + "ice"/"ot"
 *              shape (`n_slice`, `find_slot`).  Rejecting those too would be
 *              over-fitting to one binary, so they stay.
 *   recall     1374 of 1374 non-trivially-mangled stdlib `defn` names
 *              recovered exactly, with zero mis-decodes.
 *
 * tests/run-demangle.sh re-runs the precision measurement as a ratchet, so
 * adding a noisy mnemonic to the signal set fails a test rather than quietly
 * degrading every profile.
 *
 * Deliberately NOT a prefix skip-list.  An earlier draft skipped every `tur_*`
 * symbol as "runtime, not Turmeric", which is wrong: the stdlib really does
 * define names like `tur-contract-check`, mangling to `tur_hycontract_hycheck`
 * -- a genuine name a `tur_` skip would have silently suppressed.
 *
 * `--strict` drops the heuristic entirely and rewrites only module-qualified
 * names, whose "__" structural separator data can never produce.  That is the
 * zero-false-positive mode; it cannot be the default because a single-file
 * program's top-level defns are emitted BARE (`add-two` -> `add_hytwo`, no
 * module prefix), and single-file programs are the common profiling target.
 *
 * The real fix is not a better heuristic: it is for the emitter to write a
 * mangled->source side-table next to the binary, turning this guess into a
 * lookup.  That is P1 work (see the plan); this filter is what makes `perf`
 * usable today.
 *
 * Three guards on top of the decoder:
 *   - The `tur_u_` libc/keyword guard prefix (TUR_NAME_GUARD_PREFIX) is
 *     stripped before decoding, so `tur_u_double` reads back as `double`
 *     rather than failing outright.  The prefix is itself the evidence, so
 *     such a token skips the signal test.
 *   - A token starting with "__" is rejected: the decoder would turn the
 *     leading separator into a leading '/', and no Turmeric name begins with
 *     one.  Those tokens are compiler-synthesized (`__fn_5`) or C-internal
 *     (`__stdinp`, `__func__`), and are emitted verbatim by the emitter
 *     anyway.  This alone removes 30 of the 149.
 *   - A decoded name containing a control byte is rejected.  Those can only
 *     come from an "_xHH" escape, no real Turmeric name has one, and emitting
 *     raw control characters into someone's terminal is a bad way to find out.
 */
#include "cli/demangle.h"

#include <stdio.h>
#include <string.h>

#include "mangle.h"

/* Longest identifier run this filter will consider.  A longer run is passed
 * through verbatim -- no C toolchain produces identifiers near this length,
 * and the cap keeps the tokenizer allocation-free. */
#define TOK_MAX 4096

static int is_ident_byte(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int has_prefix(const char *s, size_t n, const char *p, size_t pl) {
    return n >= pl && memcmp(s, p, pl) == 0;
}

static int contains_double_underscore(const char *s, size_t n) {
    for (size_t i = 0; i + 1 < n; i++)
        if (s[i] == '_' && s[i + 1] == '_') return 1;
    return 0;
}

static int has_control_byte(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x20 || *p == 0x7f) return 1;
    return 0;
}

/* Mnemonics whose presence is evidence of mangling rather than of a C word
 * boundary -- the measured low-cost rows from the table in the file header.
 * '-' and '/' carry almost all the recall (kebab-case and `mod/name`); the
 * rest are sigils no C identifier can contain, so they cost nothing to keep
 * and they cover operator-flavored names. */
static int is_signal_mnemonic(char a, char b) {
    static const char *const sig[] = {
        "hy",  /* '-' -- kebab-case; 1178 recall / 1 cost */
        "sl",  /* '/' -- `hamt/get`, `json/decode`; 351 / 47 */
        "gt", "lt",              /* '>' '<' -- arrows, comparisons; 0 cost */
        "pc", "am", "td", "qt", "hs",  /* '%' '&' '~' '\'' '#' -- 0 cost */
    };
    for (size_t i = 0; i < sizeof sig / sizeof sig[0]; i++)
        if (a == sig[i][0] && b == sig[i][1]) return 1;
    return 0;
}

/* Does `s[0..n)` carry evidence that the mangler produced it?
 *
 * Scans the escape structure once.  Accepts on any of:
 *   (a) a "__" structural separator -- module-qualified, data cannot make one;
 *   (b) an "_xHH" hex escape, or a signal mnemonic (above);
 *   (c) a trailing "_qu" / "_ex" -- Turmeric's `name?` / `name!` convention,
 *       which is high-signal at the end and pure noise anywhere else;
 *   (d) no literal characters at all outside the escapes -- a pure operator
 *       name like `+` (_pl), `=` (_eq) or `>>>` (_gt_gt_gt), where every
 *       individual mnemonic is noisy but an all-escape token is not. */
static int token_looks_mangled(const char *s, size_t n) {
    int signal = 0;
    int has_literal = 0;
    size_t i = 0;

    while (i < n) {
        if (s[i] != '_')                    { has_literal = 1; i++; continue; }
        if (i + 1 < n && s[i + 1] == '_')   { signal = 1;      i += 2; continue; }
        if (i + 1 < n && s[i + 1] == 'x')   { signal = 1;      i += 4; continue; }
        if (i + 3 <= n) {
            char a = s[i + 1], b = s[i + 2];
            if (is_signal_mnemonic(a, b)) signal = 1;
            else if (i + 3 == n && ((a == 'q' && b == 'u') ||
                                    (a == 'e' && b == 'x'))) signal = 1;
            i += 3;
            continue;
        }
        /* Malformed tail; tur_demangle() rejects the token regardless. */
        i++;
    }
    return signal || !has_literal;
}

int tur_demangle_token(const char *tok, size_t len, char *out, size_t cap,
                       int strict) {
    if (!tok || len == 0 || !out || cap == 0) return 0;
    if (len >= TOK_MAX) return 0;

    /* The libc/C-keyword guard prefix is structural, not data: strip it and
     * decode what follows, so `tur_u_double` reads back as `double`.  A user
     * name literally spelled `tur_u_double` cannot be confused with the
     * guarded form -- its literal '_' encodes as "_un", giving
     * `tur_unu_undouble`, whose sixth byte is 'n' and so never matches here. */
    const char *body = tok;
    size_t body_len = len;
    int guarded = 0;
    if (has_prefix(tok, len, TUR_NAME_GUARD_PREFIX, TUR_NAME_GUARD_PREFIX_LEN)) {
        guarded  = 1;
        body     = tok + TUR_NAME_GUARD_PREFIX_LEN;
        body_len = len - TUR_NAME_GUARD_PREFIX_LEN;
        if (body_len == 0) return 0;
    } else {
        /* A token with no '_' at all decodes to itself; skip the work. */
        if (memchr(tok, '_', len) == NULL) return 0;
        /* A leading "__" would decode to a leading '/', which no Turmeric name
         * has.  These are compiler-synthesized (`__fn_5`) or C-internal
         * (`__stdinp`, `__func__`) and are emitted verbatim regardless. */
        if (len >= 2 && tok[0] == '_' && tok[1] == '_') return 0;
        /* A C reserved word or a libc/POSIX symbol is never a mangled
         * Turmeric name -- the emitter guards those with `tur_u_` precisely so
         * they cannot collide (see raw_name_for_binding). */
        if (tur_name_is_c_keyword(tok, len)) return 0;
        if (tur_name_collides_libc(tok, len)) return 0;
        /* The measured heuristic (see the file header): require evidence that
         * the mangler, not a C word separator, produced these underscores. */
        if (!strict && !token_looks_mangled(tok, len)) return 0;
    }

    /* --strict: only module-qualified names, i.e. those carrying the "__"
     * structural separator.  Data can never produce "__" (a literal '_'
     * encodes as "_un"), so this mode has no false positives at all -- at the
     * cost of missing every bare top-level global, which is why it is not the
     * default. */
    if (strict && !contains_double_underscore(body, body_len)) return 0;

    char nul_body[TOK_MAX];
    memcpy(nul_body, body, body_len);
    nul_body[body_len] = '\0';

    char decoded[TOK_MAX];
    if (tur_demangle(nul_body, decoded, sizeof decoded) == 0) return 0;
    if (has_control_byte(decoded)) return 0;

    /* Compare against the WHOLE token, not the guard-stripped body: for a
     * guarded token the decode is an identity on the body yet still a real
     * rewrite of the symbol (`tur_u_double` -> `double`). */
    size_t dec_len = strlen(decoded);
    if (!guarded && dec_len == len && memcmp(decoded, tok, len) == 0) return 0;
    if (dec_len + 1 > cap) return 0;

    memcpy(out, decoded, dec_len + 1);
    return 1;
}

/* Emit one identifier token, rewritten if it is recognized.  `annotate` keeps
 * the original spelling alongside the decode, which is what you want when the
 * output feeds another tool that still needs to match on the C symbol. */
static void emit_token(FILE *out, const char *tok, size_t len, int strict,
                       int annotate) {
    char decoded[TOK_MAX];
    if (len > 0 && tur_demangle_token(tok, len, decoded, sizeof decoded, strict)) {
        fputs(decoded, out);
        if (annotate) {
            fputc('[', out);
            fwrite(tok, 1, len, out);
            fputc(']', out);
        }
        return;
    }
    if (len > 0) fwrite(tok, 1, len, out);
}

/* Stream `in` to `out`, rewriting every recognized identifier token and
 * passing all other bytes through untouched.  Byte-oriented rather than
 * line-oriented so there is no line-length limit and no per-line allocation;
 * an identifier run longer than TOK_MAX streams through verbatim. */
static int filter_stream(FILE *in, FILE *out, int strict, int annotate) {
    char tok[TOK_MAX];
    size_t n = 0;
    int overflowed = 0;
    int c;

    while ((c = getc(in)) != EOF) {
        if (is_ident_byte(c)) {
            if (overflowed) { putc(c, out); continue; }
            if (n + 1 >= TOK_MAX) {
                /* Give up on this run: flush what we have and stream the
                 * rest verbatim until the run ends. */
                fwrite(tok, 1, n, out);
                putc(c, out);
                n = 0;
                overflowed = 1;
                continue;
            }
            tok[n++] = (char)c;
            continue;
        }
        if (overflowed) overflowed = 0;
        else            emit_token(out, tok, n, strict, annotate);
        n = 0;
        putc(c, out);
    }
    if (!overflowed) emit_token(out, tok, n, strict, annotate);

    if (ferror(in)) {
        fprintf(stderr, "tur demangle: read error\n");
        return 2;
    }
    if (fflush(out) != 0 || ferror(out)) {
        fprintf(stderr, "tur demangle: write error\n");
        return 2;
    }
    return 0;
}

int usage_demangle(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur demangle [flags]                 filter stdin to stdout\n"
        "  tur demangle [flags] <name>...       demangle each name, one per line\n"
        "\n"
        "flags:\n"
        "  --strict      only rewrite module-qualified names (those carrying the\n"
        "                \"__\" separator). Effectively no false positives, but\n"
        "                misses bare top-level globals.\n"
        "  --annotate    print `source[mangled]` instead of replacing the token,\n"
        "                so downstream tools can still match the C symbol.\n"
        "\n"
        "examples:\n"
        "  perf script | tur demangle | perf report -i -\n"
        "  nm -g ./build/bin/myprog | tur demangle\n"
        "  tur demangle geom__vector__add2        # -> geom/vector/add2\n");
    return 0;
}

int cmd_demangle(int argc, char **argv) {
    int strict = 0, annotate = 0;
    int first_name = -1;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--strict") == 0)        { strict = 1; continue; }
        if (strcmp(argv[i], "--annotate") == 0)      { annotate = 1; continue; }
        if (strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "-h") == 0)              { usage_demangle(); return 0; }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "tur demangle: unknown flag '%s'\n", argv[i]);
            usage_demangle();
            return 2;
        }
        first_name = i;
        break;
    }

    if (first_name < 0) return filter_stream(stdin, stdout, strict, annotate);

    /* Argument mode (c++filt's): one decoded name per line.  A name that is
     * not recognized prints unchanged, so the output always has one line per
     * argument and stays usable in a shell loop. */
    for (int i = first_name; i < argc; i++) {
        emit_token(stdout, argv[i], strlen(argv[i]), strict, annotate);
        putc('\n', stdout);
    }
    return 0;
}
