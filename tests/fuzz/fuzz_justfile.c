/* fuzz_justfile -- the Justfile parser behind `tur run` (T1).
 *
 * parse_justfile is static, so this harness includes justrun.c.  Parsing
 * EVALUATES backtick assignments today (security-audit-plan D-2, owned by
 * WP2: `tur run --list` in an untrusted tree runs shell), so before the
 * include every route to a process or to the environment is renamed to an
 * inert stand-in.  The fuzzer therefore exercises the parser -- including
 * the path a failed backtick takes -- and can never run a command it made
 * up.  When D-2 lands (parse stops evaluating), the stand-ins stay as a
 * guard. */
/* As justrun.c's own first lines: feature macros precede every header. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#if !defined(__APPLE__) && !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Declared before the rename so justrun.c's calls resolve to them; the system
 * headers above are already in, so their own declarations are untouched. */
FILE *fuzz_no_popen(const char *cmd, const char *mode);
int   fuzz_no_pclose(FILE *f);
int   fuzz_no_system(const char *cmd);
int   fuzz_no_setenv(const char *n, const char *v, int o);
int   fuzz_no_chdir(const char *p);

#define popen   fuzz_no_popen
#define pclose  fuzz_no_pclose
#define system  fuzz_no_system
#define setenv  fuzz_no_setenv
#define chdir   fuzz_no_chdir
#include "compiler/justrun.c"
#undef popen
#undef pclose
#undef system
#undef setenv
#undef chdir

#include "fuzz_common.h"

FILE *fuzz_no_popen(const char *cmd, const char *mode) { (void)cmd; (void)mode; return NULL; }
int   fuzz_no_pclose(FILE *f) { (void)f; return -1; }
int   fuzz_no_system(const char *cmd) { (void)cmd; return -1; }
int   fuzz_no_setenv(const char *n, const char *v, int o) { (void)n; (void)v; (void)o; return 0; }
int   fuzz_no_chdir(const char *p) { (void)p; return -1; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *text = fuzz_cstr(data, size);
    /* JFile is a few MiB (fixed recipe tables): heap, not stack. */
    JFile *jf = (JFile *)calloc(1, sizeof *jf);
    if (!jf) abort();
    jf->settings.n_shell  = 2;
    jf->settings.shell[0] = jr_strdup("sh");
    jf->settings.shell[1] = jr_strdup("-c");
    jf->justfile_dir      = jr_strdup("/nonexistent-fuzz-dir");
    /* Imports resolve against the path's directory, which does not exist. */
    (void)parse_justfile(text, "/nonexistent-fuzz-dir/Justfile", jf);
    jfile_free(jf);
    free(jf);
    free(text);
    return 0;
}
