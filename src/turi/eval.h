#ifndef TURI_EVAL_H
#define TURI_EVAL_H

#include "env.h"
#include "value.h"

/* ---------------------------------------------------------------------------
 * Public eval API (Phase S0)
 * --------------------------------------------------------------------------- */

/* Evaluate a Turmeric source string in the given environment.
 * Prior definitions in `env` remain visible.  New top-level definitions
 * (defn, def) are stored back into `env` for subsequent calls.
 *
 * On parse/elaboration error the diagnostic is already emitted to stderr;
 * the returned value has tag TURI_ERROR with a short description.
 *
 * The returned value is valid until `turi_env_free(env)`.  Closures hold
 * internal pointers into env-owned arenas and must not outlive env. */
TuriValue turi_eval(TuriEnv *env, const char *src);

/* Evaluate the contents of a file. */
TuriValue turi_eval_file(TuriEnv *env, const char *path);

/* Initialise the diagnostics subsystem for standalone libturi use.
 * Call once before the first turi_eval.  `use_color` enables ANSI colour
 * in error messages; pass false when output is not a terminal. */
void turi_init(bool use_color);

/* Write a human-readable REPL representation of v into buf (at most cap
 * bytes, NUL-terminated). */
void turi_value_repr(char *buf, size_t cap, TuriValue v);

#endif /* TURI_EVAL_H */
