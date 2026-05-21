#ifndef TURI_REPL_H
#define TURI_REPL_H

/* Phase S1: REPL — interactive read-eval-print loop for `tur repl`.
 *
 * Features:
 *  - libedit/readline line editing and persistent history (~/.tur_history)
 *  - Multi-line continuation: `..` prompt while parens are unbalanced
 *  - Meta-commands: :help  :quit  :type <expr>  :doc <sym>  :reload <file>
 *  - Colour diagnostics when stdout/stderr are a terminal
 */

/* Launch the interactive REPL.  Returns 0 on clean exit, 1 on fatal error. */
int turi_repl_run(void);

/* Look up a short doc string for a builtin operator or special form.
 * Returns the doc string, or NULL if sym is not a known builtin. */
const char *turi_doc_lookup_builtin(const char *sym);

#endif /* TURI_REPL_H */
