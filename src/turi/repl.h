#ifndef TURI_REPL_H
#define TURI_REPL_H

#include <stdbool.h>

/* Phase S1: REPL — interactive read-eval-print loop for `tur repl`.
 *
 * Features:
 *  - libedit/readline line editing and persistent history (~/.tur_history)
 *  - Multi-line continuation: `..` prompt while parens are unbalanced
 *  - Meta-commands: :help  :quit  :type <expr>  :doc <sym>  :reload <file>
 *  - Colour diagnostics when stdout/stderr are a terminal
 */

/* Launch the interactive REPL.  Returns 0 on clean exit, 1 on fatal error.
 *
 * RP6: when `watch_mode` is true the REPL polls the loaded spice's
 * source mtimes between prompts and auto-(reload)s when anything
 * changed. Synchronous check -- no background thread. Pass false for
 * the default behaviour (manual `(reload)` only). */
int turi_repl_run(bool watch_mode);

/* Look up a short doc string for a builtin operator or special form.
 * Returns the doc string, or NULL if sym is not a known builtin. */
const char *turi_doc_lookup_builtin(const char *sym);

/* Get the last diagnostic code captured by the REPL */
const char *turi_repl_get_last_diag_code(void);
/* Set the last diagnostic code captured by the REPL */
void turi_repl_set_last_diag_code(const char *code);

#endif /* TURI_REPL_H */
