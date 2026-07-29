/* jit_engine.h -- the in-process MIR JIT engine behind `tur jit` (Phase J1).
 *
 * Only built into `tur` under -DTUR_JIT=ON; cmd_jit in main.c compiles
 * against this header unconditionally and reports the missing capability
 * when TUR_HAVE_JIT is not defined.  See jit_engine.c and
 * docs/upcoming/jit-engine-plan.md section 3.2. */
#ifndef TUR_JIT_ENGINE_H
#define TUR_JIT_ENGINE_H

#include <stddef.h>

/* Outcomes of a JIT attempt.  Anything other than TUR_JIT_OK means the
 * program did NOT run and the caller should take the plan's step-6 fallback
 * to the cc path (with a diagnostic; c2mir already printed the specifics
 * for COMPILE failures). */
enum {
    TUR_JIT_OK          = 0,   /* ran; *prog_rc holds the program's exit code */
    TUR_JIT_ERR_COMPILE = 1,   /* c2mir rejected the C (subset gap / inline-C) */
    TUR_JIT_ERR_LINK    = 2,   /* an autolinked library could not be loaded */
    TUR_JIT_ERR_RUN     = 3,   /* engine-level failure setting up execution */
};

/* Compile `csrc` (the emitted C, post hoist_tur_include_directives) in
 * process and run its main with prog_argc/prog_argv.  `autolink` is the
 * resolved __tur_autolink__ flag string (may be NULL); -l entries are
 * dlopen'd RTLD_GLOBAL so dlsym can reach them.  `include_dirs` is where
 * `#include "hamt.h"` et al. resolve -- the caller (cmd_jit) supplies the
 * Turmeric tree / SDK dirs it already knows how to find. */
int tur_jit_execute(const char *csrc, size_t csrc_len, const char *autolink,
                    const char **include_dirs, int n_include_dirs,
                    int prog_argc, char **prog_argv, int *prog_rc);

#endif /* TUR_JIT_ENGINE_H */
