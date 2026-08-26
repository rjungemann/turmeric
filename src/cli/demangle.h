/*
 * demangle.h -- `tur demangle`: rewrite mangled C identifiers in a text stream
 * back to their Turmeric source spellings (profiling-plan P0).
 */
#ifndef TUR_CLI_DEMANGLE_H
#define TUR_CLI_DEMANGLE_H

#include <stddef.h>

/* Decide whether the C-identifier token `tok[0..len)` is a Turmeric-mangled
 * name, and if so write its source spelling into `out` (capacity `cap`,
 * including the NUL).
 *
 * Returns 1 when `out` holds a rewritten name, 0 when the token should be
 * passed through verbatim.  `strict` restricts rewriting to module-qualified
 * names (those carrying the "__" structural separator), which is the mode with
 * effectively no false positives -- see the recognizer discussion in
 * demangle.c.
 *
 * Exposed for the unit test; the filter below is the only other caller. */
int tur_demangle_token(const char *tok, size_t len, char *out, size_t cap,
                       int strict);

/* `tur demangle [--strict] [--annotate] [<name>...]`.
 *
 * With names, demangles each and prints one per line (c++filt's argument
 * mode).  With none, filters stdin to stdout, rewriting every recognized
 * token and passing all other bytes through unchanged.
 *
 * Returns 0 on success, 2 on a CLI error (message already printed). */
int cmd_demangle(int argc, char **argv);

/* Print `tur demangle` usage to stderr; always returns 0. */
int usage_demangle(void);

#endif /* TUR_CLI_DEMANGLE_H */
