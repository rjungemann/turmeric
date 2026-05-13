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

#endif /* TURI_REPL_H */
