/*
 * justrun.h -- tur run: Justfile-compatible task runner (RN0-RN7).
 *
 * Public API: a single entry point dispatched from main.c when `tur run`
 * is invoked without a .tur file argument.
 */

#ifndef TUR_JUSTRUN_H
#define TUR_JUSTRUN_H

/*
 * cmd_justrun -- execute Justfile recipes from the command line.
 *
 * argc/argv are the full program argv (argv[0] = "tur", argv[1] = "run").
 *
 * Exit codes:
 *   0   recipe succeeded
 *   1   recipe ran but exited non-zero (propagated)
 *   2   CLI / parse / unsupported-feature error
 *   127 Justfile not found or unreadable
 */
int cmd_justrun(int argc, char **argv);

/* usage_justrun -- print tur run help to stderr and return 0. */
int usage_justrun(void);

/*
 * justrun_write_template -- write the standard spice Justfile template.
 *
 * dir        directory to write Justfile into (must exist)
 * spice_name spice name to substitute for {{ spice_name }}; if NULL,
 *            derived from the directory basename
 * force      if 0, refuse to overwrite an existing Justfile
 *
 * Returns 0 on success, 1 on error.
 */
int justrun_write_template(const char *dir, const char *spice_name, int force);

#endif /* TUR_JUSTRUN_H */
