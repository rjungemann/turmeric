/* tur_source_literal_escape -- embedding a path in generated Turmeric source.
 *
 * Several entry points build `(load "<path>")` by string concatenation and
 * evaluate it.  The path lands inside a string literal, so the lexer reads it
 * with escape processing on, and an unescaped Windows path is a parse error at
 * every separator:
 *
 *     <eval>:1:12: error: unknown string escape '\U'
 *     1 | (load "C:\Users\roger\AppData\Local\Temp\turdist/stdlib/macros.tur")
 *
 * That failure took down the whole stdlib prelude, so `when` was an unknown
 * name and an installed tur.exe had an unusable REPL.  The in-tree build hid it:
 * there the stdlib root resolves to the literal "stdlib", which has nothing to
 * escape.
 *
 * These cases are all reachable on POSIX too -- '\' and '"' are both legal in a
 * POSIX filename -- so this is not a Windows-only guard, and the Linux CI legs
 * enforce it.
 */
#include "source_literal.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect (const char *what, const char *in, const char *want) {
    char got[256];
    if (!tur_source_literal_escape (in, got, sizeof got)) {
        fprintf (stderr, "FAIL %s: reported truncation on a short input\n", what);
        failures++;
        return;
    }
    if (strcmp (got, want) != 0) {
        fprintf (stderr, "FAIL %s:\n  in   %s\n  got  %s\n  want %s\n",
                 what, in, got, want);
        failures++;
    }
}

int main (void) {
    /* A POSIX path has nothing to escape: byte-identical, so nothing that works
     * today changes shape. */
    expect ("posix path unchanged", "/usr/local/share/turmeric/stdlib",
            "/usr/local/share/turmeric/stdlib");
    expect ("relative fallback unchanged", "stdlib", "stdlib");
    expect ("empty", "", "");

    /* The bug: every separator of a Windows path is an escape to the lexer. */
    expect ("windows path", "C:\\Users\\roger\\stdlib",
            "C:\\\\Users\\\\roger\\\\stdlib");
    expect ("mixed separators", "C:\\Temp\\d/stdlib/macros.tur",
            "C:\\\\Temp\\\\d/stdlib/macros.tur");
    expect ("trailing separator", "C:\\dir\\", "C:\\\\dir\\\\");

    /* A quote would terminate the literal and inject source -- legal in a POSIX
     * directory name, which is why escaping beats rewriting separators to '/'. */
    expect ("embedded quote", "/tmp/we\"ird", "/tmp/we\\\"ird");
    expect ("quote and backslash", "a\"b\\c", "a\\\"b\\\\c");

    /* Truncation is reported, not silently accepted: a truncated path would
     * produce a well-formed form naming the wrong file. */
    {
        char small[8];
        if (tur_source_literal_escape ("C:\\aaaaaaaaaaaa", small, sizeof small)) {
            fprintf (stderr, "FAIL truncation: returned success on overflow\n");
            failures++;
        } else if (strlen (small) >= sizeof small) {
            fprintf (stderr, "FAIL truncation: result not NUL-terminated in bounds\n");
            failures++;
        }
        /* An escape must never be split across the boundary -- a trailing lone
         * '\' would escape the literal's own closing quote. */
        size_t n = strlen (small);
        size_t back = 0;
        while (back < n && small[n - 1 - back] == '\\') back++;
        if (back % 2 != 0) {
            fprintf (stderr, "FAIL truncation: left a dangling escape (%s)\n", small);
            failures++;
        }
    }

    if (failures == 0) fprintf (stderr, "source literal escaping: OK\n");
    return failures != 0;
}
