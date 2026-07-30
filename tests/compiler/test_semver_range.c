/* Semver + `:tur-version` range unit test.
 *
 * pkg_semver_parse / pkg_semver_compare existed for a long time as DEAD CODE
 * (zero call sites), and carried two bugs that only matter once something reads
 * them -- which `:tur-version` now does.  Both are pinned here:
 *
 *   1. pkg_semver_compare ignored the pre-release component entirely: it parsed
 *      `prea`/`preb` and then free()d them unread, so `0.33.0-rc1` and
 *      `0.33.0` compared EQUAL.  That is precisely the comparison a version
 *      floor has to get right during a release cycle.
 *   2. pkg_semver_parse accepted trailing garbage, so `0.32.2junk` parsed as
 *      `0.32.2`.  Tolerable for a lenient sort; wrong for validating a
 *      user-authored constraint, where a typo must be an error rather than a
 *      silently different range.
 *
 * The range grammar itself is also covered, including the rule people get
 * wrong: `^0.32.2` bounds the next MINOR (`<0.33.0`) because pre-1.0 minors are
 * breaking by convention, while `^1.2.3` bounds the next MAJOR.
 *
 * See docs/reported/no-compiler-version-constraint-in-manifest.md.
 * Built via the tur_semver_range_unit CMake target.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "pkg.h"
#include "lsp_sym.h"

static int failures = 0;

/* Stub required by the tur_core link dependency (mirrors the other
 * compiler-internals unit tests). */
int tur_collect_symbols(const char *source_path, LspSymbol *out, int cap,
                        int *count_out) {
    (void)source_path; (void)out; (void)cap;
    if (count_out) *count_out = 0;
    return 0;
}

static void ck(const char *what, bool got, bool want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %s, want %s\n",
                what, got ? "true" : "false", want ? "true" : "false");
        failures++;
    } else {
        printf("PASS %s\n", what);
    }
}

/* Compare and check the SIGN (the functions are strcmp-like, so only the sign
 * is contractual -- asserting the magnitude would over-specify). */
static void ck_cmp(const char *a, const char *b, int want_sign) {
    int c = pkg_semver_compare(a, b);
    int got = (c < 0) ? -1 : (c > 0) ? 1 : 0;
    if (got != want_sign) {
        fprintf(stderr, "FAIL cmp(%s, %s): got sign %d, want %d\n",
                a, b, got, want_sign);
        failures++;
    } else {
        printf("PASS cmp(%s, %s) = %d\n", a, b, got);
    }
}

static bool parses(const char *v) {
    int ma, mi, pa;
    char *pre = NULL;
    bool ok = pkg_semver_parse(v, &ma, &mi, &pa, &pre);
    free(pre);
    return ok;
}

int main(void) {
    printf("-- pkg_semver_parse --\n");
    ck("0.32.2 parses",                 parses("0.32.2"),     true);
    ck("v-prefix parses",               parses("v0.32.2"),    true);
    ck("major.minor parses (patch opt)", parses("0.32"),      true);
    ck("pre-release parses",            parses("1.2.3-rc1"),  true);
    /* Bug 2 regression: */
    ck("trailing garbage REJECTED",     parses("0.32.2junk"), false);
    ck("non-numeric REJECTED",          parses("abc"),        false);
    ck("dangling '-' REJECTED",         parses("1.2.3-"),     false);
    ck("empty REJECTED",                parses(""),           false);
    ck("leading '-' REJECTED",          parses("-1.2.3"),     false);

    printf("\n-- pkg_semver_compare: ordering --\n");
    ck_cmp("0.32.2", "0.33.0", -1);
    ck_cmp("1.0.0",  "0.99.99", 1);
    ck_cmp("0.33.0", "0.33.0",  0);

    printf("\n-- pkg_semver_compare: pre-release is a tie-breaker (bug 1) --\n");
    ck_cmp("0.33.0-rc1", "0.33.0",     -1);   /* used to be 0 */
    ck_cmp("0.33.0",     "0.33.0-rc1",  1);
    ck_cmp("1.0.0-alpha",   "1.0.0-alpha.1",    -1); /* fewer fields is lower */
    ck_cmp("1.0.0-alpha.1", "1.0.0-alpha.beta", -1); /* numeric < alphanumeric */
    ck_cmp("1.0.0-rc1",     "1.0.0-rc2",        -1);

    printf("\n-- pkg_version_range_valid --\n");
    ck("floor",            pkg_version_range_valid(">=0.32.2"),          true);
    ck("floor + ceiling",  pkg_version_range_valid(">=0.32.2, <0.35.0"), true);
    ck("caret",            pkg_version_range_valid("^0.32"),             true);
    ck("bare == exact",    pkg_version_range_valid("1.2.3"),             true);
    ck("empty INVALID",         pkg_version_range_valid(""),          false);
    ck("comparator only INVALID", pkg_version_range_valid(">="),      false);
    ck("bad version INVALID",   pkg_version_range_valid(">=junk"),    false);
    ck("trailing comma INVALID", pkg_version_range_valid(">=1.0.0,"), false);
    /* Unsupported-by-design operators must be errors, not silently ignored. */
    ck("tilde unsupported",     pkg_version_range_valid("~1.0"),      false);
    ck("wildcard unsupported",  pkg_version_range_valid("1.*"),       false);

    printf("\n-- pkg_version_range_match --\n");
    bool below = false;
    ck("floor met",     pkg_version_range_match(">=0.32.2", "0.32.2", &below), true);
    ck("floor missed",  pkg_version_range_match(">=0.32.2", "0.32.1", &below), false);
    ck("  -> below_floor set",  below, true);
    ck("inside range",  pkg_version_range_match(">=0.32.2, <0.35.0", "0.33.0", &below), true);
    ck("above ceiling", pkg_version_range_match(">=0.32.2, <0.35.0", "0.36.0", &below), false);
    /* The distinction the caller turns into error-vs-warning: */
    ck("  -> below_floor NOT set", below, false);

    printf("\n-- caret: 0.x bounds the next MINOR --\n");
    ck("0.32.5 in ^0.32.2",     pkg_version_range_match("^0.32.2", "0.32.5", &below), true);
    ck("0.33.0 out of ^0.32.2", pkg_version_range_match("^0.32.2", "0.33.0", &below), false);
    ck("0.32.1 below ^0.32.2",  pkg_version_range_match("^0.32.2", "0.32.1", &below), false);
    ck("  -> below_floor set",  below, true);

    printf("\n-- caret: 1.x bounds the next MAJOR --\n");
    ck("1.9.9 in ^1.2.3",     pkg_version_range_match("^1.2.3", "1.9.9", &below), true);
    ck("2.0.0 out of ^1.2.3", pkg_version_range_match("^1.2.3", "2.0.0", &below), false);

    if (failures) {
        fprintf(stderr, "\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall semver/range checks passed\n");
    return 0;
}
